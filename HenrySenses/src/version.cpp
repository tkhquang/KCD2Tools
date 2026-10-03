/**
 * @file version.cpp
 * @brief Logs the version and build information.
 */

#include "version.hpp"

#include <DetourModKit.hpp>

namespace HenrySenses
{
    namespace version
    {
        void log_version_info()
        {
            DMK::Logger &logger = DMK::log();

            logger.info("{} {}", MOD_NAME, VERSION_TAG);
            logger.info("Author: {}", AUTHOR);
            logger.info("Source: {}", REPOSITORY);
            logger.info("Release URL: {}", RELEASE_URL);

            // The build stamp is for matching a log to a binary, not routine status.
            logger.debug("Built on {} at {}", BUILD_DATE, BUILD_TIME);
        }

    } // namespace version
} // namespace HenrySenses
