/**
 * @file toggle_thread.h
 * @brief Header for background thread managing overlay-driven view state changes.
 */
#ifndef TOGGLE_THREAD_HPP
#define TOGGLE_THREAD_HPP

#include <windows.h>

// Thread function prototype - processes overlay requests only.
// Key input is handled by DMKInputManager callbacks; the overlay-driven view
// state atomics are declared in global_state.h.
DWORD WINAPI MonitorThread(LPVOID param);

#endif // TOGGLE_THREAD_HPP
