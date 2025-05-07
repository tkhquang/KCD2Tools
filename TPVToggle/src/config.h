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

/**
 * @struct Config
 * @brief Holds application settings parsed from the configuration INI file.
 *
 * Stores hotkey bindings, logging level, and optional features like overlay
 * detection and custom TPV FOV settings.
 */
struct Config
{
    // Key binding lists (populated from INI's [Settings] section).
    // Store VK codes as integers.
    std::vector<int> toggle_keys; /**< VK codes that toggle between FPV/TPV. */
    std::vector<int> fpv_keys;    /**< VK codes that force First Person View. */
    std::vector<int> tpv_keys;    /**< VK codes that force Third Person View. */

    // Other configurable settings from INI.
    std::string log_level; /**< Logging level as string (e.g., "INFO", "DEBUG"). */

    // Optional features
    bool enable_overlay_feature; /**< Enable overlay detection and handling. */
    float tpv_fov_degrees;       /**< Custom TPV FOV in degrees; -1.0f if disabled. */

    // TPV Camera Offset Settings
    float tpv_offset_x;
    float tpv_offset_y;
    float tpv_offset_z;

    // Camera profile system
    bool enable_camera_profiles;         // Master toggle for camera profile system
    std::vector<int> master_toggle_keys; // Keys to enable/disable adjustment mode
    std::vector<int> profile_save_keys;  // Keys to save current profile
    std::vector<int> profile_cycle_keys; // Keys to cycle through profiles
    std::vector<int> profile_reset_keys; // Keys to reset offsets to 0

    // Offset adjustment keys
    std::vector<int> offset_x_inc_keys; // Keys to increase X offset
    std::vector<int> offset_x_dec_keys; // Keys to decrease X offset
    std::vector<int> offset_y_inc_keys; // Keys to increase Y offset
    std::vector<int> offset_y_dec_keys; // Keys to decrease Y offset
    std::vector<int> offset_z_inc_keys; // Keys to increase Z offset
    std::vector<int> offset_z_dec_keys; // Keys to decrease Z offset

    // Adjustment settings
    float offset_adjustment_step;  // How much to adjust per keypress
    std::string profile_directory; // Directory to store camera profiles

    // Transition settings
    float transition_duration;
    bool use_spring_physics;
    float spring_strength;
    float spring_damping;

    /**
     * @brief Default constructor. Initializes members to default states
     *        (empty vectors, default settings). The loading function is
     *        responsible for populating with defaults or INI values.
     */
    Config() : enable_overlay_feature(true), tpv_fov_degrees(-1.0f), tpv_offset_x(0.0f), tpv_offset_y(0.0f), tpv_offset_z(0.0f) {}
};

/**
 * @brief Loads configuration settings from an INI file using SimpleIni.
 * @details Finds the INI file relative to the mod DLL's location. Parses the
 *          [Settings] section for keys like "ToggleKey", "LogLevel", etc.
 *          Applies default values for missing or invalid entries.
 *          Performs validation on parsed values (e.g., key formats, log level).
 *          Logs progress and errors using the global Logger instance.
 * @param ini_filename Base filename of the configuration file (e.g.,
 * "KCD2_TPVToggle.ini"). The function will attempt to locate this file
 * adjacent to the mod's DLL.
 * @return Config Structure containing the loaded configuration settings,
 *         populated with either values from the INI file or defaults.
 */
Config loadConfig(const std::string &ini_filename);

#endif // CONFIG_H
