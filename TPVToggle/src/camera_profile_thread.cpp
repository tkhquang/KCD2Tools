#include "camera_profile_thread.h"
#include "camera_profile.h"
#include "logger.h"
#include "utils.h"
#include "global_state.h"
#include "config.h"
#include "hooks/tpv_input_hook.h"

#include <unordered_map>
#include <bitset>
#include <vector>
#include <algorithm>
#include <chrono>
#include <iomanip>

// External config reference
extern Config g_config;

/**
 * @brief Structure to efficiently map VK codes to bit positions for batch processing
 */
struct KeyBitMapInfo
{
    std::vector<int> allKeys;               // All registered keys in monitoring order
    std::unordered_map<int, size_t> keyMap; // Maps VK code -> bit position (0 to 63)
    size_t keyCount = 0;                    // Number of unique keys mapped

    // Bitmasks for different key groups
    uint64_t masterToggleMask = 0;
    uint64_t profileSaveMask = 0; // Create New
    uint64_t profileCycleMask = 0;
    uint64_t profileResetMask = 0;
    uint64_t profileUpdateMask = 0;
    uint64_t profileDeleteMask = 0;
    uint64_t offsetXIncMask = 0;
    uint64_t offsetXDecMask = 0;
    uint64_t offsetYIncMask = 0;
    uint64_t offsetYDecMask = 0;
    uint64_t offsetZIncMask = 0;
    uint64_t offsetZDecMask = 0;

    uint64_t orbitalModeToggleMask = 0;

    uint64_t nativeOrbitTestToggleMask = 0;
};

/**
 * @brief Create a bitmap mapping for all registered keys
 * @param config The global configuration with key settings
 * @return KeyBitMapInfo structure with mappings and masks
 */
KeyBitMapInfo createKeyBitMap(const Config &config)
{
    KeyBitMapInfo info;
    info.keyCount = 0; // Reset key count

    // Helper to register keys and update masks
    auto registerKeys = [&](const std::vector<int> &keys, uint64_t &mask)
    {
        for (int vk : keys)
        {
            if (vk != 0)
            {
                // Check if key already registered and if we have space in the 64-bit map
                if (info.keyMap.find(vk) == info.keyMap.end())
                {
                    if (info.keyCount >= 64)
                    {
                        // Log error or warning: exceeded 64 unique hotkeys
                        Logger::getInstance().log(LOG_ERROR, "CameraProfileThread: Exceeded maximum unique hotkeys (64). Key " + format_vkcode(vk) + " ignored.");
                        continue; // Skip this key
                    }
                    info.keyMap[vk] = info.keyCount; // Assign current bit position
                    info.allKeys.push_back(vk);
                    info.keyCount++;
                }
                // Set the corresponding bit in the specific action mask
                mask |= (1ULL << info.keyMap[vk]);
            }
        }
    };

    // Register all key groups from config, including new ones
    registerKeys(config.master_toggle_keys, info.masterToggleMask);
    registerKeys(config.profile_save_keys, info.profileSaveMask); // Create New
    registerKeys(config.profile_cycle_keys, info.profileCycleMask);
    registerKeys(config.profile_reset_keys, info.profileResetMask);
    registerKeys(config.profile_update_keys, info.profileUpdateMask);
    registerKeys(config.profile_delete_keys, info.profileDeleteMask);
    registerKeys(config.offset_x_inc_keys, info.offsetXIncMask);
    registerKeys(config.offset_x_dec_keys, info.offsetXDecMask);
    registerKeys(config.offset_y_inc_keys, info.offsetYIncMask);
    registerKeys(config.offset_y_dec_keys, info.offsetYDecMask);
    registerKeys(config.offset_z_inc_keys, info.offsetZIncMask);
    registerKeys(config.offset_z_dec_keys, info.offsetZDecMask);

    registerKeys(config.orbital_mode_toggle_keys, info.orbitalModeToggleMask);

    std::vector<int> native_orbit_test_keys = {VK_F2};
    registerKeys(native_orbit_test_keys, info.nativeOrbitTestToggleMask);

    return info;
}

