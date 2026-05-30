/**
 * @file dev/logic_exports.cpp
 * @brief Exported entry points for the two-DLL hot-reload dev build.
 *
 * @details Only compiled when TPVTOGGLE_DEV_BUILD is defined. In that build the
 *          mod ships as a thin loader ASI plus this logic DLL; the loader calls
 *          Init() after LoadLibrary and Shutdown() before FreeLibrary. Because
 *          there is no DMK::Bootstrap in this path, Init() configures the logger
 *          itself and Shutdown() runs DMK_Shutdown() so each reload starts from a
 *          clean DetourModKit state.
 */

#ifdef TPVTOGGLE_DEV_BUILD

#include "tpv_toggle.hpp"
#include "constants.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

extern "C" __declspec(dllexport) bool Init()
{
    DMK::Logger::configure(Constants::MOD_NAME, Constants::LOG_FILE_NAME, "%Y-%m-%d %H:%M:%S");

    DMK::AsyncLoggerConfig async_cfg;
    async_cfg.overflow_policy = DMK::OverflowPolicy::SyncFallback;
    DMK::Logger::get_instance().enable_async_mode(async_cfg);

    DMK::Logger::get_instance().info("[DEV] Logic DLL Init() called");

    if (!TPVToggle::init())
    {
        DMK::Logger::get_instance().error("[DEV] TPVToggle initialization FAILED");
        return false;
    }
    return true;
}

extern "C" __declspec(dllexport) void Shutdown()
{
    DMK::Logger::get_instance().info("[DEV] Logic DLL Shutdown() called");
    TPVToggle::shutdown();

    // Bootstrap is absent in the dev build, so tear down the DetourModKit
    // singletons here. DMK_Shutdown() is idempotent and re-initializable, which
    // is what makes the loader's FreeLibrary + reload cycle safe.
    DMK_Shutdown();
}

#endif // TPVTOGGLE_DEV_BUILD
