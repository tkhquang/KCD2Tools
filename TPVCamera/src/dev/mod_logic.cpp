/**
 * @file mod_logic.cpp
 * @brief The hot-reloaded half of the dev build: one generation of mod logic behind a C ABI.
 *
 * @details The resident loader calls Init() after LoadLibrary and Shutdown() before it releases or retains the
 *          image. There is no DllMain bootstrap on this path, so Init() owns the Session and Shutdown() drops it.
 *          Shutdown() returns a DetourModKit/abi/staged_reload.h verdict, which authorizes or refuses the unmap. The
 *          sequence follows DetourModKit's hot-reload guide: retire the hooks, stop the workers, run the typed drain,
 *          drop the Session, then let staged_reload::verdict read every leak and module pin above the baseline Init
 *          captured.
 */

#include "constants.hpp"
#include "frame_recorder.hpp"
#include "hook_profile.hpp"
#include "see_through_trace.hpp"
#include "protocol.h"
#include "tpv_camera.hpp"
#include "version.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>

namespace
{
    /// The live Session for this generation. Empty before Init() and after a retired Shutdown().
    std::optional<DMK::Session> s_session;

    /// The leak and module-pin counts before this generation started, so the verdict reads only what it added.
    DMK::staged_reload::Baseline s_baseline;

    /// This generation's id from the loader's Init request. Names the profile export of a profiling build.
    std::uint64_t s_generation_id = 0;

    /// The module that holds this code, or nullptr when the loader cannot report it.
    [[nodiscard]] HMODULE own_module() noexcept
    {
        HMODULE self = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&own_module), &self) == 0)
        {
            return nullptr;
        }
        return self;
    }

    /**
     * @brief Writes the directory this DLL was loaded from, with a trailing backslash, into @p out.
     * @return False when the module path cannot be resolved.
     */
    [[nodiscard]] bool module_directory(char (&out)[MAX_PATH]) noexcept
    {
        out[0] = '\0';
        const HMODULE self = own_module();
        if (self == nullptr || GetModuleFileNameA(self, out, MAX_PATH) == 0)
        {
            return false;
        }
        char *const slash = std::strrchr(out, '\\');
        if (slash == nullptr)
        {
            return false;
        }
        slash[1] = '\0';
        return true;
    }

    /**
     * @brief Appends one line to the loader's log, which outlives every generation.
     * @details The retirement verdict is computed after ~Session, when DMK::log() no longer has a sink.
     */
    void append_loader_log(const char *line) noexcept
    {
        char module_path[MAX_PATH]{};
        if (!module_directory(module_path))
        {
            OutputDebugStringA(line);
            return;
        }

        char log_path[MAX_PATH]{};
        (void)std::snprintf(log_path, sizeof(log_path), "%sKCD2_TPVCamera_Loader.log", module_path);
        const HANDLE file = CreateFileA(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            SYSTEMTIME now{};
            GetLocalTime(&now);
            char stamped[640];
            const int length = std::snprintf(stamped, sizeof(stamped), "[%02u:%02u:%02u.%03u] %s\n", now.wHour,
                                             now.wMinute, now.wSecond, now.wMilliseconds, line);
            if (length > 0)
            {
                DWORD written = 0;
                (void)WriteFile(file, stamped, static_cast<DWORD>(length), &written, nullptr);
            }
            CloseHandle(file);
        }
        OutputDebugStringA(line);
    }

