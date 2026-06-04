/**
 * @file hooks/ui_menu_hooks.cpp
 * @brief Implementation of in-game menu hooks for menu open/close detection using DetourModKit.
 *
 * Implements hooks that directly intercept the game's UI menu open and close
 * functions to detect when the player opens or closes the in-game menu.
 */

#include "ui_menu_hooks.hpp"
#include "constants.hpp"

#include <DetourModKit.hpp>

#include <stdexcept>
#include <atomic>

namespace TPVCamera
{

// Function pointer types for the UI menu open/close functions.
using MenuOpenFunc = void(__fastcall *)(void *this_ptr, char param_byte);
using MenuCloseFunc = void(__fastcall *)(void *this_ptr);

// Hook state
static MenuOpenFunc s_menu_open_original = nullptr;
static MenuCloseFunc s_menu_close_original = nullptr;

// Menu state tracking
static std::atomic<bool> s_is_menu_open(false);

/**
 * @brief Detour for the menu-open function (inline hook at its entry point).
 * @details Records the menu-open state, then calls the original. Only the mod-side
 *          work is guarded by try/catch; the original is invoked exactly once
 *          outside the guard so an exception in the mod logic can never call the
 *          engine function twice.
 * @param this_ptr Pointer to the UI menu object
 * @param param_byte Parameter passed to the original function
 */
static void __fastcall menu_open_detour(void *this_ptr, char param_byte)
{
    DMK::Logger &logger = DMK::Logger::get_instance();

    try
    {
        logger.debug("UIMenuHook: Game menu is opening");

        s_is_menu_open.store(true);
    }
    catch (const std::exception &e)
    {
        logger.error("UIMenuHook: Exception in menu open detour: {}", e.what());
    }
    catch (...)
    {
        logger.error("UIMenuHook: Unknown exception in menu open detour");
    }

    if (s_menu_open_original)
    {
        s_menu_open_original(this_ptr, param_byte);
    }
    else
    {
        logger.error("UIMenuHook: Menu open original function pointer is NULL");
    }
}

/**
 * @brief Detour for the menu-close function (inline hook at its entry point).
 * @details Clears the menu-open state, then calls the original. Only the mod-side
 *          work is guarded by try/catch; the original is invoked exactly once
 *          outside the guard so an exception in the mod logic can never call the
 *          engine function twice.
 * @param this_ptr Pointer to the UI menu object
 */
static void __fastcall menu_close_detour(void *this_ptr)
{
    DMK::Logger &logger = DMK::Logger::get_instance();

    try
    {
        logger.debug("UIMenuHook: Game menu is closing");

        s_is_menu_open.store(false);
    }
    catch (const std::exception &e)
    {
        logger.error("UIMenuHook: Exception in menu close detour: {}", e.what());
    }
    catch (...)
    {
        logger.error("UIMenuHook: Unknown exception in menu close detour");
    }

    if (s_menu_close_original)
    {
        s_menu_close_original(this_ptr);
    }
    else
    {
        logger.error("UIMenuHook: Menu close original function pointer is NULL");
    }
}

bool initialize_ui_menu_hooks(uintptr_t module_base, size_t module_size)
{
    DMK::Logger &logger = DMK::Logger::get_instance();

    try
    {
        auto open_pattern = DMK::Scanner::parse_aob(Constants::UI_MENU_OPEN_AOB_PATTERN);
        if (!open_pattern.has_value())
        {
            throw std::runtime_error("Failed to parse menu open AOB pattern");
        }

        auto close_pattern = DMK::Scanner::parse_aob(Constants::UI_MENU_CLOSE_AOB_PATTERN);
        if (!close_pattern.has_value())
        {
            throw std::runtime_error("Failed to parse menu close AOB pattern");
        }

        const std::byte *menu_open_hook_address = DMK::Scanner::find_pattern(reinterpret_cast<const std::byte *>(module_base), module_size, *open_pattern);
        if (!menu_open_hook_address)
        {
            throw std::runtime_error("Menu open function pattern not found");
        }

        const std::byte *menu_close_hook_address = DMK::Scanner::find_pattern(reinterpret_cast<const std::byte *>(module_base), module_size, *close_pattern);
        if (!menu_close_hook_address)
        {
            throw std::runtime_error("Menu close function pattern not found");
        }

        // The AOB matches an instruction inside each function body; step back a
        // fixed delta to the real entry point that SafetyHook must patch.
        //   open:  match is +0x36 into the function (entry sub_180C0B618)
        //   close: match is +0x18E into the function (entry sub_180C0B260)
        uintptr_t menu_open_addr = reinterpret_cast<uintptr_t>(menu_open_hook_address) - 0x36;
        uintptr_t menu_close_addr = reinterpret_cast<uintptr_t>(menu_close_hook_address) - 0x18E;

        // The back-step is only valid if the computed entry still lies inside the
        // module; a future pattern collision elsewhere could otherwise point the
        // hook at an unrelated address.
        const uintptr_t module_end = module_base + module_size;
        if (menu_open_addr < module_base || menu_open_addr >= module_end ||
            menu_close_addr < module_base || menu_close_addr >= module_end)
        {
            throw std::runtime_error("Menu function entry point resolved outside module bounds");
        }

        DMK::HookManager &hook_manager = DMK::HookManager::get_instance();

        auto open_result = hook_manager.create_inline_hook(
            "MenuOpen",
            menu_open_addr,
            reinterpret_cast<void *>(menu_open_detour),
            reinterpret_cast<void **>(&s_menu_open_original));

        if (!open_result.has_value())
        {
            throw std::runtime_error("Failed to create menu open hook: " + std::string(DMK::Hook::error_to_string(open_result.error())));
        }

        auto close_result = hook_manager.create_inline_hook(
            "MenuClose",
            menu_close_addr,
            reinterpret_cast<void *>(menu_close_detour),
            reinterpret_cast<void **>(&s_menu_close_original));

        if (!close_result.has_value())
        {
            throw std::runtime_error("Failed to create menu close hook: " + std::string(DMK::Hook::error_to_string(close_result.error())));
        }

        return true;
    }
    catch (const std::exception &e)
    {
        logger.error("UIMenuHook: Initialization failed: {}", e.what());
        return false;
    }
}

bool is_game_menu_open() noexcept
{
    return s_is_menu_open.load(std::memory_order_relaxed);
}

} // namespace TPVCamera
