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
        const DetourScope in_flight;
        if (const HideOverlaysFunc original = s_hide_overlays_original.load(std::memory_order_acquire))
        {
            original(this_ptr, param_byte, param_char);
        }
        overlay_state().active.store(true, std::memory_order_relaxed);
    }

    /**
     * @brief ShowOverlays detour: the UI element is closing, so clear the overlay flag.
     * @details Calls the original exactly once before the mod-side flag store. No SEH frame is needed: like
     *          the hide detour it only calls the trampoline and stores a mod-owned atomic, performing no
     *          foreign-memory dereference of its own.
     */
    static void __fastcall show_overlays_detour(void *this_ptr, uint8_t param_byte, char param_char) noexcept
    {
        const DetourScope in_flight;
        if (const ShowOverlaysFunc original = s_show_overlays_original.load(std::memory_order_acquire))
        {
            original(this_ptr, param_byte, param_char);
        }
        overlay_state().active.store(false, std::memory_order_relaxed);
    }

    DMK::Result<void> initialize_ui_overlay_hooks(HookSet &hooks)
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
        auto hide_result = DMK::hook::inline_at(
            DMK::hook::InlineRequest{.name = "HideOverlays", .target = DMK::Address{hide_addr}}, hide_overlays_detour);
        if (!hide_result.has_value())
        {
            return std::unexpected(hide_result.error());
        }
        // Publish each trampoline and store each handle BEFORE enable() arms its patch, so the set owns a hook whose
        // arm fails with the patch live.
        s_hide_overlays_original.store(hide_result->original<HideOverlaysFunc>(), std::memory_order_release);
        if (auto armed = hooks.push(std::move(*hide_result)).enable(); !armed.has_value())
        {
            return std::unexpected(armed.error());
        }

        const uintptr_t show_addr = gated_anchor_address(Feature::OverlayState, AnchorId::OverlayShow);
        if (show_addr == 0)
        {
            return std::unexpected(DMK::Error{DMK::ErrorCode::NoMatch, "ui_overlay_hooks/show_anchor"});
        }
        auto show_result = DMK::hook::inline_at(
            DMK::hook::InlineRequest{.name = "ShowOverlays", .target = DMK::Address{show_addr}}, show_overlays_detour);
        if (!show_result.has_value())
        {
            return std::unexpected(show_result.error());
        }
        s_show_overlays_original.store(show_result->original<ShowOverlaysFunc>(), std::memory_order_release);
        if (auto armed = hooks.push(std::move(*show_result)).enable(); !armed.has_value())
        {
            return std::unexpected(armed.error());
        }

        return {};
    }

} // namespace TPVCamera
