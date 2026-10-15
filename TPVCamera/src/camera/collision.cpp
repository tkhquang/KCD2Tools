/**
 * @file camera/collision.cpp
 * @brief Camera collision along the arm and the occluder fade set (see collision.hpp).
 */

#include "camera/collision.hpp"
#include "camera/ease.hpp"
#include "config.hpp"
#include "dev/hook_profile.hpp"
#include "hooks/occluder_fade.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <optional>

namespace TPVCamera
{
    namespace
    {
        // After a recompute met a surface the camera does not ease back out at all for this long (seconds, drained on
        // every frame): a probe that grazes an edge and misses it the next frame, or an arm swung fast through clutter,
        // keeps the camera in instead of letting it ease out and snap back in.
        constexpr float k_collision_hold_seconds = 0.3f;
        // The arm's result is reused while the pivot and the desired camera stay within k_collision_recompute_dist of
        // where it was computed, for at most k_collision_max_age_ms of real time: a STATIC collider can still move (an
        // animated door is one), so a camera held still recomputes too. Real time, not the game clock, so a result
        // also expires across a pause or with collision off. A new fence class, a fade toggle or a lapsed hold
        // recomputes at once. The hold takes a surface within one recompute step of the camera as holding it where it
        // is.
        constexpr float k_collision_recompute_dist = 0.06f;
        constexpr std::uint64_t k_collision_max_age_ms = 100;
        // The fade's sightline set is refreshed at most this often (seconds), or once the camera moved this far
        // (meters).
        constexpr float k_fade_sightline_period = 1.0f / 15.0f;
        constexpr float k_fade_sightline_move = 0.3f;
        // What the walk met, for the trace: up to this many hits in order.
        constexpr int k_walk_trace_max = 8;
        // The most probe lines classify_probe_hit() tests a structure's drawn mesh on: the lens's nine rays.
        constexpr std::size_t k_probe_lines_max = 9;

        /** @brief How one drain of the collision hold left it. */
        enum class HoldDrain : std::uint8_t
        {
            Idle,    // no hold was running: nothing drained
            Holding, // still holding after this frame's drain
            RanOut,  // the hold ran out on this frame
        };

        /**
         * @brief Drains the collision hold by @p delta_time. The throttled frames and the recomputes drain the same
         *        timer, so the hold lasts k_collision_hold_seconds of frames whichever path each one takes.
         */
        [[nodiscard]] HoldDrain drain_hold(float &timer, float delta_time) noexcept
        {
            if (!(timer > 0.0f))
            {
                return HoldDrain::Idle;
            }
            timer -= delta_time;
            return timer > 0.0f ? HoldDrain::Holding : HoldDrain::RanOut;
        }

        /** @brief Adds @p node to the fade set once; a node already in it keeps its source and gains @p buried. */
        void add_fade_node(CollisionState &state, void *node, char source, bool buried) noexcept
        {
            if (node == nullptr)
            {
                return;
            }
            for (int i = 0; i < state.fade_count; ++i)
            {
                FadeEntry &e = state.fade_nodes[static_cast<std::size_t>(i)];
                if (e.node == node)
                {
                    e.buried = e.buried || buried;
                    return;
                }
            }
            if (state.fade_count < Constants::OCCLUDER_FADE_MAX_NODES)
            {
                state.fade_nodes[static_cast<std::size_t>(state.fade_count++)] = FadeEntry{node, source, buried};
            }
        }

