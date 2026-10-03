/**
 * @file offset_heal.hpp
 * @brief Self-healing offset cache for the player and global-context pointer chains.
 *
 * The player chain (engine_env.cpp) and the gate reads (game_state.cpp) walk fixed field offsets inside structs
 * whose base is resolved live. A game patch that inserts a struct member shifts every later field, and a hardcoded
 * offset then reads the wrong slot. This unit recovers shifted offsets at runtime with DetourModKit's reverse-RTTI
 * self-heal: each offset is keyed to the mangled name of the object its slot points at, and the heal scans a window
 * around the nominal offset for the slot that still resolves to that type. Only the offset is cached, never an
 * absolute address.
 *
 * The cadence, the per-group latch and the one-shot drift warning are rtti::HealScheduler's. The main thread
 * publishes each live base with note_*_base() as it resolves one and calls offset_heal_tick() on its own cadence; a
 * group whose base is not live yet is skipped by its gate. An unresolved slot retains its
 * nominal offset for guarded reads only. A nominal value never authorizes a write.
 */
#ifndef HENRYSENSES_OFFSET_HEAL_HPP
#define HENRYSENSES_OFFSET_HEAL_HPP

#include <DetourModKit/error.hpp>
#include <DetourModKit/rtti_dissect.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace HenrySenses
{
    /**
     * @struct RuntimeOffsets
     * @brief Live (possibly healed) copies of the in-scope pointer-chain field offsets.
     * @details Each field is an rtti::HealedSlot seeded with its constants.hpp nominal (Unverified), so a read before
     *          any heal reproduces the hardcoded build while still reporting that the value carries no evidence.
     */
    struct RuntimeOffsets
    {
        /** @brief Seeds every slot with its nominal offset. */
        RuntimeOffsets() noexcept;

        DMK::rtti::HealedSlot ccryaction_actiongame;
        DMK::rtti::HealedSlot cactiongame_local_actor;
        DMK::rtti::HealedSlot c_player_entity;
        DMK::rtti::HealedSlot context_manager;
        DMK::rtti::HealedSlot context_minigame_subsystem;
    };

    /** @brief Returns the process-wide runtime offset cache. */
    [[nodiscard]] RuntimeOffsets &runtime_offsets() noexcept;

    /**
     * @brief Reads a healed offset for READ-ONLY chain navigation.
     * @param slot The cache slot.
     * @return The healed offset once a heal confirms one, otherwise the seeded nominal.
     * @note Callback-safe: a bounded seqlock read.
     */
    [[nodiscard]] std::ptrdiff_t offset_value(const DMK::rtti::HealedSlot &slot) noexcept;

    /**
     * @brief Starts the self-heal scheduler and registers every heal group.
     * @note Setup and control plane only. Call once from init().
     */
    void start_offset_heal();

    /**
     * @brief Advances the self-heal scheduler by one step.
     * @details Call from one thread only (the main-thread command handler owns the cadence). A no-op before
     *          start_offset_heal() and after every group has latched.
     */
    void offset_heal_tick() noexcept;

    /**
     * @brief Publishes the live CCryAction base for the chain-root heal group.
     * @param cry_action CCryAction address, or 0.
     */
    void note_framework_base(std::uintptr_t cry_action) noexcept;

    /**
     * @brief Publishes the live CActionGame base for the local-actor recovery group.
     * @param action_game CActionGame address, or 0.
     */
    void note_action_game_base(std::uintptr_t action_game) noexcept;

    /**
     * @brief Publishes a live, RTTI-validated C_Player base for the player-rooted heal group.
     * @param c_player C_Player address whose vtable already matched, or 0.
     */
    void note_player_base(std::uintptr_t c_player) noexcept;

    /**
     * @brief Publishes the resolved global-context object base for the context-member heal groups.
     * @param context Global-context object address, or 0.
     */
    void note_context_base(std::uintptr_t context) noexcept;

    /**
     * @brief Asks the local-actor group to recover the CActionGame local-actor offset.
     * @details C_Player is found THROUGH that offset, so it cannot be healed from a resolved C_Player. The resolver
     *          calls this when the slot holds a populated object that is not a C_Player, the signature of a layout
     *          drift; the group then scans CActionGame on its next due step and latches once it resolves.
     */
    void request_local_actor_recovery() noexcept;

    /**
     * @brief Copies the accumulated per-landmark drift report.
     * @param out Destination; at most out.size() entries are written.
     * @return The number of entries written.
     */
    [[nodiscard]] std::size_t offset_heal_drift_report(std::span<DMK::rtti::DriftEntry> out) noexcept;

} // namespace HenrySenses

#endif // HENRYSENSES_OFFSET_HEAL_HPP
