/**
 * @file hooks/camera_hook.hpp
 * @brief Header for the third-person camera (frustum-builder offset approach).
 *
 * Renders a third-person view by offsetting the game view camera's matrix at the
 * frustum builder (CCamera::UpdateFrustumPlanes), before the cull planes are
 * computed, so the rendered view and its culling move together. Every gameplay
 * camera (first person, combat, mount) funnels into that builder for the active
 * CView. Because the player's look/aim channel is never touched, aiming and
 * interaction keep working off the first-person frame. A companion hook on the
 * head-visibility setter keeps the player head rendered from behind, and an
 * input-dispatcher hook powers the free-look orbit.
 */
#ifndef TPVCAMERA_CAMERA_HOOK_HPP
#define TPVCAMERA_CAMERA_HOOK_HPP

#include <DetourModKit/error.hpp>
#include <DetourModKit/hook.hpp>

#include <cstddef>
#include <cstdint>

namespace TPVCamera
{

    // Input binding names shared across the registration site (tpv_camera.cpp) and the INI
    // hot-reload re-bind (config.cpp), so every site agrees on the exact spelling. The zoom holds are
    // queried per frame in the frustum-builder detour to drive the follow distance; the orbit hold is
    // edge-driven (its callback engages/releases free-look directly on the key edges), not polled.
    inline constexpr const char *k_zoom_in_binding = "camera_zoom_in";
    inline constexpr const char *k_zoom_out_binding = "camera_zoom_out";
    inline constexpr const char *k_orbit_hold_binding = "orbit_hold";

    /**
     * @brief Installs the third-person camera hooks.
     * @details Hooks the frustum builder (resolved by AOB) and offsets the game view
     *          camera matrix to sit behind the player while leaving the look orientation
     *          and the eye-frame pose untouched. Also hooks the head-visibility setter so
     *          the first-person rig keeps the player head while the offset is active, and
     *          the input dispatcher for free-look orbit. The detours fast-path out while
     *          the offset is toggled off, so they are harmless when the view is first-person.
     * @param module_base Base address of the target game module.
     * @param module_size Size of the target game module in bytes.
     * @param hooks The mod's hook stack; each installed handle is pushed in install order.
     * @return An empty value when the mandatory frustum-builder hook was installed (best-effort head/input
     *         hooks may still warn), or the library Error that refused it (ErrorCode::NoMatch when the anchor
     *         cascade did not resolve).
     */
    [[nodiscard]] DMK::Result<void> initialize_camera(uintptr_t module_base, size_t module_size,
                                                      DMK::hook::HookStack &hooks);

} // namespace TPVCamera

#endif // TPVCAMERA_CAMERA_HOOK_HPP
