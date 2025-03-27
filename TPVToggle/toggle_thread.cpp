/**
 * @file toggle_thread.cpp
 * @brief Implementation of key monitoring and view toggling
 *
 * This file implements the thread that monitors keyboard input
 * and toggles the third-person view flag when configured keys are pressed.
 */

#include "toggle_thread.h"
#include "logger.h"
#include "utils.h"
#include "constants.h"

#include <windows.h>
#include <vector>
#include <unordered_map>
#include <sstream>

extern volatile BYTE *toggle_addr;

/**
 * Safely toggles the third-person view state with error handling.
 * Handles potential memory access exceptions and null pointer checks.
 */
bool safeToggleViewState()
{
    // Use a local variable to prevent race conditions
    volatile BYTE *current_addr = toggle_addr;

    if (current_addr == nullptr)
    {
        Logger::getInstance().log(LOG_ERROR, "Toggle: Attempted to toggle with null address");
        return false;
    }

    // Use structured exception handling to catch access violations
    try
    {
        BYTE current_value = *current_addr;
        BYTE new_value = (current_value == 0) ? 1 : 0;
        *current_addr = new_value;
        return true;
    }
    catch (...)
    {
        Logger::getInstance().log(LOG_ERROR, "Toggle: Exception when accessing memory at " + format_address(reinterpret_cast<uintptr_t>(current_addr)));
        return false;
    }
}

/**
 * Thread function that monitors configured keys and toggles the view.
 * Tracks key states to detect press events and debounces input.
 */
DWORD WINAPI ToggleThread(LPVOID param)
{
    ToggleData *data = static_cast<ToggleData *>(param);
    std::vector<int> toggle_keys = data->toggle_keys;
    delete data;

    Logger &logger = Logger::getInstance();

    // Wait for toggle address to be initialized by exception handler
    while (toggle_addr == nullptr)
    {
        Sleep(100);
    }

    logger.log(LOG_INFO, "Thread: Toggle thread started");

    // Track key states to detect press events (not held)
    std::unordered_map<int, bool> key_states;

    while (true)
    {
        for (int vk : toggle_keys)
        {
            bool isDown = (GetAsyncKeyState(vk) & 0x8000) != 0;
            bool wasDown = key_states[vk];

            if (isDown && !wasDown)
            {
                // Key just pressed (not held) - toggle the view state
                if (safeToggleViewState())
                {
                    BYTE value = *toggle_addr;
                    logger.log(LOG_INFO, "Action: Key " + format_vkcode(vk) + " pressed, TPV: " + (value ? "ON" : "OFF"));
                }
            }

            key_states[vk] = isDown;
        }

        // Short sleep to reduce CPU usage while polling
        Sleep(20);
    }

    return 0;
}
