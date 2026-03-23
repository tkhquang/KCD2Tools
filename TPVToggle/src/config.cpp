/**
 * @file config.cpp
 * @brief Implementation of configuration loading using DetourModKit::Config.
 *
 * Uses DMKConfig's callback-based registration system for loading settings from INI.
 */

#include "config.h"
#include "constants.h"

#include <DetourModKit.hpp>

#include <windows.h>
#include <filesystem>
#include <string>

using DetourModKit::LogLevel;

/**
 * @brief Formats a DMKKeyCombo for log display.
 * @param combo The key combo to format.
 * @return std::string Human-readable representation (e.g., "[F3, F4]" or "Ctrl+[F3]").
 */
static std::string formatKeyCombo(const DMKKeyCombo &combo)
{
    if (combo.keys.empty() && combo.modifiers.empty())
        return "[]";

    std::string result;

    // Format modifiers
    for (size_t i = 0; i < combo.modifiers.size(); ++i)
    {
        if (i > 0)
            result += "+";
        result += DMKFormat::format_hex(combo.modifiers[i].code, 2);
    }

    if (!combo.modifiers.empty() && !combo.keys.empty())
        result += "+";

    // Format trigger keys
    result += "[";
    for (size_t i = 0; i < combo.keys.size(); ++i)
    {
        if (i > 0)
            result += ", ";
        result += DMKFormat::format_hex(combo.keys[i].code, 2);
    }
    result += "]";

    return result;
}

static std::string formatKeyComboList(const DMKKeyComboList &list)
{
    if (list.empty())
        return "[]";

    std::string result;
    for (size_t i = 0; i < list.size(); ++i)
    {
        if (i > 0)
            result += ", ";
        result += formatKeyCombo(list[i]);
    }
    return result;
}

/**
 * @brief Loads and validates configuration settings from the specified INI file.
 * @param ini_filename Base name of the INI file (e.g., "KCD2_TPVToggle.ini").
 * @return Config Structure containing loaded settings, using defaults where necessary.
 */
