/**
 * @file hooks/input_hook.cpp
 * @brief The input-dispatcher detour for free-look orbit (see input_hook.hpp).
 */

#include "hooks/input_hook.hpp"
#include "aob_resolver.hpp"
#include "config.hpp"
#include "constants.hpp"
#include "global_state.hpp"
#include "hooks/camera_hook.hpp"
#include "presets/preset_runtime.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace TPVCamera
{
    namespace
    {
        // Generic input-event dispatcher: void __fastcall(controller /*rcx*/, event /*rdx*/,
        // char /*r8b*/). Every input event funnels through here; hooking it lets free-look
        // capture mouse-look deltas and freeze the player by not dispatching while orbiting.
        using InputDispatchFunc = void(__fastcall *)(uintptr_t controller, uintptr_t input_event, char flag);
        std::atomic<InputDispatchFunc> s_input_dispatch_original{nullptr};

        /**
         * @brief Free-look capture/block decision for one input event (toggle orbit camera).
         * @details While the offset is active and orbit is TOGGLED on, mouse-look deltas are accumulated
         *          into the orbit angles and ONLY those look events are blocked, so the player's look stays
         *          put while free-looking. Every other input (movement keys, interaction, clicks) passes
         *          through, so the toggle orbit is a usable camera mode the character can move and act in.
         *          Lives apart from the SEH wrapper so that frame stays free of C++ object unwinding; the
         *          wrapper guards the engine-owned reads.
         * @param input_event Engine input event (GameStructures::InputEvent layout).
         * @return true to block (swallow) the event, false to dispatch it normally.
         */
        [[nodiscard]] bool orbit_capture_and_decide(uintptr_t input_event)
        {
            CameraState &cam = camera_state();
            // Gate on the offset's effective active state (so free-look also works when a forced-TPV state
            // turned the offset on without the manual toggle), the orbit toggle, the cursor-shown flag (so a
            // UI being up holds the orbit instead of letting cursor motion turn it), and the shared UI gate
            // so the cursor and look work normally while a suppressed UI is up. Returning false here leaves
            // orbit_yaw/orbit_pitch untouched, so the camera resumes from the same angle when the UI closes.
            // orbit_active is the single source of truth for whether free-look captures input. The
            // OrbitExcludeState policy turns it off when ENTERING a listed state (edge-triggered, see
            // apply_orbit_exclude_policy), so capture stops there without forbidding a manual re-enable: a
            // player who toggles free-look back on in an excluded state (e.g. for a photo on a mount) still
            // gets mouse-look here. The cursor-shown gate freezes the orbit while any UI cursor is up.
            if (!camera_offset_active() || !cam.orbit_active.load(std::memory_order_relaxed) || game_cursor_shown() ||
                !should_apply_view())
            {
                return false;
            }
            if (input_event == 0 || !DMK::memory::is_plausible_ptr(DMK::Address{input_event}))
            {
                return false;
            }

            const int32_t type = *reinterpret_cast<const int32_t *>(input_event + Constants::INPUT_EVENT_TYPE_OFFSET);
            const int32_t id = *reinterpret_cast<const int32_t *>(input_event + Constants::INPUT_EVENT_ID_OFFSET);
            // The look channel is the eIS_Changed (analog-axis-moved) state. Mouse AND gamepad both post here; the
            // keyId picks the axis and the device. Everything else (movement axes, buttons, clicks) falls through.
            if (type == Constants::MOUSE_INPUT_TYPE_ID)
            {
                const float value = *reinterpret_cast<const float *>(input_event + Constants::INPUT_EVENT_VALUE_OFFSET);

                // Mouse look: relative DELTA accumulated straight into the orbit angle (one event = one nudge).
                if (id == Constants::INPUT_LOOK_YAW_EVENT_ID || id == Constants::INPUT_LOOK_PITCH_EVENT_ID)
                {
                    if (id == Constants::INPUT_LOOK_YAW_EVENT_ID)
                    {
                        // Negated so mouse-left orbits the camera left and mouse-right orbits right; a negative X
                        // sensitivity inverts that.
                        const float sensitivity_x = Presets::preset_readout().orbit_sensitivity_x;
                        cam.orbit_yaw.store(cam.orbit_yaw.load(std::memory_order_relaxed) - value * sensitivity_x,
                                            std::memory_order_relaxed);
                    }
                    else
                    {
                        // Mouse-up raises the camera, mouse-down lowers it; a negative Y sensitivity inverts that.
                        const float sensitivity_y = Presets::preset_readout().orbit_sensitivity_y;
                        // Within the range the main thread published (see CameraState::orbit_pitch_lo); a value
                        // already outside it is kept but cannot grow.
                        const float old_pitch = cam.orbit_pitch.load(std::memory_order_relaxed);
                        cam.orbit_pitch.store(
                            std::clamp(old_pitch + value * sensitivity_y,
                                       std::min(cam.orbit_pitch_lo.load(std::memory_order_relaxed), old_pitch),
                                       std::max(cam.orbit_pitch_hi.load(std::memory_order_relaxed), old_pitch)),
                            std::memory_order_relaxed);
                    }
                    return true; // block ONLY the look so the player look stays put while free-looking
                }

                // Gamepad RIGHT STICK: latch the held DEFLECTION (-1..1) for the orbit rate integration, then ZERO
                // the event value IN PLACE and let it PASS (do NOT block). A held analog stick drives an engine
                // look-RATE that the engine only zeroes on a release event. BLOCKING swallows that release: if orbit
                // engages while the stick is already deflected (e.g. you hold the right stick then toggle orbit, or an
                // exclude state suspends/restores orbit mid-deflection), the engine keeps its last rate and SPINS the
                // player look forever - surviving a switch to first person and curable only by a hard input flush
                // (menu / alt-tab). It is the same swallowed-release defect as the move latch, but stranding the
                // ENGINE's own gamepad look. Zeroing the value instead means the engine continuously sees no
                // deflection (so it never turns and never strands) while we keep the real value for the orbit camera;
                // the release reaches the engine too. The mouse path above can still hard-block: a mouse delta is a
                // one-shot nudge with no held rate, so there is nothing to strand. Left stick (movement) keeps its own
                // ids and passes untouched.
                if (id == Constants::INPUT_PAD_LOOK_YAW_EVENT_ID)
                {
                    cam.orbit_pad_yaw.store(value, std::memory_order_relaxed);
                    *reinterpret_cast<float *>(input_event + Constants::INPUT_EVENT_VALUE_OFFSET) = 0.0f;
                    return false;
                }
                if (id == Constants::INPUT_PAD_LOOK_PITCH_EVENT_ID)
                {
                    cam.orbit_pad_pitch.store(value, std::memory_order_relaxed);
                    *reinterpret_cast<float *>(input_event + Constants::INPUT_EVENT_VALUE_OFFSET) = 0.0f;
                    return false;
                }
            }

            return false; // pass movement, interaction and everything else through
        }

        /**
         * @brief Runs orbit_capture_and_decide under SEH. A fault falls back to normal dispatch.
         * @details Apart from the detour, because the detour's DetourScope needs C++ object unwinding, which a __try
         *          frame cannot hold.
         * @return true to block (swallow) the event, false to dispatch it normally.
         */
        [[nodiscard]] bool orbit_capture_guarded(uintptr_t input_event) noexcept
        {
            __try
            {
                return orbit_capture_and_decide(input_event);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        /**
         * @brief Input-dispatcher detour. Swallows events while free-looking, else passes through.
         */
        void __fastcall detour_input_dispatch(uintptr_t controller, uintptr_t input_event, char flag) noexcept
        {
            const DetourScope in_flight;
            if (orbit_capture_guarded(input_event))
            {
                return;
            }
            if (const InputDispatchFunc original = s_input_dispatch_original.load(std::memory_order_acquire))
            {
                original(controller, input_event, flag);
            }
        }

    } // namespace

    void install_input_dispatch_hook(HookSet &hooks)
    {
        DMK::Logger &logger = DMK::log();
        const uintptr_t input_addr = gated_anchor_address(Feature::Orbit, AnchorId::InputDispatch);
        if (input_addr == 0)
        {
            logger.warning("Camera: Input dispatcher cascade unresolved; free-look orbit unavailable");
        }
        else
        {
            auto input_result = DMK::hook::inline_at(
                DMK::hook::InlineRequest{.name = "CameraInputDispatch", .target = DMK::Address{input_addr}},
                detour_input_dispatch);

            if (!input_result.has_value())
            {
                logger.warning("Camera: Input dispatcher hook failed ({}); free-look orbit unavailable",
                               input_result.error().message());
            }
            else
            {
                s_input_dispatch_original.store(input_result->original<InputDispatchFunc>(), std::memory_order_release);
                if (auto armed = hooks.push(std::move(*input_result)).enable(); !armed.has_value())
                {
                    logger.warning("Camera: Input dispatcher hook could not be armed ({}); free-look orbit "
                                   "unavailable",
                                   armed.error().message());
                }
            }
        }
    }
} // namespace TPVCamera
