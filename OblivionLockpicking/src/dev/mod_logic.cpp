/**
 * @file mod_logic.cpp
 * @brief The hot-reloaded half of the dev build: one generation of mod logic behind a C ABI.
 *
 * @details The resident loader owns the process and this DLL owns one generation. The loader calls Init() after
 *          LoadLibrary and Shutdown() before it releases or retains the image. There is no DllMain bootstrap on this
 *          path, so Init() owns the Session directly and Shutdown() drops it.
 *
 *          Shutdown()'s return value is an UNMAP AUTHORIZATION, not a status. A success returned while a detour body
 *          in this image is still reachable turns a reload into a stale image and then a crash. The sequence follows
 *          DetourModKit's hot-reload guide: retire the hooks, stop the workers, run the typed drain, drop the Session,
 *          then let staged_reload::verdict read every leak and module pin above the baseline Init captured.
 *
 *          Only compiled when OBLIVIONLP_DEV_BUILD is set by the dev preset.
 */

#include "../constants.hpp"
#include "../oblivion_lockpicking.hpp"
#include "../version.hpp"

#include "loader_log.hpp"
#include "abi/reload.h"

#include <DetourModKit/async_logger_config.hpp>
#include <DetourModKit/diagnostics.hpp>
#include <DetourModKit/filesystem.hpp>
#include <DetourModKit/hook.hpp>
#include <DetourModKit/logger.hpp>
#include <DetourModKit/session.hpp>
#include <DetourModKit/staged_reload.hpp>

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <utility>

/// The version tag as a string literal, so Revision() can append the build stamp at compile time.
#define OBLIVIONLP_DEV_VERSION                                                                                         \
    "v" VERSION_STR(VERSION_MAJOR) "." VERSION_STR(VERSION_MINOR) "." VERSION_STR(VERSION_PATCH)

namespace
{
    /// The live Session for this generation. Empty before Init() and after a retired Shutdown().
    std::optional<DMK::Session> s_session;

    /// The leak and module-pin counts before this generation started, so the verdict reads only what it added.
    DMK::staged_reload::Baseline s_baseline;

    /**
     * @brief Appends one line to the LOADER's log, which outlives every generation.
     * @details The unload verdict is computed after ~Session, so DMK::log() is gone by then. The line takes the
     *          loader log's own format (loader_log.hpp), so it sorts in with the loader's records. A line sent only to
     *          OutputDebugStringA hides from anyone without a debugger attached, which is exactly how a refusal loop
     *          goes unexplained.
     * @note get_runtime_directory() allocates, and catch(...) handlers call this with an exception already in
     *       flight, so the path build runs inside try/catch to hold the noexcept boundary.
     */
    void append_loader_log(const char *line) noexcept
    {
        std::wstring log_path;
        try
        {
            log_path = OblivionLockpicking::dev::loader_log_path(
                DMK::filesystem::get_runtime_directory(),
                OblivionLockpicking::constants::MOD_NAME
            );
        }
        catch (...)
        {
            OblivionLockpicking::dev::echo_to_debugger(line);
            return;
        }
        OblivionLockpicking::dev::append_line(log_path.c_str(), line);
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
        (void)std::snprintf(
            line,
            sizeof(line),
            "[KCD2_OblivionLockpicking][DEV] unload verdict: %s (hooks %u restored, %u vacated, %u pinned, %u in "
            "flight; pins %zu: wheel %zu, xinput %zu+%zu, worker %zu; leaks %zu)",
            verdict_name(verdict),
            retirement.restored,
            retirement.vacated,
            retirement.pinned,
            retirement.in_flight,
            diag::total_module_pins(),
            diag::module_pin_count(diag::ModulePinReason::MessageHookKeepalive),
            diag::module_pin_count(diag::ModulePinReason::XInputKeepalive),
            diag::module_pin_count(diag::ModulePinReason::XInputTarget),
            diag::module_pin_count(diag::ModulePinReason::Worker),
            diag::total_intentional_leaks()
        );
        append_loader_log(line);
        return verdict;
    }

