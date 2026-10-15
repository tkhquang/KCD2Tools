/**
 * @file camera/ease.hpp
 * @brief The camera collision ease: how the arm distance follows the distance the collision allows.
 *
 * @details Engine-free (no Windows or DetourModKit header), so the local tests compile it as is.
 */
#ifndef TPVCAMERA_CAMERA_EASE_HPP
#define TPVCAMERA_CAMERA_EASE_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace TPVCamera
{
    // A collision target within this of the desired distance (meters) holds nothing in. The ease and the throttle
    // cache both compare against it, so a reused result takes the same branch as the frame that computed it.
    constexpr float k_collision_clear_slack = 1e-4f;
    // How fast a pull-in follows its target (1/s): near-instant but still smooth.
    constexpr float k_pull_in_speed = 25.0f;

    /** @brief Which way one collision ease step moved the camera along its arm. */
    enum class EaseMotion : std::uint8_t
    {
        Settled,
        PullIn,
        Release,
    };

    /** @brief A state the collision ease runs on: CameraState, or a test's own struct with the same four fields. */
    template <typename T>
    concept CollisionEaseState = requires(T &state) {
        state.collision_valid = true;
        state.collision_distance = 0.0f;
        state.collision_desired = 0.0f;
        state.collision_speed = 0.0f;
    };

    /**
     * @brief Eases the camera collision distance toward @p target: a fast pull-in, so a wall is never clipped, and a
     *        critically damped return-out.
     * @details Pulling in follows at @p pull_in_speed (1/s) and never trails the target by more than @p pull_slack, so
     *          the lens never comes nearer to the surface that stopped it than k_lens_near_margin. Easing back out is
     *          critically damped at twice @p return_speed and starts from rest, so a release begins gently and
     *          settles on the target without a jolt. A @p return_speed of 0 snaps out. The first valid frame snaps to
     *          the target. The per-frame ease and the static-world throttle bypass share this helper, so the two
     *          cannot drift. While nothing holds the camera in (@p target reaches @p desired), the ease runs on the
     *          shortfall an earlier obstruction left, measured from @p desired, so a zoom or a preset blend moves the
     *          camera with the desired distance on the same frame. The ease does not run on the distance itself
     *          because that would trail the zoom along the pivot-to-camera ray, which carries the shoulder offset, and
     *          move the camera off its view axis and back. A @p delta_time of 0 (the frame's second build) advances no
     *          ease, so only the pull-in limit and a change of @p desired can move the camera.
     * @param target Allowed distance this frame.
     * @param desired Uncollided distance this frame.
     * @param pull_slack Most the pull-in can trail @p target by (meters).
     * @param pull_in_speed Rate the pull-in follows @p target at (1/s).
     * @return Whether the step pulled the camera in, released it outward, or left it where the shortfall had it.
     */
    template <CollisionEaseState State>
    [[nodiscard]] EaseMotion ease_collision_toward(State &cam, float target, float desired, float delta_time,
                                                   float return_speed, float pull_slack,
                                                   float pull_in_speed = k_pull_in_speed) noexcept
    {
        if (!cam.collision_valid)
        {
            cam.collision_distance = target;
            cam.collision_desired = desired;
            cam.collision_speed = 0.0f;
            cam.collision_valid = true;
            return EaseMotion::Settled;
        }
        const bool clear = target >= desired - k_collision_clear_slack;
        const float sat = cam.collision_distance;
        // With nothing holding the camera in, the last frame's shortfall carries onto this frame's desired distance,
        // so the ease does not see a zoom as a move of its target.
        const float from = clear ? desired - (cam.collision_desired - sat) : sat;
        float next = target;
        float speed_next = 0.0f;
        if (from > target)
        {
            const float eased = from + (target - from) * (1.0f - std::exp(-pull_in_speed * delta_time));
            next = std::min(eased, target + pull_slack);
        }
        else if (return_speed > 0.0f)
        {
            // Critically damped toward the target: behind is how far short of it the camera is (<= 0), the speed its
            // rate outward. A target nearer than where the camera's speed would carry it (a farther surface met on
            // the way out) lowers the speed first, so the camera slows into it instead of stopping dead at it.
            const float rate = 2.0f * return_speed;
            const float behind = from - target;
            const float speed = std::min(cam.collision_speed, -rate * behind);
            const float c = speed + rate * behind;
            const float decay = std::exp(-rate * delta_time);
            const float behind_next = (behind + c * delta_time) * decay;
            if (behind_next <= 0.0f)
            {
                next = target + behind_next;
                speed_next = (speed - rate * c * delta_time) * decay;
            }
        }
        if (clear)
        {
            // The result stays between where the camera sat and the desired distance: a zoom carries the camera no
            // further in than it sat, and no further out than it sat or than the distance the collision checked.
            next = std::clamp(next, std::min(sat, desired), std::max(sat, desired));
        }
        cam.collision_distance = next;
        cam.collision_speed = speed_next;
        cam.collision_desired = desired;
        // Measured against where the ease started from, so a zoom that moves the desired distance with no shortfall
        // counts as neither a pull-in nor a release.
        constexpr float k_motion_epsilon = 1e-4f;
        return next < from - k_motion_epsilon   ? EaseMotion::PullIn
               : next > from + k_motion_epsilon ? EaseMotion::Release
                                                : EaseMotion::Settled;
    }
} // namespace TPVCamera

#endif // TPVCAMERA_CAMERA_EASE_HPP
