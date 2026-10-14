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
        /// IPhysicalEntity* of the entity hit (ray path only; 0 on the sphere path).
        uintptr_t m_collider{0};
        /**
         * @brief Non-zero when the hit is the global TERRAIN heightmap (ray_hit.bTerrain).
         * @details Zero for a brush, and zero on the sphere path. This is the engine's own ground flag, used
         *          to always block the terrain so the camera cannot clip under the world.
         */
        int m_terrain{0};
    };

    /**
     * @brief Resolves RayWorldIntersection and the p_physical_world slot.
     * @details Best-effort: on a pattern miss the raycast features simply no-op (the camera
     *          still works), so callers treat a false return as "raycast unavailable".
     * @param module_base Base address of the game module (WHGame.dll); a world vtable and its PWI slot must lie in it.
     * @param module_size Size of the game module image, in bytes.
     * @param g_env Resolved SSystemGlobalEnvironment base, or 0 when it did not resolve; the
     *              p_physical_world slot is taken from g_env + PHYSICAL_WORLD_OFFSET (no second
     *              hardcoded address).
     * @return true if the ray helper and g_env were located.
     */
    [[nodiscard]] bool initialize_physics_raycast(uintptr_t module_base, size_t module_size, uintptr_t g_env);

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

    /**
     * @brief Swept-sphere world intersection via IPhysicalWorld::PrimitiveWorldIntersection (PWI).
     * @details Sweeps a sphere of the given radius from @p origin along @p sweep and returns the
     *          distance the sphere centre can travel before first contact. Unlike a thin ray, the
     *          contact distance is continuous as the sweep grazes edges, so the camera does not pump
     *          in dense geometry. The PWI function is resolved from the physical-world vtable
     *          each call (the world is gEnv->pPhysicalWorld, read fresh); the call is SEH-guarded and returns
     *          std::nullopt on any fault, an unresolved world, or a miss, so the caller falls back to
     *          the thin ray. Only the float return value (distance) is used. The centre stops @p radius
     *          short of the surface it meets, so that surface lies @p radius beyond the returned distance.
     * @param tag Why the sphere is swept, for the query ledger.
     * @param origin Sphere centre at the start of the sweep, world space.
     * @param radius Sphere radius, world units.
     * @param sweep Sweep vector; its length is the maximum sweep distance (not normalized).
     * @param objtypes entity_query_flags mask (which entity classes block the sphere), written to SPWIParams
     *        entTypes (+0x98) with the rwi_queue bit cleared.
     * @param p_skip_ents Optional array of IPhysicalEntity* the sweep ignores (nullptr = none).
     * @param n_skip_ents Count of @p p_skip_ents.
     * @return The hit distance and point, or std::nullopt on a miss / fault / unavailable.
     */
    [[nodiscard]] std::optional<RayHit> sphere_world_sweep(QueryLedger::Tag tag, const Vector3 &origin, float radius,
                                                           const Vector3 &sweep, int objtypes,
                                                           const uintptr_t *p_skip_ents = nullptr, int n_skip_ents = 0);

    /**
     * @brief Multi-ray "fan" approximation of a swept sphere, built on RayWorldIntersection.
     * @details Casts the centre ray plus four rays offset perpendicular to the sweep by @p radius (a square
     *          tube of half-width radius) and returns the NEAREST hit across all five. This approximates a
     *          swept sphere's edge-catching - so the camera distance does not pump as a single thin ray grazes
     *          edges - with the same object-type filtering as the PWI sphere (RWI takes @p objtypes as a plain
     *          argument; PWI reads entTypes from SPWIParams +0x98). Unlike the sphere wrapper, which returns only a
     *          distance, each fan hit carries its collider and surface normal.
     * @param tag Why the fan is cast, for the query ledger (each of its rays counts as one query).
     * @param origin Sweep start (the camera pivot), world space.
     * @param sweep Sweep vector (pivot -> camera); its length is the max distance (not normalized).
     * @param radius Half-width of the ray tube, world units (the swept-sphere radius / standoff).
     * @param objtypes entity_query_flags mask (world-only for the camera: ent_static | ent_terrain).
     * @param flags rwi_flags mask.
     * @param skip_ents Optional array of IPhysicalEntity* to ignore on every ray (nullptr = none).
     * @param n_skip_ents Count of @p skip_ents.
     * @return The nearest hit across the fan, or std::nullopt if every ray misses.
     */
    [[nodiscard]] std::optional<RayHit> ray_fan_sweep(QueryLedger::Tag tag, const Vector3 &origin, const Vector3 &sweep,
                                                      float radius, int objtypes, unsigned int flags,
                                                      const uintptr_t *skip_ents = nullptr, int n_skip_ents = 0);

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
