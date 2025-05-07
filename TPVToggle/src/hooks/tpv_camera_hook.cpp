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
    bool original_called = false;

    // Call original function first
    if (fpTpvCameraUpdateOriginal)
    {
        try
        {
            fpTpvCameraUpdateOriginal(thisPtr, outputPosePtr);
            original_called = true;
        }
        catch (...)
        {
            logger.log(LOG_ERROR, "TpvCameraHook: Exception calling original function!");
            return;
        }
    }
    else
    {
        logger.log(LOG_ERROR, "TpvCameraHook: Original function pointer NULL!");
        return;
    }

    // Skip if original failed or TPV mode not active
    if (!original_called || outputPosePtr == 0 || getViewState() != 1)
    {
        return;
    }

    try
    {
        // Check if output pose buffer is readable
        if (!isMemoryReadable((void *)outputPosePtr, Constants::TPV_OUTPUT_POSE_REQUIRED_SIZE))
        {
            return;
        }

        // Pointers to data within the output structure
        Vector3 *currentPosPtr = reinterpret_cast<Vector3 *>(outputPosePtr + Constants::TPV_OUTPUT_POSE_POSITION_OFFSET);
        Quaternion *currentRotPtr = reinterpret_cast<Quaternion *>(outputPosePtr + Constants::TPV_OUTPUT_POSE_ROTATION_OFFSET);

        Vector3 currentPos = *currentPosPtr;
        Quaternion currentRot = *currentRotPtr;

        // Get active offset - either from transition, profile system, or config
        Vector3 localOffset;

        // Check if transition is active and update if needed
        Vector3 transitionPosition;
        Quaternion transitionRotation;
        if (g_config.enable_camera_profiles &&
            TransitionManager::getInstance().updateTransition(0.016f, transitionPosition, transitionRotation))
        {
            // Use transitioning offset
            localOffset = transitionPosition;
        }
        else if (g_config.enable_camera_profiles)
        {
            // Use profile system offset
            localOffset = g_currentCameraOffset;
        }
        else
        {
            // Use static config offset
            localOffset = Vector3(g_config.tpv_offset_x, g_config.tpv_offset_y, g_config.tpv_offset_z);
        }

        // Calculate World Offset by rotating local offset by camera rotation
        Vector3 worldOffset = currentRot.Rotate(localOffset);

        // Apply offset to position
        Vector3 newPos = currentPos + worldOffset;

        // Write back if writable
        if (isMemoryWritable((void *)currentPosPtr, sizeof(Vector3)))
        {
            *currentPosPtr = newPos;
        }
    }
    catch (const std::exception &e)
    {
        logger.log(LOG_ERROR, "TpvCameraHook: Exception applying offset: " + std::string(e.what()));
    }
    catch (...)
    {
        logger.log(LOG_ERROR, "TpvCameraHook: Unknown exception applying offset.");
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
