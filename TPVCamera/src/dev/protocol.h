/**
 * @file protocol.h
 * @brief Fixed-width contract between the resident dev loader and one logic generation.
 *
 * The loader outlives every generation, so everything it hands across the DLL boundary must have a
 * layout both sides agree on without sharing a C++ ABI. A plain C struct with an explicit size and
 * version does that: the logic DLL validates both before touching a field, so a stale generation left
 * in the deploy directory fails loudly instead of reading through a shifted layout.
 *
 * Modelled on DetourModKit's checked-in staged_reload example, which the hot-reload guide treats as the
 * reference pair.
 */
#ifndef KCD2_TPVCAMERA_PROTOCOL_H
#define KCD2_TPVCAMERA_PROTOCOL_H

#include <DetourModKit/abi/wheel_host.h>

#include <stdint.h>

/// Bump whenever the request layout or the export signatures change.
#define TPVCAMERA_RELOAD_ABI_VERSION 1u

/// Success value returned by the logic DLL's Init and Shutdown exports.
#define TPVCAMERA_RELOAD_OK 1u

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @struct TpvReloadInitRequest
     * @brief What the loader tells a generation at startup.
     */
    typedef struct TpvReloadInitRequest
    {
        /// sizeof(TpvReloadInitRequest) as the LOADER knows it.
        uint32_t struct_size;
        /// TPVCAMERA_RELOAD_ABI_VERSION as the loader knows it.
        uint32_t abi_version;
        /// Loader-assigned, strictly increasing, never zero.
        uint64_t generation_id;
        /// The identity the logic DLL must find in wheel_host, so a foreign table is rejected.
        uint64_t expected_host_identity;
        /// Process-lifetime wheel host owned by the loader. Valid for the whole process.
        const WheelHostTable *wheel_host;
    } TpvReloadInitRequest;

    /// Exports the loader resolves by name on every generation.
#define TPVCAMERA_RELOAD_INIT_SYMBOL "Init"
#define TPVCAMERA_RELOAD_SHUTDOWN_SYMBOL "Shutdown"
#define TPVCAMERA_RELOAD_REVISION_SYMBOL "Revision"

#ifdef __cplusplus
}
#endif

#endif // KCD2_TPVCAMERA_PROTOCOL_H
