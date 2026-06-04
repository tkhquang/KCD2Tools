/**
 * @file dev/loader.cpp
 * @brief Thin loader ASI for the two-DLL hot-reload dev build.
 *
 * @details Only built when TPVCAMERA_DEV_BUILD is ON. The game loads this stub
 *          as an .asi; it loads the logic DLL, calls its exported Init(), and
 *          then polls Numpad 0 to unload and reload the logic in place. The build
 *          drops a freshly compiled logic DLL into a staging/ subdirectory; on a
 *          Numpad 0 release the loader runs the logic's Shutdown(), FreeLibrary's
 *          it (which frees the canonical file), copies the staged DLL over it, and
 *          reloads, all without restarting the game.
 *
 *          The staging hand-off is what lets a rebuild land while the game holds
 *          the previous logic DLL open: the build never writes the loaded file,
 *          only staging/. The loader links nothing but Win32 and logs through
 *          OutputDebugStringA, so it stays a thin stub (no DetourModKit, no
 *          file I/O, no C++ exceptions on the hot path).
 */

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
    constexpr const char *k_logic_dll_name = "KCD2_TPVCamera_Logic.dll";
    constexpr const char *k_logic_pdb_name = "KCD2_TPVCamera_Logic.pdb";
    constexpr const char *k_log_prefix = "[KCD2_TPVCamera Loader] ";
    constexpr const char *k_staging_subdir = "staging";
    constexpr int k_poll_interval_ms = 100;
    constexpr int k_post_shutdown_ms = 100; // quiescence so an in-flight detour body finishes before FreeLibrary
    constexpr int k_pre_load_delay_ms = 200;
    constexpr int k_vk_reload = VK_NUMPAD0;

    std::atomic<bool> s_running{false};
    std::atomic<bool> s_reloading{false};
    HANDLE s_thread = nullptr;
    HMODULE s_logic_dll = nullptr;

    using InitFn = bool(__cdecl *)();
    using ShutdownFn = void(__cdecl *)();

    InitFn s_fn_init = nullptr;
    ShutdownFn s_fn_shutdown = nullptr;

    void log_msg(const char *msg) noexcept
    {
        char buf[512];
        const int len = std::snprintf(buf, sizeof(buf), "%s%s\n", k_log_prefix, msg);
        if (len > 0 && static_cast<size_t>(len) < sizeof(buf))
        {
            OutputDebugStringA(buf);
        }
        else
        {
            // Keep the prefix on truncation so the line still routes to the
            // right DbgView filter.
            char fallback[128];
            std::snprintf(fallback, sizeof(fallback), "%s(message truncated)\n", k_log_prefix);
            OutputDebugStringA(fallback);
        }
    }

    std::string get_loader_dir(HMODULE self)
    {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(self, path, MAX_PATH);

        char *last_slash = std::strrchr(path, '\\');
        if (last_slash)
            *(last_slash + 1) = '\0';

        return std::string(path);
    }

    // Best-effort promotion of one staged file into the loader directory: skips silently when the source is
    // absent, and CopyFile/DeleteFile failures are intentionally non-fatal (the loader keeps the existing
    // copy). Used only by the dev hot-reload path, so a failed promotion just reloads the previous DLL.
    void move_staged_file(const std::string &staging_dir, const std::string &loader_dir, const char *filename)
    {
        const std::string src = staging_dir + filename;
        if (GetFileAttributesA(src.c_str()) == INVALID_FILE_ATTRIBUTES)
            return;

        CopyFileA(src.c_str(), (loader_dir + filename).c_str(), FALSE);
        DeleteFileA(src.c_str());
    }

    // Promotes a freshly staged logic DLL (and its PDB) into the loader directory.
    [[nodiscard]] bool copy_from_staging(const std::string &loader_dir)
    {
        const std::string staging_dir = loader_dir + k_staging_subdir + "\\";
        const std::string staged_dll = staging_dir + k_logic_dll_name;

        if (GetFileAttributesA(staged_dll.c_str()) == INVALID_FILE_ATTRIBUTES)
            return false;

        if (!CopyFileA(staged_dll.c_str(), (loader_dir + k_logic_dll_name).c_str(), FALSE))
        {
            log_msg("Failed to copy logic DLL from staging");
            return false;
        }
        DeleteFileA(staged_dll.c_str());

        move_staged_file(staging_dir, loader_dir, k_logic_pdb_name);

        log_msg("Promoted staged logic DLL");
        return true;
    }

    [[nodiscard]] bool load_logic(const std::string &dll_path)
    {
        s_logic_dll = LoadLibraryA(dll_path.c_str());
        if (!s_logic_dll)
        {
            char err[128];
            std::snprintf(err, sizeof(err), "LoadLibrary failed (error %lu)", GetLastError());
            log_msg(err);
            return false;
        }

        s_fn_init = reinterpret_cast<InitFn>(GetProcAddress(s_logic_dll, "Init"));
        s_fn_shutdown = reinterpret_cast<ShutdownFn>(GetProcAddress(s_logic_dll, "Shutdown"));
        if (!s_fn_init || !s_fn_shutdown)
        {
            log_msg("Logic DLL missing Init/Shutdown exports");
            FreeLibrary(s_logic_dll);
            s_logic_dll = nullptr;
            s_fn_init = nullptr;
            s_fn_shutdown = nullptr;
            return false;
        }

        if (!s_fn_init())
        {
            log_msg("Logic DLL Init() returned false");
            FreeLibrary(s_logic_dll);
            s_logic_dll = nullptr;
            s_fn_init = nullptr;
            s_fn_shutdown = nullptr;
            return false;
        }

        log_msg("Logic DLL loaded and initialized");
        return true;
    }

    // Runs the logic DLL's Shutdown() (removing every hook and tearing down
    // DetourModKit) without freeing the module. Safe to call from the loader
    // thread's terminal exit path, where FreeLibrary must not run: that path is
    // joined by DllMain(DLL_PROCESS_DETACH) while the OS loader lock is held, and
    // FreeLibrary would need that same lock, deadlocking until the join times out.
    void request_logic_shutdown() noexcept
    {
        if (s_fn_shutdown)
            s_fn_shutdown();
    }

    // Full unload for the interactive reload path only (runs on the loader thread,
    // not under the loader lock): shut the logic down, let any in-flight detour
    // body finish, then free the module so the staged rebuild can replace it.
    void unload_logic() noexcept
    {
        if (!s_logic_dll)
            return;

        request_logic_shutdown();

        // Shutdown() removed every hook, so no new detour entry can occur. A game
        // thread already inside a per-frame detour body (which lives in this DLL's
        // .text) still has to finish before the pages are unmapped. Per-frame
        // detours return in microseconds, so this short quiescence keeps the
        // FreeLibrary off any in-flight body.
        Sleep(k_post_shutdown_ms);

        FreeLibrary(s_logic_dll);
        s_logic_dll = nullptr;
        s_fn_init = nullptr;
        s_fn_shutdown = nullptr;
        log_msg("Logic DLL unloaded");
    }

    DWORD WINAPI loader_thread(LPVOID param)
    {
        const HMODULE self = static_cast<HMODULE>(param);
        const std::string loader_dir = get_loader_dir(self);
        const std::string dll_path = loader_dir + k_logic_dll_name;

        log_msg("Loader thread started");

        (void)copy_from_staging(loader_dir);
        if (!load_logic(dll_path))
            log_msg("Initial logic DLL load failed -- waiting for Numpad 0 to retry");

        bool was_key_down = false;
        while (s_running.load(std::memory_order_relaxed))
        {
            Sleep(k_poll_interval_ms);

            const bool is_key_down = (GetAsyncKeyState(k_vk_reload) & 0x8000) != 0;

            // Reload on the key-up edge so a held key does not retrigger.
            if (was_key_down && !is_key_down)
            {
                if (!s_reloading.exchange(true, std::memory_order_acq_rel))
                {
                    log_msg("Numpad 0 released -- reloading logic DLL...");

                    unload_logic();
                    Sleep(k_pre_load_delay_ms);
                    (void)copy_from_staging(loader_dir);

                    if (!load_logic(dll_path))
                        log_msg("Reload FAILED -- logic DLL not loaded");

                    s_reloading.store(false, std::memory_order_release);
                }
            }

            was_key_down = is_key_down;
        }

        // Terminal path: the thread is exiting because s_running was cleared in
        // DLL_PROCESS_DETACH. Only remove the logic's hooks here; do NOT
        // FreeLibrary. On an explicit loader unload this path is joined by DllMain
        // under the OS loader lock, and FreeLibrary would need that same lock,
        // deadlocking until the join times out. Leaking the logic module is the
        // accepted trade-off (the process is tearing the loader down anyway).
        request_logic_shutdown();
        log_msg("Loader thread exiting");
        return 0;
    }
} // namespace

