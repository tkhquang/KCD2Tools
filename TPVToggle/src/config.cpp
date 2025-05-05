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
    std::string trimmed_val = trim(value_str);

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

    logger.log(LOG_INFO, "Config: Loaded hotkeys (Toggle:" + std::to_string(config.toggle_keys.size()) +
                             "/FPV:" + std::to_string(config.fpv_keys.size()) +
                             "/TPV:" + std::to_string(config.tpv_keys.size()) + ")");

    logger.log(LOG_INFO, "Config: Configuration loading completed.");
    return config;
}
