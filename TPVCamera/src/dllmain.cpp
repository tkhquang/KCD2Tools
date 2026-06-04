/**
 * @file dllmain.cpp
 * @brief DLL entry point delegating the mod lifecycle to DMK::Bootstrap.
 *
 * Bootstrap owns the worker thread, the process/instance gate, logger
 * configuration, and the ordered DMK_Shutdown(); this file only wires the mod's
 * init() and shutdown() into the loader callbacks.
 */

#include "tpv_camera.hpp"
#include "constants.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

// In the two-DLL dev build the logic is loaded by a thin loader ASI that owns the
// entry points (see src/dev/logic_exports.cpp). The production ASI uses DllMain.
#ifndef TPVCAMERA_DEV_BUILD

namespace
{
    bool init_mod()
    {
        return TPVCamera::init();
    }

    void shutdown_mod()
    {
        TPVCamera::shutdown();
    }
} // namespace

BOOL APIENTRY DllMain(HMODULE h_module, DWORD ul_reason_for_call, LPVOID lp_reserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    {
        DMK::AsyncLoggerConfig async_cfg;
        // Fall back to synchronous logging if the async queue overflows so no
        // diagnostic line is lost during a burst (startup or teardown).
        async_cfg.overflow_policy = DMK::OverflowPolicy::SyncFallback;

        // No process-name gate: the WHGame.dll module check in init() is the gate.
        const DMK::Bootstrap::ModInfo info{
            Constants::MOD_NAME,
            Constants::LOG_FILE_NAME,
            "",
            Constants::INSTANCE_MUTEX_PREFIX,
            async_cfg,
        };

        return DMK::Bootstrap::on_dll_attach(h_module, info, &init_mod, &shutdown_mod);
    }

    case DLL_PROCESS_DETACH:
        DMK::Bootstrap::on_dll_detach(lp_reserved != nullptr);
        break;

    default:
        break;
    }

    return TRUE;
}

#endif // TPVCAMERA_DEV_BUILD
