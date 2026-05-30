/**
 * @file dllmain.cpp
 * @brief DLL entry point delegating the mod lifecycle to DMK::Bootstrap.
 *
 * Bootstrap owns the worker thread, the process/instance gate, logger
 * configuration, and the ordered DMK_Shutdown(); this file only wires the mod's
 * init() and shutdown() into the loader callbacks.
 */

#include "tpv_toggle.hpp"
#include "constants.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

// In the two-DLL dev build the logic is loaded by a thin loader ASI that owns the
// entry points (see src/dev/logic_exports.cpp). The production ASI uses DllMain.
#ifndef TPVTOGGLE_DEV_BUILD

namespace
{
    bool init_mod()
    {
        return TPVToggle::init();
    }

    void shutdown_mod()
    {
        TPVToggle::shutdown();
    }
} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
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

        return DMK::Bootstrap::on_dll_attach(hModule, info, &init_mod, &shutdown_mod);
    }

    case DLL_PROCESS_DETACH:
        DMK::Bootstrap::on_dll_detach(lpReserved != nullptr);
        break;

    default:
        break;
    }

    return TRUE;
}

#endif // TPVTOGGLE_DEV_BUILD