Config loadConfig(const std::string &ini_filename)
{
    Config config;
    DMKLogger &logger = DMKLogger::get_instance();

    logger.log(LogLevel::Info, "Config: Registering configuration variables with DetourModKit...");

    // --- Register all configuration variables with DMKConfig ---

    // [Settings] Section - String and bool configs
    DMKConfig::register_string("Settings", "LogLevel", "Log Level",
        [&config](const std::string &v) { config.log_level = v; }, Constants::DEFAULT_LOG_LEVEL);
    DMKConfig::register_bool("Settings", "EnableOverlayFeature", "Enable Overlay Feature",
        [&config](bool v) { config.enable_overlay_feature = v; }, true);

    // [Settings] Section - Float configs
    DMKConfig::register_float("Settings", "TpvFovDegrees", "TPV FOV Degrees",
        [&config](float v) { config.tpv_fov_degrees = v; }, -1.0f);
    DMKConfig::register_float("Settings", "TpvOffsetX", "TPV Offset X",
        [&config](float v) { config.tpv_offset_x = v; }, 0.0f);
    DMKConfig::register_float("Settings", "TpvOffsetY", "TPV Offset Y",
        [&config](float v) { config.tpv_offset_y = v; }, 0.0f);
    DMKConfig::register_float("Settings", "TpvOffsetZ", "TPV Offset Z",
        [&config](float v) { config.tpv_offset_z = v; }, 0.0f);

    // [CameraSensitivity] Section
    DMKConfig::register_float("CameraSensitivity", "PitchSensitivity", "Pitch Sensitivity",
        [&config](float v) { config.tpv_pitch_sensitivity = v; }, 1.0f);
    DMKConfig::register_float("CameraSensitivity", "YawSensitivity", "Yaw Sensitivity",
        [&config](float v) { config.tpv_yaw_sensitivity = v; }, 1.0f);
    DMKConfig::register_bool("CameraSensitivity", "EnablePitchLimits", "Enable Pitch Limits",
        [&config](bool v) { config.tpv_pitch_limits_enabled = v; }, false);
    DMKConfig::register_float("CameraSensitivity", "PitchMin", "Pitch Minimum",
        [&config](float v) { config.tpv_pitch_min = v; }, -180.0f);
    DMKConfig::register_float("CameraSensitivity", "PitchMax", "Pitch Maximum",
        [&config](float v) { config.tpv_pitch_max = v; }, 180.0f);

    // [CameraProfiles] Section
    DMKConfig::register_bool("CameraProfiles", "Enable", "Enable Camera Profiles",
        [&config](bool v) { config.enable_camera_profiles = v; }, false);
    DMKConfig::register_float("CameraProfiles", "AdjustmentStep", "Adjustment Step",
        [&config](float v) { config.offset_adjustment_step = v; }, 0.05f);
    DMKConfig::register_float("CameraProfiles", "TransitionDuration", "Transition Duration",
        [&config](float v) { config.transition_duration = v; }, 0.5f);
    DMKConfig::register_bool("CameraProfiles", "UseSpringPhysics", "Use Spring Physics",
        [&config](bool v) { config.use_spring_physics = v; }, false);
    DMKConfig::register_float("CameraProfiles", "SpringStrength", "Spring Strength",
        [&config](float v) { config.spring_strength = v; }, 8.0f);
    DMKConfig::register_float("CameraProfiles", "SpringDamping", "Spring Damping",
        [&config](float v) { config.spring_damping = v; }, 0.7f);
    DMKConfig::register_string("CameraProfiles", "ProfileDirectory", "Profile Directory",
        [&config](const std::string &v) { config.profile_directory = v; }, "");

    // [Settings] Section - Integer configs
    DMKConfig::register_int("Settings", "OverlayRestoreDelayMs", "Overlay Restore Delay (ms)",
        [&config](int v) { config.overlay_restore_delay_ms = v; }, 200);

    // [Settings] Section - Key combos
    DMKConfig::register_key_combo("Settings", "ToggleKey", "Toggle Key",
        [&config](const DMKKeyComboList &c) { config.toggle_keys = c; }, "0x72"); // F3
    DMKConfig::register_key_combo("Settings", "FPVKey", "FPV Key",
        [&config](const DMKKeyComboList &c) { config.fpv_keys = c; }, "");
    DMKConfig::register_key_combo("Settings", "TPVKey", "TPV Key",
        [&config](const DMKKeyComboList &c) { config.tpv_keys = c; }, "");
    DMKConfig::register_key_combo("Settings", "HoldKeyToScroll", "Hold Key To Scroll",
        [&config](const DMKKeyComboList &c) { config.hold_scroll_keys = c; }, "");

    // [CameraProfiles] Section - Key combos
    DMKConfig::register_key_combo("CameraProfiles", "MasterToggleKey", "Master Toggle Key",
        [&config](const DMKKeyComboList &c) { config.master_toggle_keys = c; }, "0x7A");    // F11
    DMKConfig::register_key_combo("CameraProfiles", "ProfileSaveKey", "Profile Save Key",
        [&config](const DMKKeyComboList &c) { config.profile_save_keys = c; }, "0x61");     // Numpad 1
    DMKConfig::register_key_combo("CameraProfiles", "ProfileCycleKey", "Profile Cycle Key",
        [&config](const DMKKeyComboList &c) { config.profile_cycle_keys = c; }, "0x63");    // Numpad 3
    DMKConfig::register_key_combo("CameraProfiles", "ProfileResetKey", "Profile Reset Key",
        [&config](const DMKKeyComboList &c) { config.profile_reset_keys = c; }, "0x65");    // Numpad 5
    DMKConfig::register_key_combo("CameraProfiles", "ProfileUpdateKey", "Profile Update Key",
        [&config](const DMKKeyComboList &c) { config.profile_update_keys = c; }, "0x67");   // Numpad 7
    DMKConfig::register_key_combo("CameraProfiles", "ProfileDeleteKey", "Profile Delete Key",
        [&config](const DMKKeyComboList &c) { config.profile_delete_keys = c; }, "0x69");   // Numpad 9
    DMKConfig::register_key_combo("CameraProfiles", "OffsetXIncKey", "Offset X Increase Key",
        [&config](const DMKKeyComboList &c) { config.offset_x_inc_keys = c; }, "0x66");     // Numpad 6
    DMKConfig::register_key_combo("CameraProfiles", "OffsetXDecKey", "Offset X Decrease Key",
        [&config](const DMKKeyComboList &c) { config.offset_x_dec_keys = c; }, "0x64");     // Numpad 4
    DMKConfig::register_key_combo("CameraProfiles", "OffsetYIncKey", "Offset Y Increase Key",
        [&config](const DMKKeyComboList &c) { config.offset_y_inc_keys = c; }, "0x6B");     // Numpad +
    DMKConfig::register_key_combo("CameraProfiles", "OffsetYDecKey", "Offset Y Decrease Key",
        [&config](const DMKKeyComboList &c) { config.offset_y_dec_keys = c; }, "0x6D");     // Numpad -
    DMKConfig::register_key_combo("CameraProfiles", "OffsetZIncKey", "Offset Z Increase Key",
        [&config](const DMKKeyComboList &c) { config.offset_z_inc_keys = c; }, "0x68");     // Numpad 8
    DMKConfig::register_key_combo("CameraProfiles", "OffsetZDecKey", "Offset Z Decrease Key",
        [&config](const DMKKeyComboList &c) { config.offset_z_dec_keys = c; }, "0x62");     // Numpad 2

    // Load configuration using DMKConfig (handles all registered variables)
    DMKConfig::load(ini_filename);

    // Set profile directory if not set by DMKConfig
    if (config.profile_directory.empty())
    {
        config.profile_directory = DMKFilesystem::get_runtime_directory();
        if (config.profile_directory.empty())
        {
            config.profile_directory = ".";
        }
    }

    // --- Log Summary ---
    logger.log(LogLevel::Info, "Config: Log level set to: " + config.log_level);
    logger.log(LogLevel::Info, "Config: Overlay feature: " + std::string(config.enable_overlay_feature ? "ENABLED" : "DISABLED"));
    if (config.tpv_fov_degrees > 0.0f)
        logger.log(LogLevel::Info, "Config: TPV FOV: " + std::to_string(config.tpv_fov_degrees) + " deg");
    else
        logger.log(LogLevel::Info, "Config: TPV FOV: DISABLED");
    logger.log(LogLevel::Info, "Config: Base TPV Offset (X, Y, Z): (" + std::to_string(config.tpv_offset_x) + ", " + std::to_string(config.tpv_offset_y) + ", " + std::to_string(config.tpv_offset_z) + ")");
    logger.log(LogLevel::Info, "Config: Overlay restore delay: " + std::to_string(config.overlay_restore_delay_ms) + " ms");
    logger.log(LogLevel::Info, "Config: Hold-to-scroll keys: " + formatKeyComboList(config.hold_scroll_keys));
    logger.log(LogLevel::Info, "Config: TPV/FPV keys (Toggle:" + formatKeyComboList(config.toggle_keys) +
                             "/FPV:" + formatKeyComboList(config.fpv_keys) +
                             "/TPV:" + formatKeyComboList(config.tpv_keys) + ")");

    // Camera sensitivity system summary
    logger.log(LogLevel::Info, "Config: Camera Sensitivity Settings:");
    logger.log(LogLevel::Info, "  Pitch Sensitivity: " + std::to_string(config.tpv_pitch_sensitivity));
    logger.log(LogLevel::Info, "  Yaw Sensitivity: " + std::to_string(config.tpv_yaw_sensitivity));

    if (config.tpv_pitch_limits_enabled)
    {
        logger.log(LogLevel::Info, "  Pitch Limits: " + std::to_string(config.tpv_pitch_min) + " to " +
                                 std::to_string(config.tpv_pitch_max) + " (ENABLED)");
    }
    else
    {
        logger.log(LogLevel::Info, "  Pitch Limits: DISABLED");
    }

    // Camera profile system summary
    logger.log(LogLevel::Info, "Config: Camera Profile System: " + std::string(config.enable_camera_profiles ? "ENABLED" : "DISABLED"));
    if (config.enable_camera_profiles)
    {
        logger.log(LogLevel::Info, "  Profile Dir: " + config.profile_directory);
        logger.log(LogLevel::Info, "  Adjustment Step: " + std::to_string(config.offset_adjustment_step));
        logger.log(LogLevel::Info, "  Master Toggle: " + formatKeyComboList(config.master_toggle_keys));
        logger.log(LogLevel::Info, "  Create New Profile: " + formatKeyComboList(config.profile_save_keys));
        logger.log(LogLevel::Info, "  Update Active Profile: " + formatKeyComboList(config.profile_update_keys));
        logger.log(LogLevel::Info, "  Delete Active Profile: " + formatKeyComboList(config.profile_delete_keys));
        logger.log(LogLevel::Info, "  Cycle Profiles: " + formatKeyComboList(config.profile_cycle_keys));
        logger.log(LogLevel::Info, "  Reset to Default: " + formatKeyComboList(config.profile_reset_keys));
        logger.log(LogLevel::Info, "  Adjust X +/-: " + formatKeyComboList(config.offset_x_inc_keys) + "/" + formatKeyComboList(config.offset_x_dec_keys));
        logger.log(LogLevel::Info, "  Adjust Y +/-: " + formatKeyComboList(config.offset_y_inc_keys) + "/" + formatKeyComboList(config.offset_y_dec_keys));
        logger.log(LogLevel::Info, "  Adjust Z +/-: " + formatKeyComboList(config.offset_z_inc_keys) + "/" + formatKeyComboList(config.offset_z_dec_keys));
        logger.log(LogLevel::Info, "  Transition: " + std::to_string(config.transition_duration) + "s, Spring: " +
                                 (config.use_spring_physics ? "ON (Str:" + std::to_string(config.spring_strength) + ", Damp:" + std::to_string(config.spring_damping) + ")" : "OFF"));
    }

    logger.log(LogLevel::Info, "Config: Configuration loading completed.");
    return config;
}
