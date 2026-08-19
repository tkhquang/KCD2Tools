/**
 * @file hooks/ui_menu_hooks.cpp
 * @brief Implementation of in-game menu hooks for menu open/close detection using DetourModKit.
 *
 * Implements hooks that directly intercept the game's UI menu open and close
 * functions to detect when the player opens or closes the in-game menu.
 */

#include "ui_menu_hooks.hpp"
#include "aob_resolver.hpp"

#include "../dmk_aliases.hpp"

#include <DetourModKit.hpp>

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
    static void __fastcall menu_open_detour(void *this_ptr, char param_byte) noexcept
    {
        (void)DMK::log().log_noexcept(DMK::LogLevel::Debug, "UIMenuHook: Game menu is opening");
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
    static void __fastcall menu_close_detour(void *this_ptr) noexcept
    {
        (void)DMK::log().log_noexcept(DMK::LogLevel::Debug, "UIMenuHook: Game menu is closing");
        s_is_menu_open.store(false, std::memory_order_relaxed);

        if (s_menu_close_original)
        {
            s_menu_close_original(this_ptr);
        }
    }

    DMK::Result<void> initialize_ui_menu_hooks(DMK::hook::HookStack &hooks)
    {
        // The default hook::Options prologue policy is Fail: refuse the install when the resolved entry
        // leads with a call or breakpoint byte. A sibling mod's E9 jump hook decodes as a relocatable
        // branch rather than a refusal, so the cascade's entry-anchored layering still works.
        //
        // Every failure below propagates the library's own Error rather than a stringified exception, so the
        // caller keeps the typed ErrorCode. That matters for the install codes in particular:
        // TargetAlreadyHookedByThisKit means drop our own handle, TargetAlreadyHookedByAnotherModule means a
        // sibling mod owns the target, and the correct response differs.

        // Each cascade resolves the function entry directly (P1 anchors on the entry; the mid-body P2/P3
        // fallbacks walk back to it via their negative disp_offset), resolved up front by
        // resolve_all_anchors() and read here via anchor_address().
        const uintptr_t menu_open_addr = anchor_address(AnchorId::MenuOpen);
        const uintptr_t menu_close_addr = anchor_address(AnchorId::MenuClose);
        if (menu_open_addr == 0 || menu_close_addr == 0)
        {
            return std::unexpected(DMK::Error{DMK::ErrorCode::NoMatch, "ui_menu_hooks/anchor"});
        }

        auto open_result = DMK::hook::inline_at(
            DMK::hook::InlineRequest{.name = "MenuOpen", .target = DMK::Address{menu_open_addr}}, menu_open_detour);
        if (!open_result.has_value())
        {
            return std::unexpected(open_result.error());
        }
        // Publish each trampoline BEFORE enable() arms its patch.
        s_menu_open_original = open_result->original<MenuOpenFunc>();
        if (auto armed = open_result->enable(); !armed.has_value())
        {
            return std::unexpected(armed.error());
        }
        hooks.push(std::move(*open_result));

        auto close_result = DMK::hook::inline_at(
            DMK::hook::InlineRequest{.name = "MenuClose", .target = DMK::Address{menu_close_addr}}, menu_close_detour);
        if (!close_result.has_value())
        {
            return std::unexpected(close_result.error());
        }
        s_menu_close_original = close_result->original<MenuCloseFunc>();
        if (auto armed = close_result->enable(); !armed.has_value())
        {
            return std::unexpected(armed.error());
        }
        hooks.push(std::move(*close_result));

        return {};
    }

    bool is_game_menu_open() noexcept
    {
        return s_is_menu_open.load(std::memory_order_relaxed);
    }

} // namespace TPVCamera
