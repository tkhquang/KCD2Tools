/**
 * @file config.h
 * @brief Defines configuration structure and the loading function prototype.
 *
 * Contains the `Config` struct used to hold application settings loaded from
 * the INI configuration file (e.g., hotkeys, logging level, optional features).
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <vector>
#include <string>

#include <DetourModKit.hpp>

/**
 * @struct Config
 * @brief Holds application settings parsed from the configuration INI file.
 *
 * Stores hotkey bindings, logging level, and optional features like overlay
 * detection and custom TPV FOV settings.
 */
struct Config
{
    // Key bindings (populated from INI via DMKConfig::register_key_combo).
    DMKKeyComboList toggle_keys; /**< Keys that toggle between FPV/TPV. */
    DMKKeyComboList fpv_keys;    /**< Keys that force First Person View. */
    DMKKeyComboList tpv_keys;    /**< Keys that force Third Person View. */

    // Other configurable settings from INI.
    std::string log_level; /**< Logging level as string (e.g., "INFO", "DEBUG"). */

    // Optional features
    bool enable_overlay_feature; /**< Enable overlay detection and handling. */
    float tpv_fov_degrees;       /**< Custom TPV FOV in degrees; -1.0f if disabled. */

    // Hold-key-to-scroll feature
    DMKKeyComboList hold_scroll_keys; /**< Keys that, when held, enable mouse wheel scrolling. */

    // TPV Camera Offset Settings
    float tpv_offset_x;
    float tpv_offset_y;
    float tpv_offset_z;

    // Camera profile system
    bool enable_camera_profiles;
    DMKKeyComboList master_toggle_keys;
    DMKKeyComboList profile_save_keys;
    DMKKeyComboList profile_cycle_keys;
    DMKKeyComboList profile_reset_keys;
    DMKKeyComboList profile_update_keys;
    DMKKeyComboList profile_delete_keys;

    // Offset adjustment keys
    DMKKeyComboList offset_x_inc_keys;
    DMKKeyComboList offset_x_dec_keys;
    DMKKeyComboList offset_y_inc_keys;
    DMKKeyComboList offset_y_dec_keys;
    DMKKeyComboList offset_z_inc_keys;
    DMKKeyComboList offset_z_dec_keys;

    // Adjustment settings
    float offset_adjustment_step;
    std::string profile_directory;

    // Transition settings
    float transition_duration;
    bool use_spring_physics;
    float spring_strength;
    float spring_damping;

    // TPV Camera sensitivity settings
    float tpv_pitch_sensitivity;
    float tpv_yaw_sensitivity;
    bool tpv_pitch_limits_enabled;
    float tpv_pitch_min;
    float tpv_pitch_max;

    // Overlay restore delay
    int overlay_restore_delay_ms;

    Config() : log_level("INFO"),
               enable_overlay_feature(true),
               tpv_fov_degrees(-1.0f),
               tpv_offset_x(0.0f),
               tpv_offset_y(0.0f),
               tpv_offset_z(0.0f),
               enable_camera_profiles(false),
               offset_adjustment_step(0.01f),
               transition_duration(0.3f),
               use_spring_physics(false),
               spring_strength(10.0f),
               spring_damping(0.8f),
               tpv_pitch_sensitivity(1.0f),
               tpv_yaw_sensitivity(1.0f),
               tpv_pitch_limits_enabled(false),
               tpv_pitch_min(-180.0f),
               tpv_pitch_max(180.0f),
               overlay_restore_delay_ms(200)
    {
    }
};

/**
 * @brief Loads configuration settings from an INI file using DetourModKit::Config.
 * @param ini_filename Base filename of the configuration file.
 * @return Config Structure containing the loaded configuration settings.
 */
Config loadConfig(const std::string &ini_filename);

#endif // CONFIG_H
