/**
 * @file test_rig_math.cpp
 * @brief core/rig/rig_math.hpp: a turned rig is the plain rig of the turned look, the turn's pitch raises the camera
 *        and tilts the look down, the look is rebuilt without roll, the arm bends only when looking up, the shoulder
 *        side passes through the look line, and the lock framing swings to the shoulder side, aims beside the
 *        opponent within the lock-on's drop angle and blends about the pivot.
 */

#include "core/rig/rig_math.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <numbers>

namespace
{
    using TPVCamera::aim_arm_shift;
    using TPVCamera::blend_pivot_pose;
    using TPVCamera::hold_look_snap;
    using TPVCamera::is_look_clamp;
    using TPVCamera::is_look_snap;
    using TPVCamera::level_rotation;
    using TPVCamera::lock_frame_pose;
    using TPVCamera::look_heading;
    using TPVCamera::look_turn_speed;
    using TPVCamera::PivotPose;
    using TPVCamera::Quaternion;
    using TPVCamera::shoulder_side;
    using TPVCamera::soft_follow;
    using TPVCamera::Spring;
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

    TEST(RigMath, LevelRotationOfTheLookHeadingDropsTheRoll)
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
        const Quaternion level = level_rotation(look_heading(rolled), p);
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

    /** @brief The opponent's focus seen from the pivot: @p reach meters out at heading @p bearing, @p rise up. */
    Vector3 focus_at(float bearing, float reach, float rise)
    {
        return Vector3{-std::sin(bearing) * reach, std::cos(bearing) * reach, rise};
    }

    /** @brief How far @p point lies to the right of the vertical plane through the camera ray of @p pose, meters. */
    float right_of_ray(const PivotPose &pose, const Vector3 &point)
    {
        const Vector3 right = right_of(look_of(pose.yaw, pose.pitch));
        const Vector3 to_point = point - pose.offset;
        return right.x * to_point.x + right.y * to_point.y;
    }

    TEST(RigMath, LockFrameSwingsTheArmPastTheLineToTheShoulderSide)
    {
        constexpr float k_angle = 15.0f * k_deg;
        constexpr float k_bearing = -1.2f;
        const Vector3 to_focus = focus_at(k_bearing, 2.5f, -0.3f);
        const Vector3 right_plain = plain_offset(look_of(0.7f, 0.0f), 3.0f, 0.4f);
        const PivotPose right = lock_frame_pose(right_plain, 0.7f, to_focus, 1.0f, k_angle, 0.5f);
        expect_near(right.offset, plain_offset(look_of(k_bearing + k_angle, 0.0f), 3.0f, 0.4f), 1e-4f);
        const Vector3 left_plain = plain_offset(look_of(0.7f, 0.0f), 3.0f, -0.4f);
        const PivotPose left = lock_frame_pose(left_plain, 0.7f, to_focus, -1.0f, k_angle, 0.5f);
        expect_near(left.offset, plain_offset(look_of(k_bearing - k_angle, 0.0f), 3.0f, -0.4f), 1e-4f);
        // Halfway through the shoulder swap the arm lies on the line to the opponent and aims straight at him.
        const PivotPose middle =
            lock_frame_pose(plain_offset(look_of(0.7f, 0.0f), 3.0f, 0.0f), 0.7f, to_focus, 0.0f, k_angle, 0.5f);
        expect_near(middle.offset, plain_offset(look_of(k_bearing, 0.0f), 3.0f, 0.0f), 1e-4f);
        EXPECT_NEAR(std::remainder(middle.yaw - k_bearing, 2.0f * std::numbers::pi_v<float>), 0.0f, 1e-5f);
    }