/**
 * @brief Get current state of all registered keys as a bitmap
 * @param keys Vector of VK codes to check
 * @param keyMap Mapping of VK codes to bit positions
 * @return 64-bit bitmap where each bit represents a key state (1=pressed)
 */
uint64_t getKeyStateBitmap(const std::vector<int> &keys, const std::unordered_map<int, size_t> &keyMap)
{
    uint64_t stateBitmap = 0;

    for (int vk : keys)
    {
        if (vk != 0)
        {
            auto it = keyMap.find(vk);
            if (it != keyMap.end())
            {
                // GetAsyncKeyState returns non-zero if pressed; highest bit indicates currently down.
                bool pressed = (GetAsyncKeyState(vk) & 0x8000) != 0;
                if (pressed)
                {
                    // Set the corresponding bit if key is down
                    stateBitmap |= (1ULL << it->second);
                }
            }
        }
    }

    return stateBitmap;
}

/**
 * @brief Compare current key state bitmap with previous to detect new key presses
 * @param currentState Current key state bitmap
 * @param previousState Previous key state bitmap
 * @param actionMask Bitmap mask for the specific key group
 * @return true if any key in the group was newly pressed
 */
bool isNewKeyPress(uint64_t currentState, uint64_t previousState, uint64_t actionMask)
{
    // A key is newly pressed if it's set in current state but not in previous state
    uint64_t newPresses = (currentState & ~previousState) & actionMask;
    return newPresses != 0;
}

/**
 * @brief Camera profile thread entry point - optimized with batch key checking
 */
