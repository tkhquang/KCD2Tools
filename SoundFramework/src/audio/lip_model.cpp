/**
 * @file audio/lip_model.cpp
 * @brief Log-mel features and the lip model's convolution network.
 */

#include "audio/lip_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <numbers>

namespace SoundFramework::audio
{
    namespace
    {
        constexpr std::uint32_t MODEL_VERSION = 1;
        constexpr std::size_t MAX_HIDDEN = 1024;
        constexpr std::size_t MAX_BLOCKS = 16;
        constexpr std::size_t MAX_DILATION = 64;
        constexpr std::size_t MAX_OUTPUTS = 128;
        constexpr float MAX_WEIGHT = 1.2f;

        [[nodiscard]] double hz_to_mel(double hz) noexcept
        {
            return 2595.0 * std::log10(1.0 + hz / 700.0);
        }

        [[nodiscard]] double mel_to_hz(double mel) noexcept
        {
            return 700.0 * (std::pow(10.0, mel / 2595.0) - 1.0);
        }

        /** @brief Iterative radix-2 FFT of one power-of-two size on split real and imaginary arrays. */
        class Fft
        {
        public:
            explicit Fft(std::size_t size) : m_size(size), m_reversed(size)
            {
                std::size_t bits = 0;
                while ((std::size_t{1} << bits) < size)
                {
                    ++bits;
                }
                for (std::size_t i = 0; i < size; ++i)
                {
                    std::size_t r = 0;
                    for (std::size_t b = 0; b < bits; ++b)
                    {
                        r |= ((i >> b) & 1u) << (bits - 1 - b);
                    }
                    m_reversed[i] = r;
                }
                // Each stage's twiddles stored contiguously, so the butterfly loop reads them in order.
                for (std::size_t length = 2; length <= size; length <<= 1)
                {
                    for (std::size_t j = 0; j < length / 2; ++j)
                    {
                        const double angle =
                            -2.0 * std::numbers::pi * static_cast<double>(j) / static_cast<double>(length);
                        m_twiddle_re.push_back(static_cast<float>(std::cos(angle)));
                        m_twiddle_im.push_back(static_cast<float>(std::sin(angle)));
                    }
                }
            }

            void run(float *__restrict re, float *__restrict im) const
            {
                for (std::size_t i = 0; i < m_size; ++i)
                {
                    if (i < m_reversed[i])
                    {
                        std::swap(re[i], re[m_reversed[i]]);
                        std::swap(im[i], im[m_reversed[i]]);
                    }
                }
                std::size_t stage = 0;
                for (std::size_t length = 2; length <= m_size; length <<= 1)
                {
                    const std::size_t half = length / 2;
                    const float *__restrict wr = m_twiddle_re.data() + stage;
                    const float *__restrict wi = m_twiddle_im.data() + stage;
                    for (std::size_t i = 0; i < m_size; i += length)
                    {
                        float *__restrict ar = re + i;
                        float *__restrict ai = im + i;
                        float *__restrict br = re + i + half;
                        float *__restrict bi = im + i + half;
                        for (std::size_t j = 0; j < half; ++j)
                        {
                            const float vr = br[j] * wr[j] - bi[j] * wi[j];
                            const float vi = br[j] * wi[j] + bi[j] * wr[j];
                            br[j] = ar[j] - vr;
                            bi[j] = ai[j] - vi;
                            ar[j] += vr;
                            ai[j] += vi;
                        }
                    }
                    stage += half;
                }
            }

        private:
            std::size_t m_size;
            std::vector<std::size_t> m_reversed;
            std::vector<float> m_twiddle_re;
            std::vector<float> m_twiddle_im;
        };

        /** @brief One triangular mel filter, stored over its non-zero bins. */
        struct MelFilter
        {
            std::size_t first = 0;
            std::vector<double> weights;
        };

