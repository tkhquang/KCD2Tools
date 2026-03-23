/**
 * @file hooks/ui_menu_hooks.cpp
 * @brief Implementation of in-game menu hooks for menu open/close detection using DetourModKit.
 *
 * Implements hooks that directly intercept the game's UI menu open and close
 * functions to detect when the player opens or closes the in-game menu.
 */

#include "ui_menu_hooks.h"
#include "constants.h"
#include "utils.h"
#include "game_interface.h"
#include "global_state.h"
#include "tpv_input_hook.h"

#include <DetourModKit.hpp>

#include <stdexcept>
#include <atomic>

using DetourModKit::LogLevel;
using DMKFormat::format_address;

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
    DMKLogger &logger = DMKLogger::get_instance();

    try
    {
        // Before calling original - menu is about to open
        logger.log(LogLevel::Info, "UIMenuHook: Game menu is opening");

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
            logger.log(LogLevel::Error, "UIMenuHook: Menu open original function pointer is NULL");
        }
    }
    catch (const std::exception &e)
    {
        logger.log(LogLevel::Error, "UIMenuHook: Exception in menu open detour: " + std::string(e.what()));

        // Call the original function even if we had an exception
        if (fpMenuOpenOriginal)
        {
            fpMenuOpenOriginal(thisPtr, paramByte);
        }
    }
    catch (...)
    {
        logger.log(LogLevel::Error, "UIMenuHook: Unknown exception in menu open detour");

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
    DMKLogger &logger = DMKLogger::get_instance();

    try
    {
        // Before calling original - menu is about to close
        logger.log(LogLevel::Info, "UIMenuHook: Game menu is closing");

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
            logger.log(LogLevel::Error, "UIMenuHook: Menu close original function pointer is NULL");
        }
    }
    catch (const std::exception &e)
    {
        logger.log(LogLevel::Error, "UIMenuHook: Exception in menu close detour: " + std::string(e.what()));

        // Call the original function even if we had an exception
        if (fpMenuCloseOriginal)
        {
            fpMenuCloseOriginal(thisPtr);
        }
    }
    catch (...)
    {
        logger.log(LogLevel::Error, "UIMenuHook: Unknown exception in menu close detour");

        // Call the original function even if we had an exception
        if (fpMenuCloseOriginal)
        {
            fpMenuCloseOriginal(thisPtr);
        }
    }
}

bool initializeUiMenuHooks(uintptr_t module_base, size_t module_size)
{
    DMKLogger &logger = DMKLogger::get_instance();
    logger.log(LogLevel::Info, "UIMenuHook: Initializing UI menu hooks...");

    try
    {
        // Parse AOB patterns
        auto openPattern = DMKScanner::parse_aob(Constants::UI_MENU_OPEN_AOB_PATTERN);
        if (!openPattern.has_value())
        {
            throw std::runtime_error("Failed to parse menu open AOB pattern");
        }

        auto closePattern = DMKScanner::parse_aob(Constants::UI_MENU_CLOSE_AOB_PATTERN);
        if (!closePattern.has_value())
        {
            throw std::runtime_error("Failed to parse menu close AOB pattern");
        }

        // Find menu open function
        const std::byte *menuOpenHookAddress = DMKScanner::find_pattern(reinterpret_cast<const std::byte *>(module_base), module_size, *openPattern);
        if (!menuOpenHookAddress)
        {
            throw std::runtime_error("Menu open function pattern not found");
        }

        // Find menu close function
        const std::byte *menuCloseHookAddress = DMKScanner::find_pattern(reinterpret_cast<const std::byte *>(module_base), module_size, *closePattern);
        if (!menuCloseHookAddress)
        {
            throw std::runtime_error("Menu close function pattern not found");
        }

        logger.log(LogLevel::Info, "UIMenuHook: Found menu open function at " +
                                 format_address(reinterpret_cast<uintptr_t>(menuOpenHookAddress)));
        logger.log(LogLevel::Info, "UIMenuHook: Found menu close function at " +
                                 format_address(reinterpret_cast<uintptr_t>(menuCloseHookAddress)));

        // The AOB patterns locate specific instructions within the functions
        // We need to adjust to the actual function entry points
        // Menu open function starts at WHGame.DLL+AE14BC
        uintptr_t menuOpenAddr = reinterpret_cast<uintptr_t>(menuOpenHookAddress) - 0x36; // Adjust to function start

        // Menu close function starts at WHGame.DLL+C9763C
        uintptr_t menuCloseAddr = reinterpret_cast<uintptr_t>(menuCloseHookAddress) - 0x18E; // Adjust to function start

        logger.log(LogLevel::Info, "UIMenuHook: Adjusted menu open function to " +
                                 format_address(menuOpenAddr));
        logger.log(LogLevel::Info, "UIMenuHook: Adjusted menu close function to " +
                                 format_address(menuCloseAddr));

        // Create hooks using DMKHookManager
        DMKHookManager &hook_manager = DMKHookManager::get_instance();

        // Create menu open hook
        auto openResult = hook_manager.create_inline_hook(
            "MenuOpen",
            menuOpenAddr,
            reinterpret_cast<void *>(MenuOpenDetour),
            reinterpret_cast<void **>(&fpMenuOpenOriginal));

        if (!openResult.has_value())
        {
            throw std::runtime_error("Failed to create menu open hook: " + std::string(DMK::Hook::error_to_string(openResult.error())));
        }
        g_menuOpenHookId = openResult.value();

        // Create menu close hook
        auto closeResult = hook_manager.create_inline_hook(
            "MenuClose",
            menuCloseAddr,
            reinterpret_cast<void *>(MenuCloseDetour),
            reinterpret_cast<void **>(&fpMenuCloseOriginal));

        if (!closeResult.has_value())
        {
            (void)hook_manager.remove_hook(g_menuOpenHookId);
            g_menuOpenHookId.clear();
            throw std::runtime_error("Failed to create menu close hook: " + std::string(DMK::Hook::error_to_string(closeResult.error())));
        }
        g_menuCloseHookId = closeResult.value();

        logger.log(LogLevel::Info, "UIMenuHook: UI menu hooks successfully installed");
        return true;
    }
    catch (const std::exception &e)
    {
        logger.log(LogLevel::Error, "UIMenuHook: Initialization failed: " + std::string(e.what()));
        cleanupUiMenuHooks();
        return false;
    }
}

void cleanupUiMenuHooks()
{
    DMKLogger &logger = DMKLogger::get_instance();
    DMKHookManager &hook_manager = DMKHookManager::get_instance();

    // Remove menu open hook
    if (!g_menuOpenHookId.empty())
    {
        (void)hook_manager.remove_hook(g_menuOpenHookId);
        g_menuOpenHookId.clear();
        fpMenuOpenOriginal = nullptr;
    }

    // Remove menu close hook
    if (!g_menuCloseHookId.empty())
    {
        (void)hook_manager.remove_hook(g_menuCloseHookId);
        g_menuCloseHookId.clear();
        fpMenuCloseOriginal = nullptr;
    }

    // Reset menu state
    g_isMenuOpen.store(false);

    logger.log(LogLevel::Debug, "UIMenuHook: Cleanup complete");
}

bool areUiMenuHooksActive()
{
    return (!g_menuOpenHookId.empty() && !g_menuCloseHookId.empty());
}

bool isGameMenuOpen()
{
    return g_isMenuOpen.load();
}
