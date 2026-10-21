/**
 * @file game/main_thread.cpp
 * @brief The CSystem::Update hook.
 */

#include "game/main_thread.hpp"
#include "game/console_commands.hpp"
#include "game/cutscene_voices.hpp"
#include "game/entity_tracker.hpp"
#include "game/game_clock.hpp"
#include "game/lip_drive.hpp"
#include "game/speech.hpp"

#include <windows.h>

#include <atomic>

namespace SoundFramework::game::main_thread
{
    namespace
    {
        using SystemUpdateFn = bool(__fastcall *)(void *system, int flags, int pause_mode);

        SystemUpdateFn s_original = nullptr;
        std::atomic<DWORD> s_thread_id{0};

        bool __fastcall system_update_detour(void *system, int flags, int pause_mode) noexcept
        {
            s_thread_id.store(GetCurrentThreadId(), std::memory_order_relaxed);
            game_clock::tick();
            commands::on_frame(system);
            entities::on_frame();
            cutscene_voices::on_frame();
            speech::on_frame();
            lip_drive::on_frame();
            return s_original(system, flags, pause_mode);
        }
    } // namespace

    DMK::Result<void> start(const Anchors &anchors, DMK::hook::HookStack &hooks)
    {
        DMK_TRY(
            created,
            DMK::hook::inline_at<&system_update_detour>(DMK::hook::InlineRequest{
                .name = "CSystemUpdate",
                .target = DMK::Address{anchors.system_update},
            })
        );
        s_original = created.original<SystemUpdateFn>();
        // Stored before it is armed: the stack owns it through teardown even when the arm fails half-way.
        DMK::hook::Hook &stored = hooks.push(std::move(created));
        DMK_TRY_VOID(stored.enable());
        DMK::log().info("MainThread: CSystem::Update hooked");
        return {};
    }

    bool is_main_thread() noexcept
    {
        return s_thread_id.load(std::memory_order_relaxed) == GetCurrentThreadId();
    }
} // namespace SoundFramework::game::main_thread
