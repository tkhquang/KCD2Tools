/**
 * @file version.h
 * @brief Single source of truth for TPVToggle version information
 *
 * This file is the ONLY place where version numbers should be updated.
 * All other files reference this information directly or indirectly.
 */

#ifndef VERSION_H
#define VERSION_H

#include <string>

// ---------------------------------------------------------------------------
// VERSION DEFINITION - MODIFY ONLY THESE VALUES WHEN UPDATING VERSION
// ---------------------------------------------------------------------------
#define VERSION_MAJOR 0
#define VERSION_MINOR 2
#define VERSION_PATCH 1
// ---------------------------------------------------------------------------

// Derived version components - DO NOT MODIFY DIRECTLY
#define VERSION_STR_HELPER(x) #x
#define VERSION_STR(x) VERSION_STR_HELPER(x)

namespace Version
{
    // Numeric version components
    constexpr int MAJOR = VERSION_MAJOR;
    constexpr int MINOR = VERSION_MINOR;
    constexpr int PATCH = VERSION_PATCH;

    // Combined version string (e.g., "0.1.0")
    constexpr const char *VERSION_STRING = VERSION_STR(VERSION_MAJOR) "." VERSION_STR(VERSION_MINOR) "." VERSION_STR(VERSION_PATCH);

    // Use for file names and similar contexts (e.g., "v0.1.0")
    constexpr const char *VERSION_TAG = "v" VERSION_STR(VERSION_MAJOR) "." VERSION_STR(VERSION_MINOR) "." VERSION_STR(VERSION_PATCH);

    // Use for semantic versioning contexts (e.g., "0.1.0")
    constexpr const char *SEMVER = VERSION_STRING;

    // Build information (updated at compile time)
    constexpr const char *BUILD_DATE = __DATE__;
    constexpr const char *BUILD_TIME = __TIME__;

    // Project information
    constexpr const char *MOD_NAME = "KCD2_TPVToggle";
    constexpr const char *AUTHOR = "tkhquang";
    constexpr const char *REPOSITORY = "https://github.com/tkhquang/KDC2Tools";
    constexpr const char *RELEASE_URL = "https://github.com/tkhquang/KDC2Tools/releases/tag/TPVToggle-" VERSION_STR(VERSION_MAJOR) "." VERSION_STR(VERSION_MINOR) "." VERSION_STR(VERSION_PATCH);

    /**
     * @brief Get the full version string (e.g., "0.1.0")
     * @return std::string Formatted version string
     */
    inline std::string getVersionString()
    {
        return VERSION_STRING;
    }

    /**
     * @brief Get the version tag (e.g., "v0.1.0")
     * @return std::string Formatted version tag
     */
    inline std::string getVersionTag()
    {
        return VERSION_TAG;
    }

    /**
     * @brief Get the full artifact name (e.g., "KCD2_TPVToggle_v0.1.0.zip")
     * @return std::string Formatted artifact name
     */
    inline std::string getArtifactName()
    {
        return std::string(MOD_NAME) + "_" + VERSION_TAG + ".zip";
    }

    /**
     * @brief Logs version information
     */
    void logVersionInfo();
}

#endif // VERSION_H
