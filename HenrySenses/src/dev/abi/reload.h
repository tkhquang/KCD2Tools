/**
 * @file dev/abi/reload.h
 * @brief Fixed-width contract between the resident dev loader and one logic generation.
 *
 * The loader outlives every generation, so everything it hands across the DLL boundary must have a layout both sides
 * agree on with no shared C++ ABI. A plain C struct with an explicit size and version does that: the logic DLL
 * validates both before touching a field, so a stale generation left in the deploy directory fails loudly instead of
 * reading through a shifted layout.
 *
 */
#ifndef HENRYSENSES_DEV_ABI_RELOAD_H
#define HENRYSENSES_DEV_ABI_RELOAD_H

#include <DetourModKit/abi/wheel_host.h>

#include <stdint.h>

/// Bump whenever the request layout, the persistent-state layout or the export signatures change.
#define HENRYSENSES_RELOAD_ABI_VERSION 2u

/// Success value returned by the logic DLL's Init and Shutdown exports. Zero is a refusal on both.
#define HENRYSENSES_RELOAD_OK 1u

/**
 * @brief Init failed and rollback could not prove the image unreferenced.
 * @details The loader must retain its module reference and stop reloading until the game restarts, even when
 *          the generation could not acquire its own permanent pin.
 */
#define HENRYSENSES_RELOAD_INIT_UNSAFE 2u

/// HenrySensesPersistentState::magic: ASCII "HSNPERST".
#define HENRYSENSES_PERSISTENT_MAGIC UINT64_C(0x48534E5045525354)

/// Number of 64-bit slots in HenrySensesPersistentState.
#define HENRYSENSES_PERSISTENT_SLOT_COUNT 32u

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @struct HenrySensesPersistentState
     * @brief Loader-owned storage that survives every generation swap.
     * @details The loader zero-fills it once per process, stamps the header, and never reads the slots. The logic DLL
     *          owns the slot layout and tags it inside the slots, so a generation that finds a layout it does not know
     *          discards the contents instead of misreading them. Each slot is 8-byte aligned, so a generation can use
     *          it as an atomic word.
     */
    typedef struct HenrySensesPersistentState
    {
        /// sizeof(HenrySensesPersistentState) as the LOADER knows it.
        uint32_t struct_size;
        /// HENRYSENSES_RELOAD_ABI_VERSION as the loader knows it.
        uint32_t abi_version;
        /// HENRYSENSES_PERSISTENT_MAGIC.
        uint64_t magic;
        /// Generation-defined contents. All zero until a generation writes them.
        uint64_t slots[HENRYSENSES_PERSISTENT_SLOT_COUNT];
    } HenrySensesPersistentState;

    /**
     * @struct HenrySensesReloadInitRequest
     * @brief What the loader tells a generation at startup.
     */
    typedef struct HenrySensesReloadInitRequest
    {
        /// sizeof(HenrySensesReloadInitRequest) as the LOADER knows it.
        uint32_t struct_size;
        /// HENRYSENSES_RELOAD_ABI_VERSION as the loader knows it.
        uint32_t abi_version;
        /// Loader-assigned, strictly increasing, never zero.
        uint64_t generation_id;
        /// The identity the logic DLL must find in wheel_host, so a foreign table is rejected.
        uint64_t expected_host_identity;
        /// Process-lifetime wheel host owned by the loader, or NULL. Valid for the whole process when present.
        const WheelHostTable *wheel_host;
        /// Process-lifetime state area owned by the loader. Never NULL on a valid request.
        HenrySensesPersistentState *persistent;
    } HenrySensesReloadInitRequest;

    // Exports the loader resolves by name on every generation.
#define HENRYSENSES_RELOAD_INIT_SYMBOL "Init"
#define HENRYSENSES_RELOAD_SHUTDOWN_SYMBOL "Shutdown"
#define HENRYSENSES_RELOAD_REVISION_SYMBOL "Revision"

#ifdef __cplusplus
}
#endif

#endif // HENRYSENSES_DEV_ABI_RELOAD_H
