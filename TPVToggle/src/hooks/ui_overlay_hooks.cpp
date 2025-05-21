/**
 * @file hooks/ui_overlay_hooks.cpp
 * @brief Direct hooks for UI overlay show/hide functions
 *
 * Implements hooks that directly intercept the game's UI overlay show and hide
 * functions rather than continuously polling for overlay state changes.
 */

#include "ui_overlay_hooks.h"
#include "logger.h"
#include "constants.h"
#include "utils.h"
// #include "aob_scanner.h" // No longer needed here
#include "game_interface.h" // For getViewState, setViewState, resetScrollAccumulator
#include "global_state.h"
#include "config.h"
#include "hook_manager.hpp" // Use HookManager
#include "ui_menu_hooks.h"

#include <stdexcept>
#include <string> // For std::string

// External config reference
extern Config g_config;

// Function typedefs for game's overlay functions
typedef void(__fastcall *HideOverlaysFunc)(void *thisPtr, uint8_t paramByte, char paramChar);
typedef void(__fastcall *ShowOverlaysFunc)(void *thisPtr, uint8_t paramByte, char paramChar);

// Hook state
static HideOverlaysFunc fpHideOverlaysOriginal = nullptr;
static ShowOverlaysFunc fpShowOverlaysOriginal = nullptr;
// static BYTE *g_hideOverlaysHookAddress = nullptr; // Managed by HookManager
// static BYTE *g_showOverlaysHookAddress = nullptr; // Managed by HookManager
static std::string g_hideOverlaysHookId = "";
static std::string g_showOverlaysHookId = "";

// NOP pattern for accumulator write
static const BYTE nopSequence[Constants::ACCUMULATOR_WRITE_INSTR_LENGTH] = {0x90, 0x90, 0x90, 0x90, 0x90};

/**
 * @brief Detour function for the HideOverlays function
 * @details Intercepts when UI overlay is about to be hidden (meaning another UI like menu/dialog is showing).
 *          Requests a switch to FPV.
 * @param thisPtr Pointer to the UI overlay object
 * @param paramByte First parameter to the original function (byte)
 * @param paramChar Second parameter to the original function (char)
 */
static void __fastcall HideOverlaysDetour(void *thisPtr, uint8_t paramByte, char paramChar)
{
    Logger &logger = Logger::getInstance();

    try
    {
        // This function is called when the main game UI (HUD, crosshair) is hidden,
        // typically because a menu, inventory, map, dialogue, or cutscene is about to appear.
        logger.log(LOG_DEBUG, "UIOverlayHook: HideOverlaysDetour called - Main HUD hiding, UI element incoming.");

        // Important: Call the original function FIRST.
        // This ensures the game correctly sets its internal states related to overlays hiding.
        if (fpHideOverlaysOriginal)
        {
            fpHideOverlaysOriginal(thisPtr, paramByte, paramChar);
        }
        else
        {
            logger.log(LOG_ERROR, "UIOverlayHook: fpHideOverlaysOriginal (trampoline) is NULL!");
            // If trampoline is null, we can't proceed with game logic.
            // This state should ideally be caught during hook initialization.
            return;
        }

        // After original call, game state is now "overlays hidden".
        // If we are not already marked as "overlay active" by our logic, this is the start of one.
        if (!g_isOverlayActive.load(std::memory_order_relaxed))
        {
            int currentViewState = getViewState();
            if (currentViewState == 1) // Currently in TPV
            {
                g_wasTpvBeforeOverlay.store(true, std::memory_order_relaxed);
                logger.log(LOG_DEBUG, "UIOverlayHook: Was in TPV. Storing state for restore.");
                // Request FPV. The actual switch is handled by MonitorThread.
                g_overlayFpvRequest.store(true, std::memory_order_relaxed);
                logger.log(LOG_DEBUG, "UIOverlayHook: Requested FPV due to overlay activation.");
            }
            else // Already in FPV or unknown state
            {
                g_wasTpvBeforeOverlay.store(false, std::memory_order_relaxed);
                logger.log(LOG_DEBUG, "UIOverlayHook: Was in FPV or unknown state. No FPV request needed if already FPV.");
            }

            g_isOverlayActive.store(true, std::memory_order_relaxed); // Mark our overlay state as active
            logger.log(LOG_INFO, "UIOverlayHook: Overlay screen ACTIVATED.");
        }
        else
        {
            logger.log(LOG_DEBUG, "UIOverlayHook: HideOverlays called, but g_isOverlayActive already true (e.g. nested menus).");
            // Still might need to request FPV if the game didn't already switch for some reason,
            // or if a new type of overlay appears that also needs FPV.
            if (getViewState() == 1)
            { // If somehow still in TPV despite overlay already active
                g_overlayFpvRequest.store(true, std::memory_order_relaxed);
            }
        }

        resetScrollAccumulator(true); // Always reset scroll when overlays are involved
    }
    catch (const std::exception &e)
    {
        logger.log(LOG_ERROR, "UIOverlayHook: Exception in HideOverlaysDetour: " + std::string(e.what()));
        // Try to call original even on exception in our logic, if not already called.
        // However, in this new structure, original is called first.
    }
    catch (...)
    {
        logger.log(LOG_ERROR, "UIOverlayHook: Unknown exception in HideOverlaysDetour.");
    }
}

