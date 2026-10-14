/**
 * @file camera_rules_files.hpp
 * @brief The camera rules in force: the rules files beside the ASI, or the copy built into the mod.
 *
 * load_at_start() reads KCD2_TPVCamera_rules.txt, the patches KCD2_TPVCamera_rules_<Name>.txt in name order and the
 * player's KCD2_TPVCamera_rules_User.txt last (Rules::rule_files()), compiles them (camera_rules.hpp), logs one line
 * per source and publishes the set through an atomic shared pointer. The built-in copy is
 * build/template/KCD2_TPVCamera_rules.txt, embedded at build time, and takes the main file's place when that file is
 * missing (the release ships none) or cannot be read; it is never merged with a present file. export_built_in() writes
 * it beside the ASI as KCD2_TPVCamera_rules.default.txt, a reference the mod never reads. The watcher (start_watcher())
 * lists the files once a second and publishes an edited set as the next generation, but only when every line of the
 * files that changed parses.
 */
#ifndef TPVCAMERA_CAMERA_RULES_FILES_HPP
#define TPVCAMERA_CAMERA_RULES_FILES_HPP

#include "camera_rules.hpp"

#include <cstdint>
#include <memory>
#include <string_view>

namespace TPVCamera::Rules
{

    /**
     * @brief The rule set in force, or null before load_at_start() and when no set could be built.
     * @note Any thread. std::atomic<std::shared_ptr> is not lock-free on either toolchain (a short internal lock guards
     *       the reference count), so a caller loads it once per scan, per material load or per inventory walk.
     */
    [[nodiscard]] std::shared_ptr<const RuleSet> current() noexcept;

    /** @brief The generation of the set in force, 0 before one is published. */
    [[nodiscard]] std::uint64_t current_generation() noexcept;

    /**
     * @brief Reads the rules files beside the ASI, compiles and publishes them, and logs what it found.
     * @details A missing main file gives the built-in copy, the usual case, logged at Info with the count of its
     *          rules; an unreadable one, or one larger than Constants::RULES_FILE_MAX_BYTES, gives it with a Warning
     *          first. Such a patch is skipped with a Warning. A present main file without @default is used as it is:
     *          an object no rule matches is excluded, with a Warning. Each skipped line is an Error naming its file
     *          and line, and the rest applies.
     * @note Init only, after the INI loads (so its LogLevel applies) and before the hooks go up.
     */
    void load_at_start() noexcept;

    /**
     * @brief Writes the built-in copy, byte for byte, to KCD2_TPVCamera_rules.default.txt beside the ASI
     *        (Constants::RULES_DEFAULT_EXPORT_SUFFIX), replacing the file there.
     * @details The file is a reference: no listing reads it (REFERENCE_SUFFIX), and its lines are the lines the log
     *          names "built-in KCD2_TPVCamera_rules.txt:<line>". It is written to a temporary file beside it first and
     *          then renamed over it, so a failed write leaves the previous export whole. Info with the path and the
     *          size when written, a Warning when not.
     * @note Init, or the INI watcher thread when [Advanced] ExportRules turns on. Not concurrently with itself.
     */
    void export_built_in() noexcept;

    /**
     * @brief Starts the rules watcher: a worker thread that lists the rules files once a second (Constants::
     *        RULES_POLL_MS) and reads a change once it held still for Constants::RULES_SETTLE_MS.
     * @details An edited set is published as the next generation when no file added or edited since the rules in force
     *          has a line with an Error, and the log names the files and the rules that changed. Otherwise the log
     *          lists the Errors and the rules in force stay. A bad line in a file left as it was is skipped, as at
     *          start. A main file that is gone or cannot be read gives the built-in copy, and such a patch is skipped,
     *          as at start. A file another program is still writing is read once it lets go. A listing that fails
     *          changes nothing (one Warning). A save that changes no rule publishes nothing.
     * @note Init only, after load_at_start() and the hooks. A watcher that cannot start is a Warning: edits then apply
     *       at the next start.
     */
    void start_watcher() noexcept;

    /**
     * @brief Stops the rules watcher and joins it. Idempotent.
     * @note Shutdown only, before the hooks retire: the worker holds a module reference until its join.
     */
    void stop_watcher() noexcept;

    /** @brief Where a model path with an unusual form was seen, for the Debug log. */
    enum class NameSite : std::uint8_t
    {
        Scan, // the occluder scan, which compares names as written
        Twin, // the twin scopes, which turn backslashes into slashes
    };

    /** @brief An unusual form of a model path that no shipped rule matches as written. */
    enum class NameForm : std::uint8_t
    {
        Backslash,
        LeadingSlash,
    };

    /**
     * @brief Counts one name of @p form seen at @p site and returns the running count.
     * @note Any thread. Builds nothing that needs unwinding, so a structured-exception frame can call it.
     */
    [[nodiscard]] std::uint32_t count_name_form(NameSite site, NameForm form) noexcept;

    /** @brief Logs @p name at Debug as one of the first three of its form at @p site (later counts log nothing). */
    void log_name_form(NameSite site, NameForm form, std::string_view name, std::uint32_t count) noexcept;

    /** @brief Logs at Debug how many names of each unusual form each site saw. Shutdown only. */
    void log_name_form_totals() noexcept;

    /**
     * @brief Logs at Trace the verdict of one object: its model path in lower case, how it is placed, the verdict and
     *        the deciding rule's file, line and match.
     * @note The caller checks the Trace level and deduplicates, so a model is listed once per rule set.
     */
    void log_verdict(const RuleSet &rules, std::string_view name, Kind kind, State state,
                     const Verdict &verdict) noexcept;

} // namespace TPVCamera::Rules

#endif // TPVCAMERA_CAMERA_RULES_FILES_HPP
