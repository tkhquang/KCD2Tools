/**
 * @file toggle_thread.h
 * @brief Thread for monitoring toggle keys
 *
 * This file defines the thread function that monitors keyboard input
 * and toggles the third-person view when configured keys are pressed.
 */

#ifndef TOGGLE_THREAD_H
#define TOGGLE_THREAD_H

#include <windows.h>
#include <vector>

/**
 * @brief Data structure passed to the toggle thread
 *
 * Contains the list of virtual key codes to monitor for toggling
 * the third-person view.
 */
struct ToggleData
{
    std::vector<int> toggle_keys; // Keys to check for toggling
};

/**
 * @brief Thread function that monitors keys and toggles the view
 *
 * @param param Pointer to a ToggleData structure
 * @return DWORD Thread exit code
 */
DWORD WINAPI ToggleThread(LPVOID param);

/**
 * @brief Safely toggles the view state with error handling
 *
 * @return bool True if toggle was successful, false otherwise
 */
bool safeToggleViewState();

#endif // TOGGLE_THREAD_H
