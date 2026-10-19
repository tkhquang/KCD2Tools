/**
 * @file game/entity_tracker.hpp
 * @brief World positions of the entities packs name in `ReplacesFor`, refreshed every frame on the main thread.
 *
 * The entity system is not thread safe, so only the main-thread hook reads it; the audio thread reads the published
 * positions when it decides whether an event start belongs to a named entity.
 */
#ifndef SNDFW_GAME_ENTITY_TRACKER_HPP
#define SNDFW_GAME_ENTITY_TRACKER_HPP

#include "anchors.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace SoundFramework::game::entities
{
    /** @brief A tracked entity's world position (CryEngine coordinates, z up). */
    struct Position
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    /**
     * @brief Enables tracking when the entity anchors resolved.
     * @note Setup/control-plane only; call before the main-thread hook is armed.
     */
    void configure(const Anchors &anchors) noexcept;

    /** @brief Replaces the names to track (lower case). Thread safe. */
    void set_watch_list(std::vector<std::string> names);

    /** @brief Refreshes every tracked position. Main thread only, from the main-thread hook. */
    void on_frame() noexcept;

    /**
     * @brief True when one of @p names is tracked and stands within @p radius metres of @p at.
     * @note Thread safe; the audio thread calls it for filtered replacements.
     */
    [[nodiscard]] bool any_near(const std::vector<std::string> &names, const Position &at, float radius) noexcept;

    /**
     * @brief The id of the entity named @p name (case-insensitive), or 0 when there is none.
     * @note Main thread only.
     */
    [[nodiscard]] std::uint32_t find_id(const std::string &name) noexcept;

    /** @brief Stops tracking and forgets every position. */
    void clear() noexcept;
} // namespace SoundFramework::game::entities

#endif // SNDFW_GAME_ENTITY_TRACKER_HPP
