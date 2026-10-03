/**
 * @file highlight/world_highlights.hpp
 * @brief Outline set for interactive world objects: entity meshes (doors, beds) and static brushes (wash tubs,
 *        benches) found under script triggers.
 *
 * Kept apart from the loot registry, which the loot scan replaces batch by batch: this set follows the native
 * interactables scan. A node the loot set already highlights is left to it. Brush pointers are never trusted on their
 * own: every touch first checks the CBrush identity and that the bounds still match the ones seen when it was found.
 */
#ifndef HENRYSENSES_WORLD_HIGHLIGHTS_HPP
#define HENRYSENSES_WORLD_HIGHLIGHTS_HPP

#include "engine/entity_access.hpp"
#include "game_structures.hpp"
#include "highlight/registry.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace HenrySenses
{
    /**
     * @struct WorldHighlightRequest
     * @brief One object to outline.
     */
    struct WorldHighlightRequest
    {
        /// The owner entity whose render proxy shows the object, or 0 for a brush.
        EntityId entity_id{0};
        /// The static brush that shows the object (entity_id == 0).
        std::uintptr_t brush{0};
        /// The brush's bounds when it was found, for the identity check.
        game_structures::Aabb brush_bounds{};
        /// The colour before the distance fade.
        std::uint32_t color_word{0};
        /// Distance to the player in metres, negative when unknown.
        float distance{-1.0f};
        /// Reach of the group, for the distance fade.
        float fade_radius{20.0f};
        /// The group's intensity in [0, 1].
        float intensity{1.0f};
        /// Outline or Fill (a Box group's objects are not requested: they get brackets only).
        GroupStyle style{GroupStyle::Outline};
    };

    /**
     * @brief Replaces the outlined set with @p requests, applying and restoring node state as needed.
     * @param requests The objects, nearest first.
     * @param options The loot set's apply options (word styling, render-always).
     * @note Main thread only.
     */
    void apply_world_highlights(std::span<const WorldHighlightRequest> requests, const ApplyOptions &options) noexcept;

    /**
     * @brief Restores every node the set touched and empties it.
     * @note Main thread only.
     */
    void clear_world_highlights() noexcept;

    /**
     * @brief Re-asserts the always-visible bookkeeping of every outlined node (see maintain_highlights()).
     * @note Main thread only.
     */
    void maintain_world_highlights() noexcept;

    /**
     * @brief Refreshes the persistent render object of every outlined node (see refresh_highlight_render_objects()).
     * @return The number of render objects refreshed.
     * @note Main thread only.
     */
    std::size_t refresh_world_highlight_render_objects() noexcept;

    /** @brief Number of objects currently in the set. */
    [[nodiscard]] std::size_t world_highlight_count() noexcept;

    /**
     * @brief Appends the nodes of the set that were moved to the always-visible list and whose bounds overlap @p box.
     * @details An always-visible node has left the render octree, so the octree queries that look up interactive
     *          objects no longer return it; they consider these instead. Only entries near @p box are resolved.
     * @param box World box.
     * @param out Receives the nodes (appended).
     * @note Main thread only.
     */
    void world_highlight_nodes_in(const game_structures::Aabb &box, std::vector<std::uintptr_t> &out) noexcept;

} // namespace HenrySenses

#endif // HENRYSENSES_WORLD_HIGHLIGHTS_HPP
