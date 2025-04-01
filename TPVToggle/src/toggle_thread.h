#ifndef TOGGLE_THREAD_H
#define TOGGLE_THREAD_H

#include <windows.h>
#include <vector>

/**
 * @brief Data structure passed to the toggle thread
 *
 * Contains the lists of virtual key codes to monitor for different view modes
 */
struct ToggleData
{
    std::vector<int> toggle_keys; // Keys that toggle between FPV and TPV
    std::vector<int> fpv_keys;    // Keys that force first-person view
    std::vector<int> tpv_keys;    // Keys that force third-person view
};

/**
 * @brief Thread function that monitors keys and changes the view based on key type
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

/**
 * @brief Sets the view state to first-person (0)
 *
 * @return bool True if change was successful, false otherwise
 */
bool setFirstPersonView();

/**
 * @brief Sets the view state to third-person (1)
 *
 * @return bool True if change was successful, false otherwise
 */
bool setThirdPersonView();

#endif // TOGGLE_THREAD_H
