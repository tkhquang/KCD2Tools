/**
 * @file camera_preset.hpp
 * @brief Plain-data camera preset: a named, persistable mirror of the preset-owned
 *        LiveSettings fields, plus apply/ease helpers.
 *
 * @details A CameraPreset captures only the camera GEOMETRY/feel fields (framing,
 *          orbit, collision). The state-policy globals (forced-view masks,
 *          orbit-exclude mask, EnableStateBehavior, debounce/blend timing) stay in
 *          LiveSettings and are NOT per-preset. The field list here mirrors the
 *          preset-owned atomics in config.hpp (LiveSettings); the single source of
 *          truth that pairs each field with its UI label, range and JSON key lives
 *          in camera_preset_fields.hpp so the slider loop and the (de)serializer
 *          never drift apart.
 */
#ifndef TPVCAMERA_PRESETS_CAMERA_PRESET_HPP
#define TPVCAMERA_PRESETS_CAMERA_PRESET_HPP

#include <string>

namespace TPVCamera
{
    struct LiveSettings;
}

namespace TPVCamera::Presets
{

/**
 * @struct CameraPreset
 * @brief A named set of preset-owned camera values. Its member defaults equal the built-in DEFAULT
 *        preset, so a default-constructed CameraPreset is the factory DEFAULT look: the live atomics in
 *        config.hpp are seeded from it (see settings()), preset_from_json falls back to it for any key a
 *        saved preset omits, and the editor's per-field reset restores to it. The embedded DEFAULT in
 *        default_presets.hpp lists the same values and stays the runtime authority for the DEFAULT preset,
 *        so keep the two in agreement when retuning the factory default.
 */
struct CameraPreset
{
    std::string name;                ///< Display + identity name (built-ins are reserved).
    bool builtin = false;            ///< True for the built-ins (DEFAULT/COMBAT/AIMING/MOUNT/STEALTH/LYING/SITTING/KNEEL/CART); cannot be removed/renamed.
    std::string bind_state = "none"; ///< States it auto-applies on, comma tokens (e.g. "aiming,crouch"); "none" = pin-only, "default" = floor. Built-ins fix this.

    // Follow framing. These defaults equal the built-in DEFAULT preset (default_presets.hpp); retuning the
    // factory DEFAULT means changing both, in agreement.
    float follow_distance = 3.5f;
    float follow_distance_min = 0.8f;
    float follow_distance_max = 10.0f;
    float zoom_step = 1.0f;
    float offset_up = 0.0f;
    float eye_height = 1.6f;
    bool dynamic_eye_sync = true;     // re-anchor eye height to the real FP eye on out-of-range low poses (kneel/pray)
    float offset_right = 0.5f;
    float aim_focus_distance = 12.9f; // crosshair convergence depth, meters (0 = track follow distance)
    float follow_yaw = 0.0f;         // static resting orbit yaw around the pivot, degrees (0 = directly behind)
    float follow_pitch = 0.0f;       // static resting orbit pitch around the pivot, degrees (0 = level)
    float fov = 60.0f;               // third-person FOV in degrees (0 = use the game FOV / no override)

    // Free-look orbit.
    float orbit_sensitivity = 0.2f;
    float gamepad_orbit_speed = 200.0f; // gamepad right-stick orbit rate, deg/sec at full deflection (mouse uses orbit_sensitivity)
    float orbit_pitch_min = -50.0f;
    float orbit_pitch_max = 75.0f;
    float orbit_return_speed = 8.0f;
    float orbit_smoothing = 0.5f;    // free-look angle low-pass strength, 0..1 (0 = off/raw, higher = smoother)
    bool orbit_level_aim = true;
    bool orbit_body_turn = true;
    bool orbit_continuous_align = true;

    // Camera collision.
    bool enable_collision = true;
    float collision_skin = 0.2f;
    float collision_return_speed = 6.0f;
};

/// Reserved built-in preset names (case-sensitive). These cannot be removed or renamed.
inline constexpr const char *k_builtin_default = "DEFAULT";
inline constexpr const char *k_builtin_combat = "COMBAT";
inline constexpr const char *k_builtin_aiming = "AIMING";
inline constexpr const char *k_builtin_mount = "MOUNT";
inline constexpr const char *k_builtin_stealth = "STEALTH";
inline constexpr const char *k_builtin_lying = "LYING";
inline constexpr const char *k_builtin_sitting = "SITTING";
inline constexpr const char *k_builtin_kneel = "KNEEL";
inline constexpr const char *k_builtin_cart = "CART";

/**
 * @brief Stores every preset-owned field of @p preset into the live atomics (relaxed).
 * @details Called on the render thread by the runtime resolver each active frame. Only
 *          the preset-owned atomics are written; state-policy globals are left alone.
 */
void apply_to_live(const CameraPreset &preset, LiveSettings &settings) noexcept;

/**
 * @brief Eases the float fields of @p applied toward @p target by @p alpha in [0,1]; bools snap.
 * @details applied += (target - applied) * alpha, per float field. alpha is a per-frame
 *          exponential factor (1 - exp(-speed * dt)) so the blend is frame-rate independent.
 *          name / builtin / bind_state are not touched.
 */
void ease_toward(CameraPreset &applied, const CameraPreset &target, float alpha) noexcept;

} // namespace TPVCamera::Presets

#endif // TPVCAMERA_PRESETS_CAMERA_PRESET_HPP
