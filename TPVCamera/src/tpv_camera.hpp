/**
 * @file tpv_camera.hpp
 * @brief Mod lifecycle entry points driven by DMK::Bootstrap.
 */
#ifndef TPVCAMERA_TPV_CAMERA_HPP
#define TPVCAMERA_TPV_CAMERA_HPP

namespace TPVCamera
{

/**
 * @brief Initializes the whole mod: config, hooks, and input bindings.
 * @details Runs on the Bootstrap worker thread (off the loader lock). Loads and
 *          logs configuration, validates the game module, installs the camera and
 *          UI hooks, registers input bindings, and enables INI hot-reload.
 * @return true on success; false if a critical subsystem failed.
 */
[[nodiscard]] bool init();

/**
 * @brief Tears the mod down: clears bindings and resets the game interface.
 * @details Does not call DMK_Shutdown(); DMK::Bootstrap owns that ordering and
 *          invokes it after this returns.
 */
void shutdown();

} // namespace TPVCamera

#endif // TPVCAMERA_TPV_CAMERA_HPP
