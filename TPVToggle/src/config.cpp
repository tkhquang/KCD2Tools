/**
 * @file config.cpp
 * @brief Implementation of configuration loading using SimpleIni.
 *
 * Reads settings (hotkeys, log level, optional features) from an INI file,
 * validates them, applies defaults, and handles path finding relative to DLL.
 */

#include "config.h"
#include "logger.h"
#include "constants.h"
#include "utils.h"

#include <windows.h>
#include <filesystem>
#include <cctype>
#include <algorithm>
#include <string>
#include <stdexcept>

// SimpleIni headers
#include "SimpleIni.h"

/**
 * @brief Determines the full absolute path for the INI configuration file.
 * @details Locates the INI file in the same directory as the currently
 *          running module (DLL/ASI). Uses C++17 filesystem and WinAPI.
 *          Falls back to using just the provided filename if path
 *          determination fails.
 * @param ini_filename Base name of the INI file (e.g., "KCD2_TPVToggle.ini").
 * @return std::string Full path to the INI file if successful, otherwise
 *         returns the input `ini_filename` as a fallback.
 */
static std::string getIniFilePath(const std::string &ini_filename)
{
    Logger &logger = Logger::getInstance();
    char dll_path_buf[MAX_PATH] = {0};
    HMODULE h_self = NULL;

    try
    {
        // Get a handle to this specific module (the DLL/ASI).
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCSTR)&getIniFilePath,
                                &h_self) ||
            h_self == NULL)
        {
            throw std::runtime_error("GetModuleHandleExA failed: Error " +
                                     std::to_string(GetLastError()));
        }

        // Get the full path of the loaded module.
        DWORD len = GetModuleFileNameA(h_self, dll_path_buf, MAX_PATH);
        if (len == 0)
        {
            throw std::runtime_error("GetModuleFileNameA failed (len=0): Error " +
                                     std::to_string(GetLastError()));
        }
        else if (len == MAX_PATH && GetLastError() == ERROR_INSUFFICIENT_BUFFER)
        {
            throw std::runtime_error("GetModuleFileNameA failed: Buffer too small.");
        }

        // Use std::filesystem to reliably get the parent directory.
        std::filesystem::path ini_path =
            std::filesystem::path(dll_path_buf).parent_path() / ini_filename;

        std::string full_path = ini_path.string();
        if (logger.isDebugEnabled())
        {
            logger.log(LOG_DEBUG, "Config: Determined INI path: " + full_path);
        }
        return full_path;
    }
    catch (const std::exception &e)
    {
        logger.log(LOG_WARNING, "Config: Error determining INI path based on DLL: " +
                                    std::string(e.what()) + ". Using relative path: " + ini_filename);
    }
    catch (...)
    {
        logger.log(LOG_WARNING, "Config: Unknown error determining path. Using relative: " + ini_filename);
    }

    return ini_filename; // Fallback to relative filename.
}

/**
 * @brief Parses a comma-separated string of hexadecimal VK codes from INI value.
 * @details Handles optional "0x" prefixes, trims whitespace, validates hex
 *          format for each token, and converts valid tokens to integer VK codes.
 *          Logs warnings for invalid tokens or codes outside typical range.
 * @param value_str The raw string value read from the INI file.
 * @param logger Reference to the logger for reporting parsing details/errors.
 * @param key_name The name of the INI key being parsed (e.g., "ToggleKey") for logs.
 * @return std::vector<int> A vector containing the valid integer VK codes found.
 *         Returns an empty vector if the input string is empty or contains no valid codes.
 */
