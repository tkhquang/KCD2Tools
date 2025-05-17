/**
 * @file hooks/ui_overlay_hooks.cpp
 * @brief Direct hooks for UI overlay show/hide functions
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
#include "aob_scanner.h"
#include "game_interface.h"
#include "global_state.h"
#include "config.h"
#include "MinHook.h"

#include <stdexcept>

// External config reference
extern Config g_config;

// Function typedefs
typedef void(__fastcall *HideOverlaysFunc)(void *thisPtr, uint8_t paramByte, char paramChar);
typedef void(__fastcall *ShowOverlaysFunc)(void *thisPtr, uint8_t paramByte, char paramChar);

// Hook state
static HideOverlaysFunc fpHideOverlaysOriginal = nullptr;
static ShowOverlaysFunc fpShowOverlaysOriginal = nullptr;
static BYTE *g_hideOverlaysHookAddress = nullptr;
static BYTE *g_showOverlaysHookAddress = nullptr;

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
        if (WriteBytes(g_accumulatorWriteAddress, g_originalAccumulatorWriteBytes, Constants::ACCUMULATOR_WRITE_INSTR_LENGTH, logger))
        {
            g_accumulatorWriteNOPped.store(false);
            logger.log(LOG_DEBUG, "UIOverlayHook: Restored accumulator write due to hold key press");
            return true;
        }
    }
    // If hold key is released but accumulator is not NOPped, NOP it
    else if (!holdKeyPressed && !g_accumulatorWriteNOPped.load())
    {
        if (WriteBytes(g_accumulatorWriteAddress, nopSequence, Constants::ACCUMULATOR_WRITE_INSTR_LENGTH, logger))
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

        // Pattern for HideOverlays (vftable[20])
        std::vector<BYTE> hidePattern = parseAOB(Constants::UI_OVERLAY_HIDE_AOB_PATTERN);
        if (hidePattern.empty())
        {
            throw std::runtime_error("Failed to parse HideOverlays AOB pattern");
        }

        // Pattern for ShowOverlays (vftable[21])
        std::vector<BYTE> showPattern = parseAOB(Constants::UI_OVERLAY_SHOW_AOB_PATTERN);
        if (showPattern.empty())
        {
            throw std::runtime_error("Failed to parse ShowOverlays AOB pattern");
        }

        // Find HideOverlays function
        g_hideOverlaysHookAddress = FindPattern(reinterpret_cast<BYTE *>(module_base), module_size, hidePattern);
        if (!g_hideOverlaysHookAddress)
        {
            throw std::runtime_error("HideOverlays AOB pattern not found");
        }

        // Find ShowOverlays function
        g_showOverlaysHookAddress = FindPattern(reinterpret_cast<BYTE *>(module_base), module_size, showPattern);
        if (!g_showOverlaysHookAddress)
        {
            throw std::runtime_error("ShowOverlays AOB pattern not found");
        }

        logger.log(LOG_INFO, "UIOverlayHook: Found HideOverlays at " +
                                 format_address(reinterpret_cast<uintptr_t>(g_hideOverlaysHookAddress)));
        logger.log(LOG_INFO, "UIOverlayHook: Found ShowOverlays at " +
                                 format_address(reinterpret_cast<uintptr_t>(g_showOverlaysHookAddress)));

        // Create HideOverlays hook
        MH_STATUS hideStatus = MH_CreateHook(
            g_hideOverlaysHookAddress,
            reinterpret_cast<LPVOID>(HideOverlaysDetour),
            reinterpret_cast<LPVOID *>(&fpHideOverlaysOriginal));

        if (hideStatus != MH_OK)
        {
            throw std::runtime_error("MH_CreateHook for HideOverlays failed: " +
                                     std::string(MH_StatusToString(hideStatus)));
        }

        if (!fpHideOverlaysOriginal)
        {
            MH_RemoveHook(g_hideOverlaysHookAddress);
            throw std::runtime_error("MH_CreateHook for HideOverlays returned NULL trampoline");
        }

        // Create ShowOverlays hook
        MH_STATUS showStatus = MH_CreateHook(
            g_showOverlaysHookAddress,
            reinterpret_cast<LPVOID>(ShowOverlaysDetour),
            reinterpret_cast<LPVOID *>(&fpShowOverlaysOriginal));

        if (showStatus != MH_OK)
        {
            MH_RemoveHook(g_hideOverlaysHookAddress);
            throw std::runtime_error("MH_CreateHook for ShowOverlays failed: " +
                                     std::string(MH_StatusToString(showStatus)));
        }

        if (!fpShowOverlaysOriginal)
        {
            MH_RemoveHook(g_hideOverlaysHookAddress);
            MH_RemoveHook(g_showOverlaysHookAddress);
            throw std::runtime_error("MH_CreateHook for ShowOverlays returned NULL trampoline");
        }

        // Enable both hooks
        hideStatus = MH_EnableHook(g_hideOverlaysHookAddress);
        if (hideStatus != MH_OK)
        {
            MH_RemoveHook(g_hideOverlaysHookAddress);
            MH_RemoveHook(g_showOverlaysHookAddress);
            throw std::runtime_error("MH_EnableHook for HideOverlays failed: " +
                                     std::string(MH_StatusToString(hideStatus)));
        }

        showStatus = MH_EnableHook(g_showOverlaysHookAddress);
        if (showStatus != MH_OK)
        {
            MH_DisableHook(g_hideOverlaysHookAddress);
            MH_RemoveHook(g_hideOverlaysHookAddress);
            MH_RemoveHook(g_showOverlaysHookAddress);
            throw std::runtime_error("MH_EnableHook for ShowOverlays failed: " +
                                     std::string(MH_StatusToString(showStatus)));
        }

        // Set initial hold-to-scroll state if feature is enabled
        if (!g_config.hold_scroll_keys.empty() && g_accumulatorWriteAddress)
        {
            logger.log(LOG_INFO, "UIOverlayHook: Hold-to-scroll feature enabled, applying NOP by default");
            if (WriteBytes(g_accumulatorWriteAddress, nopSequence, Constants::ACCUMULATOR_WRITE_INSTR_LENGTH, logger))
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

    // Disable and remove HideOverlays hook
    if (g_hideOverlaysHookAddress && fpHideOverlaysOriginal)
    {
        MH_DisableHook(g_hideOverlaysHookAddress);
        MH_RemoveHook(g_hideOverlaysHookAddress);
        g_hideOverlaysHookAddress = nullptr;
        fpHideOverlaysOriginal = nullptr;
    }

    // Disable and remove ShowOverlays hook
    if (g_showOverlaysHookAddress && fpShowOverlaysOriginal)
    {
        MH_DisableHook(g_showOverlaysHookAddress);
        MH_RemoveHook(g_showOverlaysHookAddress);
        g_showOverlaysHookAddress = nullptr;
        fpShowOverlaysOriginal = nullptr;
    }

    // Ensure accumulator write is restored on exit
    if (g_accumulatorWriteNOPped.load() && g_accumulatorWriteAddress && g_originalAccumulatorWriteBytes[0] != 0)
    {
        logger.log(LOG_INFO, "UIOverlayHook: Restoring accumulator write before exit");
        WriteBytes(g_accumulatorWriteAddress, g_originalAccumulatorWriteBytes, Constants::ACCUMULATOR_WRITE_INSTR_LENGTH, logger);
        g_accumulatorWriteNOPped.store(false);
    }

    logger.log(LOG_DEBUG, "UIOverlayHook: Cleanup complete");
}

bool areUiOverlayHooksActive()
{
    return (g_hideOverlaysHookAddress != nullptr && fpHideOverlaysOriginal != nullptr &&
            g_showOverlaysHookAddress != nullptr && fpShowOverlaysOriginal != nullptr);
}
