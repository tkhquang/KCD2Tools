/**
 * @file version.h
 * @brief Single source of truth for the mod's version and metadata.
 *
 * UPDATE VERSION MACROS BELOW when creating a new version release.
 * Other constants/functions derive info from these macros. Includes build time.
 */

#ifndef VERSION_HPP
#define VERSION_HPP

#include <string>

// ========================================================================== //
//                           VERSION DEFINITION                               //
//            >>> MODIFY ONLY THESE VALUES WHEN UPDATING VERSION <<<          //
// ========================================================================== //
#define VERSION_MAJOR 0
#define VERSION_MINOR 6
#define VERSION_PATCH 1
// ========================================================================== //

// Helper macros for stringification - DO NOT MODIFY
#define VERSION_STR_HELPER(x) #x
#define VERSION_STR(x) VERSION_STR_HELPER(x)

/**
 * @namespace Version
 * @brief Contains constants and functions for version/build information.
 */
namespace Version
{
    // Numeric version components.
    constexpr int MAJOR = VERSION_MAJOR;
    constexpr int MINOR = VERSION_MINOR;
    constexpr int PATCH = VERSION_PATCH;

    // String representations.

    /** @brief Full version string (e.g., "0.2.4"). */
    constexpr const char *VERSION_STRING =
        VERSION_STR(VERSION_MAJOR) "." VERSION_STR(VERSION_MINOR) "." VERSION_STR(VERSION_PATCH);

    /** @brief Version tag for filenames (e.g., "v0.2.4"). */
    constexpr const char *VERSION_TAG =
        "v" VERSION_STR(VERSION_MAJOR) "." VERSION_STR(VERSION_MINOR) "." VERSION_STR(VERSION_PATCH);

    // Build timestamp information.
    /** @brief Date of compilation (e.g., "Apr  5 2025"). */
    constexpr const char *BUILD_DATE = __DATE__;
    /** @brief Time of compilation (e.g., "10:30:00"). */
    constexpr const char *BUILD_TIME = __TIME__;

    // Static project metadata.
    /** @brief Internal name of the mod. */
    constexpr const char *MOD_NAME = "KCD2_TPVToggle";
    /** @brief Author/Maintainer. */
    constexpr const char *AUTHOR = "tkhquang";
    /** @brief URL of the source code repository. */
    constexpr const char *REPOSITORY = "https://github.com/tkhquang/KCD2Tools";
    /**
     * @brief URL pointing to the GitHub release matching this version.
     */
    constexpr const char *RELEASE_URL =
        "https://github.com/tkhquang/KCD2Tools/releases/tag/TPVToggle-"
        "v" VERSION_STR(VERSION_MAJOR) "." VERSION_STR(VERSION_MINOR) "." VERSION_STR(VERSION_PATCH);

    /**
     * @brief Logs detailed version and build info using the global Logger.
     * @details Requires Logger to be initialized. Declared in version.h,
     *          defined in version.cpp.
     */
    void logVersionInfo();

} // namespace Version

#endif // VERSION_HPP
