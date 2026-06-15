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

#include <stdexcept>

namespace TPVCamera
{

// Function pointer types for the overlay show/hide functions.
using HideOverlaysFunc = void(__fastcall *)(void *this_ptr, uint8_t param_byte, char param_char);
using ShowOverlaysFunc = void(__fastcall *)(void *this_ptr, uint8_t param_byte, char param_char);

static HideOverlaysFunc s_hide_overlays_original = nullptr;
static ShowOverlaysFunc s_show_overlays_original = nullptr;

/**
 * @brief HideOverlays detour: a UI element is about to show, so mark the overlay active.
 * @details Calls the original exactly once before the mod-side flag store. No SEH frame is needed
 *          (unlike the deref-heavy camera/interaction detours): this detour only calls the trampoline
 *          (the engine's own function, whose faults are the engine's) and stores a mod-owned atomic, so
 *          it performs no foreign-memory dereference of its own.
 */
static void __fastcall hide_overlays_detour(void *this_ptr, uint8_t param_byte, char param_char)
{
    if (s_hide_overlays_original)
    {
        s_hide_overlays_original(this_ptr, param_byte, param_char);
    }
    overlay_state().active.store(true, std::memory_order_relaxed);
}

/**
 * @brief ShowOverlays detour: the UI element is closing, so clear the overlay flag.
 * @details Calls the original exactly once before the mod-side flag store. No SEH frame is needed: like
 *          the hide detour it only calls the trampoline and stores a mod-owned atomic, performing no
 *          foreign-memory dereference of its own.
 */
static void __fastcall show_overlays_detour(void *this_ptr, uint8_t param_byte, char param_char)
{
    if (s_show_overlays_original)
    {
        s_show_overlays_original(this_ptr, param_byte, param_char);
    }
    overlay_state().active.store(false, std::memory_order_relaxed);
}

bool initialize_ui_overlay_hooks()
{
    DMK::Logger &logger = DMK::Logger::get_instance();

    // Fail closed if the resolved entry leads with a call/breakpoint byte (a cascade mis-resolution or a
    // foreign int3 stub); a sibling mod's E9 jump-hook does not trip this gate, so layering still works.
    const DMK::HookConfig hook_config{.prologue_policy = DMK::InlineProloguePolicy::Fail};

    try
    {
        DMK::HookManager &hook_manager = DMK::HookManager::get_instance();

        const uintptr_t hide_addr = anchor_address(AnchorId::OverlayHide);
        if (hide_addr == 0)
        {
            throw std::runtime_error("HideOverlays cascade did not resolve");
        }
        auto hide_result = hook_manager.create_inline_hook(
            "HideOverlays",
            hide_addr,
            reinterpret_cast<void *>(hide_overlays_detour),
            reinterpret_cast<void **>(&s_hide_overlays_original),
            hook_config);

        if (!hide_result.has_value())
        {
            throw std::runtime_error("Failed to create HideOverlays hook: " +
                                     std::string(DMK::Hook::error_to_string(hide_result.error())));
        }

        const uintptr_t show_addr = anchor_address(AnchorId::OverlayShow);
        if (show_addr == 0)
        {
            throw std::runtime_error("ShowOverlays cascade did not resolve");
        }
        auto show_result = hook_manager.create_inline_hook(
            "ShowOverlays",
            show_addr,
            reinterpret_cast<void *>(show_overlays_detour),
            reinterpret_cast<void **>(&s_show_overlays_original),
            hook_config);

        if (!show_result.has_value())
        {
            throw std::runtime_error("Failed to create ShowOverlays hook: " +
                                     std::string(DMK::Hook::error_to_string(show_result.error())));
        }

        return true;
    }
    catch (const std::exception &e)
    {
        logger.error("UIOverlayHook: Initialization failed: {}", e.what());
        return false;
    }
}

} // namespace TPVCamera