BOOL APIENTRY DllMain(HMODULE h_module, DWORD ul_reason_for_call, LPVOID lp_reserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(h_module);

        s_running.store(true, std::memory_order_release);
        s_thread = CreateThread(nullptr, 0, loader_thread, h_module, 0, nullptr);
        if (!s_thread)
        {
            s_running.store(false, std::memory_order_release);
            return FALSE;
        }
        break;

    case DLL_PROCESS_DETACH:
        s_running.store(false, std::memory_order_release);

        // Only join on an explicit FreeLibrary (lp_reserved == nullptr); on process
        // exit the OS has already stopped the other threads and joining can hang.
        if (s_thread && lp_reserved == nullptr)
        {
            // Only close the handle once the worker has actually exited. The timeout comfortably
            // exceeds the worst-case reload window (validate_game_module polls ~3s plus the pre-load
            // and quiescence sleeps). On a WAIT_TIMEOUT the thread is still running inside this DLL's
            // .text (a reload can be mid-flight in the logic DLL's Init()); closing its handle and
            // letting the unmap proceed would race a live thread, so leak the handle instead, mirroring
            // the deliberate leak on the process-exit branch.
            if (WaitForSingleObject(s_thread, 8000) == WAIT_OBJECT_0)
            {
                CloseHandle(s_thread);
            }
            s_thread = nullptr;
        }
        break;

    default:
        break;
    }

    return TRUE;
}
