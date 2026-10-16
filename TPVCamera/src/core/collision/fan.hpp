/**
 * @file core/collision/fan.hpp
 * @brief The camera arm's ray fan: a square tube of five parallel rays along a sweep, nearest hit first.
 *
 * @details Engine-free (no Windows or DetourModKit header): the rays go through a WorldProbe.
 */
#ifndef TPVCAMERA_CORE_COLLISION_FAN_HPP
#define TPVCAMERA_CORE_COLLISION_FAN_HPP

#include "core/collision/probe_types.hpp"
#include "core/collision/world_probe.hpp"
#include "query_ledger.hpp"

#include <cstdint>
#include <optional>

namespace TPVCamera
{
    /**
     * @brief Multi-ray "fan" probe of a tube along the sweep, built on RayWorldIntersection.
     * @details Casts the centre ray plus four rays offset perpendicular to the sweep by @p radius (a square
     *          tube of half-width radius) and returns the NEAREST hit across all five, so the camera distance does
     *          not pump as a single thin ray grazes edges. Each hit carries its collider and surface normal.
     * @param probe The world the rays are cast in.
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
     * @param memory Optional: what this walk's earlier casts met along the same sweep direction, read to skip the rays
     *        whose answer cannot have changed and updated with this cast (see FanMemory).
     * @param walked How far @p origin lies along the sweep from the walk's start, where @p memory measures from.
     * @return The nearest hit across the fan, or std::nullopt if every ray misses.
     */
    [[nodiscard]] std::optional<RayHit> ray_fan_sweep(WorldProbe &probe, QueryLedger::Tag tag, const Vector3 &origin,
                                                      const Vector3 &sweep, float radius, int objtypes,
                                                      unsigned int flags, const std::uintptr_t *skip_ents = nullptr,
                                                      int n_skip_ents = 0, int *rays_blocked = nullptr,
                                                      FanMemory *memory = nullptr, float walked = 0.0f);
} // namespace TPVCamera

#endif // TPVCAMERA_CORE_COLLISION_FAN_HPP
