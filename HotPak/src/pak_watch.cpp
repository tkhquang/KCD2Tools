/**
 * @file pak_watch.cpp
 * @brief Build-output polling and shadow staging on a dedicated thread.
 */

#include "pak_watch.hpp"
#include "constants.hpp"
#include "pak_names.hpp"
#include "text_util.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace HotPak
{
    namespace
    {
        /** @brief What a source looked like on one poll. */
        struct FileStamp
        {
            std::uint64_t size = 0;
            std::uint64_t write_time = 0;
            bool exists = false;

            [[nodiscard]] bool operator==(const FileStamp &) const noexcept = default;
        };

        /** @brief Per-source polling state, owned by the watcher thread. */
        struct SourceState
        {
            std::wstring path;
            FileStamp last{};
            FileStamp staged{};
            bool staged_any = false;
            bool reported_missing = false;
        };

        // Written once, by the first start_pak_watcher() before the frame hook arms, and read-only afterwards.
        std::wstring s_shadow_dir;
        std::mutex s_pending_mutex;
        std::vector<PendingGeneration> s_pending;
        // Owned by the watcher thread while it runs. start_pak_watcher() touches it only with no watcher running.
        std::uint32_t s_next_generation = 1;
        // StoppableWorker holds a module reference until its join, so a failed join shows as a Worker pin that
        // refuses the unload instead of leaving a thread in unmapped code.
        std::optional<DMK::StoppableWorker> s_watcher;
        std::mutex s_wake_mutex;
        std::condition_variable_any s_wake;

        [[nodiscard]] FileStamp stamp_of(const std::wstring &path) noexcept
        {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
            {
                return FileStamp{};
            }
            return FileStamp{
                .size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow,
                .write_time = (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                              data.ftLastWriteTime.dwLowDateTime,
                .exists = true,
            };
        }

        /**
         * @brief Deletes the shadow copies left by earlier game runs and earlier logic generations, and returns the
         *        next free generation number.
         * @details A copy that CryPak still has open in this process (mounted by an earlier logic generation) cannot be
         *          deleted, and its number is skipped, so a new copy never collides with a mounted one.
         */
        [[nodiscard]] std::uint32_t sweep_shadow_directory()
        {
            std::uint32_t highest = 0;
            unsigned deleted = 0;
            unsigned kept = 0;
            WIN32_FIND_DATAW data{};
            const HANDLE find = FindFirstFileW((s_shadow_dir + L"\\*.pak").c_str(), &data);
            if (find == INVALID_HANDLE_VALUE)
            {
                return 1;
            }
            do
            {
                const std::uint32_t generation = shadow_generation_of(data.cFileName);
                if (generation == 0)
                {
                    continue;
                }
                if (DeleteFileW((s_shadow_dir + L"\\" + data.cFileName).c_str()))
                {
                    ++deleted;
                }
                else
                {
                    ++kept;
                    highest = std::max(highest, generation);
                }
            } while (FindNextFileW(find, &data));
            FindClose(find);
            DMK::log().info("Watch: shadow folder swept ({} stale copies deleted, {} in use kept)", deleted, kept);
            return highest + 1;
        }

        /**
         * @brief Opens the shadow folder on the first call. Later calls reuse it.
         * @details The first call also sweeps the folder. A later call (an INI change) leaves it alone: a generation
         *          the main thread already took but has not mounted yet still needs its file.
         */
        [[nodiscard]] DMK::Result<void> prepare_shadow_directory()
        {
            if (!s_shadow_dir.empty())
            {
                return {};
            }
            wchar_t local_app_data[MAX_PATH]{};
            const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, MAX_PATH);
            if (length == 0 || length >= MAX_PATH)
            {
                const DWORD error = length == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER;
                DMK::log().error("Watch: %LOCALAPPDATA% is not usable (error {})", error);
                return std::unexpected(DMK::Error{DMK::ErrorCode::SystemCallFailed, "watch/localappdata", error});
            }
            std::wstring directory = std::wstring(local_app_data) + L"\\" + constants::SHADOW_FOLDER;
            if (!CreateDirectoryW(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
            {
                const DWORD error = GetLastError();
                DMK::log().error("Watch: cannot create {} (error {})", to_utf8(directory), error);
                return std::unexpected(DMK::Error{DMK::ErrorCode::SystemCallFailed, "watch/shadow_dir", error});
            }
            s_shadow_dir = std::move(directory);
            s_next_generation = std::max(s_next_generation, sweep_shadow_directory());
            return {};
        }

        /**
         * @brief Copies one stable source to a new shadow file and indexes the copy.
         * @return True when a generation was queued; false leaves the source for the next poll.
         */
        [[nodiscard]] bool stage(SourceState &source, const FileStamp &stamp)
        {
            const std::uint32_t generation = s_next_generation;
            const std::wstring shadow = s_shadow_dir + L"\\" + shadow_file_name(source.path, generation);
            if (!CopyFileW(source.path.c_str(), shadow.c_str(), TRUE))
            {
                const DWORD error = GetLastError();
                // A file of that name that the sweep could not see (another process holds it) never frees up.
                if (error == ERROR_FILE_EXISTS)
                {
                    ++s_next_generation;
                }
                DMK::log().debug("Watch: copy of {} failed (error {}); retrying", to_utf8(source.path), error);
                return false;
            }
            // A build that rewrote the file while it was being copied leaves a torn copy; the next poll stages again.
            if (stamp_of(source.path) != stamp)
            {
                DeleteFileW(shadow.c_str());
                return false;
            }
            auto index = read_zip_index(shadow);
            if (!index.has_value())
            {
                DMK::log().warning(
                    "Watch: {} is not a readable pak yet ({}); retrying",
                    to_utf8(source.path),
                    index.error().message()
                );
                DeleteFileW(shadow.c_str());
                return false;
            }
            ++s_next_generation;

            const std::string source_utf8 = to_utf8(source.path);
            DMK::log().info(
                "Watch: staged {} as generation {} ({} entries, {} bytes)",
                source_utf8,
                generation,
                index->size(),
                stamp.size
            );
            PendingGeneration pending{
                .source = source.path,
                .shadow = shadow,
                .file_name = normalize_path(file_name_of(source_utf8)),
                .generation = generation,
                .index = std::move(*index),
            };
            const std::lock_guard lock(s_pending_mutex);
            s_pending.push_back(std::move(pending));
            return true;
        }

        void watch_loop(std::stop_token stop, std::vector<SourceState> sources, std::chrono::milliseconds poll)
        {
            while (!stop.stop_requested())
            {
                for (SourceState &source : sources)
                {
                    const FileStamp now = stamp_of(source.path);
                    if (!now.exists)
                    {
                        if (!source.reported_missing)
                        {
                            DMK::log().warning("Watch: {} does not exist (yet)", to_utf8(source.path));
                            source.reported_missing = true;
                        }
                        source.last = now;
                        continue;
                    }
                    source.reported_missing = false;
                    // Staged only once two consecutive polls see the same stamp, so a build still writing is skipped.
                    const bool stable = now == source.last;
                    source.last = now;
                    const bool is_new = !source.staged_any || now != source.staged;
                    if (stable && is_new && stage(source, now))
                    {
                        source.staged = now;
                        source.staged_any = true;
                    }
                }
                std::unique_lock lock(s_wake_mutex);
                (void)s_wake.wait_for(lock, stop, poll, [] { return false; });
            }
        }
    } // namespace

    DMK::Result<void> start_pak_watcher(std::vector<std::wstring> sources, std::chrono::milliseconds poll)
    {
        stop_pak_watcher();
        DMK_TRY_VOID(prepare_shadow_directory());

        if (sources.empty())
        {
            DMK::log().warning("Watch: [Settings] Paks lists no pak; nothing is watched");
            return {};
        }

        std::vector<SourceState> states;
        states.reserve(sources.size());
        for (std::wstring &path : sources)
        {
            DMK::log().info("Watch: watching {}", to_utf8(path));
            states.push_back(
                SourceState{
                    .path = std::move(path),
                }
            );
        }
        try
        {
            s_watcher.emplace(
                "KCD2_HotPak.watch",
                [states = std::move(states), poll](std::stop_token stop) mutable
                { watch_loop(std::move(stop), std::move(states), poll); }
            );
        }
        catch (const std::exception &e)
        {
            DMK::log().error("Watch: the watcher thread could not start ({})", e.what());
            return std::unexpected(DMK::Error{DMK::ErrorCode::Unknown, "watch/thread"});
        }
        return {};
    }

    void stop_pak_watcher() noexcept
    {
        // Requests the stop and joins: the loop waits only on its stop token. Under the loader lock the worker
        // detaches instead and keeps its module reference.
        s_watcher.reset();
    }

    std::vector<PendingGeneration> take_pending_generations()
    {
        std::vector<PendingGeneration> out;
        const std::unique_lock lock(s_pending_mutex, std::try_to_lock);
        if (lock.owns_lock())
        {
            out.swap(s_pending);
        }
        return out;
    }

    const std::wstring &shadow_directory() noexcept
    {
        return s_shadow_dir;
    }

} // namespace HotPak
