/**
 * @file version.cpp
 * @brief Implements the function to log version information.
 */
#include "version.h"
#include <string>

#include <DetourModKit.hpp>

// Define the function implementation within the Version namespace.
namespace Version
{
    /**
     * @brief Logs detailed version and build info to the logger.
     * @details Must be called after Logger::getInstance() is functional.
     *          Formats output lines clearly for better readability in logs.
     */
    void logVersionInfo()
    {
        DMK::Logger &logger = DMK::Logger::getInstance();

        // Log core mod information
        logger.log(DMKLogLevel::LOG_INFO, std::string(MOD_NAME) + " " + VERSION_TAG);
        logger.log(DMKLogLevel::LOG_INFO, "Author: " + std::string(AUTHOR));
        logger.log(DMKLogLevel::LOG_INFO, "Source: " + std::string(REPOSITORY));
        logger.log(DMKLogLevel::LOG_INFO, "Release URL: " + std::string(RELEASE_URL));

        // Log build timestamp details at DEBUG level to reduce default log noise
        logger.log(DMKLogLevel::LOG_DEBUG, "Built on " + std::string(BUILD_DATE) +
                                               " at " + std::string(BUILD_TIME));
    }

} // namespace Version