    TEST(RigMath, LockFrameAimsBesideTheFocusTowardTheShoulderSide)
    {
        constexpr float k_offset = 0.5f;
        const Vector3 to_focus = focus_at(0.4f, 2.0f, -0.3f);
        for (const float side : {1.0f, -1.0f})
        {
            const PivotPose pose = lock_frame_pose(plain_offset(look_of(0.0f, 0.0f), 3.0f, 0.4f * side), 0.0f, to_focus,
                                                   side, 15.0f * k_deg, k_offset);
            // The aim point sits k_offset beside him, square to the camera's line to him, toward the shoulder side,
            // so he sits on the other side of the ray: d * k_offset / sqrt(d^2 + k_offset^2) off it, d being his
            // level distance from the camera.
            const Vector3 to_target = to_focus - pose.offset;
            const float level = std::hypot(to_target.x, to_target.y);
            const float slant = std::hypot(level, k_offset);
            EXPECT_NEAR(right_of_ray(pose, to_focus), side * k_offset * level / slant, 1e-4f);
            // And it points down at his height, below the pivot.
            EXPECT_NEAR(pose.pitch, std::atan2(to_target.z, slant), 1e-5f);
            EXPECT_LT(pose.pitch, 0.0f);
        }
    }

    TEST(RigMath, LockFrameAimStaysInsideTheLockOnDropAngle)
    {
        constexpr float k_reach = 8.0f;
        const PivotPose pose = lock_frame_pose(plain_offset(look_of(0.0f, 0.0f), 3.0f, 0.4f), 0.0f,
                                               focus_at(0.0f, k_reach, 0.0f), 1.0f, 45.0f * k_deg, 1.5f);
        const float limit = TPVCamera::k_lock_area_share * std::atan2(TPVCamera::k_lock_area_width, k_reach);
        // The unclamped aim is 0.34 rad off the line to him, past the limit, so it is held at it.
        EXPECT_NEAR(std::fabs(std::remainder(pose.yaw, 2.0f * std::numbers::pi_v<float>)), limit, 1e-5f);
    }

    TEST(RigMath, BlendPivotPoseCirclesThePivotAndTakesTheShorterWay)
    {
        const PivotPose from{
            .offset = Vector3{0.0f, -3.0f, 0.5f},
            .yaw = 3.0f,
            .pitch = 0.1f,
        };
        const PivotPose to{
            .offset = Vector3{3.0f * std::cos(60.0f * k_deg), 3.0f * std::sin(60.0f * k_deg), 0.2f},
            .yaw = -3.0f,
            .pitch = -0.2f,
        };
        const PivotPose start = blend_pivot_pose(from, to, 0.0f);
        expect_near(start.offset, from.offset, 1e-5f);
        EXPECT_NEAR(start.yaw, from.yaw, 1e-6f);
        const PivotPose end = blend_pivot_pose(from, to, 1.0f);
        expect_near(end.offset, to.offset, 1e-5f);
        EXPECT_NEAR(std::remainder(end.yaw - to.yaw, 2.0f * std::numbers::pi_v<float>), 0.0f, 1e-5f);
        EXPECT_NEAR(end.pitch, to.pitch, 1e-6f);
        // Halfway, the camera stays 3 m out, between the two on the shorter way round (-90 deg to 60 deg).
        const PivotPose middle = blend_pivot_pose(from, to, 0.5f);
        EXPECT_NEAR(std::hypot(middle.offset.x, middle.offset.y), 3.0f, 1e-5f);
        EXPECT_NEAR(std::atan2(middle.offset.y, middle.offset.x), -15.0f * k_deg, 1e-5f);
        EXPECT_NEAR(middle.offset.z, 0.35f, 1e-6f);
        // The look turns through pi, not through 0.
        EXPECT_NEAR(std::cos(middle.yaw), -1.0f, 1e-4f);
    }

    TEST(RigMath, LookHeadingReadsBackTheLevelRotation)
    {
        for (const float heading : {-170.0f * k_deg, -30.0f * k_deg, 0.0f, 75.0f * k_deg, 179.0f * k_deg})
        {
            EXPECT_NEAR(look_heading(level_rotation(heading, 40.0f * k_deg)), heading, 1e-5f);
        }
    }

