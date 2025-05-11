#include "tpv_camera_hook.h"
#include "logger.h"
#include "constants.h"
#include "utils.h"
#include "aob_scanner.h"
#include "global_state.h"
#include "game_interface.h"
#include "math_utils.h"
#include "config.h"
#include "transition_manager.h"

#include "MinHook.h"

#include <DirectXMath.h>
#include <sstream>
#include <iomanip>
#include <stdexcept>

// Function Signature for FUN_18392509c
// void FUN_18392509c(longlong param_1,undefined8 *param_2)
// thisPtr (RCX) = C_CameraThirdPerson object pointer
// outputPosePtr (RDX) = Pointer to structure receiving final pose
typedef void(__fastcall *TpvCameraUpdateFunc)(uintptr_t thisPtr, uintptr_t outputPosePtr);

// Trampoline
static TpvCameraUpdateFunc fpTpvCameraUpdateOriginal = nullptr;
static BYTE *g_tpvCameraHookAddress = nullptr;

// Global Config defined in dllmain.cpp
extern Config g_config;

void __fastcall Detour_TpvCameraUpdate(uintptr_t thisPtr, uintptr_t outputPosePtr)
{
    Logger &logger = Logger::getInstance();
    bool original_function_called_successfully = false;

    if (g_nativeOrbitCVarsEnabled.load())
    { // If game's native orbit is supposed to be active
        if (fpTpvCameraUpdateOriginal)
        {
            fpTpvCameraUpdateOriginal(thisPtr, outputPosePtr); // Let game do its full TPV update
        }
        // Optionally, AFTER original, read values from outputPosePtr and CVars to see if they match
        // Or just return and let game handle it fully.
        return;
    }

    if (g_config.enable_orbital_camera_mode && g_orbitalModeActive.load())
    {
        logger.log(LOG_DEBUG, "TpvCamUpdate: ORBITAL PATH - SKIPPING ORIGINAL");
        if (outputPosePtr == 0 || getViewState() != 1 ||
            !isMemoryWritable(reinterpret_cast<void *>(outputPosePtr), Constants::TPV_OUTPUT_POSE_REQUIRED_SIZE))
        {
            // Still call original if we can't do our work, or game might stall
            if (fpTpvCameraUpdateOriginal)
                fpTpvCameraUpdateOriginal(thisPtr, outputPosePtr);
            return;
        }
        Vector3 *outPosPtr = reinterpret_cast<Vector3 *>(outputPosePtr + Constants::TPV_OUTPUT_POSE_POSITION_OFFSET);
        Quaternion *outRotPtr = reinterpret_cast<Quaternion *>(outputPosePtr + Constants::TPV_OUTPUT_POSE_ROTATION_OFFSET);

        Vector3 playerFocusPoint = g_playerWorldPosition; // Assumes g_playerWorldPosition is fresh
        // playerFocusPoint.z += 1.5f; // Optional head offset

        Vector3 cameraForwardInWorld = g_orbitalCameraRotation.Rotate(Vector3(0.0f, 1.0f, 0.0f));
        Vector3 orbitalCameraBasePosition = playerFocusPoint - (cameraForwardInWorld * g_orbitalCameraDistance);
        Vector3 worldScreenOffset = g_orbitalCameraRotation.Rotate(g_currentCameraOffset);

        *outPosPtr = orbitalCameraBasePosition + worldScreenOffset;
        *outRotPtr = g_orbitalCameraRotation;
    }
    else
    {
        logger.log(LOG_DEBUG, "TpvCamUpdate: STANDARD PATH - CALLING ORIGINAL");
        if (fpTpvCameraUpdateOriginal)
        {
            // 1. --- Call Original Game Function ---
            // This is usually important because the original function might:
            // - Initialize or prepare the outputPosePtr structure.
            // - Update other game systems that rely on it being called.
            // - Provide a base position/rotation if our specific mode isn't active.

            try
            {
                fpTpvCameraUpdateOriginal(thisPtr, outputPosePtr);
                original_function_called_successfully = true;
            }
            catch (const std::exception &e)
            {
                logger.log(LOG_ERROR, "TpvCameraHook: Exception calling original TpvCameraUpdate func: " + std::string(e.what()));
                return; // Critical error, don't proceed
            }
            catch (...)
            {
                logger.log(LOG_ERROR, "TpvCameraHook: Unknown exception calling original TpvCameraUpdate func.");
                return; // Critical error, don't proceed
            }

            // 2. --- Early Exit Conditions ---
            // If original didn't run, output pointer is bad, or we aren't in TPV render state, do nothing further.
            if (!original_function_called_successfully || outputPosePtr == 0 || getViewState() != 1)
            {
                return;
            }

            // 3. --- Validate Memory Accessibility ---
            // Ensure we can read from and write to the outputPosePtr structure.
            if (!isMemoryWritable(reinterpret_cast<void *>(outputPosePtr), Constants::TPV_OUTPUT_POSE_REQUIRED_SIZE)) // Changed to isMemoryWritable as we will write
            {
                // Log this warning less frequently to avoid spam if it's a persistent issue
                static int write_perm_warn_count = 0;
                if (++write_perm_warn_count % 100 == 0 && logger.isDebugEnabled())
                {
                    logger.log(LOG_WARNING, "TpvCameraHook: outputPosePtr at " + format_address(outputPosePtr) + " is not writable or fully accessible.");
                }
                return;
            }

            // 4. --- Define Pointers to Position and Rotation in outputPosePtr ---
            Vector3 *outPosPtr = reinterpret_cast<Vector3 *>(outputPosePtr + Constants::TPV_OUTPUT_POSE_POSITION_OFFSET);
            Quaternion *outRotPtr = reinterpret_cast<Quaternion *>(outputPosePtr + Constants::TPV_OUTPUT_POSE_ROTATION_OFFSET);

            // 5. --- MODE-SPECIFIC LOGIC ---
            try
            {
                if (g_config.enable_orbital_camera_mode && g_orbitalModeActive.load())
                {
                    // ===== ORBITAL CAMERA MODE ACTIVE =====
                    // The renderer should use our fully custom orbital camera pose.

                    // Player's world position is the focus point for the orbital camera.
                    // This global should be updated by your player_state_hook.
                    Vector3 playerFocusPoint = g_playerWorldPosition;

                    // You might want to add a slight vertical offset to focus on the player's head/chest
                    // instead of their feet, depending on what g_playerWorldPosition represents.
                    // Example: playerFocusPoint.z += 1.5f; // Adjust as needed.

                    // Calculate the orbital camera's position:
                    // Start at the player focus point.
                    // Move backward along the orbital camera's current view direction by g_orbitalCameraDistance.
                    // The orbital camera's "forward" is typically its local +Y or +Z depending on convention.
                    // If using Quaternion::Rotate with (0,1,0) for forward:
                    Vector3 cameraForwardInWorld = g_orbitalCameraRotation.Rotate(Vector3(0.0f, 1.0f, 0.0f)); // Assumes Y-forward in camera space
                    Vector3 orbitalCameraBasePosition = playerFocusPoint - (cameraForwardInWorld * g_orbitalCameraDistance);

                    // Apply screen-space offsets (g_currentCameraOffset from profiles/live adjustments)
                    // These offsets should be relative to the *orbital camera's* orientation.
                    Vector3 worldScreenOffset = g_orbitalCameraRotation.Rotate(g_currentCameraOffset);

                    // Final position and rotation for orbital camera
                    *outPosPtr = orbitalCameraBasePosition + worldScreenOffset;
                    *outRotPtr = g_orbitalCameraRotation;

                    // Optional: Logging for orbital mode values
                    if (logger.isDebugEnabled())
                    {
                        static int orbital_log_count = 0;
                        if (++orbital_log_count % 60 == 0)
                        { // Log about once per second if 60fps
                            logger.log(LOG_DEBUG, "TpvCameraHook (Orbital): Applied Pose. Pos=" + Vector3ToString(*outPosPtr) +
                                                      " Rot=" + QuatToString(*outRotPtr) + " Dist=" + std::to_string(g_orbitalCameraDistance));
                        }
                    }
                }
                else
                {
                    // ===== STANDARD TPV OFFSET MODE (Orbital is OFF or Disabled) =====
                    // This is your original logic for applying g_currentCameraOffset or static config offsets.

                    // Read the game's calculated position and rotation (already in *outPosPtr, *outRotPtr from original call)
                    Vector3 gameCalculatedPos = *outPosPtr;
                    Quaternion gameCalculatedRot = *outRotPtr;

                    Vector3 activeOffset; // This will be g_currentCameraOffset or the static config one.

                    // Transition Manager logic (only relevant for standard TPV profiles, not typically for global static offset)
                    // Note: If TransitionManager directly sets g_currentCameraOffset, this block might be simplified.
                    bool isTransitioning = false;
                    if (g_config.enable_camera_profiles)
                    { // Check if profiles (and thus transitions) are enabled
                        Vector3 transitionPosition;
                        Quaternion transitionRotation; // Orbital camera won't use transitionRotation here.
                                                       // Standard TPV might, but it's unusual to transition TPV offsets' rotation.

                        // Delta time for transition - needs to be actual frame delta for smoothness
                        // For simplicity here, using a fixed small step, but ideally, pass actual game's deltaTime.
                        // The 0.016f here implies roughly 60fps.
                        if (TransitionManager::getInstance().updateTransition(0.016f, transitionPosition, transitionRotation))
                        {
                            activeOffset = transitionPosition; // Use the offset value from the transition
                            isTransitioning = true;
                        }
                        else
                        {
                            activeOffset = g_currentCameraOffset; // Use the active profile's offset
                        }
                    }
                    else // Profiles (and transitions) disabled, use static config offset
                    {
                        activeOffset = Vector3(g_config.tpv_offset_x, g_config.tpv_offset_y, g_config.tpv_offset_z);
                    }

                    // Apply the 'activeOffset' (which is a local camera-space offset)
                    // by rotating it with the game's calculated TPV camera rotation.
                    Vector3 worldOffset = gameCalculatedRot.Rotate(activeOffset);

                    // Final position for standard TPV offset mode
                    *outPosPtr = gameCalculatedPos + worldOffset;
                    // Rotation remains as gameCalculatedRot (i.e., *outRotPtr is not changed from game's value)

                    // Optional: Logging for standard TPV offset mode
                    if (logger.isDebugEnabled() && !isTransitioning)
                    { // Log less when transitioning to avoid spam
                        static int standard_tpv_log_count = 0;
                        if (++standard_tpv_log_count % 60 == 0)
                        {
                            logger.log(LOG_DEBUG, "TpvCameraHook (Standard): Applied Offset. NewPos=" + Vector3ToString(*outPosPtr) +
                                                      " GameRot=" + QuatToString(*outRotPtr) + " OffsetUsed=" + Vector3ToString(activeOffset));
                        }
                    }
                }
            }
            catch (const std::exception &e)
            {
                logger.log(LOG_ERROR, "TpvCameraHook: Exception applying camera modifications: " + std::string(e.what()));
            }
            catch (...)
            {
                logger.log(LOG_ERROR, "TpvCameraHook: Unknown exception applying camera modifications.");
            }
        }
        else
        {
            logger.log(LOG_ERROR, "TpvCameraHook: Original TpvCameraUpdate function pointer is NULL!");
            return; // Cannot proceed
        }
    }
}

