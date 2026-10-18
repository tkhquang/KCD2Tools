/**
 * @file tpv_camera.hpp
 * @brief Mod lifecycle entry points driven by the DetourModKit Session.
 */
#ifndef TPVCAMERA_TPV_CAMERA_HPP
#define TPVCAMERA_TPV_CAMERA_HPP

#include <DetourModKit.hpp>
#include <DetourModKit/abi/wheel_host.h>
#include <DetourModKit/hook.hpp>

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

    /** @brief The outcome of @ref shutdown. */
    struct ShutdownResult
    {
        /// The hook retirement. Busy and Refused leave every hook armed, so a later shutdown() can retry it.
        DMK::hook::HookRetirement hooks{};
        /// False when a worker or game state outside the hooks still needs this image, so the image must stay mapped.
        bool complete = false;
    };

    /**
     * @brief Tears the mod down: releases the hook-driven features, retires every hook, then joins the mod's workers.
     * @details Hooks are caller-owned, so the library unhooks nothing on shutdown and this is the only path that
     *          restores the patched prologues. The native turn animation, the ranged re-aim and the shader twins are
     *          released through their detours, so that release runs before the retirement. A retirement that is not
     *          Retired, or that pinned a hook, stops the teardown there, so everything a detour reads stays. Run it
     *          off the loader lock, where HookStack::retire changes nothing.
     * @return The hook retirement and whether every worker joined.
     * @note Idempotent: a call after Busy retries the retirement.
     */
    [[nodiscard]] ShutdownResult shutdown() noexcept;

} // namespace TPVCamera

#endif // TPVCAMERA_TPV_CAMERA_HPP
