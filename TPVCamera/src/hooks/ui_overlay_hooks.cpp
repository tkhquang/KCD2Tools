/**
 * @file hooks/ui_overlay_hooks.cpp
 * @brief Direct hooks for UI overlay show/hide functions using DetourModKit.
 *
 * Intercepts the game's UI overlay show and hide functions to maintain a single
 * flag, overlay_state().active, instead of polling. HideOverlays runs as a UI
 * element (inventory, map, dialog, codex) opens, ShowOverlays as it closes; the
 * camera reads that flag in should_apply_view() to keep the third-person
 * offset suppressed under any UI.
 */

#include "ui_overlay_hooks.hpp"
#include "aob_resolver.hpp"
#include "global_state.hpp"
#include "screen_effects.hpp"

#include <DetourModKit.hpp>

#include <atomic>

namespace TPVCamera
{

    // Function pointer types for the overlay show/hide functions.
    using HideOverlaysFunc = void(__fastcall *)(void *this_ptr, uint8_t param_byte, char param_char);
    using ShowOverlaysFunc = void(__fastcall *)(void *this_ptr, uint8_t param_byte, char param_char);

    static std::atomic<HideOverlaysFunc> s_hide_overlays_original{nullptr};
    static std::atomic<ShowOverlaysFunc> s_show_overlays_original{nullptr};

    /**
     * @brief HideOverlays detour: a UI element is about to show, so mark the overlay active.
     * @details Calls the original exactly once before the mod-side flag store. No SEH frame is needed
     *          (unlike the deref-heavy camera/interaction detours): this detour only calls the trampoline
     *          (the engine's own function, whose faults are the engine's) and stores a mod-owned atomic, so
     *          it performs no foreign-memory dereference of its own.
     */
    static void __fastcall hide_overlays_detour(void *this_ptr, uint8_t param_byte, char param_char) noexcept
    {
        if (const HideOverlaysFunc original = s_hide_overlays_original.load(std::memory_order_acquire))
        {
            original(this_ptr, param_byte, param_char);
        }
        overlay_state().active.store(true, std::memory_order_relaxed);
        capture_ui_overlay(this_ptr);
    }

    /**
     * @brief ShowOverlays detour: the UI element is closing, so clear the overlay flag.
     * @details Calls the original exactly once before the mod-side flag store. No SEH frame is needed: like
     *          the hide detour it only calls the trampoline and stores a mod-owned atomic, performing no
     *          foreign-memory dereference of its own.
     */
    static void __fastcall show_overlays_detour(void *this_ptr, uint8_t param_byte, char param_char) noexcept
    {
        if (const ShowOverlaysFunc original = s_show_overlays_original.load(std::memory_order_acquire))
        {
            original(this_ptr, param_byte, param_char);
        }
        overlay_state().active.store(false, std::memory_order_relaxed);
        capture_ui_overlay(this_ptr);
        // The original turned every screen particle back on; the ones the current view hides go off again.
        reassert_screen_effects();
    }

    DMK::Result<void> initialize_ui_overlay_hooks(DMK::hook::HookStack &hooks)
    {
        // The default hook::Options prologue policy is Fail: refuse the install when the resolved entry
        // leads with a call or breakpoint byte, the shape a cascade mis-resolution or a foreign int3 stub
        // produces. A sibling mod's E9 jump hook decodes as a relocatable branch rather than a refusal, so
        // layering still works. Every refusal below is returned as the library's own typed Error.

        const uintptr_t hide_addr = gated_anchor_address(Feature::OverlayState, AnchorId::OverlayHide);
        if (hide_addr == 0)
        {
            return std::unexpected(DMK::Error{DMK::ErrorCode::NoMatch, "ui_overlay_hooks/hide_anchor"});
        }
        const auto hide_installed = hooks.install<&hide_overlays_detour>(DMK::hook::InlineInstallRequest{
            .name = "HideOverlays",
            .target = DMK::Address{hide_addr},
            .original = &s_hide_overlays_original,
        });
        if (!hide_installed.has_value())
        {
            return std::unexpected(hide_installed.error());
        }

        const uintptr_t show_addr = gated_anchor_address(Feature::OverlayState, AnchorId::OverlayShow);
        if (show_addr == 0)
        {
            return std::unexpected(DMK::Error{DMK::ErrorCode::NoMatch, "ui_overlay_hooks/show_anchor"});
        }
        const auto show_installed = hooks.install<&show_overlays_detour>(DMK::hook::InlineInstallRequest{
            .name = "ShowOverlays",
            .target = DMK::Address{show_addr},
            .original = &s_show_overlays_original,
        });
        if (!show_installed.has_value())
        {
            return std::unexpected(show_installed.error());
        }

        return {};
    }

} // namespace TPVCamera
