/**
 * @file hooks/ui_menu_hooks.cpp
 * @brief Implementation of in-game menu hooks for menu open/close detection using DetourModKit.
 *
 * Implements hooks that directly intercept the game's UI menu open and close
 * functions to detect when the player opens or closes the in-game menu.
 */

#include "ui_menu_hooks.h"
#include "logger.h"
#include "constants.h"
#include "utils.h"
#include "game_interface.h"
#include "global_state.h"
#include "tpv_input_hook.h"

#include <DetourModKit.hpp>

#include <stdexcept>
#include <atomic>

using DMKString::format_address;

// Function typedefs for UI menu open/close functions
typedef void(__fastcall *MenuOpenFunc)(void *thisPtr, char paramByte);
typedef void(__fastcall *MenuCloseFunc)(void *thisPtr);

// Hook state
static MenuOpenFunc fpMenuOpenOriginal = nullptr;
static MenuCloseFunc fpMenuCloseOriginal = nullptr;
static std::string g_menuOpenHookId;
static std::string g_menuCloseHookId;

// Menu state tracking
static std::atomic<bool> g_isMenuOpen(false);

/**
 * @brief Detour function for menu open (vftable[1])
 * @details Intercepts when in-game menu is opened
 * @param thisPtr Pointer to the UI menu object
 * @param paramByte Parameter passed to the original function
 */
static void __fastcall MenuOpenDetour(void *thisPtr, char paramByte)
{
    Logger &logger = Logger::getInstance();

    try
    {
        // Before calling original - menu is about to open
        logger.log(LOG_INFO, "UIMenuHook: Game menu is opening");

        resetScrollAccumulator();
        // Set menu state to open
        g_isMenuOpen.store(true);

        // Call the original function
        if (fpMenuOpenOriginal)
        {
            fpMenuOpenOriginal(thisPtr, paramByte);
        }
        else
        {
            logger.log(LOG_ERROR, "UIMenuHook: Menu open original function pointer is NULL");
        }
    }
    catch (const std::exception &e)
    {
        logger.log(LOG_ERROR, "UIMenuHook: Exception in menu open detour: " + std::string(e.what()));

        // Call the original function even if we had an exception
        if (fpMenuOpenOriginal)
        {
            fpMenuOpenOriginal(thisPtr, paramByte);
        }
    }
    catch (...)
    {
        logger.log(LOG_ERROR, "UIMenuHook: Unknown exception in menu open detour");

        // Call the original function even if we had an exception
        if (fpMenuOpenOriginal)
        {
            fpMenuOpenOriginal(thisPtr, paramByte);
        }
    }
}

/**
 * @brief Detour function for menu close (vftable[2])
 * @details Intercepts when in-game menu is closed
 * @param thisPtr Pointer to the UI menu object
 */
static void __fastcall MenuCloseDetour(void *thisPtr)
{
    Logger &logger = Logger::getInstance();

    try
    {
        // Before calling original - menu is about to close
        logger.log(LOG_INFO, "UIMenuHook: Game menu is closing");

        resetScrollAccumulator(true);
        // Set menu state to closed
        g_isMenuOpen.store(false);

        // Call the original function
        if (fpMenuCloseOriginal)
        {
            fpMenuCloseOriginal(thisPtr);
        }
        else
        {
            logger.log(LOG_ERROR, "UIMenuHook: Menu close original function pointer is NULL");
        }
    }
    catch (const std::exception &e)
    {
        logger.log(LOG_ERROR, "UIMenuHook: Exception in menu close detour: " + std::string(e.what()));

        // Call the original function even if we had an exception
        if (fpMenuCloseOriginal)
        {
            fpMenuCloseOriginal(thisPtr);
        }
    }
    catch (...)
    {
        logger.log(LOG_ERROR, "UIMenuHook: Unknown exception in menu close detour");

        // Call the original function even if we had an exception
        if (fpMenuCloseOriginal)
        {
            fpMenuCloseOriginal(thisPtr);
        }
    }
}

