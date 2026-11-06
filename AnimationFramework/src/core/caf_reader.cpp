/**
 * @file core/caf_reader.cpp
 * @brief Compiled clip, clip pack and skeleton readers (see caf_reader.hpp).
 */

#include "core/caf_reader.hpp"

#include "core/text_util.hpp"
#include "core/zip_reader.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <numbers>
#include <unordered_map>

namespace AnimationFramework
{
    namespace
    {
        constexpr std::uint32_t CHUNK_FILE_VERSION = 0x746;
        constexpr std::size_t CHUNK_FILE_HEADER_SIZE = 16;
        constexpr std::size_t CHUNK_ENTRY_SIZE = 16;
        constexpr std::uint32_t CHUNK_CONTROLLER = 0x100D;
        constexpr std::uint32_t CHUNK_MOTION = 0x3002;
        constexpr std::uint32_t CHUNK_BONES = 0x2000;
        constexpr std::uint32_t CONTROLLER_829 = 0x829;
        constexpr std::uint32_t CONTROLLER_905 = 0x905;
        constexpr std::uint32_t MOTION_VERSION = 0x925;
        constexpr std::uint32_t BONES_VERSION = 0x800;
        constexpr std::size_t MOTION_SIZE = 132;
        constexpr std::size_t MOTION_START_OFFSET = 16;
        constexpr std::size_t MOTION_END_OFFSET = 20;
        constexpr std::size_t DESC_829_SIZE = 16;
        constexpr std::size_t DBA_INFO_SIZE = 20;
        constexpr std::uint32_t DBA_NO_TRACK = 0xFFFFFFFF;
        constexpr std::size_t DBA_TIME_FORMATS = 7;
        constexpr std::size_t DBA_VALUE_FORMATS = 9;
        constexpr std::size_t BONES_HEADER_SIZE = 32;
        constexpr std::size_t BONE_RECORD_SIZE = 584;
        constexpr std::size_t BONE_B2W_OFFSET = 264;
        constexpr std::size_t BONE_NAME_OFFSET = 312;
        constexpr std::size_t BONE_NAME_SIZE = 256;
        constexpr std::size_t BONE_PARENT_OFFSET = 572;
        // A small-tree quaternion keeps the three smallest components of a unit quaternion, each within +-1/sqrt(2),
        // as raw / scale - SMALL_TREE_RANGE, and the index of the dropped largest one, which decodes as positive. The
        // scales are QuatQuantization.h's MAX_15BITf, MAX_20BITf and MAX_21BITf.
        constexpr double SMALL_TREE_RANGE = 0.707106781186;
        constexpr double SMALL_TREE_15_SCALE = 23170.0;
        constexpr double SMALL_TREE_20_SCALE = 741454.0;
        constexpr double SMALL_TREE_21_SCALE = 1482909.0;
        // Formats 5, 6 and 8: 15/15/15, 20/20/20 and 21/21/20-bit components.
        constexpr std::array<double, 3> SMALL_TREE_48_SCALES{
            SMALL_TREE_15_SCALE,
            SMALL_TREE_15_SCALE,
            SMALL_TREE_15_SCALE,
        };
        constexpr std::array<double, 3> SMALL_TREE_60_SCALES{
            SMALL_TREE_20_SCALE,
            SMALL_TREE_20_SCALE,
            SMALL_TREE_20_SCALE,
        };
        constexpr std::array<double, 3> SMALL_TREE_64_SCALES{
            SMALL_TREE_21_SCALE,
            SMALL_TREE_21_SCALE,
            SMALL_TREE_20_SCALE,
        };
        // The components the three kept values fill, by the dropped component's index (x, y, z, w).
        constexpr std::array<std::array<std::size_t, 3>, 4> SMALL_TREE_KEPT{{
            {1, 2, 3},
            {0, 2, 3},
            {0, 1, 3},
            {0, 1, 2},
        }};

        /** @brief The value at @p at of @p data, or nothing when it runs past the end. */
        template <typename T>
        [[nodiscard]] std::optional<T> load(std::span<const std::byte> data, std::size_t at) noexcept
        {
            if (at > data.size() || data.size() - at < sizeof(T))
            {
                return std::nullopt;
            }
            T value{};
            std::memcpy(&value, data.data() + at, sizeof(T));
            return value;
        }

        /// @p bytes rounded up to a multiple of 4 when @p aligned.
        [[nodiscard]] std::size_t padded(std::size_t bytes, bool aligned) noexcept
        {
            return aligned ? (bytes + 3U) & ~std::size_t{3} : bytes;
        }

        /** @brief @p q at unit length, or the identity when it has no finite length (zero, NaN or infinite parts). */
        [[nodiscard]] Quat normalized(const Quat &q) noexcept
        {
            // The small-tree quantizer converts each component to an integer, which is undefined for a NaN.
            const double length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
            if (length > 0.0 && std::isfinite(length))
            {
                return Quat{
                    .x = q.x / length,
                    .y = q.y / length,
                    .z = q.z / length,
                    .w = q.w / length,
                };
            }
            return Quat{};
        }

        /** @brief A small-tree rotation from its three kept @p raw values, their @p scales and the @p dropped index. */
        [[nodiscard]] Quat small_tree(
            const std::array<std::uint64_t, 3> &raw,
            const std::array<double, 3> &scales,
            std::uint64_t dropped
        ) noexcept
        {
            std::array<double, 4> q{};
            double sum = 0.0;
            for (std::size_t k = 0; k < 3; ++k)
            {
                const double value = static_cast<double>(raw[k]) / scales[k] - SMALL_TREE_RANGE;
                q[SMALL_TREE_KEPT[dropped & 3U][k]] = value;
                sum += value * value;
            }
            q[dropped & 3U] = std::sqrt(std::max(0.0, 1.0 - sum));
            return Quat{
                .x = q[0],
                .y = q[1],
                .z = q[2],
                .w = q[3],
            };
        }

