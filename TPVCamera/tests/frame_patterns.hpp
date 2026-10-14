/**
 * @file frame_patterns.hpp
 * @brief The frame-time patterns of the rate matrix: steady 30, 60 and 144 Hz, a jittered 60 Hz and a 60 Hz run with
 *        one 0.1 s hitch.
 */
#ifndef TPVCAMERA_TESTS_FRAME_PATTERNS_HPP
#define TPVCAMERA_TESTS_FRAME_PATTERNS_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace TPVCamera::Tests
{
    /** @brief One named frame-time sequence, in seconds per frame. */
    struct FramePattern
    {
        std::string name;
        std::vector<float> frames;

        /** @brief The longest frame of the pattern: the tolerance a settle time is held to. */
        [[nodiscard]] float longest() const { return *std::max_element(frames.begin(), frames.end()); }
    };

    /** @brief Every pattern of the rate matrix, each @p count frames long. */
    [[nodiscard]] inline std::vector<FramePattern> rate_matrix(std::size_t count = 4000)
    {
        std::vector<FramePattern> patterns;
        for (const float hz : {30.0f, 60.0f, 144.0f})
        {
            patterns.push_back({std::to_string(static_cast<int>(hz)) + " Hz", std::vector<float>(count, 1.0f / hz)});
        }
        // A fixed linear congruential sequence keeps the jitter the same on every run.
        FramePattern jitter{"jitter 16.4-16.9 ms", {}};
        std::uint32_t seed = 12345u;
        for (std::size_t i = 0; i < count; ++i)
        {
            seed = seed * 1664525u + 1013904223u;
            const float unit = static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
            jitter.frames.push_back(0.0164f + 0.0005f * unit);
        }
        patterns.push_back(std::move(jitter));
        FramePattern hitch{"60 Hz with a 0.1 s hitch", std::vector<float>(count, 1.0f / 60.0f)};
        hitch.frames[3] = 0.1f;
        patterns.push_back(std::move(hitch));
        return patterns;
    }
} // namespace TPVCamera::Tests

#endif // TPVCAMERA_TESTS_FRAME_PATTERNS_HPP