bool initializeUiMenuHooks(uintptr_t module_base, size_t module_size)
{
    Logger &logger = Logger::getInstance();
    logger.log(LOG_INFO, "UIMenuHook: Initializing UI menu hooks...");

    try
    {
        // Parse AOB patterns
        std::vector<std::byte> openPattern = DMKScanner::parseAOB(Constants::UI_MENU_OPEN_AOB_PATTERN);
        if (openPattern.empty())
        {
            throw std::runtime_error("Failed to parse menu open AOB pattern");
        }

        std::vector<std::byte> closePattern = DMKScanner::parseAOB(Constants::UI_MENU_CLOSE_AOB_PATTERN);
        if (closePattern.empty())
        {
            throw std::runtime_error("Failed to parse menu close AOB pattern");
        }

        // Find menu open function
        std::byte *menuOpenHookAddress = DMKScanner::FindPattern(reinterpret_cast<std::byte *>(module_base), module_size, openPattern);
        if (!menuOpenHookAddress)
        {
            throw std::runtime_error("Menu open function pattern not found");
        }

        // Find menu close function
        std::byte *menuCloseHookAddress = DMKScanner::FindPattern(reinterpret_cast<std::byte *>(module_base), module_size, closePattern);
        if (!menuCloseHookAddress)
        {
            throw std::runtime_error("Menu close function pattern not found");
        }

        logger.log(LOG_INFO, "UIMenuHook: Found menu open function at " +
                                 format_address(reinterpret_cast<uintptr_t>(menuOpenHookAddress)));
        logger.log(LOG_INFO, "UIMenuHook: Found menu close function at " +
                                 format_address(reinterpret_cast<uintptr_t>(menuCloseHookAddress)));

        // The AOB patterns locate specific instructions within the functions
        // We need to adjust to the actual function entry points
        // Menu open function starts at WHGame.DLL+AE14BC
        menuOpenHookAddress = menuOpenHookAddress - 0x36; // Adjust to function start

        // Menu close function starts at WHGame.DLL+C9763C
        menuCloseHookAddress = menuCloseHookAddress - 0x18E; // Adjust to function start

        logger.log(LOG_INFO, "UIMenuHook: Adjusted menu open function to " +
                                 format_address(reinterpret_cast<uintptr_t>(menuOpenHookAddress)));
        logger.log(LOG_INFO, "UIMenuHook: Adjusted menu close function to " +
                                 format_address(reinterpret_cast<uintptr_t>(menuCloseHookAddress)));

        // Create hooks using DMKHookManager
        DMKHookManager &hook_manager = DMKHookManager::getInstance();

        // Create menu open hook
        g_menuOpenHookId = hook_manager.create_inline_hook(
            "MenuOpen",
            reinterpret_cast<uintptr_t>(menuOpenHookAddress),
            reinterpret_cast<void *>(MenuOpenDetour),
            reinterpret_cast<void **>(&fpMenuOpenOriginal));

        if (g_menuOpenHookId.empty())
        {
            throw std::runtime_error("Failed to create menu open hook");
        }

        // Create menu close hook
        g_menuCloseHookId = hook_manager.create_inline_hook(
            "MenuClose",
            reinterpret_cast<uintptr_t>(menuCloseHookAddress),
            reinterpret_cast<void *>(MenuCloseDetour),
            reinterpret_cast<void **>(&fpMenuCloseOriginal));

        if (g_menuCloseHookId.empty())
        {
            hook_manager.remove_hook(g_menuOpenHookId);
            g_menuOpenHookId.clear();
            throw std::runtime_error("Failed to create menu close hook");
        }

        logger.log(LOG_INFO, "UIMenuHook: UI menu hooks successfully installed");
        return true;
    }
    catch (const std::exception &e)
    {
        logger.log(LOG_ERROR, "UIMenuHook: Initialization failed: " + std::string(e.what()));
        cleanupUiMenuHooks();
        return false;
    }
}

void cleanupUiMenuHooks()
{
    Logger &logger = Logger::getInstance();
    DMKHookManager &hook_manager = DMKHookManager::getInstance();

    // Remove menu open hook
    if (!g_menuOpenHookId.empty())
    {
        hook_manager.remove_hook(g_menuOpenHookId);
        g_menuOpenHookId.clear();
        fpMenuOpenOriginal = nullptr;
    }

    // Remove menu close hook
    if (!g_menuCloseHookId.empty())
    {
        hook_manager.remove_hook(g_menuCloseHookId);
        g_menuCloseHookId.clear();
        fpMenuCloseOriginal = nullptr;
    }

    // Reset menu state
    g_isMenuOpen.store(false);

    logger.log(LOG_DEBUG, "UIMenuHook: Cleanup complete");
}

bool areUiMenuHooksActive()
{
    return (!g_menuOpenHookId.empty() && !g_menuCloseHookId.empty());
}

bool isGameMenuOpen()
{
    return g_isMenuOpen.load();
}
