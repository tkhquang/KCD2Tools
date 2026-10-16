/**
 * @file core/rig/rig_math.hpp
 * @brief The over-the-shoulder rig's geometry: the roll-free look, the rigid turn of the rig about its pivot, the arm
 *        bent when looking up, and the shoulder side the swap slides through.
 *
 * @details The follow rig sits `distance` back from the pivot along the rendered look and the shoulder offset to its
 *          right, so it is a function of the rendered look alone: the follow angle, the free-look orbit and the aim
 *          hand-off turn the whole rig with turn_rig(), and a turned rig is the plain rig of the turned look.
 *          Engine-free (no Windows or DetourModKit header), so the local tests compile it as is.
 */
#ifndef TPVCAMERA_CORE_RIG_RIG_MATH_HPP
#define TPVCAMERA_CORE_RIG_RIG_MATH_HPP

#include "core/blend/blend.hpp"
#include "core/math/vec.hpp"

#include <algorithm>
#include <cmath>

namespace TPVCamera
{
    // Lens Drop Up stops growing at this look pitch (43 deg), as Ryse's aim camera clamps its pitch slide.
    constexpr float k_aim_rig_drop_pitch = 0.75f; // rad

    /**
     * @brief Rebuilds the look orientation from its heading and the scalar pitch, without the stale roll.
     * @details The raw look quat can carry a stale roll angle (see LOOK_CONTROLLER_QUAT_OFFSET).
     *          C_CameraFirstPerson::Update never renders that quat directly. It takes only the heading of the look
     *          forward vector and combines it with the scalar pitch that C_Player vtable slot 59 returns
     *          (controller + LOOK_CONTROLLER_PITCH2_OFFSET), so the roll never reaches the first-person view. This
     *          helper builds RotZ(heading) * RotX(pitch) from the same two inputs, which gives a level aim with no
     *          view shake. The game's own third-person camera renders the raw quat instead, which is why it tilts on
     *          horseback.
     * @param look Look quat read from the controller.
     * @param pitch Scalar look pitch in radians, 0 = level.
     * @return Roll-free look orientation.
     */
    [[nodiscard]] inline Quaternion level_look_rotation(const Quaternion &look, float pitch) noexcept
    {
        // Heading of forward = look * +Y, the yaw the engine extracts. The engine writes forward.y as
        // 2 * (y * y + w * w) - 1, which holds only at unit length. The homogeneous form below keeps the heading
        // exact for the slightly non-unit quats the caller's sanity check still accepts.
        const float forward_x = 2.0f * (look.x * look.y - look.w * look.z);
        const float forward_y = look.w * look.w - look.x * look.x + look.y * look.y - look.z * look.z;
        const float heading = std::atan2(-forward_x, forward_y);
        const float sin_heading = std::sin(heading * 0.5f);
        const float cos_heading = std::cos(heading * 0.5f);
        const float sin_pitch = std::sin(pitch * 0.5f);
        const float cos_pitch = std::cos(pitch * 0.5f);
        // Expanded product RotZ(heading) * RotX(pitch) in XYZW order.
        return Quaternion{cos_heading * sin_pitch, sin_heading * sin_pitch, sin_heading * cos_pitch,
                          cos_heading * cos_pitch};
    }

