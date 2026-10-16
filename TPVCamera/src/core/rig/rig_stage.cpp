/**
 * @file core/rig/rig_stage.cpp
 * @brief The camera rig of one game-view build (see rig_stage.hpp).
 */

#include "core/rig/rig_stage.hpp"
#include "core/blend/blend.hpp"
#include "core/input/move_input.hpp"
#include "core/input/stick.hpp"
#include "core/math/angle.hpp"
#include "core/rig/rig_math.hpp"
#include "game_state.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace TPVCamera
{
    namespace
    {
        // A move armed by the game's own move input (see the re-arm guard) also ends once that input has stayed idle
        // this long, which covers the game's few frames of lag behind the keys.
        constexpr float k_orbit_move_game_stop_delay = 0.1f; // seconds
        // Eases the look pitch toward level per second while orbiting (OrbitLevelAim).
        constexpr float k_orbit_aim_level_speed = 8.0f;

        // Orbit angle low-pass. The engine smooths native look DOWNSTREAM of the input dispatch the orbit
        // hook captures and blocks, so free-look applies the raw per-frame mouse deltas and reads as jittery.
        // orbit_smoothing (0..1) maps to a frame-rate-independent catch-up speed between these bounds: higher
        // smoothing -> lower speed -> more lag (smoother); 0 disables the filter (snap to the raw target).
        constexpr float k_orbit_smooth_min_speed = 6.0f;  // strength 1.0: heavy smoothing
        constexpr float k_orbit_smooth_max_speed = 40.0f; // strength near 0: barely-there smoothing

        // Aim hand-off: the least turn or rise off the plain rig worth a hand-off, and the heading gap at which the
        // look counts as turned; how long it waits for the look; the body step between two frames that counts as a
        // load or a teleport rather than a run or a hitch; and the bound on the look pitch it writes.
        constexpr float k_aim_handoff_min_angle = 0.0175f; // rad (1 deg)
        constexpr float k_aim_handoff_timeout = 0.5f;      // seconds
        constexpr float k_aim_handoff_jump = 3.0f;         // meters per frame
        constexpr float k_aim_handoff_max_pitch = 1.4f;    // rad (80 deg)

        // Aim-basis low-pass (AimBasisSmoothing). Maps the 0..1 strength to a frame-rate-independent slerp
        // catch-up speed: higher strength -> lower speed -> more damping (and slightly laggier aim). The window
        // is faster than the orbit low-pass so even at full strength the camera still tracks genuine aim turns
        // closely while shedding the high-frequency view-shake.
        constexpr float k_basis_smooth_min_speed = 8.0f;  // strength 1.0: heaviest damping
        constexpr float k_basis_smooth_max_speed = 60.0f; // strength near 0: barely-there damping

        // SwapShoulderKey: seconds the camera takes to slide over to the other shoulder. A linear ramp read through a
        // smoothstep, so a press starts and lands without a kick, and a second press mid-slide turns back from where
        // the camera is.
        constexpr float k_shoulder_swap_seconds = 0.3f;

        // Turn-in-place pivot hold tuning. A native turn step is the body rotating TOWARD the look (faster than
        // k_turn_hold_yaw_rate) while it trails the look by more than k_turn_hold_min_gap, moving no faster than
        // k_turn_hold_max_speed and with no movement input. Its displacement, and that of the step's foot settle for
        // k_turn_hold_settle after the rotation stops, is held off the pivot. A body that moves faster than
        // k_turn_hold_follow_speed without being held is really moving, and the hold eases back onto it at
        // k_turn_hold_release_rate. A per-frame jump beyond k_turn_hold_teleport is a teleport or a load, not a step,
        // and drops the hold. The hold never exceeds k_turn_hold_max. With the turn steps kept in place
        // (detour_phys_ent_movement) there is normally nothing to hold; this keeps the camera still when that hook is
        // unavailable.
        constexpr float k_turn_hold_yaw_rate = 0.26f;    // rad/s (15 deg/s)
        constexpr float k_turn_hold_min_gap = 0.17f;     // rad (10 deg)
        constexpr float k_turn_hold_max_speed = 1.2f;    // m/s
        constexpr float k_turn_hold_follow_speed = 0.3f; // m/s
        constexpr float k_turn_hold_settle = 0.35f;      // seconds
        constexpr float k_turn_hold_teleport = 0.25f;    // meters per frame
        constexpr float k_turn_hold_max = 0.5f;          // meters
        constexpr float k_turn_hold_release_rate = 8.0f; // 1/s

        /**
         * @brief Keeps the pivot still while the body turns in place, returning the horizontal offset to hold off it.
         * @details A native turn-in-place animation carries root motion: the body steps 5-20 cm while it turns, and the
         *          rig anchors to the body, so the camera shifts with every turn unless the step is held off. The
         *          displacement of a turn step is absorbed into a hold the caller subtracts from its anchor, so the
         *          camera stays where it was during and after the turn. A turn step is recognised by its signature: the
         *          body rotates toward the look it trails, at step speed, with no movement input. Running to a stop, a
         *          scripted move or a push does not match it, so the camera follows them, and any such movement also
         *          eases the hold back onto the body. Standing still keeps the hold. Without the look the rotation
         *          alone is used. Normally the steps are kept in place (detour_phys_ent_movement) and the hold stays
         *          near zero; it is the fallback for when that hook is unavailable.
         * @param cam Camera state holding the tracker and the hold.
         * @param move_signal The action dispatcher's movement-input magnitude (RigPort::move_input()).
         * @param body_origin The entity world origin this frame.
         * @param body_yaw The entity world yaw this frame (radians).
         * @param look_yaw The look yaw this frame (radians), used only when @p look_valid.
         * @param look_valid Whether @p look_yaw was read.
         * @param delta_time Frame time in seconds.
         * @return The horizontal offset (z = 0) to subtract from the anchor.
         */
        Vector3 hold_turn_in_place_pivot(CameraState &cam, RigPort &port, const std::optional<float> &move_signal,
                                         const Vector3 &body_origin, float body_yaw, float look_yaw, bool look_valid,
                                         float delta_time)
        {
            if (!cam.turn_track_valid)
            {
                cam.turn_track_x = body_origin.x;
                cam.turn_track_y = body_origin.y;
                cam.turn_track_yaw = body_yaw;
                cam.turn_track_valid = true;
                cam.turn_track_step = 0.0f;
                cam.turn_hold_x = 0.0f;
                cam.turn_hold_y = 0.0f;
                cam.turn_hold_timer = 0.0f;
                return Vector3{0.0f, 0.0f, 0.0f};
            }
            const float step_x = body_origin.x - cam.turn_track_x;
            const float step_y = body_origin.y - cam.turn_track_y;
            const float step = std::sqrt(step_x * step_x + step_y * step_y);
            cam.turn_track_step = step;
            const float turn = std::remainder(body_yaw - cam.turn_track_yaw, 2.0f * DirectX::XM_PI);
            cam.turn_track_x = body_origin.x;
            cam.turn_track_y = body_origin.y;
            cam.turn_track_yaw = body_yaw;
            if (step > k_turn_hold_teleport)
            {
                cam.turn_hold_x = 0.0f;
                cam.turn_hold_y = 0.0f;
                cam.turn_hold_timer = 0.0f;
                return Vector3{0.0f, 0.0f, 0.0f};
            }

            const float speed = delta_time > 1e-4f ? step / delta_time : 0.0f;
            const bool native_turn = port.native_turn();
            const bool move_input = !move_signal.has_value() || *move_signal > k_move_input_stop;
            const bool in_place = native_turn && !move_input && speed < k_turn_hold_max_speed;
            bool toward_look = true;
            if (look_valid)
            {
                const float gap = std::remainder(look_yaw - body_yaw, 2.0f * DirectX::XM_PI);
                toward_look = std::fabs(gap) > k_turn_hold_min_gap && turn * gap > 0.0f;
            }
            if (in_place && toward_look && std::fabs(turn) > k_turn_hold_yaw_rate * delta_time)
            {
                cam.turn_hold_timer = k_turn_hold_settle;
            }
            else
            {
                cam.turn_hold_timer = std::max(0.0f, cam.turn_hold_timer - delta_time);
            }

            if (in_place && cam.turn_hold_timer > 0.0f)
            {
                cam.turn_hold_x += step_x;
                cam.turn_hold_y += step_y;
            }
            else if (!native_turn || speed > k_turn_hold_follow_speed)
            {
                // Real movement (or the feature off). Keyed on the body's speed rather than the movement-input latch,
                // which a combat action-map swap can strand above zero with the keys up.
                const float keep = std::exp(-k_turn_hold_release_rate * delta_time);
                cam.turn_hold_x *= keep;
                cam.turn_hold_y *= keep;
            }
            const float held = std::sqrt(cam.turn_hold_x * cam.turn_hold_x + cam.turn_hold_y * cam.turn_hold_y);
            if (held > k_turn_hold_max)
            {
                const float scale = k_turn_hold_max / held;
                cam.turn_hold_x *= scale;
                cam.turn_hold_y *= scale;
            }
            return Vector3{cam.turn_hold_x, cam.turn_hold_y, 0.0f};
        }

        // Mount follow (OrbitFollowSpeed): how long the follow takes to come back once free-look input ends, and the
        // smallest change of the free-look yaw accumulator (degrees) that counts as input.
        constexpr float k_mount_follow_ramp_seconds = 2.0f;
        constexpr float k_mount_follow_input_deg = 1e-3f;

        /**
         * @brief Turns free-look back behind the mount while riding (OrbitFollowSpeed), a flex of the rule that
         *        free-look stays where the player left it.
         * @details Riding a horse or a cart with free-look on, the camera's heading eases toward the mount's (the
         *          rider's body yaw) by OrbitReturnSpeed x min(step / OrbitFollowSpeed, dt) per frame, where step is
         *          the body's horizontal move since the last frame: at OrbitReturnSpeed once the mount covers
         *          OrbitFollowSpeed meters a second, proportionally slower below that, never while it stands, and not
         *          on the frame's second build. Free-look input (the mouse or the stick moving the orbit) stops it at
         *          once; it comes back over k_mount_follow_ramp_seconds (smoothstep) after the input ends, and ramps in
         *          the same way when it turns on. With a captured heading (camera-relative movement) the heading turns,
         *          and the look and the body written to it with it; otherwise the orbit angle turns. Main thread only.
         * @param forward_yaw The rig's forward yaw this frame (radians).
         * @param body_yaw The rider's body yaw (radians), the mount's heading.
         * @param active Free-look is on, a horse or a cart is ridden, OrbitFollowSpeed is set and the body was read.
         * @param move_orbit Moving with a captured heading.
         * @param orbit_yaw_deg The frame's orbit yaw (degrees), turned in place.
         */
        void follow_mount_heading(CameraState &cam, const RigParams &params, float forward_yaw, float body_yaw,
                                  bool active, bool move_orbit, float delta_time, float &orbit_yaw_deg)
        {
            const float follow_speed = params.orbit_follow_speed;
            if (!active || !(follow_speed > 0.0f))
            {
                cam.mount_follow_seen_valid = false;
                cam.mount_follow_idle = 0.0f;
                return;
            }
            const float accumulator = cam.orbit_yaw.load(std::memory_order_relaxed);
            const bool input = (cam.mount_follow_seen_valid &&
                                std::fabs(accumulator - cam.mount_follow_seen_yaw) > k_mount_follow_input_deg) ||
                               cam.orbit_pad_yaw.load(std::memory_order_relaxed) != 0.0f ||
                               cam.orbit_pad_pitch.load(std::memory_order_relaxed) != 0.0f;
            cam.mount_follow_idle = input ? 0.0f : cam.mount_follow_idle + delta_time;
            cam.mount_follow_seen_yaw = accumulator;
            cam.mount_follow_seen_valid = true;

            const float ramp = smoothstep(std::clamp(cam.mount_follow_idle / k_mount_follow_ramp_seconds, 0.0f, 1.0f));
            const float rate =
                params.orbit_return_speed * std::min(cam.turn_track_step / follow_speed, delta_time) * ramp;
            if (!(rate > 0.0f))
            {
                return;
            }
            const float camera_yaw = forward_yaw + degrees_to_radians(orbit_yaw_deg);
            const float turn = std::remainder(body_yaw - camera_yaw, 2.0f * DirectX::XM_PI) * (1.0f - std::exp(-rate));
            const float turn_deg = radians_to_degrees(turn);
            if (move_orbit)
            {
                cam.orbit_target_yaw += turn;
            }
            else
            {
                // The input thread adds the mouse's deltas to the same accumulator, so the turn is added atomically
                // too, and what the follow saw is its own result: a delta landing in between shows as input next frame.
                cam.mount_follow_seen_yaw = cam.orbit_yaw.fetch_add(turn_deg, std::memory_order_relaxed) + turn_deg;
            }
            orbit_yaw_deg = std::remainder(orbit_yaw_deg + turn_deg, 360.0f);
        }
    } // namespace

    bool run_rig(CameraState &cam, const RigParams &params, const RigInput &in, RigPort &port, RigFrame &out)
    {
        const float delta_time = in.seconds;
        const std::uintptr_t c_player = in.c_player;

        // Follow distance = configured base (hot-reloadable INI FollowDistance, re-read every
        // frame so an edit applies live) plus the accumulated zoom offset from the hold
        // keys, clamped to the configured window.
        const ZoomHeld zoom_held = port.zoom_held();
        float zoom_offset = cam.zoom_offset.load(std::memory_order_relaxed);
        const float zoom_step = params.zoom_step;
        if (zoom_held.in)
        {
            zoom_offset -= zoom_step * delta_time; // zoom in pulls the camera closer
        }
        if (zoom_held.out)
        {
            zoom_offset += zoom_step * delta_time;
        }
        const float base_distance = params.follow_distance;
        // A preset can carry Min above Max, and std::clamp is undefined when its upper bound is below the lower.
        const float distance_min = params.follow_distance_min;
        const float distance_max = std::max(distance_min, params.follow_distance_max);
        const float distance = std::clamp(base_distance + zoom_offset, distance_min, distance_max);
        // A requested reset keeps this frame's distance and hands the clamped offset to the preset ease, which
        // carries it from the next frame on and eases it out.
        if (cam.zoom_reset_requested.exchange(false, std::memory_order_relaxed))
        {
            port.ease_out_zoom(distance - base_distance);
            cam.zoom_offset.store(0.0f, std::memory_order_relaxed);
        }
        else
        {
            cam.zoom_offset.store(distance - base_distance, std::memory_order_relaxed);
        }

        // The eye anchor and the orientation quat of the untouched CView pose. The basis is derived from the quat
        // (CryEngine column-vector convention: right = quat * +X, forward = quat * +Y, up = quat * +Z). Reading the
        // quat rather than the matrix columns keeps the basis idempotent across the frustum builder's same-frame
        // rebuilds (the matrix is overwritten, never the pose, so the pose stays the clean eye input).
        const std::optional<EyePose> eye = port.eye_pose();
        if (!eye.has_value())
        {
            return false; // the CView pose did not read; leave the view untouched this frame
        }
        const Vector3 eye_position = eye->position;
        const Quaternion eye_rotation = eye->rotation;
        // FPV-side basis (the engine eye orientation). Kept as the view-blend SOURCE so view_blend 0 leaves
        // the untouched first-person view; the third-person rig basis below is built from a STABLE source.
        const Vector3 eye_forward = eye_rotation.rotate(Vector3{0.0f, 1.0f, 0.0f});
        const Vector3 eye_up = eye_rotation.rotate(Vector3{0.0f, 0.0f, 1.0f});

        // Stable rig basis. The third-person rig (camera = pivot - forward * distance) amplifies any rotation of
        // the basis into a position swing. The EyeHeight body anchor removes the POSITIONAL bob, and this removes
        // the ROTATIONAL component the engine bakes into the eye quat during animations (head-bob and weapon-sway
        // rotation, combat / hit / landing shake). StableAimBasis follows the player's clean look-controller AIM
        // instead, leveled by level_look_rotation. That aim carries none of the engine view-shake and equals the eye
        // quat at rest. AimBasisSmoothing low-passes it. basis_overridden records whether the basis was shaped at
        // all. When neither feature is active the basis stays the raw eye quat, and the orientation path below is
        // byte-identical to the follow-angle/orbit-only behavior (a fail-safe default).
        Quaternion basis_rotation = eye_rotation;
        bool basis_overridden = false;
        Quaternion look_rotation = eye_rotation;
        bool look_valid = false;
        if (params.stable_aim_basis && c_player != 0)
        {
            // The look controller the orbit aim control writes, so the basis tracks the same look.
            const std::optional<LookReading> look = port.look(c_player);
            if (look.has_value())
            {
                const Quaternion q = look->quat;
                const float n2 = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
                // Accept only a finite, ~unit quaternion and a finite pitch within the engine's +/-1.5608 rad look
                // clamp, plus a small margin. A layout drift that feeds garbage falls back to the eye quat and never
                // rotates the rig by a denormal.
                if (std::isfinite(n2) && n2 > 0.9f && n2 < 1.1f && std::isfinite(look->pitch) &&
                    std::fabs(look->pitch) <= 1.6f)
                {
                    look_rotation = level_look_rotation(q, look->pitch);
                    look_valid = true;
                }
            }
        }
        // Low-pass the basis quaternion toward the target: the leveled look orientation when StableAimBasis resolved
        // it, else the raw eye quat. Snaps on the first engaged frame (basis_quat_valid false) so the camera does not
        // swing in from a stale orientation across a first-person gap. AimBasisSmoothing 0 disables the filter
        // (the target is used directly).
        const float basis_smoothing = std::clamp(params.aim_basis_smoothing, 0.0f, 1.0f);
        if (look_valid || basis_smoothing > 1e-4f)
        {
            basis_overridden = true;
            const DirectX::XMVECTOR target = (look_valid ? look_rotation : eye_rotation).to_xm_vector();
            DirectX::XMVECTOR result;
            if (!cam.basis_quat_valid)
            {
                result = target; // snap on the first engaged frame so the camera does not swing in from stale
                cam.basis_quat_valid = true;
            }
            else if (basis_smoothing > 1e-4f)
            {
                const DirectX::XMVECTOR current =
                    DirectX::XMVectorSet(cam.basis_quat_x, cam.basis_quat_y, cam.basis_quat_z, cam.basis_quat_w);
                const float speed =
                    k_basis_smooth_max_speed - basis_smoothing * (k_basis_smooth_max_speed - k_basis_smooth_min_speed);
                const float t = 1.0f - std::exp(-speed * delta_time);
                // XMQuaternionSlerp takes the shortest arc, so a quaternion double-cover sign flip never spins the
                // basis the long way.
                result = DirectX::XMQuaternionNormalize(DirectX::XMQuaternionSlerp(current, target, t));
            }
            else
            {
                result = target; // StableAimBasis on, smoothing off: use the look target directly
            }
            cam.basis_quat_x = DirectX::XMVectorGetX(result);
            cam.basis_quat_y = DirectX::XMVectorGetY(result);
            cam.basis_quat_z = DirectX::XMVectorGetZ(result);
            cam.basis_quat_w = DirectX::XMVectorGetW(result);
            basis_rotation = Quaternion{cam.basis_quat_x, cam.basis_quat_y, cam.basis_quat_z, cam.basis_quat_w};
        }
        else
        {
            cam.basis_quat_valid = false; // basis not shaped: the next engage snaps the low-pass
        }

        const Vector3 right = basis_rotation.rotate(Vector3{1.0f, 0.0f, 0.0f});
        const Vector3 forward = basis_rotation.rotate(Vector3{0.0f, 1.0f, 0.0f});
        // The look pitch and heading the rig is built from (engine convention: forward = (-sin yaw, cos yaw)).
        const float forward_pitch = std::asin(std::clamp(forward.z, -1.0f, 1.0f));
        const float forward_yaw = std::atan2(-forward.x, forward.y);
        const Vector3 up = basis_rotation.rotate(Vector3{0.0f, 0.0f, 1.0f});
        // Per-preset FOV, smoothly crossing the "off" boundary. The game FOV is the one SetFrustum wrote for this build
        // (RigPort::game_fov()). params.fov is the per-preset TARGET in degrees (0 = use the game FOV). A rendered FOV
        // eases toward (target>0 ? target : game FOV) with a 2-stage critically-damped ease at the preset-blend rate,
        // so turning a preset's FOV on/off GLIDES through the game FOV instead of snapping across 0. It is written (the
        // frustum terms rescaled by tan(new/2)/tan(game/2) so culling matches) only while a preset wants a FOV or is
        // still easing back; once settled at the game FOV this passes through, so the game's own dynamic FOV keeps
        // working. TPV only.
        const float game_fov = port.game_fov(); // radians
        if (in.view_blend > 0.0f && game_fov > 0.05f && game_fov < 3.0f)
        {
            const float fov_target_degrees = params.fov;
            const float desired_fov = (fov_target_degrees > 0.0f) ? degrees_to_radians(fov_target_degrees) : game_fov;
            const float blend_seconds = params.preset_blend_in_seconds;
            if (!cam.fov_ease_valid) // first engaged frame after a first-person gap: snap, no ease across the gap
            {
                cam.fov_ease.snap(desired_fov);
                cam.fov_ease_valid = true;
            }
            else
            {
                // The preset blend's filter in the active preset's blend time; 0 snaps.
                cam.fov_ease.step(desired_fov, spring_alpha(blend_seconds, delta_time));
            }
            // Override while a preset wants a FOV or while still easing back toward the game FOV; otherwise pass
            // through (preset off + settled) so the game keeps its own FOV (including any dynamic FOV).
            if ((fov_target_degrees > 0.0f || std::fabs(cam.fov_ease.applied - game_fov) > 0.005f) &&
                cam.fov_ease.applied > 0.05f && cam.fov_ease.applied < 3.0f)
            {
                const float ratio = std::tan(cam.fov_ease.applied * 0.5f) / std::tan(game_fov * 0.5f);
                port.override_fov(cam.fov_ease.applied, ratio);
            }
        }
        // Over-the-shoulder lateral offset along the eye-right axis, applied to the non-orbit follow
        // pose. During free-look it is folded into the orbit start offset (below) so it revolves with
        // the ring, which keeps engaging free-look continuous. SwapShoulderKey mirrors it to the other side: the
        // offset is scaled from +1 to -1 through the smoothstepped swap progress, so it passes through the look line
        // instead of jumping across it. The first engaged frame takes the chosen side at once.
        const float swap_target = cam.shoulder_swapped.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
        cam.shoulder_swap = cam.shoulder_swap_valid
                                ? ramp_weight(cam.shoulder_swap, swap_target, k_shoulder_swap_seconds, delta_time)
                                : swap_target;
        cam.shoulder_swap_valid = true;
        const float offset_right = params.offset_right * shoulder_side(cam.shoulder_swap);
        const Vector3 lateral_offset = right * offset_right;

        // Anchor the rig to a STABLE point. The first-person eye (eye_position, read from CView+0x14)
        // carries head-bob, breathing and weapon-sway, so anchoring the third-person camera to it
        // shakes the whole view. Instead anchor to the player BODY origin - column 3 of the entity
        // world matrix at OFFSET_ENTITY_WORLD_MATRIX_MEMBER (Matrix34, translation at m[*][3]) - which
        // does not bob - lifted by EyeHeight to roughly eye level along world up. Falls back to the eye
        // anchor when the feature is off (EyeHeight <= 0) or the entity/world matrix is unavailable, so
        // a missing player or a layout drift degrades to the previous behaviour rather than misplacing
        // the camera.
        const Vector3 world_up{0.0f, 0.0f, 1.0f};
        const float eye_height = params.eye_height;
        // Read the player body world origin once. It feeds the stable anchor (below) AND the
        // device-agnostic movement speed used by the camera-relative move alignment; body_valid gates both.
        // The body is read FRESH from the live C_Player every frame (rather than through a mirrored pointer that goes
        // null/stale across view transitions and reloads), which is what keeps the move-detection (hence the
        // camera-relative body-turn and the body anchor) locked onto the CURRENT player. When it does not read,
        // body_valid stays false and the camera degrades to the eye anchor.
        const BodyReading body = c_player != 0 ? port.body(c_player) : BodyReading{};
        const Vector3 body_origin = body.origin;
        const float body_yaw = body.yaw;
        const bool body_valid = body.valid;
        // The action dispatcher's movement input, read once for the turn hold and camera-relative movement below.
        const std::optional<float> move_signal = port.move_input();
        // Held off the anchor while the body steps through a native turn-in-place animation, so the camera does not
        // shift with the turn. The look yaw comes from the look quat, or from the eye quat, whose yaw is the look yaw
        // in first person.
        Vector3 turn_hold{0.0f, 0.0f, 0.0f};
        if (body_valid)
        {
            const Vector3 look_fwd = (look_valid ? look_rotation : eye_rotation).rotate(Vector3{0.0f, 1.0f, 0.0f});
            const bool look_yaw_valid = look_fwd.x * look_fwd.x + look_fwd.y * look_fwd.y > 1e-6f;
            const float look_yaw = look_yaw_valid ? std::atan2(-look_fwd.x, look_fwd.y) : 0.0f;
            turn_hold = hold_turn_in_place_pivot(cam, port, move_signal, body_origin, body_yaw, look_yaw,
                                                 look_yaw_valid, delta_time);
        }
        const Vector3 anchor_origin = body_origin - turn_hold;
        // Publish the live REAL eye height (FP eye above the body root) for the overlay read-out and as the
        // dynamic-eye-sync source. eye_position carries head bob; body_origin is the bob-free feet origin.
        if (body_valid)
        {
            cam.real_eye_height.store(eye_position.z - body_origin.z, std::memory_order_relaxed);
        }
        // The eye anchor (EyeHeight 0, or no body) takes the turn hold too, so switching anchor mode with a hold in
        // place does not jump; the hold then eases out as usual.
        Vector3 anchor_base = eye_position - turn_hold;
        if (body_valid && eye_height > 0.0f)
        {
            float effective_eye_height = eye_height;
            bool sync_engaged = false; // true while actively re-anchoring (vs. just using the configured height)
            if (params.dynamic_eye_sync)
            {
                // Re-anchor the eye HEIGHT to the real FP eye when a low pose (kneel / pray, and other poses
                // EyeHeight does not model) drops it OUT OF RANGE of the configured height - where a fixed
                // height floats the camera too high. Within the threshold (head bob / a minor pose change) keep
                // the steady bob-free height; in the re-anchored low poses we accept the bob (they are mostly
                // stationary). Ease so the swap slides; eye_sync_valid resets on suppression so re-engaging snaps.
                constexpr float k_oor_threshold = 0.25f; // meters: a genuine pose drop, not head bob
                constexpr float k_sync_rate = 9.0f;      // ease rate, 1/sec (frame-rate independent below)
                const float real_eye_height = eye_position.z - body_origin.z;
                sync_engaged = std::fabs(real_eye_height - eye_height) > k_oor_threshold;
                const float target_eye_height = sync_engaged ? real_eye_height : eye_height;
                if (!cam.eye_sync_valid)
                {
                    cam.eye_sync_applied = target_eye_height;
                    cam.eye_sync_valid = true;
                }
                else
                {
                    const float k = 1.0f - std::exp(-k_sync_rate * delta_time);
                    cam.eye_sync_applied += (target_eye_height - cam.eye_sync_applied) * k;
                }
                effective_eye_height = cam.eye_sync_applied;
            }
            else
            {
                cam.eye_sync_valid = false; // feature off -> the next engage snaps to the current pose
            }
            // Publish for the overlay status line (actively re-anchoring vs. using the configured height).
            cam.eye_sync_engaged.store(sync_engaged, std::memory_order_relaxed);
            cam.eye_sync_effective.store(effective_eye_height, std::memory_order_relaxed);
            anchor_base = anchor_origin + world_up * effective_eye_height;
        }
        else
        {
            cam.eye_sync_engaged.store(false, std::memory_order_relaxed);
        }
        const Vector3 pivot = anchor_base + world_up * params.offset_up;

        // Free-look orbit angles (degrees), accumulated by the input hook while the key is
        // held. On release they ease back to center. Read here so the render reflects them.
        const bool orbit_held = cam.orbit_active.load(std::memory_order_relaxed);
        // Gamepad right-stick orbit: integrate the latched stick DEFLECTION by RATE into the SAME orbit
        // accumulator the mouse writes, so the stick orbits the camera (yaw) and raises/lowers it (pitch) like
        // the mouse. Done here (not in the input hook) because the stick reports a HELD position, so it needs
        // delta_time to be a frame-rate-independent turn rate and to keep moving while held steady. Yaw matches
        // the mouse (stick-right orbits right); the right-stick Y reads OPPOSITE the mouse look delta, so it is
        // negated below (stick-up raises the camera, like the mouse). GamepadOrbitSpeed X/Y (deg/sec at full stick,
        // one rate per axis so each can be inverted with a negative value) sets the rate - a SEPARATE knob from the
        // mouse's OrbitSensitivity X/Y, because a relative mouse (delta) and an absolute stick (rate) cannot share
        // one linear scale; the pitch is clamped to the same limits.
        // Gated on the cursor-shown freeze (a UI up holds
        // still, mirroring the mouse path); when not orbiting or frozen the latch is cleared so re-engaging with
        // the stick centred does not jump.
        if (orbit_held && !port.cursor_shown())
        {
            // The preset's response curve, then its smoothing: a curve of 1 and 0 s use the latched deflection as it
            // is. A deflection easing back to a centred stick lands on 0 below k_pad_rest instead of turning the
            // orbit ever more slowly (at 720 deg/s it is under 0.1 deg/s).
            constexpr float k_pad_rest = 1e-4f;
            const StickDeflection shaped = shape_stick({cam.orbit_pad_yaw.load(std::memory_order_relaxed),
                                                        cam.orbit_pad_pitch.load(std::memory_order_relaxed)},
                                                       std::clamp(params.gamepad_orbit_curve, 1.0f, 3.0f));
            const float pad_alpha = spring_alpha(params.gamepad_orbit_smoothing, delta_time);
            const auto ease_axis = [pad_alpha](Spring<float> &eased, float target) noexcept
            {
                eased.step(target, pad_alpha);
                if (target == 0.0f && std::fabs(eased.applied) < k_pad_rest && std::fabs(eased.stage1) < k_pad_rest)
                {
                    eased.snap(0.0f);
                }
                return eased.applied;
            };
            const float pad_yaw = ease_axis(cam.orbit_pad_yaw_eased, shaped.x);
            const float pad_pitch = ease_axis(cam.orbit_pad_pitch_eased, shaped.y);
            if (pad_yaw != 0.0f || pad_pitch != 0.0f)
            {
                const float step_x = params.gamepad_orbit_speed_x * delta_time;
                const float step_y = params.gamepad_orbit_speed_y * delta_time;
                cam.orbit_yaw.store(cam.orbit_yaw.load(std::memory_order_relaxed) - pad_yaw * step_x,
                                    std::memory_order_relaxed);
                const float old_pitch = cam.orbit_pitch.load(std::memory_order_relaxed);
                cam.orbit_pitch.store(
                    std::clamp(old_pitch - pad_pitch * step_y,
                               std::min(cam.orbit_pitch_lo.load(std::memory_order_relaxed), old_pitch),
                               std::max(cam.orbit_pitch_hi.load(std::memory_order_relaxed), old_pitch)),
                    std::memory_order_relaxed);
            }
        }
        else
        {
            cam.orbit_pad_yaw.store(0.0f, std::memory_order_relaxed);
            cam.orbit_pad_pitch.store(0.0f, std::memory_order_relaxed);
            cam.orbit_pad_yaw_eased.snap(0.0f);
            cam.orbit_pad_pitch_eased.snap(0.0f);
        }

        // Wrap the accumulated yaw into [-180, 180] for the orbit math. The orbit is periodic in yaw,
        // so this is invisible while orbiting, but on release it lets the camera ease back to centre
        // along the SHORTEST path (<= 180 deg) in one smooth pass instead of un-spinning every full
        // revolution the player made. The re-based value is only stored back on release (below), so the wrap
        // here stays main-thread-local while held. The raw accumulator itself is written by the mouse path
        // on the input thread and by the gamepad integration above on the main thread; only one input device
        // drives orbit at a time in practice, so the relaxed load here does not race a meaningful write (and a
        // dropped sub-degree delta in the unlikely simultaneous case is harmless, never torn).
        float orbit_yaw_deg = std::remainder(cam.orbit_yaw.load(std::memory_order_relaxed), 360.0f);
        float orbit_pitch_deg = cam.orbit_pitch.load(std::memory_order_relaxed);

        // While moving with a captured heading, the camera holds its WORLD yaw and the body turns UNDER it.
        // Derive the rig's orbit angle as (camera_world_yaw - current_body_forward_yaw), where camera_world_yaw =
        // the captured heading plus any orbit the player has added since capture. On the capture frame the body
        // has not turned yet, so this equals the pre-move orbit (no snap); as the eye+body rotate to the heading
        // the angle eases to the residual orbit, so the camera POSITION is identical across the turn (no pop).
        // forward is this frame's rig basis forward (the stable-aim or smoothed basis when active, else the raw
        // eye look); char_forward_yaw below derives from the SAME forward that positions the camera, so the two
        // cancel and the held world yaw is independent of which source forward tracks. cam.orbit_yaw stays the
        // raw input accumulator throughout; the rendered angle is baked back only on release/stop so free-look resumes
        // from exactly where the moving camera was. orbit_moving implies the key was held, so this also runs on
        // the release frame (orbit_moving is cleared at end of frame), giving the bake-on-release below.
        const bool move_orbit = cam.orbit_moving && cam.orbit_target_valid;
        if (move_orbit)
        {
            const float char_forward_yaw = forward_yaw;
            const float raw_user_since_deg =
                cam.orbit_yaw.load(std::memory_order_relaxed) - cam.orbit_yaw_at_capture_deg;
            // In continuous-align (GTA) mode the user's orbit-since-capture STEERS the run: it drives
            // body_target_yaw (the look + body heading) below, and the camera follows that heading (the
            // derived rig angle cancels to ~0, keeping the camera behind). Smooth that steering input when
            // OrbitSmoothing is on so the turn is fluid instead of tracking the raw per-frame mouse deltas;
            // the SAME smoothed value is reused for body_target_yaw below so the camera stays behind the body.
            // Hold mode keeps the raw value (its orbit-around rides the snapped rig offset, left instant) -
            // only continuous-align steering is smoothed.
            const float orbit_smoothing = std::clamp(params.orbit_smoothing, 0.0f, 1.0f);
            const bool steer_smooth = params.orbit_continuous_align && orbit_smoothing > 1e-4f;
            if (!steer_smooth || !cam.orbit_steer_valid)
            {
                cam.orbit_steer_smooth = raw_user_since_deg; // snap: smoothing off / hold mode, or first move frame
                cam.orbit_steer_valid = true;
            }
            else
            {
                const float speed =
                    k_orbit_smooth_max_speed - orbit_smoothing * (k_orbit_smooth_max_speed - k_orbit_smooth_min_speed);
                const float ease = 1.0f - std::exp(-speed * delta_time);
                cam.orbit_steer_smooth += std::remainder(raw_user_since_deg - cam.orbit_steer_smooth, 360.0f) * ease;
            }
            const float user_orbit_since_deg = steer_smooth ? cam.orbit_steer_smooth : raw_user_since_deg;
            orbit_yaw_deg = std::remainder(
                radians_to_degrees(cam.orbit_target_yaw - char_forward_yaw) + user_orbit_since_deg, 360.0f);
        }
        else
        {
            cam.orbit_steer_valid = false; // not move-orbiting: the next engage snaps the steer low-pass
        }

        // Riding with free-look on, the camera turns back behind the mount as it moves (OrbitFollowSpeed).
        const std::uint32_t game_state = port.game_state();
        const bool riding = (game_state & (state_bit(GameState::Mount) | state_bit(GameState::Cart))) != 0u;
        follow_mount_heading(cam, params, forward_yaw, body_yaw, orbit_held && riding && body_valid, move_orbit,
                             delta_time, orbit_yaw_deg);

        if (!orbit_held)
        {
            // Orbit toggle released. When a move-orbit was active, orbit_yaw_deg already holds the rendered
            // (world-stable) angle derived above, not the raw input accumulator, so the ease-back below starts
            // from where the camera actually is rather than jumping by the orbit held at capture. The single
            // store at the end of this block persists the re-based (and, with a return speed, eased) angle.
            const float return_speed = params.orbit_return_speed;
            if (return_speed > 0.0f)
            {
                // Ease back to the centre: directly behind and level (hardcoded 0, 0).
                const float init_yaw = 0.0f;
                const float init_pitch = 0.0f;
                const float ease = 1.0f - std::exp(-return_speed * delta_time);
                orbit_yaw_deg -= std::remainder(orbit_yaw_deg - init_yaw, 360.0f) * ease; // shortest path to centre
                orbit_pitch_deg -= (orbit_pitch_deg - init_pitch) * ease;
            }
            // Store the re-based (and, with a return speed, eased) angles so the next frame resumes from the
            // short-path value; also runs with return_speed == 0 so the held-spin accumulator is bounded the
            // moment the key is released.
            cam.orbit_yaw.store(orbit_yaw_deg, std::memory_order_relaxed);
            cam.orbit_pitch.store(orbit_pitch_deg, std::memory_order_relaxed);
        }

        // Temporal low-pass on the orbit angle. orbit_yaw_deg / orbit_pitch_deg above are the raw target
        // (the accumulated mouse input, or the eased return / world-stable move derivation); the rig is
        // built from a smoothed copy that eases toward it each frame so free-look is not as jittery as the
        // unfiltered per-frame deltas (the engine's own look smoothing runs past the dispatch we block).
        // orbit_smoothing 0 = off (snap, raw). Snap on the first engaged frame (orbit_render_valid false) so
        // the camera does not slide in from centre. SNAP ALSO while move-orbiting (a movement key is driving
        // the camera-relative turn): there orbit_yaw_deg is the WORLD-STABLE derivation that cancels the body
        // turn to keep the camera planted, so it must track the body 1:1 - smoothing it would lag the
        // cancellation and swing the camera on the move-start, when it should be instant. The target writes
        // above always use the unsmoothed value, so the accumulator and the return/move logic are unaffected;
        // only the idle (standing free-look) rendered angle is low-passed.
        const float orbit_smoothing = std::clamp(params.orbit_smoothing, 0.0f, 1.0f);
        if (orbit_smoothing <= 1e-4f || !cam.orbit_render_valid || move_orbit)
        {
            cam.orbit_yaw_render = orbit_yaw_deg;
            cam.orbit_pitch_render = orbit_pitch_deg;
            cam.orbit_render_valid = true;
        }
        else
        {
            const float smooth_speed =
                k_orbit_smooth_max_speed - orbit_smoothing * (k_orbit_smooth_max_speed - k_orbit_smooth_min_speed);
            const float smooth_ease = 1.0f - std::exp(-smooth_speed * delta_time);
            // Shortest-path ease on yaw so a wrap (e.g. -179 -> +179) eases the short way, not all the way round.
            cam.orbit_yaw_render += std::remainder(orbit_yaw_deg - cam.orbit_yaw_render, 360.0f) * smooth_ease;
            cam.orbit_pitch_render += (orbit_pitch_deg - cam.orbit_pitch_render) * smooth_ease;
        }
        const float render_yaw_deg = cam.orbit_yaw_render;
        const float render_pitch_deg = cam.orbit_pitch_render;

        // "Orbiting" while EITHER the raw target or the still-easing rendered angle is off-centre, so the
        // orbit block keeps running until the smoothed angle has fully settled back (otherwise a release
        // would cut the rig straight to centre before the low-pass finishes).
        const bool orbiting = orbit_yaw_deg < -0.05f || orbit_yaw_deg > 0.05f || orbit_pitch_deg < -0.05f ||
                              orbit_pitch_deg > 0.05f || render_yaw_deg < -0.05f || render_yaw_deg > 0.05f ||
                              render_pitch_deg < -0.05f || render_pitch_deg > 0.05f;

        // While free-look is on, the orbit is built from a held base: the follow rig for the look pitch it had when
        // free-look took over, so the camera stays put whatever the look pitch does meanwhile. Until the base is held
        // its pitch follows the look, so on the frame free-look takes over the held base is the pose on screen and is
        // taken at once; easing it in lets the live look (leveled by OrbitLevelAim meanwhile) drag the camera.
        // When free-look turns off, the held pitch eases back onto the look by angle, riding on the look at a
        // decaying offset, so the rig is always a real follow rig: free-look turned back on during the ease holds it
        // where it is, and the live rig takes over once the two meet. The hold keys on orbit_held, not on
        // "orbiting": with OrbitReturnSpeed 0 the angles stay and "orbiting" latches true, which pins the camera
        // to the held base and lock out the look pitch. Easing back on release lets a retained orbit angle ride as a
        // rigid offset on top of the real look, so vertical control is kept in every preset.
        {
            constexpr float k_orbit_base_speed = 8.0f;
            // The ease back ends once the held pitch is this close to the look's.
            constexpr float k_orbit_base_settled = 1e-3f; // rad
            if (!cam.orbit_base_held)
            {
                cam.orbit_base_pitch = forward_pitch;
                cam.orbit_base_offset = 0.0f;
                cam.orbit_base_held = orbit_held;
            }
            else if (orbit_held)
            {
                cam.orbit_base_offset = cam.orbit_base_pitch - forward_pitch;
            }
            else
            {
                cam.orbit_base_offset *= std::exp(-k_orbit_base_speed * delta_time);
                cam.orbit_base_pitch = forward_pitch + cam.orbit_base_offset;
                cam.orbit_base_held = std::fabs(cam.orbit_base_offset) >= k_orbit_base_settled;
            }
        }

        // The real aim is driven AFTER the orbit look is finalized (below), so the heading can be aligned
        // to the camera's forward direction on the idle -> moving edge.

        // The follow rig is a spring arm on the look: the camera sits `distance` back from the pivot along the look and
        // the shoulder offset to its right, and it looks along the look itself, like the first-person view carried back
        // and to the side. The screen-centre crosshair is therefore parallel to the player's aim line, a shoulder
        // offset beside it. A walk the way the player looks moves the camera along its own view axis, so what sits
        // under the crosshair stays there as it does in first person, and a change of the follow distance slides the
        // camera straight along its look. Ranged shots and the interaction ray are re-aimed onto the crosshair ray by
        // their own hooks, so the view is not toed in toward the aim line: a toe-in turns the view as the follow
        // distance changes and slides the crosshair off a target the player walks toward. The orbit rotation is
        // applied on top, not folded into this base, so the orbit stays instantly responsive and never accumulates
        // into the follow position. The follow is instant: the anchor is the de-bobbed body origin, so there is no
        // shake to smooth out.
        Vector3 camera_position = pivot - forward * distance + lateral_offset;
        Vector3 look_forward = forward;
        Vector3 look_up = up;
        bool rebuild_basis = false;

        // Static follow angle: turn the RESTING rig about the pivot by FollowYaw / FollowPitch (degrees, both 0 =
        // directly behind and level) with the same spring-arm turn as the free-look orbit below. Positive yaw swings
        // the camera to one side, positive pitch raises it and tilts the look down. The offset and the look turn
        // together, so the over-the-shoulder framing holds while the camera circles you. The free-look orbit then turns
        // further on top of this resting angle, and its held base is turned the same way.
        const float follow_yaw = params.follow_yaw;
        const float follow_pitch = params.follow_pitch;
        const bool follow_turned =
            follow_yaw < -0.05f || follow_yaw > 0.05f || follow_pitch < -0.05f || follow_pitch > 0.05f;
        const float follow_yaw_rad = degrees_to_radians(follow_yaw);
        const float follow_pitch_rad = degrees_to_radians(follow_pitch);
        if (follow_turned)
        {
            Vector3 off = camera_position - pivot;
            turn_rig(off, look_forward, look_up, follow_yaw_rad, follow_pitch_rad);
            camera_position = pivot + off;
            rebuild_basis = true;
        }

        // Aim hand-off. When a situation in OrbitExcludeState (aiming, a distraction throw, a drawn weapon, a menu)
        // turns free-look off while the camera is turned away from the look, the camera swings back behind the old
        // look, and what starts there (a shot, a throw) starts where the character faced instead of where the
        // camera points. So the look and the body are turned to the camera's direction (the follow-up below the orbit
        // rig), and the orbit rig holds the camera where it was until the look shows it. The direction comes from the
        // previous frame, the one on screen, measured against that frame's own plain rig.
        if (cam.aim_handoff_requested && !orbit_held && !cam.aim_handoff_active && cam.rig_last_orbited &&
            cam.orbit_suspended.load(std::memory_order_relaxed) &&
            (std::fabs(cam.rig_last_turn) > k_aim_handoff_min_angle ||
             std::fabs(cam.rig_last_rise) > k_aim_handoff_min_angle))
        {
            cam.aim_handoff_active = true;
            cam.aim_handoff_azimuth = cam.rig_last_azimuth;
            cam.aim_handoff_elevation = cam.rig_last_elevation;
            cam.aim_handoff_yaw = cam.rig_last_yaw;
            cam.aim_handoff_pitch = cam.rig_last_pitch;
            cam.aim_handoff_seconds = 0.0f;
            cam.aim_handoff_written = false;
            cam.aim_handoff_player = c_player;
            // The plain rig takes over at once; the hold keeps the camera in place across the switch.
            cam.orbit_base_held = false;
            port.event(RigEvent::HandoffStart, radians_to_degrees(cam.rig_last_turn), radians_to_degrees(forward_pitch),
                       radians_to_degrees(cam.rig_last_pitch), 0);
        }
        const Vector3 plain_look = look_forward;

        // Free-look orbit: turn the WHOLE non-orbit rig (the camera's offset from the pivot, its look and its up)
        // rigidly about the pivot with turn_rig - yaw about world up, then pitch about the look heading's horizontal
        // axis. The offset (shoulder + height + distance) turns with the look, so the over-the-shoulder framing and the
        // crosshair keep their screen positions while the camera circles the player, and the turned rig is exactly the
        // follow rig of the turned look: a zoom while orbited still slides the camera straight along its look. Yaw is
        // unbounded so full spins work. The pitch is tracked as the arm's elevation (how far the rig is raised: the
        // negative of its look pitch), clamped away from the pole. At zero angle the rotation is identity, so the orbit
        // engages without a jump. Mouse up/down raises/lowers the camera.
        const bool orbit_rig = orbiting || cam.orbit_base_held || cam.aim_handoff_active;
        if (orbit_rig)
        {
            // The base the orbit turns: the live follow pose (the player's look pitch included), or the held base. The
            // held base is the follow rig built the same way as above for orbit_base_pitch, with this frame's heading,
            // distance, shoulder and follow angle, so a zoom or a preset blend still reshapes it. The height stays in
            // the pivot.
            Vector3 offset0 = camera_position - pivot;
            Vector3 base_look = look_forward;
            Vector3 base_up = look_up;
            if (cam.orbit_base_held)
            {
                Vector3 heading{forward.x, forward.y, 0.0f};
                if (heading.magnitude_squared() > 1e-6f)
                {
                    heading = heading.normalized();
                }
                else
                {
                    heading = world_up.cross(right).normalized(); // straight down/up: recover heading from right
                }
                const Vector3 held_fwd =
                    heading * std::cos(cam.orbit_base_pitch) + world_up * std::sin(cam.orbit_base_pitch);
                const Vector3 held_right = heading.cross(world_up).normalized();
                offset0 = held_fwd * (-distance) + held_right * offset_right;
                base_look = held_fwd;
                base_up = held_right.cross(held_fwd);
                if (follow_turned)
                {
                    turn_rig(offset0, base_look, base_up, follow_yaw_rad, follow_pitch_rad);
                }
            }

            const float base_elevation = -std::asin(std::clamp(base_look.z, -1.0f, 1.0f));
            // The elevation stays off the overhead pole; a held base already past that limit (a steep look) is kept
            // but cannot go further.
            const float elevation_min = std::min(-1.45f, base_elevation);
            const float elevation_max = std::max(1.45f, base_elevation);
            float elevation =
                std::clamp(base_elevation + degrees_to_radians(render_pitch_deg), elevation_min, elevation_max);
            // The pitch range for the next input, from level: OrbitPitchMin / OrbitPitchMax less the held base's
            // elevation, widened to include the base itself (reachable even when it starts outside the range) and
            // kept within the elevation the rig can show.
            if (orbit_held && !cam.aim_handoff_active)
            {
                const float base_deg = radians_to_degrees(base_elevation);
                const float lo = std::max(std::min(params.orbit_pitch_min - base_deg, 0.0f),
                                          radians_to_degrees(elevation_min) - base_deg);
                const float hi = std::min(std::max(params.orbit_pitch_max - base_deg, 0.0f),
                                          radians_to_degrees(elevation_max) - base_deg);
                cam.orbit_pitch_lo.store(lo, std::memory_order_relaxed);
                cam.orbit_pitch_hi.store(hi, std::memory_order_relaxed);
            }
            float yaw_delta = degrees_to_radians(render_yaw_deg);
            if (cam.aim_handoff_active)
            {
                // Aim hand-off: keep the camera in the direction it had when the situation took over, whatever the look
                // and the basis do meanwhile, and leave the matching orbit angles behind so the ease back, or free-look
                // turned back on before the look has turned (a paused menu closed), resumes here.
                yaw_delta =
                    std::remainder(cam.aim_handoff_azimuth - std::atan2(-base_look.x, base_look.y), DirectX::XM_2PI);
                elevation = std::clamp(cam.aim_handoff_elevation, elevation_min, elevation_max);
                const float held_yaw_deg = radians_to_degrees(yaw_delta);
                const float held_pitch_deg = radians_to_degrees(elevation - base_elevation);
                cam.orbit_yaw.store(held_yaw_deg, std::memory_order_relaxed);
                cam.orbit_pitch.store(held_pitch_deg, std::memory_order_relaxed);
                cam.orbit_yaw_render = held_yaw_deg;
                cam.orbit_pitch_render = held_pitch_deg;
                cam.orbit_render_valid = true;
            }

            turn_rig(offset0, base_look, base_up, yaw_delta, elevation - base_elevation);
            camera_position = pivot + offset0;
            look_forward = base_look;
            look_up = base_up;
            rebuild_basis = true;
        }

        // While the orbit rig shapes the camera, record for the next frame's aim hand-off its look heading and arm
        // elevation, its turn and rise off this frame's plain rig, and the player look that puts the plain rig on it.
        // All are measured within one frame, so a preset blend or a basis turn between frames does not count as orbit.
        // The orbit and the follow angle both turn the rig as a spring arm, which adds to the look's yaw and pitch, so
        // that player look is this frame's look turned by the yaw and pitch of the orbited look off the plain look.
        cam.rig_last_orbited = false;
        if (orbit_rig)
        {
            const float rig_pitch = std::asin(std::clamp(look_forward.z, -1.0f, 1.0f));
            const float plain_pitch = std::asin(std::clamp(plain_look.z, -1.0f, 1.0f));
            cam.rig_last_azimuth = std::atan2(-look_forward.x, look_forward.y);
            cam.rig_last_elevation = -rig_pitch;
            cam.rig_last_turn =
                std::remainder(cam.rig_last_azimuth - std::atan2(-plain_look.x, plain_look.y), DirectX::XM_2PI);
            cam.rig_last_rise = plain_pitch - rig_pitch;
            cam.rig_last_pitch =
                std::clamp(forward_pitch + rig_pitch - plain_pitch, -k_aim_handoff_max_pitch, k_aim_handoff_max_pitch);
            cam.rig_last_yaw = std::remainder(forward_yaw + cam.rig_last_turn, DirectX::XM_2PI);
            cam.rig_last_orbited = true;
        }

        // Aim hand-off follow-up. While aiming the game re-derives the look heading from the body every frame (the body
        // follows the look), so the look alone cannot be turned: the body is turned with it, as camera-relative
        // movement does, and both are written every frame until the look shows the new heading. They are written at
        // least once, so a hand-off that only raises or lowers the camera still sets the pitch, which sticks, and it
        // can end only on a later frame than its first write: on the same frame's second build the eye pose does not
        // show the write yet. Until then the next frame's basis snaps to the look, so the rig lands on the new
        // direction at once instead of easing across the turn under AimBasisSmoothing.
        if (cam.aim_handoff_active)
        {
            const bool lost = cam.aim_handoff_cancel || c_player == 0 || c_player != cam.aim_handoff_player;
            const bool jumped = body_valid && cam.turn_track_step > k_aim_handoff_jump;
            const Vector3 look_now = (look_valid ? look_rotation : eye_rotation).rotate(Vector3{0.0f, 1.0f, 0.0f});
            const float yaw_gap =
                std::remainder(std::atan2(-look_now.x, look_now.y) - cam.aim_handoff_yaw, DirectX::XM_2PI);
            if (lost)
            {
                cam.aim_handoff_active = false;
                cam.aim_handoff_cancel = false;
                port.event(RigEvent::HandoffLost, 0.0f, 0.0f, 0.0f, 0);
            }
            else if (cam.aim_handoff_written && !in.repeat_build && std::fabs(yaw_gap) < k_aim_handoff_min_angle)
            {
                cam.aim_handoff_active = false;
                port.event(RigEvent::HandoffTurned, cam.aim_handoff_seconds, 0.0f, 0.0f, 0);
            }
            else if (jumped)
            {
                cam.aim_handoff_active = false;
                port.event(RigEvent::HandoffMoved, 0.0f, 0.0f, 0.0f, 0);
            }
            else if (cam.aim_handoff_seconds >= k_aim_handoff_timeout)
            {
                cam.aim_handoff_active = false;
                port.event(RigEvent::HandoffTimeout, radians_to_degrees(yaw_gap), cam.aim_handoff_seconds, 0.0f, 0);
            }
            else if (!port.aim_control(1.0f, cam.aim_handoff_pitch, true, cam.aim_handoff_yaw))
            {
                cam.aim_handoff_active = false;
                port.event(RigEvent::HandoffWriteFailed, 0.0f, 0.0f, 0.0f, 0);
            }
            else
            {
                port.body_turn(cam.aim_handoff_yaw);
                cam.aim_handoff_written = true;
                cam.basis_quat_valid = false;
                // The pause menu stops the player update, so the look shows the new heading only once play resumes;
                // the wait does not count paused time.
                if (!in.paused)
                {
                    cam.aim_handoff_seconds += delta_time;
                }
            }
        }

        // Camera-relative movement (toggle orbit): on the idle -> moving edge CAPTURE the camera heading, then
        // HOLD it while moving. The body turn (RigPort::body_turn()) pins the body to that heading, which is
        // what makes locomotion camera-relative in EVERY direction (KCD2 moves relative to the body rotation):
        // W runs away from the camera, A/D strafe to the sides, S backs toward it. The look-yaw write
        // (RigPort::aim_control()) faces the head/aim the same way and feeds the camera-position derivation.
        // Both are re-applied every frame because the engine reverts/consumes a single write - like the pitch
        // leveling. Released when movement stops.
        bool do_align = false;
        // Camera-relative movement, keyed on the device-agnostic action-input intent
        // (RigPort::move_input(), from the action dispatcher). The input is nonzero the instant a movement
        // key is pressed - BEFORE the body accelerates - so the heading captures immediately and the
        // character turns to the camera direction without first moving the old way; and it stays nonzero while
        // a key is held against a wall, so the heading is not falsely released on a collision arrest. The
        // body-position speed is deliberately NOT used (it lags the intent and reads zero when arrested), so
        // the feature requires the action hook - if it did not resolve, camera-relative movement stays off
        // (free-look still works).
        if (orbit_held && body_valid && move_signal.has_value())
        {
            bool moving = cam.orbit_moving;
            const float move_magnitude = *move_signal;
            // Re-arm guard: a genuine release (magnitude below the stop threshold) must be observed since orbit
            // engaged before a move-start is honored. A stranded latch (a held-move release swallowed on a
            // combat action-map swap, see player_onaction_reset) reads > 0 with the keys up; without this guard
            // it re-trips orbit_moving the instant orbit restores and drives the body-turn with no input (the
            // post-combat self-rotation). Arming on a sub-stop reading means a fresh, observed press engages it.
            // A key held while orbit restores (running on right after leaving crouch) is no stranded latch: the
            // game's own move input shows it too, so that also arms the move. A stranded latch has the keys up and
            // the game's input at 0, so the guard still holds it. A move armed that way also ends when the game's
            // input goes idle, as another key's slot can still be stranded and hold the latch up.
            const std::optional<float> game_move = port.game_move(c_player);
            if (move_magnitude < k_move_input_stop)
            {
                cam.orbit_move_armed = true;
                cam.orbit_move_armed_by_game = false;
                cam.rig_stale_move_logged = false;
            }
            else if (!cam.orbit_move_armed && game_move && *game_move > k_move_input_start)
            {
                cam.orbit_move_armed = true;
                cam.orbit_move_armed_by_game = true;
                cam.orbit_move_game_idle = 0.0f;
                cam.rig_stale_move_logged = false;
                port.event(RigEvent::MoveArmedByGame, move_magnitude, 0.0f, 0.0f, 0);
            }
            // A hand-off still turning the look and the body (free-look back on before it ends) is not interrupted.
            if (!moving && !cam.aim_handoff_active && move_magnitude > k_move_input_start)
            {
                if (cam.orbit_move_armed)
                {
                    moving = true;
                    do_align = true; // idle -> moving edge: capture the camera heading
                    // Diagnostic: what tripped the body-turn. move_magnitude near 1.0 with a movement key/stick =
                    // genuine locomotion; a smaller or unexpected value while the player is only free-looking
                    // points at game-driven movement (finishing-move / combat footwork) being mistaken for intent.
                    // state is the debounced GameState mask (see game_state.hpp) so we can tell whether a combat /
                    // aiming / cinematic state was active when the body-turn engaged.
                    port.event(RigEvent::MoveStart, move_magnitude, params.orbit_continuous_align ? 1.0f : 0.0f, 0.0f,
                               game_state);
                }
                else if (!cam.rig_stale_move_logged)
                {
                    // Unarmed (no release seen since orbit engaged) yet reading as movement: a stranded latch.
                    // Suppress the body-turn re-trip and log it once - the diagnostic for the post-combat case.
                    port.event(RigEvent::MoveStaleSuppressed, move_magnitude, 0.0f, 0.0f, 0);
                    cam.rig_stale_move_logged = true;
                }
            }
            else if (moving && move_magnitude < k_move_input_stop)
            {
                // Release the latch the instant the movement keys are let go (no debounce tail), so stopping
                // and then rotating is immediately pure free-look and the character does not keep turning to
                // the camera. No bridge is needed the way body-speed required: the action input stays nonzero
                // while any key is held (diagonals, overlapping key-switches), so a drop to zero is a genuine
                // stop; a full release-then-repress simply re-captures the heading on the new press, which is
                // the correct behaviour.
                moving = false;
                // Bake the world-stable orbit angle back into the accumulator so free-look resumes from
                // exactly where the moving camera was, with no jump the instant movement stops.
                cam.orbit_yaw.store(orbit_yaw_deg, std::memory_order_relaxed);
                port.event(RigEvent::MoveStop, move_magnitude, 0.0f, 0.0f, 0);
            }
            else if (moving && cam.orbit_move_armed_by_game)
            {
                // Armed by the game's own input: it ends once that input has stayed idle, even while another latch
                // slot is stranded. Disarmed rather than reset, so a stranded slot stays held off by the guard and a
                // key still held re-arms through the game's input on the next frame.
                cam.orbit_move_game_idle =
                    (game_move && *game_move < k_move_input_stop) ? cam.orbit_move_game_idle + delta_time : 0.0f;
                if (cam.orbit_move_game_idle >= k_orbit_move_game_stop_delay)
                {
                    moving = false;
                    cam.orbit_move_armed = false;
                    cam.orbit_move_armed_by_game = false;
                    cam.orbit_yaw.store(orbit_yaw_deg, std::memory_order_relaxed);
                    port.event(RigEvent::MoveStopGameIdle, move_magnitude, 0.0f, 0.0f, 0);
                }
            }
            cam.orbit_moving = moving;
        }
        else
        {
            cam.orbit_moving = false;
            cam.orbit_move_armed = false; // re-armed by a release, or by a held move the game's input shows
            cam.orbit_move_armed_by_game = false;
        }

        // Capture the heading on move-start: the eye-look yaw plus the orbit applied this frame, the yaw the
        // world-stable orbit derivation above renders, so the camera stays put as the body turns. It is built from
        // the angles rather than atan2(look_forward), which also carries the static follow yaw.
        if (do_align)
        {
            cam.orbit_target_yaw = forward_yaw + degrees_to_radians(cam.orbit_yaw_render);
            cam.orbit_target_valid = true;
            // Snapshot the orbit input so further orbiting while moving is measured from here. We do NOT
            // reset the orbit to 0: the world-stable derivation above holds the camera in place while the
            // body turns to this heading, so there is no snap-behind pop.
            cam.orbit_yaw_at_capture_deg = cam.orbit_yaw.load(std::memory_order_relaxed);
            // The look is turned to this heading below, so the next frame's basis snaps to it instead of easing
            // across the turn (see orbit_move_pressed).
            cam.basis_quat_valid = false;
        }
        if (!cam.orbit_moving)
        {
            cam.orbit_target_valid = false;
        }
        // The heading a move press captures, for orbit_move_pressed: the camera's yaw on screen (with the
        // orbit as rendered, so a smoothed angle still catching up does not snap), published only while a press
        // starts a camera-relative move with the body turn, and stamped so a stale one is never used.
        if (orbit_held && body_valid && move_signal.has_value() && !cam.orbit_moving && cam.orbit_move_armed &&
            !cam.aim_handoff_active && params.orbit_body_turn)
        {
            port.publish_move_press(forward_yaw + degrees_to_radians(cam.orbit_yaw_render), body_yaw);
        }
        else
        {
            port.withdraw_move_press();
        }

        const float pitch_ease =
            (orbit_held && params.orbit_level_aim) ? (1.0f - std::exp(-k_orbit_aim_level_speed * delta_time)) : 0.0f;
        const bool hold_yaw = orbit_held && cam.orbit_moving && cam.orbit_target_valid;
        // Heading fed to the body + movement while moving. CONTINUOUS-ALIGN (GTA style): follow the CURRENT
        // camera world yaw every frame (captured heading + the orbit added since), so orbiting STEERS the run
        // and the rig stays behind the character. Otherwise HOLD the heading captured on move-start (orbit
        // looks around freely while the character keeps its line). The world-stable camera derivation above is
        // the same either way - only this target differs - so in continuous mode the derived orbit angle
        // cancels to ~0 (camera behind the character) while in hold mode it carries the orbit-around offset.
        float body_target_yaw = cam.orbit_target_yaw;
        if (hold_yaw && params.orbit_continuous_align)
        {
            // Reuse the smoothed steer angle computed in the move-orbit derivation above (when OrbitSmoothing
            // is on) so the body/look heading and the rig agree - the camera follows the smoothed steering
            // and stays behind. With smoothing off this is the raw orbit-since-capture. The smoothed value is
            // only current once the move-orbit block above has run for it (cam.orbit_steer_valid); on the
            // idle->moving capture frame that block did NOT run (move_orbit read the pre-update orbit_moving,
            // which was still false), so fall back to the raw orbit-since-capture - ~0 on the capture frame
            // because orbit_yaw_at_capture_deg was just snapshotted - instead of a stale smoothed value from a
            // prior move. orbit_smoothing was read for the orbit-angle low-pass above and is constant across
            // the frame, so it is reused here.
            const float user_orbit_since_deg =
                (orbit_smoothing > 1e-4f && cam.orbit_steer_valid)
                    ? cam.orbit_steer_smooth
                    : (cam.orbit_yaw.load(std::memory_order_relaxed) - cam.orbit_yaw_at_capture_deg);
            body_target_yaw = cam.orbit_target_yaw + degrees_to_radians(user_orbit_since_deg);
        }
        // The hand-off writes the look itself while it runs.
        if (orbit_held && !cam.aim_handoff_active && (pitch_ease > 0.0f || hold_yaw))
        {
            (void)port.aim_control(pitch_ease, 0.0f, hold_yaw, body_target_yaw);
        }
        // Pin the BODY to the camera heading while moving so the character runs camera-relative in EVERY
        // direction. KCD2 moves the player relative to the ENTITY (body) rotation, NOT the look, so this body
        // override - not the look-yaw write above - is what redirects locomotion: with the body faced at the
        // camera heading, W runs away from the camera, A/D strafe to the sides and S backs toward it. The body
        // must hold the CAMERA heading, not the input direction: facing it at the movement direction would
        // rotate the move frame and re-apply the input on top of it (world_move = body + input), sending every
        // key the wrong way (A/D -> backward, S -> forward). Held every frame while moving (the override is
        // consume-once); released when movement stops.
        const bool body_turn_active = hold_yaw && params.orbit_body_turn;
        if (body_turn_active != cam.rig_body_turn_logged)
        {
            cam.rig_body_turn_logged = body_turn_active;
            port.event(body_turn_active ? RigEvent::BodyTurnEngaged : RigEvent::BodyTurnReleased, 0.0f, 0.0f, 0.0f, 0);
        }
        if (body_turn_active)
        {
            port.body_turn(body_target_yaw);
        }

        // Looking up, Arm Follow Up and Lens Drop Up bend the arm instead of swinging it down on the sphere toward the
        // ground behind the character (see aim_arm_shift()). The shift depends on the rendered look alone, so the aim
        // hand-off and free-look engage do not step.
        camera_position =
            camera_position + aim_arm_shift(look_forward, look_up, distance, params.arm_follow_up, params.lens_drop_up);

        out.eye_position = eye_position;
        out.eye_forward = eye_forward;
        out.eye_up = eye_up;
        out.forward = forward;
        out.look_forward = look_forward;
        out.look_up = look_up;
        out.camera_position = camera_position;
        out.pivot = pivot;
        out.anchor_base = anchor_base;
        out.anchor_origin = anchor_origin;
        out.turn_hold = turn_hold;
        out.body_origin = body_origin;
        out.entity = body.entity;
        out.offset_right = offset_right;
        out.body_valid = body_valid;
        // When the stable / smoothed basis shaped the rig, the rendered orientation is rebuilt from it too: otherwise
        // the engine eye orientation (which carries the view-shake) would still drive the view rotation although the
        // position is stable, leaving the view tilting during an action. When the basis was not shaped, eye_forward ==
        // forward and eye_up == up, so the rebuild would be a no-op.
        out.rebuild_basis = rebuild_basis || basis_overridden;
        return true;
    }
} // namespace TPVCamera
