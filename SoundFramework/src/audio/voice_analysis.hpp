/**
 * @file audio/voice_analysis.hpp
 * @brief Turns a voice recording into mouth poses: per-step spectral features, then jaw, lip, rounding, spread and
 *        teeth weights for the face's pose bones.
 *
 * Plain C++ with no game dependency, so it is unit-tested on synthetic signals. The features are deliberately simple
 * and language-agnostic: loudness says how much the mouth moves, the low-band centroid tracks the first formant (how
 * open the vowel is), the overall centroid separates dark rounded vowels from bright spread ones, and the share of
 * energy above 4 kHz marks fricatives (s, sh, f), which show the teeth with a nearly closed jaw.
 */
#ifndef SNDFW_AUDIO_VOICE_ANALYSIS_HPP
#define SNDFW_AUDIO_VOICE_ANALYSIS_HPP

#include <cstdint>
#include <span>
#include <vector>

namespace SoundFramework::audio
{
    /** @brief The features of one analysis step. */
    struct VoiceFrame
    {
        /// RMS loudness, normalized so the loudest step of the recording is 1.
        float loudness = 0.0f;
        /// Energy centroid between 200 and 1000 Hz (Hz): higher for more open vowels.
        float low_centroid = 0.0f;
        /// Energy centroid between 150 and 5000 Hz (Hz): low for rounded vowels, high for spread ones.
        float centroid = 0.0f;
        /// Share of the 150..8000 Hz energy that lies above 4 kHz: high for fricatives.
        float hiss = 0.0f;
    };

    /** @brief Mouth pose weights (0..1) for one step. */
    struct MouthPose
    {
        float jaw = 0.0f;
        float lips = 0.0f;
        float round_open = 0.0f;
        float round_closed = 0.0f;
        float spread = 0.0f;
        float teeth = 0.0f;
    };

    /**
     * @brief Analyzes mono @p samples (-1..1) at @p sample_rate, one VoiceFrame per @p step_ms.
     * @return The frames; empty for an empty recording or a non-positive rate.
     */
    [[nodiscard]] std::vector<VoiceFrame>
    analyze_voice(std::span<const float> samples, float sample_rate, std::uint32_t step_ms);

    /**
     * @brief Turns frames into mouth poses: noise floor and compression on loudness, vowel shape from the spectrum,
     *        attack/release smoothing, a look-ahead of @p lead_ms so the lips move before the sound, and a small lip
     *        opening kept inside speech so the mouth does not snap shut between syllables.
     */
    [[nodiscard]] std::vector<MouthPose>
    mouth_track(std::span<const VoiceFrame> frames, std::uint32_t step_ms, std::uint32_t lead_ms);
} // namespace SoundFramework::audio

#endif // SNDFW_AUDIO_VOICE_ANALYSIS_HPP
