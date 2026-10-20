/**
 * @file default_presets.hpp
 * @brief Factory preset definitions embedded in the binary (the mod does NOT ship a presets JSON).
 *
 * @details The presets file is user-owned customization, not a shipped asset: shipping it would
 *          overwrite a player's tuned presets on every mod update. Instead the canonical factory
 *          presets live here as a compile-time JSON literal. PresetStore parses it to:
 *            - seed and write the presets file on first run (create-if-missing),
 *            - re-add any built-in a user file is missing (e.g. AIMING added by a later update), and
 *            - back the "Reset to factory" and "Reset all" actions.
 *          The schema mirrors PresetStore::write() output (root: version/editing/ui_scale/value_compact/
 *          cycle_picks plus a presets array; each preset: name/builtin/bind_state/fields). Only the
 *          "presets" array and "cycle_picks" are parsed from this literal; the other root scalars are kept
 *          purely so the embedded copy matches the on-disk layout (the runtime root values come from the
 *          saved file, not from here). Field keys are the camera_preset_fields.hpp keys; preset_from_json
 *          starts every preset at the CameraPreset member defaults and keeps them for any missing key. The
 *          DEFAULT entry therefore carries no fields: the member defaults in camera_preset.hpp are the factory
 *          DEFAULT look, its one source, and retuning it means changing them alone. A preset that lists no
 *          blend_in_seconds blends in over that default, 0.52 s; the aiming presets list a shorter one and the
 *          mount and cart a longer one.
 *
 *          The entries with "builtin": false are factory starting points beside a built-in: each joins
 *          the cycle group of the built-in bound to the same states, and the player can edit, rename or
 *          delete it like their own preset. A group uses its last member until the player picks one,
 *          which here is the factory starting point, so "cycle_picks" keeps the built-in in use. A
 *          built-in's "Reset to factory" adds back a missing starting point of its group, and "Reset all"
 *          adds back every one, which is also how a presets file written before one shipped gains it.
 */
#ifndef TPVCAMERA_PRESETS_DEFAULT_PRESETS_HPP
#define TPVCAMERA_PRESETS_DEFAULT_PRESETS_HPP

namespace TPVCamera::Presets
{

