/**
 * @file tpv_camera.hpp
 * @brief Mod lifecycle entry points driven by the DetourModKit Session.
 */
#ifndef TPVCAMERA_TPV_CAMERA_HPP
#define TPVCAMERA_TPV_CAMERA_HPP

#include <DetourModKit.hpp>

namespace TPVCamera
{

    /**
     * @brief Initializes the whole mod: config, hooks, and input bindings.
     * @param session The live Session. Its scope() takes the input BindingGuards, so ~Session clears
     *        them first (reverse insertion order) and abandon() retains them untouched on the
     *        process-termination path rather than destroying callbacks under the loader lock.
     * @details Runs on the bootstrap worker thread (off the loader lock). Loads and logs configuration,
     *          validates the game module, installs the camera and UI hooks, registers input bindings, and
     *          enables INI hot-reload.
     * @return An empty Result on success. On failure, Error{Unknown, "TPVCamera::init"}: DMK has no code
     *         for a consumer's own subsystem, and the specific reason is already logged at error level by
     *         the failing step, so the value only has to carry "do not proceed".
     */
    [[nodiscard]] DMK::Result<void> init(DMK::Session &session);

    /**
     * @brief Tears the mod down: removes every hook, stops the overlay, and resets the game interface.
     * @return True when every hooked prologue was restored. False means a hook pinned its backend, so the
     *         target stays patched and the module hosting it must NOT be unloaded: doing so leaves the
     *         hook installed and the stale image mapped, and the next load returns that stale image.
     * @details Hooks are caller-owned, so the library unhooks nothing on shutdown and this is the only
     *          path that restores the patched prologues. Run it OFF the loader lock: a ~Hook under the
     *          loader lock pins the backend and leaves the target patched. The production DllMain
     *          therefore calls it only on an explicit FreeLibrary, never on process exit. Idempotent; a
     *          repeat call reports the same verdict as the first.
     */
    [[nodiscard]] bool shutdown();

} // namespace TPVCamera

#endif // TPVCAMERA_TPV_CAMERA_HPP