    TEST(RigMath, LookTurnSpeedSweepsTheArcAndIsZeroWithoutTime)
    {
        // A heading turn at 60 deg pitch sweeps half the arc it sweeps level.
        EXPECT_NEAR(look_turn_speed(0.5f, 0.0f, 60.0f * k_deg, 0.5f), 0.5f, 1e-5f);
        EXPECT_NEAR(look_turn_speed(0.3f, 0.4f, 0.0f, 1.0f), 0.5f, 1e-5f);
        EXPECT_EQ(look_turn_speed(1.0f, 1.0f, 0.0f, 0.0f), 0.0f);
    }

    TEST(RigMath, HoldLookSnapKeepsTheViewAndEasesOutFromRest)
    {
        constexpr float dt = 1.0f / 60.0f;
        constexpr float settle = 0.8f;
        const float alpha = TPVCamera::spring_alpha(settle, dt);
        Spring<float> heading{};
        Spring<float> pitch{};
        const float jump_heading = 74.0f * k_deg;
        const float jump_pitch = -36.0f * k_deg;
        // A 74 deg heading jump and a 36 deg pitch jump in one frame, held whole: the rendered look (look + hold) stays
        // put.
        hold_look_snap(heading, pitch, jump_heading, jump_pitch, true, 0.0f);
        EXPECT_NEAR(jump_heading + heading.applied, 0.0f, 1e-6f);
        EXPECT_NEAR(jump_pitch + pitch.applied, 0.0f, 1e-6f);
        // Then it eases back without overshoot, speeding up from rest, and settles within the settle time.
        std::array<float, 3> first_steps{};
        float last = heading.applied;
        const int frames = static_cast<int>(std::ceil(settle / dt));
        for (int frame = 0; frame < frames; ++frame)
        {
            hold_look_snap(heading, pitch, 0.0f, 0.0f, false, alpha);
            EXPECT_GE(heading.applied, last);
            EXPECT_LE(heading.applied, 0.0f);
            if (frame < static_cast<int>(first_steps.size()))
            {
                first_steps[static_cast<std::size_t>(frame)] = heading.applied - last;
            }
            last = heading.applied;
        }
        EXPECT_GT(first_steps[0], 0.0f);
        EXPECT_GT(first_steps[1], first_steps[0]);
        EXPECT_GT(first_steps[2], first_steps[1]);
        EXPECT_LT(first_steps[0], 0.06f * jump_heading);
        EXPECT_LT(std::fabs(heading.applied), 0.01f * jump_heading);
        EXPECT_LT(std::fabs(pitch.applied), 0.01f * std::fabs(jump_pitch));
    }

    TEST(RigMath, LookSnapIsAFastTurnOrASuddenStart)
    {
        constexpr float deg_per_s = k_deg;
        // The measured jumps: 74 deg and 36 deg in one 20 fps frame, and 10.5 deg in one 30 fps frame from rest.
        EXPECT_TRUE(is_look_snap(74.0f * 20.0f * deg_per_s, 60.0f * deg_per_s));
        EXPECT_TRUE(is_look_snap(36.0f * 20.0f * deg_per_s, 0.0f));
        EXPECT_TRUE(is_look_snap(10.5f * 30.0f * deg_per_s, 0.0f));
        // A turn the game builds up (the look following the body into a seat at 100-250 deg/s) passes once moving.
        EXPECT_FALSE(is_look_snap(250.0f * deg_per_s, 180.0f * deg_per_s));
        EXPECT_FALSE(is_look_snap(120.0f * deg_per_s, 100.0f * deg_per_s));
        // Slow drift never counts.
        EXPECT_FALSE(is_look_snap(60.0f * deg_per_s, 0.0f));
    }