/**
 * @brief Detour function for the ShowOverlays function
 * @details Intercepts when UI overlay is about to be shown again (main HUD returning).
 *          Requests a switch back to TPV if that was the previous state.
 * @param thisPtr Pointer to the UI overlay object
 * @param paramByte First parameter to the original function (byte)
 * @param paramChar Second parameter to the original function (char)
 */
static void __fastcall ShowOverlaysDetour(void *thisPtr, uint8_t paramByte, char paramChar)
{
    Logger &logger = Logger::getInstance();

    try
    {
        // This function is called when a menu/dialog/etc. is closing, and the main game UI (HUD)
        // is about to be shown again.
        logger.log(LOG_DEBUG, "UIOverlayHook: ShowOverlaysDetour called - Main HUD returning, UI element closing.");

        // Important: Call the original function FIRST.
        // Game needs to set its state to "overlays shown".
        if (fpShowOverlaysOriginal)
        {
            fpShowOverlaysOriginal(thisPtr, paramByte, paramChar);
        }
        else
        {
            logger.log(LOG_ERROR, "UIOverlayHook: fpShowOverlaysOriginal (trampoline) is NULL!");
            return; // Can't proceed with game logic if trampoline is null.
        }

        // After original call, game state is "overlays shown".
        // If our logic thinks an overlay was active, this is the end of it.
        if (g_isOverlayActive.load(std::memory_order_relaxed))
        {
            g_isOverlayActive.store(false, std::memory_order_relaxed); // Mark our overlay state as inactive
            logger.log(LOG_INFO, "UIOverlayHook: Overlay screen DEACTIVATED.");

            if (g_wasTpvBeforeOverlay.load(std::memory_order_relaxed))
            {
                g_overlayTpvRestoreRequest.store(true, std::memory_order_relaxed);
                logger.log(LOG_DEBUG, "UIOverlayHook: Requested TPV restoration.");
            }
            else
            {
                logger.log(LOG_DEBUG, "UIOverlayHook: No TPV restoration needed (was FPV or never TPV).");
            }
            g_wasTpvBeforeOverlay.store(false, std::memory_order_relaxed); // Reset for next overlay cycle
        }
        else
        {
            logger.log(LOG_DEBUG, "UIOverlayHook: ShowOverlays called, but g_isOverlayActive was already false. State sync issue or multiple calls?");
        }

        resetScrollAccumulator(true); // Reset scroll state when returning to game view
    }
    catch (const std::exception &e)
    {
        logger.log(LOG_ERROR, "UIOverlayHook: Exception in ShowOverlaysDetour: " + std::string(e.what()));
    }
    catch (...)
    {
        logger.log(LOG_ERROR, "UIOverlayHook: Unknown exception in ShowOverlaysDetour.");
    }
}

