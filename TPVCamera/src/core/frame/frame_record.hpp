/**
 * @file core/frame/frame_record.hpp
 * @brief The record of the last camera frame (LastFinal): what the camera put on screen and the eye it was built from,
 *        for the hooks that re-origin the game's own rays and observers onto it.
 */
#ifndef TPVCAMERA_CORE_FRAME_FRAME_RECORD_HPP
#define TPVCAMERA_CORE_FRAME_FRAME_RECORD_HPP

#include "core/math/vec.hpp"

namespace TPVCamera
{
    /** @brief One camera frame as it went on screen. */
    struct FrameRecord
    {
        Vector3 eye{};      // the untouched first-person eye the frame was built from (the CView pose)
        Vector3 position{}; // the rendered camera
        Vector3 forward{};  // the screen centre's direction: the crosshair
        bool valid = false; // the third-person offset rendered the frame; false in first person
    };
} // namespace TPVCamera

#endif // TPVCAMERA_CORE_FRAME_FRAME_RECORD_HPP