        /** @brief A rotation's small-tree fields: the kept components' raw values and the dropped component's index. */
        struct SmallTreeParts
        {
            std::array<std::uint64_t, 3> raw{};
            std::uint64_t dropped = 0;
        };

        /** @brief @p q's small-tree fields at @p scales: the largest component dropped, the rest rounded. */
        [[nodiscard]] SmallTreeParts small_tree_parts(const Quat &q, const std::array<double, 3> &scales) noexcept
        {
            const Quat n = normalized(q);
            std::array<double, 4> c{
                n.x,
                n.y,
                n.z,
                n.w,
            };
            std::size_t largest = 0;
            for (std::size_t i = 1; i < 4; ++i)
            {
                if (std::abs(c[i]) > std::abs(c[largest]))
                {
                    largest = i;
                }
            }
            // The decoder rebuilds the dropped component as positive, so q takes the sign that makes it so.
            if (c[largest] < 0.0)
            {
                for (double &v : c)
                {
                    v = -v;
                }
            }
            SmallTreeParts parts;
            parts.dropped = largest;
            for (std::size_t k = 0; k < 3; ++k)
            {
                const double value = c[SMALL_TREE_KEPT[largest][k]];
                const double scaled = std::round((value + SMALL_TREE_RANGE) * scales[k]);
                parts.raw[k] = static_cast<std::uint64_t>(std::clamp(scaled, 0.0, 2.0 * SMALL_TREE_RANGE * scales[k]));
            }
            return parts;
        }

        /// The byte size of one rotation key in @p format, or nothing for a format compiled clips do not use.
        [[nodiscard]] std::optional<std::size_t> rotation_size(std::uint32_t format) noexcept
        {
            switch (format)
            {
            case 0:
            case 1:
                return 16;
            case 5:
                return 6;
            case 6:
            case 8:
                return 8;
            default:
                return std::nullopt;
            }
        }

        /// The byte size of one position key in @p format, or nothing for a format compiled clips do not use.
        [[nodiscard]] std::optional<std::size_t> position_size(std::uint32_t format) noexcept
        {
            return format == 0 || format == 2 ? std::optional<std::size_t>{12} : std::nullopt;
        }

        /** @brief @p count rotation keys in @p format at @p at of @p data, or why they do not read. */
        [[nodiscard]] std::expected<std::vector<Quat>, std::string>
        decode_rotations(std::span<const std::byte> data, std::size_t at, std::uint32_t format, std::size_t count)
        {
            const std::optional<std::size_t> size = rotation_size(format);
            if (!size)
            {
                return std::unexpected{std::format("rotation format {} is not one compiled clips use", format)};
            }
            if (at > data.size() || (data.size() - at) / *size < count)
            {
                return std::unexpected{std::string{"rotation keys run past the chunk"}};
            }
            std::vector<Quat> out;
            out.reserve(count);
            for (std::size_t i = 0; i < count; ++i)
            {
                const std::size_t p = at + i * *size;
                if (format <= 1)
                {
                    out.push_back(
                        Quat{
                            .x = *load<float>(data, p),
                            .y = *load<float>(data, p + 4),
                            .z = *load<float>(data, p + 8),
                            .w = *load<float>(data, p + 12),
                        }
                    );
                }
                else if (format == 5)
                {
                    out.push_back(decode_small_tree_48({
                        *load<std::uint16_t>(data, p),
                        *load<std::uint16_t>(data, p + 2),
                        *load<std::uint16_t>(data, p + 4),
                    }));
                }
                else if (format == 6)
                {
                    // SmallTree64BitQuat: three 20-bit components and the index in the top 2 bits.
                    const std::uint64_t m1 = *load<std::uint32_t>(data, p);
                    const std::uint64_t m2 = *load<std::uint32_t>(data, p + 4);
                    out.push_back(small_tree(
                        {m1 & 0xFFFFFU, ((m1 >> 20U) + (m2 << 12U)) & 0xFFFFFU, (m2 >> 8U) & 0xFFFFFU},
                        SMALL_TREE_60_SCALES,
                        (m2 >> 30U) & 3U
                    ));
                }
                else
                {
                    out.push_back(decode_small_tree_64({
                        *load<std::uint32_t>(data, p),
                        *load<std::uint32_t>(data, p + 4),
                    }));
                }
            }
            return out;
        }

        /** @brief @p count position keys in @p format at @p at of @p data, or why they do not read. */
        [[nodiscard]] std::expected<std::vector<Vec3>, std::string>
        decode_positions(std::span<const std::byte> data, std::size_t at, std::uint32_t format, std::size_t count)
        {
            const std::optional<std::size_t> size = position_size(format);
            if (!size)
            {
                return std::unexpected{std::format("position format {} is not one compiled clips use", format)};
            }
            if (at > data.size() || (data.size() - at) / *size < count)
            {
                return std::unexpected{std::string{"position keys run past the chunk"}};
            }
            std::vector<Vec3> out;
            out.reserve(count);
            for (std::size_t i = 0; i < count; ++i)
            {
                const std::size_t p = at + i * *size;
                out.push_back(
                    Vec3{
                        .x = *load<float>(data, p),
                        .y = *load<float>(data, p + 4),
                        .z = *load<float>(data, p + 8),
                    }
                );
            }
            return out;
        }

