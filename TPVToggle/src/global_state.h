/**
 * @file global_state.h
 * @brief Header for all global variables shared across the mod.
 *
 * This header provides declarations (not definitions) for all global variables,
 * preventing multiple definition errors during linking.
 */
#ifndef GLOBAL_STATE_H
#define GLOBAL_STATE_H

#include <windows.h>
#include <cstdint>
#include <atomic>
#include <mutex>
#include "constants.h"
#include "math_utils.h"

// Forward declare Logger to avoid circular dependencies
class Logger;

// External function declarations
extern bool WriteBytes(BYTE *targetAddress, const BYTE *sourceBytes, size_t numBytes, Logger &logger);

// Global module information
extern uintptr_t g_ModuleBase;
extern size_t g_ModuleSize;

// Thread control
extern HANDLE g_exitEvent;

// Thread handles (for cleanup)
extern HANDLE g_hMonitorThread;
extern HANDLE g_hOverlayThread;
extern HANDLE g_hCameraProfileThread;

// Game interface globals
extern "C"
{
    extern BYTE *g_global_context_ptr_address;
    extern uintptr_t *g_rbx_for_overlay_flag;
    extern volatile BYTE *g_tpvFlagAddress;
}

// Event hook globals
extern BYTE *g_accumulatorWriteAddress;
extern BYTE g_originalAccumulatorWriteBytes[Constants::ACCUMULATOR_WRITE_INSTR_LENGTH];
extern volatile uintptr_t *g_scrollAccumulatorAddress;
extern volatile uintptr_t *g_scrollPtrStorageAddress;

// Thread communication atomics
extern std::atomic<bool> g_isOverlayActive;
extern std::atomic<bool> g_overlayFpvRequest;
extern std::atomic<bool> g_overlayTpvRestoreRequest;
extern std::atomic<bool> g_wasTpvBeforeOverlay;
extern std::atomic<bool> g_accumulatorWriteNOPped;
extern std::atomic<bool> g_holdToScrollActive;

extern Vector3 g_latestTpvCameraForward;

extern Vector3 g_currentCameraOffset;
extern std::atomic<bool> g_cameraAdjustmentMode;

// Orbital Camera
extern std::atomic<bool> g_orbitalModeActive;      // True if orbital camera mode is active
extern std::atomic<float> g_orbitalCameraYaw;      // Current yaw of the orbital camera (radians)
extern std::atomic<float> g_orbitalCameraPitch;    // Current pitch of the orbital camera (radians)
extern std::atomic<float> g_orbitalCameraDistance; // Current distance of camera from player
extern Quaternion g_orbitalCameraRotation;         // Combined final rotation quaternion for orbital cam
extern std::mutex g_orbitalCameraMutex;            // Mutex to protect concurrent access to yaw, pitch, distance, rotation

// Player State (updated by player_state_hook)
extern Vector3 g_playerWorldPosition;
extern Quaternion g_playerWorldOrientation;

// Pointer to the game's C_CameraThirdPerson thisPtr, if needed (EXPERIMENTAL - use with caution)
// extern std::atomic<uintptr_t> g_tpvCameraThisPtr;

// Pointer to game's CVars struct - set by CameraProfileThread
extern uintptr_t g_GlobalGameCVarsStructAddr;
extern std::atomic<bool> g_nativeOrbitCVarsEnabled; // For testing game's internal orbit CVars

// Replace the KCD2ModLoader 'big' namespace externs with your mod's own globals
extern Constants::CEntity *g_thePlayerEntity; // Your mod's global player pointer
typedef void (*CEntity_SetWorldTM_Func_t)(Constants::CEntity *this_ptr, float *tm_3x4, int flags);
extern CEntity_SetWorldTM_Func_t g_funcCEntitySetWorldTM; // Your mod's function pointer

#endif // GLOBAL_STATE_H
