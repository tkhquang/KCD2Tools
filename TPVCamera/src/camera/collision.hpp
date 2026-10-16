/**
 * @file camera/collision.hpp
 * @brief The camera collision and the occluder fade set (core/collision/arm.hpp) run on the live game: the live probe,
 *        the INI and preset values, the trace lines in the log, and the fade set handed to the dissolve hook.
 */
#ifndef TPVCAMERA_CAMERA_COLLISION_HPP
#define TPVCAMERA_CAMERA_COLLISION_HPP

#include "core/collision/arm.hpp"
#include "math_utils.hpp"

namespace TPVCamera
{
    /** @brief The collision's INI and preset values as they stand now (LiveSettings, relaxed). */
    [[nodiscard]] CollisionParams collision_params() noexcept;

    /** @brief The collision's trace lines, written to the log at Trace (see collision_trace.hpp). */
    [[nodiscard]] CollisionTrace &live_collision_trace() noexcept;

    /**
     * @brief Runs the camera collision along the arm (core/collision/arm.hpp) on the live probe with the current
     *        values. Main thread only.
     * @return The camera position after collision.
     */
    [[nodiscard]] Vector3 resolve_camera_collision(CollisionState &state, const CollisionInput &in);

    /**
     * @brief Runs the shoulder collision (core/collision/arm.hpp) on the live probe with the current values. Main
     *        thread only.
     * @return The shoulder offset's allowed length.
     */
    [[nodiscard]] float resolve_shoulder_collision(CollisionState &state, const ShoulderInput &in);

    /**
     * @brief Hands the fade set to the dissolve hook and logs it when it changes.
     * @details With the fade off the set is empty, so whatever is still faded fades back in. Each object fades until
     *          only OccluderFadeMinOpacity of it stays, a ghost of where it stands. Main thread only, once per frame
     *          (the frame's second build changes nothing).
     */
    void submit_fade_set(const CollisionState &state, float delta_time);
} // namespace TPVCamera

#endif // TPVCAMERA_CAMERA_COLLISION_HPP