        /**
         * @brief Adds the props on a sightline from @p from to the head, the chest and the hips, and the props and
         *        bushes @p from is buried in, to the fade set.
         * @details The sightline set is re-cast only when it is older than k_fade_sightline_period or @p from moved
         *          k_fade_sightline_move since it was cast; otherwise the cached set is added again.
         */
        void add_sightline_nodes(CollisionState &state, const Vector3 &from, const CollisionInput &in)
        {
            if (state.sight_age >= k_fade_sightline_period ||
                (from - state.sight_cam).magnitude_squared() > k_fade_sightline_move * k_fade_sightline_move)
            {
                state.sight_age = 0.0f;
                state.sight_cam = from;
                const Vector3 chest = (in.head + in.hip) * 0.5f;
                const Sightline lines[3] = {{from, in.head}, {from, chest}, {from, in.hip}};
                // The props the arm found the camera inside already fade at once; their meshes are not read again.
                std::array<void *, Constants::OCCLUDER_FADE_MAX_NODES> inside{};
                std::size_t n_inside = 0;
                for (int i = 0; i < state.fade_count; ++i)
                {
                    const FadeEntry &e = state.fade_nodes[static_cast<std::size_t>(i)];
                    if (e.inside)
                    {
                        inside[n_inside++] = e.node;
                    }
                }
                state.sight_count =
                    occluders_on_sightlines(lines, 3, std::span<void *const>(inside.data(), n_inside),
                                            state.sight_nodes.data(), Constants::OCCLUDER_FADE_MAX_NODES);
            }
            for (int i = 0; i < state.sight_count; ++i)
            {
                const SightlineOccluder &o = state.sight_nodes[static_cast<std::size_t>(i)];
                add_fade_node(state, o.node, 's', o.buried);
            }
        }

        /** @brief The arm's five parallel lines: its centre and four offsets by the collision radius. */
        struct ArmLines
        {
            Vector3 starts[5]; // the pivot plus each offset
            Sightline lines[5];
        };

        /** @brief What the walk along the arm met. */
        struct ArmWalk
        {
            std::optional<RayHit> fan; // the collision: terrain, a drawn structure, or anything unmatched
            // The colliders stepped past, which every later cast of the walk and the sphere skip.
            uintptr_t stepped_past[Constants::COLLISION_FADE_SKIP_MAX]{};
            int n_stepped = 0;
            // The props the arm stepped past and faded, their colliders, where a fan ray met each, and how far along
            // the arm each was entered.
            void *props[Constants::COLLISION_FADE_SKIP_MAX]{};
            uintptr_t prop_colliders[Constants::COLLISION_FADE_SKIP_MAX]{};
            Vector3 prop_points[Constants::COLLISION_FADE_SKIP_MAX];
            float prop_entries[Constants::COLLISION_FADE_SKIP_MAX]{};
            int n_props = 0;
        };