    /// Canonical factory presets as a JSON document, parsed once at runtime to seed/repair the store.
    inline constexpr const char *k_default_presets_json = R"json(
{
  "version": 3,
  "editing": "DEFAULT",
  "ui_scale": 1.0,
  "value_compact": true,
  "cycle_picks": {
    "aiming": "AIMING",
    "default": "DEFAULT"
  },
  "presets": [
    {
      "name": "DEFAULT",
      "builtin": true,
      "bind_state": "default"
    },
    {
      "name": "COMBAT",
      "builtin": true,
      "bind_state": "combat",
      "fields": {
        "collision_return_speed": 6.0,
        "collision_skin": 0.2,
        "dynamic_eye_sync": true,
        "enable_collision": true,
        "eye_height": 0.0,
        "follow_distance": 3.5,
        "follow_distance_max": 10.0,
        "follow_distance_min": 0.8,
        "follow_pitch": 5.0,
        "follow_yaw": 0.0,
        "fov": 60.0,
        "gamepad_orbit_speed_x": 200.0,
        "gamepad_orbit_speed_y": 200.0,
        "lock_framing_angle": 30.0,
        "offset_right": 0.0,
        "offset_up": 0.0,
        "orbit_body_turn": true,
        "orbit_continuous_align": true,
        "orbit_level_aim": true,
        "orbit_pitch_max": 75.0,
        "orbit_pitch_min": -50.0,
        "orbit_return_speed": 8.0,
        "orbit_sensitivity_x": 0.2,
        "orbit_sensitivity_y": 0.2,
        "orbit_smoothing": 0.5,
        "zoom_step": 4.0
      }
    },
    {
      "name": "AIMING",
      "builtin": true,
      "bind_state": "aiming",
      "fields": {
        "blend_in_seconds": 0.3,
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
        "gamepad_orbit_speed_x": 200.0,
        "gamepad_orbit_speed_y": 200.0,
        "offset_right": 0.75,
        "offset_up": 0.0,
        "orbit_body_turn": true,
        "orbit_continuous_align": true,
        "orbit_level_aim": true,
        "orbit_pitch_max": 75.0,
        "orbit_pitch_min": -50.0,
        "orbit_return_speed": 8.0,
        "orbit_sensitivity_x": 0.2,
        "orbit_sensitivity_y": 0.2,
        "orbit_smoothing": 0.5,
        "zoom_step": 4.0
      }
    },
    {
      "name": "MOUNT",
      "builtin": true,
      "bind_state": "mount",
      "fields": {
        "blend_in_seconds": 1.0,
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
        "gamepad_orbit_speed_x": 200.0,
        "gamepad_orbit_speed_y": 200.0,
        "offset_right": 0.55,
        "offset_up": 0.0,
        "orbit_body_turn": true,
        "orbit_continuous_align": true,
        "orbit_level_aim": true,
        "orbit_pitch_max": 75.0,
        "orbit_pitch_min": -50.0,
        "orbit_return_speed": 8.0,
        "orbit_sensitivity_x": 0.2,
        "orbit_sensitivity_y": 0.2,
        "orbit_smoothing": 0.5,
        "zoom_step": 4.0
      }
    },
    {
      "name": "STEALTH",
      "builtin": true,
      "bind_state": "crouch",
      "fields": {
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
        "gamepad_orbit_speed_x": 200.0,
        "gamepad_orbit_speed_y": 200.0,
        "offset_right": 0.4,
        "offset_up": 0.0,
        "orbit_body_turn": true,
        "orbit_continuous_align": true,
        "orbit_level_aim": true,
        "orbit_pitch_max": 75.0,
        "orbit_pitch_min": -50.0,
        "orbit_return_speed": 8.0,
        "orbit_sensitivity_x": 0.2,
        "orbit_sensitivity_y": 0.2,
        "orbit_smoothing": 0.5,
        "zoom_step": 4.0
      }
    },
    {
      "name": "LYING",
      "builtin": true,
      "bind_state": "lying",
      "fields": {
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
        "gamepad_orbit_speed_x": 200.0,
        "gamepad_orbit_speed_y": 200.0,
        "offset_right": 0.4,
        "offset_up": 0.0,
        "orbit_body_turn": true,
        "orbit_continuous_align": true,
        "orbit_level_aim": true,
        "orbit_pitch_max": 75.0,
        "orbit_pitch_min": -50.0,
        "orbit_return_speed": 8.0,
        "orbit_sensitivity_x": 0.2,
        "orbit_sensitivity_y": 0.2,
        "orbit_smoothing": 0.5,
        "zoom_step": 4.0
      }
    },
    {
      "name": "SITTING",
      "builtin": true,
      "bind_state": "sitting",
      "fields": {
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
        "gamepad_orbit_speed_x": 200.0,
        "gamepad_orbit_speed_y": 200.0,
        "offset_right": 0.25,
        "offset_up": 0.0,
        "orbit_body_turn": true,
        "orbit_continuous_align": true,
        "orbit_level_aim": true,
        "orbit_pitch_max": 75.0,
        "orbit_pitch_min": -50.0,
        "orbit_return_speed": 8.0,
        "orbit_sensitivity_x": 0.2,
        "orbit_sensitivity_y": 0.2,
        "orbit_smoothing": 0.5,
        "zoom_step": 4.0
      }
    },
    {
      "name": "KNEEL",
      "builtin": true,
      "bind_state": "kneel",
      "fields": {
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
        "gamepad_orbit_speed_x": 200.0,
        "gamepad_orbit_speed_y": 200.0,
        "offset_right": 0.5,
        "offset_up": 0.0,
        "orbit_body_turn": true,
        "orbit_continuous_align": true,
        "orbit_level_aim": true,
        "orbit_pitch_max": 75.0,
        "orbit_pitch_min": -50.0,
        "orbit_return_speed": 8.0,
        "orbit_sensitivity_x": 0.2,
        "orbit_sensitivity_y": 0.2,
        "orbit_smoothing": 0.5,
        "zoom_step": 4.0
      }
    },
    {
      "name": "CART",
      "builtin": true,
      "bind_state": "cart",
      "fields": {
        "blend_in_seconds": 1.0,
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
        "gamepad_orbit_speed_x": 200.0,
        "gamepad_orbit_speed_y": 200.0,
        "offset_right": 0.55,
        "offset_up": 0.0,
        "orbit_body_turn": true,
        "orbit_continuous_align": true,
        "orbit_level_aim": true,
        "orbit_pitch_max": 75.0,
        "orbit_pitch_min": -50.0,
        "orbit_return_speed": 8.0,
        "orbit_sensitivity_x": 0.2,
        "orbit_sensitivity_y": 0.2,
        "orbit_smoothing": 0.5,
        "zoom_step": 4.0
      }
    },
    {
      "name": "Close Shoulder",
      "builtin": false,
      "bind_state": "default",
      "fields": {
        "collision_return_speed": 6.0,
        "collision_skin": 0.2,
        "dynamic_eye_sync": true,
        "enable_collision": true,
        "eye_height": 1.6,
        "follow_distance": 1.8,
        "follow_distance_max": 10.0,
        "follow_distance_min": 0.8,
        "follow_pitch": 0.0,
        "follow_yaw": 0.0,
        "fov": 55.0,
        "gamepad_orbit_speed_x": 200.0,
        "gamepad_orbit_speed_y": 200.0,
        "offset_right": 0.45,
        "offset_up": 0.0,
        "orbit_body_turn": true,
        "orbit_continuous_align": true,
        "orbit_level_aim": true,
        "orbit_pitch_max": 75.0,
        "orbit_pitch_min": -50.0,
        "orbit_return_speed": 8.0,
        "orbit_sensitivity_x": 0.2,
        "orbit_sensitivity_y": 0.2,
        "orbit_smoothing": 0.5,
        "zoom_step": 4.0
      }
    },
    {
      "name": "Aiming Wide",
      "builtin": false,
      "bind_state": "aiming",
      "fields": {
        "blend_in_seconds": 0.3,
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
        "gamepad_orbit_speed_x": 200.0,
        "gamepad_orbit_speed_y": 200.0,
        "offset_right": 0.4,
        "offset_up": 0.0,
        "orbit_body_turn": true,
        "orbit_continuous_align": true,
        "orbit_level_aim": true,
        "orbit_pitch_max": 75.0,
        "orbit_pitch_min": -50.0,
        "orbit_return_speed": 8.0,
        "orbit_sensitivity_x": 0.2,
        "orbit_sensitivity_y": 0.2,
        "orbit_smoothing": 0.5,
        "zoom_step": 4.0
      }
    }
  ]
}
)json";

} // namespace TPVCamera::Presets

#endif // TPVCAMERA_PRESETS_DEFAULT_PRESETS_HPP
