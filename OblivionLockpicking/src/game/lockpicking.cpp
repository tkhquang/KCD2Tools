/**
 * @file game/lockpicking.cpp
 * @brief Field access and vanilla calls on a live C_LockPicking session, and the console services of the cues.
 */

#include "game/lockpicking.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>

namespace OblivionLockpicking::game
{
    namespace
    {
        using EmitterStepFn = void(__fastcall *)(void *emitter, float dt);
        using SessionFn = void(__fastcall *)(void *session);
        using ForceFinishFn = void(__fastcall *)(void *action);
        using GameContextFn = void *(__fastcall *)();
        using NoiseLevelFn = float(__fastcall *)(void *rpg_minigames, void *soul, float distance);
        using GetEntityFn = void *(__fastcall *)(void *entity_system, std::uint32_t entity_id);
        using PlaySoundFn = std::uint32_t(__fastcall *)(void *minigame, const char **trigger, void *entity);
        using ExecuteStringFn = void(__fastcall *)(void *console, const char *command, bool silent, bool defer);
        using SortedVarsFn =
            std::size_t(__fastcall *)(void *console, const char **names, std::size_t capacity, const char *prefix);
        using GetCVarFn = void *(__fastcall *)(void *console, const char *name);
        using GetIntFn = int(__fastcall *)(const void *cvar);
        using SkillLevelFn = float(__fastcall *)(void *soul, unsigned skill, bool flag);

        /// True when the last detect_sound_framework() call found sfw_play. Only the Update detour touches it.
        bool s_sound_framework = false;

        /**
         * @brief A CryString with a static header.
         * @details CryStringT keeps {refcount, length, capacity} right before the characters. PlaySound takes its
         *          string by value and releases it. The release decrements and frees only a refcount that is not
         *          negative. A refcount of -1, the engine's own mark for static strings, makes the release a no-op.
         *          The game never writes to or frees this buffer and keeps no pointer to it after the call.
         */
        struct StaticCryString
        {
            /// The refcount, -1 for a static string.
            std::int32_t references{-1};
            /// Characters before the terminator.
            std::int32_t length{0};
            /// Characters the buffer holds before the terminator.
            std::int32_t capacity{0};
            /// The characters and their terminator.
            char text[64]{};
        };

        StaticCryString s_click_trigger{};
        StaticCryString s_break_trigger{};

        /** @brief Copies @p text into @p target, cut to its capacity, and updates the header. */
        void assign(StaticCryString &target, const char *text) noexcept
        {
            const std::size_t length = std::min(std::strlen(text), sizeof(target.text) - 1);
            std::memcpy(target.text, text, length);
            target.text[length] = '\0';
            target.length = static_cast<std::int32_t>(length);
            target.capacity = static_cast<std::int32_t>(length);
        }

        /// The bound anchors, written at init and cleared at shutdown while no session update runs.
        std::atomic<const Anchors *> s_anchors{nullptr};
        /// The copy of the anchors that s_anchors points to.
        Anchors s_storage{};

        /** @brief The vanilla element behind the session's view, or nullptr when it is not a CFlashUIElement. */
        [[nodiscard]] void *view_element(const void *session, const Anchors &anchors) noexcept
        {
            void *const view = read<void *>(session, constants::LP_VIEW_OFFSET);
            if (view == nullptr)
            {
                return nullptr;
            }
            // The view has no RTTI check of its own, so its element field is read under the guard.
            const auto element = DMK::memory::read<void *>(DMK::Address{view}.offset(constants::VIEW_ELEMENT_OFFSET));
            return element && vtable_of(*element) == anchors.flash_element_vtable ? *element : nullptr;
        }

        /** @brief gEnv's console, or nullptr when it is not a CXConsole with the expected layout. */
        [[nodiscard]] void *console(const Anchors &anchors) noexcept
        {
            if (anchors.genv == 0 || anchors.console_vtable == 0)
            {
                return nullptr;
            }
            void *const object =
                read<void *>(reinterpret_cast<const void *>(anchors.genv), constants::GENV_CONSOLE_OFFSET);
            return vtable_of(object) == anchors.console_vtable ? object : nullptr;
        }

        /** @brief The suffix of @p cue's pack sound. */
        [[nodiscard]] const char *cue_name(Cue cue) noexcept
        {
            switch (cue)
            {
            case Cue::Push:
                return "push";
            case Cue::TopSlow:
                return "top_slow";
            case Cue::TopFast:
                return "top_fast";
            case Cue::Fall:
                return "fall";
            case Cue::Land:
                return "land";
            case Cue::Set:
                return "set";
            }
            return "push";
        }

