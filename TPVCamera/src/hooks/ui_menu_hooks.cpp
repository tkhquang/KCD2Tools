/**
 * @file hooks/ui_menu_hooks.cpp
 * @brief Implementation of in-game menu hooks for menu open/close detection using DetourModKit.
 *
 * Implements hooks that directly intercept the game's UI menu open and close
 * functions to detect when the player opens or closes the in-game menu.
 */

#include "ui_menu_hooks.hpp"
#include "aob_resolver.hpp"

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
 * @details Records the menu-open state, then calls the original exactly once. No SEH frame and no C++
 *          try: the detour only logs through the no-throw logger and stores a mod-owned atomic, so it
 *          performs no foreign-memory dereference and cannot throw. The original is invoked outside any
 *          guard so the engine function runs exactly once.
 * @param this_ptr Pointer to the UI menu object
 * @param param_byte Parameter passed to the original function
 */
static void __fastcall menu_open_detour(void *this_ptr, char param_byte)
{
    (void)DMK::Logger::get_instance().log_noexcept(DMK::LogLevel::Debug, "UIMenuHook: Game menu is opening");
    s_is_menu_open.store(true, std::memory_order_relaxed);

    if (s_menu_open_original)
    {
        s_menu_open_original(this_ptr, param_byte);
    }
}

/**
 * @brief Detour for the menu-close function (inline hook at its entry point).
 * @details Clears the menu-open state, then calls the original exactly once. As for the open detour, no
 *          SEH frame and no C++ try are needed: it only logs (no-throw) and stores a mod-owned atomic.
 * @param this_ptr Pointer to the UI menu object
 */
static void __fastcall menu_close_detour(void *this_ptr)
{
    (void)DMK::Logger::get_instance().log_noexcept(DMK::LogLevel::Debug, "UIMenuHook: Game menu is closing");
    s_is_menu_open.store(false, std::memory_order_relaxed);

    if (s_menu_close_original)
    {
        s_menu_close_original(this_ptr);
    }
}

bool initialize_ui_menu_hooks()
{
    DMK::Logger &logger = DMK::Logger::get_instance();

    // Fail closed if the resolved entry leads with a call/breakpoint byte; a sibling mod's E9 jump-hook
    // does not trip this gate, so the cascade's entry-anchored layering still works.
    const DMK::HookConfig hook_config{.prologue_policy = DMK::InlineProloguePolicy::Fail};

    try
    {
        // Each cascade resolves the function entry directly (P1 anchors on the entry; the mid-body P2/P3
        // fallbacks walk back to it via their negative disp_offset), resolved up front by
        // resolve_all_anchors() and read here via anchor_address().
        const uintptr_t menu_open_addr = anchor_address(AnchorId::MenuOpen);
        const uintptr_t menu_close_addr = anchor_address(AnchorId::MenuClose);
        if (menu_open_addr == 0 || menu_close_addr == 0)
        {
            throw std::runtime_error("Menu function entry cascade did not resolve");
        }

        DMK::HookManager &hook_manager = DMK::HookManager::get_instance();

        auto open_result = hook_manager.create_inline_hook(
            "MenuOpen",
            menu_open_addr,
            reinterpret_cast<void *>(menu_open_detour),
            reinterpret_cast<void **>(&s_menu_open_original),
            hook_config);

        if (!open_result.has_value())
        {
            throw std::runtime_error("Failed to create menu open hook: " + std::string(DMK::Hook::error_to_string(open_result.error())));
        }

        auto close_result = hook_manager.create_inline_hook(
            "MenuClose",
            menu_close_addr,
            reinterpret_cast<void *>(menu_close_detour),
            reinterpret_cast<void **>(&s_menu_close_original),
            hook_config);

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
