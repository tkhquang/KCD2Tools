/**
 * @file version.hpp
 * @brief Single source of truth for the mod's version and metadata.
 *
 * Update the VERSION_* macros below when cutting a release. CMakeLists.txt parses them for PROJECT_VERSION,
 * and every other version string derives from them.
 */

#ifndef SNDFW_VERSION_HPP
#define SNDFW_VERSION_HPP

#define VERSION_MAJOR 1
#define VERSION_MINOR 1
#define VERSION_PATCH 0

/// Stringification helpers for the version macros.
#define VERSION_STR_HELPER(x) #x
#define VERSION_STR(x) VERSION_STR_HELPER(x)

namespace SoundFramework
{
    /**
     * @namespace SoundFramework::version
     * @brief Constants and functions for version and build information.
     */
    namespace version
    {
        /** @brief Version tag for file names (e.g. "v0.1.0"). */
        inline constexpr const char *VERSION_TAG =
            "v" VERSION_STR(VERSION_MAJOR) "." VERSION_STR(VERSION_MINOR) "." VERSION_STR(VERSION_PATCH);

        /** @brief Date of compilation. */
        inline constexpr const char *BUILD_DATE = __DATE__;
        /** @brief Time of compilation (e.g. "10:30:00"). */
        inline constexpr const char *BUILD_TIME = __TIME__;

        /** @brief Internal name of the mod; also the stem of the INI, log and ASI file names. */
        inline constexpr const char *MOD_NAME = "KCD2_SoundFramework";
        /** @brief Author and maintainer. */
        inline constexpr const char *AUTHOR = "tkhquang";
        /** @brief URL of the source code repository. */
        inline constexpr const char *REPOSITORY = "https://github.com/tkhquang/KCD2Tools";

        /**
         * @brief Logs the version and build information through the session logger.
         * @note Requires the DMK logger to be live.
         */
        void log_version_info();

    } // namespace version
} // namespace SoundFramework

#endif // SNDFW_VERSION_HPP
