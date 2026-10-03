/**
 * @file engine/interactables.hpp
 * @brief Native detection of the interactive objects around a point (doors, crafting stations, beds, use spots).
 *
 * An object is interactive when its entity script offers the player an action (the script defines GetActions). Those
 * classes are known by name, so a walk of the entity system, spread over frames, resolves each entity's class once per
 * class pointer and keeps every entity of the interactive classes in the level with its position. Placed doors, beds
 * and triggers do not move, so walking around needs no new walk: each collect picks the cached entities near the player
 * by those positions, re-resolves them by id, and the visible ones within the radius become markers.
 *
 * A script trigger (a bed, a bench seat, a wash tub) has no mesh of its own; the mesh it belongs to is looked up once
 * and cached per trigger: at the pose its prefab template predicts (engine/prefab_templates.hpp), or by a search
 * around it when no template fits. Those lookups run a slice per frame, nearest trigger first
 * (advance_interactable_visuals()).
 */
#ifndef HENRYSENSES_INTERACTABLES_HPP
#define HENRYSENSES_INTERACTABLES_HPP

#include "game_structures.hpp"

#include <DetourModKit/error.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace HenrySenses
{
    /**
     * @enum InteractKind
     * @brief Display group of an interactive object.
     */
    enum class InteractKind : std::uint8_t
    {
        /// AnimDoor.
        Door = 0,
        /// Crafting and minigame stations: anvil, grindstone, alchemy table, dice, readable books.
        Station = 1,
        /// Beds (sleep and save).
        Bed = 2,
        /// Chairs and bench seats: a use trigger that seats the player on a chair, bench, stool or throne.
        Seat = 3,
        /// Scripted use spots (wash tub, kettle, food processing and the other action triggers).
        UseSpot = 4,
        /// Ladders, locks, graves, inscriptions, carry piles, cart mount points and other usable objects.
        Other = 5,
    };

    inline constexpr std::size_t INTERACT_KIND_COUNT = 6;

    /** @brief Bit of @p kind in a kind mask. */
    [[nodiscard]] constexpr std::uint32_t interact_kind_bit(InteractKind kind) noexcept
    {
        return 1u << static_cast<std::uint32_t>(kind);
    }

    /** @brief Log name of @p kind. */
    [[nodiscard]] std::string_view interact_kind_name(InteractKind kind) noexcept;

    /**
     * @struct InteractTarget
     * @brief One interactive object to mark.
     */
    struct InteractTarget
    {
        std::uint32_t entity_id{0};
        InteractKind kind{InteractKind::Other};
        /// World bounds: its mesh's, or a small box at its position for an entity without geometry.
        game_structures::Aabb bounds{};
        float distance{0.0f};
        /// Its mesh is known (see InteractVisual), so it can be outlined instead of boxed.
        bool has_mesh{false};
        /// That mesh is a static brush (outlined only with the static-mesh render hook).
        bool mesh_brush{false};
        /**
         * @brief That mesh is an entity the game keeps invisible for now (re-read on every collect): nothing is drawn,
         * so it keeps a marker while its outline state stays applied for when the game draws it again.
         */
        bool game_hidden{false};
    };

    /**
     * @struct InteractVisual
     * @brief The mesh that shows one interactive object: the entity's own render proxy (a door, a bed) or, for a
     *        script trigger, the brush or entity mesh under it (a wash tub, a bench).
     */
    struct InteractVisual
    {
        /// The entity whose render proxy shows the object, or 0 for a brush.
        std::uint32_t entity_id{0};
        /// The static brush that shows the object (entity_id == 0).
        std::uintptr_t brush{0};
        /// The mesh's world bounds (for a brush, the identity check's reference).
        game_structures::Aabb bounds{};
        InteractKind kind{InteractKind::Other};
        float distance{0.0f};
    };

    /**
     * @brief Appends the render nodes the mod keeps outlined off the octree whose bounds overlap a box: a mesh moved
     *        to the always-visible list is invisible to the octree queries the lookups run.
     */
    using OutlinedNodesFn = void (*)(const game_structures::Aabb &box, std::vector<std::uintptr_t> &out) noexcept;

    /**
     * @brief Checks that entity enumeration is available.
     * @return An empty value when interactables can be scanned.
     */
    [[nodiscard]] DMK::Result<void> initialize_interactables();

    /** @brief Marks the interactable scan unavailable, releases a running walk and drops the caches. */
    void shutdown_interactables() noexcept;

    /** @brief True once initialize_interactables() succeeded. */
    [[nodiscard]] bool interactables_available() noexcept;

    /**
     * @brief Releases a running walk and drops the cached candidates (a level change invalidates every entity).
     * @note Main thread, or teardown after the main thread stopped ticking.
     */
    void reset_interactables() noexcept;

    /** @brief Asks the next walk to log the classes found around the player. */
    void request_interactables_survey() noexcept;

    /**
     * @brief Advances the world walk within a per-frame time budget, starting a new one when the cache is stale.
     * @param center Player position (the centre of a survey).
     * @param radius Scan radius in meters (the reach of a survey).
     * @return True when a walk completed during this call (the candidates changed).
     * @note Main thread only; call every frame while interactables are shown.
     */
    bool advance_interactables(const game_structures::Vec3f &center, float radius);

    /**
     * @brief Builds the markers of the cached candidates of the kinds in @p kind_mask within @p radius of @p center.
     * @details An object with a mesh of its own (a door) gets it at once. A trigger whose mesh is unknown, due for a
     *          retry or recreated since is queued for advance_interactable_visuals() and keeps a marker at the trigger
     *          meanwhile; a use trigger whose kind (Seat or UseSpot) depends on that mesh is left out until it is
     *          looked up.
     * @param center Scan centre (the player).
     * @param radius Scan radius in meters, measured to the entity position.
     * @param kind_mask OR of interact_kind_bit() values.
     * @param out Receives the targets nearest first, cleared first. A target whose mesh is known takes the mesh's
     *        bounds.
     * @param visuals When set, receives the meshes of the targets (one per mesh, nearest first), cleared first.
     * @return The number of targets.
     * @note Main thread only.
     */
    [[nodiscard]] std::size_t collect_interactables(
        const game_structures::Vec3f &center,
        float radius,
        std::uint32_t kind_mask,
        std::vector<InteractTarget> &out,
        std::vector<InteractVisual> *visuals = nullptr
    );

    /**
     * @brief Looks up the queued trigger meshes, nearest first, within a per-frame budget, and patches the last
     *        collect's targets in place.
     * @details A lookup starts only while the average cost of the recent ones still fits the budget, so the budget is
     *          not overrun; the first of a frame always runs. The lookups wait while the template index is being built
     *          (a few frames per session), so a trigger is not searched for when its template would place it.
     * @param center The player position.
     * @param out The targets collect_interactables() filled, patched in place.
     * @param visuals The meshes it filled, rebuilt when a target changed.
     * @param budget_us Main-thread time this call may spend.
     * @param outlined The nodes the mod keeps outlined off the octree, considered by every lookup.
     * @return True when a target or mesh changed.
     * @note Main thread only.
     */
    bool advance_interactable_visuals(
        const game_structures::Vec3f &center,
        std::vector<InteractTarget> &out,
        std::vector<InteractVisual> *visuals,
        std::int64_t budget_us,
        OutlinedNodesFn outlined
    );

} // namespace HenrySenses

#endif // HENRYSENSES_INTERACTABLES_HPP
