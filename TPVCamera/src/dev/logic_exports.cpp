/**
 * @file dev/logic_exports.cpp
 * @brief Exported entry points for the two-DLL hot-reload dev build.
 *
 * @details Only compiled when TPVCAMERA_DEV_BUILD is defined. In that build the
 *          mod ships as a thin loader ASI plus this logic DLL; the loader calls
 *          Init() after LoadLibrary and Shutdown() before FreeLibrary. There is no
 *          DllMain bootstrap on this path, so Init() owns the Session directly via
 *          the synchronous Session::start() factory and Shutdown() drops it.
 *
 *          Shutdown() returns whether the module may be unmapped, and that verdict has two
 *          halves. Every hooked prologue must have been restored (a pinned backend holds a
 *          counted reference on this module, and unloading past it makes the next
 *          LoadLibrary return the stale image), AND DMK's own safe-unload drain must return
 *          LogicDllUnloadStatus::SafeToUnload, which is what certifies that no input or config
 *          callback body living in this module can still be entered. This build owns its own
 *          Session, so ~Session performs the ordered teardown of the process-wide subsystems
 *          after the drain.
 */

#ifdef TPVCAMERA_DEV_BUILD

#include "tpv_camera.hpp"
#include "constants.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <optional>

namespace
{
    // The dev host owns its Session outright: no bootstrap worker, so ~Session runs here, on the
    // loader's reload thread, which is already off the loader lock.
    std::optional<DMK::Session> s_session;
} // namespace

extern "C" __declspec(dllexport) bool Init() noexcept
{
    // The loader calls this through a C function pointer (bool(__cdecl *)()), so an exception must
    // never unwind across the boundary. Guard the whole body and return false on any failure.
    try
    {
        DMK::AsyncLoggerConfig async_cfg;
        async_cfg.overflow_policy = DMK::OverflowPolicy::SyncFallback;

        // Session::start configures the logger from ModInfo, so no separate configure call is needed.
        auto opened = DMK::Session::start(DMK::ModInfo{
            .name = Constants::MOD_NAME,
            .log_file = Constants::LOG_FILE_NAME,
            .game_process_name = "",
            .instance_mutex_prefix = Constants::INSTANCE_MUTEX_PREFIX,
            .log = async_cfg,
        });
        if (!opened.has_value())
        {
            OutputDebugStringA("[KCD2_TPVCamera][DEV] Session::start failed; returning false\n");
            return false;
        }
        s_session.emplace(std::move(*opened));

        DMK::log().info("[DEV] Logic DLL Init() called");

        if (auto ready = TPVCamera::init(*s_session); !ready.has_value())
        {
            DMK::log().error("[DEV] TPVCamera initialization FAILED ({})", ready.error().message());
            s_session.reset();
            return false;
        }
        return true;
    }
    catch (...)
    {
        // The logger may not have come up yet, so report through the OS channel.
        OutputDebugStringA("[KCD2_TPVCamera][DEV] Init() threw an exception; returning false\n");
        return false;
    }
}

extern "C" __declspec(dllexport) bool Shutdown() noexcept
{
    // The loader calls this through a C function pointer (bool(__cdecl *)()), so an exception must
    // never unwind across the boundary. Guard the whole body, matching Init().
    try
    {
        DMK::log().info("[DEV] Logic DLL Shutdown() called");

        // Mod teardown first, while this module's code pages are still mapped. It joins the overlay
        // thread, then removes every hook and reports whether each prologue was restored. A hook that
        // could not restore pins its backend, which holds a counted reference on this module; unloading
        // past that keeps the hook installed and makes the next LoadLibrary return the stale image.
        const bool prologues_restored = TPVCamera::shutdown();

        // Then the DMK safe-unload drain, which is the library's authorization to unmap a Logic DLL. It runs
        // after the consumer-owned workers are stopped and the hook handles are dropped (shutdown() above did
        // both), on this reload thread, which is off the loader lock. The drain retires every input binding and
        // config setter DMK still owns, delivers a held hold-combo's balancing edge while this module's code is
        // still mapped, and destroys the gate-owned callables, so no callback body can be entered afterwards.
        // Only SafeToUnload authorizes FreeLibrary; every other status is a refusal (LoaderLock, SelfDelivery,
        // InProgress, RetireFailed, TimedOut) and none of them can be observed by waiting a fixed interval.
        const DMK::LogicDllUnloadStatus drain = DMK::prepare_logic_dll_unload_all();
        const bool drained = drain == DMK::LogicDllUnloadStatus::SafeToUnload;
        if (!drained)
        {
            DMK::log().error("[DEV] Logic DLL drain refused unload (status {}); module stays mapped",
                             static_cast<int>(drain));
        }

        // Drop the Session last. It clears the binding scope and shuts the library subsystems down in
        // order, input included. The drain already destroyed the callables the scope's guards reach, so this
        // clear runs no consumer callback. Off the loader lock, so every subsystem joins cleanly rather than
        // detaching.
        s_session.reset();

        if (!prologues_restored)
        {
            // The logger belongs to the Session that was just dropped, so report through the OS channel.
            OutputDebugStringA("[KCD2_TPVCamera][DEV] unload REFUSED: a hooked prologue was not restored; "
                               "module stays mapped\n");
        }
        return prologues_restored && drained;
    }
    catch (...)
    {
        OutputDebugStringA("[KCD2_TPVCamera][DEV] Shutdown() threw an exception; refusing unload\n");
        return false;
    }
}

#endif // TPVCAMERA_DEV_BUILD
