/**
 * @file tests/lipmodel_tests.cpp
 * @brief Parity of the C++ lip model with the Python training code (features and network), plus file validation.
 *
 * Fixtures (tests/data) hold a seeded random model, synthetic recordings and the training code's outputs for them,
 * so no game data is involved.
 */

#include "audio/lip_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
    using namespace SoundFramework::audio;
    namespace fs = std::filesystem;

    int s_failures = 0;

    void check(bool condition, const char *what, int line)
    {
        if (!condition)
        {
            ++s_failures;
            std::printf("FAILED line %d: %s\n", line, what);
        }
    }

#define CHECK(cond) check((cond), #cond, __LINE__)

    [[nodiscard]] std::vector<std::uint8_t> read_file(const fs::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    class Cursor
    {
    public:
        explicit Cursor(const std::vector<std::uint8_t> &bytes) : m_bytes(bytes) {}

        template <typename T> T next()
        {
            T value{};
            std::memcpy(&value, m_bytes.data() + m_offset, sizeof(T));
            m_offset += sizeof(T);
            return value;
        }

        std::vector<float> floats(std::size_t count)
        {
            std::vector<float> out(count);
            std::memcpy(out.data(), m_bytes.data() + m_offset, count * sizeof(float));
            m_offset += count * sizeof(float);
            return out;
        }

    private:
        const std::vector<std::uint8_t> &m_bytes;
        std::size_t m_offset = 0;
    };

    [[nodiscard]] float max_diff(const std::vector<float> &a, const std::vector<float> &b)
    {
        if (a.size() != b.size())
        {
            return INFINITY;
        }
        float worst = 0.0f;
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            worst = std::max(worst, std::abs(a[i] - b[i]));
        }
        return worst;
    }

    void test_parity(const fs::path &data)
    {
        std::string error;
        const std::optional<LipModel> model = LipModel::load(data / "lip_parity_model.bin", error);
        CHECK(model.has_value());
        if (!model.has_value())
        {
            std::printf("load: %s\n", error.c_str());
            return;
        }
        CHECK(model->channels().size() == 3 && model->channels()[1].size() == 2);
        CHECK(model->channels()[1][1] == "FA_POSE_R_cornerPull");

        const std::vector<std::uint8_t> blob = read_file(data / "lip_parity.bin");
        CHECK(!blob.empty());
        if (blob.empty())
        {
            return;
        }
        Cursor cursor(blob);
        const auto cases = cursor.next<std::uint32_t>();
        for (std::uint32_t c = 0; c < cases; ++c)
        {
            const auto rate = cursor.next<float>();
            const auto count = cursor.next<std::uint32_t>();
            const std::vector<float> samples = cursor.floats(count);
            const auto frames = cursor.next<std::uint32_t>();
            const auto mels = cursor.next<std::uint32_t>();
            const std::vector<float> expected_features = cursor.floats(static_cast<std::size_t>(frames) * mels);
            const auto channels = cursor.next<std::uint32_t>();
            const std::vector<float> expected_out = cursor.floats(static_cast<std::size_t>(frames) * channels);

            const std::vector<float> features = lip_features_of(samples, rate);
            const float feature_error = max_diff(features, expected_features);
            CHECK(features.size() == expected_features.size());
            CHECK(feature_error < 2e-3f);

            const PoseTrack track = model->predict(samples, rate);
            const float output_error = max_diff(track.weights, expected_out);
            CHECK(track.frames() == frames && track.channels == channels);
            CHECK(output_error < 2e-3f);
            std::printf(
                "case %u (%.0f Hz, %u frames): feature error %.2e, output error %.2e\n",
                c,
                rate,
                frames,
                feature_error,
                output_error
            );
        }
    }

    /** @brief run_range() parts must equal the same rows of one run() exactly, wherever the cuts fall. */
    void test_parts(const fs::path &model_file)
    {
        std::string error;
        const std::optional<LipModel> model = LipModel::load(model_file, error);
        CHECK(model.has_value());
        if (!model.has_value())
        {
            std::printf("load %s: %s\n", model_file.string().c_str(), error.c_str());
            return;
        }
        // Six seconds of a gliding, pulsing tone pair: deterministic and far longer than the context.
        constexpr float rate = 48000.0f;
        std::vector<float> samples(static_cast<std::size_t>(rate * 6.0f));
        for (std::size_t i = 0; i < samples.size(); ++i)
        {
            const float t = static_cast<float>(i) / rate;
            const float pulse = 0.5f + 0.5f * std::sin(6.0f * t);
            samples[i] = pulse * (0.4f * std::sin(2000.0f * t + 300.0f * t * t) + 0.2f * std::sin(5200.0f * t));
        }
        const std::vector<float> features = lip_features_of(samples, rate);
        const std::size_t frames = features.size() / lip_features::MELS;
        const PoseTrack whole = model->run(features, frames);
        CHECK(whole.frames() == frames && frames > 2 * model->context_steps());

        for (const std::vector<std::size_t> &cuts :
             {std::vector<std::size_t>{0, 25, frames}, std::vector<std::size_t>{0, 1, 7, 100, 233, frames - 3, frames}})
        {
            std::vector<float> joined;
            for (std::size_t i = 0; i + 1 < cuts.size(); ++i)
            {
                const PoseTrack part = model->run_range(features, frames, cuts[i], cuts[i + 1]);
                CHECK(part.frames() == cuts[i + 1] - cuts[i] && part.channels == whole.channels);
                joined.insert(joined.end(), part.weights.begin(), part.weights.end());
            }
            CHECK(joined == whole.weights);
        }
        CHECK(model->run_range(features, frames, 10, 10).frames() == 0);
        CHECK(model->run_range(features, frames, frames - 2, frames + 50).frames() == 2);
        std::printf(
            "parts of %s: %zu frames, context %zu step(s), identical to one run\n",
            model_file.filename().string().c_str(),
            frames,
            model->context_steps()
        );
    }

    void test_validation(const fs::path &data)
    {
        std::string error;
        CHECK(!LipModel::load(data / "missing.bin", error).has_value() && error == "file not found");

        std::vector<std::uint8_t> bytes = read_file(data / "lip_parity_model.bin");
        bytes[bytes.size() / 2] ^= 0x40;
        const fs::path damaged = fs::temp_directory_path() / "sndfw_lip_damaged.bin";
        std::ofstream(damaged, std::ios::binary)
            .write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        CHECK(!LipModel::load(damaged, error).has_value() && error.starts_with("checksum"));
        fs::remove(damaged);

        // The worker's parts: 0.5 s first, then doubling up to 30 s, back to back over the whole range.
        const std::vector<StepRange> parts = lip_parts(3, 10000);
        CHECK(parts.size() > 2 && parts.front().begin == 3 && parts.front().end == 28 && parts.back().end == 10000);
        CHECK(parts[1].end - parts[1].begin == 50);
        for (std::size_t i = 1; i < parts.size(); ++i)
        {
            CHECK(parts[i].begin == parts[i - 1].end && parts[i].end - parts[i].begin <= 1500);
        }
        CHECK(lip_parts(5, 5).empty());

        CHECK(lip_features_of({}, 48000.0f).empty());
        const std::vector<float> quiet(8000, 0.0f);
        CHECK(lip_features_of(quiet, 8000.0f).empty());
        const std::vector<float> silence = lip_features_of(quiet, 16000.0f);
        CHECK(silence.size() == (1 + 8000 / 320) * lip_features::MELS);
        CHECK(std::ranges::all_of(silence, [](float v) { return std::isfinite(v); }));
    }
} // namespace

int main(int argc, char **argv)
{
    const fs::path data = argc > 1 ? fs::path(argv[1]) : fs::path(SNDFW_TEST_DATA);
    test_parity(data);
    test_parts(data / "lip_parity_model.bin");
    // The shipped model, when the tests run from the source tree.
    const fs::path shipped =
        data.parent_path().parent_path() / "build" / "template" / "KCD2_SoundFramework_lipmodel.bin";
    if (fs::exists(shipped))
    {
        test_parts(shipped);
    }
    test_validation(data);
    if (s_failures != 0)
    {
        std::printf("%d check(s) failed\n", s_failures);
        return 1;
    }
    std::printf("all lip model tests passed\n");
    return 0;
}