DWORD WINAPI CameraProfileThread(LPVOID param)
{
    Logger &logger = Logger::getInstance();
    logger.log(LOG_INFO, "CameraProfileThread: Started");

    // Read thread parameters (adjustment step)
    CameraProfileThreadData *data = static_cast<CameraProfileThreadData *>(param);
    if (!data)
    {
        logger.log(LOG_ERROR, "CameraProfileThread: NULL data received");
        return 1;
    }
    float adjustmentStep = data->adjustmentStep;
    delete data; // Clean up thread data structure

    // Create key mappings based on loaded config
    KeyBitMapInfo keyInfo = createKeyBitMap(g_config);

    // Initialize key state tracker (all keys initially up)
    uint64_t previousKeyState = 0;

    if (logger.isDebugEnabled())
    {
        logger.log(LOG_DEBUG, "CameraProfileThread: Registered " + std::to_string(keyInfo.keyCount) + " unique keys for monitoring.");
    }

    typedef uintptr_t (*GetSystemInterfaceFunc)();
    uintptr_t funcAddr = g_ModuleBase + (0xA27DE0);
    GetSystemInterfaceFunc pfnGetSystemInterface = (GetSystemInterfaceFunc)funcAddr;

    logger.log(LOG_INFO, "CameraProfileThread: Attempting to acquire CVar struct address...");
    int cvar_init_attempts = 0;
    while (g_GlobalGameCVarsStructAddr == 0 && cvar_init_attempts < 60 && WaitForSingleObject(g_exitEvent, 0) != WAIT_OBJECT_0)
    { // Try for up to 60 seconds
        // (Copy the logic to call pfnGetSystemInterface and read pSystemInterface + 0x20 here)
        // Make sure to use g_ModuleBase for calculating funcAddr
        uintptr_t func_180a27de0_offset = 0xA27DE0; // VERIFY THIS OFFSET
        uintptr_t funcAddr = g_ModuleBase + func_180a27de0_offset;
        GetSystemInterfaceFunc pfnGetSystemInterface = (GetSystemInterfaceFunc)funcAddr;
        uintptr_t pSystemInterface_temp = 0;

        if (isMemoryReadable((void *)funcAddr, 8))
        {
            try
            {
                pSystemInterface_temp = pfnGetSystemInterface();
            }
            catch (...)
            {
                pSystemInterface_temp = 0;
            }
        }

        if (pSystemInterface_temp != 0 && isMemoryReadable((void *)(pSystemInterface_temp + 0x20), sizeof(uintptr_t)))
        {
            g_GlobalGameCVarsStructAddr = *(uintptr_t *)(pSystemInterface_temp + 0x20);
            if (g_GlobalGameCVarsStructAddr != 0)
            {
                logger.log(LOG_INFO, "CameraProfileThread: Successfully acquired pSystemInterface: " + format_address(pSystemInterface_temp));
                logger.log(LOG_INFO, "CameraProfileThread: Successfully acquired g_GlobalGameCVarsStructAddr: " + format_address(g_GlobalGameCVarsStructAddr));
                break; // Success
            }
        }
        cvar_init_attempts++;
        Sleep(1000); // Wait 1 second before retrying
    }
    if (g_GlobalGameCVarsStructAddr == 0)
    {
        logger.log(LOG_ERROR, "CameraProfileThread: FAILED to acquire CVar struct address after multiple attempts.");
    }

    // Main loop
    while (WaitForSingleObject(g_exitEvent, 16) != WAIT_OBJECT_0) // ~60 Hz check
    {
        try
        {
            // Get current state of all monitored keys
            uint64_t currentKeyState = getKeyStateBitmap(keyInfo.allKeys, keyInfo.keyMap);

            static bool nativeOrbitCVarsEnabled = false; // Track state

            if (isNewKeyPress(currentKeyState, previousKeyState, keyInfo.nativeOrbitTestToggleMask))
            {
                if (g_GlobalGameCVarsStructAddr != 0)
                {
                    nativeOrbitCVarsEnabled = !nativeOrbitCVarsEnabled;

                    bool *p_cl_cam_debug = (bool *)(g_GlobalGameCVarsStructAddr + 0x1C0); // cl_cam_debug
                    bool *p_cl_cam_orbit = (bool *)(g_GlobalGameCVarsStructAddr + 0x1A8); // cl_cam_orbit

                    if (isMemoryWritable(p_cl_cam_debug, sizeof(bool)) && isMemoryWritable(p_cl_cam_orbit, sizeof(bool)))
                    {
                        *p_cl_cam_debug = nativeOrbitCVarsEnabled; // Enable debug cam features
                        *p_cl_cam_orbit = nativeOrbitCVarsEnabled; // Enable orbit mode specifically

                        logger.log(LOG_INFO, "Native Orbit Test: cl_cam_debug set to " + std::string(nativeOrbitCVarsEnabled ? "true" : "false"));
                        logger.log(LOG_INFO, "Native Orbit Test: cl_cam_orbit set to " + std::string(nativeOrbitCVarsEnabled ? "true" : "false"));

                        if (nativeOrbitCVarsEnabled)
                        {
                            // Optional: Set some default distances/offsets
                            float *p_dist = (float *)(g_GlobalGameCVarsStructAddr + 0x1BC); // cl_cam_orbit_distance
                            float *p_offX = (float *)(g_GlobalGameCVarsStructAddr + 0x1B4); // cl_cam_orbit_offsetX
                            float *p_offZ = (float *)(g_GlobalGameCVarsStructAddr + 0x1B8); // cl_cam_orbit_offsetZ
                            if (isMemoryWritable(p_dist, sizeof(float)))
                                *p_dist = 5.0f;
                            if (isMemoryWritable(p_offX, sizeof(float)))
                                *p_offX = 0.5f;
                            if (isMemoryWritable(p_offZ, sizeof(float)))
                                *p_offZ = 1.5f;
                            logger.log(LOG_INFO, "Native Orbit Test: Set default distance/offsets.");
                        }
                    }
                    else
                    {
                        logger.log(LOG_ERROR, "Native Orbit Test: Cannot write to CVar memory locations!");
                    }
                }
                else
                {
                    logger.log(LOG_ERROR, "Native Orbit Test: g_GlobalGameCVarsStructAddr is NULL!");
                }
            }

            // --- Process Keys ---

            // Master toggle: Always check regardless of adjustment mode
            if (isNewKeyPress(currentKeyState, previousKeyState, keyInfo.masterToggleMask))
            {
                bool newMode = !g_cameraAdjustmentMode.load();
                g_cameraAdjustmentMode.store(newMode);
                logger.log(LOG_INFO, "CameraProfileThread: Adjustment mode " + std::string(newMode ? "ENABLED" : "DISABLED"));
            }

            // Check other keys only if adjustment mode is enabled
            if (g_cameraAdjustmentMode.load())
            {
                // 1. CREATE NEW Profile key (e.g., Numpad 1)
                if (isNewKeyPress(currentKeyState, previousKeyState, keyInfo.profileSaveMask))
                {
                    logger.log(LOG_DEBUG, "CameraProfileThread: Create New Profile key press detected.");
                    CameraProfileManager::getInstance().createNewProfileFromLiveState("General");
                }

                // 2. UPDATE ACTIVE Profile key (e.g., Numpad 7)
                if (isNewKeyPress(currentKeyState, previousKeyState, keyInfo.profileUpdateMask))
                {
                    logger.log(LOG_DEBUG, "CameraProfileThread: Update Active Profile key press detected.");
                    // This function internally checks if active is "Default" and logs a warning if so.
                    CameraProfileManager::getInstance().updateActiveProfileWithLiveState();
                }

                // 3. DELETE ACTIVE Profile key (e.g., Numpad 9)
                if (isNewKeyPress(currentKeyState, previousKeyState, keyInfo.profileDeleteMask))
                {
                    logger.log(LOG_DEBUG, "CameraProfileThread: Delete Active Profile key press detected.");
                    // This function internally checks if active is "Default" and prevents deletion if so.
                    CameraProfileManager::getInstance().deleteActiveProfile();
                }

                // 4. Cycle Profiles key (e.g., Numpad 3)
                if (isNewKeyPress(currentKeyState, previousKeyState, keyInfo.profileCycleMask))
                {
                    logger.log(LOG_DEBUG, "CameraProfileThread: Cycle Profiles key press detected.");
                    CameraProfileManager::getInstance().cycleToNextProfile();
                }

                // 5. Reset to Default key (e.g., Numpad 5)
                if (isNewKeyPress(currentKeyState, previousKeyState, keyInfo.profileResetMask))
                {
                    logger.log(LOG_DEBUG, "CameraProfileThread: Reset to Default key press detected.");
                    CameraProfileManager::getInstance().resetToDefault();
                }

                if (isNewKeyPress(currentKeyState, previousKeyState, keyInfo.orbitalModeToggleMask))
                {
                    // Only if feature itself is enabled in config
                    if (g_config.enable_orbital_camera_mode)
                    {
                        logger.log(LOG_INFO, "CPT: Orbital Mode Toggle Key Press DETECTED! (Mask: " + std::to_string(keyInfo.orbitalModeToggleMask) + ")");
                        bool newOrbitalMode = !g_orbitalModeActive.load();
                        g_orbitalModeActive.store(newOrbitalMode);
                        logger.log(LOG_INFO, "Orbital Camera Mode " + std::string(newOrbitalMode ? "ENABLED" : "DISABLED"));
                        if (newOrbitalMode)
                        {
                            logger.log(LOG_INFO, "Orbital Camera Mode ENABLED");
                            // Initialize orbital angles from current game TPV camera to prevent jump
                            uintptr_t tpvCamObjAddr = 0; // How to get this here? See below.

                            // Method 1: Expose a function from tpv_input_hook.cpp or game_interface.cpp
                            // that can read the current C_CameraThirdPerson instance's quaternion.
                            // This is cleaner but requires passing 'thisPtr' around or storing it globally (carefully).
                            // Quaternion currentGameTpvQuat = getCurrentGameTpvQuaternion(); // Needs implementation

                            // Method 2: If difficult to get live 'thisPtr' here,
                            // the jump might be unavoidable initially, or smooth in via TransitionManager.
                            // For now, let's assume you might need to read it before Detour_TpvInputProcess starts suppressing.
                            // A simple approach might be to just use the LAST known g_latestTpvCameraForward to approximate.
                            // Not perfect for full quaternion.

                            // Ideal (if you can get currentGameTpvQuat somehow):
                            // Quaternion currentGameTpvQuat = ... ;
                            // Convert currentGameTpvQuat to Euler (yaw, pitch) -> update g_orbitalCameraYaw, g_orbitalCameraPitch
                            // DirectX::XMFLOAT4 current بازی_quat_xmfloat4 = { currentGameTpvQuat.x, currentGameTpvQuat.y, currentGameTpvQuat.z, currentGameTpvQuat.w };
                            // DirectX::XMFLOAT3 eulerAngles; // Will hold pitch, yaw, roll
                            // DirectX::XMStoreFloat3(&eulerAngles, DirectX::XMQuaternionToEulerAngles(XMLoadFloat4(¤t_game_quat_xmfloat4)));
                            // g_orbitalCameraPitch = eulerAngles.x; // DirectX order might be X=Pitch, Y=Yaw, Z=Roll
                            // g_orbitalCameraYaw = eulerAngles.y;
                            // g_orbitalCameraRoll = eulerAngles.z; // If you use roll

                            // Simpler for now (might still jump a bit but better than pure 0,0):
                            // If g_latestTpvCameraForward is somewhat fresh from non-orbital mode
                            if (g_latestTpvCameraForward.MagnitudeSquared() > 0.1f)
                            {
                                // Approximate yaw from forward vector's XZ components
                                g_orbitalCameraYaw = atan2f(g_latestTpvCameraForward.x, g_latestTpvCameraForward.y); // If Y is forward, X is right
                                // Approximate pitch from forward vector's Y (or Z) and its XZ magnitude
                                float xz_dist = sqrtf(g_latestTpvCameraForward.x * g_latestTpvCameraForward.x + g_latestTpvCameraForward.y * g_latestTpvCameraForward.y);
                                g_orbitalCameraPitch = atan2f(-g_latestTpvCameraForward.z, xz_dist);
                            }
                            else
                            {
                                // Fallback if no good previous camera state
                                g_orbitalCameraYaw = 0.0f; // Or read from player orientation
                                g_orbitalCameraPitch = 0.0f;
                            }
                            update_orbital_camera_rotation_from_euler(); // Important to calculate initial g_orbitalCameraRotation
                        }
                        else
                        {
                            logger.log(LOG_INFO, "Orbital Camera Mode DISABLED");
                        }
                    }
                }

                // --- Continuous Adjustment Handling (Check current state, not just new presses) ---
                // Check adjustment keys only if they exist in the map (non-zero mask)
                if (keyInfo.offsetXIncMask && (currentKeyState & keyInfo.offsetXIncMask))
                    CameraProfileManager::getInstance().adjustOffset(adjustmentStep, 0.0f, 0.0f);
                if (keyInfo.offsetXDecMask && (currentKeyState & keyInfo.offsetXDecMask))
                    CameraProfileManager::getInstance().adjustOffset(-adjustmentStep, 0.0f, 0.0f);
                if (keyInfo.offsetYIncMask && (currentKeyState & keyInfo.offsetYIncMask))
                    CameraProfileManager::getInstance().adjustOffset(0.0f, adjustmentStep, 0.0f);
                if (keyInfo.offsetYDecMask && (currentKeyState & keyInfo.offsetYDecMask))
                    CameraProfileManager::getInstance().adjustOffset(0.0f, -adjustmentStep, 0.0f);
                if (keyInfo.offsetZIncMask && (currentKeyState & keyInfo.offsetZIncMask))
                    CameraProfileManager::getInstance().adjustOffset(0.0f, 0.0f, adjustmentStep);
                if (keyInfo.offsetZDecMask && (currentKeyState & keyInfo.offsetZDecMask))
                    CameraProfileManager::getInstance().adjustOffset(0.0f, 0.0f, -adjustmentStep);

            } // end if(adjustment mode active)

            // Remember current key state for the next loop iteration
            previousKeyState = currentKeyState;
        }
        catch (const std::exception &e)
        {
            logger.log(LOG_ERROR, "CameraProfileThread: Exception caught: " + std::string(e.what()));
            Sleep(1000); // Prevent spamming errors
        }
        catch (...)
        {
            logger.log(LOG_ERROR, "CameraProfileThread: Caught unknown exception!");
            Sleep(1000);
        }
    } // end while (main loop)

    logger.log(LOG_INFO, "CameraProfileThread: Exiting");
    return 0;
}
