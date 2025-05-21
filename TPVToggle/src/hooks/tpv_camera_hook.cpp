/**
 * @file tpv_camera_hook.cpp
 * @brief Implementation of third-person camera position offset hook
 *
 * Intercepts the TPV camera update function to apply custom position offsets,
 * enabling over-the-shoulder and other camera positioning options.
 */

#include "tpv_camera_hook.h"
#include "logger.h"
#include "constants.h"
#include "game_structures.h" // For TPVCameraData potentially, or just for context
#include "utils.h"
// #include "aob_scanner.h" // No longer needed here if HookManager handles AOB
#include "global_state.h"
#include "game_interface.h" // For getViewState
#include "math_utils.h"     // For Vector3, Quaternion
#include "config.h"
#include "transition_manager.h"
#include "hook_manager.hpp" // Use HookManager

#include <DirectXMath.h>
#include <stdexcept>
#include <string> // For std::string

// External configuration reference
extern Config g_config;

// Function typedef for the TPV camera update function
// RCX: C_CameraThirdPerson object pointer
// RDX: Pointer to output pose structure (position + quaternion)
typedef void(__fastcall *TpvCameraUpdateFunc)(uintptr_t thisPtr, uintptr_t outputPosePtr);

// Hook state
static TpvCameraUpdateFunc fpTpvCameraUpdateOriginal = nullptr;
// static BYTE *g_tpvCameraHookAddress = nullptr; // Managed by HookManager
static std::string g_tpvCameraHookId = ""; // To store the ID from HookManager

/**
 * @brief Gets the currently active camera offset
 * @details Determines which offset source to use based on configuration
 * @return Vector3 The local space offset to apply
 */
Vector3 GetActiveOffset() // This helper remains the same
{
    // Priority 1: Active transition
    if (g_config.enable_camera_profiles)
    {
        Vector3 transitionPosition;
        Quaternion transitionRotation; // Rotation not used for offset here but part of updateTransition signature

        // Check if a transition is in progress and get interpolated values
        // deltaTime of 0.016f is approx 60FPS, adjust if your game loop for updates is different
        // or pass a real delta time if available in this context.
        // For now, assuming a fixed update for transition check.
        if (TransitionManager::getInstance().updateTransition(0.016f, transitionPosition, transitionRotation))
        {
            // If transitioning, the transitionPosition IS the target g_currentCameraOffset for that frame
            return transitionPosition;
        }
        // If transition finished or not active, use the current profile's offset (g_currentCameraOffset)
        return g_currentCameraOffset;
    }

    // Priority 2: Static configuration offsets (if profiles not enabled)
    return Vector3(g_config.tpv_offset_x, g_config.tpv_offset_y, g_config.tpv_offset_z);
}

/**
 * @brief Detour function for TPV camera update
 * @details Intercepts the camera position calculation to apply custom offsets
 *          based on configuration, camera profiles, or transitions.
 * @param thisPtr Pointer to the camera object
 * @param outputPosePtr Pointer to output structure containing position/rotation
 */
void __fastcall Detour_TpvCameraUpdate(uintptr_t thisPtr, uintptr_t outputPosePtr)
{
    Logger &logger = Logger::getInstance();

    // Call original function first to get base camera position and rotation
    if (!fpTpvCameraUpdateOriginal)
    {
        logger.log(LOG_ERROR, "TpvCameraHook: fpTpvCameraUpdateOriginal (trampoline) is NULL!");
        return;
    }

    try
    {
        fpTpvCameraUpdateOriginal(thisPtr, outputPosePtr);
    }
    catch (const std::exception &e)
    {
        logger.log(LOG_ERROR, "TpvCameraHook: Exception in original TPV camera update function: " + std::string(e.what()));
        // Attempting to proceed might be risky, but not calling original is worse.
        // If original crashes, this won't save it.
        return;
    }
    catch (...)
    {
        logger.log(LOG_ERROR, "TpvCameraHook: Unknown exception in original TPV camera update function.");
        return;
    }

    // Validate parameters and check if we're in TPV mode
    if (outputPosePtr == 0)
    {
        // logger.log(LOG_TRACE, "TpvCameraHook: outputPosePtr is NULL, skipping offset.");
        return;
    }
    if (getViewState() != 1) // Ensure TPV mode
    {
        // logger.log(LOG_TRACE, "TpvCameraHook: Not in TPV mode, skipping offset.");
        return;
    }

    // Validate output buffer is accessible for both reading current pose and writing new position
    if (!isMemoryReadable(reinterpret_cast<void *>(outputPosePtr), Constants::TPV_OUTPUT_POSE_REQUIRED_SIZE) ||
        !isMemoryWritable(reinterpret_cast<void *>(outputPosePtr + Constants::TPV_OUTPUT_POSE_POSITION_OFFSET), sizeof(Vector3)))
    {
        logger.log(LOG_DEBUG, "TpvCameraHook: Output pose buffer not readable/writable at required sections.");
        return;
    }

    try
    {
        // Get pointers to position and rotation in the output structure
        Vector3 *positionPtr = reinterpret_cast<Vector3 *>(
            outputPosePtr + Constants::TPV_OUTPUT_POSE_POSITION_OFFSET);
        Quaternion *rotationPtr = reinterpret_cast<Quaternion *>( // Read-only for current rotation
            outputPosePtr + Constants::TPV_OUTPUT_POSE_ROTATION_OFFSET);

        // Read current camera state (after original function has run)
        Vector3 currentPosition = *positionPtr;
        Quaternion currentRotation = *rotationPtr; // This is the camera's orientation

        // Determine which offset to apply
        Vector3 localOffsetToApply = GetActiveOffset();

        // Skip if no offset is to be applied (all components zero)
        if (localOffsetToApply.x == 0.0f && localOffsetToApply.y == 0.0f && localOffsetToApply.z == 0.0f)
        {
            // logger.log(LOG_TRACE, "TpvCameraHook: No active offset to apply.");
            return;
        }

        // Transform local offset to world space using the camera's current rotation
        Vector3 worldOffset = currentRotation.Rotate(localOffsetToApply);

        // Apply offset to camera position
        Vector3 newPosition = currentPosition + worldOffset;

        // Write back the modified position
        *positionPtr = newPosition;

        // Trace logging can be very verbose, use with caution or compile out in release builds
        // logger.log(LOG_TRACE, "TpvCameraHook: Applied offset. OriginalPos: " + Vector3ToString(currentPosition) +
        //                          " LocalOffset: " + Vector3ToString(localOffsetToApply) +
        //                          " WorldOffset: " + Vector3ToString(worldOffset) +
        //                          " NewPos: " + Vector3ToString(newPosition) +
        //                          " CamRot: " + QuatToString(currentRotation));
    }
    catch (const std::exception &e)
    {
        logger.log(LOG_ERROR, "TpvCameraHook: Exception while applying camera offset: " + std::string(e.what()));
    }
    catch (...)
    {
        logger.log(LOG_ERROR, "TpvCameraHook: Unknown exception while applying camera offset.");
    }
}

