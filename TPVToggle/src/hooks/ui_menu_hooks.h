/**
 * @file hooks/ui_menu_hooks.h
 * @brief Header for in-game menu hooks functionality.
 *
 * Provides functions to initialize and manage hooks that directly intercept
 * the game's UI menu open and close functions.
 */
#ifndef UI_MENU_HOOKS_H
#define UI_MENU_HOOKS_H

#include <windows.h>
#include <cstdint>

/**
 * @brief Initialize UI menu hooks.
 * @param module_base Base address of the target game module.
 * @param module_size Size of the target game module in bytes.
 * @return true if initialization successful, false otherwise.
 */
bool initializeUiMenuHooks(uintptr_t module_base, size_t module_size);

/**
 * @brief Clean up UI menu hook resources.
 */
void cleanupUiMenuHooks();

/**
 * @brief Check if UI menu hooks are active and ready.
 * @return true if all required hooks are successfully installed and functional.
 */
bool areUiMenuHooksActive();

/**
 * @brief Check if the in-game menu is currently open.
 * @return true if the menu is open, false otherwise.
 */
bool isGameMenuOpen();

#endif // UI_MENU_HOOKS_H
