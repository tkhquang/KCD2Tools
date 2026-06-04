/**
 * @file version.hpp
 * @brief Single source of truth for the mod's version and metadata.
 *
 * UPDATE VERSION MACROS BELOW when creating a new version release.
 * Other constants/functions derive info from these macros. Includes build time.
 */

#ifndef TPVCAMERA_VERSION_HPP
#define TPVCAMERA_VERSION_HPP

// ========================================================================== //
//                           VERSION DEFINITION                               //
//            >>> MODIFY ONLY THESE VALUES WHEN UPDATING VERSION <<<          //
// ========================================================================== //
#define VERSION_MAJOR 1
#define VERSION_MINOR 0
#define VERSION_PATCH 0
// ========================================================================== //

// Helper macros for stringification - DO NOT MODIFY
#define VERSION_STR_HELPER(x) #x
#define VERSION_STR(x) VERSION_STR_HELPER(x)

/**
 * @namespace Version
 * @brief Contains constants and functions for version/build information.
 */
namespace TPVCamera
{
    namespace Version
    {
        // String representations.

        /** @brief Version tag for filenames (e.g., "v1.0.0"). */
        constexpr const char *VERSION_TAG =
            "v" VERSION_STR(VERSION_MAJOR) "." VERSION_STR(VERSION_MINOR) "." VERSION_STR(VERSION_PATCH);

        // Build timestamp information.
        /** @brief Date of compilation (e.g., "Apr  5 2025"). */
        constexpr const char *BUILD_DATE = __DATE__;
        /** @brief Time of compilation (e.g., "10:30:00"). */
        constexpr const char *BUILD_TIME = __TIME__;

        // Static project metadata.
        /** @brief Internal name of the mod. */
        constexpr const char *MOD_NAME = "KCD2_TPVCamera";
        /** @brief Author/Maintainer. */
        constexpr const char *AUTHOR = "tkhquang";
        /** @brief URL of the source code repository. */
        constexpr const char *REPOSITORY = "https://github.com/tkhquang/KCD2Tools";
        /**
         * @brief URL pointing to the GitHub release matching this version.
         */
        constexpr const char *RELEASE_URL =
            "https://github.com/tkhquang/KCD2Tools/releases/tag/TPVCamera-"
            "v" VERSION_STR(VERSION_MAJOR) "." VERSION_STR(VERSION_MINOR) "." VERSION_STR(VERSION_PATCH);

        /**
         * @brief Logs detailed version and build info using the global Logger.
         * @details Requires the Logger to be initialized before it is called.
         */
        void log_version_info();

    } // namespace Version
} // namespace TPVCamera

#endif // TPVCAMERA_VERSION_HPP
