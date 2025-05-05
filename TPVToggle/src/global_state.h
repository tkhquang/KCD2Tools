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
#include "constants.h"

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

// Game interface globals
extern "C"
{
    extern BYTE *g_global_context_ptr_address;
    extern uintptr_t *g_rbx_for_overlay_flag;
}

// Event hook globals
extern BYTE *g_accumulatorWriteAddress;
extern BYTE g_originalAccumulatorWriteBytes[Constants::ACCUMULATOR_WRITE_INSTR_LENGTH];

// Thread communication atomics
extern std::atomic<bool> g_isOverlayActive;
extern std::atomic<bool> g_overlayFpvRequest;
extern std::atomic<bool> g_overlayTpvRestoreRequest;
extern std::atomic<bool> g_wasTpvBeforeOverlay;
extern std::atomic<bool> g_accumulatorWriteNOPped;

#endif // GLOBAL_STATE_H
