/**
 * @file core/rig/rig_stage.hpp
 * @brief The camera rig of one game-view build: the follow distance and zoom, the stable basis, the FOV, the body
 *        anchor, the free-look orbit, the follow angle, the aim hand-off, camera-relative movement and the up-only arm,
 *        up to where the camera sits before collision.
 *
 * @details The rig reaches the engine only through a RigPort (LiveRigPort in hooks/camera_hook.cpp is the live one):
 *          the CView pose, the look controller, the body, the FOV the builder sees, the input, the look and body writes
 *          and the trace lines. Its carry-over is the RigState it is handed, the other threads' controls come in
 *          RigInput, and its settings are a RigParams read once per build. Engine-free, so a recorded frame replays
 *          exactly (core/rig/rig_record.hpp). Main thread only.
 */
#ifndef TPVCAMERA_CORE_RIG_RIG_STAGE_HPP
#define TPVCAMERA_CORE_RIG_RIG_STAGE_HPP

#include "core/blend/blend.hpp"
#include "core/input/move_input.hpp"
#include "core/math/vec.hpp"

#include <cstdint>
#include <optional>

namespace TPVCamera
{
    /** @brief The settings the rig reads, taken once per build (camera/rig_port.hpp, rig_params()). */
    struct RigParams
    {
        float follow_distance = 0.0f;
        float follow_distance_min = 0.0f;
        float follow_distance_max = 0.0f;
        float zoom_step = 0.0f;
        float offset_up = 0.0f;
        float eye_height = 0.0f;
        float offset_right = 0.0f;
        float follow_yaw = 0.0f;
        float follow_pitch = 0.0f;
        float lock_framing_angle = 0.0f;
        float lock_focus_height = 0.0f;
        float lock_focus_offset = 0.0f;
        float arm_follow_up = 0.0f;
        float lens_drop_up = 0.0f;
        float fov = 0.0f;
        float preset_blend_in_seconds = 0.0f;
        float aim_basis_smoothing = 0.0f;
        float gamepad_orbit_speed_x = 0.0f;
        float gamepad_orbit_speed_y = 0.0f;
        float gamepad_orbit_curve = 0.0f;
        float gamepad_orbit_smoothing = 0.0f;
        float orbit_pitch_min = 0.0f;
        float orbit_pitch_max = 0.0f;
        float orbit_return_speed = 0.0f;
        float orbit_follow_speed = 0.0f;
        float orbit_smoothing = 0.0f;
        bool dynamic_eye_sync = false;
        bool stable_aim_basis = false;
        bool orbit_level_aim = false;
        bool orbit_body_turn = false;
        bool orbit_continuous_align = false;
    };

    /**
     * @brief The rig's carry-over between builds: the zoom, the free-look orbit and its smoothers, camera-relative
     *        movement, the aim hand-off and the rig's own low-passes.
     * @details Main thread only: the camera frame, and between frames the input dispatcher (the free-look angles and
     * the
     *          stick latch) and the action dispatcher (orbit_move_pressed()), which run on the same thread. The *_valid
     *          flags are cleared when the view leaves third person, so the next engaged frame snaps its smoother instead
     *          of easing across the first-person gap.
     */
    struct RigState
    {
        // Zoom offset from the preset's base Follow Distance, driven by the zoom hold keys. Keeping zoom as a delta
        // from the base (rather than an absolute value) is what lets a preset edit apply live: the base is re-read
        // every frame and the rendered distance follows immediately, while the player's accumulated zoom is preserved
        // on top.
        float zoom_offset = 0.0f;

        // SwapShoulderKey's slide (CameraControls::shoulder_swapped): the linear 0..1 progress toward the swapped side,
        // ramped over k_shoulder_swap_seconds and smoothstepped at use. shoulder_swap_valid is cleared off third
        // person, so the next engaged frame takes the chosen side at once instead of sliding across the gap.
        float shoulder_swap = 0.0f;
        bool shoulder_swap_valid = false;

