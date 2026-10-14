/**
 * @file camera_rules_files.cpp
 * @brief Reading, publishing, watching and logging the camera rules (see camera_rules_files.hpp).
 */

#include "camera_rules_files.hpp"
#include "constants.hpp"
#include "version.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace TPVCamera::Rules
{

    namespace
    {
        // build/template/KCD2_TPVCamera_rules.txt, embedded by CMake (generated, one trailing zero byte).
        constexpr unsigned char k_built_in[] = {
#include "default_rules.inc"
        };

        // The ExportRules output must stay a name no listing reads, so writing it never changes the rules in force.
        static_assert(std::string_view(Constants::RULES_DEFAULT_EXPORT_SUFFIX).ends_with(REFERENCE_SUFFIX));

        // Written by load_at_start() on the init thread and then by the watcher, one at a time; read by every thread
        // through current().
        std::atomic<std::shared_ptr<const RuleSet>> s_current{nullptr};
        std::atomic<std::uint64_t> s_generation{0};

        // Unusual name forms seen, by site and form (see count_name_form()).
        std::array<std::atomic<std::uint32_t>, 4> s_name_forms{};

        /** @brief One rules file beside the ASI as a listing sees it: a new size or write time is an edit. */
        struct FileStamp
        {
            std::wstring disk_name; // as on disk, to open it
            std::string name;       // the same in UTF-8, for the log
            SourceRole role = SourceRole::Main;
            std::uint64_t size = 0;
            std::uint64_t written = 0; // last write time, FILETIME ticks

            [[nodiscard]] bool operator==(const FileStamp &) const = default;
        };

        // The last listing read, in layer order: a listing that equals it needs no read. Written by load_at_start()
        // before the watcher starts, then only by the watcher.
        std::vector<FileStamp> s_files;
        // The files the set in force was read from, as they were then: a file listed with another stamp, or not
        // listed then, holds an edit that is not in force. Written like s_files.
        std::vector<FileStamp> s_applied;
        // The watcher. StoppableWorker holds a module reference until its join, so a retired logic image never unmaps
        // under it, and a failed join shows as a Worker pin that refuses retirement.
        std::optional<DMK::StoppableWorker> s_watcher;

        [[nodiscard]] std::size_t form_slot(NameSite site, NameForm form) noexcept
        {
            return static_cast<std::size_t>(site) * 2 + static_cast<std::size_t>(form);
        }

        [[nodiscard]] std::string_view built_in_text() noexcept
        {
            return {reinterpret_cast<const char *>(k_built_in), sizeof(k_built_in) - 1};
        }

        /** @brief Why a file was not read, or Read when its bytes are in the out text. */
        enum class FileRead : std::uint8_t
        {
            Read,
            Missing,
            Unreadable,
            Busy, // another program holds it open without sharing it (a save in progress)
            TooLarge,
        };

        [[nodiscard]] FileRead failure(DWORD error) noexcept
        {
            switch (error)
            {
            case ERROR_FILE_NOT_FOUND:
            case ERROR_PATH_NOT_FOUND:
                return FileRead::Missing;
            case ERROR_SHARING_VIOLATION:
            case ERROR_LOCK_VIOLATION:
                return FileRead::Busy;
            default:
                return FileRead::Unreadable;
            }
        }

        /**
         * @brief Reads @p path whole into @p out, sharing it with any editor that holds it open.
         * @param error Receives the Win32 error of a failed open or read.
         */
        [[nodiscard]] FileRead read_file(const std::filesystem::path &path, std::string &out, DWORD &error)
        {
            const HANDLE file =
                CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                error = GetLastError();
                return failure(error);
            }
            struct Closer
            {
                HANDLE handle;
                ~Closer() noexcept { (void)CloseHandle(handle); }
            } closer{file};
            LARGE_INTEGER size{};
            if (!GetFileSizeEx(file, &size))
            {
                error = GetLastError();
                return FileRead::Unreadable;
            }
            if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > Constants::RULES_FILE_MAX_BYTES)
            {
                return FileRead::TooLarge;
            }
            out.resize(static_cast<std::size_t>(size.QuadPart));
            std::size_t done = 0;
            while (done < out.size())
            {
                DWORD got = 0;
                if (!ReadFile(file, out.data() + done, static_cast<DWORD>(out.size() - done), &got, nullptr))
                {
                    error = GetLastError();
                    return failure(error) == FileRead::Busy ? FileRead::Busy : FileRead::Unreadable;
                }
                if (got == 0)
                {
                    break; // the file shrank while it was read
                }
                done += got;
            }
            out.resize(done);
            return FileRead::Read;
        }

        /**
         * @brief Writes @p bytes to @p path, replacing any file there, and closes it.
         * @param error Receives the Win32 error of a failed create, write or flush.
         */
        [[nodiscard]] bool write_file(const std::filesystem::path &path, std::string_view bytes, DWORD &error)
        {
            const HANDLE file =
                CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                error = GetLastError();
                return false;
            }
            struct Closer
            {
                HANDLE handle;
                ~Closer() noexcept { (void)CloseHandle(handle); }
            } closer{file};
            std::size_t done = 0;
            while (done < bytes.size())
            {
                DWORD wrote = 0;
                if (!WriteFile(file, bytes.data() + done, static_cast<DWORD>(bytes.size() - done), &wrote, nullptr))
                {
                    error = GetLastError();
                    return false;
                }
                if (wrote == 0)
                {
                    error = ERROR_WRITE_FAULT; // nothing written and no error: give up rather than spin
                    return false;
                }
                done += wrote;
            }
            if (!FlushFileBuffers(file))
            {
                error = GetLastError();
                return false;
            }
            return true;
        }

        /** @brief @p wide in UTF-8, or empty when it does not convert. */
        [[nodiscard]] std::string utf8_of(std::wstring_view wide)
        {
            const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                                                 static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
            if (size <= 0)
            {
                return {};
            }
            std::string out(static_cast<std::size_t>(size), '\0');
            (void)WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
                                      out.data(), size, nullptr, nullptr);
            return out;
        }

        /**
         * @brief Lists the rules files in @p folder in the order they layer (rule_files()), each with its size and
         *        last write time.
         * @details One FindFirstFileExW pattern ("KCD2_TPVCamera_rules*") lists the main file and every patch; the
         *          names are then checked whole, so a short 8.3 name that also matches the pattern adds nothing. A
         *          listing that fails gives the main file alone, unstamped, so it is still read by its name.
         * @param error Receives the Win32 error of a listing that failed, 0 when it worked or found nothing.
         * @throws std::bad_alloc If a list cannot allocate.
         */
        [[nodiscard]] std::vector<FileStamp> list_files(const std::filesystem::path &folder, DWORD &error)
        {
            error = 0;
            const std::wstring pattern = (folder / (std::string(Constants::MOD_NAME) + "_rules*")).wstring();
            WIN32_FIND_DATAW data{};
            const HANDLE find =
                FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
            if (find == INVALID_HANDLE_VALUE)
            {
                const DWORD failed = GetLastError();
                if (failed == ERROR_FILE_NOT_FOUND)
                {
                    return {};
                }
                error = failed;
                const std::string main_name = Constants::get_rules_filename();
                return {FileStamp{.disk_name = std::filesystem::path(main_name).wstring(), .name = main_name}};
            }
            struct Closer
            {
                HANDLE handle;
                ~Closer() noexcept { (void)FindClose(handle); }
            } closer{find};
            std::vector<FileStamp> found;
            std::vector<std::string> names;
            do
            {
                if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                {
                    continue;
                }
                std::string name = utf8_of(data.cFileName);
                if (name.empty())
                {
                    continue;
                }
                names.push_back(name);
                found.push_back(FileStamp{
                    .disk_name = data.cFileName,
                    .name = std::move(name),
                    .size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow,
                    .written = (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                               data.ftLastWriteTime.dwLowDateTime,
                });
            } while (FindNextFileW(find, &data));
            std::vector<FileStamp> files;
            for (const RuleFile &file : rule_files(names, Constants::MOD_NAME))
            {
                const auto at = std::ranges::find(found, file.name, &FileStamp::name);
                at->role = file.role;
                files.push_back(std::move(*at));
            }
            return files;
        }

        /** @brief The rules files read and the layers that view them: the main file or the built-in copy first. */
        struct LoadedLayers
        {
            std::vector<std::string> texts;
            std::vector<SourceText> layers;
            // A file another program is still writing (a live reload stops at it): read nothing now, try again later.
            std::string busy;
            // Layer 0 is the built-in copy, not a main file beside the ASI.
            bool built_in = false;
        };

        /**
         * @brief Reads the files of @p files into layers and logs each one it cannot use.
         * @details A main file that is missing gives the built-in copy as layer 0 (log_set() says so when the set is
         *          published); one that cannot be read or is larger than Constants::RULES_FILE_MAX_BYTES gives it with
         *          a Warning, and such a patch is skipped (Warning). With @p live, a file another program holds open
         *          without sharing stops the read and is named in LoadedLayers::busy.
         * @throws std::bad_alloc If a file or a list cannot allocate.
         */
        [[nodiscard]] LoadedLayers read_layers(const std::filesystem::path &folder, const std::vector<FileStamp> &files,
                                               bool live)
        {
            DMK::Logger &logger = DMK::log();
            LoadedLayers out;
            std::vector<const FileStamp *> read;
            const bool listed = !files.empty() && files.front().role == SourceRole::Main;
            bool built_in = !listed;
            for (const FileStamp &file : files)
            {
                std::string text;
                DWORD error = 0;
                const FileRead result = read_file(folder / file.disk_name, text, error);
                if (result == FileRead::Busy && live)
                {
                    out.busy = file.name;
                    return out;
                }
                if (result == FileRead::Read)
                {
                    out.texts.push_back(std::move(text));
                    read.push_back(&file);
                    continue;
                }
                const bool main = file.role == SourceRole::Main;
                built_in = built_in || main;
                const std::string then =
                    main ? std::format("using the built-in rules of TPVCamera {} instead", Version::VERSION_TAG)
                         : std::string{"it is skipped and the other rules files apply"};
                if (result == FileRead::Missing)
                {
                    logger.warning("Rules: {} is gone; {}", file.name, then);
                }
                else if (result == FileRead::TooLarge)
                {
                    logger.warning("Rules: {} is larger than {} bytes, so it is not a rules file; {}", file.name,
                                   Constants::RULES_FILE_MAX_BYTES, then);
                }
                else
                {
                    logger.warning("Rules: {} cannot be read (Win32 error {}); {}", file.name, error, then);
                }
            }
            if (built_in)
            {
                out.layers.push_back({.name = "built-in " + Constants::get_rules_filename(), .text = built_in_text()});
            }
            out.built_in = built_in;
            for (std::size_t i = 0; i < read.size(); ++i)
            {
                out.layers.push_back({.name = read[i]->name, .text = out.texts[i], .role = read[i]->role});
            }
            return out;
        }

        /** @brief "5 state, 1 state with a path, 2 text, 59 path, 1 default", without the forms a source lacks. */
        [[nodiscard]] std::string counts_text(const SourceCounts &counts)
        {
            std::string text;
            const auto add = [&text](int n, std::string_view what)
            {
                if (n != 0)
                {
                    text += std::format("{}{} {}", text.empty() ? "" : ", ", n, what);
                }
            };
            add(counts.state, "state");
            add(counts.state_path, "state with a path");
            add(counts.text, "text");
            add(counts.path, "path");
            add(counts.defaults, "default");
            return text;
        }

        /** @brief Logs @p diagnostics, each at its level. */
        void log_diagnostics(const std::vector<Diagnostic> &diagnostics)
        {
            DMK::Logger &logger = DMK::log();
            for (const Diagnostic &diagnostic : diagnostics)
            {
                if (diagnostic.level == Diagnostic::Level::Error)
                {
                    logger.error("Rules: {}", diagnostic.text);
                }
                else
                {
                    logger.warning("Rules: {}", diagnostic.text);
                }
            }
        }

        /** @brief The rules one source contributed, every form counted. */
        [[nodiscard]] int total_of(const SourceCounts &counts) noexcept
        {
            return counts.state + counts.state_path + counts.text + counts.path + counts.defaults;
        }

        /**
         * @brief Logs one line per source of @p set, in layer order, and the summary line with its catch-all.
         * @param built_in Layer 0 is the built-in copy: a line first says so, and where its lines can be read.
         */
        void log_set(const RuleSet &set, bool built_in)
        {
            DMK::Logger &logger = DMK::log();
            if (built_in && !set.counts().empty())
            {
                logger.info(
                    "Rules: built-in rules in force ({} rule(s), TPVCamera {}); this log names them '{}:<line>', "
                    "the same lines as in {}, which [Advanced] ExportRules = true writes beside the ASI",
                    total_of(set.counts().front()), Version::VERSION_TAG, set.counts().front().name,
                    Constants::get_rules_default_export_filename());
            }
            for (const SourceCounts &counts : set.counts())
            {
                const int total = total_of(counts);
                logger.info("Rules: {}: {} rule(s){}", counts.name, total,
                            total == 0 ? std::string{} : " (" + counts_text(counts) + ")");
            }
            const std::uint16_t catch_all = set.catch_all();
            const Rule *fallback = set.rule(catch_all);
            logger.info("Rules: {} rule(s) in force (generation {}); {}", set.size(), set.generation(),
                        fallback != nullptr
                            ? std::format("catch-all @default = {} ({})",
                                          describe(Verdict{.behaviour = fallback->behaviour,
                                                           .check = fallback->check,
                                                           .outcome = fallback->outcome,
                                                           .mesh = fallback->mesh,
                                                           .rule = catch_all}),
                                          set.where(catch_all))
                            : std::string{"no @default: TPVCamera leaves every object no rule matches alone"});
        }

        /** @brief Makes @p set the set in force; its generation follows it for current_generation(). */
        void store(std::shared_ptr<const RuleSet> set) noexcept
        {
            const std::uint64_t generation = set->generation();
            s_current.store(std::move(set), std::memory_order_release);
            s_generation.store(generation, std::memory_order_release);
        }

        /** @brief What a listing changed, as "<file> changed, <file> added, <file> removed". */
        [[nodiscard]] std::string file_changes(const std::vector<FileStamp> &before,
                                               const std::vector<FileStamp> &after)
        {
            std::string text;
            const auto add = [&text](std::string_view name, std::string_view what)
            { text += std::format("{}{} {}", text.empty() ? "" : ", ", name, what); };
            for (const FileStamp &file : after)
            {
                const auto old = std::ranges::find(before, file.name, &FileStamp::name);
                if (old == before.end())
                {
                    add(file.name, "added");
                }
                else if (old->size != file.size || old->written != file.written)
                {
                    add(file.name, "changed");
                }
            }
            for (const FileStamp &file : before)
            {
                if (std::ranges::find(after, file.name, &FileStamp::name) == after.end())
                {
                    add(file.name, "removed");
                }
            }
            return text.empty() ? std::string{"the rules files changed"} : text;
        }

        /** @brief True when @p name is listed in @p files but not in @p applied as it is now: added or edited since. */
        [[nodiscard]] bool edited_since(std::string_view name, const std::vector<FileStamp> &files,
                                        const std::vector<FileStamp> &applied)
        {
            const auto now = std::ranges::find(files, name, &FileStamp::name);
            if (now == files.end())
            {
                return false; // the built-in copy
            }
            const auto then = std::ranges::find(applied, name, &FileStamp::name);
            return then == applied.end() || then->size != now->size || then->written != now->written;
        }

        /**
         * @brief Reads, compiles and publishes the rules of @p files as the next generation, when no file added or
         *        edited since the rules in force has a line with an Error; logs what changed, or why the rules in
         *        force stay.
         * @details A bad line in a file left as it was (a mod's patch written for a later TPVCamera) is skipped, as at
         *          start, so it never holds back an edit to another file. A file that cannot be read is handled as at
         *          start (read_layers()).
         * @return The name of a file another program is still writing (nothing was read, and the listing in force
         *         stays, so the next tick tries again), or empty.
         * @throws std::bad_alloc If a file, a list or the set cannot allocate; the listing in force then stays.
         * @note The watcher thread only.
         */
        [[nodiscard]] std::string reload(const std::filesystem::path &folder, const std::vector<FileStamp> &files)
        {
            DMK::Logger &logger = DMK::log();
            LoadedLayers loaded = read_layers(folder, files, true);
            if (!loaded.busy.empty())
            {
                return loaded.busy;
            }
            // s_files takes the listing at each exit below, after the read, the compile and the logs, so a failure (out
            // of memory) leaves it as it was and the next tick reads the edit again.
            const std::string what = file_changes(s_files, files);
            const std::shared_ptr<const RuleSet> before = current();
            const std::uint64_t in_force = s_generation.load(std::memory_order_acquire);
            std::vector<Diagnostic> diagnostics;
            std::shared_ptr<const RuleSet> set = compile(loaded.layers, in_force + 1, diagnostics);
            log_diagnostics(diagnostics);
            std::size_t errors = 0;
            std::vector<std::string_view> held;
            for (const Diagnostic &diagnostic : diagnostics)
            {
                if (diagnostic.level != Diagnostic::Level::Error || diagnostic.layer >= loaded.layers.size())
                {
                    continue;
                }
                const std::string_view name = loaded.layers[diagnostic.layer].name;
                if (!edited_since(name, files, s_applied))
                {
                    continue;
                }
                ++errors;
                if (std::ranges::find(held, name) == held.end())
                {
                    held.push_back(name);
                }
            }
            if (errors != 0)
            {
                std::string names;
                for (const std::string_view name : held)
                {
                    names += std::format("{}{}", names.empty() ? "" : ", ", name);
                }
                logger.error("Rules: {}, but {} line(s) in {} have an error (listed above), so the edit is not applied "
                             "and the rules in force stay (generation {}); fix those lines and save again",
                             what, errors, names, in_force);
                s_files = files;
                return {};
            }
            if (before != nullptr)
            {
                const SetChanges changes = compare(*before, *set);
                if (changes.lines.empty() && changes.moved == 0)
                {
                    logger.info("Rules: {}; no rule changed, so the rules in force stay (generation {})", what,
                                in_force);
                    s_files = files;
                    s_applied = files;
                    return {};
                }
                logger.info("Rules: {}; reloaded as generation {}: {} rule(s) changed{}", what, set->generation(),
                            changes.lines.size(),
                            changes.moved != 0 ? std::format(", {} only moved to another line or file", changes.moved)
                                               : std::string{});
                const std::size_t listed = std::min(changes.lines.size(), Constants::RULES_CHANGE_LOG_LINES);
                for (std::size_t i = 0; i < listed; ++i)
                {
                    logger.info("Rules:   {}", changes.lines[i]);
                }
                if (changes.lines.size() > listed)
                {
                    logger.info("Rules:   ... and {} more", changes.lines.size() - listed);
                }
            }
            log_set(*set, loaded.built_in);
            store(std::move(set));
            s_files = files;
            s_applied = files;
            return {};
        }

        /**
         * @brief The watcher's body: lists the rules files every Constants::RULES_POLL_MS and reloads a listing that
         *        changed and then held still for Constants::RULES_SETTLE_MS.
         * @param token Every wait is a stop-token wait, so a stop ends the body at once and the join is quick.
         */
        void watch(const std::stop_token &token)
        {
            DMK::Logger &logger = DMK::log();
            std::mutex mutex;
            std::condition_variable_any wake;
            // False once a stop was requested.
            const auto sleep = [&](int ms)
            {
                std::unique_lock lock(mutex);
                (void)wake.wait_for(lock, token, std::chrono::milliseconds{ms}, [] { return false; });
                return !token.stop_requested();
            };
            try
            {
                const std::filesystem::path folder(DMK::filesystem::get_runtime_directory());
                std::string busy_logged;
                DWORD listing_error_logged = 0;
                while (sleep(Constants::RULES_POLL_MS))
                {
                    try
                    {
                        DWORD error = 0;
                        const std::vector<FileStamp> listed = list_files(folder, error);
                        // A failed listing is no edit: its fallback (the main file alone, see list_files()) would
                        // drop every patch and the User file from the rules in force.
                        if (error != 0)
                        {
                            if (error != listing_error_logged)
                            {
                                logger.warning("Rules: the folder beside the ASI cannot be listed (Win32 error {}); "
                                               "the rules in force stay until it can",
                                               error);
                            }
                            listing_error_logged = error;
                            continue;
                        }
                        listing_error_logged = 0;
                        if (listed == s_files)
                        {
                            continue;
                        }
                        // An editor saves in steps (write a copy, rename, replace): read only a listing that holds
                        // still.
                        if (!sleep(Constants::RULES_SETTLE_MS))
                        {
                            break;
                        }
                        const std::vector<FileStamp> settled = list_files(folder, error);
                        if (error != 0 || settled != listed)
                        {
                            continue;
                        }
                        const std::string busy = reload(folder, settled);
                        if (!busy.empty() && busy != busy_logged)
                        {
                            logger.info("Rules: {} is open in a program that is still writing it; it is read once that "
                                        "program lets go of it",
                                        busy);
                        }
                        busy_logged = busy;
                    }
                    catch (const std::bad_alloc &)
                    {
                        (void)logger.try_log(DMK::LogLevel::Warning, "Rules: the rules files could not be read (out of "
                                                                     "memory); the rules in force stay");
                    }
                }
            }
            catch (...)
            {
                (void)logger.try_log(DMK::LogLevel::Error, "Rules: the rules watcher stopped on an error; edits to the "
                                                           "rules files apply at the next start");
            }
        }
    } // namespace

    std::shared_ptr<const RuleSet> current() noexcept
    {
        return s_current.load(std::memory_order_acquire);
    }

    std::uint64_t current_generation() noexcept
    {
        return s_generation.load(std::memory_order_acquire);
    }

    void load_at_start() noexcept
    {
        DMK::Logger &logger = DMK::log();
        try
        {
            const std::filesystem::path folder(DMK::filesystem::get_runtime_directory());
            DWORD error = 0;
            std::vector<FileStamp> files = list_files(folder, error);
            if (error != 0)
            {
                logger.warning("Rules: the folder beside the ASI cannot be listed (Win32 error {}), so only {} is read",
                               error, Constants::get_rules_filename());
            }
            const LoadedLayers loaded = read_layers(folder, files, false);
            std::vector<Diagnostic> diagnostics;
            std::shared_ptr<const RuleSet> set =
                compile(loaded.layers, s_generation.load(std::memory_order_relaxed) + 1, diagnostics);
            log_diagnostics(diagnostics);
            log_set(*set, loaded.built_in);
            s_files = std::move(files);
            s_applied = s_files;
            store(std::move(set));
        }
        catch (...)
        {
            // Out of memory while reading or compiling: the scan then finds nothing and every hit collides.
            (void)logger.try_log(DMK::LogLevel::Error, "Rules: the camera rules could not be built (out of memory); "
                                                       "TPVCamera leaves every object alone");
        }
    }

    void export_built_in() noexcept
    {
        DMK::Logger &logger = DMK::log();
        try
        {
            const std::filesystem::path path = std::filesystem::path(DMK::filesystem::get_runtime_directory()) /
                                               Constants::get_rules_default_export_filename();
            // Beside the export, so the rename stays on one volume; no listing reads a name that ends in .tmp.
            std::filesystem::path temp = path;
            temp += L".tmp";
            const std::string shown = utf8_of(path.native());
            DWORD error = 0;
            if (!write_file(temp, built_in_text(), error))
            {
                (void)DeleteFileW(temp.c_str());
                logger.warning("Rules: the built-in rules could not be written to {} (Win32 error {})", shown, error);
                return;
            }
            if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                error = GetLastError();
                (void)DeleteFileW(temp.c_str());
                logger.warning("Rules: the built-in rules could not replace {} (Win32 error {})", shown, error);
                return;
            }
            logger.info("Rules: wrote the built-in rules of TPVCamera {} to {} ({} bytes); TPVCamera never reads that "
                        "file, so put changes in {}_rules_User.txt, or rename it to {} to replace the built-in rules",
                        Version::VERSION_TAG, shown, built_in_text().size(), Constants::MOD_NAME,
                        Constants::get_rules_filename());
        }
        catch (...)
        {
            (void)logger.try_log(DMK::LogLevel::Warning, "Rules: the built-in rules could not be exported (out of "
                                                         "memory)");
        }
    }

    void start_watcher() noexcept
    {
        if (s_watcher.has_value())
        {
            return;
        }
        try
        {
            s_watcher.emplace("TPVCamera.rules", [](std::stop_token token) { watch(token); });
            DMK::log().info("Rules: watching the rules files beside the ASI; a saved edit applies within {} ms",
                            Constants::RULES_POLL_MS + Constants::RULES_SETTLE_MS);
        }
        catch (const std::exception &e)
        {
            (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                     "Rules: the rules watcher could not start ({}); edits to the rules files apply at "
                                     "the next start",
                                     e.what());
        }
    }

    void stop_watcher() noexcept
    {
        // Requests the stop and joins: every wait in the body is a stop-token wait. Under the loader lock the worker
        // detaches instead and keeps its module reference.
        s_watcher.reset();
    }

    std::uint32_t count_name_form(NameSite site, NameForm form) noexcept
    {
        return s_name_forms[form_slot(site, form)].fetch_add(1, std::memory_order_relaxed) + 1;
    }

    void log_name_form(NameSite site, NameForm form, std::string_view name, std::uint32_t count) noexcept
    {
        if (count > 3)
        {
            return;
        }
        (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                 "Rules: {} model path {} {}, which no rule path matches as {} "
                                 "(the {} such path seen)",
                                 site == NameSite::Scan ? "a scanned" : "a prepared", name,
                                 form == NameForm::Backslash ? "holds a backslash" : "starts with '/'",
                                 (site == NameSite::Twin && form == NameForm::Backslash) ? "written; its twin scope "
                                                                                           "uses '/' instead"
                                                                                         : "written",
                                 count == 1   ? "first"
                                 : count == 2 ? "second"
                                              : "third");
    }

    void log_name_form_totals() noexcept
    {
        const auto load = [](NameSite site, NameForm form)
        { return s_name_forms[form_slot(site, form)].load(std::memory_order_relaxed); };
        (void)DMK::log().try_log(
            DMK::LogLevel::Debug,
            "Rules: model paths in unusual forms: {} scanned with a backslash, {} scanned with a "
            "leading '/', {} prepared with a backslash, {} prepared with a leading '/'",
            load(NameSite::Scan, NameForm::Backslash), load(NameSite::Scan, NameForm::LeadingSlash),
            load(NameSite::Twin, NameForm::Backslash), load(NameSite::Twin, NameForm::LeadingSlash));
    }

    void log_verdict(const RuleSet &rules, std::string_view name, Kind kind, State state,
                     const Verdict &verdict) noexcept
    {
        try
        {
            std::string path(name);
            std::ranges::transform(path, path.begin(),
                                   [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; });
            if (path.empty())
            {
                path = kind == Kind::Merged ? "<merged patch>" : "<no model path>";
            }
            const std::string placed = (state == State::None || state == State::Merged)
                                           ? std::string{kind_name(kind)}
                                           : std::format("{}, {}", kind_name(kind), state_name(state));
            const std::string decided =
                verdict.rule == NO_RULE
                    ? std::string{"no rule matches and the main file declares no @default"}
                    : std::format("{} '{}'", rules.where(verdict.rule), rules.match_text(verdict.rule));
            (void)DMK::log().try_log(DMK::LogLevel::Trace, "Rules: {} [{}] -> {} ({})", path, placed, describe(verdict),
                                     decided);
        }
        catch (...)
        {
            // A trace line that cannot be built is skipped.
        }
    }

} // namespace TPVCamera::Rules