    /**
     * @brief Tears the generation down in DetourModKit's Shutdown order and decides whether the loader may retire
     *        its image.
     * @details The hooks retire first, so a BUSY or refused retirement leaves every hook armed, and a pinned hook keeps
     *          the Session for the detour that stays reachable. The mod teardown then stops its workers, the typed
     *          drain retires every binding and config callback DMK still owns while this image is mapped, and ~Session
     *          tears the subsystems down, where the XInput layer can still retain its chain. The verdict therefore
     *          reads the counts after ~Session. Every step is idempotent, so a failed Init and a later Shutdown run
     *          the same sequence.
     * @return A DetourModKit/abi/staged_reload.h value.
     */
    [[nodiscard]] std::uint32_t retire_generation() noexcept
    {
        // It retires both hooks after the main thread detaches the input listener.
        const OblivionLockpicking::ShutdownResult teardown = OblivionLockpicking::shutdown();
        if (teardown.hooks.outcome != DMK::hook::RetirementOutcome::Retired || teardown.hooks.pinned != 0)
        {
            return report_verdict(teardown.hooks);
        }
        if (!teardown.complete)
        {
            // A worker that did not join still runs code in this image and reads the Session, so both stay.
            append_loader_log(
                "[KCD2_OblivionLockpicking][DEV] unload REFUSED: mod cleanup is incomplete; the session remains active"
            );
            return DMK_STAGED_RELOAD_REFUSED;
        }

        if (s_session.has_value())
        {
            // A held hold-combo's balancing edge is delivered here, while this module's code is still mapped.
            const DMK::LogicDllUnloadStatus drain = DMK::prepare_logic_dll_unload_all();
            if (drain != DMK::LogicDllUnloadStatus::SafeToUnload)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Error,
                    "[DEV] drain refused unload (status {}); module stays mapped",
                    static_cast<int>(drain)
                );
                return DMK_STAGED_RELOAD_REFUSED;
            }
            s_session.reset();
        }
        return report_verdict(teardown.hooks);
    }

    /**
     * @brief Returns the loader's persistent slots after a header check.
     * @param state The area from the request.
     * @return The slots, or an empty span when the header does not match this build.
     */
    [[nodiscard]] std::span<std::uint64_t> persistent_slots(OblivionLpPersistentState *state) noexcept
    {
        if (state == nullptr || state->struct_size < sizeof(OblivionLpPersistentState) ||
            state->abi_version != OBLIVIONLP_RELOAD_ABI_VERSION || state->magic != OBLIVIONLP_PERSISTENT_MAGIC)
        {
            return {};
        }
        return std::span<std::uint64_t>(state->slots, OBLIVIONLP_PERSISTENT_SLOT_COUNT);
    }
} // namespace