        /** @brief Decoded key times and the bytes their track takes. */
        struct DecodedTimes
        {
            std::vector<double> frames;
            std::size_t bytes = 0;
        };

        /** @brief Key frames in @p format, read from the @p count elements the container records. */
        [[nodiscard]] std::expected<DecodedTimes, std::string>
        decode_times(std::span<const std::byte> data, std::size_t at, std::uint32_t format, std::size_t count)
        {
            DecodedTimes out;
            const auto element = [&](std::size_t i) -> std::optional<double>
            {
                switch (format % 3)
                {
                case 0:
                    return load<float>(data, at + 4 * i).transform([](float v) { return static_cast<double>(v); });
                case 1:
                    return load<std::uint16_t>(data, at + 2 * i)
                        .transform([](auto v) { return static_cast<double>(v); });
                default:
                    return load<std::uint8_t>(data, at + i).transform([](auto v) { return static_cast<double>(v); });
                }
            };
            const std::size_t element_size = format % 3 == 0 ? 4 : format % 3 == 1 ? 2 : 1;
            if (format <= 2)
            {
                out.frames.reserve(count);
                for (std::size_t i = 0; i < count; ++i)
                {
                    const std::optional<double> frame = element(i);
                    if (!frame)
                    {
                        return std::unexpected{std::string{"key times run past the chunk"}};
                    }
                    out.frames.push_back(*frame);
                }
                out.bytes = count * element_size;
                return out;
            }
            if (format <= 5)
            {
                const std::optional<double> start = element(0);
                const std::optional<double> stop = element(1);
                // The engine's key count, (u32)(stop - start) + 1, keyed at start + i. A count by index also ends on a
                // float range too far from 0 for a step of 1 to change it. A range longer than MAX_CLIP_SECONDS marks a
                // damaged file and must not size the key array.
                const double span = start && stop ? *stop - *start : -1.0;
                if (!(span >= 0.0) || span > MAX_CLIP_SECONDS * CLIP_FRAME_RATE)
                {
                    return std::unexpected{std::string{"a key time range does not read"}};
                }
                const auto keys = static_cast<std::size_t>(span) + 1;
                out.frames.reserve(keys);
                for (std::size_t i = 0; i < keys; ++i)
                {
                    out.frames.push_back(*start + static_cast<double>(i));
                }
                out.bytes = 2 * element_size;
                return out;
            }
            if (format == 6)
            {
                const std::optional<std::uint16_t> start = load<std::uint16_t>(data, at);
                if (!start || count < 3 || at + 2 * count > data.size())
                {
                    return std::unexpected{std::string{"a key time bitset does not read"}};
                }
                for (std::size_t word = 0; word + 3 < count; ++word)
                {
                    const std::uint16_t bits = *load<std::uint16_t>(data, at + 6 + 2 * word);
                    for (unsigned bit = 0; bit < 16; ++bit)
                    {
                        if ((bits >> bit) & 1U)
                        {
                            out.frames.push_back(static_cast<double>(*start) + static_cast<double>(word * 16 + bit));
                        }
                    }
                }
                out.bytes = 2 * count;
                return out;
            }
            return std::unexpected{std::format("key time format {} is not one compiled clips use", format)};
        }

        /** @brief One bone's keys from a 0x829 controller chunk, or why they do not read. */
        [[nodiscard]] std::expected<ClipTrack, std::string> read_829_track(std::span<const std::byte> data)
        {
            if (data.size() < DESC_829_SIZE)
            {
                return std::unexpected{std::string{"a controller chunk is too short"}};
            }
            ClipTrack track;
            track.controller = *load<std::uint32_t>(data, 0);
            const std::size_t rotations = *load<std::uint16_t>(data, 4);
            const std::size_t positions = *load<std::uint16_t>(data, 6);
            const std::uint32_t rotation_format = *load<std::uint8_t>(data, 8);
            const std::uint32_t rotation_time_format = *load<std::uint8_t>(data, 9);
            const std::uint32_t position_format = *load<std::uint8_t>(data, 10);
            const bool own_position_times = *load<std::uint8_t>(data, 11) != 0;
            const std::uint32_t position_time_format = *load<std::uint8_t>(data, 12);
            const bool aligned = *load<std::uint8_t>(data, 13) != 0;
            std::size_t p = DESC_829_SIZE;
            if (rotations != 0)
            {
                auto values = decode_rotations(data, p, rotation_format, rotations);
                if (!values)
                {
                    return std::unexpected{values.error()};
                }
                track.rotations = std::move(*values);
                p += padded(*rotation_size(rotation_format) * rotations, aligned);
                auto times = decode_times(data, p, rotation_time_format, rotations);
                if (!times)
                {
                    return std::unexpected{times.error()};
                }
                track.rotation_frames = std::move(times->frames);
                p += padded(times->bytes, aligned);
            }
            if (positions != 0)
            {
                auto values = decode_positions(data, p, position_format, positions);
                if (!values)
                {
                    return std::unexpected{values.error()};
                }
                track.positions = std::move(*values);
                p += padded(*position_size(position_format) * positions, aligned);
                if (!own_position_times)
                {
                    track.position_frames = track.rotation_frames;
                }
                else
                {
                    auto times = decode_times(data, p, position_time_format, positions);
                    if (!times)
                    {
                        return std::unexpected{times.error()};
                    }
                    track.position_frames = std::move(times->frames);
                }
            }
            return track;
        }