        /**
         * @brief Walks the arm with the RWI FAN (centre + 4 rays offset by the radius; RWI takes objtypes as a plain
         *        argument, so actors are provably excluded, and each hit carries its collider).
         * @details A hit on a prop or a plant (matched to its render node through the collider's foreign link or, for
         *          baked level physics, its world box) is stepped past: its collider is skipped, a prop fades, and the
         *          fan is re-cast. The first hit that is terrain, a structure where it draws something, or matches no
         *          visible object is the collision. RayWorldIntersection does not always honour the skip list (a baked
         *          collider can come back on the re-cast), so a collider met twice is walked past from just beyond the
         *          hit instead.
         */
        void walk_arm(ArmWalk &walk, CollisionState &state, const ArmLines &arm, const Vector3 &pivot,
                      const Vector3 &ray_dir, float desired_distance, float collision_radius, bool fade_on)
        {
            float walk_from = 0.0f; // meters along the arm the fan starts from
            int resumes = 0;
            WalkStep trace[k_walk_trace_max];
            int n_trace = 0;
            const auto note_step = [&](const RayHit &at, void *node, Rules::Behaviour cls, char what)
            {
                if (n_trace < k_walk_trace_max)
                {
                    trace[n_trace++] = WalkStep{at.m_collider, node, at.m_distance, cls, what};
                }
            };
            for (;;)
            {
                std::optional<RayHit> h = ray_fan_sweep(
                    QueryLedger::Tag::Fan, pivot + ray_dir * walk_from, ray_dir * (desired_distance - walk_from),
                    collision_radius, Constants::RWI_OBJTYPES_CAMERA, Constants::RWI_FLAGS_STOP_AT_SOLID,
                    walk.stepped_past, walk.n_stepped);
                if (h.has_value())
                {
                    h->m_distance += walk_from;
                }
                if (!h.has_value() || h->m_distance >= desired_distance)
                {
                    break; // open past here
                }
                const bool met_before =
                    h->m_collider != 0 && std::find(walk.stepped_past, walk.stepped_past + walk.n_stepped,
                                                    h->m_collider) != walk.stepped_past + walk.n_stepped;
                if (met_before)
                {
                    if (++resumes > Constants::COLLISION_WALK_RESUME_MAX)
                    {
                        note_step(*h, nullptr, Rules::Behaviour::Collide, 'x');
                        walk.fan = h; // cannot get past it: the collision
                        break;
                    }
                    note_step(*h, nullptr, Rules::Behaviour::Collide, 'r');
                    walk_from = h->m_distance + Constants::COLLISION_WALK_RESUME_STEP;
                    continue;
                }
                void *node = nullptr;
                Rules::Behaviour cls = Rules::Behaviour::Collide;
                const ProbeVerdict verdict =
                    classify_probe_hit(QueryLedger::Tag::DrawnOn, *h, h->m_distance, arm.starts, ray_dir, &node, &cls);
                if (verdict == ProbeVerdict::Undrawn && ++resumes <= Constants::COLLISION_WALK_RESUME_MAX)
                {
                    note_step(*h, node, cls, 'u');
                    walk_from = h->m_distance + Constants::COLLISION_WALK_RESUME_STEP;
                    continue;
                }
                if (verdict != ProbeVerdict::PassThrough || !fade_on ||
                    walk.n_stepped >= Constants::COLLISION_FADE_SKIP_MAX)
                {
                    note_step(*h, node, cls, h->m_terrain != 0 ? 't' : 'c');
                    walk.fan = h; // terrain, a structure, without a visible mesh, or unknown: the collision
                    break;
                }
                // Step past it. A prop fades where what it draws spans the arm both across and up and down, from where
                // the arm enters its collision out to the camera; the empty part of a collision box (under a cart,
                // between a fence's rails) and a post or a rail thinner than the arm let the camera through without a
                // fade. A tree or a bush does not fade: the tree see-through window opens the leaves in front of the
                // character, the near plane cuts the ones at the lens, and a bush the camera ends up buried in fades
                // through the sightline test. An invisible designer volume hides nothing (dissolving its no-draw
                // material would draw it).
                note_step(*h, node, cls, 'p');
                walk.stepped_past[walk.n_stepped++] = h->m_collider;
                Sightline beyond[5];
                for (int i = 0; i < 5; ++i)
                {
                    beyond[i] = {arm.starts[i] + ray_dir * h->m_distance, arm.lines[i].to};
                }
                if (cls == Rules::Behaviour::Fade && spans_tube(node, beyond))
                {
                    add_fade_node(state, node, 'a', false);
                    walk.props[walk.n_props] = node;
                    walk.prop_colliders[walk.n_props] = h->m_collider;
                    walk.prop_points[walk.n_props] = h->m_point;
                    walk.prop_entries[walk.n_props++] = h->m_distance;
                }
            }
            log_walk_change(std::span<const WalkStep>(trace, static_cast<std::size_t>(n_trace)), desired_distance);
        }

