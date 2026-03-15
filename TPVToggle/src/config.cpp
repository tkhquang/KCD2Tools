/**
 * @file config.cpp
 * @brief Implementation of configuration loading using DetourModKit::Config.
 *
 * Uses DMKConfig's registration-based system for loading settings from INI.
 */

#include "config.h"
#include "logger.h"
#include "constants.h"

#include <DetourModKit.hpp>

#include <windows.h>
#include <filesystem>
#include <algorithm>
#include <string>

/**
 * @brief Loads and validates configuration settings from the specified INI file.
 * @param ini_filename Base name of the INI file (e.g., "KCD2_TPVToggle.ini").
 * @return Config Structure containing loaded settings, using defaults where necessary.
 * @note This implementation uses DMKConfig for all configuration loading including
 *       key lists via registerKeyList(). All settings are registered before calling
 *       DMKConfig::load() which handles the INI parsing automatically.
 */
Config loadConfig(const std::string &ini_filename)
{
    Config config;
    Logger &logger = Logger::getInstance();

    logger.log(LOG_INFO, "Config: Registering configuration variables with DetourModKit...");

    // --- Register all configuration variables with DMKConfig ---

    // [Settings] Section - String and bool configs
    DMKConfig::registerString("Settings", "LogLevel", "Log Level", config.log_level, Constants::DEFAULT_LOG_LEVEL);
    DMKConfig::registerBool("Settings", "EnableOverlayFeature", "Enable Overlay Feature", config.enable_overlay_feature, true);

    // [Settings] Section - Float configs
    float default_fov = -1.0f;
    DMKConfig::registerFloat("Settings", "TpvFovDegrees", "TPV FOV Degrees", config.tpv_fov_degrees, default_fov);
    DMKConfig::registerFloat("Settings", "TpvOffsetX", "TPV Offset X", config.tpv_offset_x, 0.0f);
    DMKConfig::registerFloat("Settings", "TpvOffsetY", "TPV Offset Y", config.tpv_offset_y, 0.0f);
    DMKConfig::registerFloat("Settings", "TpvOffsetZ", "TPV Offset Z", config.tpv_offset_z, 0.0f);

    // [CameraSensitivity] Section
    DMKConfig::registerFloat("CameraSensitivity", "PitchSensitivity", "Pitch Sensitivity", config.tpv_pitch_sensitivity, 1.0f);
    DMKConfig::registerFloat("CameraSensitivity", "YawSensitivity", "Yaw Sensitivity", config.tpv_yaw_sensitivity, 1.0f);
    DMKConfig::registerBool("CameraSensitivity", "EnablePitchLimits", "Enable Pitch Limits", config.tpv_pitch_limits_enabled, false);
    DMKConfig::registerFloat("CameraSensitivity", "PitchMin", "Pitch Minimum", config.tpv_pitch_min, -180.0f);
    DMKConfig::registerFloat("CameraSensitivity", "PitchMax", "Pitch Maximum", config.tpv_pitch_max, 180.0f);

    // [CameraProfiles] Section
    DMKConfig::registerBool("CameraProfiles", "Enable", "Enable Camera Profiles", config.enable_camera_profiles, false);
    DMKConfig::registerFloat("CameraProfiles", "AdjustmentStep", "Adjustment Step", config.offset_adjustment_step, 0.05f);
    DMKConfig::registerFloat("CameraProfiles", "TransitionDuration", "Transition Duration", config.transition_duration, 0.5f);
    DMKConfig::registerBool("CameraProfiles", "UseSpringPhysics", "Use Spring Physics", config.use_spring_physics, false);
    DMKConfig::registerFloat("CameraProfiles", "SpringStrength", "Spring Strength", config.spring_strength, 8.0f);
    DMKConfig::registerFloat("CameraProfiles", "SpringDamping", "Spring Damping", config.spring_damping, 0.7f);
    DMKConfig::registerString("CameraProfiles", "ProfileDirectory", "Profile Directory", config.profile_directory, "");

    // [Settings] Section - Key lists (using DMKConfig::registerKeyList)
    DMKConfig::registerKeyList("Settings", "ToggleKey", "Toggle Key", config.toggle_keys, "0x72"); // F3
    DMKConfig::registerKeyList("Settings", "FPVKey", "FPV Key", config.fpv_keys, "");
    DMKConfig::registerKeyList("Settings", "TPVKey", "TPV Key", config.tpv_keys, "");
    DMKConfig::registerKeyList("Settings", "HoldKeyToScroll", "Hold Key To Scroll", config.hold_scroll_keys, "");

    // [CameraProfiles] Section - Key lists
    DMKConfig::registerKeyList("CameraProfiles", "MasterToggleKey", "Master Toggle Key", config.master_toggle_keys, "0x7A");    // F11
    DMKConfig::registerKeyList("CameraProfiles", "ProfileSaveKey", "Profile Save Key", config.profile_save_keys, "0x61");       // Numpad 1
    DMKConfig::registerKeyList("CameraProfiles", "ProfileCycleKey", "Profile Cycle Key", config.profile_cycle_keys, "0x63");    // Numpad 3
    DMKConfig::registerKeyList("CameraProfiles", "ProfileResetKey", "Profile Reset Key", config.profile_reset_keys, "0x65");    // Numpad 5
    DMKConfig::registerKeyList("CameraProfiles", "ProfileUpdateKey", "Profile Update Key", config.profile_update_keys, "0x67"); // Numpad 7
    DMKConfig::registerKeyList("CameraProfiles", "ProfileDeleteKey", "Profile Delete Key", config.profile_delete_keys, "0x69"); // Numpad 9
    DMKConfig::registerKeyList("CameraProfiles", "OffsetXIncKey", "Offset X Increase Key", config.offset_x_inc_keys, "0x66");   // Numpad 6
    DMKConfig::registerKeyList("CameraProfiles", "OffsetXDecKey", "Offset X Decrease Key", config.offset_x_dec_keys, "0x64");   // Numpad 4
    DMKConfig::registerKeyList("CameraProfiles", "OffsetYIncKey", "Offset Y Increase Key", config.offset_y_inc_keys, "0x6B");   // Numpad +
    DMKConfig::registerKeyList("CameraProfiles", "OffsetYDecKey", "Offset Y Decrease Key", config.offset_y_dec_keys, "0x6D");   // Numpad -
    DMKConfig::registerKeyList("CameraProfiles", "OffsetZIncKey", "Offset Z Increase Key", config.offset_z_inc_keys, "0x68");   // Numpad 8
    DMKConfig::registerKeyList("CameraProfiles", "OffsetZDecKey", "Offset Z Decrease Key", config.offset_z_dec_keys, "0x62");   // Numpad 2

    // Load configuration using DMKConfig (handles all registered variables including key lists)
    DMKConfig::load(ini_filename);

    // Set profile directory if not set by DMKConfig
    if (config.profile_directory.empty())
    {
        config.profile_directory = DMKFilesystem::getRuntimeDirectory();
        if (config.profile_directory.empty())
        {
            config.profile_directory = ".";
        }
    }

    // Validate Log Level
    std::string upper_log_level = config.log_level;
    std::transform(upper_log_level.begin(), upper_log_level.end(), upper_log_level.begin(), ::toupper);
    if (upper_log_level == "TRACE")
        config.log_level = "TRACE";
    else if (upper_log_level == "DEBUG")
        config.log_level = "DEBUG";
    else if (upper_log_level == "INFO")
        config.log_level = "INFO";
    else if (upper_log_level == "WARNING")
        config.log_level = "WARNING";
    else if (upper_log_level == "ERROR")
        config.log_level = "ERROR";
    else
    {
        logger.log(LOG_WARNING, "Config: Invalid LogLevel '" + config.log_level + "'. Using default: '" + Constants::DEFAULT_LOG_LEVEL + "'.");
        config.log_level = Constants::DEFAULT_LOG_LEVEL;
    }

    // --- Log Summary ---
    logger.log(LOG_INFO, "Config: Log level set to: " + config.log_level);
    logger.log(LOG_INFO, "Config: Overlay feature: " + std::string(config.enable_overlay_feature ? "ENABLED" : "DISABLED"));
    if (config.tpv_fov_degrees > 0.0f)
        logger.log(LOG_INFO, "Config: TPV FOV: " + std::to_string(config.tpv_fov_degrees) + " deg");
    else
        logger.log(LOG_INFO, "Config: TPV FOV: DISABLED");
    logger.log(LOG_INFO, "Config: Base TPV Offset (X, Y, Z): (" + std::to_string(config.tpv_offset_x) + ", " + std::to_string(config.tpv_offset_y) + ", " + std::to_string(config.tpv_offset_z) + ")");
    logger.log(LOG_INFO, "Config: Hold-to-scroll keys: " + DMKString::format_vkcode_list(config.hold_scroll_keys));
    logger.log(LOG_INFO, "Config: TPV/FPV keys (Toggle:" + DMKString::format_vkcode_list(config.toggle_keys) +
                             "/FPV:" + DMKString::format_vkcode_list(config.fpv_keys) +
                             "/TPV:" + DMKString::format_vkcode_list(config.tpv_keys) + ")");

    // Camera sensitivity system summary
    logger.log(LOG_INFO, "Config: Camera Sensitivity Settings:");
    logger.log(LOG_INFO, "  Pitch Sensitivity: " + std::to_string(config.tpv_pitch_sensitivity));
    logger.log(LOG_INFO, "  Yaw Sensitivity: " + std::to_string(config.tpv_yaw_sensitivity));

    if (config.tpv_pitch_limits_enabled)
    {
        logger.log(LOG_INFO, "  Pitch Limits: " + std::to_string(config.tpv_pitch_min) + "° to " +
                                 std::to_string(config.tpv_pitch_max) + "° (ENABLED)");
    }
    else
    {
        logger.log(LOG_INFO, "  Pitch Limits: DISABLED");
    }

    // Camera profile system summary
    logger.log(LOG_INFO, "Config: Camera Profile System: " + std::string(config.enable_camera_profiles ? "ENABLED" : "DISABLED"));
    if (config.enable_camera_profiles)
    {
        logger.log(LOG_INFO, "  Profile Dir: " + config.profile_directory);
        logger.log(LOG_INFO, "  Adjustment Step: " + std::to_string(config.offset_adjustment_step));
        logger.log(LOG_INFO, "  Master Toggle: " + DMKString::format_vkcode_list(config.master_toggle_keys));
        logger.log(LOG_INFO, "  Create New Profile: " + DMKString::format_vkcode_list(config.profile_save_keys));
        logger.log(LOG_INFO, "  Update Active Profile: " + DMKString::format_vkcode_list(config.profile_update_keys));
        logger.log(LOG_INFO, "  Delete Active Profile: " + DMKString::format_vkcode_list(config.profile_delete_keys));
        logger.log(LOG_INFO, "  Cycle Profiles: " + DMKString::format_vkcode_list(config.profile_cycle_keys));
        logger.log(LOG_INFO, "  Reset to Default: " + DMKString::format_vkcode_list(config.profile_reset_keys));
        logger.log(LOG_INFO, "  Adjust X +/-: " + DMKString::format_vkcode_list(config.offset_x_inc_keys) + "/" + DMKString::format_vkcode_list(config.offset_x_dec_keys));
        logger.log(LOG_INFO, "  Adjust Y +/-: " + DMKString::format_vkcode_list(config.offset_y_inc_keys) + "/" + DMKString::format_vkcode_list(config.offset_y_dec_keys));
        logger.log(LOG_INFO, "  Adjust Z +/-: " + DMKString::format_vkcode_list(config.offset_z_inc_keys) + "/" + DMKString::format_vkcode_list(config.offset_z_dec_keys));
        logger.log(LOG_INFO, "  Transition: " + std::to_string(config.transition_duration) + "s, Spring: " +
                                 (config.use_spring_physics ? "ON (Str:" + std::to_string(config.spring_strength) + ", Damp:" + std::to_string(config.spring_damping) + ")" : "OFF"));
    }

    logger.log(LOG_INFO, "Config: Configuration loading completed.");
    return config;
}