        [[nodiscard]] std::vector<MelFilter> mel_filters(double rate, std::size_t fft)
        {
            const double low = hz_to_mel(lip_features::FMIN_HZ);
            const double high = hz_to_mel(lip_features::FMAX_HZ);
            std::vector<double> edges(lip_features::MELS + 2);
            for (std::size_t i = 0; i < edges.size(); ++i)
            {
                edges[i] = mel_to_hz(
                    low + (high - low) * static_cast<double>(i) / static_cast<double>(lip_features::MELS + 1)
                );
            }
            std::vector<MelFilter> filters(lip_features::MELS);
            const std::size_t bins = fft / 2 + 1;
            for (std::size_t m = 0; m < lip_features::MELS; ++m)
            {
                const double lo = edges[m];
                const double mid = edges[m + 1];
                const double hi = edges[m + 2];
                std::vector<double> row(bins, 0.0);
                for (std::size_t k = 0; k < bins; ++k)
                {
                    const double hz = static_cast<double>(k) * rate / static_cast<double>(fft);
                    row[k] = std::max(0.0, std::min((hz - lo) / (mid - lo), (hi - hz) / (hi - mid)));
                }
                const auto nonzero = [](double w) { return w > 0.0; };
                const auto first = std::ranges::find_if(row, nonzero);
                const auto last = std::ranges::find_if(row.rbegin(), row.rend(), nonzero).base();
                if (first < last)
                {
                    filters[m].first = static_cast<std::size_t>(first - row.begin());
                    filters[m].weights.assign(first, last);
                }
            }
            return filters;
        }

        [[nodiscard]] std::uint32_t crc32(std::span<const std::uint8_t> bytes) noexcept
        {
            std::uint32_t crc = 0xFFFFFFFFu;
            for (const std::uint8_t byte : bytes)
            {
                crc ^= byte;
                for (int bit = 0; bit < 8; ++bit)
                {
                    crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
                }
            }
            return ~crc;
        }

        /** @brief Bounds-checked little-endian reader over a model file. */
        class Reader
        {
        public:
            explicit Reader(std::span<const std::uint8_t> bytes) : m_bytes(bytes) {}

            template <typename T> [[nodiscard]] bool read(T &value) noexcept
            {
                if (m_bytes.size() - m_offset < sizeof(T))
                {
                    return false;
                }
                std::memcpy(&value, m_bytes.data() + m_offset, sizeof(T));
                m_offset += sizeof(T);
                return true;
            }

            [[nodiscard]] bool floats(std::vector<float> &out, std::size_t count)
            {
                if ((m_bytes.size() - m_offset) / sizeof(float) < count)
                {
                    return false;
                }
                out.resize(count);
                std::memcpy(out.data(), m_bytes.data() + m_offset, count * sizeof(float));
                m_offset += count * sizeof(float);
                return std::ranges::all_of(out, [](float v) { return std::isfinite(v); });
            }

            [[nodiscard]] bool text(std::string &out, std::size_t length)
            {
                if (m_bytes.size() - m_offset < length)
                {
                    return false;
                }
                out.assign(reinterpret_cast<const char *>(m_bytes.data() + m_offset), length);
                m_offset += length;
                return true;
            }

            [[nodiscard]] std::size_t offset() const noexcept { return m_offset; }

        private:
            std::span<const std::uint8_t> m_bytes;
            std::size_t m_offset = 0;
        };

        [[nodiscard]] bool near(float stored, double expected) noexcept
        {
            return std::abs(static_cast<double>(stored) - expected) <= 1e-6 * std::max(1.0, std::abs(expected));
        }
    } // namespace

    std::vector<float> lip_features_of(std::span<const float> samples, float sample_rate)
    {
        const double rate = sample_rate;
        if (samples.empty() || rate < 16000.0)
        {
            return {};
        }
        const auto window = static_cast<std::size_t>(rate * lip_features::WINDOW_S + 0.5);
        const auto hop = static_cast<std::size_t>(rate * lip_features::HOP_S + 0.5);
        std::size_t fft = 1;
        while (fft < window)
        {
            fft <<= 1;
        }
        const std::size_t frames = 1 + samples.size() / hop;
        const std::size_t half = window / 2;
        const std::size_t bins = fft / 2 + 1;
        std::vector<double> hann(window);
        for (std::size_t n = 0; n < window; ++n)
        {
            hann[n] =
                0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(n) / static_cast<double>(window));
        }
        const std::vector<MelFilter> filters = mel_filters(rate, fft);
        const Fft transform(fft);

