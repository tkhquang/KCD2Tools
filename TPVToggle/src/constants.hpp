/**
 * @file constants.h
 * @brief Central definitions for constants used throughout the mod.
 *
 * Includes version info, filenames, default settings, AOB patterns, and memory offsets.
 * All hardcoded memory addresses have been replaced with AOB patterns for robustness.
 */
#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <string>
#include <math.h>

#include "version.h"

/**
 * @namespace Constants
 * @brief Encapsulates global constants and config defaults.
 */
namespace Constants
{
    // Version information derived from version.h
    constexpr const char *MOD_VERSION = Version::VERSION_STRING;
    constexpr const char *MOD_NAME = Version::MOD_NAME;
    constexpr const char *MOD_WEBSITE = Version::REPOSITORY;

    // File extensions
    constexpr const char *INI_FILE_EXTENSION = ".ini";
    constexpr const char *LOG_FILE_EXTENSION = ".log";

    /** @brief Gets the INI config filename (e.g., "KCD2_TPVToggle.ini"). */
    inline std::string getConfigFilename()
    {
        return std::string(MOD_NAME) + INI_FILE_EXTENSION;
    }
    /** @brief Gets the base log filename (e.g., "KCD2_TPVToggle.log"). */
    inline std::string getLogFilename()
    {
        return std::string(MOD_NAME) + LOG_FILE_EXTENSION;
    }

    constexpr const char *FPV_CAMERA_UPDATE_AOB_PATTERN = "48 8b c4 48 89 58 08 55 56 57 41 56 41 57 48 8d 68 98 48 81 ec 40 01 00 00 0f 29 70 c8 4c 8b f9 48 8b 0d ?? ?? ?? 04 4c 8b f2 0f 29 78 b8 44 0f 29 40 a8 44 0f 29 48 98";

    constexpr const char *COMBAT_FPV_CAMERA_UPDATE_AOB_PATTERN = "48 89 5c 24 08 57 48 83 ec 20 48 8b d9 48 8b fa 48 83 c1 e8 e8 ?? ?? ?? ?? 0f 10 43 48 f3 0f 7f 47 0c f2 0f 10 43 58 f2 0f 11 07";

    // TPV and FPV apparently use the same offsets
    constexpr ptrdiff_t CAMERA_QUATERNION_OFFSET = 0x10; // Confirmed XYZW quaternion start in C_CameraThirdPerson object
    // Offsets relative to the outputPosePtr (RDX) in FUN_18392509c
    // Standard Pos(XYZ) followed by Quat(XYZW).
    constexpr ptrdiff_t OUTPUT_POSE_POSITION_OFFSET = 0x0;  // Base Offset (X, Y, Z = 12 bytes)
    constexpr ptrdiff_t OUTPUT_POSE_ROTATION_OFFSET = 0x0C; // Base Offset (Assuming starts right after Pos. Z = 0x8+0x4) (X, Y, Z, W = 16 bytes)
    constexpr size_t OUTPUT_POSE_REQUIRED_SIZE = 0x1C;      // Minimum size needed: Pos(12) + Quat(16) = 28 bytes (0x1C). Let's use 0x20 for alignment.
                                                            // constexpr size_t OUTPUT_POSE_REQUIRED_SIZE = sizeof(Vector3) + sizeof(Quaternion); // Minimum expected size

    constexpr const char *SET_HEAD_VISIBILITY_AOB_PATTERN = "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC ?? 41 8A F0";

    constexpr const char *DEFAULT_LOG_LEVEL = "DEBUG";

    constexpr unsigned long MAIN_MONITOR_SLEEP_MS = 33;

    constexpr const char *MODULE_NAME = "WHGame.dll";
} // namespace Constants
#endif // CONSTANTS_H
