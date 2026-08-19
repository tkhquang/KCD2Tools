/**
 * @file offset_heal.hpp
 * @brief Self-healing offset cache for the player / context pointer chains.
 *
 * The pointer-chain navigation in camera_hook.cpp and game_state.cpp walks fixed field offsets inside
 * structs whose base is resolved live (g_env / the anchored global context). A game patch that inserts or
 * removes a struct member shifts every field after it, and a hardcoded offset then reads the wrong slot.
 * This unit recovers the shifted offsets at runtime with DetourModKit's reverse-RTTI self-heal
 * (rtti_dissect.hpp): each in-scope offset is keyed to the MSVC mangled name of the object its slot points
 * at, and the heal scans a small window around the nominal offset for the slot that still resolves to that
 * type. Only the offset is cached (a ptrdiff_t), never an absolute address, so the recovered value stays
 * valid across instances and sessions.
 *
 * The cadence, the per-group success latch, and the one-shot "the layout drifted" warning are NOT
 * hand-rolled here: they are rtti::HealScheduler, the library's render-loop driver. This unit contributes
 * only what is mod-specific - the landmark tables, the corroborated top-of-struct bracket, and the live
 * bases each group heals from. The render path publishes a base with note_*_base() as it resolves one and
 * calls offset_heal_tick() once per frame; a group whose base is not up yet is skipped by its gate without
 * spending the retry budget or logging.
 *
 * The cache holds the current nominal offsets until a heal runs, so behaviour is identical to a hardcoded
 * build until a layout actually drifts. Healing is strictly fail-closed: an unrecoverable layout leaves the
 * nominal offset in place (degrades to current behaviour, never a guessed offset, never a crash), exactly
 * as DetourModKit's heal primitives guarantee. Each slot is an rtti::HealedSlot rather than a bare atomic,
 * so it publishes {value, generation, validity} and a consumer that AUTHORIZES A WRITE through the offset
 * can demand Confirmed (write_authorized_offset) instead of silently writing through a retained nominal.
 * Read-only navigation keeps using the retained value (offset_value), which is what preserves the
 * degrade-to-hardcoded behaviour.
 */
#ifndef TPVCAMERA_OFFSET_HEAL_HPP
#define TPVCAMERA_OFFSET_HEAL_HPP

#include "constants.hpp"

