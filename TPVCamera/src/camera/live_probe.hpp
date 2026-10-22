/**
 * @file camera/live_probe.hpp
 * @brief The camera probes' world in the running game: each query forwarded to the engine (physics_raycast.hpp,
 *        render_occlusion.hpp), the real clock, and profile scopes recorded through the DetourModKit profiler.
 */
#ifndef TPVCAMERA_CAMERA_LIVE_PROBE_HPP
#define TPVCAMERA_CAMERA_LIVE_PROBE_HPP

#include "core/collision/world_probe.hpp"

#include <array>
#include <cstdint>

namespace TPVCamera
{
    /**
     * @brief The live WorldProbe.
     * @note Main thread only (the camera frame). Profile scopes nest k_max_scopes deep; deeper ones are not recorded.
     */
    class LiveProbe final : public WorldProbe
    {
    public:
        LiveProbe() = default;

        [[nodiscard]] std::optional<RayHit> ray(QueryLedger::Tag tag, const Vector3 &origin, const Vector3 &sweep,
                                                int objtypes, unsigned int flags, const std::uintptr_t *skip,
                                                int n_skip) override;
        void refresh_scan(const Vector3 *points, int n_points) override;
        [[nodiscard]] void *node_for_collider(std::uintptr_t collider, Rules::Behaviour *cls) override;
        [[nodiscard]] bool structure_drawn_on(QueryLedger::Tag tag, void *node, const Sightline *lines,
                                              int n_lines) override;
        [[nodiscard]] float structure_on_arm(QueryLedger::Tag tag, const Sightline *lines, int n_lines, int min_lines,
                                             float max_fraction, void **node_out, bool trace) override;
        [[nodiscard]] bool spans_tube(void *node, const Sightline *tube) override;
        [[nodiscard]] int occluders_on_sightlines(const Sightline *lines, int n_lines, float end_margin,
                                                  std::span<void *const> skip, SightlineOccluder *out,
                                                  int cap) override;
        void prefetch_meshes(const Sightline *lines, int n_lines) override;
        [[nodiscard]] bool stopping_node_near(const float *box, float pad) override;
        [[nodiscard]] std::uint64_t now_ms() override;
        void scope_begin(const char *name) noexcept override;
        void scope_end() noexcept override;

    private:
        static constexpr int k_max_scopes = 8;
        struct OpenScope
        {
            const char *name = nullptr;
            std::int64_t started = 0;
        };
        std::array<OpenScope, k_max_scopes> m_scopes{};
        int m_depth = 0;
    };

    /** @brief The camera frame's live probe. Main thread only. */
    [[nodiscard]] LiveProbe &live_probe() noexcept;
} // namespace TPVCamera

#endif // TPVCAMERA_CAMERA_LIVE_PROBE_HPP
