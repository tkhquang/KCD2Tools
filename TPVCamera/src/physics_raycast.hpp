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

#include "core/collision/probe_types.hpp"
#include "math_utils.hpp"
#include "query_ledger.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace TPVCamera
{

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
