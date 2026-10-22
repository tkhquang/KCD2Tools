/**
 * @file test_fade_sightlines.cpp
 * @brief The fade's sightlines (core/collision/arm.hpp fade_sightlines()): the spine lines, the side lines across the
 *        view, and the cases that leave the side lines out.
 */

#include "core/collision/arm.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>

namespace
{
    using TPVCamera::fade_sightlines;
    using TPVCamera::Sightline;
    using TPVCamera::Vector3;

    using Lines = std::array<Sightline, Constants::OCCLUDER_FADE_SIGHTLINES>;

    constexpr float k_tolerance = 1e-5f;

    const Vector3 k_head{0.0f, 0.0f, 1.7f};
    const Vector3 k_hip{0.0f, 0.0f, 0.9f};
    const Vector3 k_chest{0.0f, 0.0f, 1.3f};

    /** @brief Expects @p actual to equal @p expected on every axis. */
    void expect_near(const Vector3 &actual, const Vector3 &expected)
    {
        EXPECT_NEAR(actual.x, expected.x, k_tolerance);
        EXPECT_NEAR(actual.y, expected.y, k_tolerance);
        EXPECT_NEAR(actual.z, expected.z, k_tolerance);
    }
} // namespace

TEST(FadeSightlines, SpineLinesRunFromTheCameraToHeadChestAndHips)
{
    const Vector3 camera{0.0f, -3.5f, 2.0f};
    Lines lines{};
    ASSERT_EQ(fade_sightlines(camera, k_head, k_hip, 0.25f, lines), Constants::OCCLUDER_FADE_SIGHTLINES);
    for (const Sightline &line : lines)
    {
        expect_near(line.from, camera);
    }
    expect_near(lines[0].to, k_head);
    expect_near(lines[1].to, k_chest);
    expect_near(lines[2].to, k_hip);
}

TEST(FadeSightlines, SideLinesEndAcrossTheViewAtTheBodyWidth)
{
    // The camera looks along +y, down at the chest: the side points lie on the x axis through the chest.
    const Vector3 camera{0.0f, -3.5f, 2.0f};
    Lines lines{};
    ASSERT_EQ(fade_sightlines(camera, k_head, k_hip, 0.25f, lines), Constants::OCCLUDER_FADE_SIGHTLINES);
    EXPECT_NEAR(std::fabs(lines[3].to.x), 0.25f, k_tolerance);
    expect_near(lines[3].to, Vector3{lines[3].to.x, 0.0f, 1.3f});
    expect_near(lines[4].to, Vector3{-lines[3].to.x, 0.0f, 1.3f});
}

TEST(FadeSightlines, SideOffsetStaysLevelAndPerpendicularToTheView)
{
    const Vector3 camera{2.0f, -1.0f, 3.5f};
    Lines lines{};
    ASSERT_EQ(fade_sightlines(camera, k_head, k_hip, 0.4f, lines), Constants::OCCLUDER_FADE_SIGHTLINES);
    const Vector3 offset = lines[3].to - k_chest;
    const Vector3 toward = k_chest - camera;
    EXPECT_NEAR(offset.z, 0.0f, k_tolerance);
    EXPECT_NEAR(offset.x * toward.x + offset.y * toward.y, 0.0f, k_tolerance);
    EXPECT_NEAR(offset.magnitude(), 0.4f, k_tolerance);
    expect_near(lines[4].to - k_chest, offset * -1.0f);
}

TEST(FadeSightlines, ZeroBodyWidthLeavesOnlyTheSpineLines)
{
    Lines lines{};
    EXPECT_EQ(fade_sightlines(Vector3{0.0f, -3.5f, 2.0f}, k_head, k_hip, 0.0f, lines), 3);
}

TEST(FadeSightlines, CameraStraightAboveTheChestLeavesOnlyTheSpineLines)
{
    Lines lines{};
    EXPECT_EQ(fade_sightlines(Vector3{0.0f, 0.0f, 4.0f}, k_head, k_hip, 0.25f, lines), 3);
}
