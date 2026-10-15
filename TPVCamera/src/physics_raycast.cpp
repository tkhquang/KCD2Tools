/**
 * @file physics_raycast.cpp
 * @brief Implementation of the IPhysicalWorld::RayWorldIntersection wrapper.
 */

#include "physics_raycast.hpp"
#include "aob_resolver.hpp"
#include "constants.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace TPVCamera
{

    // IPhysicalWorld::RayWorldIntersection inline helper (see constants.hpp for the ABI). org
    // and dir are passed by pointer (Vector3 is three packed floats, matching the engine Vec3).
    using RayWorldIntersectionFn = int(__fastcall *)(void *physical_world, const Vector3 *org, const Vector3 *dir,
                                                     int objtypes, unsigned int flags, void *hits, int n_max_hits,
                                                     void *skip_ents, int num_skip_ents, void *foreign_data,
                                                     int foreign_index, const char *name_tag);

    static RayWorldIntersectionFn s_ray_world_intersection = nullptr;

    // Address of the IPhysicalWorld* global (read fresh per ray, not cached: it is null until a
    // level loads and is replaced across level transitions).
    static uintptr_t s_physical_world_global_addr = 0;

    bool initialize_physics_raycast(uintptr_t g_env)
    {
        DMK::Logger &logger = DMK::log();

        // The module-scoped cascade resolved the helper inside the game image (or 0 on a miss); read here
        // from the anchor registry resolved at startup by resolve_all_anchors().
        const uintptr_t ray_fn = gated_anchor_address(Feature::Collision, AnchorId::RayWorldIntersection);
        if (ray_fn == 0 || g_env == 0)
        {
            logger.warning("PhysicsRaycast: RayWorldIntersection or g_env not found (game patched?); collision/aim "
                           "raycast unavailable");
            return false;
        }

        s_ray_world_intersection = reinterpret_cast<RayWorldIntersectionFn>(ray_fn);
        // p_physical_world is a member of the g_env struct (see PHYSICAL_WORLD_OFFSET); deriving its
        // slot from the patch-resiliently resolved g_env base avoids a second hardcoded address.
        s_physical_world_global_addr = g_env + Constants::PHYSICAL_WORLD_OFFSET;

        logger.info("PhysicsRaycast: RayWorldIntersection at {}, p_physical_world slot at {}",
                    DMK::format::format_address(ray_fn), DMK::format::format_address(s_physical_world_global_addr));
        return true;
    }

    /**
     * @brief SEH-isolated engine call. Held apart from the C++ caller so the structured
     *        handler shares no frame with object unwinding; a fault becomes "no hit".
     */
    static int ray_world_intersection_guarded(void *physical_world, const Vector3 *org, const Vector3 *dir,
                                              int objtypes, unsigned int flags, void *hits, void *skip_ents,
                                              int n_skip_ents)
    {
        __try
        {
            return s_ray_world_intersection(physical_world, org, dir, objtypes, flags, hits, 1, skip_ents, n_skip_ents,
                                            nullptr, 0, "TPVCameraRay");
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    std::optional<RayHit> ray_world_intersection(QueryLedger::Tag tag, const Vector3 &origin, const Vector3 &direction,
                                                 int objtypes, unsigned int flags, const uintptr_t *skip_ents,
                                                 int n_skip_ents)
    {
        if (s_ray_world_intersection == nullptr || s_physical_world_global_addr == 0)
        {
            return std::nullopt;
        }

        // Resolve the physical world fresh; bail cleanly while it is null (no level / loading).
        const auto world_value = DMK::memory::read<uintptr_t>(DMK::Address{s_physical_world_global_addr});
        if (!world_value || *world_value == 0 || !DMK::memory::is_plausible_ptr(DMK::Address{*world_value}))
        {
            return std::nullopt;
        }

        alignas(16) std::byte hit_buffer[Constants::RAY_HIT_SIZE];
        std::memset(hit_buffer, 0, sizeof(hit_buffer));

        const std::int64_t ledger_started = QueryLedger::begin();
        const int hit_count = ray_world_intersection_guarded(
            reinterpret_cast<void *>(*world_value), &origin, &direction, objtypes, flags, hit_buffer,
            const_cast<uintptr_t *>(skip_ents), (skip_ents != nullptr) ? n_skip_ents : 0);
        QueryLedger::end(tag, QueryLedger::Kind::Physics, ledger_started);
        if (hit_count < 1)
        {
            return std::nullopt;
        }

        RayHit hit{};
        hit.m_distance = *reinterpret_cast<const float *>(hit_buffer + Constants::RAY_HIT_OFFSET_DISTANCE);
        hit.m_point = *reinterpret_cast<const Vector3 *>(hit_buffer + Constants::RAY_HIT_OFFSET_POINT);
        hit.m_normal = *reinterpret_cast<const Vector3 *>(hit_buffer + Constants::RAY_HIT_OFFSET_NORMAL);
        hit.m_collider = *reinterpret_cast<const uintptr_t *>(hit_buffer + Constants::RAY_HIT_OFFSET_COLLIDER);
        hit.m_terrain = *reinterpret_cast<const int *>(hit_buffer + Constants::RAY_HIT_OFFSET_TERRAIN);
        hit.m_part = *reinterpret_cast<const int *>(hit_buffer + Constants::RAY_HIT_OFFSET_PART);
        hit.m_surface = *reinterpret_cast<const std::int16_t *>(hit_buffer + Constants::RAY_HIT_OFFSET_SURFACE);
        return hit;
    }

    std::optional<RayHit> ray_fan_sweep(QueryLedger::Tag tag, const Vector3 &origin, const Vector3 &sweep, float radius,
                                        int objtypes, unsigned int flags, const uintptr_t *skip_ents, int n_skip_ents,
                                        int *rays_blocked, FanMemory *memory, float walked)
    {
        DMK_PROFILE_SCOPE("phys.ray_fan");
        if (rays_blocked != nullptr)
        {
            *rays_blocked = 0;
        }
        const float len = sweep.magnitude();
        // Written so a NaN length also returns: a NaN ray must never reach the engine.
        if (!(len >= 1e-4f))
        {
            return std::nullopt;
        }
        const Vector3 dir = sweep / len;

        // Two axes perpendicular to the sweep: right is level (across world up), up is right x dir. One continuous
        // basis at every pitch, so the ray pattern never turns as the camera passes a pitch (world X is the seed only
        // straight up or down, which the camera never reaches).
        Vector3 right = dir.cross(Vector3{0.0f, 0.0f, 1.0f});
        if (right.magnitude() < 1e-3f)
        {
            right = dir.cross(Vector3{1.0f, 0.0f, 0.0f});
        }
        const float rlen = right.magnitude();
        if (rlen < 1e-4f)
        {
            // Degenerate: fall back to the single centre ray, which stands for the whole fan.
            std::optional<RayHit> centre =
                ray_world_intersection(tag, origin, sweep, objtypes, flags, skip_ents, n_skip_ents);
            if (rays_blocked != nullptr && centre.has_value())
            {
                *rays_blocked = k_fan_rays;
            }
            return centre;
        }
        right = right / rlen;
        const Vector3 up = right.cross(dir); // already unit (right and dir are orthonormal)

        // Centre + four parallel rays offset by the radius => a square tube around the sweep.
        const Vector3 offsets[k_fan_rays] = {Vector3{0.0f, 0.0f, 0.0f}, right * radius, right * (-radius), up * radius,
                                             up * (-radius)};

        // A remembered first hit past this start on a collider not skipped is still the first hit; one this close to
        // the start or nearer is cast again.
        constexpr float k_memory_ahead = 1e-3f;
        const auto skipped = [skip_ents, n_skip_ents](uintptr_t collider)
        {
            return skip_ents != nullptr && collider != 0 &&
                   std::find(skip_ents, skip_ents + n_skip_ents, collider) != skip_ents + n_skip_ents;
        };
        std::optional<RayHit> best;
        int blocked = 0;
        for (int ray = 0; ray < k_fan_rays; ++ray)
        {
            std::optional<RayHit> h;
            const bool remembered =
                memory != nullptr && memory->cast[ray] &&
                (!memory->hit[ray].has_value() ||
                 (memory->hit[ray]->m_distance > walked + k_memory_ahead && !skipped(memory->hit[ray]->m_collider)));
            if (remembered)
            {
                h = memory->hit[ray];
                if (h.has_value())
                {
                    h->m_distance -= walked;
                }
            }
            else
            {
                h = ray_world_intersection(tag, origin + offsets[ray], sweep, objtypes, flags, skip_ents, n_skip_ents);
                if (memory != nullptr)
                {
                    memory->cast[ray] = true;
                    memory->hit[ray] = h;
                    if (h.has_value())
                    {
                        memory->hit[ray]->m_distance += walked;
                    }
                }
            }
            if (!h.has_value())
            {
                continue;
            }
            ++blocked;
            if (!best.has_value() || h->m_distance < best->m_distance)
            {
                best = h;
            }
        }
        if (rays_blocked != nullptr)
        {
            *rays_blocked = blocked;
        }
        return best;
    }

    uintptr_t static_brush_render_node(uintptr_t collider) noexcept
    {
        if (collider == 0 || !DMK::memory::is_plausible_ptr(DMK::Address{collider}))
        {
            return 0;
        }
        // +0x28 packs m_iForeignData in the low 16 bits and m_iForeignFlags in the high 16 bits. The engine's
        // GetiForeignData reads only the signed low word, so compare that and ignore the flags.
        const auto ftype =
            DMK::memory::read<std::int16_t>(DMK::Address{collider + Constants::PHYS_ENTITY_FOREIGN_TYPE_OFFSET});
        if (!ftype || *ftype != Constants::PHYS_FOREIGN_ID_STATIC)
        {
            return 0; // not a static brush owner (entity-attached / pure-physics): no usable render node
        }
        const auto node =
            DMK::memory::read<uintptr_t>(DMK::Address{collider + Constants::PHYS_ENTITY_FOREIGN_DATA_OFFSET});
        if (!node || !DMK::memory::is_plausible_ptr(DMK::Address{*node}))
        {
            return 0;
        }
        return *node;
    }

} // namespace TPVCamera
