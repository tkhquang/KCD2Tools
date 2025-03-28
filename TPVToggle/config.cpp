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
#include <algorithm>

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

/**
 * Gets the possible paths where the INI file might be located
 * Checks multiple locations to be more flexible
 */
std::vector<std::string> getIniFilePaths(const std::string &ini_filename)
{
    std::vector<std::string> paths;
    char buffer[MAX_PATH];

    // 1. First try the same directory as the DLL
    HMODULE hModule = NULL;
    GetModuleHandleEx(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)&loadConfig,
        &hModule);

    if (hModule != NULL)
    {
        DWORD length = GetModuleFileNameA(hModule, buffer, MAX_PATH);
        if (length > 0 && length < MAX_PATH)
        {
            std::string dllPath(buffer);
            size_t lastSlash = dllPath.find_last_of("\\/");
            if (lastSlash != std::string::npos)
            {
                std::string dllDir = dllPath.substr(0, lastSlash + 1);
                paths.push_back(dllDir + ini_filename);
            }
        }
    }

    // 2. Try the current working directory
    if (GetCurrentDirectoryA(MAX_PATH, buffer) > 0)
    {
        std::string currentDir(buffer);
        if (!currentDir.empty() && currentDir.back() != '\\' && currentDir.back() != '/')
            currentDir += '\\';
        paths.push_back(currentDir + ini_filename);
    }

    // 3. Try game base directory (parent of the executable directory)
    DWORD exeLength = GetModuleFileNameA(NULL, buffer, MAX_PATH);
    if (exeLength > 0 && exeLength < MAX_PATH)
    {
        std::string exePath(buffer);
        size_t lastSlash = exePath.find_last_of("\\/");
        if (lastSlash != std::string::npos)
        {
            std::string exeDir = exePath.substr(0, lastSlash + 1);
            size_t parentSlash = exeDir.find_last_of("\\/", exeDir.length() - 2);
            if (parentSlash != std::string::npos)
            {
                std::string parentDir = exeDir.substr(0, parentSlash + 1);
                paths.push_back(parentDir + ini_filename);
            }
        }
    }

    // 4. Also add the original path that was passed in
    if (std::find(paths.begin(), paths.end(), ini_filename) == paths.end())
    {
        paths.push_back(ini_filename);
    }

    return paths;
}

/**
 * Parses a comma-separated list of hexadecimal key codes
 * Each key can be prefixed with "0x" or not
 */
std::vector<int> parseKeyList(const std::string &value, Logger &logger, const std::string &keyName)
{
    std::vector<int> keys;
    std::istringstream iss(value);
    std::string token;
    logger.log(LOG_DEBUG, "Config: Parsing " + keyName + " = '" + value + "'");

    while (std::getline(iss, token, ','))
    {
        token = trim(token);
        if (token.empty())
            continue;

        // Remove "0x" prefix if present
        if (token.rfind("0x", 0) == 0 || token.rfind("0X", 0) == 0)
            token = token.substr(2);

        try
        {
            int keycode = std::stoi(token, nullptr, 16);
            keys.push_back(keycode);
            logger.log(LOG_DEBUG, "Config: Added " + keyName + " code: 0x" + token);
        }
        catch (...)
        {
            logger.log(LOG_WARNING, "Config: Invalid key code in " + keyName + ": '" + token + "'");
        }
    }

    return keys;
}

// Manual INI parser (no WinAPI calls)
Config loadConfig(const std::string &ini_path_narrow)
{
    Config config;
    Logger &logger = Logger::getInstance();

    // Get possible INI file locations
    std::vector<std::string> possiblePaths = getIniFilePaths(ini_path_narrow);

    // Try each possible path until we find a file that exists
    std::ifstream file;
    std::string usedPath;

    for (const auto &path : possiblePaths)
    {
        file.open(path);
        if (file.is_open())
        {
            usedPath = path;
            logger.log(LOG_INFO, "Config: Successfully loaded INI from " + usedPath);
            break;
        }
    }

    if (!file.is_open())
    {
        // Log all paths we tried but failed to open
        std::string allPaths;
        for (const auto &path : possiblePaths)
        {
            if (!allPaths.empty())
                allPaths += ", ";
            allPaths += path;
        }

        logger.log(LOG_ERROR, "Config: Could not open INI file. Tried paths: " + allPaths);
        logger.log(LOG_WARNING, "Config: Using default configuration values");
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
                config.toggle_keys = parseKeyList(value, logger, "ToggleKey");
            }
            else if (key == "FPVKey")
            {
                config.fpv_keys = parseKeyList(value, logger, "FPVKey");
            }
            else if (key == "TPVKey")
            {
                config.tpv_keys = parseKeyList(value, logger, "TPVKey");
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

    // Set default toggle keys if none were provided
    if (config.toggle_keys.empty())
    {
        logger.log(LOG_WARNING, "Config: No valid ToggleKey found, using default setting)");
        config.toggle_keys.push_back(Constants::DEFAULT_TOGGLE_KEY);
    }

    // Set default FPV keys if none were provided
    if (config.fpv_keys.empty())
    {
        logger.log(LOG_INFO, "Config: No FPVKey values found, using defaults setting");
        for (int i = 0; i < Constants::DEFAULT_FPV_KEYS_COUNT; i++)
        {
            config.fpv_keys.push_back(Constants::DEFAULT_FPV_KEYS[i]);
        }
    }

    return config;
}