#include <DetourModKit/error.hpp>
#include <DetourModKit/rtti_dissect.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace TPVCamera
{

    /**
     * @struct RuntimeOffsets
     * @brief Live (possibly healed) copies of the in-scope pointer-chain field offsets.
     * @details Each field is an rtti::HealedSlot: a single-producer seqlock channel publishing the offset,
     *          the resolving image generation, and a validity, so the render thread can read it per frame
     *          while a heal writes it once. Every field is seeded with its constants.hpp nominal at
     *          construction (Unverified, generation 0), so before any heal the cache reproduces the
     *          hardcoded build exactly while still reporting that the value carries no evidence. Writes
     *          happen only inside the heal groups registered by start_offset_heal().
     */
    struct RuntimeOffsets
    {
        RuntimeOffsets() noexcept;

        DMK::rtti::HealedSlot ccryaction_actiongame;
        DMK::rtti::HealedSlot cactiongame_local_actor;
        DMK::rtti::HealedSlot c_player_entity;
        DMK::rtti::HealedSlot c_player_look_controller;
        DMK::rtti::HealedSlot c_player_animated_human;
        DMK::rtti::HealedSlot c_player_actor_model;
        DMK::rtti::HealedSlot c_player_missile_controller;
        DMK::rtti::HealedSlot animated_human_animchar;
        DMK::rtti::HealedSlot context_manager;
        DMK::rtti::HealedSlot context_minigame_subsystem;
    };

    /**
     * @brief Returns the process-wide runtime offset cache.
     * @details Backed by a single function-local static (no static-init-order dependency, like the other
     *          shared state in global_state.hpp). Constructed on first use with every field seeded at its
     *          nominal.
     */
    [[nodiscard]] RuntimeOffsets &runtime_offsets() noexcept;

    /**
     * @brief Reads a healed offset for READ-ONLY chain navigation.
     * @param slot The cache slot.
     * @return The healed offset once a heal confirms one, otherwise the seeded nominal.
     * @details This is the degrade-to-hardcoded path: a walk that only reads through the offset is no worse
     *          off with the nominal than a build that never healed at all, so it takes the retained value
     *          without demanding evidence. Use write_authorized_offset() instead whenever the offset decides
     *          where a WRITE lands.
     * @note Callback-safe: a bounded seqlock read, no allocation, locking, or I/O.
     */
    [[nodiscard]] std::ptrdiff_t offset_value(const DMK::rtti::HealedSlot &slot) noexcept;

    /**
     * @brief Reads a healed offset that is about to authorize a WRITE into a game struct.
     * @param slot The cache slot.
     * @return The offset when the slot is Confirmed, or ErrorCode::OffsetNotConfirmed when no heal has
     *         established it.
     * @details A retained nominal is safe to READ through (worst case it reads a neighbouring field) but not
     *          to WRITE through: on a genuinely drifted layout the store lands in whatever member now
     *          occupies that slot. Fail closed instead, and skip the write for the frame.
     * @note Callback-safe: a bounded seqlock read, no allocation, locking, or I/O.
     */
    [[nodiscard]] DMK::Result<std::ptrdiff_t> write_authorized_offset(const DMK::rtti::HealedSlot &slot) noexcept;

    /**
     * @brief Starts the self-heal scheduler and registers every heal group.
     * @details Call once from init(), on the init thread, before any detour is armed. Each group is gated on
     *          the live base the render path publishes through the note_*_base() calls below, so registering
     *          them all up front costs nothing until the corresponding object exists.
     * @note Setup and control plane only: allocates the scheduler and its groups.
     */
    void start_offset_heal();

    /**
     * @brief Advances the self-heal scheduler by one frame.
     * @details Render-thread only, once per frame. Every un-latched group whose gate passes and whose retry
     *          interval is due runs its scan; a group that resolves latches and stops being scanned. A no-op
     *          before start_offset_heal() and after every group has latched.
     */
    void offset_heal_tick() noexcept;

    /**
     * @brief Publishes the live CCryAction framework base for the chain-root heal group.
     * @param cry_action Live CCryAction framework base address, or 0 when it is not resolved.
     */
    void note_framework_base(std::uintptr_t cry_action) noexcept;

    /**
     * @brief Publishes the live CActionGame base for the local-actor recovery group.
     * @param action_game Live CActionGame base address, or 0 when it is not resolved.
     */
    void note_action_game_base(std::uintptr_t action_game) noexcept;

    /**
     * @brief Publishes a live, RTTI-VALIDATED C_Player base for the player-rooted heal groups.
     * @param c_player Live C_Player base address whose vtable already matched C_PLAYER_RTTI_NAME, or 0.
     */
    void note_player_base(std::uintptr_t c_player) noexcept;

    /**
     * @brief Publishes the resolved global-context base for the context-member heal groups.
     * @param context Resolved global-context object base address, or 0.
     */
    void note_context_base(std::uintptr_t context) noexcept;

    /**
     * @brief Asks the local-actor group to recover the CActionGame local-actor offset.
     * @details C_Player is found THROUGH that offset, so it cannot be healed from a resolved C_Player. The
     *          resolver calls this when the cached slot holds a populated object that is NOT a C_Player (the
     *          signature of a CActionGame layout drift); the group then scans CActionGame for the C_Player
     *          slot on its next due frame and latches once it resolves. The recovered offset is picked up by
     *          the resolver's normal read on a later frame, so no caller has to wait on the scan.
     */
    void request_local_actor_recovery() noexcept;

    /**
     * @brief Copies the accumulated per-landmark drift report.
     * @param out Destination; at most out.size() entries are written.
     * @return The number of entries written.
     * @details One rtti::DriftEntry per heal this session recorded, built from the heal results the groups
     *          already produced (rtti::heal_report is the one-base batch form; these landmarks span four
     *          bases that come up at different times, so re-running it would mean a second scan). Feeds
     *          diagnostics::collect() so the shutdown health line reports the layout state.
     */
    [[nodiscard]] std::size_t offset_heal_drift_report(std::span<DMK::rtti::DriftEntry> out) noexcept;

} // namespace TPVCamera

#endif // TPVCAMERA_OFFSET_HEAL_HPP
