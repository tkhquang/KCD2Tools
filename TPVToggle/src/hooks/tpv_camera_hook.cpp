/**
 * @file tpv_camera_hook.cpp
 * @brief Implementation of third-person camera position offset hook using DetourModKit.
 *
 * Intercepts the TPV camera update function to apply custom position offsets,
 * enabling over-the-shoulder and other camera positioning options.
 */

#include "tpv_camera_hook.h"
#include "logger.h"
#include "constants.h"
#include "game_structures.h"
#include "utils.h"
#include "global_state.h"
#include "game_interface.h"
#include "math_utils.h"
#include "config.h"
#include "transition_manager.h"

#include <DetourModKit.hpp>

#include <DirectXMath.h>
#include <stdexcept>

using DMKString::format_address;

// External configuration reference
extern Config g_config;

// Hook state
static std::string g_tpvCameraHookId;

// Function typedef for the TPV camera update function
// RCX: C_CameraThirdPerson object pointer
// RDX: Pointer to output pose structure (position + quaternion)
typedef void(__fastcall *TpvCameraUpdateFunc)(uintptr_t thisPtr, uintptr_t outputPosePtr);

// Original function pointer (trampoline from SafetyHook)
static TpvCameraUpdateFunc fpTpvCameraUpdateOriginal = nullptr;

/**
 * @brief Gets the currently active camera offset
 * @details Determines which offset source to use based on configuration
 * @return Vector3 The local space offset to apply
 */
static Vector3 GetActiveOffset()
{
    // Priority 1: Active transition
    if (g_config.enable_camera_profiles)
    {
        Vector3 transitionPosition;
        Quaternion transitionRotation;

        // Check if a transition is in progress
        if (TransitionManager::getInstance().updateTransition(0.016f, transitionPosition, transitionRotation))
        {
            return transitionPosition;
        }

        // Priority 2: Camera profile system
        return g_currentCameraOffset;
    }

    // Priority 3: Static configuration offsets
    return Vector3(g_config.tpv_offset_x, g_config.tpv_offset_y, g_config.tpv_offset_z);
}

/**
 * @brief Detour function for TPV camera update
 * @details Intercepts the camera position calculation to apply custom offsets
 *          based on configuration, camera profiles, or transitions.
 * @param thisPtr Pointer to the camera object
 * @param outputPosePtr Pointer to output structure containing position/rotation
 */
static void __fastcall Detour_TpvCameraUpdate(uintptr_t thisPtr, uintptr_t outputPosePtr)
{
    Logger &logger = Logger::getInstance();

    // Call original function first to get base camera position
    if (!fpTpvCameraUpdateOriginal)
    {
        logger.log(LOG_ERROR, "TpvCameraHook: Original function pointer is NULL");
        return;
    }

    try
    {
        fpTpvCameraUpdateOriginal(thisPtr, outputPosePtr);
    }
    catch (const std::exception &e)
    {
        logger.log(LOG_ERROR, "TpvCameraHook: Exception in original function: " + std::string(e.what()));
        return;
    }
    catch (...)
    {
        logger.log(LOG_ERROR, "TpvCameraHook: Unknown exception in original function");
        return;
    }

    // Validate parameters and check if we're in TPV mode
    if (outputPosePtr == 0 || getViewState() != 1)
    {
        return;
    }

    // Validate output buffer is accessible
    if (!DMKMemory::isMemoryReadable(reinterpret_cast<void *>(outputPosePtr), Constants::TPV_OUTPUT_POSE_REQUIRED_SIZE))
    {
        logger.log(LOG_DEBUG, "TpvCameraHook: Output pose buffer not readable");
        return;
    }

    try
    {
        // Get pointers to position and rotation in the output structure
        Vector3 *positionPtr = reinterpret_cast<Vector3 *>(
            outputPosePtr + Constants::TPV_OUTPUT_POSE_POSITION_OFFSET);
        Quaternion *rotationPtr = reinterpret_cast<Quaternion *>(
            outputPosePtr + Constants::TPV_OUTPUT_POSE_ROTATION_OFFSET);

        // Read current camera state
        Vector3 currentPosition = *positionPtr;
        Quaternion currentRotation = *rotationPtr;

        // Determine which offset to apply (priority order: transition > profile > config)
        Vector3 localOffset = GetActiveOffset();

        // Skip if no offset to apply
        if (localOffset.x == 0.0f && localOffset.y == 0.0f && localOffset.z == 0.0f)
        {
            return;
        }

        // Transform local offset to world space using camera rotation
        Vector3 worldOffset = currentRotation.Rotate(localOffset);

        // Apply offset to camera position
        Vector3 newPosition = currentPosition + worldOffset;

        // Write back the modified position if memory is writable
        if (DMKMemory::isMemoryWritable(positionPtr, sizeof(Vector3)))
        {
            *positionPtr = newPosition;

            logger.log(LOG_TRACE, "TpvCameraHook: Applied offset - Local: " +
                                      Vector3ToString(localOffset) + " World: " + Vector3ToString(worldOffset));
        }
        else
        {
            logger.log(LOG_WARNING, "TpvCameraHook: Cannot write to position buffer");
        }
    }
    catch (const std::exception &e)
    {
        logger.log(LOG_ERROR, "TpvCameraHook: Exception applying offset: " + std::string(e.what()));
    }
    catch (...)
    {
        logger.log(LOG_ERROR, "TpvCameraHook: Unknown exception applying offset");
    }
}

