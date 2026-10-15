/**
 * @file camera/blend.hpp
 * @brief The camera's eases: the two-stage critically damped filter the preset blend and the FOV share, and the 0..1
 *        weights the view switch and the first-person fallback ride.
 *
 * @details Engine-free (no Windows or DetourModKit header), so the local tests compile it as is.
 */
#ifndef TPVCAMERA_CAMERA_BLEND_HPP
#define TPVCAMERA_CAMERA_BLEND_HPP

#include <algorithm>
#include <cmath>

namespace TPVCamera
{
    // Two cascaded critically damped stages of rate k reach 95% of a step at k*t = 4.7439 and 99% at k*t = 6.6384,
    // where 1 - (1 + kt)e^-kt is 0.95 and 0.99. A blend time is the 99% settle: rate = k_spring_t99 / seconds.
    constexpr float k_spring_t95 = 4.7439f;
    constexpr float k_spring_t99 = 6.6384f;

    /** @brief The weight one first-order stage of @p rate (1/s) moves toward its target over @p dt: 1 - e^-rate*dt. */
    [[nodiscard]] inline float stage_alpha(float rate, float dt) noexcept
    {
        return 1.0f - std::exp(-rate * dt);
    }

    /**
     * @brief The weight each stage of a Spring moves over @p dt to settle 99% in @p seconds: 1 (a cut) at 0 seconds or
     *        less.
     */
    [[nodiscard]] inline float spring_alpha(float seconds, float dt) noexcept
    {
        return seconds > 0.0f ? stage_alpha(k_spring_t99 / seconds, dt) : 1.0f;
    }

    /** @brief Moves @p value by @p alpha of the way to @p target: one stage of the filter. */
    inline void ease_toward(float &value, float target, float alpha) noexcept
    {
        value = value + (target - value) * alpha;
    }

    /**
     * @brief Two cascaded first-order low-passes of the same rate: a critically damped S-curve toward the target.
     * @details A single exponential low-pass starts at full speed and decays, so a transition lurches off the mark.
     *          The second stage smooths the first one's sharp start, so the value eases in and out, and never
     *          overshoots. Frame-rate independent through stage_alpha().
     * @tparam S The eased value: float, or a struct with its own ease_toward(S &, const S &, float), found by
     *           argument-dependent lookup (the camera preset's eases each field, and snaps its mode fields).
     */
    template <typename S> struct Spring
    {
        S stage1{};  // the first stage: the low-passed target
        S applied{}; // the second stage: the low-passed first stage, the value used

        /** @brief Puts both stages on @p target: no ease across a gap or from a first frame. */
        void snap(const S &target)
        {
            stage1 = target;
            applied = target;
        }

        /** @brief Advances both stages toward @p target by one step of weight @p alpha (see spring_alpha()). */
        void step(const S &target, float alpha) noexcept
        {
            ease_toward(stage1, target, alpha);
            ease_toward(applied, stage1, alpha);
        }
    };

    /**
     * @brief A 0..1 weight moved toward @p target at 1/@p duration per second: a linear ramp that lands on the target.
     * @return @p target at once when @p duration is not above 1e-4 s (an instant switch).
     */
    [[nodiscard]] inline float ramp_weight(float weight, float target, float duration, float dt) noexcept
    {
        if (!(duration > 1e-4f))
        {
            return target;
        }
        const float step = dt / duration;
        return (weight < target) ? std::min(target, weight + step) : std::max(target, weight - step);
    }

    /** @brief The smoothstep ease-in/out of a 0..1 weight: a linear ramp read through it starts and lands gently. */
    [[nodiscard]] inline float smoothstep(float weight) noexcept
    {
        return weight * weight * (3.0f - 2.0f * weight);
    }

    /**
     * @brief A 0..1 weight eased toward @p target at @p rate (1/s), landing on it once within @p land, which the
     *        exponential would otherwise only approach.
     */
    [[nodiscard]] inline float ease_weight(float weight, float target, float rate, float dt, float land) noexcept
    {
        const float next = weight + (target - weight) * stage_alpha(rate, dt);
        return (std::fabs(target - next) < land) ? target : next;
    }
} // namespace TPVCamera

#endif // TPVCAMERA_CAMERA_BLEND_HPP
