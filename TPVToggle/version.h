/**
 * @file version.h
 * @brief Version information for the TPVToggle mod
 *
 * This file defines version information constants and utility functions
 * for displaying version details in logs and to users.
 */

#ifndef VERSION_H
#define VERSION_H

#include <string>
#include "logger.h" // Include the Logger header

namespace Version
{
    // Version numbers - update these for new releases
    constexpr int MAJOR = 0;
    constexpr int MINOR = 2;
    constexpr int PATCH = 0;

    // Build information
    constexpr const char *BUILD_DATE = __DATE__;
    constexpr const char *BUILD_TIME = __TIME__;

    // Project information
    constexpr const char *MOD_NAME = "KCD2_TPVToggle";
    constexpr const char *AUTHOR = "tkhquang";
    constexpr const char *REPOSITORY = "https://github.com/tkhquang/KDC2Tools";

    /**
     * @brief Get the full version string (e.g., "0.1.0")
     * @return std::string Formatted version string
     */
    inline std::string getVersionString()
    {
        return std::to_string(MAJOR) + "." +
               std::to_string(MINOR) + "." +
               std::to_string(PATCH);
    }

    /**
     * @brief Logs version information to the logger
     * @param logger Reference to a Logger instance
     */
    inline void logVersionInfo()
    {
        Logger &logger = Logger::getInstance();
        logger.log(LOG_INFO, std::string(MOD_NAME) + " v" + getVersionString());
        logger.log(LOG_INFO, "By " + std::string(AUTHOR));
        logger.log(LOG_INFO, "Source: " + std::string(REPOSITORY));
        logger.log(LOG_DEBUG, "Built on " + std::string(BUILD_DATE) + " at " + std::string(BUILD_TIME));
    }
}

#endif // VERSION_H
