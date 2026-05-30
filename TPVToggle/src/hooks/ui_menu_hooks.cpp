/**
 * @file hooks/ui_menu_hooks.cpp
 * @brief Implementation of in-game menu hooks for menu open/close detection using DetourModKit.
 *
 * Implements hooks that directly intercept the game's UI menu open and close
 * functions to detect when the player opens or closes the in-game menu.
 */

#include "ui_menu_hooks.hpp"
#include "constants.hpp"
#include "utils.hpp"
#include "game_interface.hpp"
#include "global_state.hpp"
#include "tpv_input_hook.hpp"

#include <DetourModKit.hpp>

#include <stdexcept>
#include <atomic>

namespace TPVToggle
{

// Function pointer types for the UI menu open/close functions.
using MenuOpenFunc = void(__fastcall *)(void *thisPtr, char paramByte);
using MenuCloseFunc = void(__fastcall *)(void *thisPtr);

// Hook state
static MenuOpenFunc fpMenuOpenOriginal = nullptr;
static MenuCloseFunc fpMenuCloseOriginal = nullptr;

// Menu state tracking
static std::atomic<bool> g_isMenuOpen(false);

/**
 * @brief Detour for the menu-open function (inline hook at its entry point).
 * @details Resets the scroll accumulator and records the menu-open state, then
 *          calls the original. Only the mod-side work is guarded by try/catch;
 *          the original is invoked exactly once outside the guard so an exception
 *          in the mod logic can never call the engine function twice.
 * @param thisPtr Pointer to the UI menu object
 * @param paramByte Parameter passed to the original function
 */
static void __fastcall MenuOpenDetour(void *thisPtr, char paramByte)
{
    DMK::Logger &logger = DMK::Logger::get_instance();

    try
    {
        logger.info("UIMenuHook: Game menu is opening");

        resetScrollAccumulator();
        g_isMenuOpen.store(true);
    }
    catch (const std::exception &e)
    {
        logger.error("UIMenuHook: Exception in menu open detour: {}", e.what());
    }
    catch (...)
    {
        logger.error("UIMenuHook: Unknown exception in menu open detour");
    }

    if (fpMenuOpenOriginal)
    {
        fpMenuOpenOriginal(thisPtr, paramByte);
    }
    else
    {
        logger.error("UIMenuHook: Menu open original function pointer is NULL");
    }
}

/**
 * @brief Detour for the menu-close function (inline hook at its entry point).
 * @details Resets the scroll accumulator and clears the menu-open state, then
 *          calls the original. Only the mod-side work is guarded by try/catch;
 *          the original is invoked exactly once outside the guard so an exception
 *          in the mod logic can never call the engine function twice.
 * @param thisPtr Pointer to the UI menu object
 */
static void __fastcall MenuCloseDetour(void *thisPtr)
{
    DMK::Logger &logger = DMK::Logger::get_instance();

    try
    {
        logger.info("UIMenuHook: Game menu is closing");

        resetScrollAccumulator(true);
        g_isMenuOpen.store(false);
    }
    catch (const std::exception &e)
    {
        logger.error("UIMenuHook: Exception in menu close detour: {}", e.what());
    }
    catch (...)
    {
        logger.error("UIMenuHook: Unknown exception in menu close detour");
    }

    if (fpMenuCloseOriginal)
    {
        fpMenuCloseOriginal(thisPtr);
    }
    else
    {
        logger.error("UIMenuHook: Menu close original function pointer is NULL");
    }
}

bool initializeUiMenuHooks(uintptr_t module_base, size_t module_size)
{
    DMK::Logger &logger = DMK::Logger::get_instance();

    try
    {
        auto openPattern = DMK::Scanner::parse_aob(Constants::UI_MENU_OPEN_AOB_PATTERN);
        if (!openPattern.has_value())
        {
            throw std::runtime_error("Failed to parse menu open AOB pattern");
        }

        auto closePattern = DMK::Scanner::parse_aob(Constants::UI_MENU_CLOSE_AOB_PATTERN);
        if (!closePattern.has_value())
        {
            throw std::runtime_error("Failed to parse menu close AOB pattern");
        }

        const std::byte *menuOpenHookAddress = DMK::Scanner::find_pattern(reinterpret_cast<const std::byte *>(module_base), module_size, *openPattern);
        if (!menuOpenHookAddress)
        {
            throw std::runtime_error("Menu open function pattern not found");
        }

        const std::byte *menuCloseHookAddress = DMK::Scanner::find_pattern(reinterpret_cast<const std::byte *>(module_base), module_size, *closePattern);
        if (!menuCloseHookAddress)
        {
            throw std::runtime_error("Menu close function pattern not found");
        }

        // The AOB matches an instruction inside each function body; step back a
        // fixed delta to the real entry point that SafetyHook must patch.
        //   open:  match is +0x36 into the function (WHGame.DLL+AE14BC entry)
        //   close: match is +0x18E into the function (WHGame.DLL+C9763C entry)
        uintptr_t menuOpenAddr = reinterpret_cast<uintptr_t>(menuOpenHookAddress) - 0x36;
        uintptr_t menuCloseAddr = reinterpret_cast<uintptr_t>(menuCloseHookAddress) - 0x18E;

        // The back-step is only valid if the computed entry still lies inside the
        // module; a future pattern collision elsewhere could otherwise point the
        // hook at an unrelated address.
        const uintptr_t module_end = module_base + module_size;
        if (menuOpenAddr < module_base || menuOpenAddr >= module_end ||
            menuCloseAddr < module_base || menuCloseAddr >= module_end)
        {
            throw std::runtime_error("Menu function entry point resolved outside module bounds");
        }

        DMK::HookManager &hook_manager = DMK::HookManager::get_instance();

        auto openResult = hook_manager.create_inline_hook(
            "MenuOpen",
            menuOpenAddr,
            reinterpret_cast<void *>(MenuOpenDetour),
            reinterpret_cast<void **>(&fpMenuOpenOriginal));

        if (!openResult.has_value())
        {
            throw std::runtime_error("Failed to create menu open hook: " + std::string(DMK::Hook::error_to_string(openResult.error())));
        }

        auto closeResult = hook_manager.create_inline_hook(
            "MenuClose",
            menuCloseAddr,
            reinterpret_cast<void *>(MenuCloseDetour),
            reinterpret_cast<void **>(&fpMenuCloseOriginal));

        if (!closeResult.has_value())
        {
            throw std::runtime_error("Failed to create menu close hook: " + std::string(DMK::Hook::error_to_string(closeResult.error())));
        }

        return true;
    }
    catch (const std::exception &e)
    {
        logger.error("UIMenuHook: Initialization failed: {}", e.what());
        return false;
    }
}

bool isGameMenuOpen() noexcept
{
    return g_isMenuOpen.load();
}

} // namespace TPVToggle
