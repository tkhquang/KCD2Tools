/**
 * @file test_ease.cpp
 * @brief The collision ease (camera/ease.hpp): its step responses, edge cases and frame-rate independence.
 */

#include "camera/ease.hpp"
#include "frame_patterns.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <span>

namespace
{
    using TPVCamera::ease_collision_toward;
    using TPVCamera::EaseMotion;

    /** @brief The four fields the ease runs on, as CameraState holds them. */
    struct EaseState
    {
        bool collision_valid = false;
        float collision_distance = 0.0f;
        float collision_desired = 0.0f;
        float collision_speed = 0.0f;
    };

    // The shipped presets' CollisionReturnSpeed: the release runs critically damped at twice it.
    constexpr float k_return_speed = 6.0f;
    constexpr float k_release_rate = 2.0f * k_return_speed;
    // The default pull-in rate of ease_collision_toward (1/s).
    constexpr float k_pull_in_rate = TPVCamera::k_pull_in_speed;
    // A pull_slack too large to bind, so a pull-in follows the plain exponential.
    constexpr float k_unbound_slack = 100.0f;

    /** @brief A state the ease has already accepted, sitting at @p distance with @p desired uncollided. */
    [[nodiscard]] EaseState settled_at(float distance, float desired)
    {
        return EaseState{
            .collision_valid = true,
            .collision_distance = distance,
            .collision_desired = desired,
            .collision_speed = 0.0f,
        };
    }

    /**
     * @brief Runs the ease over @p frames from @p state toward @p target (with @p desired uncollided) and returns the
     *        time at which the share of the change still to go first drops to @p remaining, or -1 if it never does.
     */
    [[nodiscard]] float time_to_remaining(std::span<const float> frames, EaseState state, float target, float desired,
                                          float remaining, float pull_slack, float pull_in_speed = k_pull_in_rate)
    {
        const float start = state.collision_distance;
        float time = 0.0f;
        for (const float dt : frames)
        {
            (void)ease_collision_toward(state, target, desired, dt, k_return_speed, pull_slack, pull_in_speed);
            time += dt;
            if (std::fabs(target - state.collision_distance) <= remaining * std::fabs(target - start))
            {
                return time;
            }
        }
        return -1.0f;
    }

    /** @brief The time a critically damped release from rest at @p rate keeps @p remaining of its change: solves
     *         (1 + rt) e^-rt = remaining by bisection. */
    [[nodiscard]] float critically_damped_time(float rate, float remaining)
    {
        double lo = 0.0;
        double hi = 50.0;
        for (int i = 0; i < 200; ++i)
        {
            const double mid = 0.5 * (lo + hi);
            const double left = (1.0 + mid) * std::exp(-mid);
            (left > remaining ? lo : hi) = mid;
        }
        return static_cast<float>(0.5 * (lo + hi) / rate);
    }
} // namespace

TEST(CollisionEase, FirstFrameSnapsToTheTarget)
{
    EaseState state{};
    EXPECT_EQ(ease_collision_toward(state, 1.5f, 3.0f, 0.016f, k_return_speed, 0.15f), EaseMotion::Settled);
    EXPECT_TRUE(state.collision_valid);
    EXPECT_FLOAT_EQ(state.collision_distance, 1.5f);
    EXPECT_FLOAT_EQ(state.collision_desired, 3.0f);
}

TEST(CollisionEase, PullInHalfwayAt30MsAndNinetyPercentAt100Ms)
{
    // A pull-in follows 1 - e^-25t: half of it in ln 2 / 25 = 27.7 ms, 90% in ln 10 / 25 = 92.1 ms.
    const std::vector<float> frames(400, 0.001f);
    const float half = time_to_remaining(frames, settled_at(4.0f, 4.0f), 1.0f, 4.0f, 0.5f, k_unbound_slack);
    const float most = time_to_remaining(frames, settled_at(4.0f, 4.0f), 1.0f, 4.0f, 0.1f, k_unbound_slack);
    EXPECT_NEAR(half, std::log(2.0f) / k_pull_in_rate, 0.001f);
    EXPECT_NEAR(most, std::log(10.0f) / k_pull_in_rate, 0.001f);
    EXPECT_LE(half, 0.03f);
    EXPECT_LE(most, 0.10f);
}

TEST(CollisionEase, ReleaseIsCriticallyDampedFromRest)
{
    // With the 0.3 s hold in front of it, these are the 0.45 / 0.63 / 0.87 s the camera takes to get 50%, 90% and 99%
    // of the way back out at the shipped return speed.
    const std::vector<float> frames(2000, 0.001f);
    for (const float remaining : {0.5f, 0.1f, 0.01f})
    {
        const float time = time_to_remaining(frames, settled_at(1.0f, 4.0f), 4.0f, 4.0f, remaining, k_unbound_slack);
        EXPECT_NEAR(time, critically_damped_time(k_release_rate, remaining), 0.002f) << "remaining " << remaining;
    }
}