bool handleHoldToScrollKeyState(bool holdKeyPressed)
{
    Logger &logger = Logger::getInstance();

    // This function is called by MonitorThread when a hold-to-scroll key's state changes.
    // It NOPs/Restores the game's scroll accumulator write instruction.

    // Do nothing if the accumulator write address was not found during init.
    if (g_accumulatorWriteAddress == nullptr)
    {
        // logger.log(LOG_TRACE, "UIOverlayHook: handleHoldToScrollKeyState - Accumulator address not available.");
        return false;
    }
    // Also, do nothing if an overlay is currently active (scroll should be NOPped by overlay logic anyway)
    // or if menu is open (where TPVInputHook zeros out scroll values before they reach game).
    // This function primarily manages NOP state when no overlay/menu is active.
    if (g_isOverlayActive.load(std::memory_order_relaxed) || isGameMenuOpen())
    {
        // If overlay becomes active while hold key is pressed, we want scroll to be NOPped by default.
        // If currently NOPped, no change. If not NOPped (hold key was pressed), NOP it.
        if (!g_accumulatorWriteNOPped.load(std::memory_order_relaxed))
        {
            if (WriteBytes(g_accumulatorWriteAddress, nopSequence, Constants::ACCUMULATOR_WRITE_INSTR_LENGTH, logger))
            {
                g_accumulatorWriteNOPped.store(true, std::memory_order_relaxed);
                logger.log(LOG_DEBUG, "UIOverlayHook: NOPped accumulator (overlay/menu became active while scroll allowed).");
                resetScrollAccumulator(true);
            }
            else
            {
                logger.log(LOG_ERROR, "UIOverlayHook: FAILED to NOP accumulator (overlay/menu became active).");
            }
        }
        return false; // State handled by overlay/menu logic primarily
    }

    bool stateChanged = false;
    if (holdKeyPressed)
    {
        // Hold key IS pressed. We want to ALLOW scrolling.
        // If accumulator is currently NOPped, restore original bytes.
        if (g_accumulatorWriteNOPped.load(std::memory_order_relaxed))
        {
            if (g_originalAccumulatorWriteBytes[0] != 0x00)
            { // Basic check to see if original bytes are likely valid
                if (WriteBytes(g_accumulatorWriteAddress, g_originalAccumulatorWriteBytes, Constants::ACCUMULATOR_WRITE_INSTR_LENGTH, logger))
                {
                    g_accumulatorWriteNOPped.store(false, std::memory_order_relaxed);
                    logger.log(LOG_DEBUG, "UIOverlayHook: Restored accumulator write (Hold-Key PRESSED).");
                    stateChanged = true;
                }
                else
                {
                    logger.log(LOG_ERROR, "UIOverlayHook: FAILED to restore accumulator write (Hold-Key PRESSED).");
                }
            }
            else
            {
                logger.log(LOG_WARNING, "UIOverlayHook: Original accumulator bytes seem invalid, not restoring (Hold-Key PRESSED).");
            }
        }
    }
    else
    {
        // Hold key IS NOT pressed. We want to PREVENT scrolling by NOPping.
        // If accumulator is not currently NOPped, NOP it.
        if (!g_accumulatorWriteNOPped.load(std::memory_order_relaxed))
        {
            if (WriteBytes(g_accumulatorWriteAddress, nopSequence, Constants::ACCUMULATOR_WRITE_INSTR_LENGTH, logger))
            {
                g_accumulatorWriteNOPped.store(true, std::memory_order_relaxed);
                logger.log(LOG_DEBUG, "UIOverlayHook: NOPped accumulator write (Hold-Key RELEASED or not configured for hold).");
                resetScrollAccumulator(true); // Reset any pending scroll when NOPping
                stateChanged = true;
            }
            else
            {
                logger.log(LOG_ERROR, "UIOverlayHook: FAILED to NOP accumulator write (Hold-Key RELEASED).");
            }
        }
    }
    return stateChanged;
}

bool initializeUiOverlayHooks(uintptr_t module_base, size_t module_size)
{
    Logger &logger = Logger::getInstance();
    HookManager &hookManager = HookManager::getInstance();
    bool all_hooks_successful = true;

    try
    {
        logger.log(LOG_INFO, "UIOverlayHook: Initializing UI overlay direct hooks...");

        // Hook HideOverlays
        // No AOB_OFFSET needed if pattern points to function start.
        g_hideOverlaysHookId = hookManager.create_inline_hook_aob(
            "HideOverlays",
            module_base,
            module_size,
            Constants::UI_OVERLAY_HIDE_AOB_PATTERN,
            0, // AOB_OFFSET, assume pattern starts at function
            reinterpret_cast<void *>(HideOverlaysDetour),
            reinterpret_cast<void **>(&fpHideOverlaysOriginal));

        if (g_hideOverlaysHookId.empty() || fpHideOverlaysOriginal == nullptr)
        {
            logger.log(LOG_ERROR, "UIOverlayHook: Failed to create HideOverlays hook. Overlay detection will be unreliable.");
            all_hooks_successful = false;
            // This is critical, so we might return false or throw.
            // For now, let's mark failure and continue trying ShowOverlays for completeness.
        }
        else
        {
            logger.log(LOG_INFO, "UIOverlayHook: HideOverlays hook installed (ID: " + g_hideOverlaysHookId + ").");
        }

        // Hook ShowOverlays
        g_showOverlaysHookId = hookManager.create_inline_hook_aob(
            "ShowOverlays",
            module_base,
            module_size,
            Constants::UI_OVERLAY_SHOW_AOB_PATTERN,
            0, // AOB_OFFSET
            reinterpret_cast<void *>(ShowOverlaysDetour),
            reinterpret_cast<void **>(&fpShowOverlaysOriginal));

        if (g_showOverlaysHookId.empty() || fpShowOverlaysOriginal == nullptr)
        {
            logger.log(LOG_ERROR, "UIOverlayHook: Failed to create ShowOverlays hook. Overlay detection will be unreliable.");
            all_hooks_successful = false;
        }
        else
        {
            logger.log(LOG_INFO, "UIOverlayHook: ShowOverlays hook installed (ID: " + g_showOverlaysHookId + ").");
        }

        if (!all_hooks_successful)
        {
            // If any hook failed, cleanup what might have been created and report failure
            cleanupUiOverlayHooks(); // This will attempt to remove any partially created hooks
            throw std::runtime_error("One or more UI overlay hooks failed to initialize.");
        }

        // Initial state for hold-to-scroll accumulator NOPping if applicable
        // This must be done AFTER g_accumulatorWriteAddress and g_originalAccumulatorWriteBytes are populated
        // by initializeEventHooks().
        if (g_accumulatorWriteAddress != nullptr)
        { // Check if EventHooks found it
            if (!g_config.hold_scroll_keys.empty())
            {
                // If hold-to-scroll is configured, ensure accumulator is NOPped initially
                // as hold key is not pressed at startup.
                handleHoldToScrollKeyState(false); // Force NOP if not pressed
            }
            else
            {
                // If hold-to-scroll is NOT configured, the NOP state depends on g_isOverlayActive.
                // Generally, scroll should be allowed if no overlay, NOPped if overlay.
                // The Detour functions handle this for overlay active state.
                // Here, ensure it's NOT NOPped if no overlay and no hold key configured.
                if (g_accumulatorWriteNOPped.load(std::memory_order_relaxed))
                {
                    if (g_originalAccumulatorWriteBytes[0] != 0x00)
                    {
                        if (WriteBytes(g_accumulatorWriteAddress, g_originalAccumulatorWriteBytes, Constants::ACCUMULATOR_WRITE_INSTR_LENGTH, logger))
                        {
                            g_accumulatorWriteNOPped.store(false, std::memory_order_relaxed);
                            logger.log(LOG_DEBUG, "UIOverlayHook: Ensured accumulator is NOT NOPped (no hold-scroll, no overlay at init).");
                        }
                    }
                }
            }
        }

        logger.log(LOG_INFO, "UIOverlayHook: UI overlay direct hooks successfully installed.");
        return true;
    }
    catch (const std::exception &e)
    {
        logger.log(LOG_ERROR, "UIOverlayHook: Initialization failed: " + std::string(e.what()));
        cleanupUiOverlayHooks(); // Ensure cleanup on any exception
        return false;
    }
}

