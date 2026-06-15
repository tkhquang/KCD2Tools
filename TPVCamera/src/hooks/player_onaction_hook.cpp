/**
 * @file player_onaction_hook.cpp
 * @brief Hooks the player OnAction / global action dispatcher and latches device-agnostic movement intent.
 */

#include "hooks/player_onaction_hook.hpp"
#include "aob_resolver.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <array>
#include <atomic>
#include <cmath>
#include <exception>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace TPVCamera
{

    // Global action dispatcher: sub_1808EBEE4(this /*rcx*/, const char** action_name /*rdx*/,
    // uint activation /*r8d*/, float value /*xmm3*/). Fires once per action-map action (the C++ source of
    // Lua Player:OnAction) and returns a pointer we forward unchanged. action_name points to a ref-counted
    // C-string (the action name, e.g. "xi_movey"); value is the post-action-map axis magnitude.
    using ActionDispatchFunc = uintptr_t(__fastcall *)(uintptr_t self, const char **action_name,
                                                       unsigned int activation, float value);

    static ActionDispatchFunc s_action_dispatch_original = nullptr;
    static std::atomic<bool> s_available{false};

    // Movement action names whose value magnitude signals locomotion intent, across input devices and ALL
    // directions. The orbit move-detection keys on the LARGEST of these: it engages for forward, strafe and
    // reverse alike, but only "is the player moving" matters here, not the direction. (While moving, the body is
    // pinned to the camera heading; KCD2 moves the player relative to the body rotation, so that pin is what
    // makes WASD camera-relative -- the direction the player pushes is the engine's to apply, not ours.) KEYBOARD
    // uses the digital actions (value ~1 held, 0 released); GAMEPAD uses the signed analog left-stick axes (value
    // -1..1, magnitude taken). The input is nonzero the instant a key is pressed and stays nonzero while a key is
    // held against a wall, which body-position speed cannot do. The probe below logs each distinct action name
    // once (trace level) so this vocabulary can be confirmed at runtime.
    static constexpr std::array<std::string_view, 8> k_move_actions = {
        "moveforward", "moveback",   "moveleft", "moveright", // keyboard digital (one per direction)
        "xi_movey",    "xi_movex",                            // gamepad left-stick (signed analog)
        "movement_y",  "movement_x",                          // named analog aliases (some device/rebind paths)
    };
    static constexpr size_t k_move_count = k_move_actions.size();

    // One latched |value| per movement action, written on the input thread and read on the render thread.
    // Value-initialized to 0; relaxed atomics suffice (a one-frame-stale signal is harmless).
    static std::atomic<float> s_move_values[k_move_count]{};

    bool player_onaction_available()
    {
        return s_available.load(std::memory_order_relaxed);
    }

    float player_onaction_move_magnitude()
    {
        // Largest movement-intent magnitude across all directions and devices: ~1.0 while moving, 0 when
        // released, and it stays nonzero while a key is held against a wall (intent over speed). The orbit move
        // latch keys on this, so it engages for forward, strafe and reverse alike.
        float magnitude = 0.0f;
        for (size_t i = 0; i < k_move_count; ++i)
        {
            const float value = s_move_values[i].load(std::memory_order_relaxed);
            if (value > magnitude)
            {
                magnitude = value;
            }
        }
        return magnitude;
    }

    float player_onaction_reset()
    {
        // exchange() atomically reads-and-clears each slot, so a concurrent input-thread store of a fresh
        // press is never lost (it simply re-latches after this returns). The largest cleared value is
        // returned so the caller can tell whether the latch was genuinely stranded (> the stop threshold)
        // versus already idle, which is the key signal in the log for the post-combat self-rotation bug.
        float had = 0.0f;
        for (size_t i = 0; i < k_move_count; ++i)
        {
            const float prev = s_move_values[i].exchange(0.0f, std::memory_order_relaxed);
            if (prev > had)
            {
                had = prev;
            }
        }
        return had;
    }

    /**
     * @brief Trace-only vocabulary probe: logs each distinct action name once so the on-foot movement-action
     *        names can be confirmed at runtime. Inert unless the trace log level is on; the dedup list
     *        is touched only here, on the input thread, under its own mutex.
     */
    static void maybe_log_action_name(const char *name)
    {
        DMK::Logger &logger = DMK::Logger::get_instance();
        if (!logger.is_enabled(DMK::LogLevel::Trace))
        {
            return;
        }
        static std::mutex seen_mutex;
        static std::vector<std::string> seen;
        const std::lock_guard<std::mutex> lock(seen_mutex);
        for (const std::string &entry : seen)
        {
            if (entry == name)
            {
                return;
            }
        }
        seen.emplace_back(name);
        logger.trace("PlayerOnAction: action '{}' seen", name);
    }

    /**
     * @brief Latches the movement axes from one action event. Separated from the SEH wrapper so this frame
     *        holds no unwinding objects; the engine-owned string reads are screened before use.
     */
    static void capture_movement_input(const char **action_name, float value)
    {
        if (action_name == nullptr || !DMK::Memory::plausible_userspace_ptr(reinterpret_cast<uintptr_t>(action_name)))
        {
            return;
        }
        const char *name = *action_name;
        if (name == nullptr || !DMK::Memory::plausible_userspace_ptr(reinterpret_cast<uintptr_t>(name)))
        {
            return;
        }

        maybe_log_action_name(name);

        // Latch this action's magnitude (|value|) into its own slot; player_onaction_move_magnitude takes the
        // largest across slots. Each action owns a slot, so an independent release (value 0) does not clobber
        // another still-held source (keyboard + stick). Digital keys report ~1 held / 0 released; an analog axis
        // reports a signed deflection, so its magnitude is taken regardless of direction. The name is screened
        // above; building the view here (one strlen) runs under the detour's SEH frame, so even a malformed
        // engine string fails closed rather than faulting.
        const std::string_view name_view(name);
        for (size_t i = 0; i < k_move_count; ++i)
        {
            if (k_move_actions[i] == name_view)
            {
                s_move_values[i].store(std::fabs(value), std::memory_order_relaxed);
                break;
            }
        }
    }

    /**
     * @brief Action-dispatcher detour: latch movement intent, then always forward to the original so the
     *        game's action handling (and Lua Player:OnAction) is untouched. A fault while reading the event
     *        is swallowed and the original still runs.
     */
    static uintptr_t __fastcall detour_action_dispatch(uintptr_t self, const char **action_name,
                                                       unsigned int activation, float value)
    {
        __try
        {
            capture_movement_input(action_name, value);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return s_action_dispatch_original ? s_action_dispatch_original(self, action_name, activation, value) : 0;
    }

    bool initialize_player_onaction_hook()
    {
        DMK::Logger &logger = DMK::Logger::get_instance();
        try
        {
            const uintptr_t dispatch_addr = anchor_address(AnchorId::ActionDispatch);
            if (dispatch_addr == 0)
            {
                logger.warning(
                    "PlayerOnAction: action dispatcher cascade unresolved; orbit move-detection uses body speed");
                return false;
            }

            DMK::HookManager &hook_manager = DMK::HookManager::get_instance();
            // Fail closed if the resolved entry leads with a call/breakpoint byte (a sibling mod's E9 jump
            // hook does not trip this, so layering still works).
            const DMK::HookConfig hook_config{.prologue_policy = DMK::InlineProloguePolicy::Fail};
            auto result = hook_manager.create_inline_hook(
                "PlayerOnActionDispatch", dispatch_addr, reinterpret_cast<void *>(detour_action_dispatch),
                reinterpret_cast<void **>(&s_action_dispatch_original), hook_config);

            if (!result.has_value())
            {
                logger.warning(
                    "PlayerOnAction: action dispatcher hook failed ({}); orbit move-detection uses body speed",
                    DMK::Hook::error_to_string(result.error()));
                return false;
            }

            s_available.store(true, std::memory_order_relaxed);
            return true;
        }
        catch (const std::exception &e)
        {
            logger.error("PlayerOnAction: initialization failed: {}", e.what());
            return false;
        }
    }

} // namespace TPVCamera