        /**
         * @brief Logs the collision hit (the solid-world fan/sphere result) and what the fan stepped past, so a wrong
         *        stop or a wrong fade is identifiable from the log.
         * @details Rate-limited to a meaningful change (block-state toggle or distance move > 0.1m) so it never spams
         *          a frame, and written only while Trace logging is enabled. The object is named from what the walk
         *          already knows, so the line runs no query: the collider's foreign render-node link (none for baked
         *          level physics), the scanned node the collision decided on with its class, and the camera rule that
         *          classed it.
         */
        void trace_physics_hit(CollisionState &state, const ArmWalk &walk, const std::optional<RayHit> &hit,
                               bool from_sphere, bool blocked, float surface, float desired_distance,
                               const Vector3 &camera, const Vector3 &pivot)
        {
            const float dd = blocked ? (surface - state.traced_distance) : 0.0f;
            const bool moved = (dd > 0.1f) || (dd < -0.1f);
            const bool trace_on = DMK::log().is_enabled(DMK::LogLevel::Trace);
            if (trace_on && blocked && (blocked != state.traced_blocked || moved))
            {
                // Identify the object from the fan (point + collider), not the sphere's synthetic hit.
                const RayHit &id_hit = walk.fan.has_value() ? *walk.fan : *hit;
                const Vector3 &hp = id_hit.m_point;
                const Vector3 &hn = id_hit.m_normal;
                const int hit_terrain = id_hit.m_terrain;
                const uintptr_t collider = id_hit.m_collider;
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
                // The flags of the part hit (its geom_colltype bits): which probes a part stops is read off them.
                std::uint32_t part_flags = 0;
                if (collider != 0 && id_hit.m_part >= 0)
                {
                    if (const auto parts = DMK::memory::read<std::uintptr_t>(
                            DMK::Address{collider + Constants::PHYS_ENTITY_PARTS_OFFSET});
                        parts && *parts != 0)
                    {
                        const auto flags = DMK::memory::read<std::uint32_t>(DMK::Address{
                            *parts + static_cast<std::uintptr_t>(id_hit.m_part) * Constants::PHYS_PART_STRIDE +
                            Constants::PHYS_PART_FLAGS_OFFSET});
                        part_flags = flags ? *flags : 0;
                    }
                }
                (void)DMK::log().try_log(
                    DMK::LogLevel::Trace,
                    "PhysicsCollision HIT: src={} bTerrain={} dist={} of {} stepped={} collider={:#x} part={} "
                    "partFlags={:#x} surface={} colliderExt=({}, {}, {}) linked={:#x} \"{}\" matched=\"{}\" class={} "
                    "rule={} point=({}, {}, {}) normal=({}, {}, {}) cam=({}, {}, {}) pivot=({}, {}, {})",
                    from_sphere ? "sphere" : "fan", hit_terrain, surface, desired_distance, walk.n_stepped, collider,
                    id_hit.m_part, part_flags, id_hit.m_surface, cbox[3] - cbox[0], cbox[4] - cbox[1],
                    cbox[5] - cbox[2], reinterpret_cast<uintptr_t>(linked), linked_name, matched_name,
                    matched != nullptr ? Rules::behaviour_name(matched_cls) : "unmatched", rule[0] != '\0' ? rule : "-",
                    hp.x, hp.y, hp.z, hn.x, hn.y, hn.z, camera.x, camera.y, camera.z, pivot.x, pivot.y, pivot.z);
            }
            state.traced_blocked = blocked;
            if (blocked)
            {
                state.traced_distance = surface;
            }
        }

        /**
         * @brief Whether the camera at @p camera is inside @p prop, the last prop the arm entered before it.
         * @details A ray from the camera back to the point where a fan ray met the prop shows whether the camera got
         *          out of it again: physics reports a surface only from outside, so meeting that prop on the way means
         *          the camera is past it, and reaching the point without meeting it means the camera is inside.
         *          Anything else on the way is walked past; with too much on the way the prop is not taken for one the
         *          camera is inside.
         */
        [[nodiscard]] bool camera_inside(const Vector3 &camera, const Vector3 &met_at, uintptr_t prop)
        {
            const Vector3 to = met_at - camera;
            const float length = to.magnitude();
            if (length < 0.05f)
            {
                return false;
            }
            const Vector3 dir = to / length;
            float walked = 0.0f;
            for (int step = 0; step < 4; ++step)
            {
                const float reach = length - 0.02f - walked;
                if (reach <= 0.0f)
                {
                    return true;
                }
                const std::optional<RayHit> h = ray_world_intersection(QueryLedger::Tag::Inside, camera + dir * walked,
                                                                       dir * reach, Constants::RWI_OBJTYPES_CAMERA,
                                                                       Constants::RWI_FLAGS_STOP_AT_SOLID, nullptr, 0);
                if (!h.has_value())
                {
                    return true;
                }
                if (h->m_collider == prop)
                {
                    return false;
                }
                walked += h->m_distance + 0.02f;
            }
            return false;
        }