        // Free-look base. While orbit_base_held the orbit turns a held base, the follow rig for the look pitch
        // orbit_base_pitch (radians), instead of the live follow pose. orbit_base_offset is the held pitch less the
        // look's; it decays to 0 as free-look eases back after it turns off, and the hold ends once it has. Cleared off
        // third person unless a situation holds free-look off.
        bool orbit_base_held = false;
        float orbit_base_pitch = 0.0f;
        float orbit_base_offset = 0.0f;

        // Camera-relative movement. orbit_moving latches whether the character was moving last frame so the heading is
        // aligned to the camera ONCE on the idle -> moving edge, not re-aligned every frame. The move signal is the
        // device-agnostic action-input magnitude (the action dispatcher, via player_onaction_hook), captured the
        // instant a key is pressed - not body-position speed, which lags and reads zero against a wall.
        bool orbit_moving = false;
        // Camera-relative heading HOLD. A single write to the look-yaw is reverted by the engine, so on the idle ->
        // moving edge the camera-forward heading is captured into orbit_target_yaw and then HELD - written to the look
        // every frame while moving (like the pitch leveling, whose per-frame write sticks). This turns the body to face
        // the camera and keeps it there while you orbit freely; released when movement stops so the player resumes
        // control.
        float orbit_target_yaw = 0.0f;
        bool orbit_target_valid = false;
        // Orbit-input value snapshotted at move-capture. While moving, the camera holds its WORLD yaw and the body
        // turns UNDER it: the rig's orbit angle is derived as (orbit_target_yaw + (orbit_yaw - this) -
        // char_forward_yaw), so on the capture frame it equals the pre-move orbit (no snap) and eases to the user's
        // residual orbit as the body rotates to the heading - the camera never pops.
        float orbit_yaw_at_capture_deg = 0.0f;

        // Free-look orbit (degrees). While free-look is on (CameraControls::orbit_active), the input dispatcher hook
        // adds the mouse-look deltas to orbit_yaw / orbit_pitch and blocks the look events so the player aim stays put;
        // the rig circles the camera around the player by those angles and eases them back to centre on release.
        float orbit_yaw = 0.0f;
        float orbit_pitch = 0.0f;
        // The allowed range of orbit_pitch (degrees), set by the rig while free-look is on so OrbitPitchMin /
        // OrbitPitchMax stay measured from level whatever pitch the held base has, and the camera never goes past what
        // the rig can show; the input dispatcher hook clamps the mouse to it.
        float orbit_pitch_lo = -90.0f;
        float orbit_pitch_hi = 90.0f;

        // Gamepad right-stick look DEFLECTION (-1..1), latched by the input dispatcher hook while orbiting. The mouse
        // posts relative deltas straight into orbit_yaw / orbit_pitch per event; the analog stick instead reports a
        // HELD position, so it is latched here and integrated by rate (with the frame time) in the rig - holding the
        // stick keeps orbiting, frame-rate independent. Cleared to 0 when not orbiting so re-engaging with the stick
        // centred does not jump.
        float orbit_pad_yaw = 0.0f;
        float orbit_pad_pitch = 0.0f;
        // The latched deflection read through the preset's response curve and eased by its gamepad smoothing: the
        // deflection the orbit is turned by. Snapped to 0 whenever the latch is cleared.
        Spring<float> orbit_pad_yaw_eased{};
        Spring<float> orbit_pad_pitch_eased{};

        // Orbit angle low-pass. The rendered orbit yaw/pitch (degrees) ease toward the raw accumulated orbit_yaw /
        // orbit_pitch target each frame, so free-look is not as jittery as the raw per-frame mouse deltas: the engine
        // smooths native look DOWNSTREAM of the input dispatch the orbit hook captures and blocks, so without this
        // filter the orbit gets unsmoothed input. Strength is the per-preset orbit_smoothing.
        float orbit_yaw_render = 0.0f;
        float orbit_pitch_render = 0.0f;
        bool orbit_render_valid = false;

        // Continuous-align (GTA) steering low-pass. In continuous-align mode the camera follows the body/look heading
        // (the derived rig angle cancels to ~0), so the user's orbit-since-capture STEERS the run rather than orbiting
        // the rig. This is that steering angle (degrees) low-passed toward the raw input, fed to BOTH the body/look
        // heading and the rig derivation so the camera follows the smoothed steering and stays behind.
        // orbit_steer_valid is cleared when not move-orbiting so engaging snaps instead of easing across a stale value.
        // Only eased when OrbitContinuousAlign and OrbitSmoothing are both on.
        float orbit_steer_smooth = 0.0f;
        bool orbit_steer_valid = false;

