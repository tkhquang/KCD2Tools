/**
 * @file config.cpp
 * @brief Configuration registration for the TPV Toggle mod using DMK::Config.
 *
 * Registration order is: log level, LiveSettings atomics, init-only Config
 * values, then the hold-binding key lists. Press bindings are registered
 * separately (see input registration). DMK::Config::load() / log_all() are
 * driven by the mod lifecycle once every item is registered.
 */

#include "config.hpp"
#include "constants.hpp"

#include <DetourModKit.hpp>

// Process-wide init-only configuration, shared with the hooks and threads.
Config g_config;

namespace TPVToggle
{

LiveSettings &settings() noexcept
{
    static LiveSettings s;
    return s;
}

void register_config_items()
{
    // Log level drives Logger verbosity directly on load() and reload().
    DMK::Config::register_log_level("Settings", "LogLevel", Constants::DEFAULT_LOG_LEVEL);

    // Hot-path / cross-thread values bound to atomics so hot-reload is race-free.
    LiveSettings &s = settings();
    DMK::Config::register_atomic<bool>("CameraProfiles", "Enable", "Enable Camera Profiles", s.enableCameraProfiles, false);
    DMK::Config::register_atomic<float>("Settings", "TpvOffsetX", "TPV Offset X", s.tpvOffsetX, 0.0f);
    DMK::Config::register_atomic<float>("Settings", "TpvOffsetY", "TPV Offset Y", s.tpvOffsetY, 0.0f);
    DMK::Config::register_atomic<float>("Settings", "TpvOffsetZ", "TPV Offset Z", s.tpvOffsetZ, 0.0f);
    DMK::Config::register_atomic<float>("CameraSensitivity", "YawSensitivity", "Yaw Sensitivity", s.yawSensitivity, 1.0f);
    DMK::Config::register_atomic<float>("CameraSensitivity", "PitchSensitivity", "Pitch Sensitivity", s.pitchSensitivity, 1.0f);
    DMK::Config::register_atomic<bool>("CameraSensitivity", "EnablePitchLimits", "Enable Pitch Limits", s.pitchLimitsEnabled, false);
    DMK::Config::register_atomic<float>("CameraSensitivity", "PitchMin", "Pitch Minimum", s.pitchMin, -180.0f);
    DMK::Config::register_atomic<float>("CameraSensitivity", "PitchMax", "Pitch Maximum", s.pitchMax, 180.0f);
    DMK::Config::register_atomic<float>("CameraProfiles", "AdjustmentStep", "Adjustment Step", s.offsetAdjustmentStep, 0.01f);
    DMK::Config::register_atomic<int>("Settings", "OverlayRestoreDelayMs", "Overlay Restore Delay (ms)", s.overlayRestoreDelayMs, 200);

    // Init-only values applied once during setup (and re-applied by the reload callback).
    DMK::Config::register_bool("Settings", "EnableOverlayFeature", "Enable Overlay Feature",
                               [](bool v) { g_config.enable_overlay_feature = v; }, true);
    DMK::Config::register_float("Settings", "TpvFovDegrees", "TPV FOV Degrees",
                               [](float v) { g_config.tpv_fov_degrees = v; }, -1.0f);
    DMK::Config::register_string("CameraProfiles", "ProfileDirectory", "Profile Directory",
                                 [](const std::string &v) { g_config.profile_directory = v; }, "");
    DMK::Config::register_float("CameraProfiles", "TransitionDuration", "Transition Duration",
                               [](float v) { g_config.transition_duration = v; }, 0.3f);
    DMK::Config::register_bool("CameraProfiles", "UseSpringPhysics", "Use Spring Physics",
                               [](bool v) { g_config.use_spring_physics = v; }, false);
    DMK::Config::register_float("CameraProfiles", "SpringStrength", "Spring Strength",
                               [](float v) { g_config.spring_strength = v; }, 10.0f);
    DMK::Config::register_float("CameraProfiles", "SpringDamping", "Spring Damping",
                               [](float v) { g_config.spring_damping = v; }, 0.8f);

    // Hold-binding key lists (consumed once when the InputManager hold bindings
    // are registered). Defaults: scroll has no key; offset keys map to numpad.
    DMK::Config::register_key_combo("Settings", "HoldKeyToScroll", "Hold Key To Scroll",
                                    [](const DMK::Config::KeyComboList &c) { g_config.hold_scroll_keys = c; }, "");
    DMK::Config::register_key_combo("CameraProfiles", "OffsetXIncKey", "Offset X Increase Key",
                                    [](const DMK::Config::KeyComboList &c) { g_config.offset_x_inc_keys = c; }, "0x66"); // Numpad 6
    DMK::Config::register_key_combo("CameraProfiles", "OffsetXDecKey", "Offset X Decrease Key",
                                    [](const DMK::Config::KeyComboList &c) { g_config.offset_x_dec_keys = c; }, "0x64"); // Numpad 4
    DMK::Config::register_key_combo("CameraProfiles", "OffsetYIncKey", "Offset Y Increase Key",
                                    [](const DMK::Config::KeyComboList &c) { g_config.offset_y_inc_keys = c; }, "0x6B"); // Numpad +
    DMK::Config::register_key_combo("CameraProfiles", "OffsetYDecKey", "Offset Y Decrease Key",
                                    [](const DMK::Config::KeyComboList &c) { g_config.offset_y_dec_keys = c; }, "0x6D"); // Numpad -
    DMK::Config::register_key_combo("CameraProfiles", "OffsetZIncKey", "Offset Z Increase Key",
                                    [](const DMK::Config::KeyComboList &c) { g_config.offset_z_inc_keys = c; }, "0x68"); // Numpad 8
    DMK::Config::register_key_combo("CameraProfiles", "OffsetZDecKey", "Offset Z Decrease Key",
                                    [](const DMK::Config::KeyComboList &c) { g_config.offset_z_dec_keys = c; }, "0x62"); // Numpad 2
}

} // namespace TPVToggle