void cleanupUiOverlayHooks()
{
    Logger &logger = Logger::getInstance();
    HookManager &hookManager = HookManager::getInstance();

    if (!g_hideOverlaysHookId.empty())
    {
        if (hookManager.remove_hook(g_hideOverlaysHookId))
        {
            logger.log(LOG_INFO, "UIOverlayHook: Hook '" + g_hideOverlaysHookId + "' removed.");
        }
        fpHideOverlaysOriginal = nullptr;
        g_hideOverlaysHookId = "";
    }

    if (!g_showOverlaysHookId.empty())
    {
        if (hookManager.remove_hook(g_showOverlaysHookId))
        {
            logger.log(LOG_INFO, "UIOverlayHook: Hook '" + g_showOverlaysHookId + "' removed.");
        }
        fpShowOverlaysOriginal = nullptr;
        g_showOverlaysHookId = "";
    }

    // On cleanup, attempt to restore accumulator write if it was NOPped by this system
    // and original bytes are valid.
    if (g_accumulatorWriteAddress != nullptr && g_accumulatorWriteNOPped.load(std::memory_order_relaxed))
    {
        bool originalBytesValid = false;
        for (size_t i = 0; i < Constants::ACCUMULATOR_WRITE_INSTR_LENGTH; ++i)
        {
            if (g_originalAccumulatorWriteBytes[i] != 0x00)
            {
                originalBytesValid = true;
                break;
            }
        }
        if (originalBytesValid)
        {
            logger.log(LOG_INFO, "UIOverlayHook: Restoring accumulator write before exit due to overlay hook cleanup.");
            if (WriteBytes(g_accumulatorWriteAddress, g_originalAccumulatorWriteBytes, Constants::ACCUMULATOR_WRITE_INSTR_LENGTH, logger))
            {
                g_accumulatorWriteNOPped.store(false, std::memory_order_relaxed);
            }
            else
            {
                logger.log(LOG_ERROR, "UIOverlayHook: FAILED to restore accumulator write on cleanup.");
            }
        }
        else if (Constants::ACCUMULATOR_WRITE_INSTR_LENGTH > 0)
        {
            logger.log(LOG_WARNING, "UIOverlayHook: Original accumulator bytes look invalid. Skipping restore on cleanup.");
        }
    }
    // Globals g_isOverlayActive and g_wasTpvBeforeOverlay will reset on next overlay event or game restart.

    logger.log(LOG_DEBUG, "UIOverlayHook: Cleanup complete.");
}

bool areUiOverlayHooksActive()
{
    return (fpHideOverlaysOriginal != nullptr && !g_hideOverlaysHookId.empty() &&
            fpShowOverlaysOriginal != nullptr && !g_showOverlaysHookId.empty());
}