#ifdef DMK_ENABLE_PROFILING
    /**
     * @brief Writes this generation's profiler samples to KCD2_TPVCamera_profile_genNNNN.json beside the log.
     * @details The profiler ring belongs to the DetourModKit instance linked into this image, so its samples go with
     *          the image. Each Shutdown() therefore leaves one Chrome Tracing file per generation. The export
     *          allocates and writes a file, so it runs on the loader's control thread, off the loader lock. A failure
     *          stays here, so a diagnostics export never keeps Shutdown() from its teardown.
     */
    void export_profile() noexcept
    {
        char module_path[MAX_PATH]{};
        if (!module_directory(module_path))
        {
            return;
        }
        char profile_path[MAX_PATH]{};
        (void)std::snprintf(profile_path, sizeof(profile_path), "%sKCD2_TPVCamera_profile_gen%04llu.json", module_path,
                            static_cast<unsigned long long>(s_generation_id));
        bool written = false;
        try
        {
            written = DMK::Profiler::get_instance().export_to_file(profile_path);
        }
        catch (...)
        {
            written = false;
        }
        // Two calls: the format string must be a constant expression for DMK's checked formatting.
        if (written)
        {
            (void)DMK::log().try_log(DMK::LogLevel::Info, "[DEV] Profile written to {}", profile_path);
        }
        else
        {
            (void)DMK::log().try_log(DMK::LogLevel::Warning, "[DEV] Profile export to {} failed", profile_path);
        }
    }

    /**
     * @brief Writes this generation's hook aggregates (HookProfile) and see-through trace (SeeThroughTrace) beside the
     *        log as KCD2_TPVCamera_hook_profile_genNNNN.json and KCD2_TPVCamera_see_through_genNNNN.json.
     * @details Each measuring thread owns its slot of that storage without synchronization, so the export runs only
     *          after every hook has retired and the typed drain has passed. A failure is logged and stays here.
     */
    void export_hook_profile() noexcept
    {
        char directory[MAX_PATH]{};
        if (!module_directory(directory))
        {
            return;
        }
        char path[MAX_PATH]{};
        (void)std::snprintf(path, sizeof(path), "%sKCD2_TPVCamera_hook_profile_gen%04llu.json", directory,
                            static_cast<unsigned long long>(s_generation_id));
        if (TPVCamera::HookProfile::export_after_rundown(path))
        {
            (void)DMK::log().try_log(DMK::LogLevel::Info, "[DEV] Hook aggregates written to {}", path);
        }
        else
        {
            (void)DMK::log().try_log(DMK::LogLevel::Warning, "[DEV] Hook aggregate export failed: {}", path);
        }
        (void)std::snprintf(path, sizeof(path), "%sKCD2_TPVCamera_see_through_gen%04llu.json", directory,
                            static_cast<unsigned long long>(s_generation_id));
        if (TPVCamera::SeeThroughTrace::export_after_rundown(path))
        {
            (void)DMK::log().try_log(DMK::LogLevel::Info, "[DEV] See-through CPU trace written to {}", path);
        }
        else
        {
            (void)DMK::log().try_log(DMK::LogLevel::Warning, "[DEV] See-through CPU trace export failed: {}", path);
        }
    }
