/**
 * @file toggle_thread.cpp
 * @brief Implements background threads for TPV toggling and optional features.
 *
 * Manages the main hotkey monitoring thread and the optional overlay monitoring thread.
 */

#include "toggle_thread.h"
#include "logger.h"
#include "utils.h"
#include "constants.h"
#include "game_interface.h"
#include "global_state.h"
#include "config.h"

#include <windows.h>
#include <vector>
#include <unordered_map>
#include <string>
#include <cmath>

// External config declaration
extern Config g_config;

/**
 * @brief Main hotkey monitoring thread.
 * @details Monitors for registered hotkeys and processes requests from overlay thread.
 */
DWORD WINAPI MonitorThread(LPVOID param)
{
    ToggleData *data_ptr = static_cast<ToggleData *>(param);
    if (!data_ptr)
    {
        Logger::getInstance().log(LOG_ERROR, "MonitorThread: NULL data received.");
        return 1;
    }

    // Move data and clean up
    std::vector<int> toggle_keys = std::move(data_ptr->toggle_keys);
    std::vector<int> fpv_keys = std::move(data_ptr->fpv_keys);
    std::vector<int> tpv_keys = std::move(data_ptr->tpv_keys);
    delete data_ptr;
    data_ptr = nullptr;

    Logger &logger = Logger::getInstance();
    logger.log(LOG_INFO, "MonitorThread: Started");

    // Initialize key tracking
    std::unordered_map<int, bool> key_down_states;
    bool hotkeys_active = false;

    auto initialize_keys = [&](const auto &keys)
    {
        for (int vk : keys)
        {
            if (vk != 0)
            {
                key_down_states[vk] = false;
                hotkeys_active = true;
            }
        }
    };

    initialize_keys(toggle_keys);
    initialize_keys(fpv_keys);
    initialize_keys(tpv_keys);

    // Also initialize hold-to-scroll keys if configured
    if (!g_config.hold_scroll_keys.empty())
    {
        initialize_keys(g_config.hold_scroll_keys);
    }

    logger.log(LOG_INFO, "MonitorThread: Hotkeys " + std::string(hotkeys_active ? "ENABLED" : "DISABLED"));

    // Wait for initialization
    logger.log(LOG_INFO, "MonitorThread: Waiting for game interface...");
    while (!getResolvedTpvFlagAddress() && WaitForSingleObject(g_exitEvent, 0) != WAIT_OBJECT_0)
    {
        Sleep(250);
    }
    logger.log(LOG_INFO, "MonitorThread: Game interface ready");

    // Main loop
    while (WaitForSingleObject(g_exitEvent, Constants::MAIN_MONITOR_SLEEP_MS) != WAIT_OBJECT_0)
    {
        try
        {
            // Process overlay requests
            if (g_overlayFpvRequest.load(std::memory_order_relaxed))
            {
                logger.log(LOG_DEBUG, "MonitorThread: Processing FPV request");
                if (setViewState(0))
                {
                    g_overlayFpvRequest.store(false, std::memory_order_relaxed);
                }
                else
                {
                    logger.log(LOG_ERROR, "MonitorThread: Failed to execute FPV request");
                    g_overlayFpvRequest.store(false, std::memory_order_relaxed);
                }
            }

            if (g_overlayTpvRestoreRequest.load(std::memory_order_relaxed))
            {
                logger.log(LOG_DEBUG, "MonitorThread: Processing TPV restore request");
                if (setViewState(1))
                {
                    g_overlayTpvRestoreRequest.store(false, std::memory_order_relaxed);
                }
                else
                {
                    logger.log(LOG_ERROR, "MonitorThread: Failed to execute TPV restore request");
                    g_overlayTpvRestoreRequest.store(false, std::memory_order_relaxed);
                }
            }

            // Process hotkeys if overlay is not active
            bool overlayActive = g_isOverlayActive.load(std::memory_order_relaxed);
            if (hotkeys_active && !overlayActive)
            {
                auto process_keys = [&](const auto &keys, auto callback)
                {
                    for (int vk : keys)
                    {
                        if (vk != 0)
                        {
                            bool pressed = (GetAsyncKeyState(vk) & 0x8000) != 0;
                            if (pressed && !key_down_states[vk])
                            {
                                callback(&vk);
                            }
                            key_down_states[vk] = pressed;
                        }
                    }
                };

                process_keys(toggle_keys, safeToggleViewState);
                process_keys(fpv_keys, [](int *vk)
                             { setViewState(0, vk); });
                process_keys(tpv_keys, [](int *vk)
                             { setViewState(1, vk); });
            }

            // Process hold-to-scroll keys if configured
            if (!g_config.hold_scroll_keys.empty())
            {
                bool anyHoldKeyPressed = false;

                for (int vk : g_config.hold_scroll_keys)
                {
                    if (vk != 0 && (GetAsyncKeyState(vk) & 0x8000) != 0)
                    {
                        anyHoldKeyPressed = true;
                        break;
                    }
                }

                // Set the global flag to be used by event handling
                g_holdToScrollActive.store(anyHoldKeyPressed, std::memory_order_relaxed);
            }
        }
        catch (const std::exception &e)
        {
            logger.log(LOG_ERROR, "MonitorThread: Error: " + std::string(e.what()));
            Sleep(1000);
        }
        catch (...)
        {
            logger.log(LOG_ERROR, "MonitorThread: Unknown error");
            Sleep(1000);
        }
    }

    logger.log(LOG_INFO, "MonitorThread: Exiting");
    return 0;
}

