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
    struct RigState;

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
     * @param hooks The mod's hook stack. Each handle is stored before its arm.
     * @return An empty value when the mandatory frustum-builder hook was installed (best-effort head/input
     *         hooks may still warn), or the library Error that refused it (ErrorCode::NoMatch when the anchor
     *         cascade did not resolve).
     */
    [[nodiscard]] DMK::Result<void> initialize_camera(uintptr_t module_base, size_t module_size,
                                                      DMK::hook::HookStack &hooks);

    /**
     * @brief Resolves the zoom hold bindings to BindingTokens and publishes them for the per-frame query.
     * @details BindingToken acquisition is control-plane work. A token goes stale whenever its binding reshapes
     *          (an INI rebind or a consume-flag change), so call this after the input engine starts and again
     *          after every INI reload. Until the first publish the detour uses the name-based query.
     * @note Setup/control-plane only: it allocates. Never call it from a detour or an input callback.
     */
    void refresh_zoom_binding_tokens() noexcept;

    /**
     * @brief Drops the published zoom BindingTokens. Call at teardown, after the detours are quiescent.
     */
    void release_zoom_binding_tokens() noexcept;

    /**
     * @brief Starts a camera-relative free-look move on the press of a movement key: the look and the body turn to the
     *        camera heading at the press.
     * @details The input dispatch runs at the start of the frame on the game thread, before the job-system movement
     *          update; the camera detour that captures the camera heading runs after it. Turning the look and the body
     *          from the dispatch saves the frame the camera-side start loses. The game still builds that frame's
     *          travel from the old facing, so the movement detour drops that one request. Uses the heading the last
     *          game-view frame published, and only while it is current and third person and free-look are on.
     * @note Callable from any thread; it acts only on the thread the camera detour ran on.
     */
    void orbit_move_pressed() noexcept;

    /**
     * @brief Whether the frustum detour rendered the third-person offset on its last game-view frame.
     * @note Any thread: one relaxed atomic load. The head, input and camera-observer hooks mirror it.
     */
    [[nodiscard]] bool camera_offset_active() noexcept;

    /**
     * @brief The rig's carry-over (core/rig/rig_stage.hpp).
     * @note Main thread only: the camera frame, and between frames the input dispatcher and the action dispatcher,
     * which
     *       run on the same thread.
     */
    [[nodiscard]] RigState &rig_state() noexcept;

    /**
     * @brief Whether the game showed the OS cursor (a UI is up) on the frustum detour's last game-view frame.
     * @note Any thread: one relaxed atomic load. The free-look input gate freezes the orbit while it holds.
     */
    [[nodiscard]] bool game_cursor_shown() noexcept;

    /**
     * @brief Whether the third-person offset should be applied right now.
     * @details Suppressed (forced first-person) while a cutscene plays, whatever the INI lists, and while the game
     *          is in any state listed in SuppressTPVState, so that context renders from the game's own untouched
     *          camera. This is a HARD gate: it
     *          overrides cam.applying, so the player cannot toggle third-person back on while a listed state
     *          lasts (unlike the edge-triggered ForcedFPVState policy), and the prior view resumes when the
     *          state ends. Menu and Overlay are read from the LIVE UI signals, so they suppress the same
     *          frame the screen opens and release the same frame it closes; every other state (Combat, Mount,
     *          Dialogue, Minigame, Aiming, Crouch) is matched against the debounced game-state mask, so a
     *          brief flicker does not pop the view. Read directly from the live config, so it is independent
     *          of EnableStateBehavior. Called from the main thread (the frustum detour) and the
     *          input-dispatch thread (the free-look gate); every read is atomic, so the cross-thread call is
     *          safe.
     */
    [[nodiscard]] bool should_apply_view() noexcept;

} // namespace TPVCamera

#endif // TPVCAMERA_CAMERA_HOOK_HPP