        std::vector<float> hann_f(window);
        std::ranges::transform(hann, hann_f.begin(), [](double w) { return static_cast<float>(w); });
        std::vector<double> energy(frames * lip_features::MELS, 0.0);
        std::vector<float> re(fft);
        std::vector<float> im(fft);
        std::vector<double> power_a(bins);
        std::vector<double> power_b(bins);
        const auto load = [&](float *__restrict out, std::size_t frame)
        {
            // Sample n of frame f is samples[f * hop + n - half]; outside the recording it is silence.
            const std::ptrdiff_t first = static_cast<std::ptrdiff_t>(frame * hop) - static_cast<std::ptrdiff_t>(half);
            for (std::size_t n = 0; n < window; ++n)
            {
                const std::ptrdiff_t at = first + static_cast<std::ptrdiff_t>(n);
                out[n] = at >= 0 && at < static_cast<std::ptrdiff_t>(samples.size())
                             ? samples[static_cast<std::size_t>(at)] * hann_f[n]
                             : 0.0f;
            }
        };
        // Two real frames share one complex transform: frame a in the real part, frame b in the imaginary part.
        for (std::size_t frame = 0; frame < frames; frame += 2)
        {
            const bool pair = frame + 1 < frames;
            std::fill(re.begin(), re.end(), 0.0f);
            std::fill(im.begin(), im.end(), 0.0f);
            load(re.data(), frame);
            if (pair)
            {
                load(im.data(), frame + 1);
            }
            transform.run(re.data(), im.data());
            for (std::size_t k = 0; k < bins; ++k)
            {
                const std::size_t mirror = (fft - k) % fft;
                // a = (z + conj(z_mirror)) / 2, b = (z - conj(z_mirror)) / 2i.
                const double ar = 0.5 * (static_cast<double>(re[k]) + re[mirror]);
                const double ai = 0.5 * (static_cast<double>(im[k]) - im[mirror]);
                const double br = 0.5 * (static_cast<double>(im[k]) + im[mirror]);
                const double bi = -0.5 * (static_cast<double>(re[k]) - re[mirror]);
                power_a[k] = ar * ar + ai * ai;
                power_b[k] = br * br + bi * bi;
            }
            for (std::size_t m = 0; m < lip_features::MELS; ++m)
            {
                const MelFilter &filter = filters[m];
                double sum_a = 0.0;
                double sum_b = 0.0;
                for (std::size_t j = 0; j < filter.weights.size(); ++j)
                {
                    sum_a += power_a[filter.first + j] * filter.weights[j];
                    sum_b += power_b[filter.first + j] * filter.weights[j];
                }
                energy[frame * lip_features::MELS + m] = sum_a;
                if (pair)
                {
                    energy[(frame + 1) * lip_features::MELS + m] = sum_b;
                }
            }
        }

