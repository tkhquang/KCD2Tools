/**
 * @file core/view/view_blend.hpp
 * @brief The first-person <-> third-person switch: the camera the build collides, blended between the eye and the rig
 *        around the pivot, in yaw, pitch, roll and arm length rather than on the straight line between the two lenses.
 *
 * @details While ViewTransitionDuration eases the switch (0 < s < 1), the pivot slides from the eye to the rig's pivot,
 *          the orientation turns from the eye's to the rig's by yaw, pitch and roll, and the arm grows from nothing to
 *          the rig's, turned with that orientation. The camera collision then runs once on the blended camera, so a
 *          lens mid-switch never stands beyond a wall that was not probed, and an eye and a rig looking opposite ways
 *          turn through the side instead of flipping at the halfway point. The yaw delta is integrated frame by frame
 *          (latched), so a rig that keeps turning during the switch, or crosses the half-turn, does not change the side
 *          the view turns through. At s = 1 the rig passes through unchanged. Engine-free. Main thread only.
 */
#ifndef TPVCAMERA_CORE_VIEW_VIEW_BLEND_HPP
#define TPVCAMERA_CORE_VIEW_VIEW_BLEND_HPP

#include "core/math/vec.hpp"

namespace TPVCamera
{
    /** @brief An orientation as yaw, pitch and roll (radians; engine convention forward = (-sin yaw, cos yaw)). */
    struct ViewAngles
    {
        float yaw = 0.0f;
        float pitch = 0.0f;
        float roll = 0.0f;
    };

    /** @brief The yaw, pitch and roll of the view looking along @p forward with @p up. */
    [[nodiscard]] ViewAngles view_angles(const Vector3 &forward, const Vector3 &up) noexcept;

    /** @brief The forward and up of @p angles (the inverse of view_angles()). */
    void view_axes(const ViewAngles &angles, Vector3 &forward, Vector3 &up) noexcept;

    /** @brief The switch's yaw latch: the rig's yaw off the eye's, integrated since the switch began. */
    struct ViewBlendLatch
    {
        float yaw_accumulated = 0.0f;
        float yaw_previous = 0.0f;
        bool valid = false;
    };

    /** @brief One build's input to the switch blend. */
    struct ViewBlendInput
    {
        // The untouched first-person eye.
        Vector3 eye_position{};
        Vector3 eye_forward{};
        Vector3 eye_up{};
        // The rig before collision: the arm's pivot, the camera, and its look and up.
        Vector3 pivot{};
        Vector3 camera{};
        Vector3 look_forward{};
        Vector3 look_up{};
        float s = 1.0f;            // the smoothstepped view blend, 0 first person .. 1 third person
        bool rebuild_basis = true; // the rig shapes the orientation (RigFrame::rebuild_basis)
        bool repeat_build = false; // the frame's second build, which does not advance the latch
    };

    /** @brief The camera the build collides: its pivot, its position and its orientation. */
    struct ViewBlendOutput
    {
        Vector3 pivot{};
        Vector3 camera{};
        Vector3 forward{};
        Vector3 up{};
        // The rendered orientation is rebuilt from forward / up; otherwise the engine's eye basis stays.
        bool rebuild = false;
    };

    /** @brief The camera the build collides for blend @p in.s (see the file details). */
    [[nodiscard]] ViewBlendOutput blend_view(ViewBlendLatch &latch, const ViewBlendInput &in) noexcept;
} // namespace TPVCamera

#endif // TPVCAMERA_CORE_VIEW_VIEW_BLEND_HPP
