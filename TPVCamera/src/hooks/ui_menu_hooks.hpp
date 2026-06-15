/**
 * @file hooks/ui_menu_hooks.hpp
 * @brief Header for in-game menu hooks functionality.
 *
 * Provides functions to initialize and manage hooks that directly intercept
 * the game's UI menu open and close functions.
 */
#ifndef TPVCAMERA_UI_MENU_HOOKS_HPP
#define TPVCAMERA_UI_MENU_HOOKS_HPP

namespace TPVCamera
{

/**
 * @brief Installs the UI menu open/close hooks from the pre-resolved anchors.
 * @return true if both hooks installed, false otherwise.
 * @note Call after resolve_all_anchors(); the hook targets are read via anchor_address().
 */
[[nodiscard]] bool initialize_ui_menu_hooks();

/**
 * @brief Check if the in-game menu is currently open.
 * @return true if the menu is open, false otherwise.
 */
[[nodiscard]] bool is_game_menu_open() noexcept;

} // namespace TPVCamera

#endif // TPVCAMERA_UI_MENU_HOOKS_HPP
