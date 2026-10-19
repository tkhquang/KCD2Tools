/**
 * @file audio/voice_analysis.cpp
 * @brief Spectral voice features and the mouth-pose mapping.
 */

#include "audio/voice_analysis.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace SoundFramework::audio
{
    namespace
    {
        using Complex = std::complex<float>;

        /** @brief In-place iterative radix-2 FFT; @p data's size is a power of two. */
        void fft(std::vector<Complex> &data)
        {
            const std::size_t n = data.size();
            for (std::size_t i = 1, j = 0; i < n; ++i)
            {
                std::size_t bit = n >> 1;
                for (; (j & bit) != 0; bit >>= 1)
                {
                    j ^= bit;
                }
                j ^= bit;
                if (i < j)
                {
                    std::swap(data[i], data[j]);
                }
            }
            for (std::size_t length = 2; length <= n; length <<= 1)
            {
                const float angle = -2.0f * std::numbers::pi_v<float> / static_cast<float>(length);
                const Complex step(std::cos(angle), std::sin(angle));
                for (std::size_t i = 0; i < n; i += length)
                {
                    Complex w(1.0f, 0.0f);
                    for (std::size_t k = 0; k < length / 2; ++k)
                    {
                        const Complex even = data[i + k];
                        const Complex odd = data[i + k + length / 2] * w;
                        data[i + k] = even + odd;
                        data[i + k + length / 2] = even - odd;
                        w *= step;
                    }
                }
            }
        }

        [[nodiscard]] constexpr float saturate(float value) noexcept
        {
            return std::clamp(value, 0.0f, 1.0f);
        }

        /** @brief Linear ramp from 0 at @p low to 1 at @p high (either direction). */
        [[nodiscard]] constexpr float ramp(float value, float low, float high) noexcept
        {
            return saturate((value - low) / (high - low));
        }
    } // namespace

    std::vector<VoiceFrame> analyze_voice(std::span<const float> samples, float sample_rate, std::uint32_t step_ms)
    {
        std::vector<VoiceFrame> frames;
        if (samples.empty() || sample_rate <= 0.0f || step_ms == 0)
        {
            return frames;
        }
        const std::size_t step = std::max<std::size_t>(1, static_cast<std::size_t>(sample_rate * step_ms / 1000.0f));
        std::size_t size = 1;
        while (size < step)
        {
            size <<= 1;
        }
        const float bin_hz = sample_rate / static_cast<float>(size);
        std::vector<Complex> buffer(size);
        float peak = 0.0f;
        for (std::size_t start = 0; start < samples.size(); start += step)
        {
            const std::size_t count = std::min(step, samples.size() - start);
            double sum_squares = 0.0;
            std::ranges::fill(buffer, Complex{});
            for (std::size_t i = 0; i < count; ++i)
            {
                const float sample = samples[start + i];
                sum_squares += static_cast<double>(sample) * sample;
                const float window =
                    0.5f - 0.5f * std::cos(2.0f * std::numbers::pi_v<float> * i / static_cast<float>(count));
                buffer[i] = Complex(sample * window, 0.0f);
            }
            fft(buffer);
            double low_energy = 0.0;
            double low_moment = 0.0;
            double mid_energy = 0.0;
            double mid_moment = 0.0;
            double total = 0.0;
            double high = 0.0;
            for (std::size_t k = 1; k < size / 2; ++k)
            {
                const float hz = static_cast<float>(k) * bin_hz;
                const double energy = std::norm(buffer[k]);
                if (hz >= 200.0f && hz <= 1000.0f)
                {
                    low_energy += energy;
                    low_moment += energy * hz;
                }
                if (hz >= 150.0f && hz <= 5000.0f)
                {
                    mid_energy += energy;
                    mid_moment += energy * hz;
                }
                if (hz >= 150.0f && hz <= 8000.0f)
                {
                    total += energy;
                    if (hz >= 4000.0f)
                    {
                        high += energy;
                    }
                }
            }
            VoiceFrame frame;
            frame.loudness = static_cast<float>(std::sqrt(sum_squares / static_cast<double>(count)));
            frame.low_centroid = low_energy > 0.0 ? static_cast<float>(low_moment / low_energy) : 0.0f;
            frame.centroid = mid_energy > 0.0 ? static_cast<float>(mid_moment / mid_energy) : 0.0f;
            frame.hiss = total > 0.0 ? static_cast<float>(high / total) : 0.0f;
            peak = std::max(peak, frame.loudness);
            frames.push_back(frame);
        }
        if (peak > 0.0f)
        {
            for (VoiceFrame &frame : frames)
            {
                frame.loudness /= peak;
            }
        }
        return frames;
    }

    std::vector<MouthPose> mouth_track(std::span<const VoiceFrame> frames, std::uint32_t step_ms, std::uint32_t lead_ms)
    {
        constexpr float noise_floor = 0.04f;
        constexpr float attack = 0.6f;
        constexpr float release = 0.25f;
        // Lips stay this open inside speech (within the hold window of any active step).
        constexpr float speech_lips = 0.12f;
        constexpr std::uint32_t hold_ms = 120;

        std::vector<MouthPose> raw(frames.size());
        std::vector<float> activity(frames.size());
        for (std::size_t i = 0; i < frames.size(); ++i)
        {
            const VoiceFrame &frame = frames[i];
            const float active = std::sqrt(saturate((frame.loudness - noise_floor) / (1.0f - noise_floor)));
            activity[i] = active;
            const float open = ramp(frame.low_centroid, 250.0f, 750.0f);
            const float rounded = ramp(frame.centroid, 1300.0f, 600.0f);
            const float spread = ramp(frame.centroid, 1700.0f, 3000.0f);
            const float fricative = ramp(frame.hiss, 0.15f, 0.45f);
            MouthPose &pose = raw[i];
            pose.jaw = active * (0.25f + 0.75f * open) * (1.0f - 0.6f * fricative);
            pose.lips = active * (0.6f + 0.4f * open);
            pose.round_open = active * rounded * open;
            pose.round_closed = active * rounded * (1.0f - open);
            pose.spread = active * spread;
            pose.teeth = std::max(active * spread * 0.7f, saturate(fricative * frame.loudness * 3.0f));
        }

        // Smooth each channel with a fast attack and slower release.
        MouthPose level;
        const auto follow = [](float &current, float target)
        {
            current += (target > current ? attack : release) * (target - current);
            return current;
        };
        std::vector<MouthPose> smooth(frames.size());
        for (std::size_t i = 0; i < raw.size(); ++i)
        {
            smooth[i] = MouthPose{
                .jaw = follow(level.jaw, raw[i].jaw),
                .lips = follow(level.lips, raw[i].lips),
                .round_open = follow(level.round_open, raw[i].round_open),
                .round_closed = follow(level.round_closed, raw[i].round_closed),
                .spread = follow(level.spread, raw[i].spread),
                .teeth = follow(level.teeth, raw[i].teeth),
            };
        }

        // Keep the lips slightly apart inside speech, then shift everything earlier by the lead.
        const std::size_t hold = step_ms == 0 ? 0 : hold_ms / step_ms;
        const std::size_t lead = step_ms == 0 ? 0 : lead_ms / step_ms;
        std::vector<MouthPose> track(frames.size());
        for (std::size_t i = 0; i < track.size(); ++i)
        {
            const std::size_t source = std::min(i + lead, smooth.size() - 1);
            MouthPose pose = smooth[source];
            const std::size_t from = source > hold ? source - hold : 0;
            const std::size_t to = std::min(source + hold, activity.size() - 1);
            const bool speaking = std::any_of(
                activity.begin() + static_cast<std::ptrdiff_t>(from),
                activity.begin() + static_cast<std::ptrdiff_t>(to) + 1,
                [](float a) { return a > 0.2f; }
            );
            if (speaking)
            {
                pose.lips = std::max(pose.lips, speech_lips);
            }
            track[i] = pose;
        }
        return track;
    }
} // namespace SoundFramework::audio
