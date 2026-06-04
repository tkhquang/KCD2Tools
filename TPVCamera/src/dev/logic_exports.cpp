/**
 * @file dev/logic_exports.cpp
 * @brief Exported entry points for the two-DLL hot-reload dev build.
 *
 * @details Only compiled when TPVCAMERA_DEV_BUILD is defined. In that build the
 *          mod ships as a thin loader ASI plus this logic DLL; the loader calls
 *          Init() after LoadLibrary and Shutdown() before FreeLibrary. Because
 *          there is no DMK::Bootstrap in this path, Init() configures the logger
 *          itself and Shutdown() runs DMK_Shutdown() so each reload starts from a
 *          clean DetourModKit state.
 */

#ifdef TPVCAMERA_DEV_BUILD

#include "tpv_camera.hpp"
#include "constants.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

extern "C" __declspec(dllexport) bool Init() noexcept
{
    // The loader calls this through a C function pointer (bool(__cdecl *)()), so an exception must
    // never unwind across the boundary. Guard the whole body and return false on any failure.
    try
    {
        DMK::Logger::configure(Constants::MOD_NAME, Constants::LOG_FILE_NAME, "%Y-%m-%d %H:%M:%S");

        DMK::AsyncLoggerConfig async_cfg;
        async_cfg.overflow_policy = DMK::OverflowPolicy::SyncFallback;
        DMK::Logger::get_instance().enable_async_mode(async_cfg);

        DMK::Logger::get_instance().info("[DEV] Logic DLL Init() called");

        if (!TPVCamera::init())
        {
            DMK::Logger::get_instance().error("[DEV] TPVCamera initialization FAILED");
            return false;
        }
        return true;
    }
    catch (...)
    {
        // configure() may have thrown before the logger came up, so report through the OS channel.
        OutputDebugStringA("[KCD2_TPVCamera][DEV] Init() threw an exception; returning false\n");
        return false;
    }
}

extern "C" __declspec(dllexport) void Shutdown() noexcept
{
    // The loader calls this through a C function pointer (void(__cdecl *)()), so an exception must
    // never unwind across the boundary. Guard the whole body, matching Init().
    try
    {
        DMK::Logger::get_instance().info("[DEV] Logic DLL Shutdown() called");
        TPVCamera::shutdown();

        // Bootstrap is absent in the dev build, so tear down the DetourModKit
        // singletons here. DMK_Shutdown() is idempotent and re-initializable, which
        // is what makes the loader's FreeLibrary + reload cycle safe.
        DMK_Shutdown();
    }
    catch (...)
    {
        // The logger may be the thing that threw, so report through the OS channel.
        OutputDebugStringA("[KCD2_TPVCamera][DEV] Shutdown() threw an exception; swallowed\n");
    }
}

#endif // TPVCAMERA_DEV_BUILD