    /**
     * @brief Turns the camera rig rigidly about its pivot, the way a spring arm turns with its control rotation.
     * @details Yaws about world up, then pitches about the horizontal axis square to the yawed look heading. A
     *          positive pitch raises the camera and tilts the look down by exactly that angle, and the look heading
     *          and the shoulder axis stay put. The rotation depends only on the look, never on the arm length or the
     *          shoulder offset, so a follow rig turned this way is the follow rig of the turned look: a change of the
     *          follow distance then slides the camera straight along its own look and does not swing it.
     * @param offset Camera offset from the pivot, turned in place.
     * @param look Camera look direction, turned in place and renormalized.
     * @param rig_up Camera up direction, turned in place. It gives the heading of a look straight up or down.
     * @param yaw Turn about world up in radians, counter-clockwise seen from above.
     * @param pitch Raise of the camera in radians. A positive pitch raises it and tilts the look down.
     */
    inline void turn_rig(Vector3 &offset, Vector3 &look, Vector3 &rig_up, float yaw, float pitch) noexcept
    {
        const float cy = std::cos(yaw);
        const float sy = std::sin(yaw);
        const auto yaw_about_z = [cy, sy](const Vector3 &v) noexcept -> Vector3
        { return Vector3{v.x * cy - v.y * sy, v.x * sy + v.y * cy, v.z}; };
        offset = yaw_about_z(offset);
        look = yaw_about_z(look);
        rig_up = yaw_about_z(rig_up);

        if (pitch < -1e-5f || pitch > 1e-5f)
        {
            // A look straight up or down has no heading of its own. Its up vector then leans along the heading:
            // forward for a look down, backward for a look up.
            Vector3 heading{look.x, look.y, 0.0f};
            if (heading.magnitude_squared() < 1e-6f)
            {
                heading = Vector3{rig_up.x, rig_up.y, 0.0f} * (look.z < 0.0f ? 1.0f : -1.0f);
            }
            if (heading.magnitude_squared() > 1e-6f)
            {
                // up x heading points to the left of the heading. A positive turn about it lifts a camera behind the
                // pivot.
                const Vector3 axis = Vector3{0.0f, 0.0f, 1.0f}.cross(heading.normalized());
                const float cp = std::cos(pitch);
                const float sp = std::sin(pitch);
                const auto rotate_about_axis = [&axis, cp, sp](const Vector3 &v) noexcept -> Vector3
                {
                    const float dot = axis.x * v.x + axis.y * v.y + axis.z * v.z;
                    return v * cp + axis.cross(v) * sp + axis * (dot * (1.0f - cp));
                };
                offset = rotate_about_axis(offset);
                look = rotate_about_axis(look);
                rig_up = rotate_about_axis(rig_up);
            }
        }
        if (look.magnitude_squared() > 1e-6f)
        {
            look = look.normalized();
        }
    }

    /**
     * @brief How far Arm Follow Up and Lens Drop Up move the camera off the plain rig of a look that points up.
     * @details The plain rig sits @p distance back from the pivot along @p look, so it swings down on a sphere as the
     *          look rises and can end low behind the character. Looking up, the arm instead pitches by
     *          @p arm_follow_up's share of the look's pitch around the look's level heading (taken from the rig's
     *          right, so a look straight up still has one), and the camera is lowered by @p lens_drop_up times the sine
     *          of the pitch, held from k_aim_rig_drop_pitch on. Below level, and at 1 and 0, the plain rig is kept: the
     *          crosshair line then passes beside the eye at the shoulder offset, which parapet and steep downward shots
     *          rely on. The shift depends on the look alone, so a turned rig stays the plain rig of the turned look.
     * @param look The rendered look direction (unit).
     * @param rig_up The rendered up direction (unit, square to @p look).
     * @param arm_follow_up The share of the pitch the arm follows, clamped to 0..1.
     * @param lens_drop_up How far the camera is lowered, meters (negative is 0).
     * @return The shift to add to the plain rig's camera position; zero when nothing changes.
     */
    [[nodiscard]] inline Vector3 aim_arm_shift(const Vector3 &look, const Vector3 &rig_up, float distance,
                                               float arm_follow_up, float lens_drop_up) noexcept
    {
        const float follow = std::clamp(arm_follow_up, 0.0f, 1.0f);
        const float drop_up = std::max(0.0f, lens_drop_up);
        const float look_pitch = std::asin(std::clamp(look.z, -1.0f, 1.0f));
        if (!(follow < 1.0f || drop_up > 0.0f) || !(look_pitch > 0.0f))
        {
            return Vector3{};
        }
        const Vector3 heading = Vector3{0.0f, 0.0f, 1.0f}.cross(look.cross(rig_up));
        if (!(heading.magnitude_squared() > 1e-6f))
        {
            return Vector3{};
        }
        const float arm_pitch = look_pitch * follow;
        const Vector3 arm_dir =
            heading.normalized() * std::cos(arm_pitch) + Vector3{0.0f, 0.0f, 1.0f} * std::sin(arm_pitch);
        const float drop = drop_up * std::sin(std::min(look_pitch, k_aim_rig_drop_pitch));
        return (look - arm_dir) * distance - Vector3{0.0f, 0.0f, drop};
    }

    /**
     * @brief The side the shoulder offset is on while SwapShoulderKey slides it: +1 the preset's side, -1 the other,
     *        through the smoothstep of the linear swap progress @p swap (0..1), so it passes through the look line
     *        and starts and lands without a kick.
     */
    [[nodiscard]] inline float shoulder_side(float swap) noexcept
    {
        return 1.0f - 2.0f * smoothstep(std::clamp(swap, 0.0f, 1.0f));
    }
} // namespace TPVCamera

#endif // TPVCAMERA_CORE_RIG_RIG_MATH_HPP
