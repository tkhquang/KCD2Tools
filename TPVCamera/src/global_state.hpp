/**
 * @file global_state.hpp
 * @brief cross-module shared state grouped behind accessor functions.
 *
 * @details Related state lives in small grouped structs reached through
 *          reference-returning accessors (C++ Core Guideline I.2: avoid
 *          non-const globals). Each accessor owns a single function-local
 *          static, so storage is constructed on first use with no
 *          static-initialization-order dependency between translation units.
 */
#ifndef TPVCAMERA_GLOBAL_STATE_HPP
#define TPVCAMERA_GLOBAL_STATE_HPP

#include "core/frame/frame_record.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

// Resolved base address of the game's global context. Kept as a stable unmangled symbol (rather than
// namespaced state) so it can be located from external tooling such as Cheat Engine or x64dbg during
// reverse-engineering. Atomic because the main thread reads it (the camera/minigame chain walks in
// game_state.cpp) while shutdown nulls it on the bootstrap thread before the hooks are removed; relaxed
// is sufficient (a standalone pointer, no dependent data published through it, and each chain walk
// independently validates the value it reads). std::atomic<std::byte *> is lock-free and layout-identical
// to a raw pointer on x64, so external tooling still reads it as a plain pointer.
extern "C"
{
    extern std::atomic<std::byte *> g_global_context_ptr_address;
}

namespace TPVCamera
{
    /** @brief Base address and image size of the resolved game module. */
    struct ModuleInfo
    {
        std::uintptr_t base{0};
        std::size_t size{0};
    };

    /** @brief Overlay (inventory/map/dialog/codex) presence, set by the UI overlay hooks. */
    struct OverlayState
    {
        std::atomic<bool> active{false};
    };

    /**
     * @brief The camera's toggles and requests that other threads set: the DMK hotkeys (the input poll thread) and the
     *        overlay.
     * @details Written by the hotkeys, the overlay and the frustum detour's policies; read by the frustum detour, the
     * input
     *          dispatcher and the ranged hook. Each flag guards no other data, so relaxed order is enough on every side.
     */
    struct CameraControls
    {
        // Runtime on/off of the offset, flipped by the view hotkeys and the forced-view policy. Starts off (first
        // person) so the game looks normal on load until the player toggles the third-person view.
        std::atomic<bool> applying{false};
        // Free-look is on: the orbit key toggles it, and the orbit-exclude policy suspends and restores it. While it is
        // on, the input dispatcher hook turns the orbit with the mouse and the stick (RigState::orbit_yaw).
        std::atomic<bool> orbit_active{false};
        // Posted by the OrbitHoldKey release before it turns free-look off, consumed by the orbit-exclude policy's
        // restore, and cleared by every engage (see apply_orbit_exclude_policy).
        std::atomic<bool> orbit_hold_released{false};
        // The shoulder the camera sits over, flipped by SwapShoulderKey: true puts the active preset's Offset Right on
        // the other side. A player choice, so it outlasts preset switches and first-person gaps.
        std::atomic<bool> shoulder_swapped{false};
        // Asks the camera frame to drop the zoom (a preset cycle, the overlay's Reset zoom). The frame hands the offset
        // to the preset ease, so the camera glides back instead of jumping.
        std::atomic<bool> zoom_reset_requested{false};
    };

    /** @brief The rig's values the overlay shows (RigState), as the camera frame last published them. */
    struct RigReadout
    {
        float zoom_offset = 0.0f;
        float real_eye_height = 0.0f;
        float eye_sync_effective = 0.0f;
        bool eye_sync_engaged = false;
    };

    /**
     * @brief Publishes the camera frame's record (LastFinal) for the hooks that re-origin onto it: the camera-space
     *        interaction, the ranged crosshair ray and the AI's camera observer.
     * @details The player use-cone (sub_180483740) is anchored on the EYE and never reads the render camera, so in
     *          third person the screen-centre crosshair and the use-target diverge by the shoulder offset (worst at
     *          close range - you cannot interact with what the crosshair is on); the interaction and ranged hooks
     *          re-origin onto the record's camera and crosshair direction. The AI's camera observer copies the offset
     *          view camera, so it is put back at the record's eye. The record is valid ONLY while the offset is engaged
     *          (in first person the camera IS the eye, so the hooks leave the game untouched).
     *
     *          The record is published as one seqlock snapshot (core/frame/channels.hpp). The producer and the
     *          consumers run at different points of the frame and on different threads, so reading its fields
     *          independently could mix a position from frame N with a direction from frame N+1 - an incoherent pose,
     *          not merely a stale one, which would aim the interaction ray at nothing. Every read observes one frame's
     *          record or fails closed (no record). A one-frame-stale but coherent record is harmless to its consumers.
     * @note Main thread (the frustum detour): every build that renders the offset, and an invalid record when it
     *       disengages.
     */
    void publish_last_final(const FrameRecord &record) noexcept;

    /**
     * @brief The camera frame's record as last published, or an invalid one when the read could not complete.
     * @note Any thread; lock-free.
     */
    [[nodiscard]] FrameRecord last_final() noexcept;

    /**
     * @brief The last frame's rendered camera and crosshair direction, when the offset rendered it.
     * @return False in first person or when the read could not complete; the out params are untouched then.
     */
    [[nodiscard]] bool last_final_pose(float &px, float &py, float &pz, float &dx, float &dy, float &dz) noexcept;

    /** @brief Returns the process-wide resolved game module info. */
    [[nodiscard]] ModuleInfo &module_info() noexcept;
    /** @brief Returns the overlay-presence state set by the UI overlay hooks. */
    [[nodiscard]] OverlayState &overlay_state() noexcept;
    /** @brief Returns the camera's toggles and requests (any thread). */
    [[nodiscard]] CameraControls &camera_controls() noexcept;

    /**
     * @brief Publishes the rig's read-outs for the overlay.
     * @note Main thread (the frustum detour), each engaged build.
     */
    void publish_rig_readout(const RigReadout &readout) noexcept;

    /** @brief The rig's read-outs as last published (any thread; lock-free). */
    [[nodiscard]] RigReadout rig_readout() noexcept;

    /**
     * @brief Returns the current debounced game-state mask (GameState bits, see game_state.hpp).
     * @details Written once per frame by the camera detour (main thread) and read by the input
     *          dispatch thread for the orbit-exclude gate, so it is atomic for a race-free read.
     */
    [[nodiscard]] std::atomic<uint32_t> &game_state_mask() noexcept;

    /**
     * @brief One-shot flag, set true once the player (C_Player) first resolves in-world.
     * @details Lets the overlay defer its size-sensitive setup (offscreen surface + DPI font scale)
     *          until the game is actually in gameplay - when the window is at its final resolution -
     *          instead of snapshotting the transient loading-window size. Set by the camera detour
     *          (main thread), read by the overlay thread.
     */
    [[nodiscard]] std::atomic<bool> &game_world_ready() noexcept;

} // namespace TPVCamera

#endif // TPVCAMERA_GLOBAL_STATE_HPP
