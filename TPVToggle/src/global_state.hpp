/**
 * @file global_state.hpp
 * @brief Cross-module shared state grouped behind accessor functions.
 *
 * @details Related state lives in small grouped structs reached through
 *          reference-returning accessors (C++ Core Guideline I.2: avoid
 *          non-const globals). Each accessor owns a single function-local
 *          static, so storage is constructed on first use with no
 *          static-initialization-order dependency between translation units.
 */
#ifndef GLOBAL_STATE_HPP
#define GLOBAL_STATE_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "game_structures.hpp"
#include "constants.hpp"
#include "math_utils.hpp"
#include "camera_offset_state.hpp"

// Resolved base address of the game's global context. Kept as a stable
// unmangled symbol (rather than namespaced state) so it can be located from
// external tooling such as Cheat Engine or x64dbg during reverse-engineering.
extern "C"
{
    extern std::byte *g_global_context_ptr_address;
}

namespace TPVToggle
{
    /** @brief Base address and image size of the resolved game module. */
    struct ModuleInfo
    {
        std::uintptr_t base{0};
        std::size_t size{0};
    };

    /** @brief Atomics coordinating overlay/menu-driven view transitions. */
    struct OverlayState
    {
        std::atomic<bool> active{false};
        std::atomic<bool> fpvRequest{false};
        std::atomic<bool> tpvRestoreRequest{false};
        std::atomic<bool> wasTpvBeforeOverlay{false};
        std::atomic<bool> holdToScrollActive{false};
    };

    /** @brief State owned by the scroll-accumulator event hook. */
    struct ScrollHookState
    {
        std::byte *writeAddress{nullptr};
        std::byte originalWriteBytes[Constants::ACCUMULATOR_WRITE_INSTR_LENGTH]{};
        volatile float *accumulatorAddress{nullptr};
        volatile std::uintptr_t *ptrStorageAddress{nullptr};
        std::atomic<bool> nopped{false};
    };

    /** @brief Live third-person camera state shared by hooks and the profile system. */
    struct TpvCameraState
    {
        Vector3 latestForward{0.0f, 1.0f, 0.0f};
        CameraOffsetState offset;
        std::atomic<bool> adjustmentMode{false};
    };

    /** @brief Signature of the engine's CEntity::SetWorldTM (this, 3x4 matrix, flags). */
    using CEntitySetWorldTMFn = void (*)(GameStructures::CEntity *this_ptr, float *tm_3x4, int flags);

    /** @brief Player world transform mirrored from the entity hooks. */
    struct PlayerTransform
    {
        Vector3 worldPosition{0.0f, 0.0f, 0.0f};
        Quaternion worldOrientation{Quaternion::Identity()};
        GameStructures::CEntity *entity{nullptr};
        CEntitySetWorldTMFn setWorldTM{nullptr};
    };

    /** @brief Returns the process-wide resolved game module info. */
    [[nodiscard]] ModuleInfo &module_info() noexcept;
    /** @brief Returns the overlay/menu view-transition coordination state. */
    [[nodiscard]] OverlayState &overlay_state() noexcept;
    /** @brief Returns the scroll-accumulator event-hook state. */
    [[nodiscard]] ScrollHookState &scroll_hook_state() noexcept;
    /** @brief Returns the live third-person camera state. */
    [[nodiscard]] TpvCameraState &camera_state() noexcept;
    /** @brief Returns the mirrored player world transform. */
    [[nodiscard]] PlayerTransform &player_transform() noexcept;

} // namespace TPVToggle

#endif // GLOBAL_STATE_HPP
