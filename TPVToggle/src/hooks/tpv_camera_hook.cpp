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
#include "game_structures.h"
#include "utils.h"
#include "aob_scanner.h"
#include "global_state.h"
#include "game_interface.h"
#include "math_utils.h"
#include "config.h"
#include "transition_manager.h"

#include "MinHook.h"

#include <DirectXMath.h>
#include <stdexcept>

// External configuration reference
extern Config g_config;

// Function typedef for the TPV camera update function
// RCX: C_CameraThirdPerson object pointer
// RDX: Pointer to output pose structure (position + quaternion)
typedef void(__fastcall *TpvCameraUpdateFunc)(uintptr_t thisPtr, uintptr_t outputPosePtr);

// Hook state
static TpvCameraUpdateFunc fpTpvCameraUpdateOriginal = nullptr;
static BYTE *g_tpvCameraHookAddress = nullptr;

/**
 * @brief Gets the currently active camera offset
 * @details Determines which offset source to use based on configuration
 * @return Vector3 The local space offset to apply
 */
Vector3 GetActiveOffset()
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
void __fastcall Detour_TpvCameraUpdate(uintptr_t thisPtr, uintptr_t outputPosePtr)
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
    if (!isMemoryReadable(reinterpret_cast<void *>(outputPosePtr), Constants::TPV_OUTPUT_POSE_REQUIRED_SIZE))
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
        if (isMemoryWritable(positionPtr, sizeof(Vector3)))
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
        // Parse AOB pattern
        std::vector<BYTE> pattern = parseAOB(Constants::TPV_CAMERA_UPDATE_AOB_PATTERN);
        if (pattern.empty())
        {
            throw std::runtime_error("Failed to parse TPV camera update AOB pattern");
        }

        // Find the target function
        g_tpvCameraHookAddress = FindPattern(
            reinterpret_cast<BYTE *>(moduleBase), moduleSize, pattern);
        if (!g_tpvCameraHookAddress)
        {
            throw std::runtime_error("TPV camera update pattern not found");
        }

        logger.log(LOG_INFO, "TpvCameraHook: Found TPV camera update at " +
                                 format_address(reinterpret_cast<uintptr_t>(g_tpvCameraHookAddress)));

        // Create the hook
        MH_STATUS status = MH_CreateHook(
            g_tpvCameraHookAddress,
            reinterpret_cast<LPVOID>(&Detour_TpvCameraUpdate),
            reinterpret_cast<LPVOID *>(&fpTpvCameraUpdateOriginal));

        if (status != MH_OK)
        {
            throw std::runtime_error("MH_CreateHook failed: " + std::string(MH_StatusToString(status)));
        }

        if (!fpTpvCameraUpdateOriginal)
        {
            MH_RemoveHook(g_tpvCameraHookAddress);
            throw std::runtime_error("MH_CreateHook returned NULL trampoline");
        }

        // Enable the hook
        status = MH_EnableHook(g_tpvCameraHookAddress);
        if (status != MH_OK)
        {
            MH_RemoveHook(g_tpvCameraHookAddress);
            throw std::runtime_error("MH_EnableHook failed: " + std::string(MH_StatusToString(status)));
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

    if (g_tpvCameraHookAddress)
    {
        MH_STATUS disableStatus = MH_DisableHook(g_tpvCameraHookAddress);
        MH_STATUS removeStatus = MH_RemoveHook(g_tpvCameraHookAddress);

        if (disableStatus == MH_OK && removeStatus == MH_OK)
        {
            logger.log(LOG_INFO, "TpvCameraHook: Successfully removed");
        }
        else
        {
            logger.log(LOG_WARNING, "TpvCameraHook: Cleanup issues - Disable: " +
                                        std::string(MH_StatusToString(disableStatus)) +
                                        ", Remove: " + std::string(MH_StatusToString(removeStatus)));
        }

        g_tpvCameraHookAddress = nullptr;
        fpTpvCameraUpdateOriginal = nullptr;
    }
}
