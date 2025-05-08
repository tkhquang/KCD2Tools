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

    // Initialize SimpleIni
    CSimpleIniA ini;
    ini.SetUnicode(false);
    ini.SetMultiKey(false);

    // Apply hardcoded defaults
    config.log_level = Constants::DEFAULT_LOG_LEVEL;
    config.enable_overlay_feature = true; // Default enabled
    config.tpv_fov_degrees = -1.0f;       // Default disabled

    // Load INI file
    SI_Error rc = ini.LoadFile(ini_path.c_str());
    if (rc < 0)
    {
        logger.log(LOG_ERROR, "Config: Failed to open INI file '" + ini_path + "'. Using default settings.");
    }
    else
    {
        logger.log(LOG_INFO, "Config: Successfully opened INI file.");

        // Load key bindings
        const char *value = ini.GetValue("Settings", "ToggleKey", nullptr);
        if (value)
        {
            config.toggle_keys = parseKeyList(value, logger, "ToggleKey");
        }

        value = ini.GetValue("Settings", "FPVKey", nullptr);
        if (value)
        {
            config.fpv_keys = parseKeyList(value, logger, "FPVKey");
        }

        value = ini.GetValue("Settings", "TPVKey", nullptr);
        if (value)
        {
            config.tpv_keys = parseKeyList(value, logger, "TPVKey");
        }

        // Load log level
        value = ini.GetValue("Settings", "LogLevel", nullptr);
        if (value)
        {
            config.log_level = value;
        }

        // Load optional features
        value = ini.GetValue("Settings", "EnableOverlayFeature", nullptr);
        if (value)
        {
            std::string val_str = value;
            std::transform(val_str.begin(), val_str.end(), val_str.begin(), ::tolower);
            config.enable_overlay_feature = (val_str == "true" || val_str == "1" || val_str == "yes");
        }

        value = ini.GetValue("Settings", "TpvFovDegrees", nullptr);
        if (value)
        {
            std::string val_str = value;
            std::string trimmed = trim(val_str);
            if (!trimmed.empty())
            {
                try
                {
                    float fov_degrees = std::stof(trimmed);
                    if (fov_degrees > 0.0f && fov_degrees <= 180.0f)
                    {
                        config.tpv_fov_degrees = fov_degrees;
                    }
                    else
                    {
                        logger.log(LOG_WARNING, "Config: Invalid TPV FOV value: " + trimmed + ". Must be between 0 and 180 degrees.");
                    }
                }
                catch (const std::exception &e)
                {
                    logger.log(LOG_WARNING, "Config: Failed to parse TpvFovDegrees: " + std::string(e.what()));
                }
            }
        }

        // New feature: Hold-Key-to-Scroll
        value = ini.GetValue("Settings", "HoldKeyToScroll", nullptr);
        if (value)
        {
            std::vector<int> keys = parseKeyList(value, logger, "HoldKeyToScroll");
            if (!keys.empty())
            {
                config.hold_scroll_keys = keys;
                logger.log(LOG_INFO, "Config: Hold-to-scroll key configured: " + format_vkcode_list(keys));
            }
        }

        auto parseFloat = [&](const char *key, float &target)
        {
            const char *val = ini.GetValue("Settings", key, "0.0");
            try
            {
                target = std::stof(val);
            }
            catch (const std::exception &e)
            {
                logger.log(LOG_WARNING, "Config: Failed to parse float value for '" + std::string(key) + "': " + std::string(val) + ". Using default 0.0. Error: " + e.what());
                target = 0.0f;
            }
        };

        parseFloat("TpvOffsetX", config.tpv_offset_x);
        parseFloat("TpvOffsetY", config.tpv_offset_y);
        parseFloat("TpvOffsetZ", config.tpv_offset_z);

        // Camera profile system configuration
        bool enableProfiles = false;
        value = ini.GetValue("CameraProfiles", "Enable", "false");
        if (value)
        {
            std::string val_str = value;
            std::transform(val_str.begin(), val_str.end(), val_str.begin(), ::tolower);
            enableProfiles = (val_str == "true" || val_str == "1" || val_str == "yes");
        }
        config.enable_camera_profiles = enableProfiles;

        // Load adjustment step
        value = ini.GetValue("CameraProfiles", "AdjustmentStep", "0.05");
        if (value)
        {
            try
            {
                config.offset_adjustment_step = std::stof(value);
            }
            catch (...)
            {
                config.offset_adjustment_step = 0.05f; // Default
            }
        }
        else
        {
            config.offset_adjustment_step = 0.05f; // Default
        }

        std::string runtimeDirectory = getRuntimeDirectory();

        if (!runtimeDirectory.empty())
        {
            std::filesystem::path profilePath = std::filesystem::path(runtimeDirectory) / "KCD2_TPVToggle_Profiles";
            config.profile_directory = profilePath.string();
        }
        else
        {
            config.profile_directory = "KCD2_TPVToggle_Profiles";
        }

        // Parse key lists for camera profile system - With corrected defaults matching the INI file
        config.master_toggle_keys = parseKeyList(ini.GetValue("CameraProfiles", "MasterToggleKey", "0x7A"), logger, "MasterToggleKey"); // F11
        config.profile_save_keys = parseKeyList(ini.GetValue("CameraProfiles", "ProfileSaveKey", "0x61"), logger, "ProfileSaveKey");    // Numpad 1
        config.profile_cycle_keys = parseKeyList(ini.GetValue("CameraProfiles", "ProfileCycleKey", "0x63"), logger, "ProfileCycleKey"); // Numpad 3
        config.profile_reset_keys = parseKeyList(ini.GetValue("CameraProfiles", "ProfileResetKey", "0x65"), logger, "ProfileResetKey"); // Numpad 5

        // Adjustment keys for X, Y, Z offsets
        config.offset_x_inc_keys = parseKeyList(ini.GetValue("CameraProfiles", "OffsetXIncKey", "0x66"), logger, "OffsetXIncKey"); // Numpad 6
        config.offset_x_dec_keys = parseKeyList(ini.GetValue("CameraProfiles", "OffsetXDecKey", "0x64"), logger, "OffsetXDecKey"); // Numpad 4
        config.offset_y_inc_keys = parseKeyList(ini.GetValue("CameraProfiles", "OffsetYIncKey", "0x6B"), logger, "OffsetYIncKey"); // Numpad Plus
        config.offset_y_dec_keys = parseKeyList(ini.GetValue("CameraProfiles", "OffsetYDecKey", "0x6D"), logger, "OffsetYDecKey"); // Numpad Minus
        config.offset_z_inc_keys = parseKeyList(ini.GetValue("CameraProfiles", "OffsetZIncKey", "0x68"), logger, "OffsetZIncKey"); // Numpad 8
        config.offset_z_dec_keys = parseKeyList(ini.GetValue("CameraProfiles", "OffsetZDecKey", "0x62"), logger, "OffsetZDecKey"); // Numpad 2

        // Load transition settings
        config.transition_duration = 0.5f; // Default 0.5 seconds
        config.use_spring_physics = false; // Default disabled
        config.spring_strength = 10.0f;    // Default spring strength
        config.spring_damping = 0.8f;      // Default damping factor

        value = ini.GetValue("CameraProfiles", "TransitionDuration", "0.5");
        if (value)
        {
            try
            {
                config.transition_duration = std::stof(value);
            }
            catch (...)
            {
                config.transition_duration = 0.5f;
            }
        }

        // value = ini.GetValue("CameraProfiles", "UseSpringPhysics", "false");
        // if (value)
        // {
        //     std::string val_str = value;
        //     std::transform(val_str.begin(), val_str.end(), val_str.begin(), ::tolower);
        //     config.use_spring_physics = (val_str == "true" || val_str == "1" || val_str == "yes");
        // }

        // value = ini.GetValue("CameraProfiles", "SpringStrength", "10.0");
        // if (value)
        // {
        //     try
        //     {
        //         config.spring_strength = std::stof(value);
        //     }
        //     catch (...)
        //     {
        //         config.spring_strength = 10.0f;
        //     }
        // }

        // value = ini.GetValue("CameraProfiles", "SpringDamping", "0.8");
        // if (value)
        // {
        //     try
        //     {
        //         config.spring_damping = std::stof(value);
        //     }
        //     catch (...)
        //     {
        //         config.spring_damping = 0.8f;
        //     }
        // }
    }

    // Validate log level
    std::string effective_log_level = config.log_level;
    std::transform(effective_log_level.begin(), effective_log_level.end(), effective_log_level.begin(), ::toupper);
    if (effective_log_level == "DEBUG")
        config.log_level = "DEBUG";
    else if (effective_log_level == "INFO")
        config.log_level = "INFO";
    else if (effective_log_level == "WARNING")
        config.log_level = "WARNING";
    else if (effective_log_level == "ERROR")
        config.log_level = "ERROR";
    else
    {
        logger.log(LOG_WARNING, "Config: Invalid LogLevel value '" + config.log_level + "'. Using default: '" + Constants::DEFAULT_LOG_LEVEL + "'.");
        config.log_level = Constants::DEFAULT_LOG_LEVEL;
    }

    // Log summary
    logger.log(LOG_INFO, "Config: Log level set to: " + config.log_level);
    logger.log(LOG_INFO, "Config: Overlay feature: " + std::string(config.enable_overlay_feature ? "ENABLED" : "DISABLED"));

    if (config.tpv_fov_degrees > 0.0f)
    {
        logger.log(LOG_INFO, "Config: TPV FOV set to: " + std::to_string(config.tpv_fov_degrees) + " degrees");
    }
    else
    {
        logger.log(LOG_INFO, "Config: TPV FOV feature: DISABLED");
    }

    logger.log(LOG_INFO, "Config: TPV Offset (X, Y, Z): (" + std::to_string(config.tpv_offset_x) + ", " + std::to_string(config.tpv_offset_y) + ", " + std::to_string(config.tpv_offset_z) + ")");

    if (!config.hold_scroll_keys.empty())
    {
        logger.log(LOG_INFO, "Config: Hold-to-scroll feature ENABLED with key(s): " + format_vkcode_list(config.hold_scroll_keys));
    }
    else
    {
        logger.log(LOG_INFO, "Config: Hold-to-scroll feature DISABLED");
    }

    logger.log(LOG_INFO, "Config: Loaded hotkeys (Toggle:" + std::to_string(config.toggle_keys.size()) +
                             "/FPV:" + std::to_string(config.fpv_keys.size()) +
                             "/TPV:" + std::to_string(config.tpv_keys.size()) + ")");

    // Log camera profile config
    if (config.enable_camera_profiles)
    {
        logger.log(LOG_INFO, "Config: Camera profile system ENABLED");
        logger.log(LOG_INFO, "Config: Camera profile directory: " + config.profile_directory);
        logger.log(LOG_INFO, "Config: Camera adjustment step: " + std::to_string(config.offset_adjustment_step));
    }
    else
    {
        logger.log(LOG_INFO, "Config: Camera profile system DISABLED");
    }

    // Log transition settings if camera profiles enabled
    if (config.enable_camera_profiles)
    {
        logger.log(LOG_INFO, "Config: Transition duration: " + std::to_string(config.transition_duration) + "s");
        logger.log(LOG_INFO, "Config: Spring physics: " + std::string(config.use_spring_physics ? "ENABLED" : "DISABLED"));
        if (config.use_spring_physics)
        {
            logger.log(LOG_INFO, "Config: Spring strength: " + std::to_string(config.spring_strength));
            logger.log(LOG_INFO, "Config: Spring damping: " + std::to_string(config.spring_damping));
        }
    }

    logger.log(LOG_INFO, "Config: Configuration loading completed.");
    return config;
}