bool initializeTpvCameraHook(uintptr_t moduleBase, size_t moduleSize)
{
    Logger &logger = Logger::getInstance();

    // Check if feature is disabled by configuration
    if (!g_config.enable_camera_profiles &&
        g_config.tpv_offset_x == 0.0f &&
        g_config.tpv_offset_y == 0.0f &&
        g_config.tpv_offset_z == 0.0f)
    {
        logger.log(LOG_INFO, "TpvCameraHook: Feature disabled (no offsets configured and profiles disabled).");
        return true; // Not an error, just not enabled.
    }

    logger.log(LOG_INFO, "TpvCameraHook: Initializing TPV camera position offset hook...");

    try
    {
        HookManager &hookManager = HookManager::getInstance();
        g_tpvCameraHookId = hookManager.create_inline_hook_aob(
            "TpvCameraUpdate",
            moduleBase,
            moduleSize,
            Constants::TPV_CAMERA_UPDATE_AOB_PATTERN,
            0, // AOB_OFFSET, assume pattern starts at function
            reinterpret_cast<void *>(Detour_TpvCameraUpdate),
            reinterpret_cast<void **>(&fpTpvCameraUpdateOriginal));

        if (g_tpvCameraHookId.empty() || fpTpvCameraUpdateOriginal == nullptr)
        {
            throw std::runtime_error("Failed to create TPV Camera Update hook via HookManager.");
        }

        logger.log(LOG_INFO, "TpvCameraHook: TPV Camera Update hook successfully installed with ID: " + g_tpvCameraHookId);
        if (g_config.enable_camera_profiles)
        {
            logger.log(LOG_INFO, "  - Camera profiles: ENABLED for dynamic offsets.");
        }
        else
        {
            logger.log(LOG_INFO, "  - Using static offset: X=" + std::to_string(g_config.tpv_offset_x) +
                                     ", Y=" + std::to_string(g_config.tpv_offset_y) +
                                     ", Z=" + std::to_string(g_config.tpv_offset_z));
        }
        return true;
    }
    catch (const std::exception &e)
    {
        logger.log(LOG_ERROR, "TpvCameraHook: Initialization failed: " + std::string(e.what()));
        cleanupTpvCameraHook(); // Attempt cleanup
        return false;
    }
}

void cleanupTpvCameraHook()
{
    Logger &logger = Logger::getInstance();

    if (!g_tpvCameraHookId.empty())
    {
        if (HookManager::getInstance().remove_hook(g_tpvCameraHookId))
        {
            logger.log(LOG_INFO, "TpvCameraHook: Hook '" + g_tpvCameraHookId + "' removed.");
        }
        else
        {
            logger.log(LOG_WARNING, "TpvCameraHook: Failed to remove hook '" + g_tpvCameraHookId + "' via HookManager.");
        }
        fpTpvCameraUpdateOriginal = nullptr; // Clear trampoline
        g_tpvCameraHookId = "";
    }
    logger.log(LOG_DEBUG, "TpvCameraHook: Cleanup complete.");
}

bool isTpvCameraHookActive()
{
    return (fpTpvCameraUpdateOriginal != nullptr && !g_tpvCameraHookId.empty());
}
