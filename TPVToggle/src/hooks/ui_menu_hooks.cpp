/**
 * @file hooks/ui_menu_hooks.cpp
 * @brief Implementation of in-game menu hooks for menu open/close detection.
 *
 * Implements hooks that directly intercept the game's UI menu open and close
 * functions to detect when the player opens or closes the in-game menu.
 */

#include "ui_menu_hooks.h"
#include "logger.h"
#include "constants.h"
#include "utils.h"
// #include "aob_scanner.h" // No longer needed here
#include "game_interface.h" // For resetScrollAccumulator
#include "global_state.h"
#include "tpv_input_hook.h" // For resetCameraAngles (if desired on menu open/close)
#include "hook_manager.hpp" // Use HookManager
#include "ui_menu_hooks.h"  // For isGameMenuOpen()

#include <stdexcept>
#include <atomic>
#include <string> // For std::string

// Function typedefs for UI menu open/close functions
typedef void(__fastcall *MenuOpenFunc)(void *thisPtr, char paramByte); // Assuming original params are correct
typedef void(__fastcall *MenuCloseFunc)(void *thisPtr);

// Hook state
static MenuOpenFunc fpMenuOpenOriginal = nullptr;
static MenuCloseFunc fpMenuCloseOriginal = nullptr;
// static BYTE *g_menuOpenHookAddress = nullptr; // Managed by HookManager
// static BYTE *g_menuCloseHookAddress = nullptr;// Managed by HookManager
static std::string g_menuOpenHookId = "";
static std::string g_menuCloseHookId = "";

// Menu state tracking
static std::atomic<bool> g_isMenuOpen(false);

/**
 * @brief Detour function for menu open (expected to be vftable[1] or similar game specific call)
 * @details Intercepts when in-game menu is opened
 * @param thisPtr Pointer to the UI menu object
 * @param paramByte Parameter passed to the original function
 */
static void __fastcall MenuOpenDetour(void *thisPtr, char paramByte)
{
    Logger &logger = Logger::getInstance();

    try
    {
        logger.log(LOG_INFO, "UIMenuHook: Game menu is OPENING.");

        // Actions before calling original (menu is about to become visible)
        resetScrollAccumulator(true); // Reset scroll before menu processes it
        g_isMenuOpen.store(true, std::memory_order_release);

        // Optional: Reset camera pitch tracking if menu opening should behave like a view reset
        // resetCameraAngles(); // Consider if this is desired behavior

        // Call the original function
        if (fpMenuOpenOriginal)
        {
            fpMenuOpenOriginal(thisPtr, paramByte);
        }
        else
        {
            logger.log(LOG_ERROR, "UIMenuHook: fpMenuOpenOriginal (trampoline) is NULL!");
        }
    }
    catch (const std::exception &e)
    {
        logger.log(LOG_ERROR, "UIMenuHook: Exception in MenuOpenDetour: " + std::string(e.what()));
        // Ensure original is called if an error occurs in our pre-logic, if it's safe
        if (fpMenuOpenOriginal)
        {
            fpMenuOpenOriginal(thisPtr, paramByte);
        }
    }
    catch (...)
    {
        logger.log(LOG_ERROR, "UIMenuHook: Unknown exception in MenuOpenDetour.");
        if (fpMenuOpenOriginal)
        {
            fpMenuOpenOriginal(thisPtr, paramByte);
        }
    }
}

/**
 * @brief Detour function for menu close (expected to be vftable[2] or similar)
 * @details Intercepts when in-game menu is closed
 * @param thisPtr Pointer to the UI menu object
 */
static void __fastcall MenuCloseDetour(void *thisPtr)
{
    Logger &logger = Logger::getInstance();

    try
    {
        logger.log(LOG_INFO, "UIMenuHook: Game menu is CLOSING.");

        // Actions before calling original (menu is about to hide)
        g_isMenuOpen.store(false, std::memory_order_release);
        resetScrollAccumulator(true); // Reset scroll as menu closes

        // Call the original function
        if (fpMenuCloseOriginal)
        {
            fpMenuCloseOriginal(thisPtr);
        }
        else
        {
            logger.log(LOG_ERROR, "UIMenuHook: fpMenuCloseOriginal (trampoline) is NULL!");
        }
    }
    catch (const std::exception &e)
    {
        logger.log(LOG_ERROR, "UIMenuHook: Exception in MenuCloseDetour: " + std::string(e.what()));
        if (fpMenuCloseOriginal)
        {
            fpMenuCloseOriginal(thisPtr);
        }
    }
    catch (...)
    {
        logger.log(LOG_ERROR, "UIMenuHook: Unknown exception in MenuCloseDetour.");
        if (fpMenuCloseOriginal)
        {
            fpMenuCloseOriginal(thisPtr);
        }
    }
}

