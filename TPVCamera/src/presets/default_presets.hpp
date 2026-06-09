/**
 * @file default_presets.hpp
 * @brief Factory preset definitions embedded in the binary (the mod does NOT ship a presets JSON).
 *
 * @details The presets file is user-owned customization, not a shipped asset: shipping it would
 *          overwrite a player's tuned presets on every mod update. Instead the canonical factory
 *          presets live here as a compile-time JSON literal. PresetStore parses it to:
 *            - seed and write the presets file on first run (create-if-missing),
 *            - re-add any built-in a user file is missing (e.g. AIMING added by a later update), and
 *            - back a built-in's "reset to factory" action.
 *          The schema mirrors PresetStore::save() output (root: version/editing/ui_scale/value_compact
 *          plus a presets array; each preset: name/builtin/bind_state/fields). Only the "presets" array
 *          is parsed from this literal; the root scalars are kept purely so the embedded copy matches the
 *          on-disk layout (the runtime root values come from the saved file, not from here). Field keys
 *          are the camera_preset_fields.hpp keys; preset_from_json tolerates any missing key by keeping
 *          the CameraPreset default. The DEFAULT preset's values must stay in agreement with the
 *          CameraPreset member defaults in camera_preset.hpp (the two are the single source of the factory
 *          DEFAULT look; retuning it means changing both).
 */
#ifndef TPVCAMERA_PRESETS_DEFAULT_PRESETS_HPP
#define TPVCAMERA_PRESETS_DEFAULT_PRESETS_HPP

