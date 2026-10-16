/**
 * @file constants.hpp
 * @brief Central definitions for constants used throughout the mod: its name, its files and its config defaults here,
 *        the game's memory layout in layout_offsets.hpp and the mod's own limits and timings in tuning.hpp.
 *
 * @details Code and data locations are resolved by the multi-candidate AOB cascades in aob_resolver.hpp so they
 *          survive game updates; no fixed code or data address is kept in these headers.
 */
#ifndef TPVCAMERA_CONSTANTS_HPP
#define TPVCAMERA_CONSTANTS_HPP

#include "layout_offsets.hpp"
#include "tuning.hpp"
#include "version.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

/**
 * @namespace Constants
 * @brief Encapsulates global constants and config defaults.
 */
namespace Constants
{
    // Mod name derived from version.hpp; used for the INI/log file names and the
    // per-process instance mutex. The version string and repository URL are read
    // directly from TPVCamera::Version where they are needed.
    constexpr const char *MOD_NAME = TPVCamera::Version::MOD_NAME;

    // File extensions
    constexpr const char *INI_FILE_EXTENSION = ".ini";
    constexpr const char *PRESETS_FILE_SUFFIX = "_presets.json";

    /** @brief Gets the INI config filename (e.g., "KCD2_TPVCamera.ini"). */
    [[nodiscard]] inline std::string get_config_filename()
    {
        return std::string(MOD_NAME) + INI_FILE_EXTENSION;
    }

    /** @brief Gets the camera-presets JSON filename (e.g., "KCD2_TPVCamera_presets.json"). */
    [[nodiscard]] inline std::string get_presets_filename()
    {
        return std::string(MOD_NAME) + PRESETS_FILE_SUFFIX;
    }

    // The camera rules beside the ASI: the main file, then the patches KCD2_TPVCamera_rules_<Name>.txt in name order,
    // then the player's own KCD2_TPVCamera_rules_User.txt (Rules::rule_files()). The release ships no main file: the
    // built-in copy (build/template/KCD2_TPVCamera_rules.txt, embedded at build time) takes its place.
    constexpr const char *RULES_FILE_SUFFIX = "_rules.txt";
    // The [Advanced] ExportRules output: the built-in copy, byte for byte, for reference. The mod never reads it
    // (Rules::rule_files() skips every name ending in Rules::REFERENCE_SUFFIX).
    constexpr const char *RULES_DEFAULT_EXPORT_SUFFIX = "_rules.default.txt";
    // A rules file larger than this is refused (an editor or a copy gone wrong, not a rules file).
    constexpr std::size_t RULES_FILE_MAX_BYTES = std::size_t{1} << 20;
    // Distinct (model, kind, state) verdicts the Trace log lists per rule set, before it stops listing new ones.
    constexpr int RULES_TRACE_SET = 8192;
    // How often the rules watcher lists the rules files beside the ASI, and how long a change must hold still before
    // it is read (an editor saves in steps: write, rename, replace).
    constexpr int RULES_POLL_MS = 1000;
    constexpr int RULES_SETTLE_MS = 250;
    // Changed rules a reload names one by one in the log; the rest are counted.
    constexpr std::size_t RULES_CHANGE_LOG_LINES = 40;

    /** @brief Gets the main camera-rules filename (e.g., "KCD2_TPVCamera_rules.txt"). */
    [[nodiscard]] inline std::string get_rules_filename()
    {
        return std::string(MOD_NAME) + RULES_FILE_SUFFIX;
    }

    /** @brief Gets the exported built-in rules filename (e.g., "KCD2_TPVCamera_rules.default.txt"). */
    [[nodiscard]] inline std::string get_rules_default_export_filename()
    {
        return std::string(MOD_NAME) + RULES_DEFAULT_EXPORT_SUFFIX;
    }

    // The signature file beside the ASI: optional repairs of the built-in signatures, merged over them by label.
    constexpr const char *SIGNATURE_FILE_SUFFIX = ".signatures.ini";
    // The [Advanced] ExportSignatures output: every built-in signature with its captured baselines.
    constexpr const char *SIGNATURE_EXPORT_SUFFIX = ".signatures.captured.ini";
    // The signature-contract epoch a signature file must declare. Bump it only when an in-code change makes older
    // signature files incompatible (a renamed label, a dropped signature). A file for another epoch is ignored.
    constexpr std::uint32_t SIGNATURE_REVISION = 1;

    /** @brief Log file name passed to the DMK Session (string-view-safe literal). */
    constexpr const char *LOG_FILE_NAME = "KCD2_TPVCamera.log";
    /** @brief Per-PID instance-mutex prefix so duplicate ASI loads bail cleanly. */
    constexpr const char *INSTANCE_MUTEX_PREFIX = "KCD2_TPVCamera_";

    // Default Configuration Values
    /** @brief Default logging level ("INFO"). */
    constexpr const char *DEFAULT_LOG_LEVEL = "INFO";
} // namespace Constants
#endif // TPVCAMERA_CONSTANTS_HPP
