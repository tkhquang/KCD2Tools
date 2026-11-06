/**
 * @file hooks/frame_tick.cpp
 * @brief The CharacterManager::Update detour (see frame_tick.hpp).
 */

#include "hooks/frame_tick.hpp"
#include "anim_catalog_console.hpp"
#include "clip_check_console.hpp"
#include "mod_maker_console.hpp"
#include "camera_view.hpp"
#include "custom_tags.hpp"
#include "mod_settings.hpp"
#include "hooks/anim_patch.hpp"
#include "hooks/hook_install.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <atomic>
#include <cstdint>

namespace AnimationFramework
{
    namespace
    {
        using CharacterManagerUpdateFunc = void(__fastcall *)(void *manager, bool paused);
        std::atomic<CharacterManagerUpdateFunc> s_update_original{nullptr};

        /** @brief Who owns the console commands: the per-frame work, or a teardown that removes them. */
        enum class TickState : std::uint8_t
        {
            /// No frame ran, so no command exists. A teardown claims this state as Removed.
            NotStarted,
            /// Each frame runs the per-frame work, which adds the commands once the console is up.
            Running,
            /// A teardown waits for the next frame to remove the commands.
            RemovalRequested,
            /// A frame claimed the request and removes the commands.
            Removing,
            /// No command remains, and the per-frame work is off.
            Removed,
        };
        std::atomic<TickState> s_tick_state{TickState::NotStarted};

        /** @brief The framework's per-frame work. Main thread only. */
        void run_frame_work() noexcept
        {
            run_mod_settings_frame_work();
            run_anim_catalog_frame_work();
            run_clip_check_frame_work();
            run_mod_maker_frame_work();
            run_anim_patch_frame_work();
            poll_custom_tags(poll_camera_view() == CameraView::ThirdPerson);
        }

        /** @brief Removes every console command the per-frame work added. Main thread only. */
        void remove_all_console_commands() noexcept
        {
            remove_mod_settings();
            remove_anim_catalog();
            remove_clip_check_console();
            remove_mod_maker_console();
            remove_anim_patch_commands();
        }

        /**
         * @brief CharacterManager::Update detour: the framework's frame work, then the game's update.
         * @note Main thread, once per frame. No-throw.
         */
        void __fastcall detour_character_manager_update(void *manager, bool paused) noexcept
        {
            TickState state = s_tick_state.load(std::memory_order_acquire);
            // The first frame races a teardown that finds no frame yet. Only the side whose exchange wins goes on.
            if (state == TickState::NotStarted &&
                s_tick_state.compare_exchange_strong(state, TickState::Running, std::memory_order_acq_rel))
            {
                state = TickState::Running;
            }
            if (state == TickState::Running)
            {
                run_frame_work();
            }
            else if (
                state == TickState::RemovalRequested &&
                s_tick_state.compare_exchange_strong(state, TickState::Removing, std::memory_order_acq_rel)
            )
            {
                remove_all_console_commands();
                s_tick_state.store(TickState::Removed, std::memory_order_release);
            }
            const CharacterManagerUpdateFunc original = s_update_original.load(std::memory_order_acquire);
            if (original != nullptr)
            {
                original(manager, paused);
            }
        }
    } // namespace

    DMK::Result<void> install_frame_tick_hook(std::uintptr_t target, DMK::hook::HookStack &hooks)
    {
        if (target == 0)
        {
            return std::unexpected(DMK::Error{DMK::ErrorCode::NoMatch, "AnimationFramework::frame_tick/target"});
        }
        return install_inline_hook<&detour_character_manager_update>(
            hooks,
            "CharacterManagerUpdate",
            target,
            s_update_original
        );
    }

    bool remove_console_commands(std::chrono::milliseconds deadline) noexcept
    {
        TickState state = TickState::NotStarted;
        if (s_tick_state.compare_exchange_strong(state, TickState::Removed, std::memory_order_acq_rel) ||
            state == TickState::Removed)
        {
            return true;
        }
        if (state == TickState::Running)
        {
            (void)s_tick_state.compare_exchange_strong(state, TickState::RemovalRequested, std::memory_order_acq_rel);
        }
        const auto until = std::chrono::steady_clock::now() + deadline;
        while (std::chrono::steady_clock::now() < until)
        {
            if (s_tick_state.load(std::memory_order_acquire) == TickState::Removed)
            {
                return true;
            }
            Sleep(1);
        }
        TickState pending = TickState::RemovalRequested;
        if (s_tick_state.compare_exchange_strong(pending, TickState::Running, std::memory_order_acq_rel))
        {
            return false;
        }
        // A frame claimed the request as the deadline ran out. Its removal is a fixed set of console calls, so the wait
        // is short and bounded.
        while (s_tick_state.load(std::memory_order_acquire) != TickState::Removed)
        {
            Sleep(1);
        }
        return true;
    }

    void resume_frame_work() noexcept
    {
        s_tick_state.store(TickState::Running, std::memory_order_release);
    }
} // namespace AnimationFramework