static std::vector<int> parseKeyList(const std::string &value_str, Logger &logger,
                                     const std::string &key_name)
{
    std::vector<int> keys;

    // First, remove any inline comment (everything after semicolon)
    std::string str_no_comment = value_str;
    size_t comment_pos = str_no_comment.find(';');
    if (comment_pos != std::string::npos)
    {
        str_no_comment = str_no_comment.substr(0, comment_pos);
    }

    std::string trimmed_val = trim(str_no_comment);

    if (trimmed_val.empty())
    {
        return keys; // Return empty vector, not an error.
    }

    std::istringstream iss(trimmed_val);
    std::string token;
    if (logger.isDebugEnabled())
    {
        logger.log(LOG_DEBUG, "Config: Parsing '" + key_name + "': \"" + trimmed_val + "\"");
    }
    int token_idx = 0;

    while (std::getline(iss, token, ','))
    {
        token_idx++;
        // Strip any inline comments from individual tokens too
        size_t token_comment_pos = token.find(';');
        if (token_comment_pos != std::string::npos)
        {
            token = token.substr(0, token_comment_pos);
        }

        std::string trimmed_token = trim(token);
        if (trimmed_token.empty())
        {
            continue; // Ignore empty tokens
        }

        // Check for and remove optional "0x" or "0X" prefix.
        std::string hex_part = trimmed_token;
        if (hex_part.size() >= 2 && hex_part[0] == '0' && (hex_part[1] == 'x' || hex_part[1] == 'X'))
        {
            hex_part = hex_part.substr(2);
            if (hex_part.empty())
            {
                logger.log(LOG_WARNING, "Config: Invalid key token '" + token + "' (just prefix) in '" + key_name + "' at token " + std::to_string(token_idx));
                continue;
            }
        }

        // Validate hexadecimal format
        if (hex_part.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
        {
            logger.log(LOG_WARNING, "Config: Invalid non-hex character in key token '" + token + "' for '" + key_name + "' at token " + std::to_string(token_idx));
            continue;
        }

        // Convert to integer VK code
        try
        {
            unsigned long code_ul = std::stoul(hex_part, nullptr, 16);
            if (code_ul == 0 || code_ul > 0xFF)
            {
                logger.log(LOG_WARNING, "Config: Key code " + format_hex(static_cast<int>(code_ul)) +
                                            " ('" + token + "') for '" + key_name + "' is outside typical VK range (0x01-0xFF)");
            }
            int key_code = static_cast<int>(code_ul);
            keys.push_back(key_code);
            if (logger.isDebugEnabled())
            {
                logger.log(LOG_DEBUG, "Config: Added key for '" + key_name + "': " + format_vkcode(key_code));
            }
        }
        catch (const std::exception &e)
        {
            logger.log(LOG_WARNING, "Config: Error converting hex token '" + token + "' for '" + key_name + "': " + e.what());
        }
    }

    if (keys.empty() && !trimmed_val.empty())
    {
        logger.log(LOG_WARNING, "Config: Processed value for '" + key_name + "' (\"" + trimmed_val + "\") but found no valid key codes.");
    }

    return keys;
}

/**
 * @brief Loads and validates configuration settings from the specified INI file using SimpleIni.
 * @param ini_filename Base name of the INI file (e.g., "KCD2_TPVToggle.ini").
 * @return Config Structure containing loaded settings, using defaults where necessary.
 */
Config loadConfig(const std::string &ini_filename)
{
    Config config;
    Logger &logger = Logger::getInstance();

    std::string ini_path = getIniFilePath(ini_filename);
    logger.log(LOG_INFO, "Config: Attempting to load configuration from: " + ini_path);

    CSimpleIniA ini;
    ini.SetUnicode(false);  // Assuming ASCII/MBCS INI file
    ini.SetMultiKey(false); // Don't allow duplicate keys in sections

    // Apply hardcoded defaults explicitly BEFORE loading INI
    // (Some defaults are now set in Config constructor, but can reiterate here)
    config.log_level = Constants::DEFAULT_LOG_LEVEL;
    config.enable_overlay_feature = true;
    config.tpv_fov_degrees = -1.0f;
    // Initialize orbital camera defaults explicitly if not set by constructor for some members
    config.enable_orbital_camera_mode = false;
    config.orbit_sensitivity_yaw = 0.005f;
    config.orbit_sensitivity_pitch = 0.005f;
    config.orbit_invert_pitch = false;
    config.orbit_zoom_sensitivity = 0.01f;
    config.orbit_default_distance = 5.0f;
    config.orbit_min_distance = 1.0f;
    config.orbit_max_distance = 15.0f;
    config.orbit_pitch_min_degrees = -80.0f;
    config.orbit_pitch_max_degrees = 80.0f;

    SI_Error rc = ini.LoadFile(ini_path.c_str());
    if (rc < 0)
    {
        logger.log(LOG_ERROR, "Config: Failed to open INI file '" + ini_path + "'. Using default settings.");
        // Fallback: Ensure profile directory is set even if INI fails
        config.profile_directory = getRuntimeDirectory();
        if (config.profile_directory.empty())
        {
            config.profile_directory = "."; // Fallback
        }
    }
    else
    {
        logger.log(LOG_INFO, "Config: Successfully opened INI file.");

        // Helper lambda to read key lists with defaults
        auto load_key_list = [&](const char *key, std::vector<int> &target_vector, const char *default_value)
        {
            const char *value = ini.GetValue("CameraProfiles", key, default_value);
            if (value)
            {
                target_vector = parseKeyList(value, logger, key);
            }
            else
            {
                target_vector = parseKeyList(default_value, logger, std::string(key) + " (default)");
            }
        };

        // --- [Settings] Section ---
        // Basic Toggle/View keys
        config.toggle_keys = parseKeyList(ini.GetValue("Settings", "ToggleKey", "0x72"), logger, "ToggleKey"); // F3 default
        config.fpv_keys = parseKeyList(ini.GetValue("Settings", "FPVKey", ""), logger, "FPVKey");
        config.tpv_keys = parseKeyList(ini.GetValue("Settings", "TPVKey", ""), logger, "TPVKey");

        // Log Level
        config.log_level = ini.GetValue("Settings", "LogLevel", Constants::DEFAULT_LOG_LEVEL);

        // Features
        config.enable_overlay_feature = ini.GetBoolValue("Settings", "EnableOverlayFeature", true);
        config.tpv_fov_degrees = (float)ini.GetDoubleValue("Settings", "TpvFovDegrees", -1.0);
        config.hold_scroll_keys = parseKeyList(ini.GetValue("Settings", "HoldKeyToScroll", ""), logger, "HoldKeyToScroll");

        // TPV Offsets (using new defaults from constructor now)
        config.tpv_offset_x = (float)ini.GetDoubleValue("Settings", "TpvOffsetX", config.tpv_offset_x);
        config.tpv_offset_y = (float)ini.GetDoubleValue("Settings", "TpvOffsetY", config.tpv_offset_y);
        config.tpv_offset_z = (float)ini.GetDoubleValue("Settings", "TpvOffsetZ", config.tpv_offset_z);

        // --- [CameraProfiles] Section ---
        config.enable_camera_profiles = ini.GetBoolValue("CameraProfiles", "Enable", false);

        // Only load profile-specific keys if the feature is enabled
        if (config.enable_camera_profiles)
        {
            // Basic profile actions
            load_key_list("MasterToggleKey", config.master_toggle_keys, "0x7A"); // F11
            load_key_list("ProfileSaveKey", config.profile_save_keys, "0x61");   // Numpad 1 (CREATE NEW)
            load_key_list("ProfileCycleKey", config.profile_cycle_keys, "0x63"); // Numpad 3
            load_key_list("ProfileResetKey", config.profile_reset_keys, "0x65"); // Numpad 5

            // *** Load NEW Keys ***
            load_key_list("ProfileUpdateKey", config.profile_update_keys, "0x67"); // Numpad 7 (UPDATE) - Assign default
            load_key_list("ProfileDeleteKey", config.profile_delete_keys, "0x69"); // Numpad 9 (DELETE) - Assign default

            // Offset adjustments
            load_key_list("OffsetXIncKey", config.offset_x_inc_keys, "0x66"); // Numpad 6
            load_key_list("OffsetXDecKey", config.offset_x_dec_keys, "0x64"); // Numpad 4
            load_key_list("OffsetYIncKey", config.offset_y_inc_keys, "0x6B"); // Numpad +
            load_key_list("OffsetYDecKey", config.offset_y_dec_keys, "0x6D"); // Numpad -
            load_key_list("OffsetZIncKey", config.offset_z_inc_keys, "0x68"); // Numpad 8
            load_key_list("OffsetZDecKey", config.offset_z_dec_keys, "0x62"); // Numpad 2

            // Adjustment & Transition Settings
            config.offset_adjustment_step = (float)ini.GetDoubleValue("CameraProfiles", "AdjustmentStep", 0.05);
            config.transition_duration = (float)ini.GetDoubleValue("CameraProfiles", "TransitionDuration", 0.5);
            config.use_spring_physics = ini.GetBoolValue("CameraProfiles", "UseSpringPhysics", false);
            config.spring_strength = (float)ini.GetDoubleValue("CameraProfiles", "SpringStrength", 8.0);
            config.spring_damping = (float)ini.GetDoubleValue("CameraProfiles", "SpringDamping", 0.7);

            // Profile directory
            config.profile_directory = ini.GetValue("CameraProfiles", "ProfileDirectory", "");
            if (config.profile_directory.empty())
            { // If not set in INI, use runtime dir
                config.profile_directory = getRuntimeDirectory();
                if (config.profile_directory.empty())
                {
                    config.profile_directory = "."; // Fallback if runtime dir fails
                }
            }
        }
        else
        {
            // Ensure profile directory is still set even if feature disabled, for logger etc.
            config.profile_directory = getRuntimeDirectory();
            if (config.profile_directory.empty())
            {
                config.profile_directory = ".";
            }
        }

        // [OrbitalCamera] section
        config.enable_orbital_camera_mode = ini.GetBoolValue("OrbitalCamera", "EnableOrbitalCamera", config.enable_orbital_camera_mode);
        if (config.enable_orbital_camera_mode) // Load keys only if master orbital enable is true
        {
            config.orbital_mode_toggle_keys = parseKeyList(ini.GetValue("OrbitalCamera", "OrbitalModeToggleKey", "0x73"), logger, "OrbitalModeToggleKey"); // F4 default
            load_key_list("OrbitalModeToggleKey", config.orbital_mode_toggle_keys, "0x73");
        }
        config.orbit_sensitivity_yaw = (float)ini.GetDoubleValue("OrbitalCamera", "SensitivityYaw", config.orbit_sensitivity_yaw);
        config.orbit_sensitivity_pitch = (float)ini.GetDoubleValue("OrbitalCamera", "SensitivityPitch", config.orbit_sensitivity_pitch);
        config.orbit_invert_pitch = ini.GetBoolValue("OrbitalCamera", "InvertPitch", config.orbit_invert_pitch);
        config.orbit_zoom_sensitivity = (float)ini.GetDoubleValue("OrbitalCamera", "ZoomSensitivity", config.orbit_zoom_sensitivity);
        config.orbit_default_distance = (float)ini.GetDoubleValue("OrbitalCamera", "DefaultDistance", config.orbit_default_distance);
        config.orbit_min_distance = (float)ini.GetDoubleValue("OrbitalCamera", "MinDistance", config.orbit_min_distance);
        config.orbit_max_distance = (float)ini.GetDoubleValue("OrbitalCamera", "MaxDistance", config.orbit_max_distance);
        config.orbit_pitch_min_degrees = (float)ini.GetDoubleValue("OrbitalCamera", "PitchMinDegrees", config.orbit_pitch_min_degrees);
        config.orbit_pitch_max_degrees = (float)ini.GetDoubleValue("OrbitalCamera", "PitchMaxDegrees", config.orbit_pitch_max_degrees);
    } // end else (INI loaded successfully)

    // Validate Log Level
    std::string upper_log_level = config.log_level;
    std::transform(upper_log_level.begin(), upper_log_level.end(), upper_log_level.begin(), ::toupper);
    if (upper_log_level == "DEBUG")
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
    logger.log(LOG_INFO, "Config: Hold-to-scroll keys: " + format_vkcode_list(config.hold_scroll_keys));
    logger.log(LOG_INFO, "Config: TPV/FPV keys (Toggle:" + format_vkcode_list(config.toggle_keys) +
                             "/FPV:" + format_vkcode_list(config.fpv_keys) +
                             "/TPV:" + format_vkcode_list(config.tpv_keys) + ")");

    // Camera profile system summary
    logger.log(LOG_INFO, "Config: Camera Profile System: " + std::string(config.enable_camera_profiles ? "ENABLED" : "DISABLED"));
    if (config.enable_camera_profiles)
    {
        logger.log(LOG_INFO, "  Profile Dir: " + config.profile_directory);
        logger.log(LOG_INFO, "  Adjustment Step: " + std::to_string(config.offset_adjustment_step));
        logger.log(LOG_INFO, "  Master Toggle: " + format_vkcode_list(config.master_toggle_keys));
        logger.log(LOG_INFO, "  Create New Profile: " + format_vkcode_list(config.profile_save_keys));
        logger.log(LOG_INFO, "  Update Active Profile: " + format_vkcode_list(config.profile_update_keys)); // Log new key
        logger.log(LOG_INFO, "  Delete Active Profile: " + format_vkcode_list(config.profile_delete_keys)); // Log new key
        logger.log(LOG_INFO, "  Cycle Profiles: " + format_vkcode_list(config.profile_cycle_keys));
        logger.log(LOG_INFO, "  Reset to Default: " + format_vkcode_list(config.profile_reset_keys));
        logger.log(LOG_INFO, "  Adjust X +/-: " + format_vkcode_list(config.offset_x_inc_keys) + "/" + format_vkcode_list(config.offset_x_dec_keys));
        logger.log(LOG_INFO, "  Adjust Y +/-: " + format_vkcode_list(config.offset_y_inc_keys) + "/" + format_vkcode_list(config.offset_y_dec_keys));
        logger.log(LOG_INFO, "  Adjust Z +/-: " + format_vkcode_list(config.offset_z_inc_keys) + "/" + format_vkcode_list(config.offset_z_dec_keys));
        logger.log(LOG_INFO, "  Transition: " + std::to_string(config.transition_duration) + "s, Spring: " +
                                 (config.use_spring_physics ? "ON (Str:" + std::to_string(config.spring_strength) + ", Damp:" + std::to_string(config.spring_damping) + ")" : "OFF"));
    }

    logger.log(LOG_INFO, "Config: Orbital Camera System: " + std::string(config.enable_orbital_camera_mode ? "ENABLED" : "DISABLED"));
    if (config.enable_orbital_camera_mode)
    {
        logger.log(LOG_INFO, "  Orbital Mode Toggle: " + format_vkcode_list(config.orbital_mode_toggle_keys));
        logger.log(LOG_INFO, "  Orbital Sens (Yaw/Pitch/Zoom): " + std::to_string(config.orbit_sensitivity_yaw) + "/" + std::to_string(config.orbit_sensitivity_pitch) + "/" + std::to_string(config.orbit_zoom_sensitivity));
        logger.log(LOG_INFO, "  Orbital Pitch Invert: " + std::string(config.orbit_invert_pitch ? "Yes" : "No"));
        logger.log(LOG_INFO, "  Orbital Dist (Def/Min/Max): " + std::to_string(config.orbit_default_distance) + "/" + std::to_string(config.orbit_min_distance) + "/" + std::to_string(config.orbit_max_distance));
        logger.log(LOG_INFO, "  Orbital Pitch Limits (Min/Max Deg): " + std::to_string(config.orbit_pitch_min_degrees) + "/" + std::to_string(config.orbit_pitch_max_degrees));
    }

    logger.log(LOG_INFO, "Config: Configuration loading completed.");
    return config;
}
