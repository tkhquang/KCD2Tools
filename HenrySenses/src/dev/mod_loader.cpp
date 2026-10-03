/**
 * @file mod_loader.cpp
 * @brief Resident dev loader: owns the process, reloads one logic generation at a time.
 *
 * @details Structure follows DetourModKit's checked-in `examples/staged_reload` pair, which its hot-reload guide
 *          treats as the reference implementation. Four properties matter:
 *
 *          1. **Unique staged names.** Mapping the build output locks that path, so a rebuild cannot replace it and
 *             every reload replays identical bytes while reporting success. Mapping a REUSED name is worse: once an
 *             image is pinned, LoadLibrary on the same path silently returns the pinned predecessor. Each generation
 *             gets a name never used before in this process.
 *          2. **A resident wheel host.** A mouse-wheel binding makes the input engine take a permanent module
 *             keepalive on whichever module hosts wheel capture. Hosting it here - in a module that is never unloaded
 *             - is what lets a logic generation keep a user-bound wheel combo AND still unmap. This loader therefore
 *             links only DetourModKit::WheelHost, never the full archive.
 *          3. **Proof, not assumption.** A generation is only considered gone after its typed Shutdown accepts, a
 *             probe lease opens and closes, and one of its code addresses is observed to become unmapped.
 *          4. **Persistent state.** Engine objects a generation creates can outlive its image (the silhouette stage
 *             the renderer keeps using). The loader owns one zero-filled HenrySensesPersistentState for the process and
 *             hands
 *             it to every generation, so the next generation adopts such objects instead of creating them again.
 *
 *          This loader is dev-only and process-lifetime: it pins itself on attach and never tears the live generation
 *          down. The release build is a single ASI with no reload path at all.
 *
 *          Parameterized by two macros CMakeLists.txt supplies through target_compile_definitions. Every other file
 *          name derives from the first:
 *
 *            HENRYSENSES_LOADER_MOD_NAME      : the mod's deployed stem, "KCD2_HenrySenses"
 *            HENRYSENSES_LOADER_PROCESS_NAME  : the only process this loader runs in, "KingdomCome.exe"
 */

#include "loader_log.hpp"
#include "abi/reload.h"

#include <DetourModKit/abi/wheel_host.h>

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <utility>

// IntelliSense can parse this file without the target's compile definitions, so the macros are stubbed there to keep
// the linter from cascade-erroring on every reference below. The `#error` still fires on a real compile.
#if defined(__INTELLISENSE__)
#ifndef HENRYSENSES_LOADER_MOD_NAME
#define HENRYSENSES_LOADER_MOD_NAME "StubMod"
#endif
#ifndef HENRYSENSES_LOADER_PROCESS_NAME
#define HENRYSENSES_LOADER_PROCESS_NAME "StubGame.exe"
#endif
#else
#ifndef HENRYSENSES_LOADER_MOD_NAME
#error "HENRYSENSES_LOADER_MOD_NAME must be defined to the mod's deployed stem (string)"
#endif
#ifndef HENRYSENSES_LOADER_PROCESS_NAME
#error "HENRYSENSES_LOADER_PROCESS_NAME must be defined to the host process basename (string)"
#endif
#endif

namespace
{
    constexpr const char *MOD_NAME = HENRYSENSES_LOADER_MOD_NAME;
    constexpr const char *PROCESS_NAME = HENRYSENSES_LOADER_PROCESS_NAME;
    /**
     * @brief Stable suffix of a staged generation image.
     * @details Staged generations are "<mod>.genNNNN.logic.dll", so one glob covers the build output and every
     *          staged copy.
     */
    constexpr const char *GENERATION_SUFFIX = ".logic.dll";
    constexpr const char *STAGING_SUBDIR = "staging";

    /// Numpad 9, so the key never collides with TPVCamera's Numpad 0 loader in the same game directory.
    constexpr int RELOAD_VK = VK_NUMPAD9;
    constexpr const char *RELOAD_KEY_NAME = "Numpad 9";
    constexpr DWORD CONTROL_POLL_MS = 100;
    /// Quiescence so an in-flight per-frame detour body returns before FreeLibrary.
    constexpr DWORD POST_SHUTDOWN_MS = 100;
    constexpr DWORD UNMAP_POLL_MS = 10;
    /**
     * @brief Deadline for the post-FreeLibrary unmap poll.
     * @details A release can complete slightly after FreeLibrary returns, so the check polls rather than samples
     *          once. A single sample reports a healthy generation as pinned.
     */
    constexpr DWORD UNMAP_TIMEOUT_MS = 2000;

