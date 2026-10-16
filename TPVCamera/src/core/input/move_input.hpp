/**
 * @file core/input/move_input.hpp
 * @brief The movement-input thresholds of camera-relative movement (toggle orbit).
 */
#ifndef TPVCAMERA_CORE_INPUT_MOVE_INPUT_HPP
#define TPVCAMERA_CORE_INPUT_MOVE_INPUT_HPP

namespace TPVCamera
{
    // A move starts when the device-agnostic movement-input magnitude (~0..1.4, the action dispatcher's) rises above
    // k_move_input_start from idle, which aligns the heading to the camera once, and ends only below the lower
    // k_move_input_stop (hysteresis, so a momentary dip cannot re-trigger the align). The input stays nonzero while a
    // key is held against a wall, so the heading is not falsely released on a collision stop.
    constexpr float k_move_input_start = 0.15f;
    constexpr float k_move_input_stop = 0.05f;
} // namespace TPVCamera

#endif // TPVCAMERA_CORE_INPUT_MOVE_INPUT_HPP