bool initializeTpvCameraHook(uintptr_t moduleBase, size_t moduleSize)
{
    Logger &logger = Logger::getInstance();

    // Check if feature is disabled (all offsets zero and profiles disabled)
    if (!g_config.enable_camera_profiles &&
        g_config.tpv_offset_x == 0.0f &&
        g_config.tpv_offset_y == 0.0f &&
        g_config.tpv_offset_z == 0.0f)
    {
        logger.log(LOG_INFO, "TpvCameraHook: Feature disabled (no offsets configured)");
        return true;
    }

    logger.log(LOG_INFO, "TpvCameraHook: Initializing camera position offset hook...");

    try
    {
        // Use DMKHookManager to create hook via AOB scan
        DMKHookManager &hook_manager = DMKHookManager::getInstance();

        g_tpvCameraHookId = hook_manager.create_inline_hook_aob(
            "TpvCameraUpdate",
            moduleBase,
            moduleSize,
            Constants::TPV_CAMERA_UPDATE_AOB_PATTERN,
            0, // No offset from pattern
            reinterpret_cast<void *>(Detour_TpvCameraUpdate),
            reinterpret_cast<void **>(&fpTpvCameraUpdateOriginal));

        if (g_tpvCameraHookId.empty())
        {
            throw std::runtime_error("Failed to create TPV camera update hook via AOB scan");
        }

        // Get the target address for logging
        DMK::InlineHook *hook = hook_manager.get_inline_hook(g_tpvCameraHookId);
        if (hook)
        {
            logger.log(LOG_INFO, "TpvCameraHook: Found TPV camera update at " +
                                     format_address(hook->getTargetAddress()));
        }

        // Log configuration
        logger.log(LOG_INFO, "TpvCameraHook: Successfully installed with configuration:");

        if (g_config.enable_camera_profiles)
        {
            logger.log(LOG_INFO, "  - Camera profiles: ENABLED");
        }
        else
        {
            logger.log(LOG_INFO, "  - Static offset: X=" + std::to_string(g_config.tpv_offset_x) +
                                     " Y=" + std::to_string(g_config.tpv_offset_y) +
                                     " Z=" + std::to_string(g_config.tpv_offset_z));
        }

        return true;
    }
    catch (const std::exception &e)
    {
        logger.log(LOG_ERROR, "TpvCameraHook: Initialization failed: " + std::string(e.what()));
        cleanupTpvCameraHook();
        return false;
    }
}

void cleanupTpvCameraHook()
{
    Logger &logger = Logger::getInstance();

    if (!g_tpvCameraHookId.empty())
    {
        DMKHookManager &hook_manager = DMKHookManager::getInstance();

        if (hook_manager.remove_hook(g_tpvCameraHookId))
        {
            logger.log(LOG_INFO, "TpvCameraHook: Successfully removed");
        }
        else
        {
            logger.log(LOG_WARNING, "TpvCameraHook: Failed to remove hook");
        }

        g_tpvCameraHookId.clear();
        fpTpvCameraUpdateOriginal = nullptr;
    }
}
