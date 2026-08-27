/**
 * @file mod_loader.cpp
 * @brief Resident dev loader: owns the process, reloads one logic generation at a time.
 *
 * @details Structure follows DetourModKit's checked-in `examples/staged_reload` pair, which its
 *          hot-reload guide treats as the reference implementation. Three properties matter and each
 *          exists for a failure that was actually observed here:
 *
 *          1. **Unique staged names.** Mapping the build output locks that path, so a rebuild cannot
 *             replace it and every reload replays identical bytes while reporting success. Mapping a
 *             REUSED name is worse: once an image is pinned, LoadLibrary on the same path silently
 *             returns the pinned predecessor. Each generation gets a name never used before in this
 *             process.
 *          2. **A resident wheel host.** A mouse-wheel binding makes the input engine take a permanent
 *             module keepalive on whichever module hosts wheel capture. Hosting it here - in a module
 *             that is never unloaded - is what lets the logic DLL keep `WheelUp` bindings AND still
 *             unmap. This loader therefore links only DetourModKit::WheelHost, never the full archive.
 *          3. **Proof, not assumption.** A generation is only considered gone after its typed Shutdown
 *             accepts, a probe lease opens and closes, and one of its code addresses is observed to
 *             become unmapped. Anything less has been mistaken for a successful reload before.
 *
 *          This loader is dev-only. The release build is a single ASI with no reload path at all.
 */

#include "protocol.h"

#include <DetourModKit/abi/wheel_host.h>

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

namespace
{
    constexpr const char *k_logic_dll_name = "KCD2_TPVCamera.logic.dll";
    constexpr const char *k_logic_pdb_name = "KCD2_TPVCamera.logic.pdb";
    /// Staged generations are "<mod>.genNNNN.logic.dll", so ".logic.dll" stays a stable suffix and one
    /// glob covers the build output and every staged copy. Matches the reference pair's scheme.
    constexpr const char *k_generation_prefix = "KCD2_TPVCamera.gen";
    constexpr const char *k_generation_suffix = ".logic.dll";
    constexpr const char *k_loader_log_name = "KCD2_TPVCamera_Loader.log";
    /// The mod's own log. The loader truncates it once per run; see the note in loader_thread.
    constexpr const char *k_mod_log_name = "KCD2_TPVCamera.log";
    constexpr const char *k_log_prefix = "[KCD2_TPVCamera Loader] ";
    constexpr const char *k_staging_subdir = "staging";

    constexpr int k_reload_vk = VK_NUMPAD0;
    constexpr DWORD k_control_poll_ms = 100;
    /// Quiescence so an in-flight per-frame detour body returns before FreeLibrary.
    constexpr DWORD k_post_shutdown_ms = 100;
    constexpr DWORD k_unmap_poll_ms = 10;
    /// A release can complete slightly after FreeLibrary returns, so the unmap check polls rather than
    /// sampling once. A single sample reports a healthy generation as pinned.
    constexpr DWORD k_unmap_timeout_ms = 2000;

    /// Caps retained images before the loader stops reloading and asks for a restart.
    constexpr unsigned k_max_retained_generations = 24;

    /**
     * @brief Whether a generation drives wheel capture through the loader's resident host.
     * @details FALSE for now, and the reason is a measured DMK v4.1 defect, not a preference.
     *
     *          With the resident host, wheel capture works in the FIRST generation and is dead in every
     *          generation after a reload. Measured in the dead generation: route reports Ready (mounted,
     *          target thread alive, capture armable), the binding resolves, the INI parses WheelUp, there
     *          are zero warnings - and the notches pass through to the GAME untouched, so the host is not
     *          capturing them for the new lease. The poller cannot self-correct either: it only retargets
     *          when the route is not Ready or the thread changed, and here it is Ready on the right
     *          thread, so capture is never re-armed for the new lease owner.
     *
     *          Falling back to the local MessageHook backend costs one permanent keepalive per generation
     *          (ModulePinReason::MessageHookKeepalive), so each retired image is retained and charged
     *          against the reload budget below. That is the better trade for a dev loop: every feature
     *          works, and the budget still allows a long session before a restart.
     *
     *          Flip to true once the handover is fixed upstream; the whole plumbing stays in place.
     */
    constexpr bool k_use_resident_wheel_host = true;

    /// Loader-owned owner id for the probe lease: ASCII "TPVPROBE". Any value a generation never uses.
    constexpr std::uint64_t k_lease_probe_owner = UINT64_C(0x54505650524F4245);

