/**
 * @file toggle_thread.h
 * @brief Header for background thread managing TPV toggle.
 */
#ifndef TOGGLE_THREAD_H
#define TOGGLE_THREAD_H

#include <windows.h>
#include <vector>
#include <atomic>

// Thread data structure
struct ToggleData
{
    std::vector<int> toggle_keys;
    std::vector<int> fpv_keys;
    std::vector<int> tpv_keys;
};

// Thread communication variables
extern std::atomic<bool> g_isOverlayActive;
extern std::atomic<bool> g_overlayFpvRequest;
extern std::atomic<bool> g_overlayTpvRestoreRequest;
extern std::atomic<bool> g_wasTpvBeforeOverlay;
extern std::atomic<bool> g_accumulatorWriteNOPped;

// Thread function prototype - only main monitor thread is used now
DWORD WINAPI MonitorThread(LPVOID param);

#endif // TOGGLE_THREAD_H