        /**
         * @brief The throttled frame: reuses the last recompute's allowed distance and fade set, skipping the whole
         *        walk / sphere / sightline computation.
         * @details A result that held nothing in stays the live desired distance, so a zoom between recomputes is not
         *          held to the stale one. The hold drains here too; when it runs out the next frame recomputes, so the
         *          release starts. The camera still eases in or out between recomputes: once it has moved far enough,
         *          the sightlines are cast again from where it is now, on top of what the arm walk found.
         */
        [[nodiscard]] Vector3 reuse_collision(CameraState &cam, CollisionState &state, const CollisionInput &in,
                                              const Vector3 &ray_dir, float desired_distance, float return_speed,
                                              float pull_slack)
        {
            note_collision_motion(ease_collision_toward(cam,
                                                        state.cached_held_in ? state.cached_allowed : desired_distance,
                                                        desired_distance, in.delta_time, return_speed, pull_slack));
            const Vector3 camera = in.pivot + ray_dir * cam.collision_distance;
            if (drain_hold(cam.collision_hold_timer, in.delta_time) == HoldDrain::RanOut)
            {
                state.throttle_valid = false;
            }
            if (in.fade_on &&
                (camera - state.sight_cam).magnitude_squared() > k_fade_sightline_move * k_fade_sightline_move)
            {
                state.fade_count = state.arm_fade_count;
                for (int i = 0; i < state.fade_count; ++i)
                {
                    state.fade_nodes[static_cast<std::size_t>(i)].buried = false;
                }
                add_sightline_nodes(state, camera, in);
            }
            return camera;
        }
    } // namespace

    ProbeVerdict classify_probe_hit(QueryLedger::Tag tag, const RayHit &hit, float along,
                                    std::span<const Vector3> line_starts, const Vector3 &dir, void **node,
                                    Rules::Behaviour *cls)
    {
        *cls = Rules::Behaviour::Collide;
        *node = (hit.m_terrain == 0 && hit.m_collider != 0) ? occluder_node_for_collider(hit.m_collider, cls) : nullptr;
        if (*node == nullptr)
        {
            return ProbeVerdict::Stop;
        }
        if (*cls != Rules::Behaviour::Collide)
        {
            return ProbeVerdict::PassThrough;
        }
        Sightline at_hit[k_probe_lines_max];
        const std::size_t n = std::min(line_starts.size(), k_probe_lines_max);
        for (std::size_t i = 0; i < n; ++i)
        {
            const Vector3 at = line_starts[i] + dir * along;
            at_hit[i] = {at - dir * Constants::COLLISION_DRAWN_REACH, at + dir * Constants::COLLISION_DRAWN_REACH};
        }
        return structure_drawn_on(tag, *node, at_hit, static_cast<int>(n)) ? ProbeVerdict::Stop : ProbeVerdict::Undrawn;
    }

