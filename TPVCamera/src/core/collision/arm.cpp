/**
 * @file core/collision/arm.cpp
 * @brief Camera collision along the arm, the shoulder collision and the occluder fade set (see arm.hpp).
 */

#include "core/collision/arm.hpp"
#include "core/collision/fan.hpp"

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
        /// The fade's sightlines down the character's middle: the head, the chest and the hips.
        constexpr int k_fade_spine_sightlines = 3;
        /// Below this horizontal length (meters) the view to the chest has no direction to put the side lines across.
        constexpr float k_fade_side_min_across = 1e-4f;
        static_assert(Constants::OCCLUDER_FADE_SIGHTLINES == k_fade_spine_sightlines + 2,
                      "fade_sightlines() writes the spine lines, then one side line each way");

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
         * @brief Adds the props on the fade's sightlines from @p from (fade_sightlines()), and the props and bushes
         *        @p from is buried in, to the fade set.
         * @details The sightline set is re-cast only when it is older than k_fade_sightline_period or @p from moved
         *          k_fade_sightline_move since it was cast. Otherwise the cached set is added again.
         */
        void add_sightline_nodes(CollisionState &state, const Vector3 &from, const CollisionInput &in,
                                 WorldProbe &probe)
        {
            if (state.sight_age >= k_fade_sightline_period ||
                (from - state.sight_cam).magnitude_squared() > k_fade_sightline_move * k_fade_sightline_move)
            {
                state.sight_age = 0.0f;
                state.sight_cam = from;
                std::array<Sightline, Constants::OCCLUDER_FADE_SIGHTLINES> lines{};
                const int n_lines = fade_sightlines(from, in.head, in.hip, in.fade_body_width, lines);
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
                state.sight_count = probe.occluders_on_sightlines(
                    lines.data(), n_lines, in.fade_body_margin, std::span<void *const>(inside.data(), n_inside),
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
            // The colliders stepped past, which every later cast of the walk skips.
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
         *          visible object is the collision. With @p beam_gate, a hit that matches no visible object is stepped
         *          past the same way, without a fade, unless every ray of the fan is blocked (UseBeamGate).
         *          RayWorldIntersection does not always honour the skip list (a baked collider can come back on the
         *          re-cast), so a collider met twice is walked past from just beyond the hit instead.
         */
        void walk_arm(ArmWalk &walk, CollisionState &state, const ArmLines &arm, const Vector3 &pivot,
                      const Vector3 &ray_dir, float desired_distance, float collision_radius, bool fade_on,
                      bool beam_gate, WorldProbe &probe, CollisionTrace &trace)
        {
            float walk_from = 0.0f; // meters along the arm the fan starts from
            int resumes = 0;
            WalkStep steps[k_walk_trace_max];
            int n_steps = 0;
            const auto note_step = [&](const RayHit &at, void *node, Rules::Behaviour cls, char what)
            {
                if (n_steps < k_walk_trace_max)
                {
                    steps[n_steps++] = WalkStep{at.m_collider, node, at.m_distance, cls, what};
                }
            };
            // Each re-cast asks the engine again only for the rays whose answer can have changed (FanMemory).
            FanMemory fan_memory;
            for (;;)
            {
                int rays_blocked = 0;
                std::optional<RayHit> h = ray_fan_sweep(
                    probe, QueryLedger::Tag::Fan, pivot + ray_dir * walk_from, ray_dir * (desired_distance - walk_from),
                    collision_radius, Constants::RWI_OBJTYPES_CAMERA, Constants::RWI_FLAGS_STOP_AT_SOLID,
                    walk.stepped_past, walk.n_stepped, &rays_blocked, &fan_memory, walk_from);
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
                const ProbeVerdict verdict = classify_probe_hit(probe, QueryLedger::Tag::DrawnOn, *h, h->m_distance,
                                                                arm.starts, ray_dir, &node, &cls);
                if (verdict == ProbeVerdict::Undrawn && ++resumes <= Constants::COLLISION_WALK_RESUME_MAX)
                {
                    note_step(*h, node, cls, 'u');
                    walk_from = h->m_distance + Constants::COLLISION_WALK_RESUME_STEP;
                    continue;
                }
                if (beam_gate && node == nullptr && h->m_terrain == 0 && h->m_collider != 0 &&
                    rays_blocked < k_fan_rays && walk.n_stepped < Constants::COLLISION_FADE_SKIP_MAX)
                {
                    // Matches nothing drawn, and part of the fan is clear past it to the camera: narrower than the
                    // probe, walked past without a fade.
                    note_step(*h, nullptr, cls, 'g');
                    walk.stepped_past[walk.n_stepped++] = h->m_collider;
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
                if (cls == Rules::Behaviour::Fade && probe.spans_tube(node, beyond))
                {
                    add_fade_node(state, node, 'a', false);
                    walk.props[walk.n_props] = node;
                    walk.prop_colliders[walk.n_props] = h->m_collider;
                    walk.prop_points[walk.n_props] = h->m_point;
                    walk.prop_entries[walk.n_props++] = h->m_distance;
                }
            }
            trace.walk(std::span<const WalkStep>(steps, static_cast<std::size_t>(n_steps)), desired_distance);
        }

        /**
         * @brief Hands the collision hit (the fan's solid-world result) to the trace when it starts to block or moves
         *        by more than 0.1 m, so a wrong stop is identifiable from the log; the trace names the object from
         *        what the walk knows, without a query.
         */
        void trace_physics_hit(CollisionState &state, const ArmWalk &walk, bool blocked, float surface,
                               float desired_distance, const Vector3 &camera, const Vector3 &pivot,
                               CollisionTrace &trace)
        {
            const float dd = blocked ? (surface - state.traced_distance) : 0.0f;
            const bool moved = (dd > 0.1f) || (dd < -0.1f);
            if (blocked && walk.fan.has_value() && (blocked != state.traced_blocked || moved))
            {
                trace.physics_hit(*walk.fan, surface, desired_distance, walk.n_stepped, camera, pivot);
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
        [[nodiscard]] bool camera_inside(const Vector3 &camera, const Vector3 &met_at, uintptr_t prop,
                                         WorldProbe &probe)
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
                const std::optional<RayHit> h =
                    probe.ray(QueryLedger::Tag::Inside, camera + dir * walked, dir * reach,
                              Constants::RWI_OBJTYPES_CAMERA, Constants::RWI_FLAGS_STOP_AT_SOLID, nullptr, 0);
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

        /** @brief How a pull-in follows its target this frame. */
        struct PullIn
        {
            // The most an immediate pull-in trails its target by: CollisionSkin less the lens margin.
            float slack = 0.0f;
            // The rate it follows the target at (1/s).
            float speed = k_pull_in_speed;
            // CollisionPullInSeconds is on: it may trail by more along a path through open air.
            bool eased = false;
        };

        /** @brief The pull-in for CollisionPullInSeconds @p seconds: 0 is at once, else ~95% of the way that long. */
        [[nodiscard]] PullIn pull_in_for(float skin, float seconds) noexcept
        {
            PullIn pull;
            pull.slack = std::max(0.0f, skin - k_lens_near_margin);
            if (seconds > 0.0f)
            {
                pull.speed = 3.0f / seconds;
                pull.eased = true;
            }
            return pull;
        }

        // The open-air path test re-casts its ray this far past each surface it walks past, at most this many times.
        constexpr float k_open_path_step = 0.02f;
        constexpr int k_open_path_casts = 4;

        /**
         * @brief Whether the camera can move from @p from to @p to through open air.
         * @details A ray along the path meets nothing that would stop a camera probe (classify_probe_hit(): terrain, a
         *          structure where it draws something, anything unmatched, and a prop too when the fade is off), and
         *          the path crosses no drawn man-made structure mesh (structure_on_arm(), so a cloth canopy with no
         *          physics counts). A prop or a plant the fade lets the camera through, and a structure's collision
         *          where it draws nothing, are walked past as the arm walks them. A path with more on it than
         *          k_open_path_casts rays can walk past is not open.
         */
        [[nodiscard]] bool open_path(const Vector3 &from, const Vector3 &to, bool fade_on, WorldProbe &probe)
        {
            const ProbeScope scope(probe, "camera.pull_in_path");
            const Vector3 span = to - from;
            const float length = span.magnitude();
            if (!(length >= 1e-3f))
            {
                return length < 1e-3f; // no distance to cover is open; a length that is not a number is not
            }
            const Vector3 unit = span / length;
            float walked = 0.0f;
            for (int casts = 0; walked < length; ++casts)
            {
                if (casts == k_open_path_casts)
                {
                    return false;
                }
                const std::optional<RayHit> h =
                    probe.ray(QueryLedger::Tag::PullIn, from + unit * walked, unit * (length - walked),
                              Constants::RWI_OBJTYPES_CAMERA, Constants::RWI_FLAGS_STOP_AT_SOLID, nullptr, 0);
                if (!h.has_value())
                {
                    break;
                }
                const float along = walked + h->m_distance;
                void *node = nullptr;
                Rules::Behaviour cls = Rules::Behaviour::Collide;
                const ProbeVerdict verdict = classify_probe_hit(probe, QueryLedger::Tag::PullIn, *h, along,
                                                                std::span<const Vector3>(&from, 1), unit, &node, &cls);
                if (verdict == ProbeVerdict::Stop || (verdict == ProbeVerdict::PassThrough && !fade_on))
                {
                    return false;
                }
                walked = along + k_open_path_step;
            }
            const Sightline path{from, to};
            return probe.structure_on_arm(QueryLedger::Tag::PullIn, &path, 1, 1, 1.0f, nullptr, false) > 1.0f;
        }

        /** @brief Hands a change of what the eased pull-in does to the trace. */
        void trace_pull_in(CollisionState &state, PullInTrace now, float behind, CollisionTrace &trace)
        {
            if (static_cast<std::uint8_t>(now) == state.pull_in_traced)
            {
                return;
            }
            state.pull_in_traced = static_cast<std::uint8_t>(now);
            trace.pull_in(now, behind);
        }

        /**
         * @brief One step of the collision ease toward @p target (see ease_collision_toward()).
         * @details An immediate pull-in never trails its target by more than PullIn::slack, so the lens stays short of
         *          the surface that stopped it. An eased one (CollisionPullInSeconds) follows at its own rate and may
         *          trail by more, but only where the camera gets from where it was on the last frame to where the
         *          ease puts it through open air (open_path()): the view of the character can be blocked while it
         *          catches up, and the lens never passes into or through a surface. Where the path is not open, the
         *          same frame is the immediate pull-in.
         */
        [[nodiscard]] EaseMotion ease_collision(CollisionState &state, float target, float desired,
                                                const CollisionInput &in, const Vector3 &ray_dir, float return_speed,
                                                const PullIn &pull, WorldProbe &probe, CollisionTrace &trace)
        {
            CollisionEase &ease = state.arm;
            if (pull.eased && ease.collision_valid)
            {
                const float distance = ease.collision_distance;
                const float desired_before = ease.collision_desired;
                const float speed = ease.collision_speed;
                const EaseMotion motion =
                    ease_collision_toward(ease, target, desired, in.delta_time, return_speed, desired, pull.speed);
                const float behind = ease.collision_distance - target;
                if (behind <= pull.slack)
                {
                    trace_pull_in(state, PullInTrace::Caught, 0.0f, trace);
                    return motion;
                }
                if (open_path(state.last_camera, in.pivot + ray_dir * ease.collision_distance, in.fade_on, probe))
                {
                    trace_pull_in(state, PullInTrace::Trailing, behind, trace);
                    return motion;
                }
                ease.collision_distance = distance;
                ease.collision_desired = desired_before;
                ease.collision_speed = speed;
                trace_pull_in(state, PullInTrace::Blocked, 0.0f, trace);
            }
            return ease_collision_toward(ease, target, desired, in.delta_time, return_speed, pull.slack, pull.speed);
        }

        /** @brief Records @p camera as where the collision put the camera this frame, and returns it. */
        [[nodiscard]] Vector3 placed(CollisionState &state, const Vector3 &camera) noexcept
        {
            state.last_camera = camera;
            return camera;
        }

        /**
         * @brief The throttled frame: reuses the last recompute's allowed distance and fade set, skipping the whole
         *        walk / sightline computation.
         * @details A result that held nothing in stays the live desired distance, so a zoom between recomputes is not
         *          held to the stale one. The hold drains here too; when it runs out the next frame recomputes, so the
         *          release starts. The camera still eases in or out between recomputes: once it has moved far enough,
         *          the sightlines are cast again from where it is now, on top of what the arm walk found.
         */
        [[nodiscard]] Vector3 reuse_collision(CollisionState &state, const CollisionInput &in, const Vector3 &ray_dir,
                                              float desired_distance, float return_speed, const PullIn &pull,
                                              WorldProbe &probe, CollisionTrace &trace)
        {
            trace.motion(ease_collision(state, state.cached_held_in ? state.cached_allowed : desired_distance,
                                        desired_distance, in, ray_dir, return_speed, pull, probe, trace));
            const Vector3 camera = in.pivot + ray_dir * state.arm.collision_distance;
            if (drain_hold(state.arm.hold_timer, in.delta_time) == HoldDrain::RanOut)
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
                add_sightline_nodes(state, camera, in, probe);
            }
            return placed(state, camera);
        }

        /** @brief Where the shoulder ray stopped, and at what. */
        struct ShoulderStop
        {
            float distance = 0.0f; // meters from the pivot
            void *node = nullptr;  // the scanned render node it matched, or nullptr (terrain, unmatched)
            Rules::Behaviour cls = Rules::Behaviour::Collide;
        };

        /**
         * @brief Walks one ray from @p pivot along @p dir for @p reach meters, as walk_arm() walks the fan.
         * @details A collider met a second time (RayWorldIntersection can return a skipped baked collider) or a
         *          structure's undrawn collision is walked past from just beyond the hit; past
         *          COLLISION_WALK_RESUME_MAX such resumes the ray counts as clear, so the offset is not pulled in by a
         *          surface the walk could not tell. A prop or a plant is skipped while @p fade_on, as the arm steps
         *          past it.
         * @return The stop, or std::nullopt when the ray reaches @p reach.
         */
        [[nodiscard]] std::optional<ShoulderStop> walk_shoulder(const Vector3 &pivot, const Vector3 &dir, float reach,
                                                                bool fade_on, WorldProbe &probe)
        {
            uintptr_t skip[Constants::COLLISION_FADE_SKIP_MAX]{};
            int n_skip = 0;
            float from = 0.0f;
            int resumes = 0;
            while (from < reach)
            {
                const std::optional<RayHit> h =
                    probe.ray(QueryLedger::Tag::Shoulder, pivot + dir * from, dir * (reach - from),
                              Constants::RWI_OBJTYPES_CAMERA, Constants::RWI_FLAGS_STOP_AT_SOLID, skip, n_skip);
                if (!h.has_value())
                {
                    return std::nullopt;
                }
                const float along = from + h->m_distance;
                const bool met_before =
                    h->m_collider != 0 && std::find(skip, skip + n_skip, h->m_collider) != skip + n_skip;
                ShoulderStop stop{.distance = along};
                const ProbeVerdict verdict =
                    met_before ? ProbeVerdict::Undrawn
                               : classify_probe_hit(probe, QueryLedger::Tag::Shoulder, *h, along,
                                                    std::span<const Vector3>(&pivot, 1), dir, &stop.node, &stop.cls);
                if (verdict == ProbeVerdict::Undrawn)
                {
                    if (++resumes > Constants::COLLISION_WALK_RESUME_MAX)
                    {
                        return std::nullopt;
                    }
                    from = along + Constants::COLLISION_WALK_RESUME_STEP;
                    continue;
                }
                if (verdict == ProbeVerdict::PassThrough && fade_on && n_skip < Constants::COLLISION_FADE_SKIP_MAX)
                {
                    skip[n_skip++] = h->m_collider;
                    continue;
                }
                return stop;
            }
            return std::nullopt;
        }

        /** @brief Hands a change of what stops the shoulder offset to the trace. */
        void trace_shoulder(ShoulderCollision &s, const std::optional<ShoulderStop> &stop, float lateral,
                            CollisionTrace &trace)
        {
            void *const node = stop.has_value() ? stop->node : nullptr;
            if (stop.has_value() == s.traced_blocked && node == s.traced_node)
            {
                return;
            }
            s.traced_blocked = stop.has_value();
            s.traced_node = node;
            trace.shoulder(stop.has_value(), stop.has_value() ? stop->distance : 0.0f, node,
                           stop.has_value() ? stop->cls : Rules::Behaviour::Collide, lateral);
        }
    } // namespace

    int fade_sightlines(const Vector3 &from, const Vector3 &head, const Vector3 &hip, float body_width,
                        std::span<Sightline, Constants::OCCLUDER_FADE_SIGHTLINES> out) noexcept
    {
        const Vector3 chest = (head + hip) * 0.5f;
        out[0] = {from, head};
        out[1] = {from, chest};
        out[2] = {from, hip};
        // The view's horizontal direction to the chest turned a quarter turn about world up: across the view in the
        // ground plane, whatever the camera pitch.
        const Vector3 toward = chest - from;
        const Vector3 across{-toward.y, toward.x, 0.0f};
        const float across_length = across.magnitude();
        if (!(body_width > 0.0f) || across_length < k_fade_side_min_across)
        {
            return k_fade_spine_sightlines;
        }
        const Vector3 side = across * (body_width / across_length);
        out[3] = {from, chest + side};
        out[4] = {from, chest - side};
        return Constants::OCCLUDER_FADE_SIGHTLINES;
    }

    ProbeVerdict classify_probe_hit(WorldProbe &probe, QueryLedger::Tag tag, const RayHit &hit, float along,
                                    std::span<const Vector3> line_starts, const Vector3 &dir, void **node,
                                    Rules::Behaviour *cls)
    {
        *cls = Rules::Behaviour::Collide;
        *node = (hit.m_terrain == 0 && hit.m_collider != 0) ? probe.node_for_collider(hit.m_collider, cls) : nullptr;
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
        return probe.structure_drawn_on(tag, *node, at_hit, static_cast<int>(n)) ? ProbeVerdict::Stop
                                                                                 : ProbeVerdict::Undrawn;
    }

    Vector3 resolve_camera_collision(CollisionState &state, const CollisionInput &in, const CollisionParams &params,
                                     WorldProbe &probe, CollisionTrace &trace)
    {
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
        // target by the skin less the lens margin, or, eased (CollisionPullInSeconds), by more through open air.
        const float skin = params.skin;
        const PullIn pull = pull_in_for(skin, params.pull_in_seconds);
        const float return_speed = params.return_speed;
        if (!params.enable)
        {
            // No collision in this preset: nothing holds the camera in, so it eases out to where the rig puts it, as
            // after any obstruction clears, instead of jumping there; the objects in front of the character still
            // fade. The hold goes, and the throttle with it, so turning collision back on walks the arm at once.
            state.arm.hold_timer = 0.0f;
            state.throttle_valid = false;
            if (!in.repeat_build)
            {
                state.fade_count = 0;
                state.arm_fade_count = 0;
                const Vector3 scan_points[4] = {in.pivot, in.desired, in.head, in.hip};
                probe.refresh_scan(scan_points, 4);
                if (in.fade_on)
                {
                    add_sightline_nodes(state, in.desired, in, probe);
                }
            }
            if (!(desired_distance > 1e-3f))
            {
                return placed(state, in.desired);
            }
            trace.motion(ease_collision_toward(state.arm, desired_distance, desired_distance, in.delta_time,
                                               return_speed, pull.slack, pull.speed));
            return placed(state, in.pivot + (to_camera / desired_distance) * state.arm.collision_distance);
        }

        if (!(desired_distance > 1e-3f))
        {
            // The camera sits on the pivot (or the arm is not a number): nothing to walk.
            return placed(state, in.desired);
        }
        const Vector3 ray_dir = to_camera / desired_distance;

        const float recompute_d2 = k_collision_recompute_dist * k_collision_recompute_dist;
        const std::uint64_t now_ms = probe.now_ms();
        const bool recompute = in.wall_class_changed || !state.throttle_valid || in.fade_on != state.throttle_fade_on ||
                               now_ms - state.stamp_ms >= k_collision_max_age_ms ||
                               (in.pivot - state.throttle_pivot).magnitude_squared() > recompute_d2 ||
                               (in.desired - state.throttle_cam).magnitude_squared() > recompute_d2;
        if (!recompute && state.arm.collision_valid)
        {
            return reuse_collision(state, in, ray_dir, desired_distance, return_speed, pull, probe, trace);
        }

        const ProbeScope scope(probe, "camera.collision_recompute");
        // CollisionRadius is the probes' half-width (the fan's ray offset and the drawn-mesh arm's line offset).
        const float collision_radius = params.radius;
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
            probe.refresh_scan(scan_points, 4);
        }

        ArmWalk walk;
        walk_arm(walk, state, arm, in.pivot, ray_dir, desired_distance, collision_radius, in.fade_on, params.beam_gate,
                 probe, trace);
        state.arm_fade_count = state.fade_count;
        // Where the nearest solid surface is along the arm: the fan's nearest ray hit. The camera keeps CollisionSkin
        // from it.
        float surface = walk.fan.has_value() ? walk.fan->m_distance : desired_distance;
        float blocking_distance = desired_distance;
        bool blocked = false;
        if (surface < desired_distance)
        {
            blocking_distance = std::max(0.0f, surface - skin);
            blocked = true;
        }
        trace_physics_hit(state, walk, blocked, surface, desired_distance, in.desired, in.pivot, trace);

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
            const float crossing =
                probe.structure_on_arm(QueryLedger::Tag::StructureOnArm, arm.lines, 5, 3,
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
                trace.arm_mesh(mesh_node, crossing * desired_distance, desired_distance);
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
        CollisionEase &ease = state.arm;
        float allowed_distance = blocked ? blocking_distance : desired_distance;
        if (blocked &&
            (!ease.collision_valid || allowed_distance <= ease.collision_distance + k_collision_recompute_dist))
        {
            ease.hold_timer = k_collision_hold_seconds;
        }
        else if (ease.collision_valid && drain_hold(ease.hold_timer, in.delta_time) == HoldDrain::Holding)
        {
            allowed_distance = std::min(allowed_distance, ease.collision_distance);
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
            if (last >= 0 && camera_inside(in.pivot + ray_dir * allowed_distance, walk.prop_points[last],
                                           walk.prop_colliders[last], probe))
            {
                for (int k = 0; k < state.fade_count; ++k)
                {
                    FadeEntry &e = state.fade_nodes[static_cast<std::size_t>(k)];
                    e.inside = e.inside || e.node == walk.props[last];
                }
            }
        }

        // Ease toward the allowed distance (a pull-IN that never clips a wall, a critically damped return-OUT), via the
        // shared helper the throttled frames also use.
        trace.motion(
            ease_collision(state, allowed_distance, desired_distance, in, ray_dir, return_speed, pull, probe, trace));
        const Vector3 camera = in.pivot + ray_dir * ease.collision_distance;

        // Fade whatever still hides the character from where the camera is: the sightlines from it to the head, chest
        // and hips, tested against the scanned meshes. This catches what the arm's rays do not (cloth with no ray
        // physics, a fence across the legs), and a prop or a bush the camera is buried in.
        if (in.fade_on)
        {
            add_sightline_nodes(state, camera, in, probe);
        }
        // With this frame's tests done, the mesh of a structure or prop the arm is about to reach is measured ahead, so
        // its first read does not land in the frame the arm meets it.
        probe.prefetch_meshes(arm.lines, 5);

        // Cache the freshly computed target so a held-still camera reuses it next frame (the throttle above).
        state.throttle_valid = true;
        state.throttle_fade_on = in.fade_on;
        state.throttle_pivot = in.pivot;
        state.throttle_cam = in.desired;
        state.cached_allowed = allowed_distance;
        state.cached_held_in = allowed_distance < desired_distance - k_collision_clear_slack;
        state.stamp_ms = now_ms;
        return placed(state, camera);
    }

    float resolve_shoulder_collision(CollisionState &state, const ShoulderInput &in, const CollisionParams &params,
                                     WorldProbe &probe, CollisionTrace &trace)
    {
        ShoulderCollision &s = state.shoulder;
        const float skin = params.skin;
        const float return_speed = params.return_speed;
        const PullIn pull = pull_in_for(skin, 0.0f);
        if (!in.enabled || !(in.lateral > 1e-3f))
        {
            // Off (or no offset to probe): a pulled-in offset eases back out, and once it is out the carry-over goes,
            // so the offset is the rig's own from then on.
            s.hold_timer = 0.0f;
            s.throttle_valid = false;
            if (!s.collision_valid || !(in.lateral > 1e-3f))
            {
                s.collision_valid = false;
                return in.lateral;
            }
            (void)ease_collision_toward(s, in.lateral, in.lateral, in.delta_time, return_speed, pull.slack, pull.speed);
            if (s.collision_distance >= in.lateral - k_collision_clear_slack)
            {
                s.collision_valid = false;
                return in.lateral;
            }
            return std::min(s.collision_distance, in.lateral);
        }

        const Vector3 focal = in.pivot + in.direction * in.lateral;
        const float recompute_d2 = k_collision_recompute_dist * k_collision_recompute_dist;
        const std::uint64_t now_ms = probe.now_ms();
        const bool recompute = !s.throttle_valid || !s.collision_valid ||
                               now_ms - s.stamp_ms >= k_collision_max_age_ms ||
                               (in.pivot - s.throttle_pivot).magnitude_squared() > recompute_d2 ||
                               (focal - s.throttle_focal).magnitude_squared() > recompute_d2;
        float allowed = in.lateral;
        if (recompute)
        {
            const std::optional<ShoulderStop> stop =
                walk_shoulder(in.pivot, in.direction, in.lateral + skin, in.fade_on, probe);
            trace_shoulder(s, stop, in.lateral, trace);
            // The ray reaches CollisionSkin past the shoulder, so any stop holds the offset in.
            const bool blocked = stop.has_value();
            if (blocked)
            {
                allowed = std::max(0.0f, stop->distance - skin);
            }
            // The arm's hold: a surface that holds the offset where it is, or nearer, keeps it from easing back out
            // for k_collision_hold_seconds, so a ray that grazes a post on alternate frames does not swing the camera.
            if (blocked && (!s.collision_valid || allowed <= s.collision_distance + k_collision_recompute_dist))
            {
                s.hold_timer = k_collision_hold_seconds;
            }
            else if (s.collision_valid && drain_hold(s.hold_timer, in.delta_time) == HoldDrain::Holding)
            {
                allowed = std::min(allowed, s.collision_distance);
            }
            s.throttle_valid = true;
            s.throttle_pivot = in.pivot;
            s.throttle_focal = focal;
            s.cached_allowed = allowed;
            s.cached_held_in = allowed < in.lateral - k_collision_clear_slack;
            s.stamp_ms = now_ms;
        }
        else
        {
            allowed = s.cached_held_in ? s.cached_allowed : in.lateral;
            if (drain_hold(s.hold_timer, in.delta_time) == HoldDrain::RanOut)
            {
                s.throttle_valid = false;
            }
        }
        (void)ease_collision_toward(s, allowed, in.lateral, in.delta_time, return_speed, pull.slack, pull.speed);
        return std::min(s.collision_distance, in.lateral);
    }

    void reset_collision_carry(CollisionState &state) noexcept
    {
        state.arm.collision_valid = false;
        state.arm.collision_speed = 0.0f;
        state.arm.hold_timer = 0.0f;
    }

    void reset_shoulder_carry(CollisionState &state) noexcept
    {
        state.shoulder.collision_valid = false;
        state.shoulder.collision_speed = 0.0f;
        state.shoulder.hold_timer = 0.0f;
        state.shoulder.throttle_valid = false;
    }
} // namespace TPVCamera
