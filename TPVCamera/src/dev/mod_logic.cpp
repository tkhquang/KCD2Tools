/**
 * @file mod_logic.cpp
 * @brief The hot-reloaded half of the dev build: one generation of mod logic behind a C ABI.
 *
 * @details The resident loader calls Init() after LoadLibrary and Shutdown() before it releases or retains the
 *          image. There is no DllMain bootstrap on this path, so Init() owns the Session and Shutdown() drops it.
 *          Shutdown() returns a retirement verdict (protocol.h). Zero refuses retirement, and the loader keeps the
 *          image mapped. TPVCAMERA_RELOAD_OK permits release. TPVCAMERA_RELOAD_RETAINED makes the loader keep its
 *          module reference, because a pin or a leak (a retained XInput chain, for example) can still own code or
 *          state in this image.
 */

#include "constants.hpp"
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

    // Latches the first failed teardown: a mod thread that did not join or a hook that did not restore its target.
    // A retry must never turn either into an unload acceptance, and a latched image never starts a generation.
    bool s_teardown_failed = false;

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
        (void)DMK::log().try_log(written ? DMK::LogLevel::Info : DMK::LogLevel::Warning,
                                 written ? "[DEV] Profile written to {}" : "[DEV] Profile export to {} failed",
                                 profile_path);
    }
#endif

    /**
     * @brief Tears the mod down and latches a permanent failure.
     * @return TPVCamera::shutdown()'s status. Busy stays retryable. Failed latches, so a retry never reports success.
     */
    [[nodiscard]] TPVCamera::RetireStatus tear_down_generation() noexcept
    {
        if (s_teardown_failed)
        {
            return TPVCamera::RetireStatus::Failed;
        }
        try
        {
            const TPVCamera::RetireStatus status = TPVCamera::shutdown();
            s_teardown_failed = status == TPVCamera::RetireStatus::Failed;
            return status;
        }
        catch (...)
        {
            s_teardown_failed = true;
            return TPVCamera::RetireStatus::Failed;
        }
    }

    /**
     * @brief Computes the retirement verdict after ~Session and records it in the loader's log.
     * @details A latched teardown failure refuses retirement. Otherwise any open module pin or intentional leak
     *          retains the image, because a leak counter does not prove which leaked resource still holds a pin.
     */
    [[nodiscard]] std::uint32_t retirement_verdict() noexcept
    {
        namespace diag = DMK::diagnostics;
        const std::size_t pins = diag::total_module_pins();
        const std::size_t leaks = diag::total_intentional_leaks();
        const std::uint32_t verdict = s_teardown_failed         ? 0u
                                      : pins != 0 || leaks != 0 ? TPVCAMERA_RELOAD_RETAINED
                                                                : TPVCAMERA_RELOAD_OK;

        char line[400];
        (void)std::snprintf(
            line, sizeof(line),
            "[KCD2_TPVCamera][DEV] retirement verdict: %s (pins %zu: wheel %zu, xinput %zu+%zu, worker %zu; leaks %zu)",
            verdict == 0u                          ? "REFUSED (a mod worker or a hook did not retire)"
            : verdict == TPVCAMERA_RELOAD_RETAINED ? "retained"
                                                   : "released",
            pins, diag::module_pin_count(diag::ModulePinReason::MessageHookKeepalive),
            diag::module_pin_count(diag::ModulePinReason::XInputKeepalive),
            diag::module_pin_count(diag::ModulePinReason::XInputTarget),
            diag::module_pin_count(diag::ModulePinReason::Worker), leaks);
        append_loader_log(line);
        return verdict;
    }

    /**
     * @brief Drops every reclaimable generation resource after a failed Init() step.
     * @details Input start is the last fallible step of TPVCamera::init(), so no input callback can run here, and the
     *          rollback omits the typed drain.
     */
    void roll_back_generation() noexcept
    {
        if (tear_down_generation() != TPVCamera::RetireStatus::Retired)
        {
            // A worker or a hook still depends on the Session and this image: keep both.
            return;
        }
        s_session.reset();
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
     * @return TPVCAMERA_RELOAD_OK when the generation is live, or zero when initialization fails.
     * @note The loader calls this from its control thread, off the loader lock.
     */
    __declspec(dllexport) std::uint32_t DMK_WHEELHOST_CALL Init(const TpvReloadInitRequest *request) noexcept
    {
        if (request == nullptr || request->struct_size < sizeof(TpvReloadInitRequest) ||
            request->abi_version != TPVCAMERA_RELOAD_ABI_VERSION || request->generation_id == 0 ||
            request->wheel_host == nullptr || request->expected_host_identity == 0 ||
            request->wheel_host->host_identity != request->expected_host_identity || s_session.has_value() ||
            s_teardown_failed)
        {
            append_loader_log("[KCD2_TPVCamera][DEV] Init rejected an invalid request, a foreign wheel host or a "
                              "retired image");
            return 0;
        }

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
                roll_back_generation();
                return 0;
            }
            return TPVCAMERA_RELOAD_OK;
        }
        catch (...)
        {
            append_loader_log("[KCD2_TPVCamera][DEV] Init threw an exception");
            if (s_session.has_value())
            {
                roll_back_generation();
            }
            return 0;
        }
    }

    /**
     * @brief Retires feature state before the loader releases or retains this image.
     * @return Zero refuses retirement. TPVCAMERA_RELOAD_OK permits release. TPVCAMERA_RELOAD_RETAINED requires the
     *         loader's module reference.
     * @note The loader keeps the DLL mapped on a zero result.
     */
    __declspec(dllexport) std::uint32_t DMK_WHEELHOST_CALL Shutdown() noexcept
    {
        if (!s_session.has_value())
        {
            return retirement_verdict();
        }
        try
        {
            DMK::log().info("[DEV] Shutdown called");

#ifdef DMK_ENABLE_PROFILING
            // Before teardown, while the Session's logger still reports where the file went.
            export_profile();
#endif

            // Mod workers join first, then every hook disables, drains, and restores newest-first while this image
            // stays mapped. Busy refuses this retirement only: the loader keeps the image live, and the next reload
            // press retries the teardown.
            switch (tear_down_generation())
            {
            case TPVCamera::RetireStatus::Retired:
                break;
            case TPVCamera::RetireStatus::Busy:
                DMK::log().warning("[DEV] Shutdown: a detour stayed busy; retirement refused until the next attempt");
                return 0;
            case TPVCamera::RetireStatus::Failed:
                DMK::log().error("[DEV] Shutdown: a mod worker or a hook did not retire; the image stays mapped");
                return retirement_verdict();
            }

            // The typed drain retires every DMK-owned binding and config callback and delivers a held combo's
            // release while this image is mapped. Only SafeToUnload authorizes retirement.
            if (const DMK::LogicDllUnloadStatus drain = DMK::prepare_logic_dll_unload_all();
                drain != DMK::LogicDllUnloadStatus::SafeToUnload)
            {
                DMK::log().error("[DEV] Shutdown: the drain refused retirement (status {}); the image stays mapped",
                                 static_cast<int>(drain));
                return 0;
            }

            // Ordered Session teardown can retain the XInput chain, so the verdict reads the pins after it.
            s_session.reset();
            return retirement_verdict();
        }
        catch (...)
        {
            s_teardown_failed = true;
            append_loader_log("[KCD2_TPVCamera][DEV] Shutdown threw an exception; retirement refused");
            return 0;
        }
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
