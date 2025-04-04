/**
 * @file config.cpp
 * @brief Implementation of configuration loading/validation from INI file.
 *
 * Reads settings (hotkeys, log level, AOB pattern) from an INI file format.
 * Includes validation, defaults, and INI path determination relative to DLL.
 */

#include "config.h"
#include "logger.h"
#include "constants.h"
#include "utils.h" // For trim() and formatting helpers

#include <windows.h>
#include <vector>
#include <string>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <algorithm>
#include <filesystem> // Requires C++17
#include <cctype>     // For isxdigit, toupper

/**
 * @brief Validates format of an AOB pattern string from INI.
 * @details Checks tokens for '??','?' or 2-digit hex. Warns on short length.
 * @param pattern_str The AOB string from config.
 * @param logger Logger instance for reporting.
 * @return bool True if format is valid, false otherwise.
 */
bool validateAOBPattern(const std::string &pattern_str, Logger &logger)
{
    std::string trimmed_pattern = trim(pattern_str);
    if (trimmed_pattern.empty())
    {
        logger.log(LOG_ERROR, "ConfigValidate: AOB Pattern string is empty.");
        return false;
    }

    std::istringstream iss(trimmed_pattern);
    std::string token;
    int element_count = 0;
    bool is_valid = true; // Track validity throughout loop

    while (iss >> token)
    {
        element_count++;
        if (token == "??" || token == "?")
        {
            continue; // Wildcard is valid
        }
        // Check for 2 valid hex characters
        else if (token.length() == 2 &&
                 std::isxdigit(static_cast<unsigned char>(token[0])) &&
                 std::isxdigit(static_cast<unsigned char>(token[1])))
        {
            continue; // Valid hex byte format
        }
        // If neither, it's an error
        else
        {
            std::ostringstream err_msg;
            // Break up '??' sequence here as well
            err_msg << "ConfigValidate: Invalid AOB pattern element #"
                    << element_count << ": '" << token
                    << "'. Expected '?"
                       "?"
                       "', '?', or 2 hex digits.";
            logger.log(LOG_ERROR, err_msg.str());
            is_valid = false; // Mark as invalid but continue check
        }
    }

    if (!is_valid)
    {
        return false; // Return false if any element failed
    }

    // Check minimum length after ensuring format is correct
    constexpr int MIN_AOB_LEN = 8;
    if (element_count < MIN_AOB_LEN)
    {
        logger.log(LOG_WARNING, "ConfigValidate: AOB pattern has only " +
                                    std::to_string(element_count) + " elements. Ensure "
                                                                    "it is sufficiently unique.");
    }

    return element_count > 0; // Must have at least one element
}

/**
 * @brief Determines the full path for the INI config file.
 * @details Prefers same directory as the DLL. Falls back to filename only.
 * Uses C++17 filesystem.
 * @param ini_filename Base INI filename (e.g., "KCD2_TPVToggle.ini").
 * @return std::string Full path to INI file.
 */
std::string getIniFilePath(const std::string &ini_filename)
{
    Logger &logger = Logger::getInstance();
    try
    {
        char dll_path_buf[MAX_PATH] = {0};
        HMODULE h_self = NULL;

        // Get handle to this DLL
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCSTR)&getIniFilePath, &h_self))
        {
            throw std::runtime_error("GetModuleHandleExA failed: " +
                                     std::to_string(GetLastError()));
        }

        // Get full DLL path
        DWORD len = GetModuleFileNameA(h_self, dll_path_buf, MAX_PATH);
        if (len == 0 || GetLastError() == ERROR_INSUFFICIENT_BUFFER)
        {
            throw std::runtime_error("GetModuleFileNameA failed: " +
                                     std::to_string(GetLastError()));
        }

        // Construct path: <DLL_Dir>/<ini_filename>
        std::filesystem::path ini_path =
            std::filesystem::path(dll_path_buf).parent_path() / ini_filename;
        logger.log(LOG_DEBUG, "Config Path: Using INI path based on DLL: " +
                                  ini_path.string());
        return ini_path.string();
    }
    catch (const std::exception &e)
    {
        logger.log(LOG_WARNING, "Config Path: Error determining path: " +
                                    std::string(e.what()) + ". Using relative path: " +
                                    ini_filename);
    }
    catch (...)
    {
        logger.log(LOG_WARNING, "Config Path: Unknown error determining path."
                                " Using relative path: " +
                                    ini_filename);
    }

    return ini_filename; // Fallback
}

/**
 * @brief Parses comma-separated string of hex VK codes (e.g., "0x72, 4D").
 * @param value_str String value from INI.
 * @param logger Logger reference.
 * @param key_name Key name being parsed (for logs, e.g., "ToggleKey").
 * @return std::vector<int> Vector of valid integer VK codes found.
 */
