/**
 * @file global_state.cpp
 * @brief Definitions of all global variables shared across the mod.
 */

#include "global_state.hpp"
#include <DetourModKit.hpp>
#include "utils.hpp"
#include "game_structures.hpp"
#include "constants.hpp"

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
    std::byte *g_global_context_ptr_address = nullptr;
}

// Event hook globals
std::byte *g_accumulatorWriteAddress = nullptr;
std::byte g_originalAccumulatorWriteBytes[Constants::ACCUMULATOR_WRITE_INSTR_LENGTH] = {std::byte{0}};
volatile float *g_scrollAccumulatorAddress = nullptr;
volatile uintptr_t *g_scrollPtrStorageAddress = nullptr;

// Thread communication atomics
std::atomic<bool> g_isOverlayActive(false);
std::atomic<bool> g_overlayFpvRequest(false);
std::atomic<bool> g_overlayTpvRestoreRequest(false);
std::atomic<bool> g_wasTpvBeforeOverlay(false);
std::atomic<bool> g_accumulatorWriteNOPped(false);
std::atomic<bool> g_holdToScrollActive(false);

Vector3 g_latestTpvCameraForward = {0.0f, 1.0f, 0.0f};

CameraOffsetState g_currentCameraOffset;
std::atomic<bool> g_cameraAdjustmentMode(false);

Vector3 g_playerWorldPosition(0.0f, 0.0f, 0.0f);
Quaternion g_playerWorldOrientation = Quaternion::Identity();

GameStructures::CEntity *g_thePlayerEntity = nullptr;
CEntity_SetWorldTM_Func_t g_funcCEntitySetWorldTM = nullptr;