    std::atomic<bool> s_running{false};
    std::atomic<bool> s_reloading{false};
    HANDLE s_thread = nullptr;

    /// Process-lifetime wheel host. Started once, never stopped: the loader outlives every generation.
    WheelHostTable s_wheel_host{};
    std::uint64_t s_host_identity = 0;

    /// Latched when a generation could not be proved gone. Further reloads would stack unknown state.
    bool s_restart_required = false;
    unsigned s_generation_counter = 0;
    unsigned s_retained_generations = 0;

    using InitFn = unsigned(__cdecl *)(const TpvReloadInitRequest *) noexcept;
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
    };

    std::optional<Generation> s_current;
    char s_log_path[MAX_PATH]{};
    std::string s_loader_dir;

    /* ---- logging ------------------------------------------------------------------------------ */

    void log_msg(const char *msg) noexcept
    {
        char line[768];
        const int len = std::snprintf(line, sizeof(line), "%s%s\n", k_log_prefix, msg);
        if (len <= 0)
        {
            return;
        }
        OutputDebugStringA(line);

        // The loader keeps its OWN log. The mod's log belongs to a Session that dies with each
        // generation, and the loader's most important lines are emitted while no Session exists at all.
        if (s_log_path[0] == '\0')
        {
            return;
        }
        const HANDLE file = CreateFileA(s_log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return;
        }
        SYSTEMTIME now{};
        GetLocalTime(&now);
        char stamped[832];
        const int stamped_len = std::snprintf(stamped, sizeof(stamped), "[%02u:%02u:%02u.%03u] %s", now.wHour,
                                              now.wMinute, now.wSecond, now.wMilliseconds, line);
        if (stamped_len > 0)
        {
            DWORD written = 0;
            (void)WriteFile(file, stamped, static_cast<DWORD>(stamped_len), &written, nullptr);
        }
        CloseHandle(file);
    }

    template <class... Args> void logf(const char *fmt, Args... args) noexcept
    {
        char buffer[768];
        if (std::snprintf(buffer, sizeof(buffer), fmt, args...) > 0)
        {
            log_msg(buffer);
        }
    }

    /* ---- paths -------------------------------------------------------------------------------- */

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

    std::string generation_path(unsigned generation)
    {
        char name[128];
        std::snprintf(name, sizeof(name), "%s%04u%s", k_generation_prefix, generation, k_generation_suffix);
        return s_loader_dir + name;
    }

    void move_staged_file(const std::string &staging_dir, const char *filename)
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
        const std::string staging_dir = s_loader_dir + k_staging_subdir + "\\";
        const std::string staged_dll = staging_dir + k_logic_dll_name;
        if (GetFileAttributesA(staged_dll.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            return; // nothing new was built
        }
        if (!CopyFileA(staged_dll.c_str(), (s_loader_dir + k_logic_dll_name).c_str(), FALSE))
        {
            log_msg("Failed to promote the staged logic DLL");
            return;
        }
        DeleteFileA(staged_dll.c_str());
        move_staged_file(staging_dir, k_logic_pdb_name);
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
        const std::string pattern = s_loader_dir + k_generation_prefix + "*" + k_generation_suffix;
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

    /* ---- release proofs ------------------------------------------------------------------------ */

    /**
     * @brief Waits for an address inside the retired image to stop belonging to any loaded module.
     * @details Address-based rather than name-based: it asks the loader the exact question that matters,
     *          and UNCHANGED_REFCOUNT keeps the probe from perturbing the count it measures. Probing a
     *          freed address is safe - the call simply fails, which IS the answer.
     */
    [[nodiscard]] bool wait_for_unmap(const void *address) noexcept
    {
        if (address == nullptr)
        {
            return false; // no probe address means no proof
        }
        for (DWORD waited = 0; waited < k_unmap_timeout_ms; waited += k_unmap_poll_ms)
        {
            HMODULE owner = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(address), &owner) == 0)
            {
                return true;
            }
            Sleep(k_unmap_poll_ms);
        }
        return false;
    }

    /**
     * @brief Confirms the retired generation left no lease open on the resident wheel host.
     * @details The host allows one lease at a time, so a successful open proves the generation closed
     *          its own. A failed close leaves host state unknown, which is as serious as a failed unmap.
     */
    [[nodiscard]] bool host_lease_is_closed(std::uint64_t generation_id) noexcept
    {
        WheelHostLease probe = 0;
        const int32_t open_status =
            s_wheel_host.open_lease(s_wheel_host.host_context, k_lease_probe_owner, generation_id, &probe);
        if (open_status != DMK_WHEELHOST_OK)
        {
            logf("Generation %llu left its wheel-host lease open (status %d)",
                 static_cast<unsigned long long>(generation_id), open_status);
            return false;
        }
        const int32_t close_status =
            s_wheel_host.close_lease(s_wheel_host.host_context, probe, k_lease_probe_owner, generation_id);
        if (close_status != DMK_WHEELHOST_OK)
        {
            s_restart_required = true;
            logf("The loader could not close its wheel-host probe lease (status %d); restart required",
                 close_status);
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
        if (k_use_resident_wheel_host && !host_lease_is_closed(generation.generation_id))
        {
            return false;
        }

        // Shutdown removed every hook, so no NEW detour entry can occur. What it cannot drain is a game
        // thread already inside a per-frame detour body in this image. Those return in microseconds.
        Sleep(k_post_shutdown_ms);

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

    /* ---- generation lifecycle ------------------------------------------------------------------- */

    [[nodiscard]] bool load_generation()
    {
        Generation generation;
        ++s_generation_counter;
        generation.generation_id = s_generation_counter;
        generation.path = generation_path(s_generation_counter);

        if (!CopyFileA((s_loader_dir + k_logic_dll_name).c_str(), generation.path.c_str(), FALSE))
        {
            logf("Staging generation %04u failed (error %lu)", s_generation_counter, GetLastError());
            return false;
        }

        generation.module = LoadLibraryA(generation.path.c_str());
        if (generation.module == nullptr)
        {
            logf("LoadLibrary failed (error %lu)", GetLastError());
            DeleteFileA(generation.path.c_str());
            return false;
        }
        generation.init = reinterpret_cast<InitFn>(
            reinterpret_cast<void *>(GetProcAddress(generation.module, TPVCAMERA_RELOAD_INIT_SYMBOL)));
        generation.shutdown = reinterpret_cast<ShutdownFn>(
            reinterpret_cast<void *>(GetProcAddress(generation.module, TPVCAMERA_RELOAD_SHUTDOWN_SYMBOL)));
        generation.revision = reinterpret_cast<RevisionFn>(
            reinterpret_cast<void *>(GetProcAddress(generation.module, TPVCAMERA_RELOAD_REVISION_SYMBOL)));
        generation.unmap_address = reinterpret_cast<const void *>(generation.init);

        if (generation.init == nullptr || generation.shutdown == nullptr || generation.revision == nullptr)
        {
            log_msg("The logic DLL is missing Init/Shutdown/Revision exports");
            (void)release_generation(generation);
            return false;
        }

        const TpvReloadInitRequest request{
            .struct_size = static_cast<std::uint32_t>(sizeof(TpvReloadInitRequest)),
            .abi_version = TPVCAMERA_RELOAD_ABI_VERSION,
            .generation_id = generation.generation_id,
            .expected_host_identity = k_use_resident_wheel_host ? s_host_identity : 0,
            .wheel_host = k_use_resident_wheel_host ? &s_wheel_host : nullptr,
        };
        if (generation.init(&request) != TPVCAMERA_RELOAD_OK)
        {
            log_msg("Init refused the load");
            (void)release_generation(generation);
            return false;
        }

        const char *const revision = generation.revision();
        logf("Generation %04u is live -- %s", s_generation_counter, revision != nullptr ? revision : "unknown");
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
            // Shutdown already ran its teardown before refusing, so the mod is inert but the image must
            // stay mapped. Re-entering Init on this handle restores it in place - the best available
            // outcome, and it is still the OLD code.
            log_msg("Shutdown refused the unload; the generation stays mapped");
            if (s_current->init != nullptr)
            {
                const TpvReloadInitRequest request{
                    .struct_size = static_cast<std::uint32_t>(sizeof(TpvReloadInitRequest)),
                    .abi_version = TPVCAMERA_RELOAD_ABI_VERSION,
                    .generation_id = s_current->generation_id,
                    .expected_host_identity = k_use_resident_wheel_host ? s_host_identity : 0,
                    .wheel_host = k_use_resident_wheel_host ? &s_wheel_host : nullptr,
                };
                if (s_current->init(&request) == TPVCAMERA_RELOAD_OK)
                {
                    log_msg("Re-initialized the existing generation in place; still running the OLD code");
                }
            }
            return false;
        }

        Generation retiring = std::move(*s_current);
        s_current.reset();
        if (!release_generation(retiring))
        {
            // The image stays mapped for the process. Its code can still be reachable, so the reload
            // budget shrinks and the loader eventually stops rather than stacking unknown state.
            ++s_retained_generations;
            logf("Generation %llu could not be proved gone; %u of %u retained images used",
                 static_cast<unsigned long long>(retiring.generation_id), s_retained_generations,
                 k_max_retained_generations);
            if (s_retained_generations >= k_max_retained_generations)
            {
                s_restart_required = true;
                log_msg("The retained-generation budget is exhausted; restart the game to reload again");
            }
        }
        return true;
    }

    void reload_once()
    {
        if (s_restart_required)
        {
            log_msg("A previous reload could not be proved safe; restart the game");
            return;
        }
        log_msg("Numpad 0 released -- reloading logic DLL...");
        if (!unload_current())
        {
            return; // refused: the current generation stays live
        }
        promote_from_staging();
        if (!load_generation())
        {
            log_msg("Reload FAILED -- no generation is live");
        }
    }

    DWORD WINAPI loader_thread(LPVOID param)
    {
        s_loader_dir = loader_dir(static_cast<HMODULE>(param));
        std::snprintf(s_log_path, sizeof(s_log_path), "%s%s", s_loader_dir.c_str(), k_loader_log_name);
        (void)DeleteFileA(s_log_path); // one log per game run, holding every generation

        // Truncate the MOD log here too, exactly once, before any generation opens it.
        //
        // Each generation starts its Session with LogOpenMode::Append, because the default Truncate makes
        // a reload erase the outgoing generation's teardown records - including the XInput retention
        // warning, which is the only line that explains a retained image. But Append cannot tell "next
        // generation" from "next game run", so nothing truncated the file at all and it accumulated
        // across restarts (measured: 11 sessions, 4.9 MB). Owning the reset here restores the intended
        // meaning: one file per game run, holding every generation within that run.
        {
            const std::string mod_log = s_loader_dir + k_mod_log_name;
            (void)DeleteFileA(mod_log.c_str());
        }

        log_msg("Loader thread started");
        sweep_stale_generations();

        // The resident wheel host is started once, before the first generation, and never stopped. It owns
        // the permanent wheel keepalive so no logic generation has to.
        const int32_t host_status = wheel_host_start(0, DMK_WHEELHOST_ABI_VERSION,
                                                     static_cast<std::uint32_t>(sizeof(s_wheel_host)), &s_wheel_host);
        if (host_status != DMK_WHEELHOST_OK)
        {
            logf("The resident wheel host failed to start (status %d); reload is unavailable", host_status);
            return 0;
        }
        s_host_identity = s_wheel_host.host_identity;
        log_msg("Resident wheel host started (owns the wheel keepalive for the process)");

        promote_from_staging();
        if (!load_generation())
        {
            log_msg("Initial logic DLL load failed -- press Numpad 0 after rebuilding to retry");
        }

        bool was_key_down = false;
        while (s_running.load(std::memory_order_relaxed))
        {
            Sleep(k_control_poll_ms);
            const bool is_key_down = (GetAsyncKeyState(k_reload_vk) & 0x8000) != 0;
            if (was_key_down && !is_key_down) // reload on the key-up edge so a held key cannot retrigger
            {
                if (!s_reloading.exchange(true, std::memory_order_acq_rel))
                {
                    reload_once();
                    s_reloading.store(false, std::memory_order_release);
                }
            }
            was_key_down = is_key_down;
        }

        // Terminal path: the thread is exiting because s_running was cleared in DLL_PROCESS_DETACH. Only
        // run the logic's Shutdown here; do NOT FreeLibrary. That path is joined by DllMain under the OS
        // loader lock, which FreeLibrary also needs, so it would deadlock until the join times out.
        if (s_current.has_value() && s_current->shutdown != nullptr)
        {
            (void)s_current->shutdown();
        }
        return 0;
    }
} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        s_running.store(true, std::memory_order_relaxed);
        s_thread = CreateThread(nullptr, 0, loader_thread, module, 0, nullptr);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        s_running.store(false, std::memory_order_relaxed);
        if (s_thread != nullptr)
        {
            WaitForSingleObject(s_thread, 2000);
            CloseHandle(s_thread);
            s_thread = nullptr;
        }
    }
    return TRUE;
}
