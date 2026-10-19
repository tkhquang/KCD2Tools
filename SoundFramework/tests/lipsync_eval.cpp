/**
 * @file tests/lipsync_eval.cpp
 * @brief Offline evaluation tool: runs both lip sync modes on raw recordings so the training scripts can score them
 *        against the game's own facial animation.
 *
 * Usage: sndfw_lipsync_eval <model.bin> <list.txt> <out.bin> [--parts]
 * list.txt: one "<raw f32 mono path> <sample rate>" per line. out.bin (little endian), per entry: u32 basic frames,
 * f32[frames * 6] basic MouthPose rows (jaw, lips, round_open, round_closed, spread, teeth), u32 advanced frames,
 * u32 channels, f32[frames * channels], f32 advanced milliseconds.
 * --parts also times the in-game worker's schedule per recording (features, then lip_parts() in order) and reports
 * when each part is ready against when playback reaches it.
 */

#include "audio/lip_model.hpp"
#include "audio/voice_analysis.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

int main(int argc, char **argv)
{
    using namespace SoundFramework::audio;
    const bool parts = argc == 5 && std::string_view(argv[4]) == "--parts";
    if (argc != 4 && !parts)
    {
        std::printf("usage: sndfw_lipsync_eval <model.bin> <list.txt> <out.bin> [--parts]\n");
        return 2;
    }
    std::string error;
    const std::optional<LipModel> model = LipModel::load(argv[1], error);
    if (!model.has_value())
    {
        std::printf("model: %s\n", error.c_str());
        return 1;
    }
    std::ifstream list(argv[2]);
    std::ofstream out(argv[3], std::ios::binary);
    const auto put = [&](const void *data, std::size_t size)
    { out.write(static_cast<const char *>(data), static_cast<std::streamsize>(size)); };
    std::string line;
    double total_ms = 0.0;
    double total_audio_s = 0.0;
    std::size_t entries = 0;
    while (std::getline(list, line))
    {
        std::istringstream fields(line);
        std::string path;
        float rate = 0.0f;
        if (!(fields >> path >> rate))
        {
            continue;
        }
        std::ifstream raw(path, std::ios::binary);
        const std::vector<char> bytes{std::istreambuf_iterator<char>(raw), std::istreambuf_iterator<char>()};
        std::vector<float> samples(bytes.size() / sizeof(float));
        std::memcpy(samples.data(), bytes.data(), samples.size() * sizeof(float));

        const std::vector<VoiceFrame> frames = analyze_voice(samples, rate, lip_features::STEP_MS);
        const std::vector<MouthPose> basic = mouth_track(frames, lip_features::STEP_MS, 0);
        const auto basic_frames = static_cast<std::uint32_t>(basic.size());
        put(&basic_frames, sizeof(basic_frames));
        put(basic.data(), basic.size() * sizeof(MouthPose));

        const auto start = std::chrono::steady_clock::now();
        const PoseTrack track = model->predict(samples, rate);
        const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
        const auto advanced_frames = static_cast<std::uint32_t>(track.frames());
        const auto channels = static_cast<std::uint32_t>(track.channels);
        put(&advanced_frames, sizeof(advanced_frames));
        put(&channels, sizeof(channels));
        put(track.weights.data(), track.weights.size() * sizeof(float));
        put(&ms, sizeof(ms));
        if (parts)
        {
            // The worker's schedule: features for the whole recording, then each part in order.
            const auto begin = std::chrono::steady_clock::now();
            const std::vector<float> features = lip_features_of(samples, rate);
            const std::size_t steps = features.size() / lip_features::MELS;
            const std::vector<StepRange> ranges = lip_parts(0, steps);
            double first_ms = 0.0;
            double worst_late_ms = 0.0;
            std::size_t late = 0;
            for (const StepRange &range : ranges)
            {
                (void)model->run_range(features, steps, range.begin, range.end);
                const double ready =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
                const double due = static_cast<double>(range.begin) * lip_features::STEP_MS;
                first_ms = first_ms == 0.0 ? ready : first_ms;
                if (range.begin > 0 && ready > due)
                {
                    ++late;
                    worst_late_ms = std::max(worst_late_ms, ready - due);
                }
            }
            const double all_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            std::printf(
                "%.1f s: %zu part(s), first ready in %.1f ms, all in %.1f ms, %zu late (worst %.1f ms)\n",
                static_cast<double>(samples.size()) / rate,
                ranges.size(),
                first_ms,
                all_ms,
                late,
                worst_late_ms
            );
        }
        total_ms += ms;
        total_audio_s += static_cast<double>(samples.size()) / rate;
        ++entries;
    }
    std::printf(
        "%zu recording(s), %.1f s of audio, advanced %.1f ms total (%.2f ms per second of audio), %zu parameters\n",
        entries,
        total_audio_s,
        total_ms,
        total_audio_s > 0 ? total_ms / total_audio_s : 0.0,
        model->parameter_count()
    );
    return 0;
}
