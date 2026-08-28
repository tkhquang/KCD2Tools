/**
 * @file tpv_camera.hpp
 * @brief Mod lifecycle entry points driven by the DetourModKit Session.
 */
#ifndef TPVCAMERA_TPV_CAMERA_HPP
#define TPVCAMERA_TPV_CAMERA_HPP

#include <DetourModKit.hpp>
#include <DetourModKit/abi/wheel_host.h>

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
    /**
     * @param wheel_host Resident wheel-host table owned by a never-unloaded module, or nullptr.
     * @details A mouse-wheel binding makes the input engine take a PERMANENT module keepalive on whichever
     *          module hosts the wheel capture (ModulePinReason::MessageHookKeepalive). In the single-DLL
     *          release build that module is the mod itself, which is never unloaded, so the local
     *          MessageHook backend is correct and this stays nullptr. Under the two-DLL dev loader the same
     *          keepalive would pin the logic DLL and make it permanently unloadable, so the loader owns a
     *          resident host and passes its table here.
     *
     *          Passing the pointer rather than branching on a build macro keeps ONE code path: the value
     *          selects the backend, so dev and release run the same logic.
     */
    [[nodiscard]] DMK::Result<void> init(DMK::Session &session, const WheelHostTable *wheel_host = nullptr);

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