        /** @brief Plays the game's sound @p trigger at the lock through the session's own PlaySound. */
        void play_vanilla_sound(void *session, const Anchors &anchors, const StaticCryString &trigger_name) noexcept
        {
            if (!anchors.vanilla_sounds)
            {
                return;
            }
            void *const entity_system =
                read<void *>(reinterpret_cast<const void *>(anchors.genv), constants::GENV_ENTITY_SYSTEM_OFFSET);
            if (vtable_of(entity_system) != anchors.entity_system_vtable)
            {
                return;
            }
            const std::uint32_t lock_id = read<std::uint32_t>(session, constants::LP_LOCK_ENTITY_OFFSET);
            const auto get_entity = reinterpret_cast<GetEntityFn>(
                slot(anchors.entity_system_vtable, constants::ENTITY_SYSTEM_GET_ENTITY_SLOT)
            );
            void *const lock_entity = get_entity(entity_system, lock_id);
            if (lock_entity == nullptr)
            {
                return;
            }
            // The callee takes the string by value, and the caller passes the address of its copy: this pointer.
            const char *trigger = trigger_name.text;
            const auto play =
                reinterpret_cast<PlaySoundFn>(slot(anchors.lockpicking_vtable, constants::MINIGAME_PLAY_SOUND_SLOT));
            play(session, &trigger, lock_entity);
        }

        /** @brief C_RPGMinigames through the game context, or nullptr when the chain or its vtable does not match. */
        [[nodiscard]] void *rpg_minigames(const Anchors &anchors) noexcept
        {
            void *const context = reinterpret_cast<GameContextFn>(anchors.game_context)();
            // Only the last object of the chain has an RTTI check, so the walk guards both hops to it. The final 0
            // makes the minigames pointer the walk's leaf.
            constexpr std::ptrdiff_t CHAIN[] = {
                constants::CONTEXT_RPG_MODULE_OFFSET,
                constants::RPG_MODULE_MINIGAMES_OFFSET,
                0,
            };
            const auto minigames = DMK::memory::walk(DMK::Address{context}, CHAIN);
            if (!minigames)
            {
                return nullptr;
            }
            void *const object = minigames->as<void *>();
            return vtable_of(object) == anchors.rpg_minigames_vtable ? object : nullptr;
        }
    } // namespace

    void bind(const Anchors &anchors) noexcept
    {
        assign(s_click_trigger, constants::SOUND_TUMBLER_TOP);
        assign(s_break_trigger, constants::SOUND_PICK_BREAK);
        s_storage = anchors;
        s_anchors.store(&s_storage, std::memory_order_release);
    }

    void unbind() noexcept
    {
        s_anchors.store(nullptr, std::memory_order_release);
    }

    const Anchors *bound_anchors() noexcept
    {
        return s_anchors.load(std::memory_order_acquire);
    }

    bool is_player_session(const void *session) noexcept
    {
        const Anchors *anchors = bound_anchors();
        if (anchors == nullptr || vtable_of(session) != anchors->lockpicking_vtable)
        {
            return false;
        }
        const void *const owner = read<const void *>(session, constants::LP_OWNER_OFFSET);
        return vtable_of(owner) == anchors->player_vtable;
    }

    void *player_soul(const void *session) noexcept
    {
        const void *const owner = read<const void *>(session, constants::LP_OWNER_OFFSET);
        return owner == nullptr ? nullptr : read<void *>(owner, constants::HUMAN_SOUL_OFFSET);
    }

    std::optional<float> thievery_level(void *soul) noexcept
    {
        const Anchors *anchors = bound_anchors();
        if (anchors == nullptr || !anchors->thievery || vtable_of(soul) != anchors->soul_vtable)
        {
            return std::nullopt;
        }
        const auto skill_level =
            reinterpret_cast<SkillLevelFn>(slot(anchors->soul_vtable, constants::SOUL_SKILL_LEVEL_SLOT));
        const float level = skill_level(soul, constants::SKILL_THIEVERY, false);
        return std::isfinite(level) ? std::optional<float>{level} : std::nullopt;
    }

    float lockpick_noise_level(void *soul, float distance) noexcept
    {
        const Anchors *anchors = bound_anchors();
        if (anchors == nullptr || soul == nullptr)
        {
            return 0.0f;
        }
        void *const minigames = rpg_minigames(*anchors);
        if (minigames == nullptr)
        {
            return 0.0f;
        }
        const auto noise_level =
            reinterpret_cast<NoiseLevelFn>(slot(anchors->rpg_minigames_vtable, constants::RPG_LOCKPICK_NOISE_SLOT));
        return noise_level(minigames, soul, distance);
    }

    bool finish_failure(void *session) noexcept
    {
        const Anchors *anchors = bound_anchors();
        if (anchors == nullptr || !anchors->short_failure)
        {
            return false;
        }
        reinterpret_cast<SessionFn>(anchors->on_fail_anim_done)(session);
        // The failure ended first, so the animation's own exit finds the state changed and does nothing.
        void *const action = read<void *>(session, constants::LP_TURN_FAIL_ACTION_OFFSET);
        if (action != nullptr && vtable_of(action) == anchors->turn_fail_vtable &&
            read<std::int32_t>(action, constants::ACTION_STATUS_OFFSET) != constants::ACTION_STATUS_FINISHED)
        {
            reinterpret_cast<ForceFinishFn>(anchors->action_force_finish)(action);
        }
        return true;
    }