        /**
         * @brief One kind of track table of a clip pack: each track's element count, the track count of each format
         *        and each track's storage offset.
         */
        struct TrackTable
        {
            std::vector<std::uint32_t> counts;
            std::vector<std::uint32_t> formats;
            std::vector<std::int64_t> offsets;
        };

        /** @brief Where a clip pack's track sits, its format and its element count. */
        struct TrackRef
        {
            std::size_t at = 0;
            std::uint32_t format = 0;
            std::size_t count = 0;
        };

        /**
         * @brief Track @p index of @p table, its offset taken from @p storage, or nothing when it does not resolve.
         * @details The tracks are stored grouped by format, so a track's format is the group its index falls in.
         */
        [[nodiscard]] std::optional<TrackRef>
        track_ref(const TrackTable &table, std::uint32_t index, std::int64_t storage) noexcept
        {
            if (index >= table.counts.size() || index >= table.offsets.size())
            {
                return std::nullopt;
            }
            std::uint64_t total = 0;
            for (std::size_t format = 0; format < table.formats.size(); ++format)
            {
                total += table.formats[format];
                if (index < total)
                {
                    const std::int64_t at = storage + table.offsets[index];
                    if (at < 0)
                    {
                        return std::nullopt;
                    }
                    return TrackRef{
                        .at = static_cast<std::size_t>(at),
                        .format = static_cast<std::uint32_t>(format),
                        .count = table.counts[index],
                    };
                }
            }
            return std::nullopt;
        }

        /** @brief The rotation of a row-major 3x4 bone-to-world matrix. */
        [[nodiscard]] Quat quat_from_rows(const std::array<float, 12> &m) noexcept
        {
            const auto at = [&m](std::size_t row, std::size_t column)
            { return static_cast<double>(m[row * 4 + column]); };
            const double trace = at(0, 0) + at(1, 1) + at(2, 2);
            if (trace > 0.0)
            {
                const double s = std::sqrt(trace + 1.0) * 2.0;
                return Quat{
                    .x = (at(2, 1) - at(1, 2)) / s,
                    .y = (at(0, 2) - at(2, 0)) / s,
                    .z = (at(1, 0) - at(0, 1)) / s,
                    .w = 0.25 * s,
                };
            }
            std::size_t i = 0;
            if (at(1, 1) > at(i, i))
            {
                i = 1;
            }
            if (at(2, 2) > at(i, i))
            {
                i = 2;
            }
            const std::size_t j = (i + 1) % 3;
            const std::size_t k = (i + 2) % 3;
            const double s = std::sqrt(1.0 + at(i, i) - at(j, j) - at(k, k)) * 2.0;
            std::array<double, 4> q{};
            q[i] = 0.25 * s;
            q[j] = (at(j, i) + at(i, j)) / s;
            q[k] = (at(k, i) + at(i, k)) / s;
            q[3] = (at(k, j) - at(j, k)) / s;
            return Quat{
                .x = q[0],
                .y = q[1],
                .z = q[2],
                .w = q[3],
            };
        }

        /** @brief The key pair around @p frame and the blend between them. */
        struct KeySpan
        {
            std::size_t first = 0;
            double t = 0.0;
        };

        /** @brief The key pair of @p frames (two or more, ascending) around @p frame, the blend clamped to the ends. */
        [[nodiscard]] KeySpan key_span(std::span<const double> frames, double frame) noexcept
        {
            const auto upper = std::ranges::upper_bound(frames, frame);
            const std::size_t after = static_cast<std::size_t>(upper - frames.begin());
            const std::size_t first = std::min(after == 0 ? 0 : after - 1, frames.size() - 2);
            const double gap = std::max(frames[first + 1] - frames[first], 1.0e-9);
            return KeySpan{
                .first = first,
                .t = std::clamp((frame - frames[first]) / gap, 0.0, 1.0),
            };
        }
    } // namespace

    Quat multiply(const Quat &a, const Quat &b) noexcept
    {
        return Quat{
            .x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            .y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            .z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            .w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
        };
    }

    Quat conjugate(const Quat &q) noexcept
    {
        return Quat{
            .x = -q.x,
            .y = -q.y,
            .z = -q.z,
            .w = q.w,
        };
    }

    Vec3 rotate(const Quat &q, const Vec3 &v) noexcept
    {
        const Vec3 t{
            .x = 2.0 * (q.y * v.z - q.z * v.y),
            .y = 2.0 * (q.z * v.x - q.x * v.z),
            .z = 2.0 * (q.x * v.y - q.y * v.x),
        };
        return Vec3{
            .x = v.x + q.w * t.x + (q.y * t.z - q.z * t.y),
            .y = v.y + q.w * t.y + (q.z * t.x - q.x * t.z),
            .z = v.z + q.w * t.z + (q.x * t.y - q.y * t.x),
        };
    }

    double angle_between(const Quat &a, const Quat &b) noexcept
    {
        const Quat na = normalized(a);
        const Quat nb = normalized(b);
        const double dot = std::abs(na.x * nb.x + na.y * nb.y + na.z * nb.z + na.w * nb.w);
        return 2.0 * std::acos(std::min(dot, 1.0)) * 180.0 / std::numbers::pi;
    }

    double CafClip::seconds() const noexcept
    {
        return static_cast<double>(std::max<std::int64_t>(std::int64_t{end_frame} - start_frame, 1)) / CLIP_FRAME_RATE;
    }

