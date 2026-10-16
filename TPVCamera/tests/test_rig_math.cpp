/**
 * @file test_rig_math.cpp
 * @brief core/rig/rig_math.hpp: a turned rig is the plain rig of the turned look, the turn's pitch raises the camera
 *        and tilts the look down, the look is rebuilt without roll, the arm bends only when looking up, and the
 *        shoulder side passes through the look line.
 */

#include "core/rig/rig_math.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <numbers>

namespace
{
    using TPVCamera::aim_arm_shift;
    using TPVCamera::level_look_rotation;
    using TPVCamera::Quaternion;
    using TPVCamera::shoulder_side;
    using TPVCamera::turn_rig;
    using TPVCamera::Vector3;

    constexpr float k_deg = std::numbers::pi_v<float> / 180.0f;

    /** @brief The look of heading @p yaw (engine convention, forward = (-sin yaw, cos yaw)) and pitch @p pitch. */
    Vector3 look_of(float yaw, float pitch)
    {
        return Vector3{-std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), std::sin(pitch)};
    }

    /** @brief The rig's right for @p look: level, square to the look's heading. */
    Vector3 right_of(const Vector3 &look)
    {
        return look.cross(Vector3{0.0f, 0.0f, 1.0f}).normalized();
    }

    /** @brief The plain rig's offset from the pivot: @p distance back along @p look, @p lateral to its right. */
    Vector3 plain_offset(const Vector3 &look, float distance, float lateral)
    {
        return look * (-distance) + right_of(look) * lateral;
    }

    void expect_near(const Vector3 &a, const Vector3 &b, float tolerance)
    {
        EXPECT_NEAR(a.x, b.x, tolerance);
        EXPECT_NEAR(a.y, b.y, tolerance);
        EXPECT_NEAR(a.z, b.z, tolerance);
    }

    TEST(RigMath, TurnedRigIsThePlainRigOfTheTurnedLook)
    {
        constexpr std::array<float, 4> k_yaws{0.0f, 0.7f, -2.4f, 3.1f};
        constexpr std::array<float, 4> k_pitches{0.0f, 0.3f, -0.6f, 0.9f};
        constexpr std::array<float, 3> k_turns{0.4f, -1.3f, 2.9f};
        constexpr std::array<float, 3> k_raises{0.25f, -0.5f, 0.0f};
        for (const float yaw : k_yaws)
        {
            for (const float pitch : k_pitches)
            {
                for (std::size_t i = 0; i < k_turns.size(); ++i)
                {
                    Vector3 look = look_of(yaw, pitch);
                    Vector3 up = right_of(look).cross(look);
                    Vector3 offset = plain_offset(look, 3.5f, 0.4f);
                    turn_rig(offset, look, up, k_turns[i], k_raises[i]);
                    // Within one centimetre on a 3.5 m arm: the aim hand-off keeps the camera where it was across the
                    // switch to the plain rig.
                    expect_near(offset, plain_offset(look, 3.5f, 0.4f), 1e-4f);
                    EXPECT_NEAR(look.magnitude(), 1.0f, 1e-5f);
                    EXPECT_NEAR(look.x * up.x + look.y * up.y + look.z * up.z, 0.0f, 1e-5f);
                }
            }
        }
    }

    TEST(RigMath, PositivePitchRaisesTheCameraAndTiltsTheLookDown)
    {
        Vector3 look = look_of(0.0f, 0.0f);
        Vector3 up{0.0f, 0.0f, 1.0f};
        Vector3 offset = plain_offset(look, 2.0f, 0.0f);
        turn_rig(offset, look, up, 0.0f, 20.0f * k_deg);
        EXPECT_NEAR(offset.z, 2.0f * std::sin(20.0f * k_deg), 1e-5f);
        EXPECT_NEAR(std::asin(look.z), -20.0f * k_deg, 1e-5f);
    }

    TEST(RigMath, LevelLookRotationKeepsHeadingAndPitchWithoutRoll)
    {
        // A look quat with a stale roll: heading 40 deg, pitch 10 deg, roll 15 deg (RotZ * RotX * RotY).
        const float h = 40.0f * k_deg;
        const float p = 10.0f * k_deg;
        const float r = 15.0f * k_deg;
        const DirectX::XMVECTOR q = DirectX::XMQuaternionMultiply(
            DirectX::XMQuaternionMultiply(DirectX::XMQuaternionRotationAxis(DirectX::XMVectorSet(0, 1, 0, 0), r),
                                          DirectX::XMQuaternionRotationAxis(DirectX::XMVectorSet(1, 0, 0, 0), p)),
            DirectX::XMQuaternionRotationAxis(DirectX::XMVectorSet(0, 0, 1, 0), h));
        const Quaternion rolled{DirectX::XMVectorGetX(q), DirectX::XMVectorGetY(q), DirectX::XMVectorGetZ(q),
                                DirectX::XMVectorGetW(q)};
        const Quaternion level = level_look_rotation(rolled, p);
        const Vector3 forward = level.rotate(Vector3{0.0f, 1.0f, 0.0f});
        const Vector3 right = level.rotate(Vector3{1.0f, 0.0f, 0.0f});
        EXPECT_NEAR(std::atan2(-forward.x, forward.y), h, 1e-5f);
        EXPECT_NEAR(std::asin(forward.z), p, 1e-5f);
        EXPECT_NEAR(right.z, 0.0f, 1e-6f); // no roll
    }

    TEST(RigMath, ArmShiftIsZeroForThePlainRigAndBelowLevel)
    {
        const Vector3 up_look = look_of(0.5f, 30.0f * k_deg);
        const Vector3 up_up = right_of(up_look).cross(up_look);
        expect_near(aim_arm_shift(up_look, up_up, 3.5f, 1.0f, 0.0f), Vector3{}, 0.0f);
        const Vector3 down_look = look_of(0.5f, -30.0f * k_deg);
        const Vector3 down_up = right_of(down_look).cross(down_look);
        expect_near(aim_arm_shift(down_look, down_up, 3.5f, 0.0f, 0.4f), Vector3{}, 0.0f);
    }

    TEST(RigMath, ArmShiftMatchesTheLiveCheck)
    {
        // Live at 30 deg up, a 1.5 m arm: plain -0.749 m; 0 / 0.4 -> -0.200 m; 0.5 / 0.4 -> -0.588 m.
        const Vector3 look = look_of(0.0f, 30.0f * k_deg);
        const Vector3 up = right_of(look).cross(look);
        const Vector3 plain = look * (-1.5f);
        EXPECT_NEAR(plain.z, -0.75f, 1e-4f);
        EXPECT_NEAR((plain + aim_arm_shift(look, up, 1.5f, 0.0f, 0.4f)).z, -0.2f, 1e-4f);
        EXPECT_NEAR((plain + aim_arm_shift(look, up, 1.5f, 0.5f, 0.4f)).z, -1.5f * std::sin(15.0f * k_deg) - 0.2f,
                    1e-4f);
        // The level arm keeps the full horizontal distance.
        const Vector3 level = plain + aim_arm_shift(look, up, 1.5f, 0.0f, 0.0f);
        EXPECT_NEAR(std::hypot(level.x, level.y), 1.5f, 1e-4f);
    }

    TEST(RigMath, LensDropStopsGrowingAt43Degrees)
    {
        const Vector3 steep = look_of(0.0f, 70.0f * k_deg);
        const Vector3 steep_up = right_of(steep).cross(steep);
        const Vector3 shift = aim_arm_shift(steep, steep_up, 0.0f, 1.0f, 1.0f);
        EXPECT_NEAR(shift.z, -std::sin(TPVCamera::k_aim_rig_drop_pitch), 1e-5f);
    }

    TEST(RigMath, ShoulderSideSlidesThroughTheLookLine)
    {
        EXPECT_FLOAT_EQ(shoulder_side(0.0f), 1.0f);
        EXPECT_FLOAT_EQ(shoulder_side(0.5f), 0.0f);
        EXPECT_FLOAT_EQ(shoulder_side(1.0f), -1.0f);
        float last = 2.0f;
        for (int i = 0; i <= 100; ++i)
        {
            const float side = shoulder_side(static_cast<float>(i) / 100.0f);
            EXPECT_LE(side, last);
            last = side;
        }
    }
} // namespace