#endif

    /**
     * @brief Writes the camera collision frames recorded under [Debug] RecordFrames beside the log as
     *        KCD2_TPVCamera_frames_genNNNN.tpvrec, for the offline replay. A failure is logged and stays here.
     */
    void export_recorded_frames() noexcept
    {
        char directory[MAX_PATH]{};
        if (!module_directory(directory))
        {
            return;
        }
        char path[MAX_PATH]{};
        (void)std::snprintf(path, sizeof(path), "%sKCD2_TPVCamera_frames_gen%04llu.tpvrec", directory,
                            static_cast<unsigned long long>(s_generation_id));
        if (!TPVCamera::FrameRecorder::export_after_rundown(path))
        {
            (void)DMK::log().try_log(DMK::LogLevel::Warning, "[DEV] Recorded frame export failed: {}", path);
        }
    }

    /// Names a DetourModKit/abi/staged_reload.h value for the loader's log.
    [[nodiscard]] const char *verdict_name(std::uint32_t verdict) noexcept
    {
        switch (verdict)
        {
        case DMK_STAGED_RELOAD_OK:
            return "OK";
        case DMK_STAGED_RELOAD_RETAINED:
            return "RETAINED";
        case DMK_STAGED_RELOAD_BUSY:
            return "BUSY";
        case DMK_STAGED_RELOAD_RESTART_REQUIRED:
            return "RESTART_REQUIRED";
        default:
            return "REFUSED";
        }
    }

    /**
     * @brief Computes the verdict for @p retirement and records it, with the hook and pin counts behind it, in the
     *        loader's log.
     * @details The loader is built from this tree and Init accepts only its ABI, so it knows every value of this
     *          DetourModKit's staged_reload ABI.
     */
    [[nodiscard]] std::uint32_t report_verdict(const DMK::hook::HookRetirement &retirement) noexcept
    {
        namespace diag = DMK::diagnostics;
        const std::uint32_t verdict =
            DMK::staged_reload::verdict(retirement, s_baseline, DMK_STAGED_RELOAD_ABI_VERSION);
        char line[400];
        (void)std::snprintf(line, sizeof(line),
                            "[KCD2_TPVCamera][DEV] unload verdict: %s (hooks %u restored, %u vacated, %u pinned, %u "
                            "in flight; pins %zu: wheel %zu, xinput %zu+%zu, worker %zu; leaks %zu)",
                            verdict_name(verdict), retirement.restored, retirement.vacated, retirement.pinned,
                            retirement.in_flight, diag::total_module_pins(),
                            diag::module_pin_count(diag::ModulePinReason::MessageHookKeepalive),
                            diag::module_pin_count(diag::ModulePinReason::XInputKeepalive),
                            diag::module_pin_count(diag::ModulePinReason::XInputTarget),
                            diag::module_pin_count(diag::ModulePinReason::Worker), diag::total_intentional_leaks());
        append_loader_log(line);
        return verdict;
    }

    /**
     * @brief Tears the generation down in DetourModKit's Shutdown order and decides whether the loader may retire
     *        its image.
     * @details The hooks retire first, so a BUSY or refused retirement leaves every hook armed, and a pinned hook keeps
     *          the Session for the detour that stays reachable. The mod teardown then joins its workers, the typed
     *          drain retires every binding and config callback DMK still owns while this image is mapped, and
     *          ~Session tears the subsystems down, where the XInput layer can still retain its chain. The verdict
     *          therefore reads the counts after ~Session. Every step is idempotent, so a failed Init and a later
     *          Shutdown run the same sequence.
     * @return A DetourModKit/abi/staged_reload.h value.
     */
    [[nodiscard]] std::uint32_t retire_generation() noexcept
    {
        const TPVCamera::ShutdownResult teardown = TPVCamera::shutdown();
        if (teardown.hooks.outcome != DMK::hook::RetirementOutcome::Retired || teardown.hooks.pinned != 0)
        {
            return report_verdict(teardown.hooks);
        }
        if (!teardown.complete)
        {
            // A worker that did not join still runs code in this image and reads the Session, so both stay.
            append_loader_log("[KCD2_TPVCamera][DEV] unload REFUSED: mod cleanup is incomplete; the session remains "
                              "active");
            return DMK_STAGED_RELOAD_REFUSED;
        }

        if (s_session.has_value())
        {
            // The typed drain retires every DMK-owned binding and config callback and delivers a held combo's
            // release while this image is mapped. Only SafeToUnload authorizes retirement.
            if (const DMK::LogicDllUnloadStatus drain = DMK::prepare_logic_dll_unload_all();
                drain != DMK::LogicDllUnloadStatus::SafeToUnload)
            {
                (void)DMK::log().try_log(DMK::LogLevel::Error,
                                         "[DEV] Shutdown: the drain refused retirement (status {}); the image stays "
                                         "mapped",
                                         static_cast<int>(drain));
                return DMK_STAGED_RELOAD_REFUSED;
            }

#ifdef DMK_ENABLE_PROFILING
            // Every hook has retired and drained, so no thread writes the aggregates any more.
            export_hook_profile();
#endif
            // And no camera frame appends to the recorded collision frames.
            export_recorded_frames();

            s_session.reset();
        }
        return report_verdict(teardown.hooks);
    }
} // namespace

