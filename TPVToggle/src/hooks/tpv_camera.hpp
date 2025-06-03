#ifndef TPV_CAMERA_HPP
#define TPV_CAMERA_HPP

#include <cstdint>

// Forward declaration if necessary, or include actual game headers if you have them
// For now, we'll use simplified forward declarations or uintptr_t.

namespace TpvCamera
{
    // Configuration variables that will be loaded from INI
    struct TpvConfig
    {
        bool enableTpv = true;
        float cameraDistance = 2.5f;
        float cameraHeightOffset = 0.5f; // Relative to head bone
        float cameraRightOffset = 0.3f;  // Relative to player's right
        bool toggleHotkeyPressed = false;
        int toggleTpvKey = 0x72; // VK_F3 by default
    };

    extern TpvConfig g_tpvConfig;

    bool initializeTpvCameraHooks(uintptr_t moduleBase, size_t moduleSize);
    void cleanupTpvCameraHooks();
    void updateTpvSettings(); // To be called if settings are changed at runtime (e.g., by hotkey)

} // namespace TpvCamera

#endif // TPV_CAMERA_HPP
