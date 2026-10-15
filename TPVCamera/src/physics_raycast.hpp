/**
 * @file physics_raycast.hpp
 * @brief Thin wrapper over the engine's IPhysicalWorld::RayWorldIntersection.
 *
 * Resolves the engine ray helper and the p_physical_world global once, then exposes a
 * synchronous world raycast used by the camera for collision (keep the view
 * out of geometry) and the ranged crosshair ray (find the world point under the crosshair).
 * Every call is SEH-guarded and degrades to "no hit" if the engine path faults or the
 * physical world is not ready, so a layout or version drift can never crash the game.
 */
#ifndef TPVCAMERA_PHYSICS_RAYCAST_HPP
#define TPVCAMERA_PHYSICS_RAYCAST_HPP

#include "math_utils.hpp"
#include "query_ledger.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace TPVCamera
{

    /**
     * @struct RayHit
     * @brief Decoded subset of the engine ray_hit for a single nearest hit.
     */
    struct RayHit
    {
        /// World distance from the ray origin to the hit.
        float m_distance{0.0f};
        /// World-space hit position.
        Vector3 m_point{};
        /// Surface normal at the hit.
        Vector3 m_normal{};
        /// IPhysicalEntity* of the entity hit.
        uintptr_t m_collider{0};
        /**
         * @brief Non-zero when the hit is the global TERRAIN heightmap (ray_hit.bTerrain).
         * @details Zero for a brush. This is the engine's own ground flag, used
         *          to always block the terrain so the camera cannot clip under the world.
         */
        int m_terrain{0};
        /// The geometry part of the collider hit (ray_hit.ipart).
        int m_part{0};
        /// The mapped surface type of the hit (ray_hit.surface_idx).
        int m_surface{0};
    };

    /**
     * @brief Resolves RayWorldIntersection and the p_physical_world slot.
     * @details Best-effort: on a pattern miss the raycast features simply no-op (the camera
     *          still works), so callers treat a false return as "raycast unavailable".
     * @param g_env Resolved SSystemGlobalEnvironment base, or 0 when it did not resolve; the
     *              p_physical_world slot is taken from g_env + PHYSICAL_WORLD_OFFSET (no second
     *              hardcoded address).
     * @return true if the ray helper and g_env were located.
     */
    [[nodiscard]] bool initialize_physics_raycast(uintptr_t g_env);

    /**
     * @brief Synchronous world raycast.
     * @param tag Why the ray is cast, for the query ledger.
     * @param origin Ray start in world space.
     * @param direction Ray vector; its length is the maximum ray length (not normalized).
     * @param objtypes entity_query_flags mask (which entity classes the ray can hit).
     * @param flags rwi_flags mask (pierceability and behaviour).
     * @param skip_ents Optional array of IPhysicalEntity* to ignore (RWI pSkipEnts); nullptr = none.
     * @param n_skip_ents Count of @p skip_ents.
     * @return The nearest hit, or std::nullopt on a miss or when physics is not ready.
     */
    [[nodiscard]] std::optional<RayHit> ray_world_intersection(QueryLedger::Tag tag, const Vector3 &origin,
                                                               const Vector3 &direction, int objtypes,
                                                               unsigned int flags, const uintptr_t *skip_ents = nullptr,
                                                               int n_skip_ents = 0);

    /// The rays of ray_fan_sweep(): its centre and four offsets by the radius.
    inline constexpr int k_fan_rays = 5;

    /**
     * @brief Multi-ray "fan" probe of a tube along the sweep, built on RayWorldIntersection.
     * @details Casts the centre ray plus four rays offset perpendicular to the sweep by @p radius (a square
     *          tube of half-width radius) and returns the NEAREST hit across all five, so the camera distance does
     *          not pump as a single thin ray grazes edges. Each hit carries its collider and surface normal.
     * @param tag Why the fan is cast, for the query ledger (each of its rays counts as one query).
     * @param origin Sweep start (the camera pivot), world space.
     * @param sweep Sweep vector (pivot -> camera); its length is the max distance (not normalized).
     * @param radius Half-width of the ray tube, world units.
     * @param objtypes entity_query_flags mask (world-only for the camera: ent_static | ent_terrain).
     * @param flags rwi_flags mask.
     * @param skip_ents Optional array of IPhysicalEntity* to ignore on every ray (nullptr = none).
     * @param n_skip_ents Count of @p skip_ents.
     * @param rays_blocked Optional out: how many of the k_fan_rays rays hit something (0 on a miss), which tells an
     *        obstacle narrower than the fan with open space beside it from one where the whole fan is blocked. A sweep
     *        straight along the world axes casts the centre ray alone and reports k_fan_rays for its hit.
     * @return The nearest hit across the fan, or std::nullopt if every ray misses.
     */
    [[nodiscard]] std::optional<RayHit> ray_fan_sweep(QueryLedger::Tag tag, const Vector3 &origin, const Vector3 &sweep,
                                                      float radius, int objtypes, unsigned int flags,
                                                      const uintptr_t *skip_ents = nullptr, int n_skip_ents = 0,
                                                      int *rays_blocked = nullptr);

    /**
     * @brief The visible render node (IRenderNode) a physics collider belongs to, via its foreign data.
     * @details A brush the engine physicalizes at runtime (on demand or from a heap, e.g. a basket or furniture)
     *          and a tree (its trunk collider links the CVegetation) carry their node in m_pForeignData
     *          (CPhysicalEntity + PHYS_ENTITY_FOREIGN_DATA_OFFSET) with m_iForeignData (the low word at
     *          + PHYS_ENTITY_FOREIGN_TYPE_OFFSET) == PHYS_FOREIGN_ID_STATIC. Most static world geometry is baked: with
     *          wh_e_PernamentPhysicalization on, the level loader creates its physics once with the same type but a
     *          NULL m_pForeignData, and the brush itself gets no physics of its own. Such a hit is still an ordinary
     *          visible CBrush in the render octree, it just has no link. occluder_node_for_collider takes the link as
     *          the exact owner of a hit for the step-past and the fade, and the collision trace log uses it to name
     *          the hit. SEH-guarded; returns 0 when the collider is null / baked (no link) / not a static-brush owner /
     *          unreadable.
     * @param collider The RayHit::m_collider of a hit (the IPhysicalEntity pointer).
     * @return The IRenderNode pointer, or 0 when unavailable.
     */
    [[nodiscard]] uintptr_t static_brush_render_node(uintptr_t collider) noexcept;

} // namespace TPVCamera

#endif // TPVCAMERA_PHYSICS_RAYCAST_HPP
