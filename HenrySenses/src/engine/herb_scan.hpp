/**
 * @file engine/herb_scan.hpp
 * @brief Native detection of pickable herbs and mushrooms around a point.
 *
 * A pickable herb is an instance of a merged-mesh vegetation group whose vegetation group carries the pickable flag.
 * The scan looks the merged-mesh cells overlapping a box up in the engine's own cell index (CMergedMeshesManager,
 * with the render octree as the fallback), keeps the flagged groups of the cells that are streamed in, decodes each
 * plant whose scale is non-zero (the game zeroes the scale of harvested plants until they respawn) and groups the
 * plants of one species into the clusters a single pick harvests. Pickable mushrooms are not merged: they are
 * CVegetation nodes flagged ERF_PICKABLE, found by a flag-filtered octree query, and a picked one is hidden until it
 * respawns.
 */
#ifndef HENRYSENSES_HERB_SCAN_HPP
#define HENRYSENSES_HERB_SCAN_HPP

#include "game_structures.hpp"

#include <DetourModKit/error.hpp>

#include <cstdint>
#include <vector>

namespace HenrySenses
{
    /**
     * @struct HerbCluster
     * @brief The plants of one species that one pick harvests.
     */
    struct HerbCluster
    {
        /// World bounds of the cluster's plant bases, padded for display.
        game_structures::Aabb bounds{};
        /// Vegetation group index (the species).
        std::int32_t group{-1};
        /// Unpicked plants in the cluster.
        std::uint32_t plants{0};
        /**
         * @brief None of the cluster's plants went out for outlining (its model did not resolve), so the cluster keeps
         * its marker even for a group that outlines herbs. Always false when no plants were asked for.
         */
        bool marker_fallback{false};
    };

    /**
     * @struct HerbPlant
     * @brief One unpicked plant: its model and the world transform the engine draws it with.
     */
    struct HerbPlant
    {
        /// Translation, rotation and scale, as the merged mesh (or the CVegetation node) builds it.
        game_structures::Matrix34f world{};
        /// The vegetation group's CStatObj.
        std::uintptr_t stat_obj{0};
        /// Distance to the scan centre.
        float distance{0.0f};
    };

    /**
     * @brief Checks that the octree query, the object manager and the merged-mesh class identity resolved.
     * @return An empty value when herbs can be scanned, or NoMatch naming what is missing.
     */
    [[nodiscard]] DMK::Result<void> initialize_herb_scan();

    /** @brief Marks the herb scan unavailable. */
    void shutdown_herb_scan() noexcept;

    /** @brief True once initialize_herb_scan() succeeded and no query has faulted. */
    [[nodiscard]] bool herb_scan_available() noexcept;

    /**
     * @brief Collects the unpicked herb and mushroom clusters within @p radius of @p center.
     * @param center Scan centre (the player).
     * @param radius Scan radius in meters; a cluster counts when any of its plants is inside.
     * @param out Receives the clusters, cleared first.
     * @param plants When set, receives the plants to outline, cleared first: every unpicked plant within the radius
     *        whose model resolved.
     * @return The number of clusters.
     * @note Main thread only (the cell index, the octree query and the plant arrays are updated there).
     */
    [[nodiscard]] std::size_t collect_herb_clusters(
        const game_structures::Vec3f &center,
        float radius,
        std::vector<HerbCluster> &out,
        std::vector<HerbPlant> *plants = nullptr
    );

} // namespace HenrySenses

#endif // HENRYSENSES_HERB_SCAN_HPP
