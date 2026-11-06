/**
 * @file forced_options.hpp
 * @brief The option a script asks for when it plays a fragment for Henry on tags he does not have.
 *
 * PlayAnim switches the tags it is given on in the character's tag state, and the game may switch them off again
 * before the fragment picks its option: it rebuilds Henry's tags on every tag update, so MoveSpeed (walk, run, sprint)
 * follows his speed and a standing Henry's `run` is gone by then, which leaves the fragment in the bind pose. A
 * temporary play on Henry (npc_actions.cpp) therefore takes its changes out of the tag state again at once and forces
 * them into his option queries for that fragment only (hooks/view_variants.cpp), until his next PlayAnim or the
 * force window (longer for a queued play) has passed. The fragment picks its option when it starts, so that is all it
 * needs.
 */
#ifndef ANIMFW_FORCED_OPTIONS_HPP
#define ANIMFW_FORCED_OPTIONS_HPP

#include "constants.hpp"

#include <array>
#include <cstdint>

namespace AnimationFramework::forced_options
{
    using TagState = std::array<std::uint8_t, constants::MANNEQUIN_TAG_STATE_BYTES>;

    /** @brief Records whether the option hooks are armed, which forcing needs. Set once by the hooks' setup. */
    void set_available(bool available) noexcept;

    /// Whether the option hooks are armed.
    [[nodiscard]] bool available() noexcept;

    /**
     * @brief The ID the play code finds for @p fragment in the controller definition of the SAnimationContext at
     *        @p context (the CRC lookup of CTagDefinition::Find), or -1 when it has none.
     * @note Main thread.
     */
    [[nodiscard]] std::int32_t resolve_fragment(std::uintptr_t context, const char *fragment) noexcept;

    /**
     * @brief Forces the bits that differ between @p before and @p after, at their value in @p after, into Henry's
     *        queries for fragment @p fragment_id (resolve_fragment) of the SAnimationContext at @p context. A @p queued
     *        play, which starts only when the fragment before it ends, keeps them longer.
     * @return False for a negative @p fragment_id or a definition with no plain `player` tag. Nothing is forced then.
     * @note Main thread, the only writer.
     */
    [[nodiscard]] bool force(
        std::uintptr_t context,
        std::int32_t fragment_id,
        const TagState &before,
        const TagState &after,
        bool queued
    ) noexcept;

    /** @brief Ends the forcing. Main thread. */
    void clear() noexcept;

    /** @brief Whether something is forced, a cheap first check for every query. */
    [[nodiscard]] bool active() noexcept;

    /**
     * @brief Puts the forced bits into @p global when it is a query for the forced fragment that carries `player`.
     * @return Whether it changed @p global.
     * @note Game animation threads. Lock-free and allocation-free.
     */
    [[nodiscard]] bool apply(std::int32_t fragment_id, std::uint8_t *global) noexcept;
} // namespace AnimationFramework::forced_options

#endif // ANIMFW_FORCED_OPTIONS_HPP
