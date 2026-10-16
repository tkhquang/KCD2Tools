/**
 * @file camera/live_probe.cpp
 * @brief The live WorldProbe (see live_probe.hpp).
 */

#include "camera/live_probe.hpp"
#include "physics_raycast.hpp"
#include "render_occlusion.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

namespace TPVCamera
{
    std::optional<RayHit> LiveProbe::ray(QueryLedger::Tag tag, const Vector3 &origin, const Vector3 &sweep,
                                         int objtypes, unsigned int flags, const std::uintptr_t *skip, int n_skip)
    {
        return ray_world_intersection(tag, origin, sweep, objtypes, flags, skip, n_skip);
    }

    void LiveProbe::refresh_scan(const Vector3 *points, int n_points)
    {
        refresh_occluder_scan(points, n_points);
    }

    void *LiveProbe::node_for_collider(std::uintptr_t collider, Rules::Behaviour *cls)
    {
        return occluder_node_for_collider(collider, cls);
    }

    bool LiveProbe::structure_drawn_on(QueryLedger::Tag tag, void *node, const Sightline *lines, int n_lines)
    {
        return TPVCamera::structure_drawn_on(tag, node, lines, n_lines);
    }

    float LiveProbe::structure_on_arm(QueryLedger::Tag tag, const Sightline *lines, int n_lines, int min_lines,
                                      float max_fraction, void **node_out, bool trace)
    {
        return TPVCamera::structure_on_arm(tag, lines, n_lines, min_lines, max_fraction, node_out, trace);
    }

    bool LiveProbe::spans_tube(void *node, const Sightline *tube)
    {
        return TPVCamera::spans_tube(node, tube);
    }

    int LiveProbe::occluders_on_sightlines(const Sightline *lines, int n_lines, std::span<void *const> skip,
                                           SightlineOccluder *out, int cap)
    {
        return TPVCamera::occluders_on_sightlines(lines, n_lines, skip, out, cap);
    }

    void LiveProbe::prefetch_meshes(const Sightline *lines, int n_lines)
    {
        prefetch_occluder_meshes(lines, n_lines);
    }

    bool LiveProbe::stopping_node_near(const float *box, float pad)
    {
        return TPVCamera::stopping_node_near(box, pad);
    }

    std::uint64_t LiveProbe::now_ms()
    {
        return GetTickCount64();
    }

    void LiveProbe::scope_begin([[maybe_unused]] const char *name) noexcept
    {
#ifdef DMK_ENABLE_PROFILING
        if (depth_ < k_max_scopes)
        {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            scopes_[static_cast<std::size_t>(depth_)] = OpenScope{name, now.QuadPart};
        }
        ++depth_;
#endif
    }

    void LiveProbe::scope_end() noexcept
    {
#ifdef DMK_ENABLE_PROFILING
        if (depth_ <= 0)
        {
            return;
        }
        --depth_;
        if (depth_ < k_max_scopes)
        {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            const OpenScope &scope = scopes_[static_cast<std::size_t>(depth_)];
            DMK::Profiler::get_instance().record(scope.name, scope.started, now.QuadPart, GetCurrentThreadId());
        }
#endif
    }

    LiveProbe &live_probe() noexcept
    {
        static LiveProbe s_probe;
        return s_probe;
    }
} // namespace TPVCamera
