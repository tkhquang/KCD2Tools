/**
 * @file hooks/ui_menu_hooks.hpp
 * @brief Header for in-game menu hooks functionality.
 *
 * Provides functions to initialize and manage hooks that directly intercept
 * the game's UI menu open and close functions.
 */
#ifndef UI_MENU_HOOKS_HPP
#define UI_MENU_HOOKS_HPP

#include <cstdint>
#include <cstddef>

namespace TPVToggle
{

/**
 * @brief Initialize UI menu hooks.
 * @param module_base Base address of the target game module.
 * @param module_size Size of the target game module in bytes.
 * @return true if initialization successful, false otherwise.
 */
[[nodiscard]] bool initializeUiMenuHooks(uintptr_t module_base, size_t module_size);

/**
 * @brief Check if the in-game menu is currently open.
 * @return true if the menu is open, false otherwise.
 */
[[nodiscard]] bool isGameMenuOpen() noexcept;

} // namespace TPVToggle

#endif // UI_MENU_HOOKS_HPP
