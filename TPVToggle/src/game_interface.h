/**
 * @file game_interface.h
 * @brief Provides interface to game memory structures and state.
 *
 * Handles the complex pointer chain navigation to access game state like
 * TPV flags and manages memory safety and validation.
 */
#ifndef GAME_INTERFACE_H
#define GAME_INTERFACE_H

#include <windows.h>
#include <cstdint>

/**
 * @brief Initialize game interface with dynamic AOB scanning.
 * @param module_base Base address of the target game module.
 * @param module_size Size of the target game module in bytes.
 * @return true if initialization successful, false otherwise.
 */
bool initializeGameInterface(uintptr_t module_base, size_t module_size);

/**
 * @brief Clean up game interface resources.
 */
void cleanupGameInterface();

/**
 * @brief Gets the resolved address of the TPV flag.
 * @details Resolves the full pointer chain: global context -> camera manager -> TPV object -> flag.
 * @return Pointer to TPV flag byte, or nullptr if resolution fails.
 */
volatile BYTE *getResolvedTpvFlagAddress();

/**
 * @brief Gets the current view state (FPV=0, TPV=1).
 * @return 0 for FPV, 1 for TPV, -1 on error.
 */
int getViewState();

/**
 * @brief Sets the view state.
 * @param new_state 0 for FPV, 1 for TPV.
 * @param key_pressed_vk Optional VK code that triggered this change.
 * @return true if successful, false otherwise.
 */
bool setViewState(BYTE new_state, int *key_pressed_vk = nullptr);

/**
 * @brief Toggles between FPV and TPV modes.
 * @param key_pressed_vk Optional VK code that triggered this change.
 * @return true if successful, false otherwise.
 */
bool safeToggleViewState(int *key_pressed_vk = nullptr);

/**
 * @brief Gets the current overlay state.
 * @details Checks if any UI overlay (menu, inventory, dialog) is currently active.
 * @return >0 if overlay is active, 0 if inactive, -1 on error.
 */
long long getOverlayState();

/**
 * @brief Gets the camera manager instance.
 * @details Used for FOV and other camera operations.
 * @return Pointer to camera manager, or 0 if not available.
 */
uintptr_t getCameraManagerInstance();

#endif // GAME_INTERFACE_H
