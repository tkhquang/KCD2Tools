/**
 * @file global_state.cpp
 * @brief Definitions of all global variables shared across the mod.
 */

#include "global_state.h"
#include "logger.h"
#include "utils.h"

// Define the WriteBytes function here since it's used in multiple places
bool WriteBytes(BYTE *targetAddress, const BYTE *sourceBytes, size_t numBytes, Logger &logger)
{
    if (!targetAddress || !sourceBytes || numBytes == 0)
        return false;

    DWORD oldProtect;
    if (!VirtualProtect(targetAddress, numBytes, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        logger.log(LOG_ERROR, "WriteBytes: VP (RW) fail: " + std::to_string(GetLastError()) + " @ " + format_address(reinterpret_cast<uintptr_t>(targetAddress)));
        return false;
    }

    memcpy(targetAddress, sourceBytes, numBytes);

    DWORD temp;
    if (!VirtualProtect(targetAddress, numBytes, oldProtect, &temp))
    {
        logger.log(LOG_WARNING, "WriteBytes: VP (Restore) fail: " + std::to_string(GetLastError()) + " @ " + format_address(reinterpret_cast<uintptr_t>(targetAddress)));
    }

    if (!FlushInstructionCache(GetCurrentProcess(), targetAddress, numBytes))
    {
        logger.log(LOG_WARNING, "WriteBytes: Cache flush failed after writing bytes to " + format_address(reinterpret_cast<uintptr_t>(targetAddress)));
    }

    return true;
}

// Module information
uintptr_t g_ModuleBase = 0;
size_t g_ModuleSize = 0;

// Thread control
HANDLE g_exitEvent = NULL;

// Thread handles
HANDLE g_hMonitorThread = NULL;
HANDLE g_hOverlayThread = NULL;
HANDLE g_hCameraProfileThread = NULL;

// Game interface globals
extern "C"
{
    BYTE *g_global_context_ptr_address = nullptr;
    uintptr_t *g_rbx_for_overlay_flag = nullptr;
    volatile BYTE *g_tpvFlagAddress = nullptr;
}

// Hook globals
extern "C"
{
    void *fpOverlay_OriginalCode = nullptr;
}

// Event hook globals
BYTE *g_accumulatorWriteAddress = nullptr;
BYTE g_originalAccumulatorWriteBytes[Constants::ACCUMULATOR_WRITE_INSTR_LENGTH] = {0};
volatile uintptr_t *g_scrollAccumulatorAddress = nullptr;
volatile uintptr_t *g_scrollPtrStorageAddress = nullptr;

// Thread communication atomics
std::atomic<bool> g_isOverlayActive(false);
std::atomic<bool> g_overlayFpvRequest(false);
std::atomic<bool> g_overlayTpvRestoreRequest(false);
std::atomic<bool> g_wasTpvBeforeOverlay(false);
std::atomic<bool> g_accumulatorWriteNOPped(false);
std::atomic<bool> g_holdToScrollActive(false);

Vector3 g_latestTpvCameraForward = {0.0f, 1.0f, 0.0f};

Vector3 g_currentCameraOffset(0.0f, 0.0f, 0.0f);
std::atomic<bool> g_cameraAdjustmentMode(false);

// Orbital Camera
std::atomic<bool> g_orbitalModeActive(false);
float g_orbitalCameraYaw = 0.0f;
float g_orbitalCameraPitch = 0.0f;
Quaternion g_orbitalCameraRotation = Quaternion::Identity();
float g_orbitalCameraDistance = 3.0f; // Default starting distance

Vector3 g_playerWorldPosition(0.0f, 0.0f, 0.0f);
Quaternion g_playerWorldOrientation = Quaternion::Identity();

uintptr_t g_GlobalGameCVarsStructAddr = 0;
std::atomic<bool> g_nativeOrbitCVarsEnabled(false);
