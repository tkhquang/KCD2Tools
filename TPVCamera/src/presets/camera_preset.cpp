/**
 * @file camera_preset.cpp
 * @brief Apply/ease helpers bridging CameraPreset and the live atomics.
 */

#include "camera_preset.hpp"
#include "config.hpp"

#include <atomic>

namespace TPVCamera::Presets
{

    void apply_to_live(const CameraPreset &preset, LiveSettings &s) noexcept
    {
        constexpr auto rel = std::memory_order_relaxed;

        s.follow_distance.store(preset.follow_distance, rel);
        s.follow_distance_min.store(preset.follow_distance_min, rel);
        s.follow_distance_max.store(preset.follow_distance_max, rel);
        s.zoom_step.store(preset.zoom_step, rel);
        s.offset_up.store(preset.offset_up, rel);
        s.eye_height.store(preset.eye_height, rel);
        s.dynamic_eye_sync.store(preset.dynamic_eye_sync, rel);
        s.offset_right.store(preset.offset_right, rel);
        s.aim_focus_distance.store(preset.aim_focus_distance, rel);
        s.follow_yaw.store(preset.follow_yaw, rel);
        s.follow_pitch.store(preset.follow_pitch, rel);
        s.fov.store(preset.fov, rel);

        s.orbit_sensitivity_x.store(preset.orbit_sensitivity_x, rel);
        s.orbit_sensitivity_y.store(preset.orbit_sensitivity_y, rel);
        s.gamepad_orbit_speed_x.store(preset.gamepad_orbit_speed_x, rel);
        s.gamepad_orbit_speed_y.store(preset.gamepad_orbit_speed_y, rel);
        s.orbit_pitch_min.store(preset.orbit_pitch_min, rel);
        s.orbit_pitch_max.store(preset.orbit_pitch_max, rel);
        s.orbit_return_speed.store(preset.orbit_return_speed, rel);
        s.orbit_smoothing.store(preset.orbit_smoothing, rel);
        s.orbit_level_aim.store(preset.orbit_level_aim, rel);
        s.orbit_body_turn.store(preset.orbit_body_turn, rel);
        s.orbit_continuous_align.store(preset.orbit_continuous_align, rel);

        s.enable_collision.store(preset.enable_collision, rel);
        s.collision_skin.store(preset.collision_skin, rel);
        s.collision_return_speed.store(preset.collision_return_speed, rel);
    }

    void ease_toward(CameraPreset &a, const CameraPreset &t, float alpha) noexcept
    {
        const auto lerp = [alpha](float from, float to) noexcept { return from + (to - from) * alpha; };

        a.follow_distance = lerp(a.follow_distance, t.follow_distance);
        a.follow_distance_min = lerp(a.follow_distance_min, t.follow_distance_min);
        a.follow_distance_max = lerp(a.follow_distance_max, t.follow_distance_max);
        a.zoom_step = lerp(a.zoom_step, t.zoom_step);
        a.offset_up = lerp(a.offset_up, t.offset_up);
        // eye_height and aim_focus_distance use 0 as a MODE selector, not a continuous value: eye_height 0
        // anchors to the bobbing FP eye (> 0 anchors to the body origin lifted by the height, and the body
        // origin sits at the feet, so blending across 0 sweeps the anchor through the ground); aim_focus 0
        // tracks the follow distance (> 0 pins a fixed depth). The exponential ease only asymptotes toward
        // 0, so it would never re-select the 0 mode. Snap across the 0 boundary; blend within a mode.
        const auto blend_mode_zero = [alpha](float from, float to) noexcept
        {
            constexpr float k_mode_eps = 1e-4f;
            if ((from <= k_mode_eps) != (to <= k_mode_eps))
                return to; // mode change: snap
            return from + (to - from) * alpha;
        };
        a.eye_height = blend_mode_zero(a.eye_height, t.eye_height);
        a.offset_right = lerp(a.offset_right, t.offset_right);
        a.aim_focus_distance = blend_mode_zero(a.aim_focus_distance, t.aim_focus_distance);
        a.follow_yaw = lerp(a.follow_yaw, t.follow_yaw);
        a.follow_pitch = lerp(a.follow_pitch, t.follow_pitch);
        // FOV is SNAPPED to the active target here; the actual smooth ease lives in the camera hook, which blends
        // the rendered FOV toward the target -- or toward the live GAME FOV when the target is 0 (off) -- so
        // turning a preset's FOV on/off glides through the game FOV instead of snapping across the 0 boundary.
        a.fov = t.fov;

        a.orbit_sensitivity_x = lerp(a.orbit_sensitivity_x, t.orbit_sensitivity_x);
        a.orbit_sensitivity_y = lerp(a.orbit_sensitivity_y, t.orbit_sensitivity_y);
        a.gamepad_orbit_speed_x = lerp(a.gamepad_orbit_speed_x, t.gamepad_orbit_speed_x);
        a.gamepad_orbit_speed_y = lerp(a.gamepad_orbit_speed_y, t.gamepad_orbit_speed_y);
        a.orbit_pitch_min = lerp(a.orbit_pitch_min, t.orbit_pitch_min);
        a.orbit_pitch_max = lerp(a.orbit_pitch_max, t.orbit_pitch_max);
        a.orbit_return_speed = lerp(a.orbit_return_speed, t.orbit_return_speed);
        a.orbit_smoothing = lerp(a.orbit_smoothing, t.orbit_smoothing);

        a.collision_skin = lerp(a.collision_skin, t.collision_skin);
        a.collision_return_speed = lerp(a.collision_return_speed, t.collision_return_speed);

        // Bools snap to the target (no meaningful interpolation).
        a.orbit_level_aim = t.orbit_level_aim;
        a.orbit_body_turn = t.orbit_body_turn;
        a.orbit_continuous_align = t.orbit_continuous_align;
        a.enable_collision = t.enable_collision;
        a.dynamic_eye_sync = t.dynamic_eye_sync;
    }

} // namespace TPVCamera::Presets
