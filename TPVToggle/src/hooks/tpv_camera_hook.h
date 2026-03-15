/**
 * @file hooks/tpv_camera_hook.h
 * @brief Header for third-person camera position offset hook.
 *
 * Provides functions to initialize and manage the hook that intercepts
 * TPV camera updates to apply custom position offsets.
 */
#pragma once

#include <cstdint>

/**
 * @brief Initialize TPV camera update hook.
 * @param moduleBase Base address of the target game module
 * @param moduleSize Size of the target game module in bytes
 * @return true if initialization successful, false otherwise
 */
bool initializeTpvCameraHook(uintptr_t moduleBase, size_t moduleSize);

/**
 * @brief Cleanup TPV camera update hook.
 */
void cleanupTpvCameraHook();
