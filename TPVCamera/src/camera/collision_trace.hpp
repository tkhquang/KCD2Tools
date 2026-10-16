/**
 * @file camera/collision_trace.hpp
 * @brief The camera's log lines for the collision walk, the fade set and the see-through window.
 *
 * @details Each logger keeps the last set it logged and writes a line only when the set changes, so a wrong stop, pass
 *          or fade is identifiable from the log alone. Main thread only (the camera detour). A failed allocation drops
 *          the line, so a logger never leaves the camera frame with an exception.
 */
#ifndef TPVCAMERA_CAMERA_COLLISION_TRACE_HPP
#define TPVCAMERA_CAMERA_COLLISION_TRACE_HPP

#include "core/blend/ease.hpp"
#include "hooks/shader_twins.hpp"
#include "render_occlusion.hpp"

#include <cstdint>
#include <span>

namespace TPVCamera
{
    /** @brief Notes @p motion, the result of one collision ease step, for the walk trace's pull-in and release ages. */
    void note_collision_motion(EaseMotion motion) noexcept;

    /**
     * @brief Logs the camera walk at Trace when the colliders it met or what it did with them change.
     * @details Each hit's object, class and distance along the arm, and how long ago the collision last pulled the
     *          camera in and last started a release, so a wrong stop or a wrong pass is identifiable from the log.
     */
    void log_walk_change(std::span<const WalkStep> steps, float desired_distance) noexcept;

    /**
     * @brief Logs the fade set at Debug when it changes, naming each object with how it was found.
     * @details [a] stepped past by the arm, [s] on a sightline; ! the camera inside its physics, + the camera buried in
     *          its mesh.
     */
    void log_fade_set_change(std::span<const FadeEntry> entries) noexcept;

    /**
     * @brief Logs the plants listed for the see-through window and the bare frames listed for the near cut at Debug,
     *        each when its list changes, by name and with the window's size.
     * @details The swaps themselves are logged by the shader twins module.
     */
    void log_tree_window_change(std::span<void *const> trees, std::span<void *const> frames,
                                const TreeHoleWindow &window) noexcept;
} // namespace TPVCamera

#endif // TPVCAMERA_CAMERA_COLLISION_TRACE_HPP