extern "C"
{
    /**
     * @brief Reports this generation's source version.
     * @details Exported so the LOADER can log it even when Init() fails, which is exactly when knowing the version
     *          matters most.
     * @warning This is NOT a witness of which bytes were mapped. The embedded build stamp comes from __DATE__ /
     *          __TIME__ in THIS translation unit, so it advances only when this file recompiles. A build that changed
     *          any other source relinks the DLL and leaves the stamp behind. The loader logs the mapped file's own
     *          size and write time alongside this string, and that pair is the authoritative identity.
     */
    __declspec(dllexport) const char *Revision() noexcept
    {
        return OBLIVIONLP_DEV_VERSION " (" __DATE__ " " __TIME__ ")";
    }

    /**
     * @brief Starts one generation.
     * @param request Loader-owned request. Its wheel-host table and persistent area are valid for the whole process.
     * @return DMK_STAGED_RELOAD_OK when the generation is live, or zero. A failed Init already ran the Shutdown
     *         sequence, and the loader retires the failed stage through Shutdown().
     */
    __declspec(dllexport) std::uint32_t Init(const OblivionLpReloadInitRequest *request) noexcept
    {
        // Validate the whole request before touching a field: a stale generation left in the deploy directory must
        // fail loudly rather than read through a shifted layout. A HookManager leak means a detour of this image stays
        // reachable, so no generation starts beside it.
        //
        // A NULL wheel host is a deliberate, supported choice, not an error. Without that lease, this generation uses
        // the local message-hook backend and books its own permanent wheel keepalive, so its image is retained after
        // teardown. The loader charges that against its reload budget. A NON-null table must still match the
        // expected identity, so a foreign table is never accepted.
        if (request == nullptr || request->struct_size < sizeof(OblivionLpReloadInitRequest) ||
            request->abi_version != OBLIVIONLP_RELOAD_ABI_VERSION || request->generation_id == 0 ||
            s_session.has_value() ||
            DMK::diagnostics::intentional_leak_count(DMK::diagnostics::LeakSubsystem::HookManager) != 0)
        {
            append_loader_log(
                "[KCD2_OblivionLockpicking][DEV] Init rejected an invalid or stale reload request, or a retired image"
            );
            return 0;
        }
        if (request->wheel_host != nullptr && (request->expected_host_identity == 0 ||
                                               request->wheel_host->host_identity != request->expected_host_identity))
        {
            append_loader_log("[KCD2_OblivionLockpicking][DEV] Init rejected a foreign wheel-host table");
            return 0;
        }
        const std::span<std::uint64_t> slots = persistent_slots(request->persistent);
        if (slots.empty())
        {
            // Without the area, the engine state that a previous generation left behind cannot be adopted, and this
            // generation creates a second copy of it.
            append_loader_log(
                "[KCD2_OblivionLockpicking][DEV] Init rejected a request without a valid persistent-state area"
            );
            return 0;
        }

        // Before the Session, so the verdict ignores every count this generation did not create.
        s_baseline = DMK::staged_reload::capture();

        // The loader calls this through a C function pointer, so an exception must never unwind across the boundary.
        try
        {
            DMK::AsyncLoggerConfig async_config;
            async_config.overflow_policy = DMK::OverflowPolicy::DropNewest;

            // LogOpenMode::Append is what keeps a reload diagnosable. Under the default Truncate, this generation's
            // first sink open erases the PREVIOUS generation's teardown records, which are the only lines that
            // explain a retained image. The loader truncates the file once per game run.
            auto opened = DMK::Session::start(
                DMK::ModInfo{
                    .name = OblivionLockpicking::constants::MOD_NAME,
                    .log_file = OblivionLockpicking::constants::LOG_FILE_NAME,
                    .game_process_name = "",
                    .instance_mutex_prefix = OblivionLockpicking::constants::INSTANCE_MUTEX_PREFIX,
                    .log = async_config,
                    .log_open_mode = DMK::LogOpenMode::Append,
                    .log_source_stamp_mode = DMK::LogSourceStampMode::at_or_below(DMK::LogLevel::Trace),
                }
            );
            if (!opened)
            {
                append_loader_log("[KCD2_OblivionLockpicking][DEV] Session::start failed");
                return 0;
            }
            s_session.emplace(std::move(*opened));

            DMK::log().info("[DEV] Init generation {} - {}", request->generation_id, Revision());

            if (auto ready = OblivionLockpicking::init(*s_session, request->wheel_host, slots); !ready)
            {
                DMK::log().error("[DEV] OblivionLockpicking initialization FAILED ({})", ready.error().message());
                (void)retire_generation();
                return 0;
            }
            return DMK_STAGED_RELOAD_OK;
        }
        catch (...)
        {
            append_loader_log("[KCD2_OblivionLockpicking][DEV] Init threw an exception");
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
    __declspec(dllexport) std::uint32_t Shutdown() noexcept
    {
        (void)DMK::log().try_log(DMK::LogLevel::Info, "[DEV] Shutdown called");
        return retire_generation();
    }
} // extern "C"

/**
 * @brief The loader drives Init and Shutdown explicitly, so attach and an explicit detach have no work of their own.
 * @details At process exit (a non-null @p reserved) a live generation's Session is abandoned. Otherwise the C runtime
 *          runs ~Session here, under the loader lock, after the OS already ended every other thread.
 */
BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_DETACH && reserved != nullptr && s_session.has_value())
    {
        s_session->abandon();
    }
    return TRUE;
}
