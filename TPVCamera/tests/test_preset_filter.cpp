/**
 * @file test_preset_filter.cpp
 * @brief The preset blend's two cascaded critically damped stages: their settle time and its frame-rate independence.
 *
 * @details TwoStage repeats the arithmetic Presets::resolve_and_apply() runs on every preset field (stage 1 eases
 *          toward the target, stage 2 toward the new stage 1, both by 1 - e^(-1.6 * PresetBlendSpeed * dt)).
 */

#include "frame_patterns.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <span>

namespace
{
    // resolve_and_apply()'s rate scale and the shipped PresetBlendSpeed.
    constexpr float k_rate_scale = 1.6f;
    constexpr float k_blend_speed = 8.0f;
    // Two cascaded stages reach 95% of a step at k*t = 4.7439, where 1 - (1 + kt)e^-kt = 0.95.
    constexpr float k_two_stage_t95 = 4.7439f;

    /** @brief One preset field through the two stages. */
    struct TwoStage
    {
        float stage1 = 0.0f;
        float applied = 0.0f;

        void step(float target, float speed, float dt)
        {
            const float alpha = 1.0f - std::exp(-speed * k_rate_scale * dt);
            stage1 = stage1 + (target - stage1) * alpha;
            applied = applied + (stage1 - applied) * alpha;
        }
    };

    /** @brief The time a unit step through the two stages first reaches @p reached, or -1 if it never does. */
    [[nodiscard]] float time_to_reach(std::span<const float> frames, float speed, float reached)
    {
        TwoStage filter{};
        float time = 0.0f;
        for (const float dt : frames)
        {
            filter.step(1.0f, speed, dt);
            time += dt;
            if (filter.applied >= reached)
            {
                return time;
            }
        }
        return -1.0f;
    }
} // namespace

TEST(PresetFilter, ReachesNinetyFivePercentAtTheTwoStageT95)
{
    const std::vector<float> frames(4000, 0.0005f);
    const float exact = k_two_stage_t95 / (k_rate_scale * k_blend_speed);
    EXPECT_NEAR(exact, 0.3706f, 1e-4f);
    EXPECT_NEAR(time_to_reach(frames, k_blend_speed, 0.95f), exact, 0.002f);
}

TEST(PresetFilter, NeverOvershoots)
{
    TwoStage filter{};
    for (int frame = 0; frame < 600; ++frame)
    {
        filter.step(1.0f, k_blend_speed, 1.0f / 30.0f);
        EXPECT_LE(filter.applied, 1.0f) << "frame " << frame;
        EXPECT_LE(filter.applied, filter.stage1 + 1e-6f) << "frame " << frame;
    }
}

TEST(PresetFilterRateMatrix, SettlesWithinOneFrameAtEveryRate)
{
    const float exact = k_two_stage_t95 / (k_rate_scale * k_blend_speed);
    for (const auto &pattern : TPVCamera::Tests::rate_matrix())
    {
        const float time = time_to_reach(pattern.frames, k_blend_speed, 0.95f);
        EXPECT_NEAR(time, exact, pattern.longest()) << pattern.name;
    }
}
