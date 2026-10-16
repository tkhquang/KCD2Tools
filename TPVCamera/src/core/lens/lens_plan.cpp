/**
 * @file core/lens/lens_plan.cpp
 * @brief The third-person lens near plane (see lens_plan.hpp).
 */

#include "core/lens/lens_plan.hpp"
#include "constants.hpp"
#include "core/collision/arm.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <span>

namespace TPVCamera
{
    namespace
    {
        // The lens near plane: in third person the near clip plane is pushed out to k_lens_near_max, so a leaf, a
        // branch or the corner of a prop right at the lens is cut away pixel by pixel instead of filling the view
        // (fading a whole tree for one leaf would be worse). It is held k_lens_near_margin short of the ground or a
        // building, so the camera never sees through a wall, and k_lens_near_character_margin short of the character's
        // head. A nearer clearance is taken at once, a farther one followed at k_lens_near_rate (1/s).
        constexpr float k_lens_near_max = 0.4f;
        constexpr float k_lens_near_character_margin = 0.35f;
        constexpr float k_lens_near_rate = 8.0f;

        // A measured clearance is reused while the camera holds still: moved less than k_lens_reuse_move (meters) and
        // turned less than k_lens_reuse_turn (radians, about 0.05 degree) from where it was measured, with the same FOV
        // and rule set, for at most k_lens_reuse_ms of real time. The probes hit only the static world and terrain
        // (RWI_OBJTYPES_CAMERA), which a held camera sees unchanged, so such a frame casts none of the nine rays; the
        // age bounds what a scan refresh or a rules edit can leave stale. The error this allows is about the move
        // itself (a same-frame measurement of a route's reused frames differed by 6 mm at most), well inside
        // k_lens_near_margin and far finer than the nine rays sample the near rectangle. An idle character sways the
        // camera by a few millimetres between frames, so a tighter bound would reuse nothing.
        constexpr float k_lens_reuse_move = 0.005f;
        constexpr float k_lens_reuse_turn = 0.00087f;
        constexpr std::uint64_t k_lens_reuse_ms = 100;

        // The broad phase before the nine rays (lens_may_meet()). A scanned stopping node counts while its box comes
        // within k_lens_broad_pad of the rays' box, which covers a collider standing a little out of its drawn box.
        // The ground counts while a ray straight down from the top of that box meets it within the box's height plus
        // k_lens_ground_slope times its half-diagonal across: ground rising toward an edge of the box at up to that
        // slope (about 63 degrees) is still found; steeper ground is rock or a cliff, which are scanned nodes.
        constexpr float k_lens_broad_pad = 0.25f;
        constexpr float k_lens_ground_slope = 2.0f;

        /**
         * @brief Whether @p sample still answers for the camera at @p position looking along @p forward / @p up.
         * @details The turn is measured as the distance between the unit axes, the chord of the angle, which stays
         *          exact for a fraction of a degree where a dot product near 1 loses it to float rounding.
         */
        [[nodiscard]] bool sample_holds(const LensSample &sample, const Vector3 &position, const Vector3 &forward,
                                        const Vector3 &up, float tan_half_width, float tan_half_height,
                                        std::uint64_t rules_generation, std::uint64_t now_ms) noexcept
        {
            constexpr float k_move2 = k_lens_reuse_move * k_lens_reuse_move;
            constexpr float k_turn2 = k_lens_reuse_turn * k_lens_reuse_turn;
            return sample.valid && now_ms - sample.stamp_ms < k_lens_reuse_ms &&
                   sample.rules_generation == rules_generation && sample.tan_half_width == tan_half_width &&
                   sample.tan_half_height == tan_half_height &&
                   (position - sample.position).magnitude_squared() < k_move2 &&
                   (forward - sample.forward).magnitude_squared() < k_turn2 &&
                   (up - sample.up).magnitude_squared() < k_turn2;
        }

        /**
         * @brief The nine rays from @p eye across the near end of the view (its centre, edge midpoints and corners),
         *        each @p reach meters of view depth long.
         * @param tan_half_width Tangent of half the horizontal FOV.
         * @param tan_half_height Tangent of half the vertical FOV.
         */
        void lens_rays(const Vector3 &eye, const Vector3 &right, const Vector3 &forward, const Vector3 &up,
                       float tan_half_width, float tan_half_height, float reach, Sightline (&rays)[9])
        {
            int n = 0;
            for (int sx = -1; sx <= 1; ++sx)
            {
                for (int sz = -1; sz <= 1; ++sz)
                {
                    const Vector3 dir = forward + right * (static_cast<float>(sx) * tan_half_width) +
                                        up * (static_cast<float>(sz) * tan_half_height);
                    rays[n++] = {eye, eye + dir * reach};
                }
            }
        }

