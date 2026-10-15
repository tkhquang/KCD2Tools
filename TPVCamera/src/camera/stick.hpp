/**
 * @file camera/stick.hpp
 * @brief The response curve the gamepad right stick's free-look deflection is read through.
 *
 * @details Engine-free (no Windows or DetourModKit header), so the local tests compile it as is.
 */
#ifndef TPVCAMERA_CAMERA_STICK_HPP
#define TPVCAMERA_CAMERA_STICK_HPP

#include <algorithm>
#include <cmath>

namespace TPVCamera
{
    /// One reading of a stick's two axes, each -1..1 as the engine reports it.
    struct StickDeflection
    {
        float x = 0.0f;
        float y = 0.0f;
    };

    /**
     * @brief Reads @p raw through the response curve @p exponent along the push's own direction: a push of length m
     *        in [0, 1] becomes one of length m^exponent.
     * @details Both axes are scaled by the same factor, so a diagonal push keeps its direction and neither axis gates
     *          the other. A higher exponent turns more slowly for a small push, for fine aim, and keeps the full rate
     *          at full deflection. A push longer than 1 (the corner of a square gate) keeps the factor of a full push,
     *          1, as it reached the engine. An exponent of 1 returns @p raw bit for bit.
     * @param exponent The curve, 1 (linear) or above; the caller clamps it.
     */
    [[nodiscard]] inline StickDeflection shape_stick(StickDeflection raw, float exponent) noexcept
    {
        const float length = std::hypot(raw.x, raw.y);
        if (exponent == 1.0f || !(length > 0.0f))
        {
            return raw;
        }
        const float scale = std::pow(std::min(length, 1.0f), exponent - 1.0f);
        return {raw.x * scale, raw.y * scale};
    }
} // namespace TPVCamera

#endif // TPVCAMERA_CAMERA_STICK_HPP
