/**
 * @file constants.h
 * @brief Central definitions for constants used throughout the mod.
 *
 * Includes version info, filenames, default settings, memory offsets, AOB
 * patterns. Uses a namespace for organization.
 */
#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <string>
#include "version.h" // Mod versioning definitions

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

    /**
     * @brief Gets the expected INI config filename (e.g.,
     * "KCD2_TPVToggle.ini").
     * @return std::string The config filename.
     */
    inline std::string getConfigFilename()
    {
        return std::string(MOD_NAME) + INI_FILE_EXTENSION;
    }

    /**
     * @brief Gets the base log filename (e.g., "KCD2_TPVToggle.log").
     * @details Actual path determined by Logger using DLL location.
     * @return std::string The log filename base.
     */
    inline std::string getLogFilename()
    {
        return std::string(MOD_NAME) + LOG_FILE_EXTENSION;
    }

    // --- Default Configuration Values ---

    /** @brief Default logging level ("INFO"). Used if INI missing/invalid. */
    constexpr const char *DEFAULT_LOG_LEVEL = "INFO";

    // --- AOB (Array-of-Bytes) Patterns ---

    /**
     * @brief Default AOB pattern to find the TPV view context code.
     * @details Targets the sequence including `mov r9,[rax+38]`.
     */
    constexpr const char *DEFAULT_AOB_PATTERN =
        "48 8B 8F 58 0A 00 00 48 83 C1 10 4C 8B 48 38 "
        "4C 8B 01 41 8A 41 38 F6 D8 48 1B D2";

    /**
     * @brief AOB pattern for identifying the menu *open* / show overlay instruction.
     * @details Targets `mov byte ptr [rax+rcx+80h], 1`. Sets g_isOverlayActive=true.
     * Address: ...D5C7
     * Sequence: mov ...,1; lea rcx,[rsp+30]; call <rel>; lea r8,[rsp+40]
     */
    constexpr const char *MENU_OPEN_AOB_PATTERN = // Hook target is mov ..., 1
        "C6 84 08 80 00 00 00 01 48 8D 4C 24 30 E8 ?? ?? ?? ?? 4C 8D 44";

    /**
     * @brief AOB pattern for identifying the menu *close* / hide overlay instruction.
     * @details Targets `mov byte ptr [rax+rcx+80h], 0`. Sets g_isOverlayActive=false.
     * Address: ...C52E
     * Sequence: mov ...,0; call <rel>; test al,al; je <short>; lea rdx,...
     */
    constexpr const char *MENU_CLOSE_AOB_PATTERN = // Hook target is mov ..., 0
        "C6 84 08 80 00 00 00 00 E8 ?? ?? ?? ?? 84 C0 74 ?? 48 8D 15";

    /**
     * @brief AOB pattern for identifying code related to camera distance.
     * @details Targets `movss xmm0,[rbx+20h]`. Hooking this read operation
     *          was found to be more stable than hooking `addss` or `movss` writes.
     */
    constexpr const char *CAMERA_DISTANCE_AOB_PATTERN =
        "F3 0F 10 43 20 F3 0F 59 0D CB 07 6B 00 F3 0F 5C C1 F3 0F 11 43 20 E8 69 3C 0F FD";

    // --- Memory Offsets ---

    /**
     * @brief Offset (bytes) from TPV AOB start to `mov r9,[rax+38]` hook target.
     */
    constexpr int TPV_HOOK_OFFSET = 11;

    /**
     * @brief Offset (bytes) from captured R9 pointer to the TPV flag byte.
     * @details Flag: 0 = FPV, 1 = TPV. Relative to the captured R9 value.
     */
    constexpr int TOGGLE_FLAG_OFFSET = 0x38;

    /** @brief Offset from Menu Open AOB start to `mov ..., 1` target. */
    constexpr int MENU_OPEN_HOOK_OFFSET = 0; // Target is at start of pattern

    /** @brief Offset from Menu Close AOB start to `mov ..., 0` target. */
    constexpr int MENU_CLOSE_HOOK_OFFSET = 0; // Target is at start of pattern

    /**
     * @brief Offset (bytes) from Camera Distance AOB start to `movss` read target.
     */
    constexpr int CAMERA_HOOK_OFFSET = 0;

    /**
     * @brief Offset (bytes) from captured RBX (at distance read) to the camera
     *        distance float. Currently UNUSED by core logic.
     */
    constexpr int CAMERA_DISTANCE_OFFSET = 0x20;

    /** @brief Name of the game module to scan. */
    constexpr const char *MODULE_NAME = "WHGame.dll";

} // namespace Constants

#endif // CONSTANTS_H
