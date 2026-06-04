/**
 * @file global_state.hpp
 * @brief cross-module shared state grouped behind accessor functions.
 *
 * @details Related state lives in small grouped structs reached through
 *          reference-returning accessors (C++ Core Guideline I.2: avoid
 *          non-const globals). Each accessor owns a single function-local
 *          static, so storage is constructed on first use with no
 *          static-initialization-order dependency between translation units.
 */
#ifndef TPVCAMERA_GLOBAL_STATE_HPP
#define TPVCAMERA_GLOBAL_STATE_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>

// Resolved base address of the game's global context. Kept as a stable
// unmangled symbol (rather than namespaced state) so it can be located from
// external tooling such as Cheat Engine or x64dbg during reverse-engineering.
extern "C"
{
    extern std::byte *g_global_context_ptr_address;
}

namespace TPVCamera
{
    /** @brief Base address and image size of the resolved game module. */
    struct ModuleInfo
    {
        std::uintptr_t base{0};
        std::size_t size{0};
    };

    /** @brief Overlay (inventory/map/dialog/codex) presence, set by the UI overlay hooks. */
    struct OverlayState
    {
        std::atomic<bool> active{false};
    };

    /**
     * @brief State for the third-person camera built on the frustum-builder offset.
     * @details The camera renders a third-person view by rewriting the game view camera's matrix
     *          at the frustum builder, before the cull planes are computed, instead of activating
     *          the engine's built-in third-person camera.
     */
    struct CameraState
    {
        // Runtime on/off of the offset, flipped by the view hotkeys. Starts off (first-person)
        // so the game looks normal on load until the player toggles the third-person view.
        std::atomic<bool> applying{false};

        // Zoom offset from the configured base distance, driven by the zoom hold keys
        // (polled per frame in the detour). Keeping zoom as a delta from the INI
        // FollowDistance (rather than an absolute value) is what lets an INI edit apply
        // live: the base is re-read every frame and the rendered distance follows
        // immediately, while the player's accumulated zoom is preserved on top.
        std::atomic<float> zoom_offset{0.0f};

        // First-person <-> third-person view-switch blend (render thread only): 0 = first person,
        // 1 = third person. Eased toward the target view each frame (smoothstepped at use) so toggling
        // and UI suppression slide instead of snapping. NOT reset on suppression -- it eases back to 0.
        float view_blend{0.0f};

        // Camera-collision carry-over, touched only by the frustum-builder detour (the game's
        // render thread), so it needs no synchronization. The allowed follow distance after the
        // collision raycast, eased so the view pulls in instantly but returns out smoothly.
        // collision_valid is cleared whenever the offset is suppressed (menu, overlay, view
        // switch) so the next applied frame snaps instead of easing across a suppression gap.
        float collision_distance{0.0f};
        bool collision_valid{false};
        // Seconds remaining to HOLD the pulled-in collision distance after the last blocking
        // hit. A thin ray grazing an edge alternates hit/miss each frame; holding through the
        // gap stops the camera pumping (sawtooth) instead of easing out and snapping back in.
        float collision_hold_timer{0.0f};

        // Free-look "level" blend (render thread only, like the fields above): eases 0 -> 1 while
        // orbiting and back to 0 on release. While orbiting the camera rig is built from a level
        // reference so a steep up/down look does not tip the orbit near the overhead pole (where it
        // spins messily); easing this in/out keeps engaging and releasing free-look smooth instead
        // of snapping between the steep follow pose and the level orbit pose. Reset with the others.
        float orbit_level_blend{0.0f};