    /**
     * @brief Caps retained images, by count and by total image bytes, before the loader stops reloading and asks for a
     * restart.
     */
    constexpr unsigned MAX_RETAINED_GENERATIONS = 24;
    constexpr std::uint64_t MAX_RETAINED_BYTES = 128ull * 1024 * 1024;

    /// Loader-owned owner id for the probe lease: ASCII "HSNPROBE". Any value a generation never uses.
    constexpr std::uint64_t LEASE_PROBE_OWNER = UINT64_C(0x48534E50524F4245);

    /// Set in DLL_PROCESS_ATTACH when the pin that makes this loader process-lifetime took hold.
    bool s_loader_pinned = false;

    /**
     * @brief Process-lifetime wheel host. Started once, never stopped: the loader outlives every generation.
     * @details Requires DetourModKit 4.2.0 or newer. Earlier releases lost wheel capture on a lease handover: the
     *          first generation captured wheel notches and every generation after a reload let them pass through to
     *          the game while the route still reported Ready.
     */
    WheelHostTable s_wheel_host{};
    std::uint64_t s_host_identity = 0;
    bool s_wheel_host_live = false;

    /// Process-lifetime state area. Zero-filled static storage, stamped once before the first generation.
    HenrySensesPersistentState s_persistent{};

    /// Latched when a generation cannot be proved gone. A further reload stacks unknown state.
    bool s_restart_required = false;
    unsigned s_generation_counter = 0;
    unsigned s_retained_generations = 0;
    std::uint64_t s_retained_bytes = 0;

    using InitFn = unsigned(__cdecl *)(const HenrySensesReloadInitRequest *) noexcept;
    using ShutdownFn = unsigned(__cdecl *)() noexcept;
    using RevisionFn = const char *(__cdecl *)() noexcept;

    /// One mapped generation and everything needed to retire it.
    struct Generation
    {
        HMODULE module = nullptr;
        InitFn init = nullptr;
        ShutdownFn shutdown = nullptr;
        RevisionFn revision = nullptr;
        /// An address inside the image, used to prove the unmap. Any exported code address works.
        const void *unmap_address = nullptr;
        std::uint64_t generation_id = 0;
        std::string path;
        /// Size of the staged image, charged against the retention byte budget if it cannot be proved gone.
        std::uint64_t image_bytes = 0;
    };

    std::optional<Generation> s_current;
    char s_log_path[MAX_PATH]{};
    std::string s_loader_dir;
    std::string s_log_prefix;
    std::string s_logic_dll_name;
    std::string s_logic_pdb_name;
    std::string s_generation_prefix;

    /// Writes one line to the debugger and to the loader's own log file.
    void log_msg(const char *msg) noexcept
    {
        char line[768];
        if (std::snprintf(line, sizeof(line), "%s%s", s_log_prefix.c_str(), msg) <= 0)
        {
            return;
        }

        // The loader keeps its OWN log. The mod's log belongs to a Session that dies with each generation, and the
        // loader's most important lines are emitted while no Session exists at all.
        if (s_log_path[0] == '\0')
        {
            HenrySenses::dev::echo_to_debugger(line);
            return;
        }
        HenrySenses::dev::append_line(s_log_path, line);
    }

    /// Formats one line into a fixed stack buffer and forwards it to log_msg.
    template <class... Args> void logf(const char *fmt, Args... args) noexcept
    {
        char buffer[768];
        if (std::snprintf(buffer, sizeof(buffer), fmt, args...) > 0)
        {
            log_msg(buffer);
        }
    }

    /**
     * @brief Reports whether this process is the game the loader targets.
     * @details ASI hosts fan the loader out into every executable in the game directory, crash handlers and
     *          launcher stubs included. Only the game process gets a control thread.
     */
    [[nodiscard]] bool running_in_game_process() noexcept
    {
        char path[MAX_PATH]{};
        const DWORD len = GetModuleFileNameA(nullptr, path, MAX_PATH);
        if (len == 0 || len >= MAX_PATH)
        {
            return false;
        }
        const char *const slash = std::strrchr(path, '\\');
        const char *const exe = (slash != nullptr) ? slash + 1 : path;
        return _stricmp(exe, PROCESS_NAME) == 0;
    }

