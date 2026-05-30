/**
 * @file version.cpp
 * @brief Implements the function to log version information.
 */
#include "version.hpp"
#include <DetourModKit.hpp>
#include <string>

using DetourModKit::LogLevel;

namespace Version
{
    /**
     * @brief Logs detailed version and build info to the logger.
     * @details Must be called after DMKLogger::get_instance() is functional.
     *          Formats output lines clearly for better readability in logs.
     */
    void logVersionInfo()
    {
        DMKLogger &logger = DMKLogger::get_instance();

        logger.log(LogLevel::Info, std::string(MOD_NAME) + " " + VERSION_TAG);
        logger.log(LogLevel::Info, "Author: " + std::string(AUTHOR));
        logger.log(LogLevel::Info, "Source: " + std::string(REPOSITORY));
        logger.log(LogLevel::Info, "Release URL: " + std::string(RELEASE_URL));

        // Log build timestamp details at DEBUG level to reduce default log noise
        logger.log(LogLevel::Debug, "Built on " + std::string(BUILD_DATE) +
                                  " at " + std::string(BUILD_TIME));
    }

} // namespace Version
