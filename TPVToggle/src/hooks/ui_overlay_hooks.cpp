/**
 * @file hooks/ui_overlay_hooks.cpp
 * @brief Direct hooks for UI overlay show/hide functions using DetourModKit.
 *
 * Implements hooks that directly intercept the game's UI overlay show and hide
 * functions rather than continuously polling for overlay state changes.
 *
 * This approach provides more reliable and immediate detection of UI state
 * changes with lower performance overhead than polling.
 */

#include "ui_overlay_hooks.hpp"
#include "constants.hpp"
#include "utils.hpp"
#include "game_interface.hpp"
#include "global_state.hpp"
#include "config.hpp"

#include <DetourModKit.hpp>

#include <stdexcept>


// External config reference
extern Config g_config;

namespace TPVToggle
{

// Function pointer types for the overlay show/hide functions.
using HideOverlaysFunc = void(__fastcall *)(void *thisPtr, uint8_t paramByte, char paramChar);
using ShowOverlaysFunc = void(__fastcall *)(void *thisPtr, uint8_t paramByte, char paramChar);

// Hook state
static HideOverlaysFunc fpHideOverlaysOriginal = nullptr;
static ShowOverlaysFunc fpShowOverlaysOriginal = nullptr;

// NOP pattern for the accumulator write. Typed as std::byte so it passes straight
// to DMK::Memory::write_bytes without a cast.
static constexpr std::byte nopSequence[Constants::ACCUMULATOR_WRITE_INSTR_LENGTH] = {
    std::byte{0x90}, std::byte{0x90}, std::byte{0x90}, std::byte{0x90}, std::byte{0x90}};

/**
 * @brief Detour function for the HideOverlays function
 * @details Intercepts when UI overlay is about to be hidden and requests a
 *          switch to first-person view before any UI elements appear.
 *          Also handles scroll state and NOP'ing scroll accumulator.
 * @param thisPtr Pointer to the UI overlay object
 * @param paramByte First parameter to the original function (byte)
 * @param paramChar Second parameter to the original function (char)
 */
static void __fastcall HideOverlaysDetour(void *thisPtr, uint8_t paramByte, char paramChar)
{
    DMK::Logger &logger = DMK::Logger::get_instance();

    // UI overlay is about to hide, which means another UI element (menu, dialog,
    // etc.) is about to show. Call the original exactly once, before the mod-side
    // work that reacts to the post-call state; guarding the original inside the
    // try would let an exception in the mod logic call it a second time.
    logger.debug("UIOverlayHook: HideOverlays called - UI element will show");

    if (fpHideOverlaysOriginal)
    {
        fpHideOverlaysOriginal(thisPtr, paramByte, paramChar);
    }
    else
    {
        logger.error("UIOverlayHook: HideOverlays original function pointer is NULL");
    }

    try
    {
        // If already in an overlay and another overlay opens, only re-request FPV.
        if (!TPVToggle::overlay_state().active.load())
        {
            // Remember whether we are currently in TPV mode so it can be restored.
            const int viewState = getViewState();
            if (viewState == 1)
            {
                TPVToggle::overlay_state().wasTpvBeforeOverlay.store(true);
                logger.debug("UIOverlayHook: Stored TPV state for later restoration");
            }
            else
            {
                TPVToggle::overlay_state().wasTpvBeforeOverlay.store(false);
            }

            // Request the switch to FPV; the monitor thread processes it.
            TPVToggle::overlay_state().fpvRequest.store(true);

            resetScrollAccumulator(true);
            TPVToggle::overlay_state().active.store(true);
        }
        else
        {
            TPVToggle::overlay_state().fpvRequest.store(true);
        }
    }
    catch (const std::exception &e)
    {
        logger.error("UIOverlayHook: Exception in HideOverlays detour: {}", e.what());
    }
    catch (...)
    {
        logger.error("UIOverlayHook: Unknown exception in HideOverlays detour");
    }
}

/**
 * @brief Detour function for the ShowOverlays function
 * @details Intercepts when UI overlay is about to be shown again and requests
 *          a switch back to third-person view if that was the previous state.
 *          Also handles restoring scroll accumulator functionality.
 * @param thisPtr Pointer to the UI overlay object
 * @param paramByte First parameter to the original function (byte)
 * @param paramChar Second parameter to the original function (char)
 */
static void __fastcall ShowOverlaysDetour(void *thisPtr, uint8_t paramByte, char paramChar)
{
    DMK::Logger &logger = DMK::Logger::get_instance();

    // UI overlay is about to show, which means another UI element (menu, dialog,
    // etc.) is about to hide. Call the original exactly once, before the mod-side
    // work; guarding it inside the try would let an exception in the mod logic
    // call it a second time.
    logger.debug("UIOverlayHook: ShowOverlays called - UI element will hide");

    if (fpShowOverlaysOriginal)
    {
        fpShowOverlaysOriginal(thisPtr, paramByte, paramChar);
    }
    else
    {
        logger.error("UIOverlayHook: ShowOverlays original function pointer is NULL");
    }

    try
    {
        resetScrollAccumulator(true);
        TPVToggle::overlay_state().active.store(false);

        // Request restoration to TPV if that was the previous state.
        if (TPVToggle::overlay_state().wasTpvBeforeOverlay.load())
        {
            logger.debug("UIOverlayHook: Requesting TPV restoration");
            TPVToggle::overlay_state().tpvRestoreRequest.store(true);
        }
        else
        {
            logger.debug("UIOverlayHook: No TPV restoration needed");
        }

        TPVToggle::overlay_state().wasTpvBeforeOverlay.store(false);
    }
    catch (const std::exception &e)
    {
        logger.error("UIOverlayHook: Exception in ShowOverlays detour: {}", e.what());
    }
    catch (...)
    {
        logger.error("UIOverlayHook: Unknown exception in ShowOverlays detour");
    }
}

/**
 * @brief Handler for hold-to-scroll key state changes
 * @details Called by the main monitor thread when hold-to-scroll key state changes
 * @param holdKeyPressed Whether a hold key is currently pressed
 * @return true if the state was successfully handled, false otherwise
 */
bool handleHoldToScrollKeyState(bool holdKeyPressed)
{
    DMK::Logger &logger = DMK::Logger::get_instance();

    // Skip if no accumulator address found or if overlay is active
    if (!TPVToggle::scroll_hook_state().writeAddress || TPVToggle::overlay_state().active.load())
    {
        return false;
    }

    // If hold key is pressed but accumulator is currently NOPped, restore original bytes
    if (holdKeyPressed && TPVToggle::scroll_hook_state().nopped.load())
    {
        if (DMK::Memory::write_bytes(TPVToggle::scroll_hook_state().writeAddress,
                                   TPVToggle::scroll_hook_state().originalWriteBytes,
                                   Constants::ACCUMULATOR_WRITE_INSTR_LENGTH)
                .has_value())
        {
            TPVToggle::scroll_hook_state().nopped.store(false);
            logger.debug("UIOverlayHook: Restored accumulator write due to hold key press");
            return true;
        }
    }
    // If hold key is released but accumulator is not NOPped, NOP it
    else if (!holdKeyPressed && !TPVToggle::scroll_hook_state().nopped.load())
    {
        if (DMK::Memory::write_bytes(TPVToggle::scroll_hook_state().writeAddress,
                                   nopSequence,
                                   Constants::ACCUMULATOR_WRITE_INSTR_LENGTH)
                .has_value())
        {
            TPVToggle::scroll_hook_state().nopped.store(true);
            logger.debug("UIOverlayHook: NOPped accumulator write due to hold key release");
            resetScrollAccumulator(true);
            return true;
        }
    }

    return false;
}

bool initializeUiOverlayHooks(uintptr_t module_base, size_t module_size)
{
    DMK::Logger &logger = DMK::Logger::get_instance();

    try
    {
        DMK::HookManager &hook_manager = DMK::HookManager::get_instance();

        auto hideResult = hook_manager.create_inline_hook_aob(
            "HideOverlays",
            module_base,
            module_size,
            Constants::UI_OVERLAY_HIDE_AOB_PATTERN,
            0,
            reinterpret_cast<void *>(HideOverlaysDetour),
            reinterpret_cast<void **>(&fpHideOverlaysOriginal));

        if (!hideResult.has_value())
        {
            throw std::runtime_error("Failed to create HideOverlays hook: " + std::string(DMK::Hook::error_to_string(hideResult.error())));
        }

        auto showResult = hook_manager.create_inline_hook_aob(
            "ShowOverlays",
            module_base,
            module_size,
            Constants::UI_OVERLAY_SHOW_AOB_PATTERN,
            0,
            reinterpret_cast<void *>(ShowOverlaysDetour),
            reinterpret_cast<void **>(&fpShowOverlaysOriginal));

        if (!showResult.has_value())
        {
            throw std::runtime_error("Failed to create ShowOverlays hook: " + std::string(DMK::Hook::error_to_string(showResult.error())));
        }

        // Set initial hold-to-scroll state if feature is enabled
        if (!g_config.hold_scroll_keys.empty() && TPVToggle::scroll_hook_state().writeAddress)
        {
            logger.info("UIOverlayHook: Hold-to-scroll feature enabled, applying NOP by default");
            if (DMK::Memory::write_bytes(TPVToggle::scroll_hook_state().writeAddress,
                                      nopSequence,
                                      Constants::ACCUMULATOR_WRITE_INSTR_LENGTH)
                    .has_value())
            {
                TPVToggle::scroll_hook_state().nopped.store(true);
            }
        }

        return true;
    }
    catch (const std::exception &e)
    {
        logger.error("UIOverlayHook: Initialization failed: {}", e.what());
        return false;
    }
}

} // namespace TPVToggle