        // Mount follow (follow_mount_heading()): the seconds since free-look input last moved the orbit, and the yaw
        // accumulator as the follow left it, whose change since tells input apart from the follow's own turn.
        // mount_follow_seen_valid is false while the follow is off, so it starts with no input seen and ramps in.
        float mount_follow_idle = 0.0f;
        float mount_follow_seen_yaw = 0.0f;
        bool mount_follow_seen_valid = false;

        // Orbit move-detection re-arm latch. A genuine sub-stop movement-input reading must be observed since orbit
        // engaged, or the game's own move input must show the movement too, before a move-start is honored, so a
        // stranded input latch (a held-move release swallowed on a combat action-map swap) cannot re-trip the
        // body-turn with the keys released. Cleared when orbit disengages, so re-engaging needs an observed release or
        // a held key the game's own move input shows. orbit_move_armed_by_game is set when that game input armed it:
        // such a move also ends once that input has stayed idle for k_game_move_stop_delay (counted in
        // orbit_move_game_idle), since another latch slot can still be stranded. Pairs with player_onaction_reset()
        // (which drops the stale latch outright on the suspend/disengage edges).
        bool orbit_move_armed = false;
        bool orbit_move_armed_by_game = false;
        float orbit_move_game_idle = 0.0f;
        // The reverse of a stranded latch: a key or stick held through player_onaction_reset() reads 0 in the latch, so
        // the game's own move input stands in for it (step_game_move_stand_in()).
        GameMoveStandIn orbit_move_stand_in{};

        // The rig's trace latches, so a suppressed stale move re-trip and the body-turn edges are traced once each.
        bool rig_stale_move_logged = false;
        bool rig_body_turn_logged = false;

        // Body anchor: the pivot's offset from the body origin (meters), eased toward the target the upper body sets
        // (run_rig): its lean in world x and y and its height. While the body turns in place the height target is
        // kept, and the lean (its target and both eased stages) turns with the body by the yaw it turned since
        // body_anchor_yaw, the body yaw of the last anchor update. body_anchor_valid cleared means the next engaged
        // frame snaps. real_eye_height is the eye height the upper body gives (the neck joint lifted to eye
        // level, or the first-person eye when the neck does not read). eye_sync_engaged says Dynamic Eye Sync moves the
        // anchor off Eye Height, and eye_sync_effective is the anchor height applied. The three reach the overlay
        // through RigReadout.
        Spring<float> body_anchor_lean_x{};
        Spring<float> body_anchor_lean_y{};
        float body_anchor_lean_target_x = 0.0f;
        float body_anchor_lean_target_y = 0.0f;
        Spring<float> body_anchor_up{};
        float body_anchor_up_target = 0.0f;
        float body_anchor_yaw = 0.0f;
        // Where the neck sits forward in the stance's idle (meters, body frame), the point the lean counts from, eased
        // from stance to stance so a crouch or a stand-up does not lunge the anchor.
        Spring<float> body_anchor_rest{};
        bool body_anchor_valid = false;
        float real_eye_height = 0.0f;
        bool eye_sync_engaged = false;
        float eye_sync_effective = 0.0f;

        // Per-preset FOV override ease: the preset blend's two-stage filter, whose applied stage is the eased rendered
        // FOV (radians) written into the render CCamera.
        Spring<float> fov_ease{};
        bool fov_ease_valid = false;

        // Aim-basis low-pass. The third-person rig basis quaternion (XYZW), low-passed toward the per-frame target (the
        // look-controller aim quat under StableAimBasis, else the eye quat) when AimBasisSmoothing is on, so
        // engine-driven view rotation - which the follow distance amplifies into a camera-position swing - is damped.
        float basis_quat_x = 0.0f;
        float basis_quat_y = 0.0f;
        float basis_quat_z = 0.0f;
        float basis_quat_w = 1.0f;
        bool basis_quat_valid = false;