extern "C"
{
    /**
     * @brief Reports which bytes the loader mapped.
     * @details The image identity (PE timestamp, image size and section layout) changes with every link, so it names
     *          the exact build without a hand-bumped counter.
     */
    __declspec(dllexport) const char *DMK_WHEELHOST_CALL Revision() noexcept
    {
        static const std::array<char, 96> revision = []() noexcept
        {
            std::array<char, 96> text{};
            const HMODULE self = own_module();
            const DMK::scan::ImageIdentity identity =
                self != nullptr
                    ? DMK::scan::image_identity(DMK::Region{DMK::Address{reinterpret_cast<std::uintptr_t>(self)}, 1})
                    : DMK::scan::ImageIdentity{};
            (void)std::snprintf(text.data(), text.size(), "%s image %016llX", TPVCamera::Version::VERSION_TAG,
                                static_cast<unsigned long long>(identity.token()));
            return text;
        }();
        return revision.data();
    }

    /**
     * @brief Starts one generation with the loader's resident wheel host.
     * @param request The versioned request. Its host table remains valid for the process lifetime.
     * @return DMK_STAGED_RELOAD_OK when the generation is live, or zero. A failed Init already ran the Shutdown
     *         sequence, and the loader retires the failed stage through Shutdown().
     * @note The loader calls this from its control thread, off the loader lock.
     */
    __declspec(dllexport) std::uint32_t DMK_WHEELHOST_CALL Init(const TpvReloadInitRequest *request) noexcept
    {
        // A HookManager leak means a detour of this image stays reachable, so no generation starts beside it.
        if (request == nullptr || request->struct_size < sizeof(TpvReloadInitRequest) ||
            request->abi_version != TPVCAMERA_RELOAD_ABI_VERSION || request->generation_id == 0 ||
            request->wheel_host == nullptr || request->expected_host_identity == 0 ||
            request->wheel_host->host_identity != request->expected_host_identity || s_session.has_value() ||
            DMK::diagnostics::intentional_leak_count(DMK::diagnostics::LeakSubsystem::HookManager) != 0)
        {
            append_loader_log("[KCD2_TPVCamera][DEV] Init rejected an invalid request, a foreign wheel host or a "
                              "retired image");
            return 0;
        }

        // Before the Session, so the verdict ignores every count this generation did not create.
        s_baseline = DMK::staged_reload::capture();

        // The loader calls this through a C function pointer, so no exception may cross the boundary.
        try
        {
            DMK::AsyncLoggerConfig async_cfg;
            async_cfg.overflow_policy = DMK::OverflowPolicy::SyncFallback;

            // LogOpenMode::Append keeps the previous generation's teardown records, including the XInput retention
            // lines that explain a retained image. The loader truncates the file once per game run.
            auto opened = DMK::Session::start(DMK::ModInfo{
                .name = Constants::MOD_NAME,
                .log_file = Constants::LOG_FILE_NAME,
                .game_process_name = "",
                .instance_mutex_prefix = Constants::INSTANCE_MUTEX_PREFIX,
                .log = async_cfg,
                .log_open_mode = DMK::LogOpenMode::Append,
                // The [file:line] stamp stays on Trace records, which are read while debugging. Every generation
                // appends to one file, so Debug and above render without it.
                .log_source_stamp_mode = DMK::LogSourceStampMode::at_or_below(DMK::LogLevel::Trace),
            });
            if (!opened.has_value())
            {
                append_loader_log("[KCD2_TPVCamera][DEV] Session::start failed");
                return 0;
            }
            s_session.emplace(std::move(*opened));
            s_generation_id = request->generation_id;

            DMK::log().info("[DEV] Init generation {} - {}", request->generation_id, Revision());

            // The resident host table reaches Input::start, so the wheel keepalive lands on the loader.
            if (auto ready = TPVCamera::init(*s_session, request->wheel_host); !ready.has_value())
            {
                DMK::log().error("[DEV] TPVCamera initialization FAILED ({})", ready.error().message());
                (void)retire_generation();
                return 0;
            }
            return DMK_STAGED_RELOAD_OK;
        }
        catch (...)
        {
            append_loader_log("[KCD2_TPVCamera][DEV] Init threw an exception");
            (void)retire_generation();
            return 0;
        }
    }

    /**
     * @brief Tears this generation down and reports whether the image may be unmapped.
     * @return A DetourModKit/abi/staged_reload.h value. BUSY leaves every hook armed, so the loader can retry.
     *         REFUSED keeps the image mapped. RESTART_REQUIRED means a detour of this image stays reachable.
     * @note The loader calls this from its control thread, off the loader lock.
     */
    __declspec(dllexport) std::uint32_t DMK_WHEELHOST_CALL Shutdown() noexcept
    {
        (void)DMK::log().try_log(DMK::LogLevel::Info, "[DEV] Shutdown called");
#ifdef DMK_ENABLE_PROFILING
        // Before teardown, while the Session's logger still reports where the file went.
        export_profile();
#endif
        return retire_generation();
    }
} // extern "C"

/** @brief Prevents Session teardown under the loader lock at process termination. */
BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID reserved) noexcept
{
    if (reason == DLL_PROCESS_DETACH && reserved != nullptr && s_session.has_value())
    {
        s_session->abandon();
    }
    return TRUE;
}