        // Camera-relative movement (render thread only). orbit_moving latches whether the character was
        // moving last frame so the heading is aligned to the camera ONCE on the idle -> moving edge, not
        // re-aligned every frame. The move signal is the device-agnostic action-input magnitude (the
        // action dispatcher, via player_onaction_hook), captured the instant a key is pressed -- not
        // body-position speed, which lags and reads zero against a wall. Reset with the others on suppression.
        bool orbit_moving{false};
        // Camera-relative heading HOLD. A single write to the look-yaw is reverted by the engine, so
        // on the idle -> moving edge the camera-forward heading is captured into orbit_target_yaw and then
        // HELD -- written to the look every frame while moving (like the pitch leveling, whose per-frame
        // write sticks). This turns the body to face the camera and keeps it there while you orbit
        // freely; released when movement stops so the player resumes control.
        float orbit_target_yaw{0.0f};
        bool orbit_target_valid{false};
        // Orbit-input value snapshotted at move-capture. While moving, the camera holds its WORLD yaw
        // and the body turns UNDER it: the rig's orbit angle is derived as
        // (orbit_target_yaw + (orbit_yaw - this) - char_forward_yaw), so on the capture frame it equals the
        // pre-move orbit (no snap) and eases to the user's residual orbit as the body rotates to the
        // heading -- the camera never pops. Render-thread only.
        float orbit_yaw_at_capture_deg{0.0f};

        // Free-look orbit. While orbit_active (the orbit key is toggled on), the input
        // dispatcher hook captures mouse-look deltas into orbit_yaw/orbit_pitch (degrees)
        // and blocks the look events so the player aim stays put; the render hook circles
        // the camera around the player by those angles. On release the angles ease back to
        // the configured initial yaw/pitch. orbit_active is written by the input poll thread
        // and read by the render thread, the angles by both, so all are atomic.
        std::atomic<bool> orbit_active{false};
        std::atomic<float> orbit_yaw{0.0f};
        std::atomic<float> orbit_pitch{0.0f};

        // Gamepad right-stick look DEFLECTION (-1..1), latched by the input hook while orbiting. The mouse
        // posts relative deltas straight into orbit_yaw/orbit_pitch per event; the analog stick instead
        // reports a HELD position, so it is latched here and integrated by rate (with delta_time) in the
        // render hook -- holding the stick keeps orbiting, frame-rate independent. Cleared to 0 when not
        // orbiting so re-engaging with the stick centred does not jump. Written on the input thread, read
        // (and cleared) on the render thread; relaxed atomics (a one-frame-stale deflection is harmless).
        std::atomic<float> orbit_pad_yaw{0.0f};
        std::atomic<float> orbit_pad_pitch{0.0f};

        // Orbit angle low-pass (render thread only). The rendered orbit yaw/pitch (degrees) ease toward
        // the raw accumulated orbit_yaw/orbit_pitch target each frame, so free-look is not as jittery as
        // the raw per-frame mouse deltas: the engine smooths native look DOWNSTREAM of the input dispatch
        // the orbit hook captures and blocks, so without this filter the orbit gets unsmoothed input.
        // Strength is the per-preset orbit_smoothing. orbit_render_valid is cleared on suppression (and is
        // false on first engage) so the next applied frame snaps to the target instead of easing across a gap.
        float orbit_yaw_render{0.0f};
        float orbit_pitch_render{0.0f};
        bool orbit_render_valid{false};

        // Continuous-align (GTA) steering low-pass (render thread only). In continuous-align mode the
        // camera follows the body/look heading (the derived rig angle cancels to ~0), so the user's
        // orbit-since-capture STEERS the run rather than orbiting the rig. This is that steering angle
        // (degrees) low-passed toward the raw input, fed to BOTH the body/look heading and the rig
        // derivation so the camera follows the smoothed steering and stays behind. orbit_steer_valid is
        // cleared when not move-orbiting (and on suppression) so engaging snaps instead of easing across a
        // stale value. Only eased when OrbitContinuousAlign and OrbitSmoothing are both on.
        float orbit_steer_smooth{0.0f};
        bool orbit_steer_valid{false};

