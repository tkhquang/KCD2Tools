#include "tpv_camera_hook.h"
#include "logger.h"
#include "constants.h"
#include "utils.h"
#include "aob_scanner.h"
#include "global_state.h"
#include "game_interface.h" // For getViewState potentially, although maybe not needed
#include "math_utils.h"
#include "config.h" // Include config to access g_config offsets
#include "MinHook.h"

#include <DirectXMath.h> // For XMVector etc.
#include <sstream>
#include <iomanip>
#include <stdexcept> // For exception handling

// Helper to log quaternion
static std::string QuatToString(const Quaternion &q)
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(4)
        << "Q(" << q.x << ", " << q.y << ", " << q.z << ", " << q.w << ")";
    return oss.str();
}

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

// Detour
void __fastcall Detour_TpvCameraUpdate(uintptr_t thisPtr, uintptr_t outputPosePtr)
{
    Logger &logger = Logger::getInstance();
    bool original_called = false;

    // 1. Call original function first to calculate default pose
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
            // Depending on severity, might want to just return or try to continue carefully
            return; // Safest is usually to return
        }
    }
    else
    {
        logger.log(LOG_ERROR, "TpvCameraHook: Original function pointer NULL!");
        return; // Can't proceed
    }

    // Proceed only if original succeeded and we have a valid output pointer
    if (!original_called || outputPosePtr == 0)
    {
        return;
    }

    // Check if we are actually in TPV mode - Belt and suspenders check
    if (getViewState() != 1)
    {
        return; // Only apply offset in TPV
    }

    // Check if offsets are non-zero (minor optimization)
    if (g_config.tpv_offset_x == 0.0f && g_config.tpv_offset_y == 0.0f && g_config.tpv_offset_z == 0.0f)
    {
        return; // No offset to apply
    }

    // Rate limit logging
    static std::chrono::steady_clock::time_point last_log_time_cam;
    bool enableDetailedLogging = false;
    auto now = std::chrono::steady_clock::now();
    if (logger.isDebugEnabled() && (now - last_log_time_cam > std::chrono::milliseconds(100)))
    { // Log max ~10 times/sec
        enableDetailedLogging = true;
        last_log_time_cam = now;
    }

    try
    {
        // 2. Read necessary data from outputPose (Check Readability!)
        // Adjust required size based on VERIFIED offsets
        if (!isMemoryReadable((void *)outputPosePtr, Constants::TPV_OUTPUT_POSE_REQUIRED_SIZE))
        {
            if (enableDetailedLogging)
                logger.log(LOG_WARNING, "TpvCameraHook: Output Pose buffer not readable: " + format_address(outputPosePtr));
            return;
        }

        // Pointers to data within the output structure (USING PLACEHOLDER OFFSETS)
        Vector3 *currentPosPtr = reinterpret_cast<Vector3 *>(outputPosePtr + Constants::TPV_OUTPUT_POSE_POSITION_OFFSET);
        Quaternion *currentRotPtr = reinterpret_cast<Quaternion *>(outputPosePtr + Constants::TPV_OUTPUT_POSE_ROTATION_OFFSET);

        Vector3 currentPos = *currentPosPtr;
        Quaternion currentRot = *currentRotPtr; // Assuming XYZW

        if (enableDetailedLogging)
        {
            std::ostringstream oss;
            oss << "TpvCameraHook: Read Original Pose: Pos(" << currentPos.x << "," << currentPos.y << "," << currentPos.z << ") "
                << QuatToString(currentRot); // Using helper from previous step
            logger.log(LOG_DEBUG, oss.str());
        }

        // 3. Calculate World Offset
        Vector3 localOffset(g_config.tpv_offset_x, g_config.tpv_offset_y, g_config.tpv_offset_z);
        Vector3 worldOffset = currentRot.Rotate(localOffset); // Rotate local offset by world rotation

        // 4. Calculate New Position
        Vector3 newPos = currentPos + worldOffset;

        // 5. Write New Position Back (Check Writable!)
        if (!isMemoryWritable((void *)currentPosPtr, sizeof(Vector3)))
        {
            if (enableDetailedLogging)
                logger.log(LOG_WARNING, "TpvCameraHook: Output Pose Position buffer not writable: " + format_address((uintptr_t)currentPosPtr));
            return;
        }

        *currentPosPtr = newPos;

        if (enableDetailedLogging)
        {
            logger.log(LOG_DEBUG, "TpvCameraHook: Applied Offset: (" + std::to_string(worldOffset.x) + "," + std::to_string(worldOffset.y) + "," + std::to_string(worldOffset.z) + ") -> NewPos: (" + std::to_string(newPos.x) + "," + std::to_string(newPos.y) + "," + std::to_string(newPos.z) + ")");
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
