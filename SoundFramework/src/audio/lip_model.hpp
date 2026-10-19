/**
 * @file audio/lip_model.hpp
 * @brief The learned lip model ([LipSync] Mode = ADVANCED): face pose weights predicted from a voice recording.
 *
 * A small dilated 1D convolution network trained on the game's own voice lines and their facial animations maps
 * normalized log-mel features (40 bands, 20 ms steps) to FA_POSE weights. The features match the training
 * extractor exactly, so both sides must change together. Pure code: no game or FMOD dependency.
 */
#ifndef SNDFW_AUDIO_LIP_MODEL_HPP
#define SNDFW_AUDIO_LIP_MODEL_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace SoundFramework::audio
{
    namespace lip_features
    {
        inline constexpr double WINDOW_S = 0.025;
        inline constexpr double HOP_S = 0.02;
        inline constexpr double FMIN_HZ = 60.0;
        inline constexpr double FMAX_HZ = 7600.0;
        /// Silence floor, relative to the recording's mean band energy.
        inline constexpr double FLOOR = 1e-6;
        inline constexpr std::size_t MELS = 40;
        inline constexpr std::uint32_t STEP_MS = 20;
    } // namespace lip_features

    /**
     * @brief Normalized log-mel features of mono @p samples: row-major frames x lip_features::MELS, frame i centred on
     *        i * hop, frames = 1 + samples / hop. Every band is normalized over the recording (mean 0, std 1).
     * @return Empty for an empty recording or a rate below 16 kHz.
     */
    [[nodiscard]] std::vector<float> lip_features_of(std::span<const float> samples, float sample_rate);

    /** @brief A range of steps, [begin, end). */
    struct StepRange
    {
        std::size_t begin = 0;
        std::size_t end = 0;
    };

    /**
     * @brief The parts a line's steps [@p begin, @p frames) are computed in, in order: 0.5 s first, so the mouth starts
     *        at once, then each part twice the last, so it is ready long before playback reaches it, up to 30 s, which
     *        keeps the network's buffers small enough to stay in the CPU cache.
     */
    [[nodiscard]] std::vector<StepRange> lip_parts(std::size_t begin, std::size_t frames);

    /** @brief Pose weights over time: row-major frames x channels, one row per step. */
    struct PoseTrack
    {
        std::uint32_t step_ms = lip_features::STEP_MS;
        std::size_t channels = 0;
        std::vector<float> weights;

        [[nodiscard]] std::size_t frames() const noexcept { return channels == 0 ? 0 : weights.size() / channels; }
    };

    class LipModel
    {
    public:
        /**
         * @brief Loads a model file written by the training script (magic SFLM, version 1, CRC32 trailer).
         * @return Nothing, with @p error set, when the file is missing, damaged or was made for other features.
         */
        [[nodiscard]] static std::optional<LipModel> load(const std::filesystem::path &path, std::string &error);

        /** @brief Pose weights (clamped to 0..1.2) for mono @p samples at @p sample_rate. */
        [[nodiscard]] PoseTrack predict(std::span<const float> samples, float sample_rate) const;

        /** @brief Pose weights for precomputed features (row-major @p frames x MELS). */
        [[nodiscard]] PoseTrack run(std::span<const float> features, std::size_t frames) const;

        /**
         * @brief Pose weights for steps [@p begin, @p end) of precomputed features (row-major @p frames x MELS).
         *        Runs the network over that range plus context_steps() on each side, so the rows equal the same rows
         *        of run() exactly.
         */
        [[nodiscard]] PoseTrack
        run_range(std::span<const float> features, std::size_t frames, std::size_t begin, std::size_t end) const;

        /** @brief Steps on each side of a step that its output depends on (the network's receptive radius). */
        [[nodiscard]] std::size_t context_steps() const noexcept;

        /** @brief The FA_POSE joint names each output channel drives (mirrored or duplicated poses share a channel). */
        [[nodiscard]] const std::vector<std::vector<std::string>> &channels() const noexcept { return m_channels; }

        [[nodiscard]] std::size_t parameter_count() const noexcept;

    private:
        struct Conv
        {
            std::size_t in = 0;
            std::size_t out = 0;
            std::size_t taps = 1;
            std::size_t dilation = 1;
            /// (out, in, taps), as PyTorch stores Conv1d weights.
            std::vector<float> weight;
            std::vector<float> bias;
        };

        struct Block
        {
            Conv conv;
            Conv mix;
        };

        /** @brief Applies @p conv to channel-major activations with @p margin zero columns on both sides. */
        static void apply(const Conv &conv, const float *input, float *output, std::size_t frames, std::size_t margin);

        std::size_t m_hidden = 0;
        Conv m_input;
        std::vector<Block> m_blocks;
        Conv m_output;
        std::vector<std::vector<std::string>> m_channels;
    };
} // namespace SoundFramework::audio

#endif // SNDFW_AUDIO_LIP_MODEL_HPP
