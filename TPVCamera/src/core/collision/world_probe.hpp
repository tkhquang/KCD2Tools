/**
 * @file core/collision/world_probe.hpp
 * @brief The world the camera's probes ask: physics rays, the occluder scan and the scanned render meshes.
 *
 * @details The camera collision and the lens decide only from these answers and the camera's own state, so a recorded
 *          set of answers replays a frame exactly (tests/replay). The live probe (camera/live_probe.hpp) forwards each
 *          call to the engine; a recording probe wraps it in DEV builds. Engine-free (no Windows or DetourModKit
 *          header), so the local tests compile it as is. Main thread only, like the camera frame that asks.
 */
#ifndef TPVCAMERA_CORE_COLLISION_WORLD_PROBE_HPP
#define TPVCAMERA_CORE_COLLISION_WORLD_PROBE_HPP

#include "core/collision/probe_types.hpp"
#include "core/math/vec.hpp"
#include "core/rules/camera_rules.hpp"
#include "query_ledger.hpp"

#include <cstdint>
#include <optional>
#include <span>

namespace TPVCamera
{
    /** @brief The world queries of the camera's probes (see render_occlusion.hpp and physics_raycast.hpp). */
    class WorldProbe
    {
    public:
        WorldProbe() = default;
        WorldProbe(const WorldProbe &) = delete;
        WorldProbe &operator=(const WorldProbe &) = delete;
        WorldProbe(WorldProbe &&) = delete;
        WorldProbe &operator=(WorldProbe &&) = delete;
        virtual ~WorldProbe() = default;

        /** @brief RayWorldIntersection: the nearest hit along @p sweep from @p origin, skipping @p skip. */
        [[nodiscard]] virtual std::optional<RayHit> ray(QueryLedger::Tag tag, const Vector3 &origin,
                                                        const Vector3 &sweep, int objtypes, unsigned int flags,
                                                        const std::uintptr_t *skip, int n_skip) = 0;

        /** @brief refresh_occluder_scan(): keeps the scan around @p points current. */
        virtual void refresh_scan(const Vector3 *points, int n_points) = 0;

        /** @brief occluder_node_for_collider(): the scanned node a collider belongs to, with its class. */
        [[nodiscard]] virtual void *node_for_collider(std::uintptr_t collider, Rules::Behaviour *cls) = 0;

        /** @brief structure_drawn_on(): whether structure @p node draws something along @p lines. */
        [[nodiscard]] virtual bool structure_drawn_on(QueryLedger::Tag tag, void *node, const Sightline *lines,
                                                      int n_lines) = 0;

        /** @brief structure_on_arm(): where @p lines meet a drawn man-made structure, as a fraction of their length. */
        [[nodiscard]] virtual float structure_on_arm(QueryLedger::Tag tag, const Sightline *lines, int n_lines,
                                                     int min_lines, float max_fraction, void **node_out,
                                                     bool trace) = 0;

        /** @brief spans_tube(): whether @p node draws across the five-line tube @p tube. */
        [[nodiscard]] virtual bool spans_tube(void *node, const Sightline *tube) = 0;

        /** @brief occluders_on_sightlines(): the props crossing @p lines, and those the camera is buried in. */
        [[nodiscard]] virtual int occluders_on_sightlines(const Sightline *lines, int n_lines,
                                                          std::span<void *const> skip, SightlineOccluder *out,
                                                          int cap) = 0;

        /** @brief prefetch_occluder_meshes(): measures ahead the meshes @p lines are about to reach. */
        virtual void prefetch_meshes(const Sightline *lines, int n_lines) = 0;

        /** @brief stopping_node_near(): whether a scanned stopping node's box comes within @p pad of @p box. */
        [[nodiscard]] virtual bool stopping_node_near(const float *box, float pad) = 0;

        /** @brief Real time in milliseconds, which the collision throttle and the lens reuse age by. */
        [[nodiscard]] virtual std::uint64_t now_ms() = 0;

        /** @brief Opens a named profile scope (a string literal); a probe without a profiler ignores it. */
        virtual void scope_begin(const char *name) noexcept = 0;

        /** @brief Closes the scope scope_begin() opened last. */
        virtual void scope_end() noexcept = 0;
    };

    /** @brief A profile scope on @p probe for the lifetime of the guard. */
    class ProbeScope
    {
    public:
        ProbeScope(WorldProbe &probe, const char *name) noexcept : m_probe(probe) { m_probe.scope_begin(name); }
        ProbeScope(const ProbeScope &) = delete;
        ProbeScope &operator=(const ProbeScope &) = delete;
        ProbeScope(ProbeScope &&) = delete;
        ProbeScope &operator=(ProbeScope &&) = delete;
        ~ProbeScope() { m_probe.scope_end(); }

    private:
        WorldProbe &m_probe;
    };
} // namespace TPVCamera

#endif // TPVCAMERA_CORE_COLLISION_WORLD_PROBE_HPP