namespace TPVCamera::Presets
{

/// Canonical factory presets as a JSON document, parsed once at runtime to seed/repair the store.
inline constexpr const char *k_default_presets_json = R"json(
{
  "version": 1,
  "editing": "DEFAULT",
  "ui_scale": 1.0,
  "value_compact": true,
  "presets": [
    {
      "name": "DEFAULT",
      "builtin": true,
      "bind_state": "default",
      "fields": {
        "aim_focus_distance": 12.9,
        "collision_return_speed": 6.0,
        "collision_skin": 0.2,
        "dynamic_eye_sync": true,
        "enable_collision": true,
        "eye_height": 1.6,
        "follow_distance": 3.5,
        "follow_distance_max": 10.0,
        "follow_distance_min": 0.8,
        "follow_pitch": 0.0,
        "follow_yaw": 0.0,
        "fov": 60.0,
        "gamepad_orbit_speed": 200.0,
        "offset_right": 0.5,
        "offset_up": 0.0,
        "orbit_body_turn": true,
        "orbit_continuous_align": true,
        "orbit_level_aim": true,
        "orbit_pitch_max": 75.0,
        "orbit_pitch_min": -50.0,
        "orbit_return_speed": 8.0,
        "orbit_sensitivity": 0.2,
        "orbit_smoothing": 0.5,
        "zoom_step": 4.0
      }
    },
    {
      "name": "COMBAT",
      "builtin": true,
      "bind_state": "combat",
      "fields": {
        "aim_focus_distance": 12.9,
        "collision_return_speed": 6.0,
        "collision_skin": 0.2,
        "dynamic_eye_sync": false,
        "enable_collision": true,
        "eye_height": 0.0,
        "follow_distance": 3.5,
        "follow_distance_max": 10.0,
        "follow_distance_min": 0.8,
        "follow_pitch": 0.0,
        "follow_yaw": 0.0,
        "fov": 60.0,
        "gamepad_orbit_speed": 200.0,
        "offset_right": 0.75,
        "offset_up": 0.0,
        "orbit_body_turn": true,
        "orbit_continuous_align": true,
        "orbit_level_aim": true,
        "orbit_pitch_max": 75.0,
        "orbit_pitch_min": -50.0,
        "orbit_return_speed": 8.0,
        "orbit_sensitivity": 0.2,
        "orbit_smoothing": 0.5,
        "zoom_step": 4.0
      }
    },
    {
      "name": "AIMING",
      "builtin": true,
      "bind_state": "aiming",
      "fields": {
        "aim_focus_distance": 12.9,
        "collision_return_speed": 6.0,
        "collision_skin": 0.2,
        "dynamic_eye_sync": false,
        "enable_collision": true,
        "eye_height": 0.0,
        "follow_distance": 2.0,
        "follow_distance_max": 10.0,
        "follow_distance_min": 0.8,
        "follow_pitch": 0.0,
        "follow_yaw": 0.0,
        "fov": 0.0,
        "gamepad_orbit_speed": 200.0,
        "offset_right": 0.9,
        "offset_up": 0.0,
        "orbit_body_turn": true,
        "orbit_continuous_align": true,
        "orbit_level_aim": true,
        "orbit_pitch_max": 75.0,
        "orbit_pitch_min": -50.0,
        "orbit_return_speed": 8.0,
        "orbit_sensitivity": 0.2,
        "orbit_smoothing": 0.5,
        "zoom_step": 4.0
      }
    },
    {
      "name": "MOUNT",
      "builtin": true,
      "bind_state": "mount",
      "fields": {
        "aim_focus_distance": 12.9,
        "collision_return_speed": 6.0,
        "collision_skin": 0.2,
        "dynamic_eye_sync": true,
        "enable_collision": true,
        "eye_height": 0.75,
        "follow_distance": 4.5,
        "follow_distance_max": 10.0,
        "follow_distance_min": 0.8,
        "follow_pitch": 0.0,
        "follow_yaw": 0.0,
        "fov": 60.0,
        "gamepad_orbit_speed": 200.0,
        "offset_right": 0.75,
        "offset_up": 0.0,
        "orbit_body_turn": true,
        "orbit_continuous_align": true,
        "orbit_level_aim": true,
        "orbit_pitch_max": 75.0,
        "orbit_pitch_min": -50.0,
        "orbit_return_speed": 8.0,
        "orbit_sensitivity": 0.2,
        "orbit_smoothing": 0.5,
        "zoom_step": 4.0
      }
    },
    {
      "name": "STEALTH",
      "builtin": true,
      "bind_state": "crouch",
      "fields": {
        "aim_focus_distance": 12.9,
        "collision_return_speed": 6.0,
        "collision_skin": 0.2,
        "dynamic_eye_sync": true,
        "enable_collision": true,
        "eye_height": 1.1,
        "follow_distance": 2.5,
        "follow_distance_max": 10.0,
        "follow_distance_min": 0.8,
        "follow_pitch": 0.0,
        "follow_yaw": 0.0,
        "fov": 60.0,
        "gamepad_orbit_speed": 200.0,
        "offset_right": 0.5,
        "offset_up": 0.0,
        "orbit_body_turn": true,
        "orbit_continuous_align": true,
        "orbit_level_aim": true,
        "orbit_pitch_max": 75.0,
        "orbit_pitch_min": -50.0,
        "orbit_return_speed": 8.0,
        "orbit_sensitivity": 0.2,
        "orbit_smoothing": 0.5,
        "zoom_step": 4.0
      }
    },
    {
      "name": "LYING",
      "builtin": true,
      "bind_state": "lying",
      "fields": {
        "aim_focus_distance": 12.9,
        "collision_return_speed": 6.0,
        "collision_skin": 0.2,
        "dynamic_eye_sync": true,
        "enable_collision": true,
        "eye_height": 0.0,
        "follow_distance": 2.5,
        "follow_distance_max": 10.0,
        "follow_distance_min": 0.8,
        "follow_pitch": 0.0,
        "follow_yaw": 0.0,
        "fov": 60.0,
        "gamepad_orbit_speed": 200.0,
        "offset_right": 0.5,
        "offset_up": 0.0,
        "orbit_body_turn": true,
        "orbit_continuous_align": true,
        "orbit_level_aim": true,
        "orbit_pitch_max": 75.0,
        "orbit_pitch_min": -50.0,
        "orbit_return_speed": 8.0,
        "orbit_sensitivity": 0.2,
        "orbit_smoothing": 0.5,
        "zoom_step": 4.0
      }
    },
    {
      "name": "SITTING",
      "builtin": true,
      "bind_state": "sitting",
      "fields": {
        "aim_focus_distance": 12.9,
        "collision_return_speed": 6.0,
        "collision_skin": 0.2,
        "dynamic_eye_sync": true,
        "enable_collision": true,
        "eye_height": 0.0,
        "follow_distance": 1.5,
        "follow_distance_max": 10.0,
        "follow_distance_min": 0.8,
        "follow_pitch": 0.0,
        "follow_yaw": 0.0,
        "fov": 60.0,
        "gamepad_orbit_speed": 200.0,
        "offset_right": 0.3,
        "offset_up": 0.0,
        "orbit_body_turn": true,
        "orbit_continuous_align": true,
        "orbit_level_aim": true,
        "orbit_pitch_max": 75.0,
        "orbit_pitch_min": -50.0,
        "orbit_return_speed": 8.0,
        "orbit_sensitivity": 0.2,
        "orbit_smoothing": 0.5,
        "zoom_step": 4.0
      }
    },
    {
      "name": "KNEEL",
      "builtin": true,
      "bind_state": "kneel",
      "fields": {
        "aim_focus_distance": 12.9,
        "collision_return_speed": 6.0,
        "collision_skin": 0.2,
        "dynamic_eye_sync": true,
        "enable_collision": true,
        "eye_height": 0.0,
        "follow_distance": 2.5,
        "follow_distance_max": 10.0,
        "follow_distance_min": 0.8,
        "follow_pitch": 0.0,
        "follow_yaw": 0.0,
        "fov": 60.0,
        "gamepad_orbit_speed": 200.0,
        "offset_right": 0.6,
        "offset_up": 0.0,
        "orbit_body_turn": true,
        "orbit_continuous_align": true,
        "orbit_level_aim": true,
        "orbit_pitch_max": 75.0,
        "orbit_pitch_min": -50.0,
        "orbit_return_speed": 8.0,
        "orbit_sensitivity": 0.2,
        "orbit_smoothing": 0.5,
        "zoom_step": 4.0
      }
    },
    {
      "name": "CART",
      "builtin": true,
      "bind_state": "cart",
      "fields": {
        "aim_focus_distance": 12.9,
        "collision_return_speed": 6.0,
        "collision_skin": 0.2,
        "dynamic_eye_sync": true,
        "enable_collision": true,
        "eye_height": 0.0,
        "follow_distance": 5.0,
        "follow_distance_max": 10.0,
        "follow_distance_min": 0.8,
        "follow_pitch": 0.0,
        "follow_yaw": 0.0,
        "fov": 60.0,
        "gamepad_orbit_speed": 200.0,
        "offset_right": 0.75,
        "offset_up": 0.0,
        "orbit_body_turn": true,
        "orbit_continuous_align": true,
        "orbit_level_aim": true,
        "orbit_pitch_max": 75.0,
        "orbit_pitch_min": -50.0,
        "orbit_return_speed": 8.0,
        "orbit_sensitivity": 0.2,
        "orbit_smoothing": 0.5,
        "zoom_step": 4.0
      }
    }
  ]
}
)json";

} // namespace TPVCamera::Presets

#endif // TPVCAMERA_PRESETS_DEFAULT_PRESETS_HPP