TEST(CollisionEase, ZeroDeltaMovesOnlyByThePullInLimit)
{
    EaseState held = settled_at(2.0f, 4.0f);
    EXPECT_EQ(ease_collision_toward(held, 1.9f, 4.0f, 0.0f, k_return_speed, 0.15f), EaseMotion::Settled);
    EXPECT_FLOAT_EQ(held.collision_distance, 2.0f);

    EaseState limited = settled_at(2.0f, 4.0f);
    EXPECT_EQ(ease_collision_toward(limited, 1.0f, 4.0f, 0.0f, k_return_speed, 0.15f), EaseMotion::PullIn);
    EXPECT_FLOAT_EQ(limited.collision_distance, 1.15f);
}

TEST(CollisionEase, PullInNeverTrailsTheTargetByMoreThanTheSlack)
{
    EaseState state = settled_at(4.0f, 4.0f);
    for (int frame = 0; frame < 60; ++frame)
    {
        (void)ease_collision_toward(state, 0.5f, 4.0f, 1.0f / 60.0f, k_return_speed, 0.15f);
        EXPECT_LE(state.collision_distance, 0.5f + 0.15f + 1e-6f) << "frame " << frame;
    }
}

TEST(CollisionEase, PullInFollowsTheGivenRate)
{
    // CollisionPullInSeconds 0.2 asks for 3 / 0.2 = 15 1/s: 95% of the way in ln 20 / 15 = 0.200 s.
    const std::vector<float> frames(400, 0.001f);
    const float rate = 3.0f / 0.2f;
    const float most = time_to_remaining(frames, settled_at(4.0f, 4.0f), 1.0f, 4.0f, 0.05f, k_unbound_slack, rate);
    EXPECT_NEAR(most, std::log(20.0f) / rate, 0.001f);
}

TEST(CollisionEase, SlowerPullInStillHonoursTheSlack)
{
    EaseState state = settled_at(4.0f, 4.0f);
    for (int frame = 0; frame < 60; ++frame)
    {
        (void)ease_collision_toward(state, 0.5f, 4.0f, 1.0f / 60.0f, k_return_speed, 0.15f, 3.0f / 0.2f);
        EXPECT_LE(state.collision_distance, 0.5f + 0.15f + 1e-6f) << "frame " << frame;
    }
}

TEST(CollisionEase, ZeroReturnSpeedSnapsOut)
{
    EaseState state = settled_at(1.0f, 4.0f);
    EXPECT_EQ(ease_collision_toward(state, 4.0f, 4.0f, 1.0f / 60.0f, 0.0f, 0.15f), EaseMotion::Release);
    EXPECT_FLOAT_EQ(state.collision_distance, 4.0f);
}

TEST(CollisionEase, ZoomWithoutShortfallIsNeitherPullInNorRelease)
{
    EaseState state = settled_at(3.0f, 3.0f);
    EXPECT_EQ(ease_collision_toward(state, 3.5f, 3.5f, 1.0f / 60.0f, k_return_speed, 0.15f), EaseMotion::Settled);
    EXPECT_FLOAT_EQ(state.collision_distance, 3.5f);
    EXPECT_EQ(ease_collision_toward(state, 2.5f, 2.5f, 1.0f / 60.0f, k_return_speed, 0.15f), EaseMotion::Settled);
    EXPECT_FLOAT_EQ(state.collision_distance, 2.5f);
}

TEST(CollisionEase, ShortfallRidesAZoom)
{
    // Held 1 m short of a 3 m arm, a zoom to 3.5 m moves the camera with it on the same frame: the shortfall eases,
    // not the distance.
    EaseState state = settled_at(2.0f, 3.0f);
    (void)ease_collision_toward(state, 3.5f, 3.5f, 0.0f, k_return_speed, 0.15f);
    EXPECT_FLOAT_EQ(state.collision_distance, 2.5f);
}

TEST(CollisionEaseRateMatrix, SettlesWithinOneFrameAtEveryRate)
{
    for (const auto &pattern : TPVCamera::Tests::rate_matrix())
    {
        const float tolerance = pattern.longest();
        for (const float remaining : {0.5f, 0.1f})
        {
            const float pull =
                time_to_remaining(pattern.frames, settled_at(4.0f, 4.0f), 1.0f, 4.0f, remaining, k_unbound_slack);
            const float pull_exact = std::log(1.0f / remaining) / k_pull_in_rate;
            EXPECT_GE(pull, pull_exact - 1e-4f) << pattern.name << " pull-in " << remaining;
            EXPECT_LE(pull, pull_exact + tolerance) << pattern.name << " pull-in " << remaining;
        }
        for (const float remaining : {0.5f, 0.1f, 0.01f})
        {
            const float release =
                time_to_remaining(pattern.frames, settled_at(1.0f, 4.0f), 4.0f, 4.0f, remaining, k_unbound_slack);
            const float release_exact = critically_damped_time(k_release_rate, remaining);
            EXPECT_GE(release, release_exact - 1e-4f) << pattern.name << " release " << remaining;
            EXPECT_LE(release, release_exact + tolerance) << pattern.name << " release " << remaining;
        }
    }
}
