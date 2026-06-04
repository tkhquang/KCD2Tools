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
#include "constants.hpp"
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
 * @details Calls the original exactly once before the mod-side flag store; guarding
 *          the original inside the try would let an exception call it a second time.
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
 * @details Calls the original exactly once before the mod-side flag store.
 */
static void __fastcall show_overlays_detour(void *this_ptr, uint8_t param_byte, char param_char)
{
    if (s_show_overlays_original)
    {
        s_show_overlays_original(this_ptr, param_byte, param_char);
    }
    overlay_state().active.store(false, std::memory_order_relaxed);
}

bool initialize_ui_overlay_hooks(uintptr_t module_base, size_t module_size)
{
    DMK::Logger &logger = DMK::Logger::get_instance();

    try
    {
        DMK::HookManager &hook_manager = DMK::HookManager::get_instance();

        auto hide_result = hook_manager.create_inline_hook_aob(
            "HideOverlays",
            module_base,
            module_size,
            Constants::UI_OVERLAY_HIDE_AOB_PATTERN,
            0,
            reinterpret_cast<void *>(hide_overlays_detour),
            reinterpret_cast<void **>(&s_hide_overlays_original));

        if (!hide_result.has_value())
        {
            throw std::runtime_error("Failed to create HideOverlays hook: " +
                                     std::string(DMK::Hook::error_to_string(hide_result.error())));
        }

        auto show_result = hook_manager.create_inline_hook_aob(
            "ShowOverlays",
            module_base,
            module_size,
            Constants::UI_OVERLAY_SHOW_AOB_PATTERN,
            0,
            reinterpret_cast<void *>(show_overlays_detour),
            reinterpret_cast<void **>(&s_show_overlays_original));

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
