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

// Resolved base address of the game's global context. Kept as a stable unmangled symbol (rather than
// namespaced state) so it can be located from external tooling such as Cheat Engine or x64dbg during
// reverse-engineering. Atomic because the render thread reads it (the camera/minigame chain walks in
// game_state.cpp) while shutdown nulls it on the bootstrap thread before the hooks are removed; relaxed
// is sufficient (a standalone pointer, no dependent data published through it, and each chain walk
// independently validates the value it reads). std::atomic<std::byte *> is lock-free and layout-identical
// to a raw pointer on x64, so external tooling still reads it as a plain pointer.
extern "C"
{
    extern std::atomic<std::byte *> g_global_context_ptr_address;
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
        // and UI suppression slide instead of snapping. NOT reset on suppression - it eases back to 0.
        float view_blend{0.0f};

        // Camera-collision carry-over, touched only by the frustum-builder detour (the game's
        // render thread), so it needs no synchronization. The allowed follow distance after the
        // collision raycast, eased so the view pulls in instantly but returns out smoothly.
        // collision_valid is cleared whenever the offset is suppressed (menu, overlay, view
        // switch) so the next applied frame snaps instead of easing across a suppression gap.
        float collision_distance{0.0f};
        bool collision_valid{false};
        // How fast the collision distance is easing back out (m/s, outward positive); any pull-in zeroes it, so
        // every release starts from rest.
        float collision_speed{0.0f};
        // Seconds the pulled-in distance is still held after the last recompute that met a surface (drained on
        // every frame): a probe that grazes an edge and misses it the next frame, or an arm swung fast through
        // clutter, keeps the camera in instead of letting it ease out and snap back in.
        float collision_hold_timer{0.0f};
        // First-person fallback (render thread only): set while the camera arm is shorter than HeadClearance, so the
        // camera would sit inside the player's head; head_fallback_blend eases the camera from the collided position
        // onto the eye (1 = at the eye) and back. The head is hidden while it is set (see the head-visibility detour).
        bool head_fallback{false};
        float head_fallback_blend{0.0f};

        // Free-look base (render thread only, like the fields above). While orbit_base_held the orbit turns a held
        // base, the follow rig for the look pitch orbit_base_pitch (radians), instead of the live follow pose.
        // orbit_base_offset is the held pitch less the look's; it decays to 0 as free-look eases back after it turns
        // off, and the hold ends once it has. Cleared on suppression unless a situation holds free-look off.
        bool orbit_base_held{false};
        float orbit_base_pitch{0.0f};
        float orbit_base_offset{0.0f};

        // Camera-relative movement (render thread only). orbit_moving latches whether the character was
        // moving last frame so the heading is aligned to the camera ONCE on the idle -> moving edge, not
        // re-aligned every frame. The move signal is the device-agnostic action-input magnitude (the
        // action dispatcher, via player_onaction_hook), captured the instant a key is pressed - not
        // body-position speed, which lags and reads zero against a wall. Reset with the others on suppression.
        bool orbit_moving{false};
        // Camera-relative heading HOLD. A single write to the look-yaw is reverted by the engine, so
        // on the idle -> moving edge the camera-forward heading is captured into orbit_target_yaw and then
        // HELD - written to the look every frame while moving (like the pitch leveling, whose per-frame
        // write sticks). This turns the body to face the camera and keeps it there while you orbit
        // freely; released when movement stops so the player resumes control.
        float orbit_target_yaw{0.0f};
        bool orbit_target_valid{false};
        // Orbit-input value snapshotted at move-capture. While moving, the camera holds its WORLD yaw
        // and the body turns UNDER it: the rig's orbit angle is derived as
        // (orbit_target_yaw + (orbit_yaw - this) - char_forward_yaw), so on the capture frame it equals the
        // pre-move orbit (no snap) and eases to the user's residual orbit as the body rotates to the
        // heading - the camera never pops. Render-thread only.
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
        // The allowed range of orbit_pitch this frame (degrees), published by the render thread while free-look is on
        // so OrbitPitchMin / OrbitPitchMax stay measured from level whatever pitch the held base has, and the camera
        // never goes past what the rig can show.
        std::atomic<float> orbit_pitch_lo{-90.0f};
        std::atomic<float> orbit_pitch_hi{90.0f};

        // Gamepad right-stick look DEFLECTION (-1..1), latched by the input hook while orbiting. The mouse
        // posts relative deltas straight into orbit_yaw/orbit_pitch per event; the analog stick instead
        // reports a HELD position, so it is latched here and integrated by rate (with delta_time) in the
        // render hook - holding the stick keeps orbiting, frame-rate independent. Cleared to 0 when not
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

        // Orbit move-detection re-arm latch (render thread only). A genuine sub-stop movement-input reading must
        // be observed since orbit engaged, or the game's own move input must show the movement too, before a
        // move-start is honored, so a stranded input latch (a held-move release swallowed on a combat action-map
        // swap) cannot re-trip the body-turn with the keys released. Cleared when orbit disengages, so re-engaging
        // needs an observed release or a held key the game's own move input shows. orbit_move_armed_by_game is set
        // when that game input armed it: such a move also ends once that input has stayed idle for
        // orbit_move_game_idle seconds, since another latch slot can still be stranded. Pairs with
        // player_onaction_reset() (which drops the stale latch outright on the suspend/disengage edges).
        bool orbit_move_armed{false};
        bool orbit_move_armed_by_game{false};
        float orbit_move_game_idle{0.0f};

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

        // Aim-basis low-pass (render thread only). The third-person rig basis quaternion (XYZW), low-passed
        // toward the per-frame target (the look-controller aim quat under StableAimBasis, else the eye quat)
        // when AimBasisSmoothing is on, so engine-driven view rotation - which the follow distance amplifies
        // into a camera-position swing - is damped. basis_quat_valid is cleared on suppression (and is false
        // on first engage) so the next engaged frame SNAPS to the current orientation instead of slerping
        // across the first-person gap, matching the orbit / eye-sync / FOV / collision smoothers.
        float basis_quat_x{0.0f};
        float basis_quat_y{0.0f};
        float basis_quat_z{0.0f};
        float basis_quat_w{1.0f};
        bool basis_quat_valid{false};

        // Turn-in-place pivot hold (render thread only). A native turn-in-place animation steps the body a little
        // (its root motion moves the entity 5-20 cm), and the pivot follows the entity origin, so without this the
        // camera shifts with every turn whose steps were not kept in place. turn_hold_x/y is the horizontal
        // displacement the body made while turning in place. It is subtracted from the body origin so the pivot stays
        // where it was, and eased back to zero once the player moves or the native turn stops. Normally the steps are
        // kept in place and this stays near zero. turn_hold_timer keeps absorbing briefly after the rotation stops,
        // for the step's settle. turn_track_* is the previous frame's body origin and yaw, and turn_track_step the
        // body's horizontal move since then (meters, 0 on the frame the tracker seeds). turn_track_valid is cleared on
        // suppression so the first engaged frame only seeds the tracker.
        float turn_hold_x{0.0f};
        float turn_hold_y{0.0f};
        float turn_hold_timer{0.0f};
        float turn_track_x{0.0f};
        float turn_track_y{0.0f};
        float turn_track_yaw{0.0f};
        float turn_track_step{0.0f};
        bool turn_track_valid{false};

        // Orbit-exclude policy and aim hand-off (render thread only, except orbit_hold_released). orbit_suspended is
        // true while the policy holds free-look off and will restore it. orbit_hold_released is posted by the
        // OrbitHoldKey release on the input thread before it turns free-look off, consumed by the policy's restore,
        // and cleared by every engage (see apply_orbit_exclude_policy). aim_handoff_requested is raised by the frustum
        // detour on the frame the policy turns free-look off in a situation where the game leaves the facing to the
        // player. rig_last_* describe the previous frame while the orbit rig shaped it: the camera's direction from
        // the pivot (azimuth and elevation, radians), its turn and rise off that frame's plain rig, and the look yaw
        // and pitch that put the plain rig there. While aim_handoff_active the camera holds the direction
        // aim_handoff_azimuth / aim_handoff_elevation, and the look and the body are written to aim_handoff_yaw /
        // aim_handoff_pitch until the look shows that heading. aim_handoff_seconds is the unpaused time since, and
        // aim_handoff_written is set once they have been written: the hand-off is not taken as done before that. It
        // ends without writing when a situation that sets the facing shows up (aim_handoff_cancel), when
        // aim_handoff_player changes or the body jumps, and when a look write fails. Cleared on suppression;
        // orbit_suspended belongs to the policy and is not.
        std::atomic<bool> orbit_suspended{false};
        std::atomic<bool> orbit_hold_released{false};
        bool aim_handoff_requested{false};
        bool rig_last_orbited{false};
        float rig_last_azimuth{0.0f};
        float rig_last_elevation{0.0f};
        float rig_last_turn{0.0f};
        float rig_last_rise{0.0f};
        float rig_last_yaw{0.0f};
        float rig_last_pitch{0.0f};
        bool aim_handoff_active{false};
        bool aim_handoff_written{false};
        bool aim_handoff_cancel{false};
        std::uintptr_t aim_handoff_player{0};
        float aim_handoff_azimuth{0.0f};
        float aim_handoff_elevation{0.0f};
        float aim_handoff_yaw{0.0f};
        float aim_handoff_pitch{0.0f};
        float aim_handoff_seconds{0.0f};
    };

    /**
     * @brief Rendered camera pose shared with the camera-space interaction hook.
     * @details The player use-cone (sub_180483740) is anchored on the EYE and never reads the render
     *          camera, so in third person the screen-centre crosshair and the use-target diverge by the
     *          shoulder offset (worst at close range - you cannot interact with what the crosshair is
     *          on). The frustum-builder detour publishes the final rendered camera position + crosshair
     *          direction here each engaged frame; the interactor eye-copy detour reads it to re-origin the
     *          cone onto the screen centre. A published pose is valid ONLY while the offset is engaged (in
     *          first person the camera IS the eye, so the hook leaves the game untouched).
     *
     *          The pose is published as a tear-free seqlock snapshot. The producer and consumer run at
     *          different points of the frame (and possibly on different threads), so reading the seven
     *          fields independently could mix a position from frame N with a direction from frame N+1 -
     *          an incoherent pose, not merely a stale one, which would aim the interaction ray at nothing.
     *          The seqlock makes every read observe one coherent frame or fail closed (no pose). A
     *          one-frame-stale but coherent pose is harmless for use-target selection.
     */
    class InteractionAimPose
    {
    public:
        /**
         * @brief Publishes a coherent pose snapshot (camera position + crosshair direction) as valid.
         * @details Seqlock writer: marks the sequence odd, writes the payload behind a release fence,
         *          then marks it even to publish. Single-producer (the frustum-builder detour is the only
         *          writer). Callback-safe: no allocation, no lock.
         */
        void store(float px, float py, float pz, float dx, float dy, float dz) noexcept;

        /**
         * @brief Reads a coherent pose snapshot.
         * @param px,py,pz Filled with the rendered camera position (the new cone origin).
         * @param dx,dy,dz Filled with the crosshair direction (the new scoring axis).
         * @return true and fills the out params when a valid tear-free pose was read; false when no pose
         *         is published or the read could not be confirmed tear-free (the out params are untouched).
         * @note Callback-safe: a bounded retry caps the seqlock spin and fails closed.
         */
        [[nodiscard]] bool load(float &px, float &py, float &pz, float &dx, float &dy, float &dz) const noexcept;

        /// Marks the published pose invalid (first person / offset suppressed).
        void invalidate() noexcept;

        /// Returns whether a valid pose is currently published (tear-free seqlock read of the flag).
        [[nodiscard]] bool is_valid() const noexcept;

    private:
        // Seqlock sequence: even = stable, odd = write in progress. The producer brackets its payload
        // writes between the odd and even transitions; a reader retries while it observes an odd or
        // changed sequence, so a partially written pose is never returned.
        std::atomic<std::uint32_t> m_seq{0};
        // Pose payload. The seqlock sequence provides coherence (no mixed frame) and the release/acquire
        // fences provide the publish ordering; the fields are relaxed atomics (not plain) so a producer
        // store racing a consumer load is well-defined rather than a data race, which keeps the seqlock
        // correct on any memory model, not just x86 TSO.
        std::atomic<float> m_pos_x{0.0f};
        std::atomic<float> m_pos_y{0.0f};
        std::atomic<float> m_pos_z{0.0f};
        std::atomic<float> m_dir_x{0.0f};
        std::atomic<float> m_dir_y{1.0f};
        std::atomic<float> m_dir_z{0.0f};
        std::atomic<bool> m_valid{false};
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
     *          until the game is actually in gameplay - when the window is at its final resolution -
     *          instead of snapshotting the transient loading-window size. Set by the camera detour
     *          (render thread), read by the overlay thread.
     */
    [[nodiscard]] std::atomic<bool> &game_world_ready() noexcept;

} // namespace TPVCamera

#endif // TPVCAMERA_GLOBAL_STATE_HPP
