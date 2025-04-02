/**
 * @file constants.h
 * @brief Constants used throughout the TPVToggle mod
 *
 * This file contains constants and default values used by the TPVToggle mod,
 * including default configuration values, patterns, and file paths.
 */
#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <string>
#include "version.h" // Include version header

namespace Constants
{
    // Version information now pulled from version.h
    constexpr const char *MOD_VERSION = Version::VERSION_STRING;
    constexpr const char *MOD_NAME = Version::MOD_NAME;
    constexpr const char *MOD_WEBSITE = Version::REPOSITORY;

    // File extensions
    constexpr const char *INI_FILE_EXTENSION = ".ini";
    constexpr const char *LOG_FILE_EXTENSION = ".log";

    // Get full configuration filename (MOD_NAME + .extension)
    inline std::string getConfigFilename()
    {
        return std::string(MOD_NAME) + INI_FILE_EXTENSION;
    }

    // Get full configuration filename (MOD_NAME + .extension)
    inline std::string getLogFilename()
    {
        return std::string(MOD_NAME) + LOG_FILE_EXTENSION;
    }

    // Default configuration values
    constexpr const char *DEFAULT_LOG_LEVEL = "DEBUG";

    // AOB Patterns
    constexpr const char *DEFAULT_AOB_PATTERN =
        "48 8B 8F 58 0A 00 00 48 83 C1 10 4C 8B 48 38 4C 8B 01 41 8A 41 38 F6 D8 48 1B D2";
    constexpr const char *OVERLAY_AOB_PATTERN =
        "48 83 BB D8 00 00 00 00 77 27 48 8B CB E8 FC 00 00 00 48 8B CB E8 24 00 00 00 48";
    constexpr const char *CAMERA_DISTANCE_AOB_PATTERN =
        "F3 0F 58 43 20 0F 2F F0 76 16 E8 46 3C 0F FD 48 8B 48 20 F3 0F 5C B1 10 01 00 00";

    // Memory offsets
    constexpr int TOGGLE_FLAG_OFFSET = 0x38;
    constexpr int OVERLAY_FLAG_OFFSET = 0xD8;
    constexpr int CAMERA_DISTANCE_OFFSET = 0x20;

    constexpr const char *MODULE_NAME = "WHGame.dll";
}

#endif // CONSTANTS_H
