/**
 * @file toggle_thread.cpp
 * @brief Implementation of key monitoring and view toggling
 *
 * This file implements the thread that monitors keyboard input
 * and toggles the third-person view flag when configured keys are pressed.
 * It also provides functions for accessing and manipulating the view state.
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
 * Returns a pointer to the toggle_addr for use in other modules.
 */
volatile BYTE *getToggleAddr()
{
    return toggle_addr;
}

/**
 * Gets the current view state (0 for FPV, 1 for TPV).
 * Returns 0 (FPV) if the address is invalid or an exception occurs.
 */
BYTE getViewState()
{
    // Use a local variable to prevent race conditions
    volatile BYTE *current_addr = toggle_addr;

    if (current_addr == nullptr)
    {
        Logger::getInstance().log(LOG_ERROR, "Toggle: Attempted to get view state with null address");
        return 0; // Default to FPV on error
    }

    try
    {
        return *current_addr;
    }
    catch (...)
    {
        Logger::getInstance().log(LOG_ERROR, "Toggle: Exception when accessing memory at " +
                                                 format_address(reinterpret_cast<uintptr_t>(current_addr)));
        return 0; // Default to FPV on error
    }
}

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
 * Sets the view state to a specific value (0 for FPV, 1 for TPV)
 * Only changes if the current state is different
 */
bool setViewState(BYTE state)
{
    // Use a local variable to prevent race conditions
    volatile BYTE *current_addr = toggle_addr;

    if (current_addr == nullptr)
    {
        Logger::getInstance().log(LOG_ERROR, "Toggle: Attempted to set view state with null address");
        return false;
    }

    try
    {
        BYTE current_value = *current_addr;

        // Only change if the state is different
        if (current_value != state)
        {
            *current_addr = state;
            return true;
        }
        return false; // No change needed
    }
    catch (...)
    {
        Logger::getInstance().log(LOG_ERROR, "Toggle: Exception when accessing memory at " + format_address(reinterpret_cast<uintptr_t>(current_addr)));
        return false;
    }
}

/**
 * Sets the view to first-person (value 0)
 */
bool setFirstPersonView()
{
    return setViewState(0);
}

/**
 * Sets the view to third-person (value 1)
 */
bool setThirdPersonView()
{
    return setViewState(1);
}

/**
 * Thread function that monitors configured keys and changes the view state.
 * Handles three types of keys: toggle keys, FPV keys, and TPV keys.
 * Tracks key states to detect press events and debounces input.
 *
 * If all key vectors are empty, the thread will still run but won't monitor any keys,
 * effectively becoming a no-operation (noop) thread that consumes minimal resources.
 */
DWORD WINAPI ToggleThread(LPVOID param)
{
    ToggleData *data = static_cast<ToggleData *>(param);

    // Copy the key vectors for local use
    std::vector<int> toggle_keys = data->toggle_keys;
    std::vector<int> fpv_keys = data->fpv_keys;
    std::vector<int> tpv_keys = data->tpv_keys;

    // Clean up the parameter data
    delete data;

    Logger &logger = Logger::getInstance();

    // Wait for toggle address to be initialized by exception handler
    while (toggle_addr == nullptr)
    {
        Sleep(100);
    }

    logger.log(LOG_INFO, "Thread: Toggle thread started");

    // Check if all key vectors are empty - noop mode
    bool noopMode = toggle_keys.empty() && fpv_keys.empty() && tpv_keys.empty();

    if (noopMode)
    {
        logger.log(LOG_INFO, "Thread: No keys configured to monitor. Thread is in no-operation mode.");
        logger.log(LOG_INFO, "Thread: Mod is loaded and initialized, but no key monitoring will occur.");

        // Enter idle loop with minimal CPU usage
        // We keep the thread alive but essentially do nothing
        while (true)
        {
            Sleep(1000); // Sleep for a full second to minimize resource use
        }

        return 0;
    }

    // Log configured keys for each type
    if (!toggle_keys.empty())
    {
        std::string keys_str;
        for (int vk : toggle_keys)
        {
            if (!keys_str.empty())
                keys_str += ", ";
            keys_str += format_vkcode(vk);
        }
        logger.log(LOG_INFO, "Thread: Monitoring toggle keys: " + keys_str);
    }

    if (!fpv_keys.empty())
    {
        std::string keys_str;
        for (int vk : fpv_keys)
        {
            if (!keys_str.empty())
                keys_str += ", ";
            keys_str += format_vkcode(vk);
        }
        logger.log(LOG_INFO, "Thread: Monitoring FPV keys: " + keys_str);
    }

    if (!tpv_keys.empty())
    {
        std::string keys_str;
        for (int vk : tpv_keys)
        {
            if (!keys_str.empty())
                keys_str += ", ";
            keys_str += format_vkcode(vk);
        }
        logger.log(LOG_INFO, "Thread: Monitoring TPV keys: " + keys_str);
    }

    // Track key states to detect press events (not held)
    std::unordered_map<int, bool> key_states;

    while (true)
    {
        // Handle toggle keys (switch between FPV and TPV)
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
                    logger.log(LOG_INFO, "Action: Toggle key " + format_vkcode(vk) + " pressed, TPV: " + (value ? "ON" : "OFF"));
                }
            }

            key_states[vk] = isDown;
        }

        // Handle FPV keys (force first-person view)
        for (int vk : fpv_keys)
        {
            bool isDown = (GetAsyncKeyState(vk) & 0x8000) != 0;
            bool wasDown = key_states[vk];

            if (isDown && !wasDown)
            {
                // Key just pressed - set to first-person view
                if (setFirstPersonView())
                {
                    logger.log(LOG_INFO, "Action: FPV key " + format_vkcode(vk) + " pressed, TPV: OFF");
                }
            }

            key_states[vk] = isDown;
        }

        // Handle TPV keys (force third-person view)
        for (int vk : tpv_keys)
        {
            bool isDown = (GetAsyncKeyState(vk) & 0x8000) != 0;
            bool wasDown = key_states[vk];

            if (isDown && !wasDown)
            {
                // Key just pressed - set to third-person view
                if (setThirdPersonView())
                {
                    logger.log(LOG_INFO, "Action: TPV key " + format_vkcode(vk) + " pressed, TPV: ON");
                }
            }

            key_states[vk] = isDown;
        }

        // Short sleep to reduce CPU usage while polling
        // Use longer sleep if we're monitoring fewer keys to reduce CPU usage
        int sleepDuration = 20; // Default 20ms polling rate

        // If we're monitoring very few keys, we can sleep longer
        if (toggle_keys.size() + fpv_keys.size() + tpv_keys.size() <= 3)
        {
            sleepDuration = 30; // Slightly longer sleep for few keys
        }

        Sleep(sleepDuration);
    }

    return 0;
}