    /// Returns the directory of module @p self, with a trailing separator, or an empty string on failure.
    std::string loader_dir(HMODULE self)
    {
        char path[MAX_PATH]{};
        if (GetModuleFileNameA(self, path, MAX_PATH) == 0)
        {
            return {};
        }
        char *const slash = std::strrchr(path, '\\');
        if (slash == nullptr)
        {
            return {};
        }
        slash[1] = '\0';
        return std::string{path};
    }

    /**
     * @brief Formats the size and last-write time of @p path as "bytes=N built=YYYY-MM-DD HH:MM:SS".
     * @details This is the loader's own witness of what it mapped, taken from the file it copied. It cannot go stale
     *          the way a compiled-in __TIME__ can, because the loader reads it from disk at load time instead of
     *          baking it into one translation unit at compile time.
     */
    std::string file_identity(const std::string &path)
    {
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &info) == 0)
        {
            return "bytes=? built=?";
        }

        SYSTEMTIME utc{};
        SYSTEMTIME local{};
        char stamp[32] = "?";
        if (FileTimeToSystemTime(&info.ftLastWriteTime, &utc) != 0 &&
            SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local) != 0)
        {
            (void)std::snprintf(
                stamp,
                sizeof(stamp),
                "%04u-%02u-%02u %02u:%02u:%02u",
                local.wYear,
                local.wMonth,
                local.wDay,
                local.wHour,
                local.wMinute,
                local.wSecond
            );
        }

        const auto bytes =
            (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | static_cast<std::uint64_t>(info.nFileSizeLow);
        char out[96];
        (void)std::snprintf(out, sizeof(out), "bytes=%llu built=%s", static_cast<unsigned long long>(bytes), stamp);
        return out;
    }

    /// Returns the full path of staged generation number @p generation.
    std::string generation_path(unsigned generation)
    {
        char name[160];
        std::snprintf(name, sizeof(name), "%s%04u%s", s_generation_prefix.c_str(), generation, GENERATION_SUFFIX);
        return s_loader_dir + name;
    }

    /// Moves one staged file into the loader directory, and does nothing when the file is absent.
    void move_staged_file(const std::string &staging_dir, const std::string &filename)
    {
        const std::string src = staging_dir + filename;
        if (GetFileAttributesA(src.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            return;
        }
        CopyFileA(src.c_str(), (s_loader_dir + filename).c_str(), FALSE);
        DeleteFileA(src.c_str());
    }

    /// Promotes a freshly built logic DLL (and its PDB) out of the staging directory.
    void promote_from_staging()
    {
        const std::string staging_dir = s_loader_dir + STAGING_SUBDIR + "\\";
        const std::string staged_dll = staging_dir + s_logic_dll_name;
        if (GetFileAttributesA(staged_dll.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            return; // nothing new was built
        }
        if (!CopyFileA(staged_dll.c_str(), (s_loader_dir + s_logic_dll_name).c_str(), FALSE))
        {
            log_msg("Failed to promote the staged logic DLL");
            return;
        }
        DeleteFileA(staged_dll.c_str());
        move_staged_file(staging_dir, s_logic_pdb_name);
        log_msg("Promoted staged logic DLL");
    }

    /**
     * @brief Deletes staged copies left by earlier runs.
     * @details A copy that still backs a mapped image stays locked, so failures are expected and ignored.
     */
    void sweep_stale_generations()
    {
        unsigned removed = 0;
        WIN32_FIND_DATAA found{};
        const std::string pattern = s_loader_dir + s_generation_prefix + "*" + GENERATION_SUFFIX;
        const HANDLE search = FindFirstFileA(pattern.c_str(), &found);
        if (search == INVALID_HANDLE_VALUE)
        {
            return;
        }
        do
        {
            if (DeleteFileA((s_loader_dir + found.cFileName).c_str()))
            {
                ++removed;
            }
        } while (FindNextFileA(search, &found));
        FindClose(search);
        if (removed != 0)
        {
            logf("Swept %u stale generation file(s) from previous runs", removed);
        }
    }

    /**
     * @brief Counts the persistent slots a generation has written.
     * @details The loader never interprets the slots. The count only shows in the log whether a retiring generation
     *          left state for its successor.
     */
    [[nodiscard]] unsigned persistent_slots_in_use() noexcept
    {
        unsigned used = 0;
        for (const std::uint64_t slot : s_persistent.slots)
        {
            if (slot != 0)
            {
                ++used;
            }
        }
        return used;
    }

    /**
     * @brief Waits for an address inside the retired image to stop belonging to any loaded module.
     * @details Address-based rather than name-based: it asks the loader the exact question that matters, and
     *          UNCHANGED_REFCOUNT keeps the probe from perturbing the count it measures. Probing a freed address is
     *          safe: the call fails, which IS the answer.
     */
    [[nodiscard]] bool wait_for_unmap(const void *address) noexcept
    {
        if (address == nullptr)
        {
            return false; // no probe address means no proof
        }
        for (DWORD waited = 0; waited < UNMAP_TIMEOUT_MS; waited += UNMAP_POLL_MS)
        {
            HMODULE owner = nullptr;
            if (GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(address),
                    &owner
                ) == 0)
            {
                return true;
            }
            Sleep(UNMAP_POLL_MS);
        }
        return false;
    }

    /**
     * @brief Confirms the retired generation left no lease open on the resident wheel host.
     * @details The host allows one lease at a time, so a successful open proves the generation closed its own. A
     *          failed close leaves host state unknown, which is as serious as a failed unmap.
     */
    [[nodiscard]] bool host_lease_is_closed(std::uint64_t generation_id) noexcept
    {
        WheelHostLease probe = 0;
        const std::int32_t open_status =
            s_wheel_host.open_lease(s_wheel_host.host_context, LEASE_PROBE_OWNER, generation_id, &probe);
        if (open_status != DMK_WHEELHOST_OK)
        {
            // A lease the generation left open, or a host that refuses the probe, leaves host state unknown.
            s_restart_required = true;
            logf(
                "Generation %llu left its wheel-host lease open (status %d); restart required",
                static_cast<unsigned long long>(generation_id),
                open_status
            );
            return false;
        }
        const std::int32_t close_status =
            s_wheel_host.close_lease(s_wheel_host.host_context, probe, LEASE_PROBE_OWNER, generation_id);
        if (close_status != DMK_WHEELHOST_OK)
        {
            s_restart_required = true;
            logf("The loader could not close its wheel-host probe lease (status %d); restart required", close_status);
            return false;
        }
        return true;
    }

    /**
     * @brief Charges an image that could not be proved gone against the reload budget.
     * @details The image stays mapped for the process and its code can still be reachable, so the budget shrinks and
     *          the loader eventually stops rather than stacking unknown state.
     */
    void record_retained_generation(const Generation &generation) noexcept
    {
        ++s_retained_generations;
        s_retained_bytes += generation.image_bytes;
        logf(
            "Generation %llu could not be proved gone; %u of %u retained images, %llu of %llu bytes used",
            static_cast<unsigned long long>(generation.generation_id),
            s_retained_generations,
            MAX_RETAINED_GENERATIONS,
            static_cast<unsigned long long>(s_retained_bytes),
            static_cast<unsigned long long>(MAX_RETAINED_BYTES)
        );
        if (s_retained_generations >= MAX_RETAINED_GENERATIONS || s_retained_bytes >= MAX_RETAINED_BYTES)
        {
            s_restart_required = true;
            log_msg("The retained-generation budget is exhausted; restart the game to reload again");
        }
    }

    /**
     * @brief Refuses a reload whose worst case (the live image retained) would breach the retention budget.
     */
    [[nodiscard]] bool budget_allows_reload() noexcept
    {
        const std::uint64_t current_bytes = s_current.has_value() ? s_current->image_bytes : 0;
        if (s_retained_generations + 1 > MAX_RETAINED_GENERATIONS ||
            s_retained_bytes + current_bytes > MAX_RETAINED_BYTES)
        {
            s_restart_required = true;
            log_msg("The retained-generation budget is full; restart the game before another reload");
            return false;
        }
        return true;
    }

    /// Retires a generation: lease probe, FreeLibrary once, then proof of unmap.
    [[nodiscard]] bool release_generation(Generation &generation) noexcept
    {
        if (generation.module == nullptr)
        {
            return true;
        }
        // Only meaningful when a generation actually leased the host.
        if (s_wheel_host_live && !host_lease_is_closed(generation.generation_id))
        {
            return false;
        }

        // Shutdown removed every hook, so no NEW detour entry can occur. What it cannot drain is a game thread
        // already inside a per-frame detour body in this image. Those return in microseconds.
        Sleep(POST_SHUTDOWN_MS);

        const HMODULE module = generation.module;
        const void *const address = generation.unmap_address;
        generation.module = nullptr;
        generation.init = nullptr;
        generation.shutdown = nullptr;
        generation.revision = nullptr;

        if (FreeLibrary(module) == 0)
        {
            logf("FreeLibrary failed (error %lu); restart required", GetLastError());
            s_restart_required = true;
            return false;
        }
        if (!wait_for_unmap(address))
        {
            return false;
        }
        DeleteFileA(generation.path.c_str());
        return true;
    }

    /// Builds the request for generation @p generation_id, with the resident host when it is live.
    [[nodiscard]] HenrySensesReloadInitRequest make_request(std::uint64_t generation_id) noexcept
    {
        return HenrySensesReloadInitRequest{
            .struct_size = static_cast<std::uint32_t>(sizeof(HenrySensesReloadInitRequest)),
            .abi_version = HENRYSENSES_RELOAD_ABI_VERSION,
            .generation_id = generation_id,
            .expected_host_identity = s_wheel_host_live ? s_host_identity : 0,
            .wheel_host = s_wheel_host_live ? &s_wheel_host : nullptr,
            .persistent = &s_persistent,
        };
    }

    /**
     * @brief Stages the promoted build under the next unique name, maps it, and runs its Init.
     * @return False when no generation is live afterwards; a refused Init has already been released.
     */
    [[nodiscard]] bool load_generation()
    {
        Generation generation;
        ++s_generation_counter;
        generation.generation_id = s_generation_counter;
        generation.path = generation_path(s_generation_counter);

        if (!CopyFileA((s_loader_dir + s_logic_dll_name).c_str(), generation.path.c_str(), FALSE))
        {
            logf("Staging generation %04u failed (error %lu)", s_generation_counter, GetLastError());
            return false;
        }

        WIN32_FILE_ATTRIBUTE_DATA staged{};
        if (GetFileAttributesExA(generation.path.c_str(), GetFileExInfoStandard, &staged) != 0)
        {
            generation.image_bytes = (static_cast<std::uint64_t>(staged.nFileSizeHigh) << 32) |
                                     static_cast<std::uint64_t>(staged.nFileSizeLow);
        }

        generation.module = LoadLibraryA(generation.path.c_str());
        if (generation.module == nullptr)
        {
            logf("LoadLibrary failed (error %lu)", GetLastError());
            DeleteFileA(generation.path.c_str());
            return false;
        }
        generation.init = reinterpret_cast<InitFn>(
            reinterpret_cast<void *>(GetProcAddress(generation.module, HENRYSENSES_RELOAD_INIT_SYMBOL))
        );
        generation.shutdown = reinterpret_cast<ShutdownFn>(
            reinterpret_cast<void *>(GetProcAddress(generation.module, HENRYSENSES_RELOAD_SHUTDOWN_SYMBOL))
        );
        generation.revision = reinterpret_cast<RevisionFn>(
            reinterpret_cast<void *>(GetProcAddress(generation.module, HENRYSENSES_RELOAD_REVISION_SYMBOL))
        );
        generation.unmap_address = reinterpret_cast<const void *>(generation.init);

        if (generation.init == nullptr || generation.shutdown == nullptr || generation.revision == nullptr)
        {
            log_msg("The logic DLL is missing Init/Shutdown/Revision exports");
            if (!release_generation(generation))
            {
                record_retained_generation(generation);
            }
            return false;
        }

        logf(
            "Generation %04u starting (persistent state: %u of %u slots in use)",
            s_generation_counter,
            persistent_slots_in_use(),
            HENRYSENSES_PERSISTENT_SLOT_COUNT
        );
        const HenrySensesReloadInitRequest request = make_request(generation.generation_id);
        if (const unsigned init_status = generation.init(&request); init_status != HENRYSENSES_RELOAD_OK)
        {
            if (init_status == HENRYSENSES_RELOAD_INIT_UNSAFE)
            {
                // The rollback could not prove the image unreferenced and pinned it: code in it may still run.
                s_restart_required = true;
                log_msg("Init refused the load and could not prove its image unreferenced; restart required");
                // Keep the LoadLibrary reference even if the logic DLL could not acquire its own pin.
                record_retained_generation(generation);
                return false;
            }
            else
            {
                log_msg("Init refused the load");
            }
            if (!release_generation(generation))
            {
                record_retained_generation(generation);
            }
            return false;
        }

        // Two identities, because they answer different questions and can disagree. file_identity is what this
        // loader mapped, read from the image on disk. revision is the logic DLL's self-reported source version, and
        // its embedded build stamp only advances when mod_logic.cpp itself recompiles - a build that changed any
        // other file relinks the DLL and leaves that stamp behind. Trust the file identity to tell new bytes from a
        // replay. Read revision as the version string it is.
        const char *const revision = generation.revision();
        logf(
            "Generation %04u is live - %s [%s]",
            s_generation_counter,
            revision != nullptr ? revision : "unknown",
            file_identity(generation.path).c_str()
        );
        s_current.emplace(std::move(generation));
        return true;
    }

    /**
     * @brief Retires the live generation.
     * @return false when it must stay mapped. The caller must NOT load another over it.
     */
    [[nodiscard]] bool unload_current() noexcept
    {
        if (!s_current.has_value())
        {
            return true;
        }
        if (s_current->shutdown() == 0)
        {
            // Shutdown already ran its teardown before refusing, so the mod is inert but the image must stay mapped.
            // Re-entering Init on this handle restores it in place - the best available outcome, and it is still the
            // OLD code.
            log_msg("Shutdown refused the unload; the generation stays mapped");
            if (s_current->init != nullptr)
            {
                const HenrySensesReloadInitRequest request = make_request(s_current->generation_id);
                if (s_current->init(&request) == HENRYSENSES_RELOAD_OK)
                {
                    log_msg("Re-initialized the existing generation in place; still running the OLD code");
                }
            }
            return false;
        }
        logf(
            "Generation %llu shut down (persistent state: %u of %u slots in use)",
            static_cast<unsigned long long>(s_current->generation_id),
            persistent_slots_in_use(),
            HENRYSENSES_PERSISTENT_SLOT_COUNT
        );

        Generation retiring = std::move(*s_current);
        s_current.reset();
        if (!release_generation(retiring))
        {
            record_retained_generation(retiring);
        }
        return true;
    }

    /**
     * @brief Retires the live generation, promotes the staged build, and loads it.
     * @details The order is fixed: the outgoing image must be proved gone before another maps over it. A refused
     *          unload leaves the current generation live and returns without a load.
     */
    void reload_once()
    {
        if (s_restart_required)
        {
            log_msg("A previous reload could not be proved safe; restart the game");
            return;
        }
        logf("%s released - reloading logic DLL...", RELOAD_KEY_NAME);
        if (!budget_allows_reload())
        {
            return;
        }
        if (!unload_current())
        {
            return; // refused: the current generation stays live
        }
        promote_from_staging();
        if (!load_generation())
        {
            log_msg("Reload FAILED - no generation is live");
        }
    }

    /**
     * @brief Accepts the reload key only while this process owns the foreground window.
     * @details GetAsyncKeyState reads the global key state, so without this a Numpad 9 typed into another window
     *          (an IDE while the game sits minimized) would reload the game.
     */
    [[nodiscard]] bool foreground_belongs_to_this_process() noexcept
    {
        const HWND foreground = GetForegroundWindow();
        if (foreground == nullptr)
        {
            return false;
        }
        DWORD process_id = 0;
        (void)GetWindowThreadProcessId(foreground, &process_id);
        return process_id == GetCurrentProcessId();
    }

    /**
     * @brief Control thread: sets the loader paths up, starts the wheel host, then polls the reload key.
     * @details Runs the one-time setup in order (paths, logs, stale sweep, persistent state, wheel host, first
     *          generation) before the poll loop, so no generation maps before the resident host exists. The loader
     *          lives for the whole process, so the thread never exits and never tears the live generation down;
     *          process exit ends it.
     * @param param The loader module handle, which names the directory every path derives from.
     * @return Never returns.
     */
    DWORD WINAPI loader_thread(LPVOID param)
    {
        s_loader_dir = loader_dir(static_cast<HMODULE>(param));
        s_log_prefix = std::string{"["} + MOD_NAME + " Loader] ";
        s_logic_dll_name = std::string{MOD_NAME} + GENERATION_SUFFIX;
        s_logic_pdb_name = std::string{MOD_NAME} + ".logic.pdb";
        s_generation_prefix = std::string{MOD_NAME} + ".gen";

        const std::string loader_log = s_loader_dir + MOD_NAME + HenrySenses::dev::LOADER_LOG_SUFFIX;
        std::snprintf(s_log_path, sizeof(s_log_path), "%s", loader_log.c_str());
        (void)DeleteFileA(s_log_path); // one log per game run, holding every generation

        // Truncate the MOD log here too, exactly once, before any generation opens it. Each generation starts its
        // Session with LogOpenMode::Append, because the default Truncate makes a reload erase the outgoing
        // generation's teardown records - including the retention warnings that explain a retained image. Append
        // cannot tell "next generation" from "next game run", so owning the reset here gives one file per game run,
        // holding every generation within that run.
        {
            const std::string mod_log = s_loader_dir + MOD_NAME + ".log";
            (void)DeleteFileA(mod_log.c_str());
        }

        log_msg("Loader thread started");
        if (!s_loader_pinned)
        {
            log_msg("The loader could not pin itself; it must not be unloaded while the game runs");
        }
        sweep_stale_generations();

        s_persistent.struct_size = static_cast<std::uint32_t>(sizeof(HenrySensesPersistentState));
        s_persistent.abi_version = HENRYSENSES_RELOAD_ABI_VERSION;
        s_persistent.magic = HENRYSENSES_PERSISTENT_MAGIC;

        // The resident wheel host is started once, before the first generation, and never stopped. It owns the
        // permanent wheel keepalive so no logic generation has to. A host that refuses to start is not fatal: the
        // generation then uses the local message-hook backend and is charged as a retained image if the user binds a
        // wheel combo.
        const std::int32_t host_status = wheel_host_start(
            0,
            DMK_WHEELHOST_ABI_VERSION,
            static_cast<std::uint32_t>(sizeof(s_wheel_host)),
            &s_wheel_host
        );
        if (host_status == DMK_WHEELHOST_OK)
        {
            s_host_identity = s_wheel_host.host_identity;
            s_wheel_host_live = true;
            log_msg("Resident wheel host started (owns the wheel keepalive for the process)");
        }
        else
        {
            logf(
                "The resident wheel host failed to start (status %d); wheel bindings pin each generation",
                host_status
            );
        }

        promote_from_staging();
        if (!load_generation())
        {
            logf("Initial logic DLL load failed - press %s after rebuilding to retry", RELOAD_KEY_NAME);
        }

        // This thread is the only reload caller, so reload_once() cannot re-enter.
        bool was_key_down = false;
        for (;;)
        {
            Sleep(CONTROL_POLL_MS);
            const bool is_key_down = (GetAsyncKeyState(RELOAD_VK) & 0x8000) != 0;
            // Reload on the key-up edge so a held key cannot retrigger, and only for a press made in the game.
            if (was_key_down && !is_key_down && foreground_belongs_to_this_process())
            {
                reload_once();
            }
            was_key_down = is_key_down;
        }
    }
} // namespace

/**
 * @brief Loader entry point: pins the loader and starts the control thread in the game process.
 * @details The loader is process-lifetime. The pin keeps a FreeLibrary from unmapping it under its control thread,
 *          which may be inside a generation's Shutdown for many seconds, so detach has no work: at process exit every
 *          other thread is already gone.
 * @param module This module's handle, forwarded to the control thread as its path root.
 * @param reason The loader notification.
 * @return Always TRUE. A sibling executable stays inert instead of failing its load.
 */
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        if (!running_in_game_process())
        {
            return TRUE; // loaded into a sibling executable; stay inert
        }
        HMODULE pinned = nullptr;
        s_loader_pinned = GetModuleHandleExW(
                              GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                              reinterpret_cast<LPCWSTR>(&loader_thread),
                              &pinned
                          ) != 0;
        const HANDLE thread = CreateThread(nullptr, 0, loader_thread, module, 0, nullptr);
        if (thread != nullptr)
        {
            CloseHandle(thread);
        }
    }
    return TRUE;
}
