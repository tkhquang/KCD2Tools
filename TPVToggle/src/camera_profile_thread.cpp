#include "camera_profile_thread.h"
#include "camera_profile.h"
#include "logger.h"
#include "utils.h"
#include "global_state.h"
#include "config.h"
#include "hooks/tpv_input_hook.h"
#include "game_interface.h"
#include "constants.h"

#include <unordered_map>
#include <bitset>
#include <vector>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <math.h>

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

    CameraProfileThreadData *data = static_cast<CameraProfileThreadData *>(param);
    if (!data)
    {
        logger.log(LOG_ERROR, "CameraProfileThread: NULL data received");
        return 1;
    }
    float adjustmentStep = data->adjustmentStep;
    delete data;

    KeyBitMapInfo keyInfo = createKeyBitMap(g_config);
    uint64_t previousKeyState = 0;

    // Initialize orbital camera parameters (once at thread start)
    g_orbitalCameraDistance.store(g_config.orbit_default_distance, std::memory_order_relaxed);
    g_orbitalCameraYaw.store(0.0f, std::memory_order_relaxed);
    // Convert default pitch from degrees (if that's how you think of it) to radians
    g_orbitalCameraPitch.store(g_config.orbit_pitch_min_degrees * (static_cast<float>(M_PI) / 180.0f), std::memory_order_relaxed); // Example: start at min pitch
    update_orbital_camera_rotation_from_euler();

    if (logger.isDebugEnabled())
    {
        logger.log(LOG_DEBUG, "CameraProfileThread: Registered " + std::to_string(keyInfo.keyCount) + " unique keys for monitoring.");
    }

    // --- CVar Address Acquisition Logic (from your snippet) ---
    // Typedef for GetSystemInterface from KCD2 mod loader seems to point to FUN_180a27de0
    typedef uintptr_t (*GetSystemInterfaceFunc_t)(); // Assuming it returns uintptr_t to ISystem or similar
    uintptr_t func_180a27de0_offset = 0xA27DE0;      // Ensure this is the correct RVA for FUN_180a27de0

    // Important: Check if g_ModuleBase is valid before using it
    GetSystemInterfaceFunc_t pfnGetSystemInterface = nullptr;
    if (g_ModuleBase != 0)
    {
        pfnGetSystemInterface = reinterpret_cast<GetSystemInterfaceFunc_t>(g_ModuleBase + func_180a27de0_offset);
    }
    else
    {
        logger.log(LOG_ERROR, "CameraProfileThread: g_ModuleBase is NULL, cannot get pfnGetSystemInterface.");
    }

    if (pfnGetSystemInterface) // Proceed only if function pointer is valid
    {
        logger.log(LOG_INFO, "CameraProfileThread: Attempting to acquire CVar struct address via pfnGetSystemInterface at " + format_address(reinterpret_cast<uintptr_t>(pfnGetSystemInterface)));
        int cvar_init_attempts = 0;
        while (g_GlobalGameCVarsStructAddr == 0 && cvar_init_attempts < 60 && WaitForSingleObject(g_exitEvent, 0) != WAIT_OBJECT_0)
        {
            uintptr_t pSystemInterface_temp = 0;
            try
            {
                // Call the function to get the ISystem (or equivalent) pointer
                pSystemInterface_temp = pfnGetSystemInterface();
            }
            catch (...)
            {
                pSystemInterface_temp = 0; // Catch potential crashes if function is wrong
                logger.log(LOG_ERROR, "CameraProfileThread: Exception calling pfnGetSystemInterface.");
            }

            if (pSystemInterface_temp != 0)
            {
                // The CVar struct seems to be at offset 0x20 from the returned ISystem pointer
                uintptr_t cvar_struct_candidate_addr = pSystemInterface_temp + 0x20; // ISystem + 0x20
                if (isMemoryReadable(reinterpret_cast<void *>(cvar_struct_candidate_addr), sizeof(uintptr_t)))
                {
                    g_GlobalGameCVarsStructAddr = *reinterpret_cast<uintptr_t *>(cvar_struct_candidate_addr);
                    if (g_GlobalGameCVarsStructAddr != 0)
                    {
                        logger.log(LOG_INFO, "CameraProfileThread: Successfully acquired pSystemInterface: " + format_address(pSystemInterface_temp));
                        logger.log(LOG_INFO, "CameraProfileThread: Successfully acquired g_GlobalGameCVarsStructAddr: " + format_address(g_GlobalGameCVarsStructAddr));
                        break;
                    }
                }
            }
            cvar_init_attempts++;
            Sleep(1000);
        }
        if (g_GlobalGameCVarsStructAddr == 0)
        {
            logger.log(LOG_ERROR, "CameraProfileThread: FAILED to acquire CVar struct address after multiple attempts.");
        }
    }

    // --- Main Loop ---
    while (WaitForSingleObject(g_exitEvent, 16) != WAIT_OBJECT_0)
    {
        try
        {
            uint64_t currentKeyState = getKeyStateBitmap(keyInfo.allKeys, keyInfo.keyMap);

            // --- Section 1: Global Toggles ---

            // Toggle for testing native game CVars (F2 by default)
            if (isNewKeyPress(currentKeyState, previousKeyState, keyInfo.nativeOrbitTestToggleMask))
            {
                if (g_GlobalGameCVarsStructAddr != 0)
                {
                    bool newNativeOrbitState = !g_nativeOrbitCVarsEnabled.load(std::memory_order_relaxed);
                    g_nativeOrbitCVarsEnabled.store(newNativeOrbitState, std::memory_order_relaxed);

                    // Example CVar offsets from your previous log (ensure these are correct for KCD2)
                    // bool* p_cl_cam_debug = reinterpret_cast<bool*>(g_GlobalGameCVarsStructAddr + 0x1C0); // Example
                    // bool* p_cl_cam_orbit = reinterpret_cast<bool*>(g_GlobalGameCVarsStructAddr + 0x1A8); // Example

                    // For KCD1, Crysis games: cl_tpvYaw=xxx; cl_tpvDist=xxx; cl_cam_orbit=1;
                    // It's highly probable these are float or int console variables.
                    // A more robust way is to use game's console command execution if you find it,
                    // or find the ICVar interface to set them by name.
                    // Directly writing to memory based on struct offsets from another game is risky.
                    // Placeholder for direct CVar write if offsets are known AND VERIFIED FOR KCD2:
                    // if (isMemoryWritable(p_cl_cam_orbit, sizeof(bool))) {
                    //    *p_cl_cam_orbit = newNativeOrbitState;
                    //    logger.log(LOG_INFO, "Native Orbit Test: Game CVar cl_cam_orbit set to " + std::string(newNativeOrbitState ? "true" : "false"));
                    // }
                    logger.log(LOG_INFO, "Native Orbit Test Toggle: " + std::string(newNativeOrbitState ? "Requesting ENABLED (manual CVar set needed)" : "Requesting DISABLED"));
                }
                else
                {
                    logger.log(LOG_WARNING, "Native Orbit Test: g_GlobalGameCVarsStructAddr is NULL, cannot toggle game CVars.");
                }
            }

            // Toggle for Orbital Camera Mode (e.g., F5)
            if (isNewKeyPress(currentKeyState, previousKeyState, keyInfo.orbitalModeToggleMask))
            {
                if (g_config.enable_orbital_camera_mode) // Check if feature is enabled in INI
                {
                    bool newOrbitalState = !g_orbitalModeActive.load(std::memory_order_relaxed);
                    g_orbitalModeActive.store(newOrbitalState, std::memory_order_relaxed);
                    logger.log(LOG_INFO, "Orbital Camera Mode: " + std::string(newOrbitalState ? "ENABLED" : "DISABLED"));

                    if (newOrbitalState) // Just enabled orbital mode
                    {
                        logger.log(LOG_INFO, "Orbital Camera Mode: Initializing pose...");
                        { // Scope for lock
                            std::lock_guard<std::mutex> lock(g_orbitalCameraMutex);

                            Quaternion playerRot = g_playerWorldOrientation; // Read once

                            float targetYaw = 0.0f;
                            // Attempt to get player's current world yaw to initially position camera behind them
                            if (playerRot != Quaternion::Identity())
                            {
                                Vector3 playerForward = playerRot.Rotate(Vector3(0.0f, 1.0f, 0.0f)); // Player's local Y is forward
                                if (playerForward.MagnitudeSquared() > 0.001f)
                                {
                                    playerForward.Normalize();
                                    targetYaw = atan2f(playerForward.x, playerForward.y); // Yaw from player's forward
                                }
                            }

                            g_orbitalCameraYaw.store(targetYaw + static_cast<float>(M_PI), std::memory_order_relaxed); // Place camera yaw 180 deg from player's current yaw

                            // Set a default pitch (e.g., looking slightly down at player)
                            float defaultPitchDegrees = -15.0f; // Configurable?
                            float initialPitchClampedDeg = std::max(g_config.orbit_pitch_min_degrees, std::min(defaultPitchDegrees, g_config.orbit_pitch_max_degrees));
                            g_orbitalCameraPitch.store(initialPitchClampedDeg * (static_cast<float>(M_PI) / 180.0f), std::memory_order_relaxed);

                            g_orbitalCameraDistance.store(g_config.orbit_default_distance, std::memory_order_relaxed);
                        } // lock released

                        update_orbital_camera_rotation_from_euler(); // Calculate initial g_orbitalCameraRotation

                        logger.log(LOG_DEBUG, "Orbital Mode ENABLED (New Init). TargetYaw=" + std::to_string(g_orbitalCameraYaw.load()) +
                                                  " TargetPitch=" + std::to_string(g_orbitalCameraPitch.load()) +
                                                  " TargetDist=" + std::to_string(g_orbitalCameraDistance.load()));
                    }
                }
            }

            // Toggle for Camera Profile Adjustment Mode (e.g., F11)
            if (isNewKeyPress(currentKeyState, previousKeyState, keyInfo.masterToggleMask))
            {
                bool newAdjMode = !g_cameraAdjustmentMode.load(std::memory_order_relaxed);
                g_cameraAdjustmentMode.store(newAdjMode, std::memory_order_relaxed);
                logger.log(LOG_INFO, "CameraProfileThread: Profile Adjustment mode " + std::string(newAdjMode ? "ENABLED" : "DISABLED"));
            }

            // --- Section 2: Orbital Camera Player Movement & Orientation ---
            if (g_config.enable_orbital_camera_mode && g_orbitalModeActive.load(std::memory_order_relaxed) && getViewState() == 1)
            {
                Vector3 targetMoveInputLocal = {0.0f, 0.0f, 0.0f}; // X for strafe, Y for forward/back
                bool isPlayerAttemptingToMove = false;

                if (GetAsyncKeyState(0x57) & 0x8000)
                {
                    targetMoveInputLocal.y += 1.0f;
                    isPlayerAttemptingToMove = true;
                } // W (Forward)
                if (GetAsyncKeyState(0x53) & 0x8000)
                {
                    targetMoveInputLocal.y -= 1.0f;
                    isPlayerAttemptingToMove = true;
                } // S (Backward)
                if (GetAsyncKeyState(0x41) & 0x8000)
                {
                    targetMoveInputLocal.x -= 1.0f;
                    isPlayerAttemptingToMove = true;
                } // A (Strafe Left)
                if (GetAsyncKeyState(0x44) & 0x8000)
                {
                    targetMoveInputLocal.x += 1.0f;
                    isPlayerAttemptingToMove = true;
                } // D (Strafe Right)

                // If player is moving, orient them.
                // g_thePlayerEntity and g_funcCEntitySetWorldTM are resolved in entity_hooks.cpp
                if (isPlayerAttemptingToMove && g_thePlayerEntity && g_funcCEntitySetWorldTM)
                {
                    Quaternion cameraPureYawRotation;
                    { // Brief scope for mutex to read g_orbitalCameraYaw
                        std::lock_guard<std::mutex> lock(g_orbitalCameraMutex);
                        // Player movement direction should be based on camera's YAW only (horizontal plane)
                        cameraPureYawRotation = Quaternion::FromXMVector(
                            DirectX::XMQuaternionRotationNormal(DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), // World Z-axis for yaw
                                                                g_orbitalCameraYaw.load(std::memory_order_relaxed)));
                    }

                    // Transform local input (camera-relative) to world direction based on camera's yaw
                    Vector3 worldMoveDir = cameraPureYawRotation.Rotate(targetMoveInputLocal);
                    worldMoveDir.z = 0.0f; // Movement is primarily horizontal plane

                    if (worldMoveDir.MagnitudeSquared() > 0.001f) // If there's significant movement input
                    {
                        worldMoveDir.Normalize();
                        // Player should face this worldMoveDir
                        Quaternion playerTargetOrientation = Quaternion::LookRotation(worldMoveDir, Vector3(0.0f, 0.0f, 1.0f)); // Assuming Z-up world

                        Vector3 currentPlayerPos = g_playerWorldPosition; // Updated by PlayerStateHook

                        Constants::Matrix34f playerTransform;
                        playerTransform.Set(playerTargetOrientation, currentPlayerPos);

                        // IMPORTANT: Determine correct flags for SetWorldTM for KCD2
                        // 0x1 is a common "force update/teleport". May need to experiment.
                        // KCD2ModLoader example for noclip used 0x400000.
                        int setTmFlags = 0x1;

                        g_funcCEntitySetWorldTM(g_thePlayerEntity, playerTransform.AsFloatPtr(), setTmFlags);

                        static int player_orient_log_count = 0;
                        if (logger.isDebugEnabled() && (++player_orient_log_count % 30 == 0))
                        { // Log less frequently
                            logger.log(LOG_DEBUG, "CPT (OrbitalMove): SetPlayerTM. MoveDir=" + Vector3ToString(worldMoveDir) +
                                                      " PlayerTargetRot=" + QuatToString(playerTargetOrientation));
                        }
                    }
                }
                // The critical "IDLE ROTATION PREVENTION" part needs to be handled by hooking
                // the game function that forces player to face camera and conditionally skipping it
                // if (g_orbitalModeActive && !isPlayerAttemptingToMove). This hook isn't defined here.
            }

            // --- Section 3: Camera Profile Adjustments (Only if adjustment mode is active) ---
            if (g_cameraAdjustmentMode.load(std::memory_order_relaxed))
            {
                if (isNewKeyPress(currentKeyState, previousKeyState, keyInfo.profileSaveMask))
                {
                    CameraProfileManager::getInstance().createNewProfileFromLiveState("General");
                }
                if (isNewKeyPress(currentKeyState, previousKeyState, keyInfo.profileUpdateMask))
                {
                    CameraProfileManager::getInstance().updateActiveProfileWithLiveState();
                }
                if (isNewKeyPress(currentKeyState, previousKeyState, keyInfo.profileDeleteMask))
                {
                    CameraProfileManager::getInstance().deleteActiveProfile();
                }
                if (isNewKeyPress(currentKeyState, previousKeyState, keyInfo.profileCycleMask))
                {
                    CameraProfileManager::getInstance().cycleToNextProfile();
                }
                if (isNewKeyPress(currentKeyState, previousKeyState, keyInfo.profileResetMask))
                {
                    CameraProfileManager::getInstance().resetToDefault();
                }

                // Offset adjustments (continuous while key held)
                if (keyInfo.offsetXIncMask && (currentKeyState & keyInfo.offsetXIncMask))
                    CameraProfileManager::getInstance().adjustOffset(adjustmentStep, 0.0f, 0.0f);
                if (keyInfo.offsetXDecMask && (currentKeyState & keyInfo.offsetXDecMask))
                    CameraProfileManager::getInstance().adjustOffset(-adjustmentStep, 0.0f, 0.0f);
                if (keyInfo.offsetYIncMask && (currentKeyState & keyInfo.offsetYIncMask))
                    CameraProfileManager::getInstance().adjustOffset(0.0f, adjustmentStep, 0.0f); // Note: Y for screen offset could be up/down
                if (keyInfo.offsetYDecMask && (currentKeyState & keyInfo.offsetYDecMask))
                    CameraProfileManager::getInstance().adjustOffset(0.0f, -adjustmentStep, 0.0f);
                if (keyInfo.offsetZIncMask && (currentKeyState & keyInfo.offsetZIncMask))
                    CameraProfileManager::getInstance().adjustOffset(0.0f, 0.0f, adjustmentStep); // Note: Z for screen offset could be depth/zoom
                if (keyInfo.offsetZDecMask && (currentKeyState & keyInfo.offsetZDecMask))
                    CameraProfileManager::getInstance().adjustOffset(0.0f, 0.0f, -adjustmentStep);
            }

            previousKeyState = currentKeyState;
        }
        catch (const std::exception &e)
        {
            logger.log(LOG_ERROR, "CameraProfileThread: Exception caught: " + std::string(e.what()));
            Sleep(1000);
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
