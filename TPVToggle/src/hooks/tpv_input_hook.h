/**
 * @file tpv_input_hook.h
 * @brief Hook for the third-person camera input processing function
 *
 * Provides customizable camera control including sensitivity adjustment
 * and vertical pitch limits for third-person view.
 */
#pragma once

#include <cstdint>

/**
 * @brief Initializes the TPV camera input hook
 * @param moduleBase Base address of the game's main module
 * @param moduleSize Size of the game's main module
 * @return true if initialization was successful, false otherwise
 */
bool initializeTpvInputHook(uintptr_t moduleBase, size_t moduleSize);

/**
 * @brief Cleans up and removes the TPV camera input hook
 */
void cleanupTpvInputHook();

/**
 * @brief Reset camera angles to default values
 * @details Used when switching views or resetting camera state
 */
void resetCameraAngles();

// Optional: function to check if the hook is active
bool isTpvInputHookActive();