    const ClipTrack *CafClip::track(std::uint32_t controller) const noexcept
    {
        const auto it = std::ranges::find(tracks, controller, &ClipTrack::controller);
        return it != tracks.end() ? &*it : nullptr;
    }

    std::expected<std::vector<ChunkView>, std::string> read_chunk_views(std::span<const std::byte> file)
    {
        if (file.size() < CHUNK_FILE_HEADER_SIZE || std::memcmp(file.data(), "CrCh", 4) != 0 ||
            *load<std::uint32_t>(file, 4) != CHUNK_FILE_VERSION)
        {
            return std::unexpected{std::string{"not a compiled chunk file (CrCh 0x746)"}};
        }
        const std::uint32_t count = *load<std::uint32_t>(file, 8);
        const std::uint32_t table = *load<std::uint32_t>(file, 12);
        if (table > file.size() || (file.size() - table) / CHUNK_ENTRY_SIZE < count)
        {
            return std::unexpected{std::string{"the chunk table runs past the file"}};
        }
        std::vector<ChunkView> chunks;
        chunks.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const std::size_t entry = table + std::size_t{i} * CHUNK_ENTRY_SIZE;
            const std::uint32_t size = *load<std::uint32_t>(file, entry + 8);
            const std::uint32_t offset = *load<std::uint32_t>(file, entry + 12);
            if (offset > file.size() || file.size() - offset < size)
            {
                return std::unexpected{std::format("chunk {} runs past the file", i)};
            }
            chunks.push_back(
                ChunkView{
                    .type = *load<std::uint16_t>(file, entry),
                    .version = *load<std::uint16_t>(file, entry + 2),
                    .id = *load<std::uint32_t>(file, entry + 4),
                    .data = file.subspan(offset, size),
                }
            );
        }
        return chunks;
    }

    std::expected<CafClip, std::string> read_caf_clip(std::span<const ChunkView> chunks)
    {
        CafClip clip;
        bool has_motion = false;
        for (const ChunkView &chunk : chunks)
        {
            if (chunk.type == CHUNK_MOTION && chunk.version == MOTION_VERSION && chunk.data.size() >= MOTION_SIZE)
            {
                clip.start_frame = *load<std::int32_t>(chunk.data, MOTION_START_OFFSET);
                clip.end_frame = *load<std::int32_t>(chunk.data, MOTION_END_OFFSET);
                has_motion = true;
            }
            else if (chunk.type == CHUNK_CONTROLLER && chunk.version == CONTROLLER_829)
            {
                auto track = read_829_track(chunk.data);
                if (!track)
                {
                    return std::unexpected{track.error()};
                }
                clip.tracks.push_back(std::move(*track));
            }
        }
        if (!has_motion)
        {
            return std::unexpected{std::string{"no motion parameters chunk: not a compiled clip"}};
        }
        return clip;
    }

    std::expected<CafClip, std::string> read_caf_file(std::span<const std::byte> file)
    {
        auto chunks = read_chunk_views(file);
        if (!chunks)
        {
            return std::unexpected{chunks.error()};
        }
        return read_caf_clip(*chunks);
    }

    std::string clip_stem(std::string_view path)
    {
        std::string stem = fold_path(path);
        if (const std::size_t slash = stem.rfind('/'); slash != std::string::npos)
        {
            stem.erase(0, slash + 1);
        }
        if (const std::size_t dot = stem.rfind('.'); dot != std::string::npos)
        {
            stem.erase(dot);
        }
        return stem;
    }

    std::expected<std::vector<CafClip>, std::string>
    read_dba_clips(std::span<const std::byte> file, std::span<const std::string> stems)
    {
        auto chunks = read_chunk_views(file);
        if (!chunks)
        {
            return std::unexpected{chunks.error()};
        }
        return read_dba_chunks(*chunks, stems);
    }

    std::expected<std::vector<CafClip>, std::string>
    read_dba_chunks(std::span<const ChunkView> chunks, std::span<const std::string> stems)
    {
        const auto chunk = std::ranges::find_if(
            chunks,
            [](const ChunkView &c) { return c.type == CHUNK_CONTROLLER && c.version == CONTROLLER_905; }
        );
        if (chunk == chunks.end())
        {
            return std::unexpected{std::string{"no clip pack chunk (0x905)"}};
        }
        // Every position here is relative to the chunk. Track offsets are relative to the track storage: the storage
        // follows the track tables, or, when the end marker is negative ("in place"), ends at the chunk's end.
        const std::span<const std::byte> data = chunk->data;
        std::size_t p = 0;
        const auto take_u32 = [&](std::uint32_t &out) -> bool
        {
            const std::optional<std::uint32_t> v = load<std::uint32_t>(data, p);
            p += 4;
            out = v.value_or(0);
            return v.has_value();
        };
        std::uint32_t num_pos = 0;
        std::uint32_t num_rot = 0;
        std::uint32_t num_time = 0;
        std::uint32_t num_anims = 0;
        if (!take_u32(num_pos) || !take_u32(num_rot) || !take_u32(num_time) || !take_u32(num_anims) ||
            num_anims > data.size())
        {
            return std::unexpected{std::string{"the clip pack header does not read"}};
        }
        // The track tables: a u16 element count and an i32 offset per track, the rotations' storage end marker, and
        // the u32 track counts of 7 time and twice 9 value formats. Their size is checked before the counts size any
        // allocation.
        constexpr std::size_t FORMAT_TABLES_SIZE = (DBA_TIME_FORMATS + 2 * DBA_VALUE_FORMATS) * 4;
        const std::uint64_t tables_size =
            (std::uint64_t{num_time} + num_pos + num_rot) * 6 + 4 + std::uint64_t{FORMAT_TABLES_SIZE};
        if (tables_size > data.size() - p)
        {
            return std::unexpected{std::string{"the clip pack's track tables run past the chunk"}};
        }
        const std::size_t base = p;
        const auto take_u16s = [&](std::size_t n) -> std::vector<std::uint32_t>
        {
            std::vector<std::uint32_t> values(n);
            for (auto &v : values)
            {
                v = load<std::uint16_t>(data, p).value_or(0);
                p += 2;
            }
            return values;
        };
        const auto take_u32s = [&](std::size_t n) -> std::vector<std::uint32_t>
        {
            std::vector<std::uint32_t> values(n);
            for (auto &v : values)
            {
                v = load<std::uint32_t>(data, p).value_or(0);
                p += 4;
            }
            return values;
        };
        const auto take_i32s = [&](std::size_t n) -> std::vector<std::int64_t>
        {
            std::vector<std::int64_t> values(n);
            for (auto &v : values)
            {
                v = load<std::int32_t>(data, p).value_or(0);
                p += 4;
            }
            return values;
        };
        TrackTable times;
        TrackTable positions;
        TrackTable rotations;
        times.counts = take_u16s(num_time);
        times.formats = take_u32s(DBA_TIME_FORMATS);
        positions.counts = take_u16s(num_pos);
        positions.formats = take_u32s(DBA_VALUE_FORMATS);
        rotations.counts = take_u16s(num_rot);
        rotations.formats = take_u32s(DBA_VALUE_FORMATS);
        times.offsets = take_i32s(num_time);
        positions.offsets = take_i32s(num_pos);
        rotations.offsets = take_i32s(std::size_t{num_rot} + 1);
        const std::int64_t storage_end = rotations.offsets.back();
        const bool in_place = storage_end < 0;
        std::uint32_t padding = 0;
        if (in_place)
        {
            take_u32(padding);
        }
        if ((p - base) % 4 != 0)
        {
            p += 4 - (p - base) % 4;
        }
        std::int64_t storage = 0;
        if (in_place)
        {
            storage = static_cast<std::int64_t>(data.size());
        }
        else
        {
            storage = static_cast<std::int64_t>(p);
            p += static_cast<std::size_t>((storage_end + 3) & ~std::int64_t{3});
        }
        p += padding;

        const std::size_t anim_block = p;
        std::vector<CafClip> clips;
        for (std::uint32_t a = 0; a < num_anims; ++a)
        {
            const std::optional<std::uint16_t> name_length = load<std::uint16_t>(data, p);
            if (!name_length || p + 2 + *name_length + MOTION_SIZE > data.size())
            {
                return std::unexpected{std::format("clip {} of the pack does not read", a)};
            }
            p += 2;
            std::string name{reinterpret_cast<const char *>(data.data() + p), *name_length};
            name.resize(std::strlen(name.c_str()));
            p += *name_length;
            const std::span<const std::byte> motion = data.subspan(p, MOTION_SIZE);
            p += MOTION_SIZE;
            const std::optional<std::uint16_t> foot_plants = load<std::uint16_t>(data, p);
            const std::optional<std::uint16_t> controller_count =
                foot_plants ? load<std::uint16_t>(data, p + 2 + *foot_plants) : std::nullopt;
            if (!controller_count)
            {
                return std::unexpected{std::format("clip {} of the pack does not read", a)};
            }
            p += 2 + *foot_plants + 2;
            const std::size_t controllers = *controller_count;
            std::int64_t info_at = static_cast<std::int64_t>(p - anim_block);
            if (in_place)
            {
                const std::optional<std::int32_t> offset = load<std::int32_t>(data, p);
                if (!offset)
                {
                    return std::unexpected{std::format("clip {} of the pack does not read", a)};
                }
                info_at = *offset;
                p += 4;
            }
            else
            {
                p += controllers * DBA_INFO_SIZE;
            }

            if (!stems.empty() && std::ranges::find(stems, clip_stem(name)) == stems.end())
            {
                continue;
            }

            CafClip clip;
            clip.name = std::move(name);
            clip.start_frame = *load<std::int32_t>(motion, MOTION_START_OFFSET);
            clip.end_frame = *load<std::int32_t>(motion, MOTION_END_OFFSET);
            const std::int64_t infos = static_cast<std::int64_t>(anim_block) + info_at;
            for (std::size_t c = 0; c < controllers; ++c)
            {
                const std::int64_t at = infos + static_cast<std::int64_t>(c * DBA_INFO_SIZE);
                if (at < 0)
                {
                    return std::unexpected{std::format("{}: a track record sits before the pack", clip.name)};
                }
                const auto field = [&](std::size_t k)
                { return load<std::uint32_t>(data, static_cast<std::size_t>(at) + 4 * k); };
                const std::optional<std::uint32_t> id = field(0);
                const std::optional<std::uint32_t> pos_time = field(1);
                const std::optional<std::uint32_t> pos_track = field(2);
                const std::optional<std::uint32_t> rot_time = field(3);
                const std::optional<std::uint32_t> rot_track = field(4);
                if (!id || !pos_time || !pos_track || !rot_time || !rot_track)
                {
                    return std::unexpected{std::format("{}: a track record runs past the pack", clip.name)};
                }
                ClipTrack track;
                track.controller = *id;
                if (*rot_track != DBA_NO_TRACK && *rot_time != DBA_NO_TRACK)
                {
                    const std::optional<TrackRef> time = track_ref(times, *rot_time, storage);
                    const std::optional<TrackRef> value = track_ref(rotations, *rot_track, storage);
                    if (!time || !value)
                    {
                        return std::unexpected{std::format("{}: a rotation track does not resolve", clip.name)};
                    }
                    auto frames = decode_times(data, time->at, time->format, time->count);
                    auto values = decode_rotations(data, value->at, value->format, value->count);
                    if (!frames || !values)
                    {
                        return std::unexpected{
                            std::format("{}: {}", clip.name, !frames ? frames.error() : values.error())
                        };
                    }
                    track.rotation_frames = std::move(frames->frames);
                    track.rotations = std::move(*values);
                }
                if (*pos_track != DBA_NO_TRACK && *pos_time != DBA_NO_TRACK)
                {
                    const std::optional<TrackRef> time = track_ref(times, *pos_time, storage);
                    const std::optional<TrackRef> value = track_ref(positions, *pos_track, storage);
                    if (!time || !value)
                    {
                        return std::unexpected{std::format("{}: a position track does not resolve", clip.name)};
                    }
                    auto frames = decode_times(data, time->at, time->format, time->count);
                    auto values = decode_positions(data, value->at, value->format, value->count);
                    if (!frames || !values)
                    {
                        return std::unexpected{
                            std::format("{}: {}", clip.name, !frames ? frames.error() : values.error())
                        };
                    }
                    track.position_frames = std::move(frames->frames);
                    track.positions = std::move(*values);
                }
                clip.tracks.push_back(std::move(track));
            }
            clips.push_back(std::move(clip));
        }
        return clips;
    }

    std::expected<std::vector<SkeletonBone>, std::string> read_skeleton_bones(std::span<const std::byte> bones_chunk)
    {
        if (bones_chunk.size() < BONES_HEADER_SIZE)
        {
            return std::unexpected{std::string{"the bones chunk is too short"}};
        }
        const std::size_t count = (bones_chunk.size() - BONES_HEADER_SIZE) / BONE_RECORD_SIZE;
        std::vector<SkeletonBone> bones;
        bones.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            const std::span<const std::byte> record =
                bones_chunk.subspan(BONES_HEADER_SIZE + i * BONE_RECORD_SIZE, BONE_RECORD_SIZE);
            std::array<float, 12> b2w{};
            std::memcpy(b2w.data(), record.data() + BONE_B2W_OFFSET, sizeof(b2w));
            std::string name{reinterpret_cast<const char *>(record.data() + BONE_NAME_OFFSET), BONE_NAME_SIZE};
            name.resize(std::strlen(name.c_str()));
            const std::int32_t parent_offset = *load<std::int32_t>(record, BONE_PARENT_OFFSET);
            const std::int64_t parent = parent_offset != 0 ? static_cast<std::int64_t>(i) + parent_offset : -1;
            if (parent_offset != 0 && (parent < 0 || parent >= static_cast<std::int64_t>(i)))
            {
                return std::unexpected{std::format("bone {} has a parent outside the bones before it", name)};
            }
            bones.push_back(
                SkeletonBone{
                    .name = std::move(name),
                    .controller = *load<std::uint32_t>(record, 0),
                    .parent = static_cast<int>(parent),
                    .rotation = normalized(quat_from_rows(b2w)),
                    .position = Vec3{.x = b2w[3], .y = b2w[7], .z = b2w[11]},
                }
            );
        }
        return bones;
    }

    std::expected<std::vector<SkeletonBone>, std::string> read_skeleton_file(std::span<const std::byte> file)
    {
        auto chunks = read_chunk_views(file);
        if (!chunks)
        {
            return std::unexpected{chunks.error()};
        }
        for (const ChunkView &chunk : *chunks)
        {
            if (chunk.type == CHUNK_BONES)
            {
                if (chunk.version != BONES_VERSION)
                {
                    return std::unexpected{std::format("bones chunk version {:#x}", chunk.version)};
                }
                return read_skeleton_bones(chunk.data);
            }
        }
        return std::unexpected{std::string{"no bones chunk"}};
    }

    std::uint32_t bone_controller_id(std::string_view name) noexcept
    {
        return zip_crc32({reinterpret_cast<const std::uint8_t *>(name.data()), name.size()});
    }

    std::array<std::uint16_t, 3> encode_small_tree_48(const Quat &q) noexcept
    {
        const SmallTreeParts parts = small_tree_parts(q, SMALL_TREE_48_SCALES);
        const std::uint64_t v = parts.raw[0] | (parts.raw[1] << 15U) | (parts.raw[2] << 30U) | (parts.dropped << 46U);
        return {
            static_cast<std::uint16_t>(v & 0xFFFFU),
            static_cast<std::uint16_t>((v >> 16U) & 0xFFFFU),
            static_cast<std::uint16_t>((v >> 32U) & 0xFFFFU),
        };
    }

    std::array<std::uint32_t, 2> encode_small_tree_64(const Quat &q) noexcept
    {
        const SmallTreeParts parts = small_tree_parts(q, SMALL_TREE_64_SCALES);
        const std::uint64_t v = parts.raw[0] | (parts.raw[1] << 21U) | (parts.raw[2] << 42U) | (parts.dropped << 62U);
        return {static_cast<std::uint32_t>(v & 0xFFFFFFFFU), static_cast<std::uint32_t>(v >> 32U)};
    }

    Quat decode_small_tree_48(const std::array<std::uint16_t, 3> &words) noexcept
    {
        const std::uint64_t m1 = words[0];
        const std::uint64_t m2 = words[1];
        const std::uint64_t m3 = words[2];
        return small_tree(
            {m1 & 0x7FFFU, ((m1 >> 15U) + (m2 << 1U)) & 0x7FFFU, ((m2 >> 14U) + (m3 << 2U)) & 0x7FFFU},
            SMALL_TREE_48_SCALES,
            (m3 >> 14U) & 3U
        );
    }

    Quat decode_small_tree_64(const std::array<std::uint32_t, 2> &words) noexcept
    {
        const std::uint64_t m1 = words[0];
        const std::uint64_t m2 = words[1];
        return small_tree(
            {m1 & 0x1FFFFFU, ((m1 >> 21U) + (m2 << 11U)) & 0x1FFFFFU, (m2 >> 10U) & 0xFFFFFU},
            SMALL_TREE_64_SCALES,
            (m2 >> 30U) & 3U
        );
    }

    std::optional<Quat> sample_rotation(const ClipTrack &track, double frame) noexcept
    {
        const std::size_t n = std::min(track.rotation_frames.size(), track.rotations.size());
        if (n == 0)
        {
            return std::nullopt;
        }
        if (n == 1)
        {
            return track.rotations.front();
        }
        const KeySpan span = key_span(std::span{track.rotation_frames}.first(n), frame);
        const Quat &a = track.rotations[span.first];
        Quat b = track.rotations[span.first + 1];
        if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0.0)
        {
            b = Quat{
                .x = -b.x,
                .y = -b.y,
                .z = -b.z,
                .w = -b.w,
            };
        }
        return normalized(
            Quat{
                .x = a.x + (b.x - a.x) * span.t,
                .y = a.y + (b.y - a.y) * span.t,
                .z = a.z + (b.z - a.z) * span.t,
                .w = a.w + (b.w - a.w) * span.t,
            }
        );
    }

    std::optional<Vec3> sample_position(const ClipTrack &track, double frame) noexcept
    {
        const std::size_t n = std::min(track.position_frames.size(), track.positions.size());
        if (n == 0)
        {
            return std::nullopt;
        }
        if (n == 1)
        {
            return track.positions.front();
        }
        const KeySpan span = key_span(std::span{track.position_frames}.first(n), frame);
        const Vec3 &a = track.positions[span.first];
        const Vec3 &b = track.positions[span.first + 1];
        return Vec3{
            .x = a.x + (b.x - a.x) * span.t,
            .y = a.y + (b.y - a.y) * span.t,
            .z = a.z + (b.z - a.z) * span.t,
        };
    }

    ClipPoser::ClipPoser(const std::vector<SkeletonBone> &bones, const CafClip &clip)
        : m_bones(bones), m_bind_rotations(bones.size()), m_bind_positions(bones.size()),
          m_tracks(bones.size(), nullptr)
    {
        std::unordered_map<std::uint32_t, const ClipTrack *> by_controller;
        for (const ClipTrack &track : clip.tracks)
        {
            by_controller.emplace(track.controller, &track);
        }
        for (std::size_t i = 0; i < bones.size(); ++i)
        {
            const SkeletonBone &bone = bones[i];
            if (bone.parent < 0)
            {
                m_bind_rotations[i] = bone.rotation;
                m_bind_positions[i] = bone.position;
            }
            else
            {
                const SkeletonBone &parent = bones[static_cast<std::size_t>(bone.parent)];
                const Quat inverse = conjugate(parent.rotation);
                m_bind_rotations[i] = multiply(inverse, bone.rotation);
                m_bind_positions[i] = rotate(
                    inverse,
                    Vec3{
                        .x = bone.position.x - parent.position.x,
                        .y = bone.position.y - parent.position.y,
                        .z = bone.position.z - parent.position.z,
                    }
                );
            }
            if (const auto it = by_controller.find(bone.controller); it != by_controller.end())
            {
                m_tracks[i] = it->second;
            }
        }
    }

    void ClipPoser::pose(double frame, std::vector<Quat> &rotations, std::vector<Vec3> &positions) const
    {
        rotations.resize(m_bones.size());
        positions.resize(m_bones.size());
        for (std::size_t i = 0; i < m_bones.size(); ++i)
        {
            Quat local_rotation = m_bind_rotations[i];
            Vec3 local_position = m_bind_positions[i];
            if (const ClipTrack *track = m_tracks[i]; track != nullptr)
            {
                local_rotation = sample_rotation(*track, frame).value_or(local_rotation);
                local_position = sample_position(*track, frame).value_or(local_position);
            }
            const int parent = m_bones[i].parent;
            if (parent < 0)
            {
                rotations[i] = local_rotation;
                positions[i] = local_position;
                continue;
            }
            const auto p = static_cast<std::size_t>(parent);
            rotations[i] = multiply(rotations[p], local_rotation);
            const Vec3 offset = rotate(rotations[p], local_position);
            positions[i] = Vec3{
                .x = positions[p].x + offset.x,
                .y = positions[p].y + offset.y,
                .z = positions[p].z + offset.z,
            };
        }
    }

    std::optional<std::size_t> ClipPoser::bone(std::string_view name) const noexcept
    {
        const auto it = std::ranges::find(m_bones, name, &SkeletonBone::name);
        return it != m_bones.end() ? std::optional{static_cast<std::size_t>(it - m_bones.begin())} : std::nullopt;
    }
} // namespace AnimationFramework