    Vector3 resolve_camera_collision(CameraState &cam, CollisionState &state, const CollisionInput &in)
    {
        LiveSettings &cfg = settings();
        if (!in.repeat_build)
        {
            state.sight_age += in.delta_time;
        }
        if (!in.fade_on || state.fade_stale)
        {
            state.fade_count = 0;
            state.arm_fade_count = 0;
            state.sight_count = 0;
            state.sight_age = 1e9f;
            state.fade_stale = false;
        }
        if (in.wall_class_changed)
        {
            state.sight_age = 1e9f;
        }

        const Vector3 to_camera = in.desired - in.pivot;
        const float desired_distance = to_camera.magnitude();
        // CollisionSkin is the gap the camera keeps from whatever surface the probes found. A pull-in may lag its
        // target by the skin less the lens margin.
        const float skin = cfg.collision_skin.load(std::memory_order_relaxed);
        const float pull_slack = std::max(0.0f, skin - k_lens_near_margin);
        const float return_speed = cfg.collision_return_speed.load(std::memory_order_relaxed);
        if (!cfg.enable_collision.load(std::memory_order_relaxed))
        {
            // No collision in this preset: nothing holds the camera in, so it eases out to where the rig puts it, as
            // after any obstruction clears, instead of jumping there; the objects in front of the character still
            // fade. The hold goes, and the throttle with it, so turning collision back on walks the arm at once.
            cam.collision_hold_timer = 0.0f;
            state.throttle_valid = false;
            if (!in.repeat_build)
            {
                state.fade_count = 0;
                state.arm_fade_count = 0;
                const Vector3 scan_points[4] = {in.pivot, in.desired, in.head, in.hip};
                refresh_occluder_scan(scan_points, 4);
                if (in.fade_on)
                {
                    add_sightline_nodes(state, in.desired, in);
                }
            }
            if (!(desired_distance > 1e-3f))
            {
                return in.desired;
            }
            note_collision_motion(ease_collision_toward(cam, desired_distance, desired_distance, in.delta_time,
                                                        return_speed, pull_slack));
            return in.pivot + (to_camera / desired_distance) * cam.collision_distance;
        }

        if (!(desired_distance > 1e-3f))
        {
            return in.desired; // the camera sits on the pivot (or the arm is not a number): nothing to walk
        }
        const Vector3 ray_dir = to_camera / desired_distance;

        const float recompute_d2 = k_collision_recompute_dist * k_collision_recompute_dist;
        const std::uint64_t now_ms = GetTickCount64();
        const bool recompute = in.wall_class_changed || !state.throttle_valid || in.fade_on != state.throttle_fade_on ||
                               now_ms - state.stamp_ms >= k_collision_max_age_ms ||
                               (in.pivot - state.throttle_pivot).magnitude_squared() > recompute_d2 ||
                               (in.desired - state.throttle_cam).magnitude_squared() > recompute_d2;
        if (!recompute && cam.collision_valid)
        {
            return reuse_collision(cam, state, in, ray_dir, desired_distance, return_speed, pull_slack);
        }

        DMK_PROFILE_SCOPE("camera.collision_recompute");
        // CollisionRadius is the probes' half-width (the fan's ray offset, the sphere's radius, the drawn-mesh arm's
        // line offset).
        const float collision_radius = cfg.collision_radius.load(std::memory_order_relaxed);
        const Vector3 desired_cam = in.pivot + to_camera;
        // One continuous basis across the arm (side level, lift = side x arm), the one the fan uses too, for the
        // drawn-mesh arm's lines.
        Vector3 arm_side = ray_dir.cross(in.world_up);
        arm_side = (arm_side.magnitude() > 1e-3f) ? arm_side.normalized()
                                                  : ray_dir.cross(Vector3{1.0f, 0.0f, 0.0f}).normalized();
        const Vector3 arm_lift = arm_side.cross(ray_dir);
        const Vector3 offsets[5] = {Vector3{}, arm_side * collision_radius, arm_side * -collision_radius,
                                    arm_lift * collision_radius, arm_lift * -collision_radius};
        ArmLines arm;
        for (int i = 0; i < 5; ++i)
        {
            arm.starts[i] = in.pivot + offsets[i];
            arm.lines[i] = {arm.starts[i], desired_cam + offsets[i]};
        }
        state.fade_count = 0;
        {
            // The visible brushes, trees and entities around the arm and the character, for the hit match below, the
            // drawn-mesh test of the arm and the sightline test. The scan is cached and re-queried only when the arm
            // leaves it.
            const Vector3 scan_points[4] = {in.pivot, desired_cam, in.head, in.hip};
            refresh_occluder_scan(scan_points, 4);
        }

        ArmWalk walk;
        walk_arm(walk, state, arm, in.pivot, ray_dir, desired_distance, collision_radius, in.fade_on);
        state.arm_fade_count = state.fade_count;
        std::optional<RayHit> hit = walk.fan;
        bool from_sphere = false;
        // UseSphereCollision adds the swept SPHERE to the fan: its contact is continuous where the fan's five rays
        // graze an edge. The FAN is the collision AUTHORITY either way (its hits carry the collider and the normal);
        // the sphere is trusted only where it agrees with the fan's surface, and only to put that surface a little
        // nearer. With no fan hit there is no collision whatever the sphere would meet, so the sphere is swept only
        // after a fan hit, and only as far as its agreement window reaches: a contact beyond fan +
        // k_sphere_agree_beyond is rejected, so a longer sweep cannot change the result.
        if (walk.fan.has_value() && cfg.use_sphere_collision.load(std::memory_order_relaxed))
        {
            // The sweep's entTypes (SPWIParams +0x98) is the 0x101 = static|terrain mask, which the real PWI (1.5.6
            // sub_180811680) hands to GetEntitiesAround as its objtypes (EAX = 0x101 at 0x180811D8F), so the player's
            // living entity and its skeleton / worn-gear articulated entity are never candidates and need no skip
            // entry. The colliders the fan stepped past are skipped, so the sphere does not stop at a faded post (the
            // synchronous PWI walks every nSkipEnts entry).
            constexpr float k_sphere_agree_beyond = 0.05f;
            constexpr float k_sphere_agree_before = 0.10f;
            const float sweep_reach = std::min(desired_distance, walk.fan->m_distance + k_sphere_agree_beyond);
            const std::optional<RayHit> sphere =
                sphere_world_sweep(QueryLedger::Tag::SweepPwi, in.pivot, collision_radius, ray_dir * sweep_reach,
                                   Constants::RWI_OBJTYPES_CAMERA, walk.stepped_past, walk.n_stepped);
            // The FAN is the AUTHORITY: each of its hits carries the real collider and surface normal, while the
            // sphere wrapper returns only a distance. The sphere is trusted ONLY when it AGREES with the fan's world
            // surface (within the radius inset).
            if (sphere.has_value() &&
                sphere->m_distance >= walk.fan->m_distance - collision_radius - k_sphere_agree_before &&
                sphere->m_distance <= walk.fan->m_distance + k_sphere_agree_beyond &&
                sphere->m_distance + collision_radius < walk.fan->m_distance)
            {
                hit = sphere; // sphere agrees with the fan's world surface -> use it (continuous, no pump)
                from_sphere = true;
                HookProfile::count(HookProfile::Counter::SphereWin);
            }
        }

        // Where the nearest solid surface is along the arm: the fan's ray hit, or the sphere's contact plus its radius
        // (the sphere's centre stops CollisionRadius short of the surface). The camera keeps the same CollisionSkin
        // from it whichever probe found it, so a frame that switches between the fan and the sphere does not move the
        // camera.
        float surface = desired_distance;
        if (hit.has_value())
        {
            surface = hit->m_distance + (from_sphere ? collision_radius : 0.0f);
        }
        float blocking_distance = desired_distance;
        bool blocked = false;
        if (surface < desired_distance)
        {
            blocking_distance = std::max(0.0f, surface - skin);
            blocked = true;
        }
        trace_physics_hit(state, walk, hit, from_sphere, blocked, surface, desired_distance, in.desired, in.pivot);

        // A building's drawn mesh can stand inside its physics: a thatched roof's physics is its outer surface while
        // the drawn thatch hangs below it, so the arm can pass the physics and still end in the thatch. The cloth
        // canopies over market stalls have no physics at all. The arm (its centre and four offsets by the radius, like
        // the fan) is also tested against the render meshes of the man-made structures it passes through short of
        // where the physics stops it; one that at least three of the lines cross stops the camera CollisionSkin short
        // of where the third of them meets it (about where the centre does on a flat surface, and moving smoothly as
        // the arm swings into a sloped cloth or roof), so the lens never sits inside a drawn wall or roof, or above a
        // canopy the character stands under, while a guy rope or a chain with no physics does not stop it.
        {
            void *mesh_node = nullptr;
            const float crossing = structure_on_arm(QueryLedger::Tag::StructureOnArm, arm.lines, 5, 3,
                                                    surface / std::max(desired_distance, 1e-3f), &mesh_node, true);
            const float mesh_surface = crossing * desired_distance;
            if (crossing <= 1.0f && mesh_surface < surface)
            {
                surface = mesh_surface;
                blocking_distance = std::max(0.0f, mesh_surface - skin);
                blocked = true;
            }
            else
            {
                mesh_node = nullptr;
            }
            if (mesh_node != state.traced_mesh)
            {
                state.traced_mesh = mesh_node;
                char name[160] = {};
                render_node_name(mesh_node, name, static_cast<int>(sizeof(name)));
                if (mesh_node != nullptr)
                {
                    (void)DMK::log().try_log(DMK::LogLevel::Trace,
                                             "Camera: arm stops at the drawn mesh of \"{}\" at {:.2f} of {:.2f} m",
                                             name, crossing * desired_distance, desired_distance);
                }
                else
                {
                    (void)DMK::log().try_log(DMK::LogLevel::Trace, "Camera: arm clear of drawn structure meshes");
                }
            }
        }

        // The target: the blocking distance, or the desired one when the arm is clear. For k_collision_hold_seconds
        // after a recompute met a surface that holds the camera where it is (within one recompute step) or nearer,
        // the camera does not ease back out at all (drained on every frame, the throttled ones too), toward a clear
        // arm or a farther surface alike: the fan's rays meet a thin pole only on the frames it lies on one of them,
        // and on the others would let the camera out to whatever stands behind it, only to pull it back in. A surface
        // met farther out drains the hold instead, so the camera follows a surface that slides away once the hold runs
        // out. A pull-in is never held back. A hold that runs out on this frame releases at once: the throttled frames
        // after it reuse this target and drain nothing.
        float allowed_distance = blocked ? blocking_distance : desired_distance;
        if (blocked &&
            (!cam.collision_valid || allowed_distance <= cam.collision_distance + k_collision_recompute_dist))
        {
            cam.collision_hold_timer = k_collision_hold_seconds;
        }
        else if (cam.collision_valid && drain_hold(cam.collision_hold_timer, in.delta_time) == HoldDrain::Holding)
        {
            allowed_distance = std::min(allowed_distance, cam.collision_distance);
        }

        // A prop the arm let through (the hay on a wagon) that the lens sits inside fills the view, so its fade starts
        // at once instead of after the listing delay (FadeEntry::inside). Only the last prop entered before the camera
        // is tested (see camera_inside()). What only touches the lens, a corner or a branch, is cut by the lens near
        // plane instead (see k_lens_near_max).
        {
            int last = -1;
            for (int i = 0; i < walk.n_props; ++i)
            {
                if (walk.prop_entries[i] < allowed_distance &&
                    (last < 0 || walk.prop_entries[i] > walk.prop_entries[last]))
                {
                    last = i;
                }
            }
            if (last >= 0 &&
                camera_inside(in.pivot + ray_dir * allowed_distance, walk.prop_points[last], walk.prop_colliders[last]))
            {
                for (int k = 0; k < state.fade_count; ++k)
                {
                    FadeEntry &e = state.fade_nodes[static_cast<std::size_t>(k)];
                    e.inside = e.inside || e.node == walk.props[last];
                }
            }
        }

        // Ease toward the allowed distance (fast pull-IN so a wall is never clipped, a critically damped return-OUT),
        // via the shared helper the throttled frames also use.
        note_collision_motion(
            ease_collision_toward(cam, allowed_distance, desired_distance, in.delta_time, return_speed, pull_slack));
        const Vector3 camera = in.pivot + ray_dir * cam.collision_distance;

        // Fade whatever still hides the character from where the camera is: the sightlines from it to the head, chest
        // and hips, tested against the scanned meshes. This catches what the arm's rays do not (cloth with no ray
        // physics, a fence across the legs), and a prop or a bush the camera is buried in.
        if (in.fade_on)
        {
            add_sightline_nodes(state, camera, in);
        }
        // With this frame's tests done, the mesh of a structure or prop the arm is about to reach is measured ahead, so
        // its first read does not land in the frame the arm meets it.
        prefetch_occluder_meshes(arm.lines, 5);

        // Cache the freshly computed target so a held-still camera reuses it next frame (the throttle above).
        state.throttle_valid = true;
        state.throttle_fade_on = in.fade_on;
        state.throttle_pivot = in.pivot;
        state.throttle_cam = in.desired;
        state.cached_allowed = allowed_distance;
        state.cached_held_in = allowed_distance < desired_distance - k_collision_clear_slack;
        state.stamp_ms = now_ms;
        return camera;
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

    void reset_collision_carry(CameraState &cam) noexcept
    {
        cam.collision_valid = false;
        cam.collision_speed = 0.0f;
        cam.collision_hold_timer = 0.0f;
    }
} // namespace TPVCamera
