/**
 * @file game/main_thread.cpp
 * @brief The main-thread tick, a mid hook inside CSystem::Update.
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
        std::atomic<DWORD> s_thread_id{0};

        /** @brief The tick callback. At the site, rsi holds the CSystem* that the prologue moved there from rcx. */
        void system_update_tick(DMK::hook::MidContext &context) noexcept
        {
            s_thread_id.store(GetCurrentThreadId(), std::memory_order_relaxed);
            game_clock::tick();
            commands::on_frame(reinterpret_cast<void *>(DMK::hook::gpr(context, DMK::hook::Gpr::Rsi)));
            entities::on_frame();
            cutscene_voices::on_frame();
            speech::on_frame();
            lip_drive::on_frame();
        }
    } // namespace

    DMK::Result<void> start(const Anchors &anchors, DMK::hook::HookStack &hooks)
    {
        DMK_TRY_VOID(hooks.install(
            DMK::hook::MidRequest{
                .name = "CSystemUpdate",
                .target = DMK::Address{anchors.system_update_tick},
            },
            &system_update_tick
        ));
        DMK::log().info(
            "MainThread: CSystem::Update hooked at {}",
            DMK::format::format_address(anchors.system_update_tick)
        );
        return {};
    }

    bool is_main_thread() noexcept
    {
        return s_thread_id.load(std::memory_order_relaxed) == GetCurrentThreadId();
    }
} // namespace SoundFramework::game::main_thread
