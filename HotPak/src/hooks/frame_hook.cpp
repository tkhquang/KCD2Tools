/**
 * @file hooks/frame_hook.cpp
 * @brief CCryAction::PostUpdate detour driving the main-thread tick.
 */

#include "hooks/frame_hook.hpp"
#include "reload_pipeline.hpp"

#include <DetourModKit.hpp>

#include <cstdint>

namespace HotPak
{
    namespace
    {
        using PostUpdateFn = void(__fastcall *)(void *self, bool have_focus, std::uint32_t update_flags);

        PostUpdateFn s_post_update_original = nullptr;

        /**
         * @brief PostUpdate detour: the original first, then the tick.
         * @param self CCryAction.
         * @param have_focus Forwarded.
         * @param update_flags Forwarded.
         */
        void __fastcall post_update_detour(void *self, bool have_focus, std::uint32_t update_flags) noexcept
        {
            s_post_update_original(self, have_focus, update_flags);
            reload_tick();
        }
    } // namespace

    DMK::Result<void> initialize_frame_hook(DMK::hook::HookStack &hooks, std::uintptr_t target)
    {
        if (target == 0)
        {
            return std::unexpected(DMK::Error{DMK::ErrorCode::InvalidArg, "frame_hook/anchor"});
        }
        DMK_TRY(
            created,
            DMK::hook::inline_at<&post_update_detour>(DMK::hook::InlineRequest{
                .name = "CryActionPostUpdate",
                .target = DMK::Address{target},
            })
        );
        s_post_update_original = created.original<PostUpdateFn>();
        // Stored before it is armed: the stack owns it through teardown even when the arm fails half-way.
        DMK::hook::Hook &stored = hooks.push(std::move(created));
        DMK_TRY_VOID(stored.enable());
        DMK::log().info(
            "FrameHook: CCryAction::PostUpdate hooked at {}; the main-thread tick is live",
            DMK::format::format_address(target)
        );
        return {};
    }

} // namespace HotPak
