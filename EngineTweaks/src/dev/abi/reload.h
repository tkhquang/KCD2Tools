/**
 * @file dev/abi/reload.h
 * @brief Fixed-width contract between the resident dev loader and one logic generation.
 *
 * The loader outlives every generation, so everything it hands across the DLL boundary must have a layout both sides
 * agree on with no shared C++ ABI. A plain C struct with an explicit size and version does that: the logic DLL
 * validates both before touching a field, so a stale generation left in the deploy directory fails loudly instead of
 * reading through a shifted layout.
 *
 * Init returns DMK_STAGED_RELOAD_OK for a live generation and zero for a failed one. Shutdown returns a
 * DetourModKit/abi/staged_reload.h value.
 */
#ifndef ENGINETWEAKS_DEV_ABI_RELOAD_H
#define ENGINETWEAKS_DEV_ABI_RELOAD_H

#include <DetourModKit/abi/staged_reload.h>
#include <DetourModKit/abi/wheel_host.h>

#include <stdint.h>

/// Bump whenever the request layout, the persistent-state layout, the export signatures or their results change.
#define ENGINETWEAKS_RELOAD_ABI_VERSION 3u

/// EngineTweaksPersistentState::magic: ASCII "ENGTWKPS".
#define ENGINETWEAKS_PERSISTENT_MAGIC UINT64_C(0x454E4754574B5053)

/// Number of 64-bit slots in EngineTweaksPersistentState.
#define ENGINETWEAKS_PERSISTENT_SLOT_COUNT 32u

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @struct EngineTweaksPersistentState
     * @brief Loader-owned storage that survives every generation swap.
     * @details The loader zero-fills it once per process, stamps the header, and never reads the slots. The logic DLL
     *          owns the slot layout and tags it inside the slots, so a generation that finds a layout it does not know
     *          discards the contents instead of misreading them. Each slot is 8-byte aligned, so a generation can use
     *          it as an atomic word.
     */
    typedef struct EngineTweaksPersistentState
    {
        /// sizeof(EngineTweaksPersistentState) as the LOADER knows it.
        uint32_t struct_size;
        /// ENGINETWEAKS_RELOAD_ABI_VERSION as the loader knows it.
        uint32_t abi_version;
        /// ENGINETWEAKS_PERSISTENT_MAGIC.
        uint64_t magic;
        /// Generation-defined contents. All zero until a generation writes them.
        uint64_t slots[ENGINETWEAKS_PERSISTENT_SLOT_COUNT];
    } EngineTweaksPersistentState;

    /**
     * @struct EngineTweaksReloadInitRequest
     * @brief What the loader tells a generation at startup.
     */
    typedef struct EngineTweaksReloadInitRequest
    {
        /// sizeof(EngineTweaksReloadInitRequest) as the LOADER knows it.
        uint32_t struct_size;
        /// ENGINETWEAKS_RELOAD_ABI_VERSION as the loader knows it.
        uint32_t abi_version;
        /// Loader-assigned, strictly increasing, never zero.
        uint64_t generation_id;
        /// The identity the logic DLL must find in wheel_host, so a foreign table is rejected.
        uint64_t expected_host_identity;
        /// Process-lifetime wheel host owned by the loader, or NULL. Valid for the whole process when present.
        const WheelHostTable *wheel_host;
        /// Process-lifetime state area owned by the loader. Never NULL on a valid request.
        EngineTweaksPersistentState *persistent;
    } EngineTweaksReloadInitRequest;

    // Exports the loader resolves by name on every generation.
#define ENGINETWEAKS_RELOAD_INIT_SYMBOL "Init"
#define ENGINETWEAKS_RELOAD_SHUTDOWN_SYMBOL "Shutdown"
#define ENGINETWEAKS_RELOAD_REVISION_SYMBOL "Revision"

#ifdef __cplusplus
}
#endif

#endif // ENGINETWEAKS_DEV_ABI_RELOAD_H
