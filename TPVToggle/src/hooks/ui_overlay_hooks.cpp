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

#include "ui_overlay_hooks.h"
#include "logger.h"
#include "constants.h"
#include "utils.h"
#include "game_interface.h"
#include "global_state.h"
#include "config.h"

#include <DetourModKit.hpp>

#include <stdexcept>

using DMKString::format_address;

// External config reference
extern Config g_config;

// Function typedefs
typedef void(__fastcall *HideOverlaysFunc)(void *thisPtr, uint8_t paramByte, char paramChar);
typedef void(__fastcall *ShowOverlaysFunc)(void *thisPtr, uint8_t paramByte, char paramChar);

// Hook state
static HideOverlaysFunc fpHideOverlaysOriginal = nullptr;
static ShowOverlaysFunc fpShowOverlaysOriginal = nullptr;
static std::string g_hideOverlaysHookId;
static std::string g_showOverlaysHookId;

// NOP pattern for accumulator write
static const BYTE nopSequence[Constants::ACCUMULATOR_WRITE_INSTR_LENGTH] = {0x90, 0x90, 0x90, 0x90, 0x90};

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
    Logger &logger = Logger::getInstance();

    try
    {
        // UI overlay is about to hide, which means
        // another UI element (menu, dialog, etc.) is about to show
        logger.log(LOG_DEBUG, "UIOverlayHook: HideOverlays called - UI element will show");

        // Call the original function
        if (fpHideOverlaysOriginal)
        {
            fpHideOverlaysOriginal(thisPtr, paramByte, paramChar);
        }
        else
        {
            logger.log(LOG_ERROR, "UIOverlayHook: HideOverlays original function pointer is NULL");
        }

        // If is currently in overlays and open another overlay, skip
        if (!g_isOverlayActive.load())
        {
            // Remember if we're currently in TPV mode
            int viewState = getViewState();
            if (viewState == 1)
            {
                // We're in TPV - remember this for later restoration
                g_wasTpvBeforeOverlay.store(true);
                logger.log(LOG_DEBUG, "UIOverlayHook: Stored TPV state for later restoration");
            }
            else
            {
                // We're already in FPV or unknown state
                g_wasTpvBeforeOverlay.store(false);
            }

            // Request switch to FPV when UI shows
            // This will be processed by the monitor thread
            g_overlayFpvRequest.store(true);

            resetScrollAccumulator(true);
            // Mark overlay as active
            g_isOverlayActive.store(true);
        }
        else
        {
            // Request switch to FPV when UI shows
            // This will be processed by the monitor thread
            g_overlayFpvRequest.store(true);
        }
    }
    catch (const std::exception &e)
    {
        logger.log(LOG_ERROR, "UIOverlayHook: Exception in HideOverlays detour: " + std::string(e.what()));

        // Call the original function even if we had an exception
        if (fpHideOverlaysOriginal)
        {
            fpHideOverlaysOriginal(thisPtr, paramByte, paramChar);
        }
    }
    catch (...)
    {
        logger.log(LOG_ERROR, "UIOverlayHook: Unknown exception in HideOverlays detour");

        // Call the original function even if we had an exception
        if (fpHideOverlaysOriginal)
        {
            fpHideOverlaysOriginal(thisPtr, paramByte, paramChar);
        }
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
    Logger &logger = Logger::getInstance();

    try
    {
        // Before calling original - UI overlay is about to show, which means
        // another UI element (menu, dialog, etc.) is about to hide
        logger.log(LOG_DEBUG, "UIOverlayHook: ShowOverlays called - UI element will hide");

        // Call the original function first
        if (fpShowOverlaysOriginal)
        {
            fpShowOverlaysOriginal(thisPtr, paramByte, paramChar);
        }
        else
        {
            logger.log(LOG_ERROR, "UIOverlayHook: ShowOverlays original function pointer is NULL");
        }

        resetScrollAccumulator(true);
        // Mark overlay as inactive
        g_isOverlayActive.store(false);

        // Request restoration to TPV if that was the previous state
        if (g_wasTpvBeforeOverlay.load())
        {
            logger.log(LOG_DEBUG, "UIOverlayHook: Requesting TPV restoration");
            g_overlayTpvRestoreRequest.store(true);
        }
        else
        {
            logger.log(LOG_DEBUG, "UIOverlayHook: No TPV restoration needed");
        }

        // Reset restoration flag
        g_wasTpvBeforeOverlay.store(false);
    }
    catch (const std::exception &e)
    {
        logger.log(LOG_ERROR, "UIOverlayHook: Exception in ShowOverlays detour: " + std::string(e.what()));

        // Call the original function even if we had an exception
        if (fpShowOverlaysOriginal)
        {
            fpShowOverlaysOriginal(thisPtr, paramByte, paramChar);
        }
    }
    catch (...)
    {
        logger.log(LOG_ERROR, "UIOverlayHook: Unknown exception in ShowOverlays detour");

        // Call the original function even if we had an exception
        if (fpShowOverlaysOriginal)
        {
            fpShowOverlaysOriginal(thisPtr, paramByte, paramChar);
        }
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
    Logger &logger = Logger::getInstance();

    // Skip if no accumulator address found or if overlay is active
    if (!g_accumulatorWriteAddress || g_isOverlayActive.load())
    {
        return false;
    }

    // If hold key is pressed but accumulator is currently NOPped, restore original bytes
    if (holdKeyPressed && g_accumulatorWriteNOPped.load())
    {
        if (DMKMemory::WriteBytes(g_accumulatorWriteAddress,
                                  g_originalAccumulatorWriteBytes,
                                  Constants::ACCUMULATOR_WRITE_INSTR_LENGTH,
                                  DMKLogger::getInstance()))
        {
            g_accumulatorWriteNOPped.store(false);
            logger.log(LOG_DEBUG, "UIOverlayHook: Restored accumulator write due to hold key press");
            return true;
        }
    }
    // If hold key is released but accumulator is not NOPped, NOP it
    else if (!holdKeyPressed && !g_accumulatorWriteNOPped.load())
    {
        if (DMKMemory::WriteBytes(g_accumulatorWriteAddress,
                                  reinterpret_cast<const std::byte *>(nopSequence),
                                  Constants::ACCUMULATOR_WRITE_INSTR_LENGTH,
                                  DMKLogger::getInstance()))
        {
            g_accumulatorWriteNOPped.store(true);
            logger.log(LOG_DEBUG, "UIOverlayHook: NOPped accumulator write due to hold key release");
            resetScrollAccumulator(true);
            return true;
        }
    }

    return false;
}

bool initializeUiOverlayHooks(uintptr_t module_base, size_t module_size)
{
    Logger &logger = Logger::getInstance();

    try
    {
        logger.log(LOG_INFO, "UIOverlayHook: Initializing UI overlay hooks...");

        // Use DMKHookManager to create hooks via AOB scan
        DMKHookManager &hook_manager = DMKHookManager::getInstance();

        // Create HideOverlays hook
        g_hideOverlaysHookId = hook_manager.create_inline_hook_aob(
            "HideOverlays",
            module_base,
            module_size,
            Constants::UI_OVERLAY_HIDE_AOB_PATTERN,
            0,
            reinterpret_cast<void *>(HideOverlaysDetour),
            reinterpret_cast<void **>(&fpHideOverlaysOriginal));

        if (g_hideOverlaysHookId.empty())
        {
            throw std::runtime_error("Failed to create HideOverlays hook via AOB scan");
        }

        // Create ShowOverlays hook
        g_showOverlaysHookId = hook_manager.create_inline_hook_aob(
            "ShowOverlays",
            module_base,
            module_size,
            Constants::UI_OVERLAY_SHOW_AOB_PATTERN,
            0,
            reinterpret_cast<void *>(ShowOverlaysDetour),
            reinterpret_cast<void **>(&fpShowOverlaysOriginal));

        if (g_showOverlaysHookId.empty())
        {
            hook_manager.remove_hook(g_hideOverlaysHookId);
            g_hideOverlaysHookId.clear();
            throw std::runtime_error("Failed to create ShowOverlays hook via AOB scan");
        }

        // Log hook addresses
        DMK::InlineHook *hideHook = hook_manager.get_inline_hook(g_hideOverlaysHookId);
        DMK::InlineHook *showHook = hook_manager.get_inline_hook(g_showOverlaysHookId);

        if (hideHook)
        {
            logger.log(LOG_INFO, "UIOverlayHook: Found HideOverlays at " +
                                     format_address(hideHook->getTargetAddress()));
        }
        if (showHook)
        {
            logger.log(LOG_INFO, "UIOverlayHook: Found ShowOverlays at " +
                                     format_address(showHook->getTargetAddress()));
        }

        // Set initial hold-to-scroll state if feature is enabled
        if (!g_config.hold_scroll_keys.empty() && g_accumulatorWriteAddress)
        {
            logger.log(LOG_INFO, "UIOverlayHook: Hold-to-scroll feature enabled, applying NOP by default");
            if (DMKMemory::WriteBytes(g_accumulatorWriteAddress,
                                      reinterpret_cast<const std::byte *>(nopSequence),
                                      Constants::ACCUMULATOR_WRITE_INSTR_LENGTH,
                                      DMKLogger::getInstance()))
            {
                g_accumulatorWriteNOPped.store(true);
            }
        }

        logger.log(LOG_INFO, "UIOverlayHook: UI overlay hooks successfully installed");
        return true;
    }
    catch (const std::exception &e)
    {
        logger.log(LOG_ERROR, "UIOverlayHook: Initialization failed: " + std::string(e.what()));
        cleanupUiOverlayHooks();
        return false;
    }
}

void cleanupUiOverlayHooks()
{
    Logger &logger = Logger::getInstance();
    DMKHookManager &hook_manager = DMKHookManager::getInstance();

    // Remove HideOverlays hook
    if (!g_hideOverlaysHookId.empty())
    {
        hook_manager.remove_hook(g_hideOverlaysHookId);
        g_hideOverlaysHookId.clear();
        fpHideOverlaysOriginal = nullptr;
    }

    // Remove ShowOverlays hook
    if (!g_showOverlaysHookId.empty())
    {
        hook_manager.remove_hook(g_showOverlaysHookId);
        g_showOverlaysHookId.clear();
        fpShowOverlaysOriginal = nullptr;
    }

    // Ensure accumulator write is restored on exit
    if (g_accumulatorWriteNOPped.load() && g_accumulatorWriteAddress && g_originalAccumulatorWriteBytes[0] != std::byte{0})
    {
        logger.log(LOG_INFO, "UIOverlayHook: Restoring accumulator write before exit");
        DMKMemory::WriteBytes(g_accumulatorWriteAddress,
                              g_originalAccumulatorWriteBytes,
                              Constants::ACCUMULATOR_WRITE_INSTR_LENGTH,
                              DMKLogger::getInstance());
        g_accumulatorWriteNOPped.store(false);
    }

    logger.log(LOG_DEBUG, "UIOverlayHook: Cleanup complete");
}

bool areUiOverlayHooksActive()
{
    return (!g_hideOverlaysHookId.empty() && !g_showOverlaysHookId.empty());
}
