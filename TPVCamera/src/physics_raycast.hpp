/**
 * @file physics_raycast.hpp
 * @brief Thin wrapper over the engine's IPhysicalWorld::RayWorldIntersection.
 *
 * Resolves the engine ray helper and the p_physical_world global once, then exposes a
 * synchronous world raycast used by the camera for collision (keep the view
 * out of geometry) and aim convergence (land the crosshair on the exact world target).
 * Every call is SEH-guarded and degrades to "no hit" if the engine path faults or the
 * physical world is not ready, so a layout or version drift can never crash the game.
 */
#ifndef TPVCAMERA_PHYSICS_RAYCAST_HPP
#define TPVCAMERA_PHYSICS_RAYCAST_HPP

#include "math_utils.hpp"

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
        /// Non-zero when the hit is the global TERRAIN heightmap (ray_hit.bTerrain); 0 for brushes / 0 on the
        /// sphere path. The engine's own ground flag -- used to always block the terrain (no under-world clip).
        int m_terrain{0};
    };

    /**
     * @brief Resolves RayWorldIntersection and the p_physical_world slot.
     * @details Best-effort: on a pattern miss the raycast features simply no-op (the camera
     *          still works), so callers treat a false return as "raycast unavailable".
     * @param g_env Resolved SSystemGlobalEnvironment base; the p_physical_world slot is taken
     *              from g_env + PHYSICAL_WORLD_OFFSET (no second hardcoded address).
     * @return true if the ray helper was located.
     */
    [[nodiscard]] bool initialize_physics_raycast(uintptr_t module_base, size_t module_size, uintptr_t g_env);

    /**
     * @brief Synchronous world raycast.
     * @param origin Ray start in world space.
     * @param direction Ray vector; its length is the maximum ray length (not normalized).
     * @param objtypes entity_query_flags mask (which entity classes the ray can hit).
     * @param flags rwi_flags mask (pierceability and behaviour).
     * @param skip_ents Optional array of IPhysicalEntity* to ignore (RWI pSkipEnts); nullptr = none.
     * @param n_skip_ents Count of @p skip_ents.
     * @return The nearest hit, or std::nullopt on a miss or when physics is not ready.
     */
    [[nodiscard]] std::optional<RayHit> ray_world_intersection(const Vector3 &origin, const Vector3 &direction,
                                                               int objtypes, unsigned int flags,
                                                               const uintptr_t *skip_ents = nullptr,
                                                               int n_skip_ents = 0);

    /**
     * @brief Swept-sphere world intersection via IPhysicalWorld::PrimitiveWorldIntersection (PWI).
     * @details Sweeps a sphere of the given radius from @p origin along @p sweep and returns the
     *          distance the sphere centre can travel before first contact. Unlike a thin ray, the
     *          contact distance is continuous as the sweep grazes edges, so the camera does not pump
     *          in dense geometry. The PWI function is resolved from the LIVE physical-world vtable
     *          each call (the static global is unreliable); the call is SEH-guarded and returns
     *          std::nullopt on any fault, an unresolved world, or a miss, so the caller falls back to
     *          the thin ray. Only the float return value (distance) is used; the sphere radius is the
     *          standoff, so the caller must NOT subtract a collision skin on top.
     * @param origin Sphere centre at the start of the sweep, world space.
     * @param radius Sphere radius (the standoff kept from surfaces), world units.
     * @param sweep Sweep vector; its length is the maximum sweep distance (not normalized).
     * @param objtypes entity_query_flags mask (which entity classes block the sphere).
     * @return The hit distance and point, or std::nullopt on a miss / fault / unavailable.
     */
    [[nodiscard]] std::optional<RayHit> sphere_world_sweep(const Vector3 &origin, float radius, const Vector3 &sweep,
                                                           int objtypes, const uintptr_t *p_skip_ents = nullptr,
                                                           int n_skip_ents = 0);

    /**
     * @brief Multi-ray "fan" approximation of a swept sphere, built on RayWorldIntersection.
     * @details Casts the centre ray plus four rays offset perpendicular to the sweep by @p radius (a square
     *          tube of half-width radius) and returns the NEAREST hit across all five. This approximates a
     *          swept sphere's edge-catching -- so the camera distance does not pump as a single thin ray grazes
     *          edges -- while keeping the CORRECT object-type filtering: RWI takes @p objtypes as a plain
     *          function argument (honoured), unlike PrimitiveWorldIntersection whose fork SPWIParams entTypes
     *          offset is mis-mapped (it queried ent_all and collided with the player's own articulated gear).
     * @param origin Sweep start (the camera pivot), world space.
     * @param sweep Sweep vector (pivot -> camera); its length is the max distance (not normalized).
     * @param radius Half-width of the ray tube, world units (the swept-sphere radius / standoff).
     * @param objtypes entity_query_flags mask (world-only for the camera: ent_static | ent_terrain).
     * @param flags rwi_flags mask.
     * @param skip_ents Optional array of IPhysicalEntity* to ignore on every ray (nullptr = none).
     * @param n_skip_ents Count of @p skip_ents.
     * @return The nearest hit across the fan, or std::nullopt if every ray misses.
     */
    [[nodiscard]] std::optional<RayHit> ray_fan_sweep(const Vector3 &origin, const Vector3 &sweep, float radius,
                                                      int objtypes, unsigned int flags,
                                                      const uintptr_t *skip_ents = nullptr, int n_skip_ents = 0);

    /**
     * @brief Fraction (0..1) of the character's silhouette occluded by WORLD geometry as seen from @p camera.
     * @details Samples a small grid over the character body (four vertical levels head->shins x three columns
     *          across the shoulders, relative to @p pivot at ~eye height) and casts a ray from @p camera to each;
     *          a sample counts as occluded when world geometry lies before it. The player body is ent_living and
     *          must be excluded by a world-only @p objtypes (ent_static | ent_terrain), so the character never
     *          occludes itself. Used to gate camera collision: a thin pole / rail that hides little of the
     *          character returns a low fraction (ignore it, no camera jump); a wall or a near post that hides
     *          most of it returns a high fraction (collide). Distance-aware by construction (a closer obstacle
     *          of the same size hides more), which a fixed width / size threshold cannot capture.
     * @param out_head_fill Optional; receives the fraction (0..1) of the HEAD level (top samples) occluded, for the
     *        head-visible collision gate, or -1 if no valid head sample. nullptr to skip.
     * @return Occluded fraction in [0, 1]; 0 if no samples were valid.
     */
    [[nodiscard]] float character_occluded_fraction(const Vector3 &camera, const Vector3 &pivot, int objtypes,
                                                    unsigned int flags, float *out_head_fill = nullptr);

    /**
     * @brief The visible render node (IRenderNode) a physics collider belongs to, via its foreign data.
     * @details A static-world collision hit carries the brush it came from in m_pForeignData
     *          (CPhysicalEntity + PHYS_ENTITY_FOREIGN_DATA_OFFSET) when m_iForeignData
     *          (+ PHYS_ENTITY_FOREIGN_TYPE_OFFSET) is PHYS_FOREIGN_ID_STATIC. This is the EXACT brush the ray
     *          hit, so the caller can measure that brush's visible coverage instead of guessing from a box.
     *          SEH-guarded; returns 0 when the collider is null / not a static-brush owner / unreadable.
     * @param collider The RayHit::m_collider of a hit (the IPhysicalEntity pointer).
     * @return The IRenderNode pointer, or 0 when unavailable.
     */
    [[nodiscard]] uintptr_t static_brush_render_node(uintptr_t collider) noexcept;

    /**
     * @brief Larger horizontal (X / Y) extent of a physics collider's world AABB (m_BBox), in meters.
     * @details The fallback classifier for a foreign-null collider whose render-mesh coverage cannot be measured
     *          (an entity post / HLOD-baked prop). A post is small in BOTH horizontal axes (small footprint); a
     *          wall is large in at least one (large footprint) even when thin in depth -- so this separates a thin
     *          POST (the body is visible past it, skip) from a thin WALL (collide), which the minimum-extent metric
     *          cannot. Any owned collider (static brush OR placed entity, foreignType != 0) carries a sane AABB
     *          here; terrain / unowned geom (foreignType == 0, degenerate bbox) returns -1. SEH-guarded.
     * @param collider The RayHit::m_collider of a hit (the IPhysicalEntity pointer).
     * @return max(x-extent, y-extent), or -1 when the collider is null / unowned (terrain, foreignType == 0) /
     *         the AABB is unreadable or degenerate (the caller treats < 0 as "unknown -- not a post").
     */
    [[nodiscard]] float collider_horizontal_footprint(uintptr_t collider) noexcept;

} // namespace TPVCamera

#endif // TPVCAMERA_PHYSICS_RAYCAST_HPP
