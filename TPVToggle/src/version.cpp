#include "version.h"
#include "logger.h"

namespace Version
{
    void logVersionInfo()
    {
        Logger &logger = Logger::getInstance();
        logger.log(LOG_INFO, std::string(MOD_NAME) + " " + VERSION_TAG);
        logger.log(LOG_INFO, "By " + std::string(AUTHOR));
        logger.log(LOG_INFO, "Source: " + std::string(REPOSITORY));
        logger.log(LOG_DEBUG, "Built on " + std::string(BUILD_DATE) + " at " + std::string(BUILD_TIME));
    }
}
