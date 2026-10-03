/**
 * @file hooks/frame_hook.cpp
 * @brief CCryAction::PostUpdate detour driving the main-thread tick.
 */

#include "hooks/frame_hook.hpp"
#include "aob_resolver.hpp"
#include "global_state.hpp"
#include "highlight/controller.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <atomic>
#include <cstdint>

namespace HenrySenses
{
    namespace
    {
        using PostUpdateFn = void(__fastcall *)(void *self, bool have_focus, std::uint32_t update_flags);

        PostUpdateFn s_post_update_original = nullptr;
        std::atomic<bool> s_armed{true};
        std::atomic<int> s_in_flight{0};

        // A loading screen can hold one PostUpdate for several seconds.
        constexpr DWORD RUNDOWN_TIMEOUT_MS = 10000;
        // Settle time after the counter reads zero: a thread that took the patch jump just before the prologue was
        // restored has not counted itself yet.
        constexpr DWORD ENTRY_WINDOW_MS = 50;

        /**
         * @brief PostUpdate detour: the original first, then the tick.
         * @param self CCryAction.
         * @param have_focus Forwarded.
         * @param update_flags Forwarded.
         */
        void __fastcall post_update_detour(void *self, bool have_focus, std::uint32_t update_flags) noexcept
        {
            s_in_flight.fetch_add(1, std::memory_order_acq_rel);
            s_post_update_original(self, have_focus, update_flags);
            if (s_armed.load(std::memory_order_acquire))
            {
                controller_tick();
            }
            s_in_flight.fetch_sub(1, std::memory_order_acq_rel);
        }
    } // namespace

    DMK::Result<void> initialize_frame_hook(HookSet &hooks)
    {
        const std::uintptr_t target = gated_anchor_address(Feature::Tick, AnchorId::PostUpdate);
        if (target == 0)
        {
            return std::unexpected(DMK::Error{DMK::ErrorCode::NoMatch, "frame_hook/anchor"});
        }
        s_armed.store(true, std::memory_order_release);
        DMK_TRY(
            created,
            DMK::hook::inline_at(
                DMK::hook::InlineRequest{
                    .name = "CryActionPostUpdate",
                    .target = DMK::Address{target},
                },
                post_update_detour
            )
        );
        s_post_update_original = created.original<PostUpdateFn>();
        // Stored before it is armed: the set owns it through teardown even when the arm fails half-way.
        DMK::hook::Hook &stored = hooks.push(std::move(created));
        DMK_TRY_VOID(stored.enable());
        DMK::log().info(
            "FrameHook: CCryAction::PostUpdate hooked at {} (RVA {:#x}); the main-thread tick is live",
            DMK::format::format_address(target),
            target - module_info().base
        );
        return {};
    }

    void disarm_frame_hook() noexcept
    {
        s_armed.store(false, std::memory_order_release);
    }

    bool rundown_frame_hook() noexcept
    {
        const ULONGLONG deadline = GetTickCount64() + RUNDOWN_TIMEOUT_MS;
        for (;;)
        {
            while (s_in_flight.load(std::memory_order_acquire) != 0 && GetTickCount64() < deadline)
            {
                Sleep(5);
            }
            Sleep(ENTRY_WINDOW_MS);
            if (s_in_flight.load(std::memory_order_acquire) == 0)
            {
                return true;
            }
            if (GetTickCount64() >= deadline)
            {
                (void)DMK::log().log_noexcept(
                    DMK::LogLevel::Error,
                    "FrameHook: the main thread was still inside "
                    "PostUpdate at shutdown"
                );
                return false;
            }
        }
    }

} // namespace HenrySenses
