/**
 * @file test_stick.cpp
 * @brief The right stick's response curve of core/input/stick.hpp: identity at 1, the push's direction kept, its length
 *        raised to the exponent up to a full push and left alone past it.
 */

#include "core/input/stick.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace
{
    using TPVCamera::shape_stick;
    using TPVCamera::StickDeflection;

    // Pushes over the whole range the engine reports, both axes alone and together, a square gate's corners included.
    constexpr std::array<StickDeflection, 12> k_pushes{{{0.0f, 0.0f},
                                                        {1.0f, 0.0f},
                                                        {0.0f, -1.0f},
                                                        {0.25f, 0.0f},
                                                        {-0.1f, 0.05f},
                                                        {0.3f, 0.4f},
                                                        {-0.6f, -0.8f},
                                                        {0.7071f, 0.7071f},
                                                        {1.0f, 1.0f},
                                                        {-1.0f, 0.9f},
                                                        {0.02f, -0.015f},
                                                        {0.95f, -0.2f}}};
    constexpr std::array<float, 4> k_exponents{1.5f, 2.0f, 2.5f, 3.0f};

    TEST(StickCurve, ExponentOneIsBitIdentical)
    {
        for (const StickDeflection &push : k_pushes)
        {
            const StickDeflection shaped = shape_stick(push, 1.0f);
            EXPECT_EQ(std::bit_cast<std::uint32_t>(shaped.x), std::bit_cast<std::uint32_t>(push.x));
            EXPECT_EQ(std::bit_cast<std::uint32_t>(shaped.y), std::bit_cast<std::uint32_t>(push.y));
        }
    }

    TEST(StickCurve, KeepsTheDirection)
    {
        for (const float exponent : k_exponents)
        {
            for (const StickDeflection &push : k_pushes)
            {
                const StickDeflection shaped = shape_stick(push, exponent);
                // Parallel and the same way round: the cross product vanishes and the dot product does not go negative.
                EXPECT_NEAR(shaped.x * push.y - shaped.y * push.x, 0.0f, 1e-6f);
                EXPECT_GE(shaped.x * push.x + shaped.y * push.y, 0.0f);
            }
        }
    }

    TEST(StickCurve, RaisesTheLengthUpToAFullPush)
    {
        for (const float exponent : k_exponents)
        {
            for (const StickDeflection &push : k_pushes)
            {
                const float length = std::hypot(push.x, push.y);
                const StickDeflection shaped = shape_stick(push, exponent);
                const float expected = length <= 1.0f ? std::pow(length, exponent) : length;
                EXPECT_NEAR(std::hypot(shaped.x, shaped.y), expected, 1e-5f) << "exponent " << exponent;
            }
        }
    }

    TEST(StickCurve, SlowerForASmallPushFullRateAtFull)
    {
        const StickDeflection small = shape_stick({0.2f, 0.0f}, 2.0f);
        EXPECT_FLOAT_EQ(small.x, 0.04f);
        const StickDeflection full = shape_stick({0.0f, 1.0f}, 3.0f);
        EXPECT_FLOAT_EQ(full.y, 1.0f);
    }
} // namespace
