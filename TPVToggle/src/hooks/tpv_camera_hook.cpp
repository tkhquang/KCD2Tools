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
    bool original_called_successfully = false;

    if (g_config.enable_orbital_camera_mode && g_orbitalModeActive.load(std::memory_order_relaxed))
    {
        // --- ORBITAL CAMERA MODE ---
        if (outputPosePtr == 0 || getViewState() != 1 ||
            !isMemoryWritable(reinterpret_cast<void *>(outputPosePtr), Constants::TPV_OUTPUT_POSE_REQUIRED_SIZE))
        {
            // Fallback to original if conditions not met for orbital override
            if (fpTpvCameraUpdateOriginal)
                fpTpvCameraUpdateOriginal(thisPtr, outputPosePtr);
            return;
        }

        Vector3 *outPosPtr = reinterpret_cast<Vector3 *>(outputPosePtr + Constants::TPV_OUTPUT_POSE_POSITION_OFFSET);
        Quaternion *outRotPtr = reinterpret_cast<Quaternion *>(outputPosePtr + Constants::TPV_OUTPUT_POSE_ROTATION_OFFSET);

        Vector3 playerFocusPoint = g_playerWorldPosition; // Read atomic/updated by other hook

        // Critical section to get a consistent snapshot of orbital parameters
        Quaternion currentOrbitalRotation_snapshot;
        float currentOrbitalDistance_snapshot;
        Vector3 currentScreenOffset_snapshot; // Assuming g_currentCameraOffset is updated by a serialized thread or also protected.
                                              // If g_currentCameraOffset can be changed truly concurrently without its own lock,
                                              // this read is not perfectly safe. Consider std::atomic<Vector3> or a separate mutex for it.
        {                                     // Start of critical section for reading shared orbital data
            std::lock_guard<std::mutex> lock(g_orbitalCameraMutex);
            currentOrbitalRotation_snapshot = g_orbitalCameraRotation; // Read the Quaternion computed by the input thread
            // Atomics can be read without the lock IF their updates are also atomic and don't depend
            // on a complex relationship with g_orbitalCameraRotation that is only valid inside the lock.
            // However, if update_orbital_camera_rotation_from_euler modifies yaw/pitch AND then rotation
            // inside its lock, reading them all together here ensures some consistency.
            // But yaw/pitch from atomics are fine to read directly usually. The QUATERNION g_orbitalCameraRotation is the main one
            // that benefits from the same lock during read as its write in update_orbital_camera_rotation_from_euler.
        } // lock_guard goes out of scope, mutex released.

        // Atomics can be read directly.
        currentOrbitalDistance_snapshot = g_orbitalCameraDistance.load(std::memory_order_relaxed);
        currentScreenOffset_snapshot = g_currentCameraOffset; // Still assuming this is safe enough or protected elsewhere for writes

        // ... (playerFocusPoint validity check) ...
        if (playerFocusPoint.MagnitudeSquared() < 0.001f && (playerFocusPoint.x == 0.0f && playerFocusPoint.y == 0.0f && playerFocusPoint.z == 0.0f))
        {
            if (fpTpvCameraUpdateOriginal)
                fpTpvCameraUpdateOriginal(thisPtr, outputPosePtr);
            return;
        }
        playerFocusPoint.z += g_config.tpv_offset_z; // Using config Z offset as vertical player focus adjustment

        Vector3 cameraForwardLocal = Vector3(0.0f, 1.0f, 0.0f); // Assumes camera's local Y is forward
        Vector3 cameraForwardInWorld = currentOrbitalRotation_snapshot.Rotate(cameraForwardLocal);

        Vector3 orbitalCameraBasePosition = playerFocusPoint - (cameraForwardInWorld * currentOrbitalDistance_snapshot);

        Vector3 localScreenOffset = Vector3(currentScreenOffset_snapshot.x, 0.0f, currentScreenOffset_snapshot.y);
        Vector3 worldScreenOffset = currentOrbitalRotation_snapshot.Rotate(localScreenOffset);

        *outPosPtr = orbitalCameraBasePosition + worldScreenOffset;
        *outRotPtr = currentOrbitalRotation_snapshot;

        g_latestTpvCameraForward = cameraForwardInWorld.Normalized(); // This is now derived from our snapshot

        return; // IMPORTANT: Do not fall through to standard TPV logic
    }

    // --- STANDARD TPV OFFSET MODE (Not Orbital) ---
    if (fpTpvCameraUpdateOriginal)
    {
        try
        {
            fpTpvCameraUpdateOriginal(thisPtr, outputPosePtr);
            original_called_successfully = true;
        }
        catch (const std::exception &e)
        { /* log */
            return;
        }
        catch (...)
        { /* log */
            return;
        }
    }
    else
    {
        logger.log(LOG_ERROR, "TpvCameraHook: Original TPV Update function pointer is NULL!");
        return;
    }

    if (!original_called_successfully || outputPosePtr == 0 || getViewState() != 1 ||
        !isMemoryWritable(reinterpret_cast<void *>(outputPosePtr), Constants::TPV_OUTPUT_POSE_REQUIRED_SIZE))
    {
        return;
    }

    Vector3 *outPosPtr = reinterpret_cast<Vector3 *>(outputPosePtr + Constants::TPV_OUTPUT_POSE_POSITION_OFFSET);
    Quaternion *outRotPtr = reinterpret_cast<Quaternion *>(outputPosePtr + Constants::TPV_OUTPUT_POSE_ROTATION_OFFSET);

    Vector3 gameCalculatedPos = *outPosPtr;
    Quaternion gameCalculatedRot = *outRotPtr;
    Vector3 activeOffset;

    if (g_config.enable_camera_profiles)
    {
        Vector3 transitionPosition;
        Quaternion dummyRotation; // Not typically used for offset transitions
        if (TransitionManager::getInstance().updateTransition(0.016f /* Estimate dt */, transitionPosition, dummyRotation))
        {
            activeOffset = transitionPosition;
        }
        else
        {
            activeOffset = g_currentCameraOffset;
        }
    }
    else
    {
        activeOffset = Vector3(g_config.tpv_offset_x, g_config.tpv_offset_y, g_config.tpv_offset_z);
    }

    Vector3 worldOffset = gameCalculatedRot.Rotate(activeOffset);
    *outPosPtr = gameCalculatedPos + worldOffset;
    // gameCalculatedRot (*outRotPtr) is already set by the original call for standard TPV.

    // Update g_latestTpvCameraForward based on the game's TPV camera rotation
    // Assumes gameCalculatedRot.Rotate Y-forward gives the view direction. Adjust if different.
    g_latestTpvCameraForward = gameCalculatedRot.Rotate(Vector3(0.0f, 1.0f, 0.0f)).Normalized();
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
