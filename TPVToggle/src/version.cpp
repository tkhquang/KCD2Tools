/**
 * @file version.cpp
 * @brief Implements the function to log version information.
 */
#include "version.hpp"
#include <DetourModKit.hpp>
#include <string>


namespace TPVToggle
{
namespace Version
{
    /**
     * @brief Logs detailed version and build info to the logger.
     * @details Must be called after DMK::Logger::get_instance() is functional.
     *          Formats output lines clearly for better readability in logs.
     */
    void logVersionInfo()
    {
        DMK::Logger &logger = DMK::Logger::get_instance();

        logger.info("{} {}", MOD_NAME, VERSION_TAG);
        logger.info("Author: {}", AUTHOR);
        logger.info("Source: {}", REPOSITORY);
        logger.info("Release URL: {}", RELEASE_URL);

        // Log build timestamp details at DEBUG level to reduce default log noise
        logger.debug("Built on {} at {}", BUILD_DATE, BUILD_TIME);
    }

} // namespace Version
} // namespace TPVToggle
