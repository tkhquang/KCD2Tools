/**
 * @file hooks/tpv_camera_hook.hpp
 * @brief Header for third-person camera position offset hook.
 *
 * Provides functions to initialize and manage the hook that intercepts
 * TPV camera updates to apply custom position offsets.
 */
#ifndef TPV_CAMERA_HOOK_HPP
#define TPV_CAMERA_HOOK_HPP

#include <cstdint>

namespace TPVToggle
{

/**
 * @brief Initialize the TPV camera update hook.
 * @details The same detour also writes the custom FOV (in radians) to the
 *          output pose at Constants::OFFSET_TpvFovWrite on every frame the
 *          third-person view is active. The FOV is read from the hot-reload
 *          atomic settings().tpvFovDegrees, so INI edits take effect on the
 *          next frame and the per-frame write survives FPV<->TPV mode toggles
 *          (the engine reinitializes the pose on a toggle and this re-applies
 *          on the next frame).
 * @param moduleBase Base address of the target game module.
 * @param moduleSize Size of the target game module in bytes.
 * @return true if initialization succeeded, false otherwise.
 */
[[nodiscard]] bool initializeTpvCameraHook(uintptr_t moduleBase, size_t moduleSize);

} // namespace TPVToggle

#endif // TPV_CAMERA_HOOK_HPP
