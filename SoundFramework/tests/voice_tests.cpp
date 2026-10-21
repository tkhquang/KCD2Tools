/**
 * @file tests/voice_tests.cpp
 * @brief Unit tests of the voice analysis and mouth mapping on synthetic signals. Plain asserts; the exit code is the
 *        verdict.
 */

#include "audio/voice_analysis.hpp"
#include "face/mood.hpp"

#include <cmath>
#include <cstdio>
#include <numbers>
#include <random>
#include <vector>

namespace
{
    using namespace SoundFramework::audio;

    int s_failures = 0;

    void check(bool condition, const char *what, int line)
    {
        if (!condition)
        {
            std::printf("FAIL line %d: %s\n", line, what);
            ++s_failures;
        }
    }

#define CHECK(cond) check((cond), #cond, __LINE__)

    constexpr float RATE = 48000.0f;
    constexpr std::uint32_t STEP_MS = 20;

    [[nodiscard]] std::vector<float> tone(float hz, float seconds, float amplitude = 0.5f)
    {
        std::vector<float> out(static_cast<std::size_t>(RATE * seconds));
        for (std::size_t i = 0; i < out.size(); ++i)
        {
            out[i] = amplitude * std::sin(2.0f * std::numbers::pi_v<float> * hz * static_cast<float>(i) / RATE);
        }
        return out;
    }

    [[nodiscard]] std::vector<float> hiss(float seconds, float amplitude = 0.5f)
    {
        std::minstd_rand random(7);
        std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
        std::vector<float> out(static_cast<std::size_t>(RATE * seconds));
        float previous = 0.0f;
        for (float &sample : out)
        {
            // First difference of white noise: a rising spectrum, like a fricative.
            const float white = noise(random);
            sample = amplitude * 0.5f * (white - previous);
            previous = white;
        }
        return out;
    }

    [[nodiscard]] float
    mean_of(const std::vector<MouthPose> &track, float MouthPose::*field, std::size_t from, std::size_t to)
    {
        float sum = 0.0f;
        for (std::size_t i = from; i < to; ++i)
        {
            sum += track[i].*field;
        }
        return sum / static_cast<float>(to - from);
    }

    void test_features()
    {
        const std::vector<VoiceFrame> dark = analyze_voice(tone(320.0f, 0.5f), RATE, STEP_MS);
        CHECK(dark.size() == 25);
        CHECK(std::abs(dark[10].low_centroid - 320.0f) < 40.0f);
        CHECK(dark[10].centroid < 600.0f);
        CHECK(dark[10].hiss < 0.05f);
        CHECK(std::abs(dark[10].loudness - 1.0f) < 0.05f);

        const std::vector<VoiceFrame> bright = analyze_voice(tone(2600.0f, 0.5f), RATE, STEP_MS);
        CHECK(bright[10].centroid > 2200.0f);

        const std::vector<VoiceFrame> fricative = analyze_voice(hiss(0.5f), RATE, STEP_MS);
        CHECK(fricative[10].hiss > 0.5f);

        CHECK(analyze_voice({}, RATE, STEP_MS).empty());
    }

    void test_mouth()
    {
        // 0.3 s silence, 0.5 s open vowel (700 Hz), 0.3 s silence.
        std::vector<float> voice(static_cast<std::size_t>(RATE * 0.3f), 0.0f);
        const std::vector<float> vowel = tone(700.0f, 0.5f);
        voice.insert(voice.end(), vowel.begin(), vowel.end());
        voice.resize(voice.size() + static_cast<std::size_t>(RATE * 0.3f), 0.0f);
        const std::vector<VoiceFrame> frames = analyze_voice(voice, RATE, STEP_MS);
        const std::vector<MouthPose> still = mouth_track(frames, STEP_MS, 0);
        const std::vector<MouthPose> led = mouth_track(frames, STEP_MS, 60);
        CHECK(still.size() == frames.size());
        CHECK(mean_of(still, &MouthPose::jaw, 0, 10) < 0.05f);
        CHECK(mean_of(still, &MouthPose::jaw, 20, 35) > 0.6f);
        CHECK(mean_of(still, &MouthPose::spread, 20, 35) < 0.1f);
        // The lead moves the onset earlier: three steps before the sound the led track is already opening.
        CHECK(led[13].jaw > still[13].jaw + 0.2f);
        // The tail closes again.
        CHECK(still.back().jaw < 0.05f);

        const std::vector<MouthPose> rounded =
            mouth_track(analyze_voice(tone(320.0f, 0.5f), RATE, STEP_MS), STEP_MS, 0);
        CHECK(mean_of(rounded, &MouthPose::round_closed, 5, 20) > 0.4f);
        const std::vector<MouthPose> spread =
            mouth_track(analyze_voice(tone(2600.0f, 0.5f), RATE, STEP_MS), STEP_MS, 0);
        CHECK(mean_of(spread, &MouthPose::spread, 5, 20) > 0.6f);
        const std::vector<MouthPose> sibilant = mouth_track(analyze_voice(hiss(0.5f), RATE, STEP_MS), STEP_MS, 0);
        CHECK(mean_of(sibilant, &MouthPose::teeth, 5, 20) > 0.5f);
        CHECK(mean_of(sibilant, &MouthPose::jaw, 5, 20) < 0.5f);
    }
} // namespace

namespace
{
    void test_moods()
    {
        using namespace SoundFramework::face;
        std::string error;
        const auto poses = parse_poses("L_browsLowerer:0.5, fa_pose_c_kiss:1", error);
        CHECK(poses.has_value() && poses->size() == 2 && (*poses)[1].pose == "C_kiss");
        CHECK(parse_poses("C_kiss:1.5", error).has_value());
        CHECK(!parse_poses("C_kiss:2.5", error).has_value());
        CHECK(!parse_poses("C_kiss:-0.1", error).has_value());
        CHECK(!parse_poses("C_kiss", error).has_value());
        const auto merged = merge_poses(
            {{"C_kiss", 0.2f}, {"C_press", 0.3f}},
            std::vector<PoseWeight>{{"C_kiss", 0.9f}, {"C_suck", 0.1f}}
        );
        CHECK(merged.size() == 3 && merged[0].weight == 0.9f && merged[2].pose == "C_suck");
        CHECK(canonical_pose("fa_pose_l_browslowerer") == std::optional<std::string>("L_browsLowerer"));
        CHECK(!canonical_pose("L_nope").has_value());
        CHECK(region_of("L_browsLowerer") == Region::Upper);
        CHECK(region_of("C_eyesDown") == Region::Upper);
        CHECK(region_of("L_cornerPull") == Region::Mouth);
        CHECK(region_of("C_jawDrop") == Region::Mouth);
        for (const std::string_view pose : articulation_poses())
        {
            CHECK(canonical_pose(pose) == std::optional<std::string>(pose));
            CHECK(region_of(pose) == Region::Mouth);
        }
        CHECK(std::ranges::find(articulation_poses(), "L_cornerPull") == articulation_poses().end());
    }
} // namespace

int main()
{
    test_moods();
    test_features();
    test_mouth();
    if (s_failures != 0)
    {
        std::printf("%d check(s) failed\n", s_failures);
        return 1;
    }
    std::printf("all voice tests passed\n");
    return 0;
}
