/**
 * @file test_blend.cpp
 * @brief The camera eases of camera/blend.hpp against the arithmetic each site ran before it: bit-equal over the rate
 *        matrix.
 *
 * @details Each reference repeats the replaced code as it was: the two-stage filter of the preset blend and the FOV
 *          ease, the view switch's linear ramp and smoothstep, and the first-person fallback's exponential ease with
 *          its landing.
 */

#include "camera/blend.hpp"
#include "frame_patterns.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

namespace
{
    using TPVCamera::Spring;

    /** @brief The preset blend's two stages as resolve_and_apply() ran them, at 1.6 times the speed. */
    struct TwoStage
    {
        float stage1 = 0.0f;
        float applied = 0.0f;

        void step(float target, float speed, float dt)
        {
            const float alpha = 1.0f - std::exp(-speed * 1.6f * dt);
            stage1 += (target - stage1) * alpha;
            applied += (stage1 - applied) * alpha;
        }
    };

    /** @brief The view switch's ramp as the frustum detour ran it. */
    [[nodiscard]] float reference_ramp(float blend, float target, float duration, float dt)
    {
        if (duration > 1e-4f)
        {
            const float step = dt / duration;
            return (blend < target) ? std::min(target, blend + step) : std::max(target, blend - step);
        }
        return target;
    }

    /** @brief The first-person fallback's ease as the too-close block ran it. */
    [[nodiscard]] float reference_fallback(float blend, float target, float dt)
    {
        const float k = 1.0f - std::exp(-12.0f * dt);
        blend += (target - blend) * k;
        if (std::fabs(target - blend) < 1e-3f)
        {
            blend = target;
        }
        return blend;
    }

    [[nodiscard]] std::uint32_t bits(float value)
    {
        return std::bit_cast<std::uint32_t>(value);
    }
} // namespace

TEST(Blend, SpringIsBitEqualToTheTwoStageFilter)
{
    for (const auto &pattern : TPVCamera::Tests::rate_matrix(2000))
    {
        for (const float speed : {0.5f, 3.0f, 8.0f, 25.0f})
        {
            TwoStage reference{};
            Spring<float> spring{};
            for (std::size_t i = 0; i < pattern.frames.size(); ++i)
            {
                // A target that steps every 300 frames, so the filter keeps moving.
                const float target = ((i / 300) % 2 == 0) ? 1.0f : -0.25f;
                const float dt = pattern.frames[i];
                reference.step(target, speed, dt);
                spring.step(target, TPVCamera::stage_alpha(speed * TPVCamera::k_spring_rate_scale, dt));
                ASSERT_EQ(bits(spring.stage1), bits(reference.stage1)) << pattern.name << " speed " << speed;
                ASSERT_EQ(bits(spring.applied), bits(reference.applied)) << pattern.name << " speed " << speed;
            }
        }
    }
}

TEST(Blend, SpringSnapPutsBothStagesOnTheTarget)
{
    Spring<float> spring{};
    spring.step(1.0f, 0.3f);
    spring.snap(-2.0f);
    EXPECT_EQ(spring.stage1, -2.0f);
    EXPECT_EQ(spring.applied, -2.0f);
}

TEST(Blend, RampIsBitEqualToTheViewSwitch)
{
    for (const auto &pattern : TPVCamera::Tests::rate_matrix(600))
    {
        for (const float duration : {0.0f, 1e-4f, 0.25f, 0.6f})
        {
            float reference = 0.0f;
            float weight = 0.0f;
            for (std::size_t i = 0; i < pattern.frames.size(); ++i)
            {
                const float target = ((i / 40) % 2 == 0) ? 1.0f : 0.0f;
                reference = reference_ramp(reference, target, duration, pattern.frames[i]);
                weight = TPVCamera::ramp_weight(weight, target, duration, pattern.frames[i]);
                ASSERT_EQ(bits(weight), bits(reference)) << pattern.name << " duration " << duration;
                const float shaped = reference * reference * (3.0f - 2.0f * reference);
                ASSERT_EQ(bits(TPVCamera::smoothstep(weight)), bits(shaped)) << pattern.name;
            }
        }
    }
}

TEST(Blend, EaseWeightIsBitEqualToTheFallbackEase)
{
    for (const auto &pattern : TPVCamera::Tests::rate_matrix(600))
    {
        float reference = 0.0f;
        float weight = 0.0f;
        for (std::size_t i = 0; i < pattern.frames.size(); ++i)
        {
            const float target = ((i / 50) % 2 == 0) ? 1.0f : 0.0f;
            reference = reference_fallback(reference, target, pattern.frames[i]);
            weight = TPVCamera::ease_weight(weight, target, 12.0f, pattern.frames[i], 1e-3f);
            ASSERT_EQ(bits(weight), bits(reference)) << pattern.name;
        }
    }
}