/**
 * @brief Overlay monitoring thread (optional feature).
 * @details Monitors overlay state and manages associated behaviors like NOP'ing scroll input.
 */
DWORD WINAPI OverlayMonitorThread(LPVOID param)
{
    (void)param;
    Logger &logger = Logger::getInstance();
    logger.log(LOG_INFO, "OverlayMonitor: Started");

    // NOP pattern for accumulator write
    const BYTE nopSequence[Constants::ACCUMULATOR_WRITE_INSTR_LENGTH] = {0x90, 0x90, 0x90, 0x90, 0x90};

    bool prevActiveState = false;
    long long initialOverlayState = getOverlayState();
    if (initialOverlayState != -1)
    {
        prevActiveState = (initialOverlayState > 0);
    }
    else
    {
        logger.log(LOG_DEBUG, "OverlayMonitor: Could not read initial overlay state, assuming INACTIVE");
    }
    g_isOverlayActive.store(prevActiveState);

    logger.log(LOG_INFO, "OverlayMonitor: Initial overlay state: " + std::string(prevActiveState ? "ACTIVE" : "INACTIVE"));

    // If hold-to-scroll feature is enabled, NOP by default
    if (!g_config.hold_scroll_keys.empty() && g_accumulatorWriteAddress)
    {
        logger.log(LOG_INFO, "OverlayMonitor: Hold-to-scroll feature enabled, applying NOP by default");
        if (WriteBytes(g_accumulatorWriteAddress, nopSequence, Constants::ACCUMULATOR_WRITE_INSTR_LENGTH, logger))
        {
            g_accumulatorWriteNOPped.store(true);
        }
    }

    while (WaitForSingleObject(g_exitEvent, Constants::OVERLAY_MONITOR_INTERVAL_MS) != WAIT_OBJECT_0)
    {
        try
        {
            // Always handle Hold-to-Scroll functionality if enabled
            if (!g_config.hold_scroll_keys.empty() && g_accumulatorWriteAddress)
            {
                bool isHoldActive = g_holdToScrollActive.load(std::memory_order_relaxed);

                // If hold key is pressed but accumulator is currently NOPped, restore original bytes
                if (isHoldActive && g_accumulatorWriteNOPped.load())
                {
                    if (WriteBytes(g_accumulatorWriteAddress, g_originalAccumulatorWriteBytes, Constants::ACCUMULATOR_WRITE_INSTR_LENGTH, logger))
                    {
                        g_accumulatorWriteNOPped.store(false);
                        logger.log(LOG_DEBUG, "OverlayMonitor: Restored accumulator write due to hold key press");
                    }
                }
                // If hold key is released but accumulator is not NOPped, NOP it
                else if (!isHoldActive && !g_accumulatorWriteNOPped.load() && !prevActiveState)
                {
                    if (WriteBytes(g_accumulatorWriteAddress, nopSequence, Constants::ACCUMULATOR_WRITE_INSTR_LENGTH, logger))
                    {
                        g_accumulatorWriteNOPped.store(true);
                        logger.log(LOG_DEBUG, "OverlayMonitor: NOPped accumulator write due to hold key release");
                    }
                }
            }

            // Next, handle overlay state changes
            long long overlayState = getOverlayState();
            if (overlayState != -1)
            {
                bool isActiveNow = (overlayState > 0);
                if (isActiveNow != prevActiveState)
                {
                    logger.log(LOG_INFO, "OverlayMonitor: State change -> " + std::string(isActiveNow ? "ACTIVE" : "INACTIVE"));
                    g_isOverlayActive.store(isActiveNow);
                    prevActiveState = isActiveNow;

                    if (isActiveNow)
                    {
                        // Overlay opened
                        int viewState = getViewState();
                        g_wasTpvBeforeOverlay.store(viewState == 1);
                        g_overlayFpvRequest.store(true);

                        // NOP the accumulator write if possible
                        if (g_accumulatorWriteAddress && !g_accumulatorWriteNOPped.load())
                        {
                            logger.log(LOG_DEBUG, "OverlayMonitor: NOPping accumulator write");
                            if (WriteBytes(g_accumulatorWriteAddress, nopSequence, Constants::ACCUMULATOR_WRITE_INSTR_LENGTH, logger))
                            {
                                g_accumulatorWriteNOPped.store(true);
                            }
                        }
                    }
                    else
                    {
                        // Overlay closed
                        Sleep(500); // Give UI time to settle

                        // If we're not using Hold-to-Scroll, restore accumulator write when overlay closes
                        if (g_config.hold_scroll_keys.empty())
                        {
                            // Restore accumulator write if it was NOPed
                            if (g_accumulatorWriteNOPped.load())
                            {
                                if (g_accumulatorWriteAddress && g_originalAccumulatorWriteBytes[0] != 0)
                                {
                                    logger.log(LOG_DEBUG, "OverlayMonitor: Restoring accumulator write");
                                    if (WriteBytes(g_accumulatorWriteAddress, g_originalAccumulatorWriteBytes, Constants::ACCUMULATOR_WRITE_INSTR_LENGTH, logger))
                                    {
                                        g_accumulatorWriteNOPped.store(false);
                                    }
                                }
                            }
                        }

                        // Restore TPV if needed
                        if (g_wasTpvBeforeOverlay.load())
                        {
                            g_overlayTpvRestoreRequest.store(true);
                        }
                        g_wasTpvBeforeOverlay.store(false);
                    }
                }
            }
        }
        catch (const std::exception &e)
        {
            logger.log(LOG_ERROR, "OverlayMonitor: Error: " + std::string(e.what()));
            Sleep(1000);
        }
        catch (...)
        {
            logger.log(LOG_ERROR, "OverlayMonitor: Unknown error");
            Sleep(1000);
        }
    }

    // Ensure accumulator write is restored on exit
    if (g_accumulatorWriteNOPped.load() && g_accumulatorWriteAddress && g_originalAccumulatorWriteBytes[0] != 0)
    {
        logger.log(LOG_INFO, "OverlayMonitor: Restoring accumulator write before exit");
        WriteBytes(g_accumulatorWriteAddress, g_originalAccumulatorWriteBytes, Constants::ACCUMULATOR_WRITE_INSTR_LENGTH, logger);
    }

    logger.log(LOG_INFO, "OverlayMonitor: Exiting");
    return 0;
}
