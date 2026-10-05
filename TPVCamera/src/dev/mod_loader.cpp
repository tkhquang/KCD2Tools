/**
 * @file mod_loader.cpp
 * @brief Resident dev loader: owns the process and replaces one logic generation at a time.
 *
 * @details The loader follows DetourModKit's staged-generation reload pattern (its hot-reload guide and the
 *          examples/staged_reload pair). It links only DetourModKit::WheelHost, never the full archive, and it is
 *          never unloaded:
 *          - The build deploys KCD2_TPVCamera.logic.dll (and its PDB) to staging\. A reload promotes them beside the
 *            loader, then maps a unique copy (KCD2_TPVCamera.genNNNN.logic.dll). The build output stays unlocked, and
 *            a reused name can never return a retained predecessor with its statics intact.
 *          - The resident wheel host owns the wheel capture and its permanent keepalive, so a generation can keep
 *            its WheelUp bindings and still unmap.
 *          - A generation retires only on a typed Shutdown() verdict, a closed probe lease, and either an observed
 *            unmap or a charge against the retained-generation budget. A refused or failed retirement keeps the
 *            image mapped. The loader never initializes a retired or refused image again.
 */

#include "protocol.h"

#include <DetourModKit/abi/wheel_host.h>

#include <windows.h>

#include <process.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace
{
    using InitFn = std::uint32_t(DMK_WHEELHOST_CALL *)(const TpvReloadInitRequest *) noexcept;
    using ShutdownFn = std::uint32_t(DMK_WHEELHOST_CALL *)() noexcept;
    using RevisionFn = const char *(DMK_WHEELHOST_CALL *)() noexcept;

    constexpr std::wstring_view LOGIC_DLL_NAME = L"KCD2_TPVCamera.logic.dll";
    constexpr std::wstring_view LOGIC_PDB_NAME = L"KCD2_TPVCamera.logic.pdb";
    /// Staged copies are "KCD2_TPVCamera.genNNNN.logic.dll", so one prefix and suffix cover every one of them.
    constexpr std::wstring_view STAGED_PREFIX = L"KCD2_TPVCamera.gen";
    constexpr std::wstring_view STAGED_SUFFIX = L".logic.dll";
    constexpr std::wstring_view STAGING_DIRECTORY = L"staging";
    constexpr std::wstring_view LOADER_LOG_NAME = L"KCD2_TPVCamera_Loader.log";
    constexpr std::wstring_view MOD_LOG_NAME = L"KCD2_TPVCamera.log";

    /// The reload key. A reload starts on its release, so a held key cannot retrigger it.
    constexpr int RELOAD_VK = VK_NUMPAD0;
    constexpr SHORT KEY_DOWN_MASK = static_cast<SHORT>(0x8000);
    constexpr DWORD CONTROL_POLL_MS = 50;
    constexpr DWORD UNMAP_POLL_MS = 10;
    /// A release can complete slightly after FreeLibrary returns, so the unmap probe polls until this deadline.
    constexpr DWORD UNMAP_TIMEOUT_MS = 2000;
    constexpr std::size_t MODULE_PATH_INITIAL_CHARS = 512;
    constexpr std::size_t MODULE_PATH_MAX_CHARS = 32'768;

    /// Caps retained generations, by count and by staged-file bytes, before the loader requests a restart.
    constexpr std::size_t MAX_RETAINED_GENERATIONS = 32;
    constexpr std::uintmax_t MAX_RETAINED_BYTES = 128ull * 1024 * 1024;

    /// Loader-owned owner id for the probe lease: ASCII "TPVPROBE", a value no generation uses.
    constexpr std::uint64_t LEASE_PROBE_OWNER = UINT64_C(0x54505650524F4245);

    /// One loaded staged copy and its exports.
    struct Generation
    {
        std::filesystem::path path;
        HMODULE module = nullptr;
        InitFn init = nullptr;
        ShutdownFn shutdown = nullptr;
        RevisionFn revision = nullptr;
        /// An exported code address inside the image, probed after FreeLibrary to prove the unmap.
        const void *unmap_address = nullptr;
        std::uint64_t generation_id = 0;
        /// The staged file size, charged against the retention byte budget when the image stays mapped.
        std::uintmax_t image_bytes = 0;
    };

    HMODULE s_loader_module = nullptr;
    std::filesystem::path s_directory;
    WheelHostTable s_wheel_host{};
    // The host identity captured once at start. The request carries this copy, so the logic-side identity check
    // compares against the start-time value instead of the field it validates.
    std::uint64_t s_host_identity = 0;
    std::optional<Generation> s_current;
    unsigned s_generation_counter = 0;
    std::size_t s_retained_count = 0;
    std::uintmax_t s_retained_bytes = 0;
    bool s_restart_required = false;
    // The loader's own reference of each retained image, held until process exit. A leak counter does not prove that
    // the leaked resource holds a module pin, so the loader keeps a reference of its own.
    std::array<HMODULE, MAX_RETAINED_GENERATIONS> s_retained_loader_refs{};

    static_assert(std::is_nothrow_move_constructible_v<Generation>);

    [[nodiscard]] std::optional<std::filesystem::path> loader_directory()
    {
        std::wstring buffer(MODULE_PATH_INITIAL_CHARS, L'\0');
        for (;;)
        {
            const DWORD capacity = static_cast<DWORD>(buffer.size());
            const DWORD length = GetModuleFileNameW(s_loader_module, buffer.data(), capacity);
            if (length == 0)
            {
                return std::nullopt;
            }
            if (length < capacity)
            {
                buffer.resize(length);
                return std::filesystem::path{buffer}.parent_path();
            }
            if (buffer.size() >= MODULE_PATH_MAX_CHARS)
            {
                return std::nullopt;
            }
            const std::size_t next_size = buffer.size() * 2;
            buffer.resize(next_size > MODULE_PATH_MAX_CHARS ? MODULE_PATH_MAX_CHARS : next_size);
        }
    }

    /// Appends one line to the loader-owned log, which survives every generation.
    void append_log(std::string_view line) noexcept
    {
        try
        {
            SYSTEMTIME now{};
            GetLocalTime(&now);
            const std::string stamped = std::format("[{:02}:{:02}:{:02}.{:03}] [KCD2_TPVCamera Loader] {}\n", now.wHour,
                                                    now.wMinute, now.wSecond, now.wMilliseconds, line);
            OutputDebugStringA(stamped.c_str());
            if (s_directory.empty())
            {
                return;
            }
            const std::filesystem::path log_path = s_directory / LOADER_LOG_NAME;
            const HANDLE file = CreateFileW(log_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                return;
            }
            DWORD written = 0;
            (void)WriteFile(file, stamped.data(), static_cast<DWORD>(stamped.size()), &written, nullptr);
            CloseHandle(file);
        }
        catch (...)
        {
            // The log is best-effort and never terminates the loader.
        }
    }

    template <typename... Args> void append_formatted_log(std::format_string<Args...> text, Args &&...args) noexcept
    {
        try
        {
            append_log(std::format(text, std::forward<Args>(args)...));
        }
        catch (...)
        {
        }
    }

    void remove_file(const std::filesystem::path &path) noexcept
    {
        std::error_code error;
        (void)std::filesystem::remove(path, error);
    }

    /// Deletes staged copies from earlier sessions. A copy that still backs a mapped image stays locked and survives.
    void remove_stale_staged_files() noexcept
    {
        try
        {
            std::size_t removed = 0;
            std::error_code error;
            for (const std::filesystem::directory_entry &entry :
                 std::filesystem::directory_iterator(s_directory, error))
            {
                const std::wstring name = entry.path().filename().wstring();
                std::error_code remove_error;
                if (name.starts_with(STAGED_PREFIX) && name.ends_with(STAGED_SUFFIX) &&
                    std::filesystem::remove(entry.path(), remove_error))
                {
                    ++removed;
                }
            }
            if (removed > 0)
            {
                append_formatted_log("Removed {} stale staged copies", removed);
            }
        }
        catch (...)
        {
        }
    }

    /// Moves a freshly built logic DLL and its PDB from staging\ beside the loader. A missing build changes nothing.
    void promote_from_staging() noexcept
    {
        const std::filesystem::path staging = s_directory / STAGING_DIRECTORY;
        std::error_code error;
        if (!std::filesystem::exists(staging / LOGIC_DLL_NAME, error))
        {
            return;
        }
        for (const std::wstring_view name : {LOGIC_DLL_NAME, LOGIC_PDB_NAME})
        {
            std::error_code copy_error;
            if (std::filesystem::copy_file(staging / name, s_directory / name,
                                           std::filesystem::copy_options::overwrite_existing, copy_error))
            {
                remove_file(staging / name);
            }
            else if (name == LOGIC_DLL_NAME)
            {
                append_formatted_log("Promoting the staged logic DLL failed: {}", copy_error.message());
                return;
            }
        }
        append_log("Promoted the staged logic DLL");
    }

    void record_retained_generation(const Generation &generation, HMODULE loader_reference = nullptr) noexcept
    {
        if (s_retained_count < s_retained_loader_refs.size())
        {
            s_retained_loader_refs[s_retained_count] = loader_reference;
        }
        if (s_retained_count < (std::numeric_limits<std::size_t>::max)())
        {
            ++s_retained_count;
        }
        s_retained_bytes = generation.image_bytes > (std::numeric_limits<std::uintmax_t>::max)() - s_retained_bytes
                               ? (std::numeric_limits<std::uintmax_t>::max)()
                               : s_retained_bytes + generation.image_bytes;
        append_formatted_log("Generation {} retained ({} of {} images, {} of {} bytes)", generation.generation_id,
                             s_retained_count, MAX_RETAINED_GENERATIONS, s_retained_bytes, MAX_RETAINED_BYTES);
    }

    /**
     * @brief Waits until no loaded module owns an address of the retired image.
     * @details UNCHANGED_REFCOUNT leaves the reference count as it is, and a probe of a released address fails,
     *          which is the answer. Never pass the returned handle to FreeLibrary.
     * @return True only when the address becomes unmapped before the deadline.
     */
    [[nodiscard]] bool wait_for_unmap(const void *address) noexcept
    {
        if (address == nullptr)
        {
            return false;
        }
        for (DWORD waited = 0; waited < UNMAP_TIMEOUT_MS; waited += UNMAP_POLL_MS)
        {
            HMODULE owner = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(address), &owner) == 0)
            {
                return true;
            }
            Sleep(UNMAP_POLL_MS);
        }
        return false;
    }

    /**
     * @brief Opens and closes a probe lease after a generation's Shutdown().
     * @details The host allows one lease at a time, so an open proves the generation closed its own. A failed
     *          close leaves the host state unknown, which requires a restart.
     * @return True only when the generation left no lease open.
     */
    [[nodiscard]] bool host_lease_is_closed(std::uint64_t generation_id) noexcept
    {
        WheelHostLease probe = 0;
        const int32_t open_status =
            s_wheel_host.open_lease(s_wheel_host.host_context, LEASE_PROBE_OWNER, generation_id, &probe);
        if (open_status != DMK_WHEELHOST_OK)
        {
            append_formatted_log("Generation {} left its wheel-host lease open (status {})", generation_id,
                                 open_status);
            return false;
        }
        const int32_t close_status =
            s_wheel_host.close_lease(s_wheel_host.host_context, probe, LEASE_PROBE_OWNER, generation_id);
        if (close_status != DMK_WHEELHOST_OK)
        {
            s_restart_required = true;
            append_formatted_log("The loader could not close its wheel-host probe lease (status {})", close_status);
            return false;
        }
        return true;
    }

    /**
     * @brief Releases a retired image, or keeps its loader reference within the retention budget.
     * @return True after accepted retirement. A failed lease probe or module release returns false.
     */
    [[nodiscard]] bool release_generation(Generation &generation, std::uint32_t verdict) noexcept
    {
        if (generation.module == nullptr)
        {
            return true;
        }
        if (!host_lease_is_closed(generation.generation_id))
        {
            return false;
        }
        if (verdict == TPVCAMERA_RELOAD_RETAINED)
        {
            // Keep the loader's reference: a leaked resource can still need this image even without a module pin.
            record_retained_generation(generation, generation.module);
            generation.module = nullptr;
            return true;
        }
        if (FreeLibrary(generation.module) == 0)
        {
            append_formatted_log("FreeLibrary failed (error {})", GetLastError());
            return false;
        }
        generation.module = nullptr;
        if (!wait_for_unmap(generation.unmap_address))
        {
            record_retained_generation(generation);
            return true;
        }
        remove_file(generation.path);
        return true;
    }

    /// Runs a generation's Shutdown() and releases it on an accepted verdict.
    [[nodiscard]] bool retire_generation(Generation &generation) noexcept
    {
        if (generation.shutdown == nullptr)
        {
            return false;
        }
        const std::uint32_t verdict = generation.shutdown();
        if (verdict != TPVCAMERA_RELOAD_OK && verdict != TPVCAMERA_RELOAD_RETAINED)
        {
            append_log("Shutdown refused retirement; the generation stays mapped");
            return false;
        }
        return release_generation(generation, verdict);
    }

    /// Retires a stage whose exports or Init() failed. A stage that does not retire stays mapped until restart.
    void retire_failed_stage(Generation &generation) noexcept
    {
        if (!retire_generation(generation))
        {
            s_restart_required = true;
            record_retained_generation(generation, generation.module);
            generation.module = nullptr;
            append_log("The failed stage did not retire; restart the game before another reload");
        }
    }

    /**
     * @brief Copies the logic DLL to a unique staged name and measures it.
     * @return False when the copy or its size query fails, in which case no file is left behind.
     */
    [[nodiscard]] bool stage_copy(Generation &generation) noexcept
    {
        try
        {
            ++s_generation_counter;
            generation.generation_id = s_generation_counter;
            generation.path =
                s_directory / std::format(L"{}{:04}{}", STAGED_PREFIX, s_generation_counter, STAGED_SUFFIX);
            std::error_code error;
            std::filesystem::copy_file(s_directory / LOGIC_DLL_NAME, generation.path,
                                       std::filesystem::copy_options::overwrite_existing, error);
            if (error)
            {
                remove_file(generation.path);
                append_formatted_log("Staging generation {} failed: {}", generation.generation_id, error.message());
                return false;
            }
            generation.image_bytes = std::filesystem::file_size(generation.path, error);
            if (error)
            {
                const std::string message = error.message();
                remove_file(generation.path);
                append_formatted_log("Measuring staged generation {} failed: {}", generation.generation_id, message);
                return false;
            }
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    template <class Fn> [[nodiscard]] Fn resolve_export(HMODULE module, const char *symbol) noexcept
    {
        return reinterpret_cast<Fn>(reinterpret_cast<void *>(GetProcAddress(module, symbol)));
    }

    /// Stages, loads, resolves and initializes one generation.
    [[nodiscard]] bool load_generation() noexcept
    {
        Generation generation;
        if (!stage_copy(generation))
        {
            return false;
        }
        if (s_retained_count >= MAX_RETAINED_GENERATIONS || s_retained_bytes > MAX_RETAINED_BYTES ||
            generation.image_bytes > MAX_RETAINED_BYTES - s_retained_bytes)
        {
            s_restart_required = true;
            remove_file(generation.path);
            append_log("The next image exceeds the retention budget; restart the game before another reload");
            return false;
        }
        generation.module = LoadLibraryW(generation.path.c_str());
        if (generation.module == nullptr)
        {
            const DWORD error = GetLastError();
            remove_file(generation.path);
            append_formatted_log("LoadLibrary failed (error {})", error);
            return false;
        }
        generation.init = resolve_export<InitFn>(generation.module, "Init");
        generation.shutdown = resolve_export<ShutdownFn>(generation.module, "Shutdown");
        generation.revision = resolve_export<RevisionFn>(generation.module, "Revision");
        generation.unmap_address = reinterpret_cast<const void *>(generation.init);
        if (generation.init == nullptr || generation.shutdown == nullptr || generation.revision == nullptr)
        {
            append_log("The logic DLL is missing its Init, Shutdown or Revision export");
            retire_failed_stage(generation);
            return false;
        }
        const TpvReloadInitRequest request{
            .struct_size = static_cast<std::uint32_t>(sizeof(TpvReloadInitRequest)),
            .abi_version = TPVCAMERA_RELOAD_ABI_VERSION,
            .generation_id = generation.generation_id,
            .expected_host_identity = s_host_identity,
            .wheel_host = &s_wheel_host,
        };
        if (generation.init(&request) != TPVCAMERA_RELOAD_OK)
        {
            append_log("Init failed");
            retire_failed_stage(generation);
            return false;
        }
        const char *const revision = generation.revision();
        append_formatted_log("Generation {} is live. Revision: {}.", generation.generation_id,
                             revision != nullptr ? std::string_view{revision} : std::string_view{"unknown"});
        s_current.emplace(std::move(generation));
        return true;
    }

    /**
     * @brief Retires the live generation before a fresh load.
     * @return False after a refusal or a failed release. The caller must not load another generation over it.
     */
    [[nodiscard]] bool unload_current() noexcept
    {
        if (!s_current.has_value())
        {
            return true;
        }
        const std::uint32_t verdict = s_current->shutdown();
        if (verdict != TPVCAMERA_RELOAD_OK && verdict != TPVCAMERA_RELOAD_RETAINED)
        {
            append_log("Shutdown refused retirement; the generation stays mapped. Retry after quiescence");
            return false;
        }
        if (!release_generation(*s_current, verdict))
        {
            s_restart_required = true;
            append_log("The current generation did not retire; restart the game before another reload");
            return false;
        }
        s_current.reset();
        return true;
    }

    /**
     * @brief Checks the retention budget before teardown, so a full budget never retires the live generation.
     * @details The count check reserves slots for the current generation and a failed successor. The byte check
     *          includes the current image. load_generation() checks the successor's size before its load.
     */
    [[nodiscard]] bool budget_allows_reload() noexcept
    {
        const std::uintmax_t current_bytes = s_current.has_value() ? s_current->image_bytes : 0;
        const std::size_t current_slot = s_current.has_value() ? 1 : 0;
        if (s_retained_count >= MAX_RETAINED_GENERATIONS - current_slot || s_retained_bytes > MAX_RETAINED_BYTES ||
            current_bytes > MAX_RETAINED_BYTES - s_retained_bytes)
        {
            s_restart_required = true;
            append_log("The retained-generation budget is full; restart the game before another reload");
            return false;
        }
        return true;
    }

    void reload_once() noexcept
    {
        if (s_restart_required)
        {
            append_log("A previous reload could not be proved safe; restart the game");
            return;
        }
        if (!budget_allows_reload())
        {
            return;
        }
        append_log("Numpad 0 released. The loader retires the live generation and loads the build.");
        if (!unload_current())
        {
            return;
        }
        promote_from_staging();
        if (!load_generation())
        {
            append_log("The reload failed; no generation is live until the next press");
        }
    }

    /**
     * @brief Accepts the reload key only while this process owns the foreground window.
     * @details GetAsyncKeyState reads global key state, so an unguarded press in another window reloads the game.
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

    unsigned __stdcall control_thread(void *) noexcept
    {
        try
        {
            const std::optional<std::filesystem::path> directory = loader_directory();
            if (!directory.has_value())
            {
                return 0;
            }
            s_directory = *directory;

            // One loader log and one mod log per game run. Each generation appends to the mod log
            // (LogOpenMode::Append), so the loader starts both files fresh exactly once.
            remove_file(s_directory / LOADER_LOG_NAME);
            remove_file(s_directory / MOD_LOG_NAME);

            append_log("The loader started");
            remove_stale_staged_files();

            // The resident wheel host starts once, before the first generation, and lives for the process. It owns
            // the permanent wheel keepalive, so no generation has to. ABI v2 starts unmounted in target-wait state.
            // The logic-side poller resolves the game UI thread and drives the host retarget through the C table,
            // so the loader needs no window wait of its own.
            const int32_t host_status = wheel_host_start(
                0, DMK_WHEELHOST_ABI_VERSION, static_cast<std::uint32_t>(sizeof(s_wheel_host)), &s_wheel_host);
            if (host_status != DMK_WHEELHOST_OK)
            {
                append_formatted_log("The resident wheel host failed to start (status {}); reload is unavailable",
                                     host_status);
                return 0;
            }
            s_host_identity = s_wheel_host.host_identity;

            promote_from_staging();
            if (!load_generation())
            {
                append_log("The initial load failed; press Numpad 0 after a rebuild to retry");
            }

            bool was_down = false;
            for (;;) // The loader lives for the game session. Process exit ends this thread.
            {
                Sleep(CONTROL_POLL_MS);
                const bool down = (GetAsyncKeyState(RELOAD_VK) & KEY_DOWN_MASK) != 0;
                if (was_down && !down && foreground_belongs_to_this_process())
                {
                    reload_once();
                }
                was_down = down;
            }
        }
        catch (...)
        {
            // An exception cannot cross the CRT thread boundary into the host process.
            return 0;
        }
    }
} // namespace

/** @brief Starts the control thread on attach. The loader is never unloaded, so detach has no work. */
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) noexcept
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        s_loader_module = module;
        DisableThreadLibraryCalls(module);
        const std::uintptr_t thread = _beginthreadex(nullptr, 0, &control_thread, nullptr, 0, nullptr);
        if (thread == 0)
        {
            return FALSE;
        }
        CloseHandle(reinterpret_cast<HANDLE>(thread));
    }
    return TRUE;
}
