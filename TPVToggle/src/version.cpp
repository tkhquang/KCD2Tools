/**
 * @file version.cpp
 * @brief Implements the function to log version information.
 */
#include "version.h" // Function prototype and version constants
#include <DetourModKit.hpp>  // Required for logging
#include <string>    // For std::string operations

using DetourModKit::LogLevel;

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
        DMKLogger &logger = DMKLogger::get_instance(); // Get singleton logger instance

        // Log core mod information
        logger.log(LogLevel::Info, std::string(MOD_NAME) + " " + VERSION_TAG);
        logger.log(LogLevel::Info, "Author: " + std::string(AUTHOR));
        logger.log(LogLevel::Info, "Source: " + std::string(REPOSITORY));
        logger.log(LogLevel::Info, "Release URL: " + std::string(RELEASE_URL));

        // Log build timestamp details at DEBUG level to reduce default log noise
        logger.log(LogLevel::Debug, "Built on " + std::string(BUILD_DATE) +
                                  " at " + std::string(BUILD_TIME));
    }

} // namespace Version