std::vector<int> parseKeyList(const std::string &value_str, Logger &logger,
                              const std::string &key_name)
{
    std::vector<int> keys;
    std::string trimmed_val = trim(value_str);

    if (trimmed_val.empty())
    {
        logger.log(LOG_DEBUG, "Config ParseKeys: List '" + key_name +
                                  "' is empty.");
        return keys;
    }

    std::istringstream iss(trimmed_val);
    std::string token;
    logger.log(LOG_DEBUG, "Config ParseKeys: Parsing '" + key_name +
                              "': '" + trimmed_val + "'");
    int idx = 0;

    while (std::getline(iss, token, ','))
    {
        idx++;
        std::string trimmed_token = trim(token);
        if (trimmed_token.empty())
        {
            logger.log(LOG_WARNING, "Config ParseKeys: Empty token in '" +
                                        key_name + "' list at pos " + std::to_string(idx));
            continue;
        }

        // Handle optional "0x" or "0X" prefix
        bool has_prefix = (trimmed_token.rfind("0x", 0) == 0 ||
                           trimmed_token.rfind("0X", 0) == 0);
        if (has_prefix)
        {
            if (trimmed_token.length() > 2)
            {
                trimmed_token = trimmed_token.substr(2); // Strip prefix
            }
            else
            { // Only prefix found ("0x")
                logger.log(LOG_WARNING, "Config ParseKeys: Invalid token "
                                        "(only prefix) in '" +
                                            key_name + "': '" + token + "'.");
                continue;
            }
        }
        if (trimmed_token.empty())
        { // Empty after stripping?
            logger.log(LOG_WARNING, "Config ParseKeys: Empty token after "
                                    "processing in '" +
                                        key_name + "': '" + token + "'.");
            continue;
        }

        // Validate remaining: Must be only hex digits
        if (trimmed_token.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
        {
            logger.log(LOG_WARNING, "Config ParseKeys: Invalid char in key "
                                    "token for '" +
                                        key_name + "': '" + token + "'.");
            continue;
        }

        // Convert validated hex string to integer key code
        try
        {
            unsigned long code_ul = std::stoul(trimmed_token, nullptr, 16);
            // Optional check for typical VK range
            if (code_ul == 0 || code_ul > 0xFF)
            {
                logger.log(LOG_WARNING, "Config ParseKeys: Key code " +
                                            format_hex(static_cast<int>(code_ul)) + " for '" +
                                            key_name + "' outside VK range (0x01-0xFF). Using anyway.");
            }
            keys.push_back(static_cast<int>(code_ul));
            logger.log(LOG_DEBUG, "Config ParseKeys: Added key for '" +
                                      key_name + "': " + format_vkcode(keys.back()));
        }
        catch (const std::exception &e)
        { // Catch stoul errors
            logger.log(LOG_WARNING, "Config ParseKeys: Error converting hex "
                                    "token '" +
                                        token + "' for '" + key_name + "': " + e.what());
        }
        catch (...)
        {
            logger.log(LOG_WARNING, "Config ParseKeys: Unknown error parsing "
                                    "hex token '" +
                                        token + "' for '" + key_name + "'.");
        }
    } // End token loop

    if (keys.empty() && !trimmed_val.empty())
    {
        logger.log(LOG_WARNING, "Config ParseKeys: Parsed '" + key_name +
                                    "' value '" + trimmed_val + "' but found 0 valid keys.");
    }

    return keys;
}

/**
 * @brief Loads config settings from INI file specified by filename.
 * @details Parses [Settings] section. Applies defaults and validates values.
 * @param ini_filename Base name of the INI file.
 * @return Config Structure containing loaded or default settings.
 */
Config loadConfig(const std::string &ini_filename)
{
    Config config; // Initialize default struct
    Logger &logger = Logger::getInstance();

    std::string ini_path = getIniFilePath(ini_filename);
    logger.log(LOG_INFO, "Config Load: Attempting load from: " + ini_path);

    // --- Set Defaults ---
    config.log_level = Constants::DEFAULT_LOG_LEVEL;
    config.aob_pattern = Constants::DEFAULT_AOB_PATTERN;

    // --- Open & Parse ---
    std::ifstream file(ini_path);
    if (!file.is_open())
    {
        logger.log(LOG_ERROR, "Config Load: Failed open INI: " + ini_path +
                                  ". Using default settings.");
        return config; // Return struct with defaults already set
    }
    logger.log(LOG_INFO, "Config Load: Successfully opened INI file.");

    std::string line;
    std::string section;
    int line_num = 0;

    while (std::getline(file, line))
    {
        line_num++;
        std::string trimmed_line = trim(line);

        if (trimmed_line.empty() || trimmed_line[0] == ';' || trimmed_line[0] == '#')
        {
            continue; // Skip blank lines/comments
        }

        // Check for section header: [SectionName]
        if (trimmed_line.length() >= 2 && trimmed_line.front() == '[' &&
            trimmed_line.back() == ']')
        {
            section = trim(trimmed_line.substr(1, trimmed_line.size() - 2));
            logger.log(LOG_DEBUG, "Config Load: Entering section [" + section + "]");
            continue;
        }

        // Look for key=value pair within a section
        size_t eq_pos = trimmed_line.find('=');
        if (eq_pos != std::string::npos)
        {
            std::string key = trim(trimmed_line.substr(0, eq_pos));
            std::string value = trim(trimmed_line.substr(eq_pos + 1));

            // Process only keys within the [Settings] section
            if (section == "Settings")
            {
                logger.log(LOG_DEBUG, "Config Load: [Settings] -> '" + key +
                                          "' = '" + value + "'");
                if (key == "ToggleKey")
                {
                    config.toggle_keys = parseKeyList(value, logger, key);
                }
                else if (key == "FPVKey")
                {
                    config.fpv_keys = parseKeyList(value, logger, key);
                }
                else if (key == "TPVKey")
                {
                    config.tpv_keys = parseKeyList(value, logger, key);
                }
                else if (key == "LogLevel")
                {
                    config.log_level = trim(value); // Validate later
                }
                else if (key == "AOBPattern")
                {
                    config.aob_pattern = value; // Validate later
                }
                else
                {
                    logger.log(LOG_WARNING, "Config Load: Unknown key '" + key +
                                                "' in [Settings] at line " +
                                                std::to_string(line_num));
                }
            }
            else if (!section.empty())
            {
                logger.log(LOG_DEBUG, "Config Load: Skipping key '" + key +
                                          "' in other section [" + section + "]");
            }
            else
            { // Key outside any section
                logger.log(LOG_WARNING, "Config Load: Key '" + key +
                                            "' outside section at line " +
                                            std::to_string(line_num));
            }
        }
        else
        { // Line is not empty/comment/section/key=value
            logger.log(LOG_WARNING, "Config Load: Malformed line " +
                                        std::to_string(line_num) + ": '" + trimmed_line + "'");
        }
    }
    file.close();

    // --- Final Validation ---
    // Validate LogLevel
    std::string upper_level = config.log_level;
    std::transform(upper_level.begin(), upper_level.end(), upper_level.begin(),
                   [](unsigned char c)
                   { return std::toupper(c); });
    if (upper_level != "DEBUG" && upper_level != "INFO" &&
        upper_level != "WARNING" && upper_level != "ERROR")
    {
        logger.log(LOG_WARNING, "Config Load: Invalid LogLevel '" +
                                    config.log_level + "' in INI. Using default '" +
                                    Constants::DEFAULT_LOG_LEVEL + "'.");
        config.log_level = Constants::DEFAULT_LOG_LEVEL;
    }
    else
    {
        config.log_level = upper_level; // Use validated uppercase version
        logger.log(LOG_DEBUG, "Config Load: Effective LogLevel: " +
                                  config.log_level);
    }

    // Validate AOB Pattern format
    if (!validateAOBPattern(config.aob_pattern, logger))
    {
        logger.log(LOG_ERROR, "Config Load: AOBPattern in INI is invalid.");
        logger.log(LOG_WARNING, "Config Load: Using default AOB pattern.");
        config.aob_pattern = Constants::DEFAULT_AOB_PATTERN;
    }
    else
    {
        // Clean extra whitespace from validated pattern for consistency
        std::string cleaned_pattern;
        std::istringstream iss_p(config.aob_pattern);
        std::string part;
        bool first = true;
        while (iss_p >> part)
        {
            if (!first)
                cleaned_pattern += " ";
            cleaned_pattern += part;
            first = false;
        }
        config.aob_pattern = cleaned_pattern;
        logger.log(LOG_DEBUG, "Config Load: Using AOB Pattern: " +
                                  config.aob_pattern);
    }

    // Log key loading summary
    bool no_keys = config.toggle_keys.empty() && config.fpv_keys.empty() &&
                   config.tpv_keys.empty();
    if (no_keys)
    {
        logger.log(LOG_WARNING, "Config Load: No valid keys defined. "
                                "Hotkeys will be inactive.");
    }
    else
    {
        logger.log(LOG_INFO, "Config Load: Loaded Hotkeys (Toggle:" +
                                 std::to_string(config.toggle_keys.size()) + " FPV:" +
                                 std::to_string(config.fpv_keys.size()) + " TPV:" +
                                 std::to_string(config.tpv_keys.size()) + ")");
    }

    logger.log(LOG_INFO, "Config Load: Finished processing configuration.");
    return config;
}
