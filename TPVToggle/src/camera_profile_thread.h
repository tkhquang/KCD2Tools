#ifndef CAMERA_PROFILE_THREAD_H
#define CAMERA_PROFILE_THREAD_H

#include <windows.h>

// Thread function prototype
DWORD WINAPI CameraProfileThread(LPVOID param);

// Helper structure to pass data to the thread
struct CameraProfileThreadData
{
    float adjustmentStep;
    // Keys are already stored in Config and accessible via g_config
};

#endif // CAMERA_PROFILE_THREAD_H
