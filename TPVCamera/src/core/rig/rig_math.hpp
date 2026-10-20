/**
 * @file core/rig/rig_math.hpp
 * @brief The over-the-shoulder rig's geometry: the roll-free look, the rigid turn of the rig about its pivot, the arm
 *        bent when looking up, the shoulder side the swap slides through, and the combat lock framing.
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
#include <numbers>

namespace TPVCamera
{
    // Lens Drop Up stops growing at this look pitch (43 deg), as Ryse's aim camera clamps its pitch slide.
    constexpr float k_aim_rig_drop_pitch = 0.75f; // rad

    /**
     * @brief The heading of a look orientation: the yaw of its forward (look * +Y), forward = (-sin, cos).
     * @details The engine writes forward.y as 2 * (y * y + w * w) - 1, which holds only at unit length. The homogeneous
     *          form keeps the heading exact for the slightly non-unit quats the rig's sanity check still accepts.
     */
    [[nodiscard]] inline float look_heading(const Quaternion &look) noexcept
    {
        const float forward_x = 2.0f * (look.x * look.y - look.w * look.z);
        const float forward_y = look.w * look.w - look.x * look.x + look.y * look.y - look.z * look.z;
        return std::atan2(-forward_x, forward_y);
    }

    /**
     * @brief The roll-free look orientation RotZ(@p heading) * RotX(@p pitch), radians.
     * @details The raw look quat can carry a stale roll angle (see LOOK_CONTROLLER_QUAT_OFFSET).
     *          C_CameraFirstPerson::Update never renders that quat directly. It takes only the heading of the look
     *          forward vector (look_heading()) and combines it with the scalar pitch that C_Player vtable slot 59
     *          returns (controller + LOOK_CONTROLLER_PITCH2_OFFSET), so the roll never reaches the first-person view.
     *          The same two inputs give a level aim with no view shake. The game's own third-person camera renders
     *          the raw quat instead, which is why it tilts on horseback.
     */
    [[nodiscard]] inline Quaternion level_rotation(float heading, float pitch) noexcept
    {
        const float sin_heading = std::sin(heading * 0.5f);
        const float cos_heading = std::cos(heading * 0.5f);
        const float sin_pitch = std::sin(pitch * 0.5f);
        const float cos_pitch = std::cos(pitch * 0.5f);
        // Expanded product RotZ(heading) * RotX(pitch) in XYZW order.
        return Quaternion{cos_heading * sin_pitch, sin_heading * sin_pitch, sin_heading * cos_pitch,
                          cos_heading * cos_pitch};
    }

    /**
     * @brief How fast the look turned over one step, radians per second: the heading turn shortened by the cosine of
     *        the pitch (the arc the forward sweeps) combined with the pitch turn.
     * @return 0 for a step of no time.
     */
    [[nodiscard]] inline float look_turn_speed(float turn_heading, float turn_pitch, float pitch, float dt) noexcept
    {
        if (!(dt > 0.0f))
        {
            return 0.0f;
        }
        const float sweep = turn_heading * std::cos(pitch);
        return std::sqrt(sweep * sweep + turn_pitch * turn_pitch) / dt;
    }

    /**
     * @brief Follows @p offset softly: offset^3 / (offset^2 + dead_zone^2), with no step anywhere.
     * @details Near 0 it barely moves (a fifth of the offset at half the dead zone), at the dead zone it gives half,
     *          and past it it tracks the offset to within dead_zone^2 / offset. So head bob and breathing stay out of
     *          the anchor while a kneel or a bow comes through, and the follow never jumps at a threshold.
     */
    [[nodiscard]] inline float soft_follow(float offset, float dead_zone) noexcept
    {
        const float squared = offset * offset;
        const float denominator = squared + dead_zone * dead_zone;
        return denominator > 0.0f ? offset * squared / denominator : 0.0f;
    }

    // Look snap detection (is_look_snap()). A jump the game makes lands in one frame, so its speed scales with the
    // frame rate: 36-74 deg jumps read over 700 deg/s, but a 10 deg one at 30 fps only 315 deg/s. A turn faster than
    // k_look_snap_speed is a jump at any frame rate. A slower turn is a jump when it starts at once: faster than
    // k_look_snap_onset_speed and k_look_snap_onset_ratio times the last step's speed. A turn the game eases (a horse's
    // heading, the look that follows the body into a seat) builds up over frames and passes.
    constexpr float k_look_snap_speed = 6.2832f;       // rad/s (360 deg/s)
    constexpr float k_look_snap_onset_speed = 1.5708f; // rad/s (90 deg/s)
    constexpr float k_look_snap_onset_ratio = 3.0f;

    /**
     * @brief Whether a look turn of @p speed (rad/s) after a step of @p last_speed is a jump rather than a turn the
     *        game eases (see k_look_snap_speed).
     */
    [[nodiscard]] inline bool is_look_snap(float speed, float last_speed) noexcept
    {
        return speed > k_look_snap_speed ||
               (speed > k_look_snap_onset_speed && speed > k_look_snap_onset_ratio * last_speed);
    }

    /**
     * @brief Whether the game's part of a frame's look turn (@p game_heading, @p game_pitch) is a view limit that
     *        stops the player's own turn (@p player_heading, @p player_pitch): against it, and no larger. Radians.
     */
    [[nodiscard]] inline bool is_look_clamp(float game_heading, float game_pitch, float player_heading,
                                            float player_pitch) noexcept
    {
        const float against = game_heading * player_heading + game_pitch * player_pitch;
        const float game = game_heading * game_heading + game_pitch * game_pitch;
        const float player = player_heading * player_heading + player_pitch * player_pitch;
        return against < 0.0f && game <= player * 1.001f;
    }

    /**
     * @brief Holds a turn of the look the game made off the rendered look and eases it back out along an S-curve.
     * @details The hold is added to the look the rig renders. A held turn is taken out of both stages of each spring,
     *          so the rendered look does not move with it, and each spring then steps toward 0 by @p alpha
     *          (spring_alpha() of the settle time, where 0 keeps a jump whole on its frame). The second stage trails
     *          the first, so the ease starts from rest instead of a one-stage ease's lurch, and turns taken in frame
     *          after frame come out as a second-order low-pass of the look. The heading hold is kept within half a
     *          turn, so it eases back the short way.
     * @param heading_hold The heading hold (radians), stepped in place.
     * @param pitch_hold The pitch hold (radians), stepped in place.
     * @param turn_heading This step's heading turn (radians), held when @p hold.
     * @param turn_pitch This step's pitch turn (radians), held when @p hold.
     * @param hold Whether this step's turn is held.
     * @param alpha The step's ease weight toward 0.
     */
    inline void hold_look_snap(Spring<float> &heading_hold, Spring<float> &pitch_hold, float turn_heading,
                               float turn_pitch, bool hold, float alpha) noexcept
    {
        if (hold)
        {
            heading_hold.stage1 -= turn_heading;
            heading_hold.applied -= turn_heading;
            pitch_hold.stage1 -= turn_pitch;
            pitch_hold.applied -= turn_pitch;
            const float wrap =
                heading_hold.applied - std::remainder(heading_hold.applied, 2.0f * std::numbers::pi_v<float>);
            heading_hold.stage1 -= wrap;
            heading_hold.applied -= wrap;
        }
        heading_hold.step(0.0f, alpha);
        pitch_hold.step(0.0f, alpha);
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

    // The game drops the lock-on while the player turns the view once the camera points farther from the opponent,
    // seen from the player, than atan2(k_lock_area_width, distance): LockAreaWidth + UnlockAreaWidthHyst with the
    // shipped wh_cs_ values (sub_1808BA62C). The lock framing aims within k_lock_area_share of that angle.
    constexpr float k_lock_area_width = 1.75f; // m
    constexpr float k_lock_area_share = 0.6f;

    /**
     * @struct PivotPose
     * @brief A camera about the rig's pivot: its offset from the pivot and its look's heading and pitch.
     */
    struct PivotPose
    {
        /// The camera's offset from the pivot.
        Vector3 offset{};
        /// The look's heading, a world yaw in radians (forward = (-sin yaw, cos yaw)).
        float yaw = 0.0f;
        /// The look's pitch in radians, positive up.
        float pitch = 0.0f;
    };

    /**
     * @brief The fully framed camera of the combat lock framing.
     * @details The plain rig's arm turns about the pivot onto the line to the focus and @p framing_angle further toward
     *          @p side, and the look aims @p focus_offset beside the focus toward @p side, so the opponent sits that
     *          far toward the other side of the screen from the player. The aim stays within k_lock_area_share of the
     *          angle at which the game drops the lock while the player turns the view, so at range the opponent sits a
     *          little off the framed spot instead of the lock dropping with the next mouse move.
     * @param plain_offset The plain rig's camera offset from the pivot.
     * @param plain_yaw The plain rig's look heading.
     * @param to_focus The focus point less the pivot. Its level part must not be zero.
     * @param side The shoulder side: +1 right, -1 left, and in between while the shoulder swap slides.
     * @param framing_angle Radians the arm swings past the line to the focus.
     * @param focus_offset Meters the aim point sits beside the focus.
     * @return The framed camera.
     */
    [[nodiscard]] inline PivotPose lock_frame_pose(const Vector3 &plain_offset, float plain_yaw,
                                                   const Vector3 &to_focus, float side, float framing_angle,
                                                   float focus_offset) noexcept
    {
        constexpr float k_two_pi = 2.0f * std::numbers::pi_v<float>;
        const float bearing = std::atan2(-to_focus.x, to_focus.y);
        const float turn = std::remainder(bearing + side * framing_angle - plain_yaw, k_two_pi);
        const float cos_turn = std::cos(turn);
        const float sin_turn = std::sin(turn);
        const Vector3 offset{plain_offset.x * cos_turn - plain_offset.y * sin_turn,
                             plain_offset.x * sin_turn + plain_offset.y * cos_turn, plain_offset.z};
        const Vector3 to_target = to_focus - offset;
        const Vector3 aim_right = to_target.cross(Vector3{0.0f, 0.0f, 1.0f}).normalized();
        const Vector3 aim = (to_target - aim_right * (side * focus_offset)).normalized();
        float yaw = std::atan2(-aim.x, aim.y);
        const float reach = std::sqrt(to_focus.x * to_focus.x + to_focus.y * to_focus.y);
        const float aim_limit = k_lock_area_share * std::atan2(k_lock_area_width, reach);
        const float aim_off = std::remainder(yaw - bearing, k_two_pi);
        if (std::fabs(aim_off) > aim_limit)
        {
            yaw = bearing + std::copysign(aim_limit, aim_off);
        }
        return PivotPose{
            .offset = offset,
            .yaw = yaw,
            .pitch = std::asin(std::clamp(aim.z, -1.0f, 1.0f)),
        };
    }

    /**
     * @brief Blends two cameras about the pivot by angle, so a wide swing circles the pivot instead of cutting through
     *        it.
     * @details The offset's level angle about the pivot and the look's heading take the shorter way round. The level
     *          reach, the height and the pitch blend linearly.
     * @param from The camera at @p weight 0.
     * @param to The camera at @p weight 1.
     * @param weight The blend, 0..1.
     * @return The blended camera.
     */
    [[nodiscard]] inline PivotPose blend_pivot_pose(const PivotPose &from, const PivotPose &to, float weight) noexcept
    {
        constexpr float k_two_pi = 2.0f * std::numbers::pi_v<float>;
        const float from_reach = std::sqrt(from.offset.x * from.offset.x + from.offset.y * from.offset.y);
        const float to_reach = std::sqrt(to.offset.x * to.offset.x + to.offset.y * to.offset.y);
        const float from_around = std::atan2(from.offset.y, from.offset.x);
        const float around =
            from_around + weight * std::remainder(std::atan2(to.offset.y, to.offset.x) - from_around, k_two_pi);
        const float reach = from_reach + (to_reach - from_reach) * weight;
        return PivotPose{
            .offset = Vector3{std::cos(around) * reach, std::sin(around) * reach,
                              from.offset.z + (to.offset.z - from.offset.z) * weight},
            .yaw = from.yaw + weight * std::remainder(to.yaw - from.yaw, k_two_pi),
            .pitch = from.pitch + (to.pitch - from.pitch) * weight,
        };
    }
} // namespace TPVCamera

#endif // TPVCAMERA_CORE_RIG_RIG_MATH_HPP
