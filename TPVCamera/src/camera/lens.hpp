/**
 * @file camera/lens.hpp
 * @brief The third-person lens near plane: the near clip plane pushed out, so what touches the lens is cut away.
 */
#ifndef TPVCAMERA_CAMERA_LENS_HPP
#define TPVCAMERA_CAMERA_LENS_HPP

#include "game_structures.hpp"

#include <cstdint>

namespace TPVCamera
{
    /**
     * @brief Moves the near plane of this build of @p camera out to the lens clearance (see k_lens_near_max).
     * @details The builder rebuilds the cull planes and the render projection from the near corner, which SetFrustum
     *          writes fresh before every build, so scaling it moves the near plane for this build only. The clearance
     *          is measured once per frame from the final pose and the same plane is used for the frame's second build;
     *          it eases with the view blend, so first person keeps the game's plane. Main thread only (the camera
     *          detour), after the final pose is written to @p view.
     * @param camera The CCamera being built; its near corner is at CCAMERA_NEAR_CORNER_OFFSET.
     * @param view The camera's final matrix: the basis columns and the translation.
     * @param arm The arm after collision, from the head: the plane stays k_lens_near_character_margin short of it.
     */
    void apply_lens_near_plane(std::uintptr_t camera, const GameStructures::Matrix34f &view, float arm,
                               float view_blend, float delta_time, bool repeat_build);
} // namespace TPVCamera

#endif // TPVCAMERA_CAMERA_LENS_HPP
