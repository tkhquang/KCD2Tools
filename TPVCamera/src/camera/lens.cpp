/**
 * @file camera/lens.cpp
 * @brief The third-person lens near plane (see lens.hpp).
 */

#include "camera/lens.hpp"
#include "camera/collision.hpp"
#include "constants.hpp"
#include "physics_raycast.hpp"
#include "query_ledger.hpp"
#include "render_occlusion.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
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

        // The near distance the last measured game-view frame eased to; 0 while the plane is the game's own.
        float s_lens_near = 0.0f;

        /** @brief The last measured clearance and the pose, view and rule set it was measured for. */
        struct LensSample
        {
            Vector3 position{};
            Vector3 forward{};
            Vector3 up{};
            float tan_half_width = 0.0f;
            float tan_half_height = 0.0f;
            std::uint64_t rules_generation = 0;
            std::uint64_t stamp_ms = 0;
            float solid = 0.0f;
            bool valid = false;
        };
        LensSample s_lens_sample;

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
         * @brief The view depth of the nearest surface within @p reach meters of the camera that the near clip plane
         *        must not cut: the ground, a structure (its physics or its drawn mesh), or anything that matches no
         *        scanned node.
         * @details Tested along lens_rays(). A prop, a frame or a plant met on a ray is walked past: cutting those at
         *          the lens is the point. So is a structure's collision where it draws nothing (classify_probe_hit()).
         *          Main thread only (uses the occluder scan).
         * @return The depth, or @p reach when nothing is that close.
         */
        [[nodiscard]] float lens_clearance(const Sightline (&rays)[9], float reach)
        {
            float nearest = reach;
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
                    const std::optional<RayHit> h = ray_world_intersection(
                        QueryLedger::Tag::Lens, ray.from + unit * walked, unit * (length - walked),
                        Constants::RWI_OBJTYPES_CAMERA, Constants::RWI_FLAGS_STOP_AT_SOLID, nullptr, 0);
                    if (!h.has_value())
                    {
                        break;
                    }
                    const float along = walked + h->m_distance;
                    void *node = nullptr;
                    Rules::Behaviour cls = Rules::Behaviour::Collide;
                    if (classify_probe_hit(QueryLedger::Tag::Lens, *h, along, std::span<const Vector3>(&ray.from, 1),
                                           unit, &node, &cls) == ProbeVerdict::Stop)
                    {
                        nearest = std::min(nearest, along / dir_length);
                        break;
                    }
                    walked = along + 0.02f; // a prop, a frame, a plant, or a structure's collision that draws nothing
                }
            }
            const float drawn = structure_on_arm(QueryLedger::Tag::Lens, rays, 9, 1, nearest / reach, nullptr);
            if (drawn <= 1.0f)
            {
                nearest = std::min(nearest, drawn * reach);
            }
            return nearest;
        }
    } // namespace

    void apply_lens_near_plane(std::uintptr_t camera, const GameStructures::Matrix34f &view, float arm,
                               float view_blend, float delta_time, bool repeat_build)
    {
        auto *near_corner = reinterpret_cast<float *>(camera + Constants::CCAMERA_NEAR_CORNER_OFFSET);
        const float game_near = near_corner[1];
        if (!(view_blend > 0.0f && game_near > 1e-4f && game_near < k_lens_near_max))
        {
            s_lens_near = 0.0f;
            s_lens_sample.valid = false;
            return;
        }
        if (!repeat_build)
        {
            const Vector3 position{view.m[0][3], view.m[1][3], view.m[2][3]};
            const Vector3 right{view.m[0][0], view.m[1][0], view.m[2][0]};
            const Vector3 forward{view.m[0][1], view.m[1][1], view.m[2][1]};
            const Vector3 up{view.m[0][2], view.m[1][2], view.m[2][2]};
            const float tan_half_width = std::fabs(near_corner[0] / game_near);
            const float tan_half_height = std::fabs(near_corner[2] / game_near);
            const std::uint64_t rules_generation = occluder_rules_generation();
            const std::uint64_t now_ms = GetTickCount64();
            float solid = s_lens_sample.solid;
            if (!sample_holds(s_lens_sample, position, forward, up, tan_half_width, tan_half_height, rules_generation,
                              now_ms))
            {
                DMK_PROFILE_SCOPE("camera.lens_clearance");
                // The surfaces the plane must not cut, looked for only as far as the plane may go.
                Sightline rays[9];
                lens_rays(position, right, forward, up, tan_half_width, tan_half_height, k_lens_near_max, rays);
                solid = lens_clearance(rays, k_lens_near_max);
                s_lens_sample = LensSample{
                    .position = position,
                    .forward = forward,
                    .up = up,
                    .tan_half_width = tan_half_width,
                    .tan_half_height = tan_half_height,
                    .rules_generation = rules_generation,
                    .stamp_ms = now_ms,
                    .solid = solid,
                    .valid = true,
                };
            }
            const float target =
                std::max(game_near, std::min(solid - k_lens_near_margin, arm - k_lens_near_character_margin));
            s_lens_near =
                (target < s_lens_near || s_lens_near < game_near)
                    ? target
                    : s_lens_near + (target - s_lens_near) * (1.0f - std::exp(-k_lens_near_rate * delta_time));
        }
        const float near_plane = game_near + (std::max(s_lens_near, game_near) - game_near) * view_blend;
        const float scale = near_plane / game_near;
        near_corner[0] *= scale;
        near_corner[1] *= scale;
        near_corner[2] *= scale;
    }
} // namespace TPVCamera