// Initialize/Cleanup Functions
bool initializeTpvCameraHook(uintptr_t moduleBase, size_t moduleSize)
{
    Logger &logger = Logger::getInstance();

    if (!g_config.enable_camera_profiles && g_config.tpv_offset_x == 0.0f && g_config.tpv_offset_y == 0.0f && g_config.tpv_offset_z == 0.0f)
    {
        logger.log(LOG_INFO, "TpvCameraHook: Feature disabled (X,Y,Z = 0 and Profiles Enabled = false)");
        return true; // Not an error condition
    }

    logger.log(LOG_INFO, "Initializing TPV Camera Update Hook..."); // Added info log

    std::vector<BYTE> pattern = parseAOB(Constants::TPV_CAMERA_UPDATE_AOB_PATTERN);
    if (pattern.empty())
    {
        logger.log(LOG_ERROR, "TpvCameraHook: Failed to parse AOB pattern.");
        return false;
    }

    g_tpvCameraHookAddress = FindPattern(reinterpret_cast<BYTE *>(moduleBase), moduleSize, pattern);
    if (!g_tpvCameraHookAddress)
    {
        logger.log(LOG_ERROR, "TpvCameraHook: AOB pattern not found.");
        return false;
    }
    logger.log(LOG_INFO, "TpvCameraHook: Found TPV Camera Update function at " + format_address(reinterpret_cast<uintptr_t>(g_tpvCameraHookAddress)));

    MH_STATUS status = MH_CreateHook(g_tpvCameraHookAddress, reinterpret_cast<LPVOID>(&Detour_TpvCameraUpdate), reinterpret_cast<LPVOID *>(&fpTpvCameraUpdateOriginal));
    if (status != MH_OK)
    {
        logger.log(LOG_ERROR, "TpvCameraHook: MH_CreateHook failed: " + std::string(MH_StatusToString(status)));
        return false;
    }
    if (!fpTpvCameraUpdateOriginal)
    {
        logger.log(LOG_ERROR, "TpvCameraHook: MH_CreateHook returned NULL trampoline.");
        MH_RemoveHook(g_tpvCameraHookAddress);
        return false;
    }

    status = MH_EnableHook(g_tpvCameraHookAddress);
    if (status != MH_OK)
    {
        logger.log(LOG_ERROR, "TpvCameraHook: MH_EnableHook failed: " + std::string(MH_StatusToString(status)));
        MH_RemoveHook(g_tpvCameraHookAddress);
        return false;
    }

    logger.log(LOG_INFO, "TpvCameraHook: Successfully installed.");
    return true;
}

void cleanupTpvCameraHook()
{
    Logger &logger = Logger::getInstance();
    if (g_tpvCameraHookAddress)
    {
        MH_DisableHook(g_tpvCameraHookAddress);
        MH_RemoveHook(g_tpvCameraHookAddress);
        g_tpvCameraHookAddress = nullptr;
        fpTpvCameraUpdateOriginal = nullptr;
        logger.log(LOG_DEBUG, "TpvCameraHook: Hook cleaned up.");
    }
}
