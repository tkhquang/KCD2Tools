/**
 * @file camera_profile_thread.h
 * @brief Header for camera profile offset adjustment thread.
 *
 * Edge-triggered profile actions (save, cycle, reset, etc.) are handled
 * by DMKInputManager press callbacks registered in dllmain.cpp.
 * This thread handles continuous offset adjustment via is_binding_active() queries.
 */
#ifndef CAMERA_PROFILE_THREAD_H
#define CAMERA_PROFILE_THREAD_H

#include <windows.h>

// Thread function prototype
DWORD WINAPI CameraProfileThread(LPVOID param);

// Helper structure to pass data to the thread
struct CameraProfileThreadData
{
    float adjustmentStep;
};

#endif // CAMERA_PROFILE_THREAD_H
