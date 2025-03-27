/**
 * @file config.cpp
 * @brief Implementation of configuration loading functionality
 *
 * This file implements functions for loading and validating configuration
 * from an INI file, handling string conversions, and providing default values.
 */

#include "config.h"
#include "logger.h"
#include "constants.h"
#include <windows.h>
#include <vector>
#include <string>
#include <sstream>
#include <fstream>
#include <iomanip>

// Helper: Convert std::wstring to std::string (ACP encoding)
std::string WideToNarrow_std(const std::wstring &wstr)
{
    if (wstr.empty())
        return "";
    int size_needed = WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string result(size_needed, 0);
    WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), (int)wstr.size(), &result[0], size_needed, NULL, NULL);
    return result;
}

// Helper: Convert std::string to std::wstring (ACP encoding)
std::wstring NarrowToWide_std(const std::string &str)
{
    if (str.empty())
        return L"";
    int size_needed = MultiByteToWideChar(CP_ACP, 0, str.c_str(), (int)str.size(), NULL, 0);
    std::wstring result(size_needed, 0);
    MultiByteToWideChar(CP_ACP, 0, str.c_str(), (int)str.size(), &result[0], size_needed);
    return result;
}

// Helper: Trim leading/trailing whitespace from string
std::string trim(const std::string &s)
{
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end = s.find_last_not_of(" \t\r\n");
    return (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
}

/**
 * Validates if an AOB pattern is correctly formatted.
 * Checks that each byte is a valid hexadecimal value and
 * the pattern is long enough to be useful.
 */
bool validateAOBPattern(const std::string &pattern, Logger &logger)
{
    std::istringstream iss(pattern);
    std::string byte_str;
    int count = 0;

    while (iss >> byte_str)
    {
        count++;
        // Check that each byte is a valid hex value
        if (byte_str.length() != 2 ||
            !std::isxdigit(byte_str[0]) ||
            !std::isxdigit(byte_str[1]))
        {
            logger.log(LOG_ERROR, "Config: Invalid AOB pattern format at position " + std::to_string(count));
            return false;
        }
    }

    if (count < 10)
    { // Arbitrary minimum length for a useful pattern
        logger.log(LOG_WARNING, "Config: AOB pattern is suspiciously short (" + std::to_string(count) + " bytes)");
    }

    return count > 0;
}

// Manual INI parser (no WinAPI calls)
Config loadConfig(const std::string &ini_path_narrow)
{
    Config config;
    Logger &logger = Logger::getInstance();

    // Try to resolve and log full path
    std::wstring ini_path_wide = NarrowToWide_std(ini_path_narrow);
    wchar_t full_path[MAX_PATH];
    DWORD result = GetFullPathNameW(ini_path_wide.c_str(), MAX_PATH, full_path, nullptr);
    if (result > 0 && result < MAX_PATH)
        logger.log(LOG_DEBUG, "Config: Loading from " + WideToNarrow_std(full_path));

    // Open INI file
    std::ifstream file(ini_path_narrow);
    if (!file.is_open())
    {
        logger.log(LOG_ERROR, "Config: Could not open INI file at " + ini_path_narrow);
        return config;
    }

    std::string line;
    std::string currentSection;
    while (std::getline(file, line))
    {
        logger.log(LOG_DEBUG, "INI Line: " + line);
        std::string trimmed = trim(line);

        if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#')
            continue;

        // Section header
        if (trimmed.front() == '[' && trimmed.back() == ']')
        {
            currentSection = trimmed.substr(1, trimmed.size() - 2);
            continue;
        }

        size_t eqPos = trimmed.find('=');
        if (eqPos == std::string::npos)
            continue;

        std::string key = trim(trimmed.substr(0, eqPos));
        std::string value = trim(trimmed.substr(eqPos + 1));

        if (currentSection == "Settings")
        {
            if (key == "ToggleKey")
            {
                logger.log(LOG_DEBUG, "Config: Raw ToggleKey = '" + value + "'");
                std::istringstream iss(value);
                std::string token;
                while (std::getline(iss, token, ','))
                {
                    token = trim(token);
                    if (token.rfind("0x", 0) == 0 || token.rfind("0X", 0) == 0)
                        token = token.substr(2);
                    try
                    {
                        int keycode = std::stoi(token, nullptr, 16);
                        config.toggle_keys.push_back(keycode);
                    }
                    catch (...)
                    {
                    }
                }
            }
            else if (key == "LogLevel")
            {
                config.log_level = value;
                logger.log(LOG_DEBUG, "Config: Raw LogLevel = '" + value + "'");
            }
            else if (key == "AOBPattern")
            {
                config.aob_pattern = value;
                logger.log(LOG_DEBUG, "Config: Raw AOBPattern = '" + value + "'");
            }
        }
    }

    // Default fallback log level if empty
    if (config.log_level.empty())
    {
        config.log_level = Constants::DEFAULT_LOG_LEVEL;
        logger.log(LOG_WARNING, "Config: LogLevel missing, defaulting to " + std::string(Constants::DEFAULT_LOG_LEVEL));
    }

    // Final validation
    if (config.toggle_keys.empty())
    {
        logger.log(LOG_WARNING, "Config: No valid ToggleKey found, using default 0x72");
        config.toggle_keys.push_back(Constants::DEFAULT_TOGGLE_KEY);
    }

    if (config.aob_pattern.empty())
    {
        logger.log(LOG_ERROR, "Config: AOBPattern is empty or missing");
    }
    else
    {
        if (!validateAOBPattern(config.aob_pattern, logger))
        {
            logger.log(LOG_ERROR, "Config: Using default AOB pattern as fallback");
            config.aob_pattern = Constants::DEFAULT_AOB_PATTERN;
        }
    }

    return config;
}
