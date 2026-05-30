/**
 * @file toggle_thread.cpp
 * @brief Implements the overlay-driven view state change worker.
 *
 * Processes overlay FPV/TPV restore requests set by UI overlay hooks.
 * Key input monitoring is handled by DMK::InputManager callbacks registered
 * during input setup, so this worker only handles hook-driven state changes.
 */

#include "toggle_thread.hpp"
#include "config.hpp"
#include "constants.hpp"
#include "game_interface.hpp"
#include "global_state.hpp"
#include "utils.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

namespace TPVToggle
{

void overlay_monitor_body(std::stop_token st)
{
    DMK::Logger &logger = DMK::Logger::get_instance();
    logger.info("OverlayMonitor: Started");

    logger.info("OverlayMonitor: Waiting for game interface...");
    while (!getResolvedTpvFlagAddress())
    {
        if (!sleep_until_stop(st, 250))
        {
            logger.info("OverlayMonitor: Stopped before game interface was ready");
            return;
        }
    }
    logger.info("OverlayMonitor: Game interface ready");

    // Main loop: process overlay requests only.
    while (sleep_until_stop(st, Constants::MAIN_MONITOR_SLEEP_MS))
    {
        try
        {
            if (overlay_state().fpvRequest.load(std::memory_order_relaxed))
            {
                logger.debug("OverlayMonitor: Processing FPV request");
                (void)setViewState(0);
                overlay_state().fpvRequest.store(false, std::memory_order_relaxed);
            }

            if (overlay_state().tpvRestoreRequest.load(std::memory_order_relaxed))
            {
                logger.debug("OverlayMonitor: Processing TPV restore request");
                // Allow the UI to settle before restoring TPV. Wait on the stop
                // token so a shutdown during the (configurable, possibly large)
                // delay tears down promptly instead of blocking the join.
                const int restore_delay = settings().overlayRestoreDelayMs.load();
                if (restore_delay > 0 && !sleep_until_stop(st, static_cast<unsigned long>(restore_delay)))
                {
                    logger.info("OverlayMonitor: Stopped during TPV restore delay");
                    return;
                }
                (void)setViewState(1);
                overlay_state().tpvRestoreRequest.store(false, std::memory_order_relaxed);
            }
        }
        catch (const std::exception &e)
        {
            logger.error("OverlayMonitor: Error: {}", e.what());
            if (!sleep_until_stop(st, 1000))
                return;
        }
        catch (...)
        {
            logger.error("OverlayMonitor: Unknown error");
            if (!sleep_until_stop(st, 1000))
                return;
        }
    }

    logger.info("OverlayMonitor: Exiting");
}

} // namespace TPVToggle