    TEST(RigMath, SoftFollowKeepsSmallMovesOutAndTracksLargeOnesWithoutAStep)
    {
        constexpr float dead_zone = 0.15f;
        // Breathing and head bob (a few centimetres) barely move it, while a kneel's half metre comes through.
        EXPECT_LT(std::fabs(soft_follow(0.04f, dead_zone)), 0.003f);
        EXPECT_NEAR(soft_follow(dead_zone, dead_zone), 0.5f * dead_zone, 1e-6f);
        EXPECT_NEAR(soft_follow(-0.5f, dead_zone), -0.5f + dead_zone * dead_zone / 0.5f, 0.01f);
        EXPECT_EQ(soft_follow(0.0f, dead_zone), 0.0f);
        // Odd, and rising with no jump: its slope peaks at 9/8 (at sqrt(3) dead zones), so a millimetre step moves it
        // by at most 1.125 mm.
        float last = soft_follow(-1.0f, dead_zone);
        for (int mm = -999; mm <= 1000; ++mm)
        {
            const float value = soft_follow(static_cast<float>(mm) * 0.001f, dead_zone);
            EXPECT_GE(value, last);
            EXPECT_LE(value - last, 0.00113f);
            last = value;
        }
        EXPECT_NEAR(soft_follow(0.3f, dead_zone), -soft_follow(-0.3f, dead_zone), 1e-7f);
    }

    TEST(RigMath, HoldLookSnapTakesInATurnAsALowPass)
    {
        // The game spins the look 330 deg into a seat at 330 deg/s, and each frame's turn is held and eased.
        constexpr float dt = 1.0f / 60.0f;
        const float alpha = TPVCamera::spring_alpha(0.8f, dt);
        const float step = 330.0f * k_deg * dt;
        Spring<float> heading{};
        Spring<float> pitch{};
        float look = 0.0f;
        float rendered = 0.0f;
        for (int frame = 0; frame < 160; ++frame)
        {
            const float turn = frame < 60 ? step : 0.0f;
            look += turn;
            hold_look_snap(heading, pitch, turn, 0.0f, true, alpha);
            const float now = look + heading.applied;
            // The camera follows the same way, never faster than the look turned.
            EXPECT_GE(now - rendered, -1e-5f);
            EXPECT_LE(now - rendered, step + 1e-5f);
            rendered = now;
        }
        // 1.67 s on (0.8 s settle after the turn stopped), it is on the look: the short way round to 330 deg.
        EXPECT_NEAR(std::remainder(rendered - 330.0f * k_deg, 2.0f * std::numbers::pi_v<float>), 0.0f, 0.01f);
    }

    TEST(RigMath, LookClampIsALimitStoppingThePlayersOwnTurn)
    {
        // The player turns 17 deg right into a seat's yaw limit and the limit takes back 12 deg, or all of it.
        EXPECT_TRUE(is_look_clamp(-12.0f * k_deg, 0.0f, 17.0f * k_deg, 0.0f));
        EXPECT_TRUE(is_look_clamp(-17.0f * k_deg, 0.0f, 17.0f * k_deg, 0.0f));
        // A game turn larger than the player's, or with it, or with no player turn at all, is the game's.
        EXPECT_FALSE(is_look_clamp(-74.0f * k_deg, 0.0f, 17.0f * k_deg, 0.0f));
        EXPECT_FALSE(is_look_clamp(10.0f * k_deg, 0.0f, 17.0f * k_deg, 0.0f));
        EXPECT_FALSE(is_look_clamp(0.0f, -36.0f * k_deg, 0.0f, 0.0f));
    }

    TEST(RigMath, HoldLookSnapEasesBackTheShortWay)
    {
        Spring<float> heading{};
        Spring<float> pitch{};
        // Two jumps of 2 rad hold 4 rad, which is -2.28 rad the short way round.
        hold_look_snap(heading, pitch, 2.0f, 0.0f, true, 0.0f);
        hold_look_snap(heading, pitch, 2.0f, 0.0f, true, 0.0f);
        EXPECT_NEAR(heading.applied, -4.0f + 2.0f * std::numbers::pi_v<float>, 1e-5f);
        EXPECT_NEAR(heading.stage1, heading.applied, 1e-6f);
    }
} // namespace
