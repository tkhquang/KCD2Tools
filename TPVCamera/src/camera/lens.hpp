/**
 * @file camera/lens.hpp
 * @brief The third-person lens near plane: the near clip plane pushed out, so what touches the lens is cut away.
 */
#ifndef TPVCAMERA_CAMERA_LENS_HPP
#define TPVCAMERA_CAMERA_LENS_HPP

#include "camera/commit.hpp"

#include <array>
#include <optional>

namespace TPVCamera
{
    /**
     * @brief The near plane this build of the game view uses, or std::nullopt to keep the game's own
     *        (core/lens/lens_plan.hpp).
     * @details The clearance is measured once per frame from the final pose and the same plane is used for the frame's
     *          second build; it eases with the view blend, so first person keeps the game's plane. Main thread only
     *          (the camera detour), once the final pose is decided.
     * @param view The final pose: the position and the basis this build commits.
     * @param near_corner The near corner as this build commits it before the near plane: the one it found, scaled by
     *        the FOV override (committed_near_corner() with no near override yet).
     * @param arm The arm after collision, from the head: the plane stays k_lens_near_character_margin short of it.
     */
    [[nodiscard]] std::optional<float> plan_live_lens_near(const ViewWrite &view,
                                                           const std::array<float, 3> &near_corner, float arm,
                                                           float view_blend, float delta_time, bool repeat_build);
} // namespace TPVCamera

#endif // TPVCAMERA_CAMERA_LENS_HPP
