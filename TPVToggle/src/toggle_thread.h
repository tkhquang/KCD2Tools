/**
 * @file toggle_thread.h
 * @brief Header for background thread managing overlay-driven view state changes.
 */
#ifndef TOGGLE_THREAD_H
#define TOGGLE_THREAD_H

#include <windows.h>
#include <atomic>

// Thread communication variables
extern std::atomic<bool> g_isOverlayActive;
extern std::atomic<bool> g_overlayFpvRequest;
extern std::atomic<bool> g_overlayTpvRestoreRequest;
extern std::atomic<bool> g_wasTpvBeforeOverlay;
extern std::atomic<bool> g_accumulatorWriteNOPped;

// Thread function prototype - processes overlay requests only
// Key input is handled by DMKInputManager callbacks
DWORD WINAPI MonitorThread(LPVOID param);

#endif // TOGGLE_THREAD_H
