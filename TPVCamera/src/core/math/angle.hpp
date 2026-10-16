/**
 * @file core/math/angle.hpp
 * @brief Degree and radian conversions, the same arithmetic as DetourModKit's math, for the engine-free core.
 */
#ifndef TPVCAMERA_CORE_MATH_ANGLE_HPP
#define TPVCAMERA_CORE_MATH_ANGLE_HPP

#include <numbers>

namespace TPVCamera
{
    [[nodiscard]] constexpr float degrees_to_radians(float degrees) noexcept
    {
        return degrees * (std::numbers::pi_v<float> / 180.0f);
    }

    [[nodiscard]] constexpr float radians_to_degrees(float radians) noexcept
    {
        return radians * (180.0f / std::numbers::pi_v<float>);
    }
} // namespace TPVCamera

#endif // TPVCAMERA_CORE_MATH_ANGLE_HPP