        // Dynamic eye-height sync (render thread only). When DynamicEyeSync is on, eye_sync_applied is the
        // eased effective eye height: it re-anchors to the REAL first-person eye when a low pose (kneel /
        // pray, and other poses EyeHeight does not model) drops it OUT of range of the configured height,
        // where a fixed height floats the camera too high. eye_sync_valid is cleared on suppression so the
        // next engaged frame snaps to the current pose instead of easing across the gap. real_eye_height is
        // the live FP-eye height above the body root, published each engaged frame for the overlay read-out
        // (and is the sync source); atomic because the overlay thread reads it.
        float eye_sync_applied{0.0f};
        bool eye_sync_valid{false};
        std::atomic<float> real_eye_height{0.0f};
        // Published live for the overlay status line: eye_sync_engaged = the dynamic sync is actively
        // re-anchoring to the real eye (a low pose is out of range) vs. idle (the configured Eye Height is in
        // effect); eye_sync_effective = the eye height actually applied this frame. Overlay thread reads.
        std::atomic<bool> eye_sync_engaged{false};
        std::atomic<float> eye_sync_effective{0.0f};

        // Per-preset FOV override ease (render thread only). fov_ease_applied is the eased rendered FOV
        // (radians) written into the render CCamera; fov_ease_stage1 is the 2-stage critically-damped
        // intermediate. fov_ease_valid is cleared on suppression (and is false on first engage) so the next
        // engaged frame SNAPS to the desired FOV instead of easing across the first-person gap, matching the
        // orbit / eye-sync / collision smoothers.
        float fov_ease_stage1{0.0f};
        float fov_ease_applied{0.0f};
        bool fov_ease_valid{false};
    };

    /**
     * @brief Rendered camera pose shared with the camera-space interaction hook.
     * @details The player use-cone (sub_180483740) is anchored on the EYE and never reads the render
     *          camera, so in third person the screen-centre crosshair and the use-target diverge by the
     *          shoulder offset (worst at close range -- you cannot interact with what the crosshair is
     *          on). The frustum-builder detour publishes the final rendered camera position + crosshair
     *          direction here each engaged frame; the interactor eye-copy detour reads it to re-origin the
     *          cone onto the screen centre. valid is true ONLY while the offset is engaged (in first
     *          person the camera IS the eye, so the hook leaves the game untouched). Written and read on
     *          different points of the frame (possibly different threads); relaxed atomics are sufficient
     *          since a one-frame-stale selection pose is harmless.
     */
    struct InteractionAimPose
    {
        std::atomic<float> pos_x{0.0f}; // rendered camera position (new cone origin)
        std::atomic<float> pos_y{0.0f};
        std::atomic<float> pos_z{0.0f};
        std::atomic<float> dir_x{0.0f}; // crosshair direction (new scoring axis)
        std::atomic<float> dir_y{1.0f};
        std::atomic<float> dir_z{0.0f};
        std::atomic<bool> valid{false};
    };

    /** @brief Returns the process-wide resolved game module info. */
    [[nodiscard]] ModuleInfo &module_info() noexcept;
    /** @brief Returns the overlay-presence state set by the UI overlay hooks. */
    [[nodiscard]] OverlayState &overlay_state() noexcept;
    /** @brief Returns the third-person camera state. */
    [[nodiscard]] CameraState &camera_state() noexcept;

    /** @brief Returns the rendered camera pose shared with the camera-space interaction hook. */
    [[nodiscard]] InteractionAimPose &interaction_aim_pose() noexcept;

    /**
     * @brief Returns the current debounced game-state mask (GameState bits, see game_state.hpp).
     * @details Written once per frame by the camera detour (render thread) and read by the input
     *          dispatch thread for the orbit-exclude gate, so it is atomic for a race-free read.
     */
    [[nodiscard]] std::atomic<uint32_t> &game_state_mask() noexcept;

    /**
     * @brief One-shot flag, set true once the player (C_Player) first resolves in-world.
     * @details Lets the overlay defer its size-sensitive setup (offscreen surface + DPI font scale)
     *          until the game is actually in gameplay -- when the window is at its final resolution --
     *          instead of snapshotting the transient loading-window size. Set by the camera detour
     *          (render thread), read by the overlay thread.
     */
    [[nodiscard]] std::atomic<bool> &game_world_ready() noexcept;

} // namespace TPVCamera

#endif // TPVCAMERA_GLOBAL_STATE_HPP