        // Look snap hold. The game turns the look by itself at the start and the end of an action (10-74 deg in one
        // frame for a seat, a stand-up or a meal, and a 330 deg spin into a seat), and the rig built on the look would
        // carry the camera along the arc. Each frame's turn splits into the player's (the look input and the procedural
        // kick the controller added, LookReading) and the game's rest. look_snap_heading / look_snap_pitch (radians)
        // hold the game's part off the rendered look and ease it back out over k_look_snap_settle: a jump
        // (is_look_snap()) always, the rest while the player gives no input. The player's part always passes.
        // look_snap_last_* is the previous build's look (heading, pitch) and the game part's turn speed (rad/s), and
        // look_snap_last_valid says it was read. The hold drops with basis_quat_valid, so the rig takes a look the mod
        // turned itself as it is. look_input_idle counts the seconds since the player's look input last turned the
        // look.
        Spring<float> look_snap_heading{};
        Spring<float> look_snap_pitch{};
        float look_snap_last_heading = 0.0f;
        float look_snap_last_pitch = 0.0f;
        float look_snap_last_speed = 0.0f;
        bool look_snap_last_valid = false;
        float look_input_idle = 0.0f;
        // The player's raw look input, from the input dispatcher hook. raw_look_seen marks a mouse look delta since the
        // last build, raw_look_pad_* is the right stick's latched deflection, and raw_look_idle counts the seconds
        // since either showed input. Raw input that does not reach the look controller's input (look_input_idle) turns
        // the view another way (the combat lock-on's free look), so its turns cannot split and nothing is held.
        bool raw_look_seen = false;
        float raw_look_pad_heading = 0.0f;
        float raw_look_pad_pitch = 0.0f;
        float raw_look_idle = 0.0f;

        // Turn-in-place pivot hold. A native turn-in-place animation steps the body a little (its root motion moves the
        // entity 5-20 cm), and the pivot follows the entity origin, so without this the camera shifts with every turn
        // whose steps were not kept in place. turn_hold_x/y is the horizontal displacement the body made while turning
        // in place. It is subtracted from the body origin so the pivot stays where it was, and eased back to zero once
        // the player moves or the native turn stops. Normally the steps are kept in place and this stays near zero.
        // turn_hold_timer keeps absorbing briefly after the rotation stops, for the step's settle. turn_track_* is the
        // previous frame's body origin and yaw, and turn_track_step the body's horizontal move since then (meters, 0 on
        // the frame the tracker seeds); turn_track_valid cleared means the first engaged frame only seeds the tracker.
        float turn_hold_x = 0.0f;
        float turn_hold_y = 0.0f;
        float turn_hold_timer = 0.0f;
        float turn_track_x = 0.0f;
        float turn_track_y = 0.0f;
        float turn_track_yaw = 0.0f;
        float turn_track_step = 0.0f;
        bool turn_track_valid = false;

