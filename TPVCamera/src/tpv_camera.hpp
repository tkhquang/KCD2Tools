/**
 * @file tpv_camera.hpp
 * @brief Mod lifecycle entry points driven by the DetourModKit Session.
 */
#ifndef TPVCAMERA_TPV_CAMERA_HPP
#define TPVCAMERA_TPV_CAMERA_HPP

#include "hooks/hook_set.hpp"

#include <DetourModKit.hpp>
#include <DetourModKit/abi/wheel_host.h>

namespace TPVCamera
{

    /**
     * @brief Initializes the whole mod: config, hooks, and input bindings.
     * @param session The live Session. Its scope() takes the input BindingGuards, so ~Session clears
     *        them first (reverse insertion order) and abandon() retains them untouched on the
     *        process-termination path rather than destroying callbacks under the loader lock.
     * @param wheel_host Resident wheel-host table owned by a never-unloaded module, or nullptr.
     * @details Runs off the loader lock: on the bootstrap worker in the release build, on the dev loader's control
     *          thread in the dev build. Loads and logs configuration, validates the game module, installs the camera
     *          and UI hooks, registers input bindings, and enables INI hot-reload.
     *
     *          A mouse-wheel binding makes the input engine take a permanent module keepalive on whichever module
     *          hosts the wheel capture (ModulePinReason::MessageHookKeepalive). The release build is a single DLL that
     *          is never unloaded, so the local MessageHook backend is correct there and @p wheel_host stays nullptr.
     *          Under the dev loader the same keepalive pins the logic DLL, so the loader owns a resident host
     *          and passes its table here. The pointer selects the backend, so dev and release run one code path.
     * @return An empty Result on success, or the library Error of the step that failed. The failing step logs the
     *         reason at error level.
     */
    [[nodiscard]] DMK::Result<void> init(DMK::Session &session, const WheelHostTable *wheel_host = nullptr);

    /**
     * @brief Tears the mod down: joins the mod's workers and retires every hook.
     * @return Retired when every worker joined and every hooked prologue was restored. Busy when a game thread stayed
     *         inside a detour: the hooks stay disabled, and a later call can finish. Failed when a worker did not join
     *         or a hook failed to disable or restore its target: the module must then stay mapped for the process.
     * @details Hooks are caller-owned, so the library unhooks nothing on shutdown and this is the only path that
     *          restores the patched prologues. Run it off the loader lock: a ~Hook under the loader lock pins the
     *          backend and leaves the target patched, and a worker join there detaches instead. Every step is
     *          idempotent, so a call after Busy resumes the teardown.
     */
    [[nodiscard]] RetireStatus shutdown();

} // namespace TPVCamera

#endif // TPVCAMERA_TPV_CAMERA_HPP
