/**
 * @file protocol.h
 * @brief Fixed-width contract between the resident dev loader and one logic generation.
 *
 * The loader outlives every generation, so the request it hands across the DLL boundary is a plain C struct
 * with an explicit size and version. The logic DLL validates both before it reads a field. The request layout,
 * the export signatures and each export's result values form one versioned contract (TPVCAMERA_RELOAD_ABI_VERSION).
 * Init returns DMK_STAGED_RELOAD_OK for a live generation and zero for a failed one. Shutdown returns a
 * DetourModKit/abi/staged_reload.h value.
 */
#ifndef KCD2_TPVCAMERA_PROTOCOL_H
#define KCD2_TPVCAMERA_PROTOCOL_H

#include <DetourModKit/abi/staged_reload.h>
#include <DetourModKit/abi/wheel_host.h>

#include <stdint.h>

/** @brief ABI revision of the request, the export signatures and their result values. */
#define TPVCAMERA_RELOAD_ABI_VERSION 3u

/**
 * @struct TpvReloadInitRequest
 * @brief Fixed-width request passed from the resident loader to one logic generation.
 */
typedef struct TpvReloadInitRequest
{
    /** @brief The request size known to the loader. */
    uint32_t struct_size;
    /** @brief The request ABI revision known to the loader. */
    uint32_t abi_version;
    /** @brief The loader-assigned generation id: strictly increasing, never zero. */
    uint64_t generation_id;
    /** @brief The identity the logic DLL must find in wheel_host, so a foreign table is rejected. */
    uint64_t expected_host_identity;
    /** @brief The process-lifetime resident wheel host. */
    const WheelHostTable *wheel_host;
} TpvReloadInitRequest;

#endif /* KCD2_TPVCAMERA_PROTOCOL_H */
