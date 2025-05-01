/**
 * @file hooks/overlay_hook.h
 * @brief Header for overlay detection hook functionality.
 *
 * Provides functions to initialize and manage the hook that captures
 * the UI module pointer (RBX) during overlay checks.
 */
#ifndef OVERLAY_HOOK_H
#define OVERLAY_HOOK_H

#include <windows.h>
#include <cstdint>

extern "C"
{
    // Storage for the captured RBX value from overlay check
    extern uintptr_t *g_rbx_for_overlay_flag;

    // Trampoline for original code continuation
    extern void *fpOverlay_OriginalCode;

    // Assembly detour function
    void Overlay_CaptureRBX_Detour();
}

/**
 * @brief Initialize the overlay detection hook.
 * @param module_base Base address of the target game module.
 * @param module_size Size of the target game module in bytes.
 * @return true if initialization successful, false otherwise.
 */
bool initializeOverlayHook(uintptr_t module_base, size_t module_size);

/**
 * @brief Clean up overlay hook resources.
 */
void cleanupOverlayHook();

/**
 * @brief Check if overlay hook is active and ready.
 * @return true if hook is successfully installed and functional.
 */
bool isOverlayHookActive();

#endif // OVERLAY_HOOK_H
