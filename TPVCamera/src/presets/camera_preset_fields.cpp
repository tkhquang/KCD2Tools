/**
 * @file camera_preset_fields.cpp
 * @brief The editable-field table for CameraPreset (UI + JSON share this).
 */

#include "camera_preset_fields.hpp"

namespace TPVCamera::Presets
{
namespace
{

constexpr PresetField float_field(const char *key, const char *label, const char *tooltip,
                                  const char *group, float CameraPreset::*member,
                                  float min_value, float max_value, float fine_step) noexcept
{
    return PresetField{key, label, tooltip, group, FieldType::Float, member, nullptr, min_value, max_value, fine_step};
}

constexpr PresetField bool_field(const char *key, const char *label, const char *tooltip,
                                 const char *group, bool CameraPreset::*member) noexcept
{
    return PresetField{key, label, tooltip, group, FieldType::Bool, nullptr, member, 0.0f, 0.0f, 0.0f};
}

// Order defines the editor layout; grouping is by the "group" column. Ranges are the slider bounds
// (intentionally wider than the INI defaults so presets can be pushed further). fine_step is the
// SINGLE-arrow increment (0.01 for every field, including degrees); the << / >> double arrows step
// 10x that (0.1).
constexpr PresetField k_fields[] = {
    // Framing.
    float_field("follow_distance", "Follow Distance",
                "How far the camera rests behind you, in meters. This is the starting distance; the zoom "
                "keys move it within the Min/Max range below.",
                "Framing", &CameraPreset::follow_distance, 0.5f, 8.0f, 0.01f),
    float_field("follow_distance_min", "Follow Distance Min",
                "Closest the zoom keys can pull the camera in, in meters.",
                "Framing", &CameraPreset::follow_distance_min, 0.3f, 4.0f, 0.01f),
    float_field("follow_distance_max", "Follow Distance Max",
                "Farthest the zoom keys can push the camera out, in meters.",
                "Framing", &CameraPreset::follow_distance_max, 1.0f, 12.0f, 0.01f),
    float_field("zoom_step", "Zoom Step",
                "How fast the camera moves while you hold a zoom key, in meters per second.",
                "Framing", &CameraPreset::zoom_step, 0.1f, 10.0f, 0.01f),
    float_field("offset_up", "Offset Up",
                "Raises or lowers the pivot (the point the camera sits behind and frames) along the world "
                "up axis, in meters. Positive lifts the framing up.",
                "Framing", &CameraPreset::offset_up, -2.0f, 2.0f, 0.01f),
    float_field("offset_right", "Offset Right",
                "Over-the-shoulder shift, in meters along the camera's right axis (positive = right, 0 = "
                "centered behind). A bigger shift increases the crosshair-vs-shoulder parallax, which Aim "
                "Focus Distance compensates for.",
                "Framing", &CameraPreset::offset_right, -2.0f, 2.0f, 0.01f),
    float_field("eye_height", "Eye Height",
                "Height of the camera anchor above your feet, in meters. 0 is special: it anchors to the "
                "first-person eye, which moves with the walk/run head bob, so the camera will bob too. Use "
                "a positive value (around 1.6) for a steady, bob-free anchor.",
                "Framing", &CameraPreset::eye_height, 0.0f, 2.5f, 0.01f),
    bool_field("dynamic_eye_sync", "Dynamic Eye Sync",
               "Lowers the camera anchor to your real eye when a pose drops it well below Eye Height "
               "(kneeling, praying, and other low poses Eye Height does not account for), so the camera "
               "stops floating above you. It eases in and out, and accepts a little head bob in those "
               "poses. Only applies when Eye Height is non-zero.",
               "Framing", &CameraPreset::dynamic_eye_sync),
    float_field("aim_focus_distance", "Aim Focus Distance",
                "Affects your aim. The depth, in meters, the centered crosshair is converged to so it lines "
                "up with where your shot actually lands. 0 = auto (tracks the follow distance). Set it to "
                "your usual engagement range when using a small follow distance with a shoulder offset; a "
                "value that is too short or too long makes the crosshair and the real hit point diverge.",
                "Framing", &CameraPreset::aim_focus_distance, 0.0f, 30.0f, 0.01f),
    float_field("follow_yaw", "Follow Yaw",
                "Affects your aim. Statically swings the resting camera left/right around you, in degrees "
                "(0 = directly behind). This rotates the camera, and because aiming follows the camera it "
                "also turns your effective aim/shot direction, not just the framing.",
                "Framing", &CameraPreset::follow_yaw, -180.0f, 180.0f, 0.01f),
    float_field("follow_pitch", "Follow Pitch",
                "Affects your aim. Statically tilts the resting camera up/down around you, in degrees "
                "(0 = level, positive lifts the camera and looks down). This rotates the camera, and "
                "because aiming follows the camera it also shifts your effective aim/shot direction.",
                "Framing", &CameraPreset::follow_pitch, -89.0f, 89.0f, 0.01f),
    float_field("fov", "FOV",
                "Third-person field of view, in degrees. 0 = use the game's FOV (no override). Higher is "
                "wider (more peripheral / zoomed-out), lower is narrower (zoomed in). Per preset, so e.g. "
                "AIMING can narrow and COMBAT widen; it eases when the active preset changes.",
                "Framing", &CameraPreset::fov, 0.0f, 120.0f, 1.0f),

    // Orbit (free-look).
    float_field("orbit_sensitivity", "Orbit Sensitivity",
                "Free-look MOUSE sensitivity, as a multiplier (negative inverts the direction). Gamepad "
                "uses Gamepad Orbit Speed instead. Your normal in-game look sensitivity is unchanged.",
                "Orbit", &CameraPreset::orbit_sensitivity, -3.0f, 3.0f, 0.01f),
    float_field("gamepad_orbit_speed", "Gamepad Orbit Speed",
                "How fast the gamepad right stick orbits, in degrees/second at full stick. Independent of "
                "Orbit Sensitivity (which is mouse-only) -- raise it if the stick feels slower than the mouse.",
                "Orbit", &CameraPreset::gamepad_orbit_speed, 30.0f, 720.0f, 5.0f),
    float_field("orbit_pitch_min", "Orbit Pitch Min",
                "Lowest angle free-look can tilt down to, in degrees (negative looks down).",
                "Orbit", &CameraPreset::orbit_pitch_min, -89.0f, 0.0f, 0.01f),
    float_field("orbit_pitch_max", "Orbit Pitch Max",
                "Highest angle free-look can tilt up to, in degrees (positive looks up).",
                "Orbit", &CameraPreset::orbit_pitch_max, 0.0f, 89.0f, 0.01f),
    float_field("orbit_return_speed", "Orbit Return Speed",
                "How fast free-look eases back to center when you release it. 0 leaves the camera where you "
                "left it.",
                "Orbit", &CameraPreset::orbit_return_speed, 0.0f, 20.0f, 0.01f),
    float_field("orbit_smoothing", "Orbit Smoothing",
                "Smooths the free-look mouse motion (0 = raw/instant, higher = smoother but more lag). "
                "Free-look bypasses the engine's own look smoothing, so this adds it back.",
                "Orbit", &CameraPreset::orbit_smoothing, 0.0f, 1.0f, 0.01f),
    bool_field("orbit_level_aim", "Orbit Level Aim",
               "While free-looking, keep your character's real aim/head level (forward) instead of "
               "following the camera, so looking around does not swing where you aim.",
               "Orbit", &CameraPreset::orbit_level_aim),
    bool_field("orbit_body_turn", "Orbit Body Turn",
               "While moving in free-look, turn your body to face the camera's heading, so you run in the "
               "direction the camera points.",
               "Orbit", &CameraPreset::orbit_body_turn),
    bool_field("orbit_continuous_align", "Orbit Continuous Align",
               "While moving, keep the body continuously aligned to the camera heading (the camera steers "
               "your run) instead of aligning just once when you start moving.",
               "Orbit", &CameraPreset::orbit_continuous_align),

    // Collision.
    bool_field("enable_collision", "Enable Collision",
               "Pull the camera in when something would block the view, so it does not clip through walls "
               "or terrain.",
               "Collision", &CameraPreset::enable_collision),
    float_field("collision_skin", "Collision Skin",
                "Gap kept between the camera and a surface it pulls in against, in meters. Larger keeps the "
                "camera further off walls.",
                "Collision", &CameraPreset::collision_skin, 0.0f, 1.0f, 0.01f),
    float_field("collision_return_speed", "Collision Return Speed",
                "How fast the camera eases back out once the obstruction clears. Lower is smoother, higher "
                "snaps out faster.",
                "Collision", &CameraPreset::collision_return_speed, 0.0f, 20.0f, 0.01f),
};

} // namespace

std::span<const PresetField> fields() noexcept
{
    return std::span<const PresetField>{k_fields};
}

} // namespace TPVCamera::Presets