        // Aim hand-off. rig_last_* describe the previous frame while the orbit rig shaped it: the rig's look heading
        // and arm elevation (the negative of its look pitch), radians, its turn and rise off that frame's plain rig,
        // and the look yaw and pitch that put the plain rig there. While aim_handoff_active the rig holds the heading
        // and elevation aim_handoff_azimuth / aim_handoff_elevation, and the look and the body are written to
        // aim_handoff_yaw / aim_handoff_pitch until the look shows that heading. aim_handoff_seconds is the unpaused
        // time since, and aim_handoff_written is set once they are written: the hand-off is not taken as done before
        // that. It ends without a write when a situation that sets the facing shows up (aim_handoff_cancel, raised by
        // the frustum detour), when aim_handoff_player changes or the body jumps, and when a look write fails.
        // aim_handoff_opponent marks a hand-off toward the combat lock-on's opponent (RigInput::aim_handoff_heading)
        // rather than the camera's direction: it leaves the look pitch to the game and ends once the look is within
        // k_aim_handoff_opponent_tolerance of the heading, where the game's own steer takes over.
        // rig_last_azimuth / rig_last_elevation and the arm (rig_last_offset_*, the camera less the pivot) are kept on
        // every build (rig_last_built), the rest only while orbited.
        bool rig_last_built = false;
        float rig_last_offset_x = 0.0f;
        float rig_last_offset_y = 0.0f;
        float rig_last_offset_z = 0.0f;
        bool rig_last_orbited = false;
        float rig_last_azimuth = 0.0f;
        float rig_last_elevation = 0.0f;
        float rig_last_turn = 0.0f;
        float rig_last_rise = 0.0f;
        float rig_last_yaw = 0.0f;
        float rig_last_pitch = 0.0f;
        bool aim_handoff_active = false;
        bool aim_handoff_written = false;
        bool aim_handoff_cancel = false;
        bool aim_handoff_opponent = false;
        // RigInput::game_owns_facing with free-look on: the rig basis yaw of the previous build, whose turns are taken
        // out of the free-look yaw so the camera keeps its world heading (facing_hold_valid: the last build held).
        bool facing_hold_valid = false;
        float facing_hold_last_yaw = 0.0f;
        // Combat lock framing (RigInput::lock_focus): its 0..1 ramp, and the focus point it aims at, kept after the
        // lock ends so the ease out still aims there (lock_frame_focus_valid: one has been taken since the ramp was
        // last 0).
        float lock_frame_weight = 0.0f;
        bool lock_frame_focus_valid = false;
        float lock_frame_focus_x = 0.0f;
        float lock_frame_focus_y = 0.0f;
        float lock_frame_focus_z = 0.0f;
        // The focus as RigInput::lock_focus gave it last build, and a pan from lock_frame_from_* to it when the lock
        // moves to another opponent (lock_frame_switch: 0..1 ramp, 1 = done).
        float lock_frame_raw_x = 0.0f;
        float lock_frame_raw_y = 0.0f;
        float lock_frame_raw_z = 0.0f;
        float lock_frame_from_x = 0.0f;
        float lock_frame_from_y = 0.0f;
        float lock_frame_from_z = 0.0f;
        float lock_frame_switch = 1.0f;
        // The camera the framing eases in from (the last frame's arm and look, lock_frame_entering while it does).
        bool lock_frame_entering = false;
        float lock_frame_entry_x = 0.0f;
        float lock_frame_entry_y = 0.0f;
        float lock_frame_entry_z = 0.0f;
        float lock_frame_entry_yaw = 0.0f;
        float lock_frame_entry_pitch = 0.0f;
        std::uintptr_t aim_handoff_player = 0;
        float aim_handoff_azimuth = 0.0f;
        float aim_handoff_elevation = 0.0f;
        float aim_handoff_yaw = 0.0f;
        float aim_handoff_pitch = 0.0f;
        float aim_handoff_seconds = 0.0f;
    };

    /** @brief One build's input to the rig: the frame, and the controls the frustum detour read once for it. */
    struct RigInput
    {
        std::uintptr_t c_player = 0; // the live C_Player, or 0: the rig then anchors to the eye
        float view_blend = 0.0f;     // smoothstepped, 0 first person .. 1 third person
        float seconds = 0.0f;        // the build's step (FrameStep::seconds)
        bool repeat_build = false;
        bool paused = false;
        bool orbit_active = false;     // free-look is on (CameraControls::orbit_active)
        bool shoulder_swapped = false; // the camera sits over the other shoulder (CameraControls::shoulder_swapped)
        bool zoom_reset = false;       // a zoom reset was asked for since the last build (taken from CameraControls)
        bool orbit_suspended = false;  // the mod, not the player, turned free-look off (the orbit-exclude policy, or a
                                       // kill's own free-look ending)
        bool aim_handoff_requested =
            false; // the policy turned free-look off this frame where the player owns the facing
        // With aim_handoff_requested: the heading the look and the body turn to instead of the camera's direction (the
        // combat lock-on's opponent, as seen from the player), a world yaw in radians (forward = (-sin, cos)).
        std::optional<float> aim_handoff_heading{};
        // The game still sets the player's facing although free-look is on (a combat stance held on a dead opponent
        // through the finishing animation): free-look turns the camera only, and a movement key does not turn the body.
        bool game_owns_facing = false;
        // The feet of the living opponent the combat stance is locked on (not while aiming): the lock framing aims
        // LockFocusHeight above them.
        std::optional<Vector3> lock_focus{};
        // The lock-on's free look is on (the player turns the view with the mouse or the stick, the steer paused).
        bool lock_free_look = false;
        // The input dispatcher hook is live, so RigState::raw_look_seen and the stick latch report the player's raw
        // look input.
        bool raw_look_known = false;
        // The game's stance is crouched. The body anchor's lean then counts from the neck of the crouched idle.
        bool crouched = false;
        // The player's last crouched clip started from the NPC set, which the crouched animation hook swaps in, rather
        // than his own (crouched_set_is_npc).
        bool crouched_npc_set = false;
        // The game's focused look turns the view (GameState::Focus). It turns the look by the rendered camera, so the
        // basis takes the look as it is: no look snap hold and no AimBasisSmoothing.
        bool focused_look = false;
    };

