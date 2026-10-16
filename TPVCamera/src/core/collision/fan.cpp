/**
 * @file core/collision/fan.cpp
 * @brief The camera arm's ray fan (see fan.hpp).
 */

#include "core/collision/fan.hpp"

#include <algorithm>

namespace TPVCamera
{
    std::optional<RayHit> ray_fan_sweep(WorldProbe &probe, QueryLedger::Tag tag, const Vector3 &origin,
                                        const Vector3 &sweep, float radius, int objtypes, unsigned int flags,
                                        const std::uintptr_t *skip_ents, int n_skip_ents, int *rays_blocked,
                                        FanMemory *memory, float walked)
    {
        const ProbeScope scope(probe, "phys.ray_fan");
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
            std::optional<RayHit> centre = probe.ray(tag, origin, sweep, objtypes, flags, skip_ents, n_skip_ents);
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
        const auto skipped = [skip_ents, n_skip_ents](std::uintptr_t collider)
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
                h = probe.ray(tag, origin + offsets[ray], sweep, objtypes, flags, skip_ents, n_skip_ents);
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
} // namespace TPVCamera
