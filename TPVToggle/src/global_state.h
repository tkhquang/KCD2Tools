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
#include "game_structures.h"
#include "constants.h"
#include "math_utils.h"

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

extern Vector3 g_playerWorldPosition;
extern Quaternion g_playerWorldOrientation;

extern GameStructures::CEntity *g_thePlayerEntity;
typedef void (*CEntity_SetWorldTM_Func_t)(GameStructures::CEntity *this_ptr, float *tm_3x4, int flags);
extern CEntity_SetWorldTM_Func_t g_funcCEntitySetWorldTM;

#endif // GLOBAL_STATE_H
