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
     * @brief Resolves the player's / actors' physics entities to skip during camera collision, via the physics
     *        world (NOT the engine entity/character API -- the KCD2 fork shifted those vtable slots, and the
     *        skeleton physics is not a reachable data offset on C_Player).
     * @details The camera pivot sits INSIDE the player body, so a FORWARD (pivot->camera) ray exits through the
     *          body's back-face and never registers it, so the probe casts its rays in REVERSE: from the camera
     *          end (origin + sweep) back toward the pivot, so each ray strikes the body / any in-between actor
     *          from OUTSIDE and reports it. A collider the
     *          world-only cast (@p objtypes_world) does NOT see at the same range is a non-world entity (player
     *          body, worn gear, NPC) and is collected to skip; world geometry appears in both casts at the same
     *          range and is never collected. Output is capped at @p max_out (SPWIParams nSkipEnts clamps to 4).
     * @return Count of distinct non-world physics entities written to @p out_skip.
     */
    [[nodiscard]] int resolve_player_physics_skip(const Vector3 &origin, const Vector3 &sweep, float radius,
                                                  int objtypes_all, int objtypes_world, unsigned int flags,
                                                  uintptr_t *out_skip, int max_out);

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
     * @brief Multi-ray fan that SKIPS standalone thin scenery, re-casting to the real surface behind it.
     * @details For each fan result it looks up the hit physics entity's world AABB (PHYS_ENTITY_BBOX_*) and, if
     *          the smallest dimension is below @p thin_max_size, treats it as ignorable thin scenery (a lone
     *          stick / pole / thin sign): the entity is added to a skip list and the fan re-cast, up to a small
     *          cap, so the camera blocks on the first NON-thin surface (or nothing). @p thin_max_size <= 0
     *          disables the check (returns a plain @ref ray_fan_sweep, no bbox reads). An unreadable bbox counts
     *          as NOT thin (keep colliding -- the safe default). Cannot drop multi-rail fences (one mesh).
     * @return The nearest non-thin hit, or std::nullopt (every layer thin / all rays miss).
     */
    [[nodiscard]] std::optional<RayHit> ray_fan_sweep_skipping_thin(const Vector3 &origin, const Vector3 &sweep,
                                                                    float radius, int objtypes, unsigned int flags,
                                                                    float thin_max_size);

} // namespace TPVCamera

#endif // TPVCAMERA_PHYSICS_RAYCAST_HPP