bool initializeUiMenuHooks(uintptr_t module_base, size_t module_size)
{
    Logger &logger = Logger::getInstance();
    logger.log(LOG_INFO, "UIMenuHook: Initializing UI menu open/close hooks...");

    bool success = true;
    HookManager &hookManager = HookManager::getInstance();

    try
    {
        // Hook Menu Open
        // The AOB_OFFSET needs to be determined carefully if the AOB pattern
        // doesn't point directly to the function start.
        // Previous code had: g_menuOpenHookAddress = g_menuOpenHookAddress - 0x47;
        ptrdiff_t open_aob_offset = -0x47; // MAKE SURE THIS IS STILL CORRECT for your AOB pattern

        g_menuOpenHookId = hookManager.create_inline_hook_aob(
            "UIMenuOpen",
            module_base,
            module_size,
            Constants::UI_MENU_OPEN_AOB_PATTERN,
            open_aob_offset,
            reinterpret_cast<void *>(MenuOpenDetour),
            reinterpret_cast<void **>(&fpMenuOpenOriginal));

        if (g_menuOpenHookId.empty() || fpMenuOpenOriginal == nullptr)
        {
            logger.log(LOG_WARNING, "UIMenuHook: Failed to create UI Menu Open hook. Menu detection might be impaired.");
            success = false; // Mark as partial failure, but might not be critical
        }
        else
        {
            logger.log(LOG_INFO, "UIMenuHook: UI Menu Open hook installed (ID: " + g_menuOpenHookId + ").");
        }

        // Hook Menu Close
        // Previous code had: g_menuCloseHookAddress = g_menuCloseHookAddress - 0x207;
        ptrdiff_t close_aob_offset = -0x207; // MAKE SURE THIS IS STILL CORRECT

        g_menuCloseHookId = hookManager.create_inline_hook_aob(
            "UIMenuClose",
            module_base,
            module_size,
            Constants::UI_MENU_CLOSE_AOB_PATTERN,
            close_aob_offset,
            reinterpret_cast<void *>(MenuCloseDetour),
            reinterpret_cast<void **>(&fpMenuCloseOriginal));

        if (g_menuCloseHookId.empty() || fpMenuCloseOriginal == nullptr)
        {
            logger.log(LOG_WARNING, "UIMenuHook: Failed to create UI Menu Close hook. Menu detection might be impaired.");
            success = false; // Mark as partial failure
        }
        else
        {
            logger.log(LOG_INFO, "UIMenuHook: UI Menu Close hook installed (ID: " + g_menuCloseHookId + ").");
        }

        if (!success)
        {
            logger.log(LOG_WARNING, "UIMenuHook: One or more UI menu hooks failed to initialize. Menu open/close detection may be unreliable.");
            // Depending on criticality, you might choose to call cleanupUiMenuHooks() here.
            // For now, we'll let other hooks proceed if some UI menu hooks fail.
        }
        else
        {
            logger.log(LOG_INFO, "UIMenuHook: All UI menu hooks initialized successfully.");
        }
        return success; // Return true even if partially successful for now, to not halt mod.
    }
    catch (const std::exception &e) // Catch exceptions from AOB scanning or HookManager itself
    {
        logger.log(LOG_ERROR, "UIMenuHook: Critical exception during initialization: " + std::string(e.what()));
        cleanupUiMenuHooks(); // Attempt to clean up any hooks that might have been created
        return false;         // Indicate critical failure
    }
}

void cleanupUiMenuHooks()
{
    Logger &logger = Logger::getInstance();
    HookManager &hookManager = HookManager::getInstance();

    if (!g_menuOpenHookId.empty())
    {
        if (hookManager.remove_hook(g_menuOpenHookId))
        {
            logger.log(LOG_INFO, "UIMenuHook: Hook '" + g_menuOpenHookId + "' removed.");
        }
        fpMenuOpenOriginal = nullptr;
        g_menuOpenHookId = "";
    }

    if (!g_menuCloseHookId.empty())
    {
        if (hookManager.remove_hook(g_menuCloseHookId))
        {
            logger.log(LOG_INFO, "UIMenuHook: Hook '" + g_menuCloseHookId + "' removed.");
        }
        fpMenuCloseOriginal = nullptr;
        g_menuCloseHookId = "";
    }

    g_isMenuOpen.store(false, std::memory_order_relaxed); // Ensure state is reset
    logger.log(LOG_DEBUG, "UIMenuHook: Cleanup complete.");
}

bool areUiMenuHooksActive()
{
    // Considered active if both hooks were successfully created
    return (fpMenuOpenOriginal != nullptr && !g_menuOpenHookId.empty() &&
            fpMenuCloseOriginal != nullptr && !g_menuCloseHookId.empty());
}

bool isGameMenuOpen()
{
    return g_isMenuOpen.load(std::memory_order_acquire);
}
