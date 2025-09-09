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
#include "aob_scanner.h"
#include "game_interface.h"
#include "global_state.h"
#include "tpv_input_hook.h"
#include "MinHook.h"

#include <stdexcept>
#include <atomic>

// Function typedefs for UI menu open/close functions
typedef void(__fastcall *MenuOpenFunc)(void *thisPtr, char paramByte);
typedef void(__fastcall *MenuCloseFunc)(void *thisPtr);

// Hook state
static MenuOpenFunc fpMenuOpenOriginal = nullptr;
static MenuCloseFunc fpMenuCloseOriginal = nullptr;
static BYTE *g_menuOpenHookAddress = nullptr;
static BYTE *g_menuCloseHookAddress = nullptr;

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
        std::vector<BYTE> openPattern = parseAOB(Constants::UI_MENU_OPEN_AOB_PATTERN);
        if (openPattern.empty())
        {
            throw std::runtime_error("Failed to parse menu open AOB pattern");
        }

        std::vector<BYTE> closePattern = parseAOB(Constants::UI_MENU_CLOSE_AOB_PATTERN);
        if (closePattern.empty())
        {
            throw std::runtime_error("Failed to parse menu close AOB pattern");
        }

        // Find menu open function
        g_menuOpenHookAddress = FindPattern(reinterpret_cast<BYTE *>(module_base), module_size, openPattern);
        if (!g_menuOpenHookAddress)
        {
            throw std::runtime_error("Menu open function pattern not found");
        }

        // Find menu close function
        g_menuCloseHookAddress = FindPattern(reinterpret_cast<BYTE *>(module_base), module_size, closePattern);
        if (!g_menuCloseHookAddress)
        {
            throw std::runtime_error("Menu close function pattern not found");
        }

        logger.log(LOG_INFO, "UIMenuHook: Found menu open function at " +
                                 format_address(reinterpret_cast<uintptr_t>(g_menuOpenHookAddress)));
        logger.log(LOG_INFO, "UIMenuHook: Found menu close function at " +
                                 format_address(reinterpret_cast<uintptr_t>(g_menuCloseHookAddress)));

        // The AOB patterns locate specific instructions within the functions
        // We need to adjust to the actual function entry points
        // Menu open function starts at WHGame.DLL+AE14BC
        g_menuOpenHookAddress = g_menuOpenHookAddress - 0x36; // Adjust to function start

        // Menu close function starts at WHGame.DLL+C9763C
        g_menuCloseHookAddress = g_menuCloseHookAddress - 0x18E; // Adjust to function start

        logger.log(LOG_INFO, "UIMenuHook: Adjusted menu open function to " +
                                 format_address(reinterpret_cast<uintptr_t>(g_menuOpenHookAddress)));
        logger.log(LOG_INFO, "UIMenuHook: Adjusted menu close function to " +
                                 format_address(reinterpret_cast<uintptr_t>(g_menuCloseHookAddress)));

        // Create menu open hook
        MH_STATUS openStatus = MH_CreateHook(
            g_menuOpenHookAddress,
            reinterpret_cast<LPVOID>(MenuOpenDetour),
            reinterpret_cast<LPVOID *>(&fpMenuOpenOriginal));

        if (openStatus != MH_OK)
        {
            throw std::runtime_error("MH_CreateHook for menu open failed: " +
                                     std::string(MH_StatusToString(openStatus)));
        }

        if (!fpMenuOpenOriginal)
        {
            MH_RemoveHook(g_menuOpenHookAddress);
            throw std::runtime_error("MH_CreateHook for menu open returned NULL trampoline");
        }

        // Create menu close hook
        MH_STATUS closeStatus = MH_CreateHook(
            g_menuCloseHookAddress,
            reinterpret_cast<LPVOID>(MenuCloseDetour),
            reinterpret_cast<LPVOID *>(&fpMenuCloseOriginal));

        if (closeStatus != MH_OK)
        {
            MH_RemoveHook(g_menuOpenHookAddress);
            throw std::runtime_error("MH_CreateHook for menu close failed: " +
                                     std::string(MH_StatusToString(closeStatus)));
        }

        if (!fpMenuCloseOriginal)
        {
            MH_RemoveHook(g_menuOpenHookAddress);
            MH_RemoveHook(g_menuCloseHookAddress);
            throw std::runtime_error("MH_CreateHook for menu close returned NULL trampoline");
        }

        // Enable both hooks
        openStatus = MH_EnableHook(g_menuOpenHookAddress);
        if (openStatus != MH_OK)
        {
            MH_RemoveHook(g_menuOpenHookAddress);
            MH_RemoveHook(g_menuCloseHookAddress);
            throw std::runtime_error("MH_EnableHook for menu open failed: " +
                                     std::string(MH_StatusToString(openStatus)));
        }

        closeStatus = MH_EnableHook(g_menuCloseHookAddress);
        if (closeStatus != MH_OK)
        {
            MH_DisableHook(g_menuOpenHookAddress);
            MH_RemoveHook(g_menuOpenHookAddress);
            MH_RemoveHook(g_menuCloseHookAddress);
            throw std::runtime_error("MH_EnableHook for menu close failed: " +
                                     std::string(MH_StatusToString(closeStatus)));
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

    // Disable and remove menu open hook
    if (g_menuOpenHookAddress && fpMenuOpenOriginal)
    {
        MH_DisableHook(g_menuOpenHookAddress);
        MH_RemoveHook(g_menuOpenHookAddress);
        g_menuOpenHookAddress = nullptr;
        fpMenuOpenOriginal = nullptr;
    }

    // Disable and remove menu close hook
    if (g_menuCloseHookAddress && fpMenuCloseOriginal)
    {
        MH_DisableHook(g_menuCloseHookAddress);
        MH_RemoveHook(g_menuCloseHookAddress);
        g_menuCloseHookAddress = nullptr;
        fpMenuCloseOriginal = nullptr;
    }

    // Reset menu state
    g_isMenuOpen.store(false);

    logger.log(LOG_DEBUG, "UIMenuHook: Cleanup complete");
}

bool areUiMenuHooksActive()
{
    return (g_menuOpenHookAddress != nullptr && fpMenuOpenOriginal != nullptr &&
            g_menuCloseHookAddress != nullptr && fpMenuCloseOriginal != nullptr);
}

bool isGameMenuOpen()
{
    return g_isMenuOpen.load();
}
