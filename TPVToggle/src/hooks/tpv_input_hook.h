#pragma once

#include <cstdint>

/**
 * @file tpv_input_hook.h
 * @brief Hook for the C_CameraThirdPerson input processing function.
 *        Used to implement orbital mouse control.
 */

// Function signature for FUN_183924908 (or its KCD2 equivalent)
// thisPtr (RCX) is the C_CameraThirdPerson instance (or similar TPV camera state object)
// inputEventPtr (RDX) points to the raw input event structure
typedef void(__fastcall *TpvCameraInputFunc)(uintptr_t thisPtr, char *inputEventPtr);

/**
 * @brief Initializes the TPV camera input hook.
 *
 * @param moduleBase Base address of the game's main module (e.g., WHGame.dll).
 * @param moduleSize Size of the game's main module.
 * @return true if initialization was successful, false otherwise.
 */
bool initializeTpvInputHook(uintptr_t moduleBase, size_t moduleSize);

/**
 * @brief Cleans up and removes the TPV camera input hook.
 */
void cleanupTpvInputHook();

/**
 * @brief Recalculates g_orbitalCameraRotation based on g_orbitalCameraYaw and g_orbitalCameraPitch.
 *        Also applies pitch clamping.
 */
void update_orbital_camera_rotation_from_euler();
