/**
 * @file constants.h
 * @brief Central definitions for constants used throughout the mod.
 *
 * Includes version info, filenames, default settings, AOB patterns, and memory offsets.
 * All hardcoded memory addresses have been replaced with AOB patterns for robustness.
 */
#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <string>
#include <math.h>

#include "version.h"

/**
 * @namespace Constants
 * @brief Encapsulates global constants and config defaults.
 */
namespace Constants
{
    // Version information derived from version.h
    constexpr const char *MOD_VERSION = Version::VERSION_STRING;
    constexpr const char *MOD_NAME = Version::MOD_NAME;
    constexpr const char *MOD_WEBSITE = Version::REPOSITORY;

    // File extensions
    constexpr const char *INI_FILE_EXTENSION = ".ini";
    constexpr const char *LOG_FILE_EXTENSION = ".log";

    /** @brief Gets the INI config filename (e.g., "KCD2_TPVToggle.ini"). */
    inline std::string getConfigFilename()
    {
        return std::string(MOD_NAME) + INI_FILE_EXTENSION;
    }
    /** @brief Gets the base log filename (e.g., "KCD2_TPVToggle.log"). */
    inline std::string getLogFilename()
    {
        return std::string(MOD_NAME) + LOG_FILE_EXTENSION;
    }

    constexpr const char *DEFAULT_LOG_LEVEL = "INFO";

    constexpr unsigned long MAIN_MONITOR_SLEEP_MS = 33;

    constexpr const char *MODULE_NAME = "WHGame.dll";
} // namespace Constants
#endif // CONSTANTS_H
