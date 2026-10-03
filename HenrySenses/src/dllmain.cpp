/**
 * @file dllmain.cpp
 * @brief DLL entry point wiring the mod lifecycle to the DetourModKit Session.
 *
 * bootstrap_attach() performs the process and single-instance gates without allocating, then runs
 * on_ready(session) on its own worker thread off the loader lock; ~Session (also on that worker) clears the binding
 * scope and tears the DMK subsystems down in order.
 *
 * DetourModKit does not own the mod's own state: hooks are caller-owned handles, and HenrySenses::shutdown() restores
 * the patched prologues and every engine field the mod wrote. It must run off the loader lock, so DllMain never calls
 * it. The production ASI lives for the whole process: a bare FreeLibrary cannot reach DLL_PROCESS_DETACH while the
 * bootstrap worker holds its module reference, and process exit takes bootstrap_detach's abandon path. The dev
 * build's Shutdown export runs the teardown on the loader's control thread.
 */

#include "henry_senses.hpp"
#include "constants.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

// In the two-DLL dev build the logic is loaded by a thin loader ASI that owns the entry points (see
// src/dev/mod_logic.cpp). The production ASI uses DllMain.
#ifndef HENRYSENSES_DEV_BUILD

namespace
{
    /**
     * @brief Bootstrap callback: runs the mod init on the DMK worker thread.
     * @param session The live Session handed over by bootstrap_attach().
     * @return The init result; a failure leaves the mod inert.
     */
    DMK::Result<void> on_ready(DMK::Session &session)
    {
        DMK::Result<void> result;
        try
        {
            result = HenrySenses::init(session);
        }
        catch (...)
        {
            result = std::unexpected(DMK::Error{DMK::ErrorCode::Unknown, "HenrySenses::init/exception"});
        }
        if (!result && !HenrySenses::shutdown())
        {
            // The bootstrap worker retains its Session and module reference until process exit, even after
            // on_ready reports failure. An incomplete rollback therefore leaves this image mapped.
            (void)DMK::log().log_noexcept(DMK::LogLevel::Error, "Bootstrap: cleanup refused; the mod stays mapped");
        }
        return result;
    }
} // namespace

/**
 * @brief Production DLL entry point.
 * @param h_module This module (unused; DetourModKit captures its own module from a code address).
 * @param ul_reason_for_call The loader notification.
 * @param lp_reserved Null for an explicit FreeLibrary, non-null for process exit.
 * @return TRUE unless the attach gate refused this load.
 */
BOOL APIENTRY DllMain(HMODULE h_module, DWORD ul_reason_for_call, LPVOID lp_reserved)
{
    (void)h_module;

    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    {
        DMK::AsyncLoggerConfig async_cfg;
        // Detours share this logger. Queue overflow must never move file I/O onto a game callback.
        async_cfg.overflow_policy = DMK::OverflowPolicy::DropNewest;

        // No process-name gate: the WHGame.dll module check in init() is the gate.
        const DMK::ModInfo info{
            .name = HenrySenses::constants::MOD_NAME,
            .log_file = HenrySenses::constants::LOG_FILE_NAME,
            .game_process_name = "",
            .instance_mutex_prefix = HenrySenses::constants::INSTANCE_MUTEX_PREFIX,
            .log = async_cfg,
            // The [file:line] stamp is kept only on Trace, where it is read while debugging. The default Truncate
            // open mode is correct here: this build is a single DLL with one Session per run.
            .log_source_stamp_mode = DMK::LogSourceStampMode::at_or_below(DMK::LogLevel::Trace),
        };

        // A gate refusal (wrong process, a duplicate load already holding the mutex) is a reason for this DLL to
        // go away, not for the host to fail, so report it as a failed attach and let the loader unmap us.
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

#endif // HENRYSENSES_DEV_BUILD
