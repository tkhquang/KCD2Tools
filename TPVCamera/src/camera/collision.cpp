/**
 * @file camera/collision.cpp
 * @brief The camera collision on the live game (see collision.hpp).
 */

#include "camera/collision.hpp"
#include "camera/collision_trace.hpp"
#include "camera/live_probe.hpp"
#include "config.hpp"
#include "constants.hpp"
#ifdef TPVCAMERA_DEV_BUILD
#include "dev/frame_recorder.hpp"
#endif
#include "hooks/occluder_fade.hpp"
#include "physics_raycast.hpp"
#include "render_occlusion.hpp"

#include <DetourModKit.hpp>

#include <algorithm>
#include <array>
#include <cstdint>

namespace TPVCamera
{
    namespace
    {
        /** @brief The collision's trace lines in the log (see CollisionTrace). */
        class LiveCollisionTrace final : public CollisionTrace
        {
        public:
            void motion(EaseMotion motion) noexcept override { note_collision_motion(motion); }

            void walk(std::span<const WalkStep> steps, float desired_distance) noexcept override
            {
                log_walk_change(steps, desired_distance);
            }

            /**
             * @details Written only while Trace logging is enabled. The object is named from what the walk already
             *          knows, so the line runs no physics query: the collider's foreign render-node link (none for
             *          baked level physics), the scanned node the collision decided on with its class, and the camera
             *          rule that classed it, with the flags of the part hit (its geom_colltype bits), which tell which
             *          probes a part stops.
             */
            void physics_hit(const RayHit &hit, float surface, float desired_distance, int stepped,
                             const Vector3 &camera, const Vector3 &pivot) noexcept override
            {
                if (!DMK::log().is_enabled(DMK::LogLevel::Trace))
                {
                    return;
                }
                const Vector3 &hp = hit.m_point;
                const Vector3 &hn = hit.m_normal;
                const int hit_terrain = hit.m_terrain;
                const uintptr_t collider = hit.m_collider;
                void *linked = reinterpret_cast<void *>(static_brush_render_node(collider));
                char linked_name[160] = {};
                render_node_name(linked, linked_name, static_cast<int>(sizeof(linked_name)));
                Rules::Behaviour matched_cls = Rules::Behaviour::Collide;
                void *matched =
                    (hit_terrain == 0 && collider != 0) ? occluder_node_for_collider(collider, &matched_cls) : nullptr;
                char matched_name[160] = {};
                render_node_name(matched, matched_name, static_cast<int>(sizeof(matched_name)));
                char rule[160] = {};
                occluder_rule_where(matched, rule, static_cast<int>(sizeof(rule)));
                float cbox[6] = {};
                (void)read_collider_box(collider, cbox);
                std::uint32_t part_flags = 0;
                if (collider != 0 && hit.m_part >= 0)
                {
                    if (const auto parts = DMK::memory::read<std::uintptr_t>(
                            DMK::Address{collider + Constants::PHYS_ENTITY_PARTS_OFFSET});
                        parts && *parts != 0)
                    {
                        const auto flags = DMK::memory::read<std::uint32_t>(DMK::Address{
                            *parts + static_cast<std::uintptr_t>(hit.m_part) * Constants::PHYS_PART_STRIDE +
                            Constants::PHYS_PART_FLAGS_OFFSET});
                        part_flags = flags ? *flags : 0;
                    }
                }
                (void)DMK::log().try_log(
                    DMK::LogLevel::Trace,
                    "PhysicsCollision HIT: bTerrain={} dist={} of {} stepped={} collider={:#x} part={} "
                    "partFlags={:#x} surface={} colliderExt=({}, {}, {}) linked={:#x} \"{}\" matched=\"{}\" class={} "
                    "rule={} point=({}, {}, {}) normal=({}, {}, {}) cam=({}, {}, {}) pivot=({}, {}, {})",
                    hit_terrain, surface, desired_distance, stepped, collider, hit.m_part, part_flags, hit.m_surface,
                    cbox[3] - cbox[0], cbox[4] - cbox[1], cbox[5] - cbox[2], reinterpret_cast<uintptr_t>(linked),
                    linked_name, matched_name, matched != nullptr ? Rules::behaviour_name(matched_cls) : "unmatched",
                    rule[0] != '\0' ? rule : "-", hp.x, hp.y, hp.z, hn.x, hn.y, hn.z, camera.x, camera.y, camera.z,
                    pivot.x, pivot.y, pivot.z);
            }

