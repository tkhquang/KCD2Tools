/**
 * @file hooks/fov_hook.h
 * @brief Header for TPV FOV hook functionality.
 *
 * Provides functions to initialize and manage the hook that modifies
 * the field of view when in third-person view mode.
 */
#ifndef FOV_HOOK_H
#define FOV_HOOK_H

#include <windows.h>
#include <cstdint>

// Function signature for the TPV FOV calculation function
typedef void(__fastcall *TpvFovCalculateFunc)(float *pViewStruct, float deltaTime);

/**
 * @brief Initialize the TPV FOV hook.
 * @param module_base Base address of the target game module.
 * @param module_size Size of the target game module in bytes.
 * @param desired_fov_degrees Desired FOV in degrees (or -1 to disable).
 * @return true if initialization successful, false otherwise.
 */
bool initializeFovHook(uintptr_t module_base, size_t module_size, float desired_fov_degrees);

/**
 * @brief Clean up FOV hook resources.
 */
void cleanupFovHook();

/**
 * @brief Check if FOV hook is active and ready.
 * @return true if hook is successfully installed and functional.
 */
bool isFovHookActive();

#endif // FOV_HOOK_H