        double mean_energy = 0.0;
        for (const double e : energy)
        {
            mean_energy += e;
        }
        mean_energy /= static_cast<double>(energy.size());
        const double floor = lip_features::FLOOR * mean_energy + 1e-20;
        for (double &e : energy)
        {
            e = std::log(e + floor);
        }
        std::vector<float> out(energy.size());
        for (std::size_t m = 0; m < lip_features::MELS; ++m)
        {
            double mean = 0.0;
            for (std::size_t f = 0; f < frames; ++f)
            {
                mean += energy[f * lip_features::MELS + m];
            }
            mean /= static_cast<double>(frames);
            double variance = 0.0;
            for (std::size_t f = 0; f < frames; ++f)
            {
                const double d = energy[f * lip_features::MELS + m] - mean;
                variance += d * d;
            }
            const double scale = 1.0 / (std::sqrt(variance / static_cast<double>(frames)) + 1e-3);
            for (std::size_t f = 0; f < frames; ++f)
            {
                out[f * lip_features::MELS + m] =
                    static_cast<float>((energy[f * lip_features::MELS + m] - mean) * scale);
            }
        }
        return out;
    }

    std::optional<LipModel> LipModel::load(const std::filesystem::path &path, std::string &error)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            error = "file not found";
            return std::nullopt;
        }
        const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        if (bytes.size() < 8 + sizeof(std::uint32_t))
        {
            error = "file too short";
            return std::nullopt;
        }
        std::uint32_t stored_crc = 0;
        std::memcpy(&stored_crc, bytes.data() + bytes.size() - sizeof(stored_crc), sizeof(stored_crc));
        const std::span<const std::uint8_t> body(bytes.data(), bytes.size() - sizeof(stored_crc));
        if (crc32(body) != stored_crc)
        {
            error = "checksum mismatch (damaged file)";
            return std::nullopt;
        }
        Reader reader(body);
        char magic[4] = {};
        std::uint32_t version = 0;
        std::uint32_t mels = 0;
        std::uint32_t hidden = 0;
        std::uint32_t outputs = 0;
        std::uint32_t block_count = 0;
        float window_s = 0.0f;
        float hop_s = 0.0f;
        float fmin = 0.0f;
        float fmax = 0.0f;
        float floor = 0.0f;
        if (!reader.read(magic) || std::memcmp(magic, "SFLM", 4) != 0 || !reader.read(version) ||
            version != MODEL_VERSION)
        {
            error = "not a version 1 lip model";
            return std::nullopt;
        }
        if (!reader.read(mels) || !reader.read(hidden) || !reader.read(outputs) || !reader.read(block_count) ||
            !reader.read(window_s) || !reader.read(hop_s) || !reader.read(fmin) || !reader.read(fmax) ||
            !reader.read(floor))
        {
            error = "truncated header";
            return std::nullopt;
        }
        if (mels != lip_features::MELS || !near(window_s, lip_features::WINDOW_S) ||
            !near(hop_s, lip_features::HOP_S) || !near(fmin, lip_features::FMIN_HZ) ||
            !near(fmax, lip_features::FMAX_HZ) || !near(floor, lip_features::FLOOR))
        {
            error = "made for different audio features";
            return std::nullopt;
        }
        if (hidden == 0 || hidden > MAX_HIDDEN || outputs == 0 || outputs > MAX_OUTPUTS || block_count > MAX_BLOCKS)
        {
            error = "unsupported network size";
            return std::nullopt;
        }

        LipModel model;
        model.m_hidden = hidden;
        const auto conv = [&](Conv &c, std::size_t in, std::size_t out, std::size_t taps, std::size_t dilation)
        {
            c.in = in;
            c.out = out;
            c.taps = taps;
            c.dilation = dilation;
            return reader.floats(c.weight, out * in * taps) && reader.floats(c.bias, out);
        };
        if (!conv(model.m_input, mels, hidden, 3, 1))
        {
            error = "truncated input layer";
            return std::nullopt;
        }
        model.m_blocks.resize(block_count);
        for (Block &block : model.m_blocks)
        {
            std::uint32_t dilation = 0;
            if (!reader.read(dilation) || dilation == 0 || dilation > MAX_DILATION ||
                !conv(block.conv, hidden, hidden, 3, dilation) || !conv(block.mix, hidden, hidden, 1, 1))
            {
                error = "truncated or invalid block";
                return std::nullopt;
            }
        }
        if (!conv(model.m_output, hidden, outputs, 1, 1))
        {
            error = "truncated output layer";
            return std::nullopt;
        }
        model.m_channels.resize(outputs);
        for (std::vector<std::string> &channel : model.m_channels)
        {
            std::uint32_t names = 0;
            if (!reader.read(names) || names == 0 || names > 8)
            {
                error = "invalid channel table";
                return std::nullopt;
            }
            for (std::uint32_t n = 0; n < names; ++n)
            {
                std::uint16_t length = 0;
                std::string name;
                if (!reader.read(length) || length == 0 || length > 64 || !reader.text(name, length) ||
                    !name.starts_with("FA_POSE_"))
                {
                    error = "invalid channel name";
                    return std::nullopt;
                }
                channel.push_back(std::move(name));
            }
        }
        if (reader.offset() != body.size())
        {
            error = "trailing data";
            return std::nullopt;
        }
        return model;
    }

    void LipModel::apply(const Conv &conv, const float *input, float *output, std::size_t frames, std::size_t margin)
    {
        const std::size_t stride = frames + 2 * margin;
        const auto centre = static_cast<std::ptrdiff_t>(conv.taps / 2);
        for (std::size_t o = 0; o < conv.out; ++o)
        {
            float *__restrict const y = output + o * stride + margin;
            std::fill_n(y, frames, conv.bias[o]);
            for (std::size_t i = 0; i < conv.in; ++i)
            {
                const float *const x = input + i * stride + margin;
                for (std::size_t k = 0; k < conv.taps; ++k)
                {
                    const float w = conv.weight[(o * conv.in + i) * conv.taps + k];
                    const float *__restrict const xs =
                        x + (static_cast<std::ptrdiff_t>(k) - centre) * static_cast<std::ptrdiff_t>(conv.dilation);
                    for (std::size_t t = 0; t < frames; ++t)
                    {
                        y[t] += w * xs[t];
                    }
                }
            }
        }
    }

    PoseTrack LipModel::run(std::span<const float> features, std::size_t frames) const
    {
        PoseTrack track;
        track.channels = m_output.out;
        if (frames == 0 || features.size() < frames * lip_features::MELS)
        {
            return track;
        }
        std::size_t margin = 1;
        for (const Block &block : m_blocks)
        {
            margin = std::max(margin, block.conv.dilation);
        }
        const std::size_t stride = frames + 2 * margin;
        std::vector<float> input(lip_features::MELS * stride, 0.0f);
        for (std::size_t f = 0; f < frames; ++f)
        {
            for (std::size_t m = 0; m < lip_features::MELS; ++m)
            {
                input[m * stride + margin + f] = features[f * lip_features::MELS + m];
            }
        }
        std::vector<float> h(m_hidden * stride, 0.0f);
        std::vector<float> t(m_hidden * stride, 0.0f);
        std::vector<float> u(m_hidden * stride, 0.0f);
        const auto relu = [&](std::vector<float> &a)
        {
            for (std::size_t c = 0; c < m_hidden; ++c)
            {
                float *const row = a.data() + c * stride + margin;
                for (std::size_t f = 0; f < frames; ++f)
                {
                    row[f] = std::max(row[f], 0.0f);
                }
            }
        };
        apply(m_input, input.data(), h.data(), frames, margin);
        relu(h);
        for (const Block &block : m_blocks)
        {
            apply(block.conv, h.data(), t.data(), frames, margin);
            relu(t);
            apply(block.mix, t.data(), u.data(), frames, margin);
            for (std::size_t c = 0; c < m_hidden; ++c)
            {
                float *const a = h.data() + c * stride + margin;
                const float *const b = u.data() + c * stride + margin;
                for (std::size_t f = 0; f < frames; ++f)
                {
                    a[f] = std::max(a[f] + b[f], 0.0f);
                }
            }
        }
        std::vector<float> out(m_output.out * stride, 0.0f);
        apply(m_output, h.data(), out.data(), frames, margin);
        track.weights.resize(frames * m_output.out);
        for (std::size_t f = 0; f < frames; ++f)
        {
            for (std::size_t c = 0; c < m_output.out; ++c)
            {
                track.weights[f * m_output.out + c] = std::clamp(out[c * stride + margin + f], 0.0f, MAX_WEIGHT);
            }
        }
        return track;
    }

    std::vector<StepRange> lip_parts(std::size_t begin, std::size_t frames)
    {
        constexpr std::size_t first_part_steps = 25;
        constexpr std::size_t max_part_steps = 1500;
        std::vector<StepRange> parts;
        for (std::size_t size = first_part_steps; begin < frames; size = std::min(size * 2, max_part_steps))
        {
            const std::size_t end = std::min(frames, begin + size);
            parts.push_back(
                StepRange{
                    .begin = begin,
                    .end = end,
                }
            );
            begin = end;
        }
        return parts;
    }

    PoseTrack
    LipModel::run_range(std::span<const float> features, std::size_t frames, std::size_t begin, std::size_t end) const
    {
        PoseTrack track;
        track.channels = m_output.out;
        end = std::min(end, frames);
        if (begin >= end || features.size() < frames * lip_features::MELS)
        {
            return track;
        }
        // Every layer pads with zeros, so a window cut out of the recording is wrong only within context_steps() of
        // a cut; the rows kept are at least that far from both cuts, and each sums its inputs in the same order.
        const std::size_t context = context_steps();
        const std::size_t low = begin - std::min(begin, context);
        const std::size_t high = std::min(frames, end + context);
        PoseTrack window =
            run(features.subspan(low * lip_features::MELS, (high - low) * lip_features::MELS), high - low);
        const auto first = window.weights.begin() + static_cast<std::ptrdiff_t>((begin - low) * m_output.out);
        track.weights.assign(first, first + static_cast<std::ptrdiff_t>((end - begin) * m_output.out));
        return track;
    }

    std::size_t LipModel::context_steps() const noexcept
    {
        const auto radius = [](const Conv &conv) { return conv.taps / 2 * conv.dilation; };
        std::size_t steps = radius(m_input) + radius(m_output);
        for (const Block &block : m_blocks)
        {
            steps += radius(block.conv) + radius(block.mix);
        }
        return steps;
    }

    PoseTrack LipModel::predict(std::span<const float> samples, float sample_rate) const
    {
        const std::vector<float> features = lip_features_of(samples, sample_rate);
        return run(features, features.size() / lip_features::MELS);
    }

    std::size_t LipModel::parameter_count() const noexcept
    {
        std::size_t count = m_input.weight.size() + m_input.bias.size() + m_output.weight.size() + m_output.bias.size();
        for (const Block &block : m_blocks)
        {
            count +=
                block.conv.weight.size() + block.conv.bias.size() + block.mix.weight.size() + block.mix.bias.size();
        }
        return count;
    }
} // namespace SoundFramework::audio