    /** @brief The zoom keys held this build. */
    struct ZoomHeld
    {
        bool in = false;
        bool out = false;
    };

    /** @brief The untouched first-person eye pose of the game view. */
    struct EyePose
    {
        Vector3 position{};
        Quaternion rotation{};
    };

    /**
     * @brief The player's look controller as read: its orientation and its scalar pitch, not yet checked, and this
     *        frame's turns of the look the controller added from the player's look input and from its procedural kick
     *        (radians). The rest of a frame's turn is the game's own.
     */
    struct LookReading
    {
        Quaternion quat{};
        float pitch = 0.0f;
        float input_pitch = 0.0f;
        float input_yaw = 0.0f;
        float kick_pitch = 0.0f;
        float kick_yaw = 0.0f;
    };

    /** @brief The player's body: its entity, and its world origin and heading when its matrix read. */
    struct BodyReading
    {
        std::uintptr_t entity = 0;
        Vector3 origin{};
        float yaw = 0.0f;
        bool valid = false;
    };

    /** @brief The rig's trace lines; RigPort::event() formats each (camera/rig_port.cpp). */
    enum class RigEvent : std::uint8_t
    {
        HandoffStart,        // a: turn, b: look pitch, c: target pitch (degrees)
        HandoffTurned,       // a: seconds
        HandoffLost,         //
        HandoffMoved,        //
        HandoffTimeout,      // a: heading gap (degrees), b: seconds
        HandoffWriteFailed,  //
        MoveArmedByGame,     // a: magnitude
        MoveStandInStarted,  // a: the game's move input, b: the latch
        MoveStart,           // a: magnitude, b: continuous align (0 or 1), mask: game state
        MoveStaleSuppressed, // a: magnitude
        MoveStop,            // a: magnitude
        MoveStopGameIdle,    // a: magnitude
        BodyTurnEngaged,     //
        BodyTurnReleased,    //
        HandoffToOpponent,   // a: look turn to the opponent, b: camera turn off the look (degrees)
        LookSnapHeld,        // the game's part of the turn - a: heading, b: pitch (degrees), c: speed (deg/s)
    };

    /** @brief The engine as the rig sees it (see the file details). */
    class RigPort
    {
    public:
        RigPort() = default;
        RigPort(const RigPort &) = delete;
        RigPort &operator=(const RigPort &) = delete;
        RigPort(RigPort &&) = delete;
        RigPort &operator=(RigPort &&) = delete;
        virtual ~RigPort() = default;

