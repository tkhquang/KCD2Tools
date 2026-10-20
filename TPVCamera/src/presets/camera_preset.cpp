/**
 * @file camera_preset.cpp
 * @brief The preset blend's ease helper.
 */

#include "camera_preset.hpp"

namespace TPVCamera::Presets
{

    void ease_toward(CameraPreset &a, const CameraPreset &t, float alpha) noexcept
    {
        const auto lerp = [alpha](float from, float to) noexcept { return from + (to - from) * alpha; };

        a.follow_distance = lerp(a.follow_distance, t.follow_distance);
        a.follow_distance_min = lerp(a.follow_distance_min, t.follow_distance_min);
        a.follow_distance_max = lerp(a.follow_distance_max, t.follow_distance_max);
        a.zoom_step = lerp(a.zoom_step, t.zoom_step);
        a.offset_up = lerp(a.offset_up, t.offset_up);
        // eye_height uses 0 as a MODE selector, not a continuous value: eye_height 0 follows the upper body fully
        // (> 0 is the standing height the anchor is lifted to). The exponential ease only asymptotes toward 0, so
        // it never re-selects the 0 mode. Snap across the 0 boundary and blend within a mode. The rig eases the
        // anchor across the switch.
        const auto blend_mode_zero = [alpha](float from, float to) noexcept
        {
            constexpr float k_mode_eps = 1e-4f;
            if ((from <= k_mode_eps) != (to <= k_mode_eps))
                return to; // mode change: snap
            return from + (to - from) * alpha;
        };
        a.eye_height = blend_mode_zero(a.eye_height, t.eye_height);
        a.offset_right = lerp(a.offset_right, t.offset_right);
        a.follow_yaw = lerp(a.follow_yaw, t.follow_yaw);
        a.follow_pitch = lerp(a.follow_pitch, t.follow_pitch);
        a.lock_framing_angle = lerp(a.lock_framing_angle, t.lock_framing_angle);
        a.lock_focus_height = lerp(a.lock_focus_height, t.lock_focus_height);
        a.lock_focus_offset = lerp(a.lock_focus_offset, t.lock_focus_offset);
        a.arm_follow_up = lerp(a.arm_follow_up, t.arm_follow_up);
        a.lens_drop_up = lerp(a.lens_drop_up, t.lens_drop_up);
        // FOV is SNAPPED to the active target here; the actual smooth ease lives in the camera hook, which blends
        // the rendered FOV toward the target - or toward the live GAME FOV when the target is 0 (off) - so
        // turning a preset's FOV on/off glides through the game FOV instead of snapping across the 0 boundary.
        a.fov = t.fov;
        // The blend time belongs to the preset being blended into, so it is taken as it is, never blended.
        a.blend_in_seconds = t.blend_in_seconds;

        a.orbit_sensitivity_x = lerp(a.orbit_sensitivity_x, t.orbit_sensitivity_x);
        a.orbit_sensitivity_y = lerp(a.orbit_sensitivity_y, t.orbit_sensitivity_y);
        a.gamepad_orbit_speed_x = lerp(a.gamepad_orbit_speed_x, t.gamepad_orbit_speed_x);
        a.gamepad_orbit_speed_y = lerp(a.gamepad_orbit_speed_y, t.gamepad_orbit_speed_y);
        a.gamepad_orbit_curve = lerp(a.gamepad_orbit_curve, t.gamepad_orbit_curve);
        a.gamepad_orbit_smoothing = lerp(a.gamepad_orbit_smoothing, t.gamepad_orbit_smoothing);
        a.orbit_pitch_min = lerp(a.orbit_pitch_min, t.orbit_pitch_min);
        a.orbit_pitch_max = lerp(a.orbit_pitch_max, t.orbit_pitch_max);
        a.orbit_return_speed = lerp(a.orbit_return_speed, t.orbit_return_speed);
        a.orbit_follow_speed = lerp(a.orbit_follow_speed, t.orbit_follow_speed);
        a.orbit_smoothing = lerp(a.orbit_smoothing, t.orbit_smoothing);

        a.collision_skin = lerp(a.collision_skin, t.collision_skin);
        a.collision_return_speed = lerp(a.collision_return_speed, t.collision_return_speed);

        // Bools snap to the target (no meaningful interpolation).
        a.orbit_level_aim = t.orbit_level_aim;
        a.orbit_body_turn = t.orbit_body_turn;
        a.orbit_continuous_align = t.orbit_continuous_align;
        a.enable_collision = t.enable_collision;
        a.shoulder_collision = t.shoulder_collision;
        a.dynamic_eye_sync = t.dynamic_eye_sync;
    }

} // namespace TPVCamera::Presets
