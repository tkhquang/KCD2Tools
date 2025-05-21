/**
 * @file version.cpp
 * @brief Implements the function to log version information.
 */
#include "version.h" // Function prototype and version constants
#include "logger.h"  // Required for logging
#include <string>    // For std::string operations

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
        Logger &logger = Logger::getInstance(); // Get singleton logger instance

        // Log core mod information
        logger.log(LOG_INFO, std::string(MOD_NAME) + " " + VERSION_TAG);
        logger.log(LOG_INFO, "Author: " + std::string(AUTHOR));
        logger.log(LOG_INFO, "Source: " + std::string(REPOSITORY));
        logger.log(LOG_INFO, "Release URL: " + std::string(RELEASE_URL));

        // Log build timestamp details at DEBUG level to reduce default log noise
        logger.log(LOG_DEBUG, "Built on " + std::string(BUILD_DATE) +
                                  " at " + std::string(BUILD_TIME));
    }

} // namespace Version