        /** @brief Which zoom keys are held. */
        [[nodiscard]] virtual ZoomHeld zoom_held() = 0;
        /** @brief Hands a reset zoom offset to the preset ease, which eases it out (Presets::ease_out_zoom_offset). */
        virtual void ease_out_zoom(float offset) = 0;
        /** @brief The CView's eye pose, or std::nullopt when it does not read. */
        [[nodiscard]] virtual std::optional<EyePose> eye_pose() = 0;
        /** @brief The look controller of @p c_player, or std::nullopt when it does not read. */
        [[nodiscard]] virtual std::optional<LookReading> look(std::uintptr_t c_player) = 0;
        /** @brief The FOV SetFrustum wrote into the camera for this build (radians). */
        [[nodiscard]] virtual float game_fov() = 0;
        /**
         * @brief Writes @p fov (radians) into the camera and scales its cull-frustum terms by @p ratio, which is
         *        tan(fov / 2) / tan(game_fov / 2).
         */
        virtual void override_fov(float fov, float ratio) = 0;
        /** @brief The body of @p c_player. */
        [[nodiscard]] virtual BodyReading body(std::uintptr_t c_player) = 0;
        /**
         * @brief The neck joint of @p entity's character in its model space (x right, y forward, z up, meters from the
         *        entity's origin), or std::nullopt when it does not read.
         */
        [[nodiscard]] virtual std::optional<Vector3> neck(std::uintptr_t entity) = 0;
        /** @brief Whether a native turn-in-place step is under way (native_turn_engaged()). */
        [[nodiscard]] virtual bool native_turn() = 0;
        /** @brief The action dispatcher's movement-input magnitude, or std::nullopt when its hook did not resolve. */
        [[nodiscard]] virtual std::optional<float> move_input() = 0;
        /** @brief The game's own movement input for @p c_player, or std::nullopt (player_move_input()). */
        [[nodiscard]] virtual std::optional<float> game_move(std::uintptr_t c_player) = 0;
        /** @brief Whether the game shows the OS cursor (a UI is up). */
        [[nodiscard]] virtual bool cursor_shown() = 0;
        /** @brief The debounced game state mask. */
        [[nodiscard]] virtual std::uint32_t game_state() = 0;
        /**
         * @brief Eases the look pitch toward @p pitch_target by @p pitch_ease and, with @p set_yaw, sets the look yaw
         *        (apply_orbit_aim_control()).
         * @return False when a requested write did not land.
         */
        [[nodiscard]] virtual bool aim_control(float pitch_ease, float pitch_target, bool set_yaw, float yaw) = 0;
        /** @brief Turns the body to @p yaw for this frame (apply_orbit_body_turn()). */
        virtual void body_turn(float yaw) = 0;
        /** @brief Publishes the heading a move press takes, with the body yaw now (orbit_move_pressed()). */
        virtual void publish_move_press(float heading, float body_yaw) = 0;
        /** @brief Withdraws the move-press heading. */
        virtual void withdraw_move_press() = 0;
        /** @brief A trace line (see RigEvent for the arguments). */
        virtual void event(RigEvent event, float a, float b, float c, std::uint32_t mask) = 0;
    };

    /** @brief What the rig hands the later stages: the camera before collision and what it was built from. */
    struct RigFrame
    {
        // The untouched first-person eye: the view-blend source.
        Vector3 eye_position{};
        Vector3 eye_forward{};
        Vector3 eye_up{};
        // The rig basis forward: the stable look, or the eye's.
        Vector3 forward{};
        // The rig's look and up, after the follow angle and the orbit turned them.
        Vector3 look_forward{};
        Vector3 look_up{};
        // The rig's look before the lock framing turned it: the lock-on's target pick reads it while lock_framed and
        // the player turns the view (hooks/combat_pick.hpp).
        Vector3 pick_forward{};
        // Where the rig puts the camera, before collision.
        Vector3 camera_position{};
        Vector3 pivot{};
        // The head point (anchor_base) and the de-bobbed body origin less the turn hold (anchor_origin).
        Vector3 anchor_base{};
        Vector3 anchor_origin{};
        Vector3 turn_hold{};
        Vector3 body_origin{};
        std::uintptr_t entity = 0;
        // The shoulder offset after SwapShoulderKey's slide (meters, negative = left).
        float offset_right = 0.0f;
        bool body_valid = false;
        // The lock framing turned the camera this build.
        bool lock_framed = false;
        // The rendered orientation is rebuilt from look_forward / look_up: the basis was shaped, or the follow angle or
        // the orbit turned the look.
        bool rebuild_basis = false;
    };

    /**
     * @brief Builds the rig for this build.
     * @return False when the CView pose did not read: the caller leaves the view untouched this build. The zoom has
     *         advanced by then.
     */
    [[nodiscard]] bool run_rig(RigState &state, const RigParams &params, const RigInput &in, RigPort &port,
                               RigFrame &out);
} // namespace TPVCamera

#endif // TPVCAMERA_CORE_RIG_RIG_STAGE_HPP
