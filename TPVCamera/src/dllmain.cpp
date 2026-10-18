/**
 * @file dllmain.cpp
 * @brief DLL entry point wiring the mod lifecycle to the DetourModKit Session.
 *
 * bootstrap_attach() performs the process and single-instance gates without allocating, then runs
 * on_ready(session) on its own worker thread off the loader lock; ~Session (also on that worker)
 * clears the binding scope and tears the DMK subsystems down in order.
 *
 * DetourModKit does not own the mod's own state. Hooks are caller-owned handles, so TPVCamera::shutdown() is the
 * only path that restores the patched prologues. It must run off the loader lock, so DllMain never calls it. The
 * production ASI lives for the whole process: a bare FreeLibrary cannot reach DLL_PROCESS_DETACH while the bootstrap
 * worker holds its module reference, and process exit takes bootstrap_detach's abandon path. on_ready rolls a failed
 * init back on the bootstrap worker, and the dev build's Shutdown export runs the teardown on the loader's control
 * thread.
 */

#include "tpv_camera.hpp"
#include "constants.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

// In the two-DLL dev build the resident loader ASI (src/dev/mod_loader.cpp) drives the logic DLL through the
// Init and Shutdown exports in src/dev/mod_logic.cpp. The production ASI uses this DllMain.
#ifndef TPVCAMERA_DEV_BUILD

namespace
{
    /**
     * @brief Bootstrap callback: runs the mod init on the DMK worker thread.
     * @param session The live Session handed over by bootstrap_attach().
     * @return The init result. A failure leaves the mod inert.
     */
    DMK::Result<void> on_ready(DMK::Session &session)
    {
        DMK::Result<void> result;
        try
        {
            result = TPVCamera::init(session);
        }
        catch (...)
        {
            result = std::unexpected(DMK::Error{DMK::ErrorCode::Unknown, "TPVCamera::init/exception"});
        }
        if (!result)
        {
            // The bootstrap worker retains its Session and module reference until process exit, even after on_ready
            // reports failure, so hooks that did not retire stay valid. shutdown() logs that outcome itself.
            (void)TPVCamera::shutdown();
        }
        return result;
    }
} // namespace

BOOL APIENTRY DllMain(HMODULE h_module, DWORD ul_reason_for_call, LPVOID lp_reserved)
{
    // bootstrap_attach captures the calling module itself, because DetourModKit links statically into this
    // DLL and its code address resolves to this HMODULE.
    (void)h_module;

    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    {
        DMK::AsyncLoggerConfig async_cfg;
        // Fall back to synchronous logging if the async queue overflows so no
        // diagnostic line is lost during a burst (startup or teardown).
        async_cfg.overflow_policy = DMK::OverflowPolicy::SyncFallback;

        // No process-name gate: the WHGame.dll module check in init() is the gate.
        const DMK::ModInfo info{
            .name = Constants::MOD_NAME,
            .log_file = Constants::LOG_FILE_NAME,
            .game_process_name = "",
            .instance_mutex_prefix = Constants::INSTANCE_MUTEX_PREFIX,
            .log = async_cfg,
            // Matches the dev build: the [file:line] stamp is kept only on Trace, where it is read while
            // debugging, and every higher level renders clean. log_open_mode is deliberately NOT matched -
            // this build is a single DLL with one Session per run, so the default Truncate is correct here
            // while the dev loader needs Append to keep each generation's teardown records.
            .log_source_stamp_mode = DMK::LogSourceStampMode::at_or_below(DMK::LogLevel::Trace),
        };

        // A gate refusal (wrong process, a duplicate load already holding the mutex) is a reason for
        // this DLL to go away, not for the host to fail, so report it as a failed attach and let the
        // loader unmap us.
        return DMK::bootstrap_attach(info, &on_ready).has_value() ? TRUE : FALSE;
    }

    case DLL_PROCESS_DETACH:
        // Loader-lock safe on both paths ([B-100]): it neither waits nor joins, and at process exit it abandons the
        // Session, whose threads the OS has already ended.
        DMK::bootstrap_detach(lp_reserved);
        break;

    default:
        break;
    }

    return TRUE;
}

#endif // TPVCAMERA_DEV_BUILD
