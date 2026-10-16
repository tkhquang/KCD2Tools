/**
 * @file test_view_blend.cpp
 * @brief core/view/view_blend.hpp: the angles round-trip, s = 1 passes the rig through bit for bit, a small s stays at
 *        the eye, opposite looks turn through the side, the latched yaw keeps its side when the rig crosses the
 *        half-turn, the arm grows with s, and a second build does not advance the latch.
 */

#include "core/view/view_blend.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <numbers>

namespace
{
    using TPVCamera::blend_view;
    using TPVCamera::Vector3;
    using TPVCamera::view_angles;
    using TPVCamera::view_axes;
    using TPVCamera::ViewAngles;
    using TPVCamera::ViewBlendInput;
    using TPVCamera::ViewBlendLatch;
    using TPVCamera::ViewBlendOutput;

    constexpr float k_deg = std::numbers::pi_v<float> / 180.0f;

    float dot(const Vector3 &a, const Vector3 &b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    bool same_bits(const Vector3 &a, const Vector3 &b)
    {
        return std::memcmp(&a, &b, sizeof(Vector3)) == 0;
    }

    void expect_near(const Vector3 &a, const Vector3 &b, float tolerance)
    {
        EXPECT_NEAR(a.x, b.x, tolerance);
        EXPECT_NEAR(a.y, b.y, tolerance);
        EXPECT_NEAR(a.z, b.z, tolerance);
    }

    /** @brief An eye at the origin looking along +Y, and a rig 3 m behind a pivot 0.2 m above it, looking along @p yaw.
     */
    ViewBlendInput switch_input(float rig_yaw, float s)
    {
        ViewBlendInput in;
        in.eye_position = Vector3{0.0f, 0.0f, 0.0f};
        view_axes(ViewAngles{0.0f, 0.0f, 0.0f}, in.eye_forward, in.eye_up);
        in.pivot = Vector3{0.0f, 0.0f, 0.2f};
        view_axes(ViewAngles{rig_yaw, -10.0f * k_deg, 0.0f}, in.look_forward, in.look_up);
        const Vector3 right = in.look_forward.cross(in.look_up);
        in.camera = in.pivot - in.look_forward * 3.0f + right * 0.4f;
        in.s = s;
        return in;
    }
} // namespace

TEST(ViewBlend, AnglesRoundTrip)
{
    for (const ViewAngles angles : {ViewAngles{0.0f, 0.0f, 0.0f}, ViewAngles{1.2f, -0.6f, 0.05f},
                                    ViewAngles{-2.9f, 1.3f, -0.2f}, ViewAngles{3.0f, 0.4f, 0.0f}})
    {
        Vector3 forward;
        Vector3 up;
        view_axes(angles, forward, up);
        EXPECT_NEAR(forward.magnitude(), 1.0f, 1e-6f);
        EXPECT_NEAR(up.magnitude(), 1.0f, 1e-6f);
        EXPECT_NEAR(dot(forward, up), 0.0f, 1e-6f);
        const ViewAngles back = view_angles(forward, up);
        EXPECT_NEAR(back.yaw, angles.yaw, 1e-5f);
        EXPECT_NEAR(back.pitch, angles.pitch, 1e-5f);
        EXPECT_NEAR(back.roll, angles.roll, 1e-5f);
    }
}

TEST(ViewBlend, FullBlendPassesTheRigThrough)
{
    ViewBlendLatch latch;
    latch.valid = true;
    const ViewBlendInput in = switch_input(0.7f, 1.0f);
    const ViewBlendOutput out = blend_view(latch, in);
    EXPECT_TRUE(same_bits(out.pivot, in.pivot));
    EXPECT_TRUE(same_bits(out.camera, in.camera));
    EXPECT_TRUE(same_bits(out.forward, in.look_forward));
    EXPECT_TRUE(same_bits(out.up, in.look_up));
    EXPECT_TRUE(out.rebuild);
    EXPECT_FALSE(latch.valid);
}

TEST(ViewBlend, SmallBlendStaysAtTheEye)
{
    ViewBlendLatch latch;
    const ViewBlendInput in = switch_input(2.0f, 1e-4f);
    const ViewBlendOutput out = blend_view(latch, in);
    expect_near(out.pivot, in.eye_position, 1e-4f);
    expect_near(out.camera, in.eye_position, 1e-3f);
    expect_near(out.forward, in.eye_forward, 1e-3f);
    expect_near(out.up, in.eye_up, 1e-3f);
}

TEST(ViewBlend, OppositeLooksTurnThroughTheSide)
{
    // The rig looks back at the eye (a half-turn): halfway through, the view looks to one side, square to both looks,
    // where a straight blend of the two forwards would pass through zero and flip.
    ViewBlendLatch latch;
    ViewBlendInput in = switch_input(std::numbers::pi_v<float>, 0.5f);
    in.look_forward = Vector3{0.0f, -1.0f, 0.0f};
    in.look_up = Vector3{0.0f, 0.0f, 1.0f};
    const ViewBlendOutput out = blend_view(latch, in);
    EXPECT_NEAR(out.forward.magnitude(), 1.0f, 1e-5f);
    EXPECT_NEAR(dot(out.forward, in.eye_forward), 0.0f, 1e-5f);
    EXPECT_NEAR(out.up.z, 1.0f, 1e-5f);
}

TEST(ViewBlend, LatchedYawKeepsItsSide)
{
    // The switch starts with the rig 170 degrees to the left; the rig keeps turning left past the half-turn (to 190,
    // which wraps to -170). The latched delta goes on to 190 degrees instead of swinging to -170.
    ViewBlendLatch latch;
    (void)blend_view(latch, switch_input(170.0f * k_deg, 0.2f));
    EXPECT_NEAR(latch.yaw_accumulated, 170.0f * k_deg, 1e-4f);
    (void)blend_view(latch, switch_input(180.0f * k_deg, 0.3f));
    const ViewBlendOutput out = blend_view(latch, switch_input(190.0f * k_deg, 0.5f));
    EXPECT_NEAR(latch.yaw_accumulated, 190.0f * k_deg, 1e-4f);
    // Halfway along a 190-degree turn to the left: 95 degrees left of the eye.
    const ViewAngles angles = view_angles(out.forward, out.up);
    EXPECT_NEAR(angles.yaw, 95.0f * k_deg, 1e-4f);
}

TEST(ViewBlend, SecondBuildDoesNotAdvanceTheLatch)
{
    ViewBlendLatch latch;
    (void)blend_view(latch, switch_input(30.0f * k_deg, 0.2f));
    ViewBlendInput repeat = switch_input(60.0f * k_deg, 0.2f);
    repeat.repeat_build = true;
    (void)blend_view(latch, repeat);
    EXPECT_NEAR(latch.yaw_accumulated, 30.0f * k_deg, 1e-6f);
}

TEST(ViewBlend, ArmGrowsWithTheBlend)
{
    for (const float s : {0.1f, 0.5f, 0.9f})
    {
        ViewBlendLatch latch;
        const ViewBlendInput in = switch_input(0.4f, s);
        const ViewBlendOutput out = blend_view(latch, in);
        EXPECT_NEAR((out.camera - out.pivot).magnitude(), s * (in.camera - in.pivot).magnitude(), 1e-5f);
        expect_near(out.pivot, in.eye_position + (in.pivot - in.eye_position) * s, 1e-6f);
    }
}

TEST(ViewBlend, EyeBasisRigKeepsTheEyeOrientation)
{
    // A rig built on the eye's own basis (nothing reshaped it) keeps that orientation and is not rebuilt.
    ViewBlendLatch latch;
    ViewBlendInput in = switch_input(0.0f, 0.5f);
    in.look_forward = in.eye_forward;
    in.look_up = in.eye_up;
    in.camera = in.pivot - in.look_forward * 3.0f;
    in.rebuild_basis = false;
    const ViewBlendOutput out = blend_view(latch, in);
    EXPECT_FALSE(out.rebuild);
    EXPECT_TRUE(same_bits(out.forward, in.eye_forward));
    EXPECT_NEAR((out.camera - out.pivot).magnitude(), 1.5f, 1e-5f);
    EXPECT_FALSE(latch.valid);
}
