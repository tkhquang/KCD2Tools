/**
 * @file dllmain.cpp
 * @brief DLL entry point of the production build, which wires the mod lifecycle to the DetourModKit Session.
 *
 * bootstrap_attach() runs the process and single-instance gates without allocation, then runs on_ready(session) on its
 * own worker thread off the loader lock. ~Session, also on that worker, clears the binding scope and tears the DMK
 * subsystems down in order.
 *
 * DetourModKit does not own the mod's state. The hooks are caller-owned handles, and OblivionLockpicking::shutdown()
 * restores the patched prologues. It must run off the loader lock, so DllMain never calls it. The production module
 * lives for the whole process: a bare FreeLibrary cannot reach DLL_PROCESS_DETACH while the bootstrap worker holds its
 * module reference, and process exit takes the abandon path of bootstrap_detach(). The dev build loads the logic
 * through a loader ASI instead (src/dev/mod_logic.cpp), so this file is not part of it.
 */

#include "oblivion_lockpicking.hpp"
#include "constants.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

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
            result = OblivionLockpicking::init(session);
        }
        catch (...)
        {
            result = std::unexpected(DMK::Error{DMK::ErrorCode::Unknown, "OblivionLockpicking::init/exception"});
        }
        if (!result)
        {
            // The bootstrap worker keeps its Session and module reference until process exit, even after on_ready
            // reports failure, so hooks that did not retire stay valid. shutdown() logs that outcome itself.
            (void)OblivionLockpicking::shutdown();
        }
        return result;
    }
} // namespace

/**
 * @brief Production DLL entry point.
 * @param ul_reason_for_call The loader notification.
 * @param lp_reserved Null for an explicit FreeLibrary, non-null for process exit.
 * @return TRUE unless the attach gate refused this load.
 * @note The module handle is unused, because DetourModKit captures its own module from a code address.
 */
BOOL APIENTRY DllMain(HMODULE, DWORD ul_reason_for_call, LPVOID lp_reserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    {
        DMK::AsyncLoggerConfig async_cfg;
        // Detours share this logger. Queue overflow must never move file I/O onto a game callback.
        async_cfg.overflow_policy = DMK::OverflowPolicy::DropNewest;

        // No process-name gate: the WHGame.dll module check in init() is the gate.
        const DMK::ModInfo info{
            .name = OblivionLockpicking::constants::MOD_NAME,
            .log_file = OblivionLockpicking::constants::LOG_FILE_NAME,
            .game_process_name = "",
            .instance_mutex_prefix = OblivionLockpicking::constants::INSTANCE_MUTEX_PREFIX,
            .log = async_cfg,
            // The [file:line] stamp stays only on Trace, where a debug session reads it. The default Truncate open
            // mode is correct here, because this build is a single DLL with one Session per run.
            .log_source_stamp_mode = DMK::LogSourceStampMode::at_or_below(DMK::LogLevel::Trace),
        };

        // A gate refusal (wrong process, or a duplicate load that holds the mutex) is a reason for this DLL to go
        // away, not for the host to fail. A failed attach lets the loader unmap it.
        return DMK::bootstrap_attach(info, &on_ready).has_value() ? TRUE : FALSE;
    }

    case DLL_PROCESS_DETACH:
        // Loader-lock safe on both paths ([B-100]): it neither waits nor joins, and at process exit it abandons the
        // Session, whose threads the OS already ended.
        DMK::bootstrap_detach(lp_reserved);
        break;

    default:
        break;
    }

    return TRUE;
}