        /**
         * @brief Whether anything the lens stops on can lie along @p rays: a scanned stopping node near the box around
         *        them, or the ground under it (see k_lens_broad_pad). When neither can, the rays would meet nothing
         *        that counts, so they are not cast.
         */
        [[nodiscard]] bool lens_may_meet(const Sightline (&rays)[9], WorldProbe &probe)
        {
            float box[6] = {rays[0].from.x, rays[0].from.y, rays[0].from.z,
                            rays[0].from.x, rays[0].from.y, rays[0].from.z};
            for (const Sightline &ray : rays)
            {
                for (const Vector3 &p : {ray.from, ray.to})
                {
                    box[0] = std::min(box[0], p.x);
                    box[1] = std::min(box[1], p.y);
                    box[2] = std::min(box[2], p.z);
                    box[3] = std::max(box[3], p.x);
                    box[4] = std::max(box[4], p.y);
                    box[5] = std::max(box[5], p.z);
                }
            }
            if (probe.stopping_node_near(box, k_lens_broad_pad))
            {
                return true;
            }
            const float half_across = 0.5f * std::hypot(box[3] - box[0], box[4] - box[1]);
            const float depth = (box[5] - box[2]) + k_lens_ground_slope * half_across;
            const Vector3 top{0.5f * (box[0] + box[3]), 0.5f * (box[1] + box[4]), box[5]};
            return probe
                .ray(QueryLedger::Tag::Lens, top, Vector3{0.0f, 0.0f, -depth}, Constants::RWI_OBJTYPES_TERRAIN,
                     Constants::RWI_FLAGS_STOP_AT_SOLID, nullptr, 0)
                .has_value();
        }

        /**
         * @brief The view depth of the nearest surface within @p reach meters of the camera that the near clip plane
         *        must not cut: the ground, a structure (its physics or its drawn mesh), or anything that matches no
         *        scanned node.
         * @details Tested along lens_rays(). A prop, a frame or a plant met on a ray is walked past: cutting those at
         *          the lens is the point. So is a structure's collision where it draws nothing (classify_probe_hit()).
         * @return The depth, or @p reach when nothing is that close.
         */
        [[nodiscard]] float lens_clearance(const Sightline (&rays)[9], float reach, WorldProbe &probe)
        {
            float nearest = reach;
            if (!lens_may_meet(rays, probe))
            {
                return nearest;
            }
            for (const Sightline &ray : rays)
            {
                // A hit's view depth is its distance along the ray over the ray's length per meter of view depth.
                const Vector3 span = ray.to - ray.from;
                const float length = span.magnitude();
                const float dir_length = length / reach;
                const Vector3 unit = span / length;
                float walked = 0.0f;
                for (int step = 0; step < 4 && walked < length; ++step)
                {
                    const std::optional<RayHit> h =
                        probe.ray(QueryLedger::Tag::Lens, ray.from + unit * walked, unit * (length - walked),
                                  Constants::RWI_OBJTYPES_CAMERA, Constants::RWI_FLAGS_STOP_AT_SOLID, nullptr, 0);
                    if (!h.has_value())
                    {
                        break;
                    }
                    const float along = walked + h->m_distance;
                    void *node = nullptr;
                    Rules::Behaviour cls = Rules::Behaviour::Collide;
                    if (classify_probe_hit(probe, QueryLedger::Tag::Lens, *h, along,
                                           std::span<const Vector3>(&ray.from, 1), unit, &node,
                                           &cls) == ProbeVerdict::Stop)
                    {
                        nearest = std::min(nearest, along / dir_length);
                        break;
                    }
                    walked = along + 0.02f; // a prop, a frame, a plant, or a structure's collision that draws nothing
                }
            }
            const float drawn =
                probe.structure_on_arm(QueryLedger::Tag::Lens, rays, 9, 1, nearest / reach, nullptr, false);
            if (drawn <= 1.0f)
            {
                nearest = std::min(nearest, drawn * reach);
            }
            return nearest;
        }
    } // namespace

    std::optional<float> plan_lens_near(LensState &state, const LensInput &in, WorldProbe &probe)
    {
        const float game_near = in.game_near;
        if (!(in.view_blend > 0.0f && game_near > 1e-4f && game_near < k_lens_near_max) || !in.use_lens_cut)
        {
            state.near = 0.0f;
            state.sample.valid = false;
            return std::nullopt;
        }
        if (!in.repeat_build)
        {
            const float tan_half_width = std::fabs(in.corner_x / game_near);
            const float tan_half_height = std::fabs(in.corner_z / game_near);
            const std::uint64_t now_ms = probe.now_ms();
            float solid = state.sample.solid;
            if (!sample_holds(state.sample, in.position, in.forward, in.up, tan_half_width, tan_half_height,
                              in.rules_generation, now_ms))
            {
                const ProbeScope scope(probe, "camera.lens_clearance");
                // The surfaces the plane must not cut, looked for only as far as the plane may go.
                Sightline rays[9];
                lens_rays(in.position, in.right, in.forward, in.up, tan_half_width, tan_half_height, k_lens_near_max,
                          rays);
                solid = lens_clearance(rays, k_lens_near_max, probe);
                state.sample = LensSample{
                    .position = in.position,
                    .forward = in.forward,
                    .up = in.up,
                    .tan_half_width = tan_half_width,
                    .tan_half_height = tan_half_height,
                    .rules_generation = in.rules_generation,
                    .stamp_ms = now_ms,
                    .solid = solid,
                    .valid = true,
                };
            }
            const float target =
                std::max(game_near, std::min(solid - k_lens_near_margin, in.arm - k_lens_near_character_margin));
            state.near =
                (target < state.near || state.near < game_near)
                    ? target
                    : state.near + (target - state.near) * (1.0f - std::exp(-k_lens_near_rate * in.delta_time));
        }
        return game_near + (std::max(state.near, game_near) - game_near) * in.view_blend;
    }
} // namespace TPVCamera