    void emit_noise(void *session, float level, float dt) noexcept
    {
        const Anchors *anchors = bound_anchors();
        void *const emitter = read<void *>(session, constants::LP_EMITTER_OFFSET);
        if (anchors == nullptr || emitter == nullptr || level <= 0.0f)
        {
            return;
        }
        // The emitter has no RTTI check, so the level goes in through a guarded write that fails closed.
        if (!DMK::memory::write_in_place<float>(DMK::Address{emitter}.offset(constants::EMITTER_LEVEL_OFFSET), level))
        {
            return;
        }
        reinterpret_cast<EmitterStepFn>(anchors->emitter_step)(emitter, dt);
    }

    void *vanilla_view_element(const void *session) noexcept
    {
        const Anchors *anchors = bound_anchors();
        return anchors == nullptr ? nullptr : view_element(session, *anchors);
    }

    bool detect_sound_framework(bool main_thread) noexcept
    {
        s_sound_framework = false;
        const Anchors *anchors = bound_anchors();
        void *const object = anchors == nullptr || !main_thread ? nullptr : console(*anchors);
        if (object != nullptr)
        {
            const auto count =
                reinterpret_cast<SortedVarsFn>(slot(anchors->console_vtable, constants::CONSOLE_SORTED_VARS_SLOT));
            s_sound_framework = count(object, nullptr, 0, constants::SOUND_FRAMEWORK_PLAY) != 0;
        }
        return s_sound_framework;
    }

    std::uint32_t main_thread_id() noexcept
    {
        static std::atomic<std::uint32_t> s_main_thread{0};
        const std::uint32_t known = s_main_thread.load(std::memory_order_relaxed);
        if (known != 0)
        {
            return known;
        }
        // The visible top-level game window of this process, not the launcher splash and not a small helper window.
        HWND found = nullptr;
        EnumWindows(
            [](HWND hwnd, LPARAM param) -> BOOL
            {
                DWORD pid = 0;
                GetWindowThreadProcessId(hwnd, &pid);
                if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd))
                {
                    return TRUE;
                }
                RECT client{};
                GetClientRect(hwnd, &client);
                if (client.right < 640 || client.bottom < 480)
                {
                    return TRUE;
                }
                char name[64]{};
                GetClassNameA(hwnd, name, sizeof(name));
                if (std::strcmp(name, "SplashWindow") == 0)
                {
                    return TRUE;
                }
                *reinterpret_cast<HWND *>(param) = hwnd;
                return FALSE;
            },
            reinterpret_cast<LPARAM>(&found)
        );
        const std::uint32_t thread = found == nullptr ? 0 : GetWindowThreadProcessId(found, nullptr);
        s_main_thread.store(thread, std::memory_order_relaxed);
        return thread;
    }

    int hint_device_code() noexcept
    {
        const Anchors *anchors = bound_anchors();
        void *const object = anchors == nullptr ? nullptr : console(*anchors);
        if (object == nullptr)
        {
            return 0;
        }
        const auto get_cvar =
            reinterpret_cast<GetCVarFn>(slot(anchors->console_vtable, constants::CONSOLE_GET_CVAR_SLOT));
        const void *const cvar = get_cvar(object, constants::HINT_DEVICE_CVAR);
        const std::uintptr_t cvar_vtable = vtable_of(cvar);
        return cvar_vtable == 0 ? 0 : reinterpret_cast<GetIntFn>(slot(cvar_vtable, constants::CVAR_GET_INT_SLOT))(cvar);
    }

    void play_cue(void *session, Cue cue) noexcept
    {
        const Anchors *anchors = bound_anchors();
        if (anchors == nullptr)
        {
            return;
        }
        void *const object = s_sound_framework ? console(*anchors) : nullptr;
        if (object != nullptr)
        {
            char command[96]{};
            try
            {
                const auto written = std::format_to_n(
                    command,
                    sizeof(command) - 1,
                    "{} {}{}",
                    constants::SOUND_FRAMEWORK_PLAY,
                    constants::SOUND_PACK_PREFIX,
                    cue_name(cue)
                );
                *written.out = '\0';
            }
            catch (...)
            {
                return;
            }
            reinterpret_cast<ExecuteStringFn>(slot(anchors->console_vtable, constants::CONSOLE_EXECUTE_SLOT))(
                object,
                command,
                true,
                false
            );
            return;
        }
        if (cue == Cue::TopSlow || cue == Cue::TopFast)
        {
            play_vanilla_sound(session, *anchors, s_click_trigger);
        }
    }

    void play_break_sound(void *session) noexcept
    {
        const Anchors *anchors = bound_anchors();
        if (anchors != nullptr)
        {
            play_vanilla_sound(session, *anchors, s_break_trigger);
        }
    }
} // namespace OblivionLockpicking::game
