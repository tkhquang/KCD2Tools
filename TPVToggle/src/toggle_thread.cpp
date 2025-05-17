/**
 * @file toggle_thread.cpp
 * @brief Implements background threads for TPV toggling and optional features.
 *
 * Manages the main hotkey monitoring thread that handles key input for toggling
 * between first-person and third-person view modes.
 */

#include "toggle_thread.h"
#include "logger.h"
#include "utils.h"
#include "constants.h"
#include "game_interface.h"
#include "global_state.h"
#include "config.h"
#include "hooks/ui_overlay_hooks.h"

#include <windows.h>
#include <vector>
#include <unordered_map>
#include <string>
#include <cmath>

// External config declaration
extern Config g_config;

/**
 * @brief Main hotkey monitoring thread.
 * @details Monitors for registered hotkeys and processes requests from overlay hooks.
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

    // Track hold key state for hold-to-scroll feature
    bool prevHoldKeyState = false;

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

                // Check if hold key state changed
                if (anyHoldKeyPressed != prevHoldKeyState)
                {
                    // Set the global flag and handle state change
                    g_holdToScrollActive.store(anyHoldKeyPressed, std::memory_order_relaxed);

                    // Call the UI overlay hook to handle the scroll state change
                    handleHoldToScrollKeyState(anyHoldKeyPressed);

                    prevHoldKeyState = anyHoldKeyPressed;
                }
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
