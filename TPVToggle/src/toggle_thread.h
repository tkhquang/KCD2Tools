/**
 * @file toggle_thread.h
 * @brief Header for background key monitoring and game interaction thread.
 *
 * Declares data structures, thread entry function, view state manipulation
 * functions, and overlay state tracking boolean.
 */
#ifndef TOGGLE_THREAD_H
#define TOGGLE_THREAD_H

#include <windows.h>
#include <vector>
#include <string>
#include <cstdint> // For uintptr_t

// --- Global Pointers/Flags (Managed in dllmain.cpp) ---
extern "C"
{
    // Pointers to allocated storage for captured register values
    extern uintptr_t *g_r9_for_tpv_flag;
    extern uintptr_t *g_rbx_for_camera_distance;

    // Trampoline/Continuation Function Pointers
    extern void *fpTPV_OriginalCode;
    extern void *fpMenuOpen_OriginalCode;
    extern void *fpMenuClose_OriginalCode;
    extern void *fpCameraDistance_OriginalCode;

    // Global overlay active flag - directly accessible to assembly
    extern bool g_isOverlayActive;
    // Pointer to the flag (for compatibility with old code)
    extern bool *g_pIsOverlayActive;
}

// --- End Extern "C" ---

/**
 * @struct ToggleData
 * @brief Structure holding configured key codes passed to the toggle thread.
 */
struct ToggleData
{
    std::vector<int> toggle_keys;
    std::vector<int> fpv_keys;
    std::vector<int> tpv_keys;
};

// --- Thread Entry Point ---
DWORD WINAPI MonitorThread(LPVOID param);

// --- Game Interaction Functions (Prototypes) ---
bool safeToggleViewState(int *key_pressed_vk = nullptr);
bool setFirstPersonView(int *key_pressed_vk = nullptr);
bool setThirdPersonView(int *key_pressed_vk = nullptr);
int getViewState();

#endif // TOGGLE_THREAD_H
