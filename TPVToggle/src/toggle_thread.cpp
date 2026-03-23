/**
 * @file toggle_thread.cpp
 * @brief Implements background thread for overlay-driven view state changes.
 *
 * Processes overlay FPV/TPV restore requests set by UI overlay hooks.
 * Key input monitoring is handled by DMKInputManager callbacks registered
 * in dllmain.cpp, so this thread only handles hook-driven state changes.
 */

#include "toggle_thread.h"
#include <DetourModKit.hpp>
#include "constants.h"
#include "game_interface.h"
#include "global_state.h"

#include <windows.h>

using DetourModKit::LogLevel;

/**
 * @brief Overlay request processing thread.
 * @details Waits for the game interface to be ready, then processes
 *          FPV/TPV restore requests from UI overlay hooks.
 */
DWORD WINAPI MonitorThread(LPVOID param)
{
    (void)param;
    DMKLogger &logger = DMKLogger::get_instance();
    logger.log(LogLevel::Info, "MonitorThread: Started");

    // Wait for game interface initialization
    logger.log(LogLevel::Info, "MonitorThread: Waiting for game interface...");
    while (!getResolvedTpvFlagAddress() && WaitForSingleObject(g_exitEvent, 0) != WAIT_OBJECT_0)
    {
        Sleep(250);
    }
    logger.log(LogLevel::Info, "MonitorThread: Game interface ready");

    // Main loop - process overlay requests only
    while (WaitForSingleObject(g_exitEvent, Constants::MAIN_MONITOR_SLEEP_MS) != WAIT_OBJECT_0)
    {
        try
        {
            // Process overlay FPV request
            if (g_overlayFpvRequest.load(std::memory_order_relaxed))
            {
                logger.log(LogLevel::Debug, "MonitorThread: Processing FPV request");
                setViewState(0);
                g_overlayFpvRequest.store(false, std::memory_order_relaxed);
            }

            // Process overlay TPV restore request
            if (g_overlayTpvRestoreRequest.load(std::memory_order_relaxed))
            {
                logger.log(LogLevel::Debug, "MonitorThread: Processing TPV restore request");
                // Allow UI to settle
                Sleep(200);
                setViewState(1);
                g_overlayTpvRestoreRequest.store(false, std::memory_order_relaxed);
            }
        }
        catch (const std::exception &e)
        {
            logger.log(LogLevel::Error, "MonitorThread: Error: " + std::string(e.what()));
            Sleep(1000);
        }
        catch (...)
        {
            logger.log(LogLevel::Error, "MonitorThread: Unknown error");
            Sleep(1000);
        }
    }

    logger.log(LogLevel::Info, "MonitorThread: Exiting");
    return 0;
}