            void arm_mesh(void *node, float at, float desired_distance) noexcept override
            {
                char name[160] = {};
                render_node_name(node, name, static_cast<int>(sizeof(name)));
                if (node != nullptr)
                {
                    (void)DMK::log().try_log(DMK::LogLevel::Trace,
                                             "Camera: arm stops at the drawn mesh of \"{}\" at {:.2f} of {:.2f} m",
                                             name, at, desired_distance);
                }
                else
                {
                    (void)DMK::log().try_log(DMK::LogLevel::Trace, "Camera: arm clear of drawn structure meshes");
                }
            }

            void pull_in(PullInTrace what, float behind) noexcept override
            {
                switch (what)
                {
                case PullInTrace::Trailing:
                    (void)DMK::log().try_log(DMK::LogLevel::Trace,
                                             "Camera: the pull-in eases in through open air, {:.2f} m behind its "
                                             "target",
                                             behind);
                    break;
                case PullInTrace::Blocked:
                    (void)DMK::log().try_log(DMK::LogLevel::Trace,
                                             "Camera: the pull-in's path is not open, so it pulls in at once");
                    break;
                case PullInTrace::Caught:
                    (void)DMK::log().try_log(DMK::LogLevel::Trace, "Camera: the pull-in caught up with its target");
                    break;
                }
            }

            void shoulder(bool blocked, float distance, void *node, Rules::Behaviour cls,
                          float lateral) noexcept override
            {
                if (!blocked)
                {
                    (void)DMK::log().try_log(DMK::LogLevel::Trace, "Camera: shoulder clear ({:.2f} m)", lateral);
                    return;
                }
                char name[160] = {};
                render_node_name(node, name, static_cast<int>(sizeof(name)));
                (void)DMK::log().try_log(DMK::LogLevel::Trace,
                                         "Camera: shoulder stops at {:.2f} of {:.2f} m on \"{}\" ({})", distance,
                                         lateral, node != nullptr ? name : "terrain or unmatched",
                                         node != nullptr ? Rules::behaviour_name(cls) : "-");
            }
        };
    } // namespace

    CollisionParams collision_params() noexcept
    {
        const LiveSettings &cfg = settings();
        constexpr auto rel = std::memory_order_relaxed;
        return CollisionParams{
            .enable = cfg.enable_collision.load(rel),
            .skin = cfg.collision_skin.load(rel),
            .return_speed = cfg.collision_return_speed.load(rel),
            .pull_in_seconds = cfg.collision_pull_in_seconds.load(rel),
            .radius = cfg.collision_radius.load(rel),
            .beam_gate = cfg.use_beam_gate.load(rel),
        };
    }

    CollisionTrace &live_collision_trace() noexcept
    {
        static LiveCollisionTrace s_trace;
        return s_trace;
    }

    Vector3 resolve_camera_collision(CollisionState &state, const CollisionInput &in)
    {
#ifdef TPVCAMERA_DEV_BUILD
        if (FrameRecorder::recording())
        {
            return FrameRecorder::arm(state, in, collision_params(), live_probe(), live_collision_trace());
        }
#endif
        return resolve_camera_collision(state, in, collision_params(), live_probe(), live_collision_trace());
    }

    float resolve_shoulder_collision(CollisionState &state, const ShoulderInput &in)
    {
#ifdef TPVCAMERA_DEV_BUILD
        if (FrameRecorder::recording())
        {
            return FrameRecorder::shoulder(state, in, collision_params(), live_probe(), live_collision_trace());
        }
#endif
        return resolve_shoulder_collision(state, in, collision_params(), live_probe(), live_collision_trace());
    }

    void submit_fade_set(const CollisionState &state, float delta_time)
    {
        const float strength =
            1.0f - std::clamp(settings().occluder_fade_min_opacity.load(std::memory_order_relaxed), 0.0f, 1.0f);
        std::array<FadeRequest, Constants::OCCLUDER_FADE_MAX_NODES> requests{};
        const auto n = static_cast<std::size_t>(state.fade_count);
        for (std::size_t i = 0; i < n; ++i)
        {
            const FadeEntry &e = state.fade_nodes[i];
            // OccluderFadeMinOpacity of every object stays, the camera inside it or not: one the camera is buried in or
            // inside only starts at once instead of after the listing delay. A full fade there makes it vanish and pop
            // back once the camera leaves it, on every pass.
            requests[i] = FadeRequest{e.node, strength, e.buried || e.inside, render_node_identity(e.node)};
        }
        log_fade_set_change(std::span<const FadeEntry>(state.fade_nodes.data(), n));
        update_occluder_fade(std::span<const FadeRequest>(requests.data(), n), delta_time);
    }
} // namespace TPVCamera
