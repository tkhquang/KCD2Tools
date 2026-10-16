/**
 * @file core/view/view_blend.cpp
 * @brief The first-person <-> third-person switch blend (see view_blend.hpp).
 */

#include "core/view/view_blend.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace TPVCamera
{
    namespace
    {
        constexpr float k_two_pi = 2.0f * std::numbers::pi_v<float>;

        /** @brief @p angle wrapped into [-pi, pi]. */
        [[nodiscard]] float wrap_pi(float angle) noexcept
        {
            return std::remainder(angle, k_two_pi);
        }

        [[nodiscard]] float dot(const Vector3 &a, const Vector3 &b) noexcept
        {
            return a.x * b.x + a.y * b.y + a.z * b.z;
        }
    } // namespace

    ViewAngles view_angles(const Vector3 &forward, const Vector3 &up) noexcept
    {
        // Yaw and pitch place the forward; the roll is the turn of the up about it, measured from the up of the same
        // forward without roll (level right x forward, the CryEngine right x forward = up).
        const Vector3 unit_forward = forward.normalized();
        ViewAngles angles;
        angles.yaw = std::atan2(-unit_forward.x, unit_forward.y);
        angles.pitch = std::asin(std::clamp(unit_forward.z, -1.0f, 1.0f));
        const Vector3 level_right{std::cos(angles.yaw), std::sin(angles.yaw), 0.0f};
        const Vector3 level_up = level_right.cross(unit_forward);
        angles.roll = std::atan2(-dot(up, level_right), dot(up, level_up));
        return angles;
    }

    void view_axes(const ViewAngles &angles, Vector3 &forward, Vector3 &up) noexcept
    {
        const float cos_yaw = std::cos(angles.yaw);
        const float sin_yaw = std::sin(angles.yaw);
        const float cos_pitch = std::cos(angles.pitch);
        forward = Vector3{-sin_yaw * cos_pitch, cos_yaw * cos_pitch, std::sin(angles.pitch)};
        const Vector3 level_right{cos_yaw, sin_yaw, 0.0f};
        const Vector3 level_up = level_right.cross(forward);
        up = level_up * std::cos(angles.roll) - level_right * std::sin(angles.roll);
    }

    ViewBlendOutput blend_view(ViewBlendLatch &latch, const ViewBlendInput &in) noexcept
    {
        if (!(in.s < 1.0f))
        {
            latch.valid = false;
            return ViewBlendOutput{
                .pivot = in.pivot,
                .camera = in.camera,
                .forward = in.look_forward,
                .up = in.look_up,
                .rebuild = in.rebuild_basis,
            };
        }
        const float s = std::max(in.s, 0.0f);
        ViewBlendOutput out;
        out.pivot = in.eye_position + (in.pivot - in.eye_position) * s;
        out.rebuild = in.rebuild_basis;
        if (in.rebuild_basis)
        {
            // Yaw, pitch and roll each turn from the eye's to the rig's. The yaw takes the delta integrated since the
            // switch began, so the side it turns through is the one it started on: a raw shortest-arc delta would
            // swap sides when the rig crosses the half-turn from the eye, and the view would flip.
            const ViewAngles eye = view_angles(in.eye_forward, in.eye_up);
            const ViewAngles rig = view_angles(in.look_forward, in.look_up);
            const float yaw_delta = wrap_pi(rig.yaw - eye.yaw);
            if (!latch.valid)
            {
                latch.yaw_accumulated = yaw_delta;
                latch.yaw_previous = yaw_delta;
                latch.valid = true;
            }
            else if (!in.repeat_build)
            {
                latch.yaw_accumulated += wrap_pi(yaw_delta - latch.yaw_previous);
                latch.yaw_previous = yaw_delta;
            }
            const ViewAngles blended{
                .yaw = eye.yaw + s * latch.yaw_accumulated,
                .pitch = eye.pitch + s * (rig.pitch - eye.pitch),
                .roll = eye.roll + s * wrap_pi(rig.roll - eye.roll),
            };
            view_axes(blended, out.forward, out.up);
        }
        else
        {
            // The rig was built on the eye's own basis (no stable basis, follow angle or orbit), which it keeps.
            out.forward = in.look_forward;
            out.up = in.look_up;
        }
        // The arm in the rig's frame (back along the look, the shoulder to its right, the height above it), turned with
        // the blended orientation and grown with s.
        const Vector3 rig_right = in.look_forward.cross(in.look_up).normalized();
        const Vector3 offset = in.camera - in.pivot;
        const Vector3 right = out.forward.cross(out.up).normalized();
        const Vector3 arm = right * dot(offset, rig_right) + out.forward * dot(offset, in.look_forward) +
                            out.up * dot(offset, in.look_up);
        out.camera = out.pivot + arm * s;
        return out;
    }
} // namespace TPVCamera
