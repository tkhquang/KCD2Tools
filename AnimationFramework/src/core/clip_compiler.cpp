/**
 * @file core/clip_compiler.cpp
 * @brief Compiles an animation into a game-ready `.caf` (see clip_compiler.hpp).
 */

#include "core/clip_compiler.hpp"
#include "core/clip_check.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <numbers>
#include <unordered_map>
#include <unordered_set>

namespace AnimationFramework
{
    namespace
    {
        constexpr std::uint32_t CHUNK_FILE_VERSION = 0x746;
        constexpr std::uint16_t CHUNK_MOTION = 0x3002;
        constexpr std::uint16_t MOTION_VERSION = 0x925;
        constexpr std::uint16_t CHUNK_CONTROLLER = 0x100D;
        constexpr std::uint16_t CONTROLLER_829 = 0x829;
        constexpr std::uint16_t CONTROLLER_827 = 0x827;
        constexpr std::uint32_t CA_ASSET_ADDITIVE = 0x001;
        constexpr std::uint32_t CA_ASSET_CYCLE = 0x002;
        constexpr float NO_FOOT_PLANT = -10000.0f;
        constexpr std::uint8_t ROTATION_FLOAT = 1;
        constexpr std::uint8_t ROTATION_SMALL_TREE_48 = 5;
        constexpr std::uint8_t ROTATION_SMALL_TREE_64 = 8;
        constexpr std::uint8_t POSITION_FLOAT = 2;
        constexpr std::uint8_t TIMES_F32 = 0;
        constexpr std::uint8_t TIMES_U16 = 1;
        // The intermediate format's clock: 4800 ticks a second, 160 a frame at CLIP_FRAME_RATE.
        constexpr double TICKS_PER_SECOND = 4800.0;
        constexpr std::size_t INTERMEDIATE_KEY_SIZE = 28;
        constexpr double CENTIMETRES_PER_METRE = 100.0;
        // The Resource Compiler's root tolerance (AnimationLoader.cpp: 0.5 mm, 0.1 degrees), stored at 64 bits.
        constexpr double ROOT_POSITION_TOLERANCE = 0.0005;
        constexpr double ROOT_ROTATION_TOLERANCE = 0.1 * std::numbers::pi / 180.0;
        // DetectCycleAnimations: first and last rotation within 0.1 per component (IsQuatEquivalent_dot).
        constexpr double CYCLE_COMPONENT_TOLERANCE = 0.1;
        // A channel counts as unchanged within these (DropUnchangedAtDefault).
        constexpr double STATIC_ROTATION_TOLERANCE = 1.0e-3;
        constexpr double STATIC_POSITION_FLOOR = 1.0e-4;
        constexpr double STATIC_POSITION_CEILING = 0.02;
        constexpr double STATIC_POSITION_EPSILON = 1.0e-3;

        /// The 4D dot product of @p a and @p b.
        [[nodiscard]] double dot(const Quat &a, const Quat &b) noexcept
        {
            return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        }

        /** @brief @p q at unit length, or the identity when it has no finite length (zero, NaN or infinite parts). */
        [[nodiscard]] Quat normalized(const Quat &q) noexcept
        {
            const double n = std::sqrt(dot(q, q));
            if (n > 0.0 && std::isfinite(n))
            {
                return Quat{
                    .x = q.x / n,
                    .y = q.y / n,
                    .z = q.z / n,
                    .w = q.w / n,
                };
            }
            return Quat{};
        }

        /// The same rotation with every component negated.
        [[nodiscard]] Quat negated(const Quat &q) noexcept
        {
            return Quat{
                .x = -q.x,
                .y = -q.y,
                .z = -q.z,
                .w = -q.w,
            };
        }

        /** @brief Normalized linear interpolation on the shorter arc. */
        [[nodiscard]] Quat nlerp(const Quat &a, Quat b, double t) noexcept
        {
            if (dot(a, b) < 0.0)
            {
                b = negated(b);
            }
            return normalized(
                Quat{
                    .x = a.x + (b.x - a.x) * t,
                    .y = a.y + (b.y - a.y) * t,
                    .z = a.z + (b.z - a.z) * t,
                    .w = a.w + (b.w - a.w) * t,
                }
            );
        }

        /// The angle between two rotations, in radians (0 to pi).
        [[nodiscard]] double angle_radians(const Quat &a, const Quat &b) noexcept
        {
            return 2.0 * std::acos(std::min(1.0, std::abs(dot(normalized(a), normalized(b)))));
        }

        /// @p a minus @p b.
        [[nodiscard]] Vec3 subtract(const Vec3 &a, const Vec3 &b) noexcept
        {
            return Vec3{
                .x = a.x - b.x,
                .y = a.y - b.y,
                .z = a.z - b.z,
            };
        }

        /// The point a fraction @p t of the way from @p a to @p b.
        [[nodiscard]] Vec3 lerp(const Vec3 &a, const Vec3 &b, double t) noexcept
        {
            return Vec3{
                .x = a.x + (b.x - a.x) * t,
                .y = a.y + (b.y - a.y) * t,
                .z = a.z + (b.z - a.z) * t,
            };
        }

        /// The Euclidean length of @p v.
        [[nodiscard]] double length(const Vec3 &v) noexcept
        {
            return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        }

        /** @brief exp of a rotation log (half angle times axis), as CryEngine's Quat::exp. */
        [[nodiscard]] Quat quat_exp(const Vec3 &v) noexcept
        {
            const double n = length(v);
            if (n <= 0.0)
            {
                return Quat{};
            }
            const double s = std::sin(n) / n;
            return Quat{
                .x = v.x * s,
                .y = v.y * s,
                .z = v.z * s,
                .w = std::cos(n),
            };
        }

        /** @brief A track value at a fractional frame, clamped to the ends. @p values must not be empty. */
        template <typename T, typename Mix>
        [[nodiscard]] T sample(const std::vector<T> &values, double frame, Mix mix) noexcept
        {
            if (frame <= 0.0)
            {
                return values.front();
            }
            const double last = static_cast<double>(values.size() - 1);
            if (frame >= last)
            {
                return values.back();
            }
            const auto i = static_cast<std::size_t>(frame);
            return mix(values[i], values[i + 1], frame - static_cast<double>(i));
        }

        /** @brief @p q as the engine reads it back from small-tree rotation format @p format (5 or 8). */
        [[nodiscard]] Quat quantized(const Quat &q, std::uint8_t format) noexcept
        {
            return format == ROTATION_SMALL_TREE_48 ? decode_small_tree_48(encode_small_tree_48(q))
                                                    : decode_small_tree_64(encode_small_tree_64(q));
        }

        /** @brief A little-endian byte buffer. */
        class Bytes
        {
        public:
            /// Appends @p value.
            template <typename T> void put(T value)
            {
                const std::size_t at = m_data.size();
                m_data.resize(at + sizeof(T));
                std::memcpy(m_data.data() + at, &value, sizeof(T));
            }

            /// Zero-pads to a multiple of 4 bytes.
            void pad4() { m_data.resize((m_data.size() + 3U) & ~std::size_t{3}); }

            /// Appends @p bytes.
            void append(std::span<const std::byte> bytes) { m_data.insert(m_data.end(), bytes.begin(), bytes.end()); }

            /// The bytes, moved out.
            [[nodiscard]] std::vector<std::byte> take() noexcept { return std::move(m_data); }

        private:
            std::vector<std::byte> m_data;
        };

        /** @brief One chunk of a chunk file to write. */
        struct Chunk
        {
            std::uint16_t type = 0;
            std::uint16_t version = 0;
            std::vector<std::byte> data;
        };

        /** @brief A CrCh 0x746 chunk file: ids from 1, each chunk 4-byte aligned after the table. */
        [[nodiscard]] std::vector<std::byte> chunk_file(const std::vector<Chunk> &chunks)
        {
            constexpr std::uint32_t HEADER = 16;
            constexpr std::uint32_t ENTRY = 16;
            Bytes out;
            out.append(std::as_bytes(std::span{"CrCh", 4}));
            out.put<std::uint32_t>(CHUNK_FILE_VERSION);
            out.put<std::uint32_t>(static_cast<std::uint32_t>(chunks.size()));
            out.put<std::uint32_t>(HEADER);
            std::uint32_t offset = HEADER + ENTRY * static_cast<std::uint32_t>(chunks.size());
            std::uint32_t id = 1;
            for (const Chunk &chunk : chunks)
            {
                out.put<std::uint16_t>(chunk.type);
                out.put<std::uint16_t>(chunk.version);
                out.put<std::uint32_t>(id++);
                out.put<std::uint32_t>(static_cast<std::uint32_t>(chunk.data.size()));
                out.put<std::uint32_t>(offset);
                offset += (static_cast<std::uint32_t>(chunk.data.size()) + 3U) & ~3U;
            }
            for (const Chunk &chunk : chunks)
            {
                out.append(chunk.data);
                out.pad4();
            }
            return out.take();
        }

        /**
         * @brief The frames to keep so interpolating between kept keys stays within @p tolerance of every frame.
         * @param fits Whether the segment from kept frame a to frame b reproduces every frame between them.
         * @details Greedy: from each kept frame, the step doubles while the segment fits, then bisects between the
         *          last segment that fits and the first that does not. Every segment taken is checked in full, and the
         *          first and last frame are kept.
         */
        template <typename Fits> [[nodiscard]] std::vector<std::size_t> reduce(std::size_t frames, Fits fits)
        {
            std::vector<std::size_t> kept{0};
            if (frames < 2)
            {
                return kept;
            }
            std::size_t from = 0;
            while (from < frames - 1)
            {
                std::size_t good = from + 1;
                std::size_t step = 1;
                std::size_t bad = frames;
                while (true)
                {
                    const std::size_t next = std::min(from + 1 + step, frames - 1);
                    if (!fits(from, next))
                    {
                        bad = next;
                        break;
                    }
                    good = next;
                    if (next == frames - 1)
                    {
                        break;
                    }
                    step *= 2;
                }
                while (bad != frames && bad - good > 1)
                {
                    const std::size_t middle = good + (bad - good) / 2;
                    if (fits(from, middle))
                    {
                        good = middle;
                    }
                    else
                    {
                        bad = middle;
                    }
                }
                kept.push_back(good);
                from = good;
            }
            return kept;
        }

        /** @brief The rotation keys to keep: nlerp between the @p stored (quantized) keys stays within @p tolerance. */
        [[nodiscard]] std::vector<std::size_t>
        reduce_rotations(const std::vector<Quat> &values, const std::vector<Quat> &stored, double tolerance)
        {
            return reduce(
                values.size(),
                [&](std::size_t a, std::size_t b) -> bool
                {
                    const double span = static_cast<double>(b - a);
                    for (std::size_t k = a + 1; k < b; ++k)
                    {
                        const Quat mid = nlerp(stored[a], stored[b], static_cast<double>(k - a) / span);
                        if (angle_radians(mid, values[k]) > tolerance)
                        {
                            return false;
                        }
                    }
                    return true;
                }
            );
        }

        /** @brief The position keys to keep: lerp between the @p stored (float) keys stays within @p tolerance. */
        [[nodiscard]] std::vector<std::size_t>
        reduce_positions(const std::vector<Vec3> &values, const std::vector<Vec3> &stored, double tolerance)
        {
            return reduce(
                values.size(),
                [&](std::size_t a, std::size_t b) -> bool
                {
                    const double span = static_cast<double>(b - a);
                    for (std::size_t k = a + 1; k < b; ++k)
                    {
                        const Vec3 mid = lerp(stored[a], stored[b], static_cast<double>(k - a) / span);
                        if (length(subtract(mid, values[k])) > tolerance)
                        {
                            return false;
                        }
                    }
                    return true;
                }
            );
        }

        /** @brief One channel's kept keys: their frames and their rotations or positions. */
        struct Channel
        {
            std::vector<double> frames;
            std::vector<Quat> rotations;
            std::vector<Vec3> positions;
            std::uint8_t rotation_format = ROTATION_FLOAT;
        };

        /// 16-bit key times when the last (ascending) frame fits them, else 32-bit floats.
        [[nodiscard]] std::uint8_t time_format(const std::vector<double> &frames) noexcept
        {
            return !frames.empty() && frames.back() <= std::numeric_limits<std::uint16_t>::max() ? TIMES_U16
                                                                                                 : TIMES_F32;
        }

        /// Appends @p frames as key times in @p format.
        void put_times(Bytes &out, const std::vector<double> &frames, std::uint8_t format)
        {
            for (const double frame : frames)
            {
                if (format == TIMES_U16)
                {
                    out.put<std::uint16_t>(static_cast<std::uint16_t>(frame));
                }
                else
                {
                    out.put<float>(static_cast<float>(frame));
                }
            }
        }

        /** @brief A 0x829 controller chunk (CONTROLLER_CHUNK_DESC_0829): rotations, then positions, own key times. */
        [[nodiscard]] std::vector<std::byte>
        controller_chunk(std::uint32_t controller, const Channel &rotation, const Channel &position)
        {
            Bytes out;
            const std::uint8_t rotation_times = time_format(rotation.frames);
            const std::uint8_t position_times = time_format(position.frames);
            out.put<std::uint32_t>(controller);
            out.put<std::uint16_t>(static_cast<std::uint16_t>(rotation.rotations.size()));
            out.put<std::uint16_t>(static_cast<std::uint16_t>(position.positions.size()));
            out.put<std::uint8_t>(rotation.rotation_format);
            out.put<std::uint8_t>(rotation_times);
            out.put<std::uint8_t>(POSITION_FLOAT);
            out.put<std::uint8_t>(1); // positions carry their own key times
            out.put<std::uint8_t>(position_times);
            out.put<std::uint8_t>(1); // every block 4-byte aligned
            out.put<std::uint16_t>(0);
            if (!rotation.rotations.empty())
            {
                // compress_rotations stores every key in one of the two small-tree formats.
                for (const Quat &q : rotation.rotations)
                {
                    if (rotation.rotation_format == ROTATION_SMALL_TREE_48)
                    {
                        for (const std::uint16_t word : encode_small_tree_48(q))
                        {
                            out.put(word);
                        }
                    }
                    else
                    {
                        for (const std::uint32_t word : encode_small_tree_64(q))
                        {
                            out.put(word);
                        }
                    }
                }
                out.pad4(); // as the Resource Compiler: each block starts 4-byte aligned, the last one is not padded
                put_times(out, rotation.frames, rotation_times);
            }
            if (!position.positions.empty())
            {
                out.pad4();
                for (const Vec3 &p : position.positions)
                {
                    out.put(static_cast<float>(p.x));
                    out.put(static_cast<float>(p.y));
                    out.put(static_cast<float>(p.z));
                }
                out.pad4();
                put_times(out, position.frames, position_times);
            }
            return out.take();
        }

        /** @brief The kept rotation keys of @p values (first frame number @p first_frame), quantized. */
        [[nodiscard]] Channel
        compress_rotations(const std::vector<Quat> &values, double first_frame, double tolerance, bool root)
        {
            Channel channel;
            // The 48-bit format when its rounding costs at most half the tolerance, else the 64-bit one.
            channel.rotation_format = ROTATION_SMALL_TREE_64;
            if (!root)
            {
                double worst = 0.0;
                for (const Quat &q : values)
                {
                    worst = std::max(worst, angle_radians(q, quantized(q, ROTATION_SMALL_TREE_48)));
                }
                if (worst <= tolerance * 0.5)
                {
                    channel.rotation_format = ROTATION_SMALL_TREE_48;
                }
            }
            std::vector<Quat> stored;
            stored.reserve(values.size());
            for (const Quat &q : values)
            {
                stored.push_back(quantized(q, channel.rotation_format));
            }
            for (const std::size_t i : reduce_rotations(values, stored, tolerance))
            {
                channel.frames.push_back(first_frame + static_cast<double>(i));
                channel.rotations.push_back(values[i]);
            }
            return channel;
        }

        /** @brief The kept position keys of @p values (first frame number @p first_frame), stored as floats. */
        [[nodiscard]] Channel compress_positions(const std::vector<Vec3> &values, double first_frame, double tolerance)
        {
            Channel channel;
            std::vector<Vec3> stored;
            stored.reserve(values.size());
            for (const Vec3 &p : values)
            {
                stored.push_back(
                    Vec3{
                        .x = static_cast<float>(p.x),
                        .y = static_cast<float>(p.y),
                        .z = static_cast<float>(p.z),
                    }
                );
            }
            for (const std::size_t i : reduce_positions(values, stored, tolerance))
            {
                channel.frames.push_back(first_frame + static_cast<double>(i));
                channel.positions.push_back(values[i]);
            }
            return channel;
        }

        /** @brief A rotation and a position (CryEngine's QuatT). */
        struct Location
        {
            Quat rotation;
            Vec3 position;
        };

        /** @brief The motion parameters a compiled clip carries (see motion_chunk). */
        struct Motion
        {
            std::uint32_t flags = 0;
            std::int32_t end = 0;
            double speed = 0.0;
            double turn_speed = 0.0;
            double turn = 0.0;
            double distance = 0.0;
            double slope = 0.0;
            Location start;
            Location finish;
        };

        /// Appends @p location as a float QuatT: x, y, z, w, then the position.
        void put_location(Bytes &out, const Location &location)
        {
            out.put(static_cast<float>(location.rotation.x));
            out.put(static_cast<float>(location.rotation.y));
            out.put(static_cast<float>(location.rotation.z));
            out.put(static_cast<float>(location.rotation.w));
            out.put(static_cast<float>(location.position.x));
            out.put(static_cast<float>(location.position.y));
            out.put(static_cast<float>(location.position.z));
        }

        /** @brief MotionParams905 (CryHeaders.h), 132 bytes. */
        [[nodiscard]] std::vector<std::byte> motion_chunk(const Motion &motion, int compression)
        {
            Bytes out;
            out.put<std::uint32_t>(motion.flags);
            out.put<std::uint32_t>(static_cast<std::uint32_t>(compression));
            out.put<std::int32_t>(1);                                  // ticks per frame
            out.put<float>(static_cast<float>(1.0 / CLIP_FRAME_RATE)); // seconds per tick
            out.put<std::int32_t>(0);                                  // start
            out.put<std::int32_t>(motion.end);
            out.put(static_cast<float>(motion.speed));
            out.put(static_cast<float>(motion.turn_speed));
            out.put(static_cast<float>(motion.turn));
            out.put(static_cast<float>(motion.distance));
            out.put(static_cast<float>(motion.slope));
            put_location(out, motion.start);
            put_location(out, motion.finish);
            for (int i = 0; i < 8; ++i)
            {
                out.put(NO_FOOT_PLANT);
            }
            return out.take();
        }

        /** @brief The signed angle about Z from @p a to @p b (Ang3::CreateRadZ). */
        [[nodiscard]] double radians_about_z(const Vec3 &a, const Vec3 &b) noexcept
        {
            return std::atan2(a.x * b.y - a.y * b.x, a.x * b.x + a.y * b.y);
        }

        /**
         * @brief Turn and slope from the root's original keys at 0, 0.5 and 1 of the clip, as the Resource Compiler
         *        (AnimationLoader.cpp ReplaceRootByLocator) takes them.
         */
        void measure_turn(const SampledTrack &root, double duration, Motion &motion) noexcept
        {
            const double last = static_cast<double>(root.rotations.size() - 1);
            const auto rotation_at = [&](double t) -> Quat
            {
                return sample(
                    root.rotations,
                    t * last,
                    [](const Quat &a, const Quat &b, double f) { return nlerp(a, b, f); }
                );
            };
            const auto position_at = [&](double t) -> Vec3
            {
                return root.positions.empty() ? Vec3{}
                                              : sample(
                                                    root.positions,
                                                    t * last,
                                                    [](const Vec3 &a, const Vec3 &b, double f) { return lerp(a, b, f); }
                                                );
            };
            const Quat first = rotation_at(0.0);
            const Quat middle = rotation_at(0.5);
            const Quat end = rotation_at(1.0);
            const Vec3 travel = subtract(position_at(1.0), position_at(0.0));
            if (length(travel) > 0.01)
            {
                Vec3 direction = rotate(conjugate(first), travel); // CryEngine's Vec3 * Quat
                const double n = length(direction);
                direction = Vec3{
                    .x = direction.x / n,
                    .y = direction.y / n,
                    .z = direction.z / n,
                };
                const double flat = std::sqrt(direction.x * direction.x + direction.y * direction.y);
                if (flat > 0.0001)
                {
                    motion.slope = std::atan2(-direction.z * (direction.y / flat), flat);
                }
            }
            const Vec3 forward{
                .y = 1.0,
            };
            motion.turn = radians_about_z(rotate(first, forward), rotate(middle, forward)) +
                          radians_about_z(rotate(middle, forward), rotate(end, forward));
            motion.turn_speed = duration > 0.0 ? motion.turn / duration : 0.0;
        }

        /** @brief Distance and speed from the root's path in 100 steps (EvaluateSpeed). */
        void measure_speed(const SampledTrack &root, double duration, Motion &motion) noexcept
        {
            if (root.positions.size() < 2)
            {
                return;
            }
            const double last = static_cast<double>(root.positions.size() - 1);
            const auto at = [&](float t) -> Vec3
            {
                return sample(
                    root.positions,
                    static_cast<double>(t) * last,
                    [](const Vec3 &a, const Vec3 &b, double f) { return lerp(a, b, f); }
                );
            };
            Vec3 previous = at(0.0f);
            double distance = 0.0;
            for (float t = 0.01f; t <= 1.0f; t += 0.01f) // the Resource Compiler's float steps
            {
                const Vec3 next = at(t);
                distance += length(subtract(next, previous));
                previous = next;
            }
            motion.distance = distance;
            motion.speed = duration > 0.0 ? distance / duration : 0.0;
        }

        /// Whether @p a matches @p b or its negation within @p e per component (IsQuatEquivalent_dot).
        [[nodiscard]] bool equivalent_per_component(const Quat &a, const Quat &b, double e) noexcept
        {
            const auto close = [e](const Quat &p, const Quat &q) -> bool
            {
                return std::abs(p.x - q.x) <= e && std::abs(p.y - q.y) <= e && std::abs(p.z - q.z) <= e &&
                       std::abs(p.w - q.w) <= e;
            };
            return close(a, b) || close(a, negated(b));
        }

        /** @brief Each bone's default pose relative to its parent, from the skeleton's model-space bind pose. */
        [[nodiscard]] std::vector<Location> default_local_pose(std::span<const SkeletonBone> skeleton)
        {
            std::vector<Location> local(skeleton.size());
            for (std::size_t i = 0; i < skeleton.size(); ++i)
            {
                const SkeletonBone &bone = skeleton[i];
                if (bone.parent < 0 || static_cast<std::size_t>(bone.parent) >= skeleton.size())
                {
                    local[i] = Location{
                        .rotation = bone.rotation,
                        .position = bone.position,
                    };
                    continue;
                }
                const SkeletonBone &parent = skeleton[static_cast<std::size_t>(bone.parent)];
                const Quat inverse = conjugate(normalized(parent.rotation));
                local[i] = Location{
                    .rotation = normalized(multiply(inverse, bone.rotation)),
                    .position = rotate(inverse, subtract(bone.position, parent.position)),
                };
            }
            return local;
        }

        /// Whether every rotation of @p values lies within STATIC_ROTATION_TOLERANCE of @p value.
        [[nodiscard]] bool rotations_unchanged_at(const std::vector<Quat> &values, const Quat &value) noexcept
        {
            return std::ranges::all_of(
                values,
                [&](const Quat &q) { return angle_radians(q, value) <= STATIC_ROTATION_TOLERANCE; }
            );
        }

        /// Whether every position of @p values lies near @p value: within 0.1% of its length, from 0.1 mm to 2 cm.
        [[nodiscard]] bool positions_unchanged_at(const std::vector<Vec3> &values, const Vec3 &value) noexcept
        {
            const double tolerance = std::max(
                STATIC_POSITION_FLOOR,
                std::min(length(value) * STATIC_POSITION_EPSILON, STATIC_POSITION_CEILING)
            );
            return std::ranges::all_of(values, [&](const Vec3 &p) { return length(subtract(p, value)) <= tolerance; });
        }
    } // namespace

    std::expected<CompiledClip, std::string>
    compile_clip(const SampledClip &clip, std::span<const SkeletonBone> skeleton, const CompileOptions &options)
    {
        if (skeleton.empty())
        {
            return std::unexpected{std::string{"the skeleton has no bones"}};
        }
        if (clip.frames == 0)
        {
            return std::unexpected{std::string{"the clip has no frames"}};
        }
        if (options.additive && clip.frames < 2)
        {
            return std::unexpected{std::string{"an additive clip needs at least two frames"}};
        }
        std::unordered_map<std::uint32_t, std::size_t> bone_of;
        for (std::size_t i = 0; i < skeleton.size(); ++i)
        {
            bone_of.emplace(skeleton[i].controller, i);
        }
        std::unordered_set<std::uint32_t> keyed;
        for (const SampledTrack &track : clip.tracks)
        {
            if (track.rotations.size() != clip.frames ||
                (!track.positions.empty() && track.positions.size() != clip.frames))
            {
                return std::unexpected{std::format("track {:#010x} does not have one key per frame", track.controller)};
            }
            // A second track of one bone compiles to a second controller chunk with the same id.
            if (!keyed.insert(track.controller).second)
            {
                return std::unexpected{std::format("track {:#010x} appears twice", track.controller)};
            }
        }

        CompiledClip out;
        CompileReport &report = out.report;
        report.frames = clip.frames;
        report.additive = options.additive;
        const double duration = static_cast<double>(clip.frames - 1) / CLIP_FRAME_RATE;
        const std::uint32_t root_controller = skeleton[0].controller;
        const std::vector<Location> defaults = default_local_pose(skeleton);

        Motion motion;
        motion.end = static_cast<std::int32_t>(clip.frames - 1);
        const auto root_it = std::ranges::find(clip.tracks, root_controller, &SampledTrack::controller);
        SampledTrack root;
        if (root_it != clip.tracks.end())
        {
            // The root's first key is the clip's start location, and the root then starts at the identity
            // (ReplaceRootByLocator, with the root as its own locator: its children keep their keys).
            root = *root_it;
            Location first{
                .rotation = normalized(root.rotations.front()),
            };
            if (!root.positions.empty())
            {
                first.position = root.positions.front();
            }
            if (1.0 - std::abs(first.rotation.w) < 1.0e-5 && std::abs(first.rotation.x) < 1.0e-5 &&
                std::abs(first.rotation.y) < 1.0e-5 && std::abs(first.rotation.z) < 1.0e-5)
            {
                first.rotation = Quat{};
            }
            motion.start = first;
            motion.finish = Location{
                .rotation = normalized(root.rotations.back()),
                .position = root.positions.empty() ? Vec3{} : root.positions.back(),
            };
            measure_turn(root, duration, motion);
            const Quat inverse = conjugate(first.rotation);
            for (Quat &q : root.rotations)
            {
                q = normalized(multiply(inverse, q));
            }
            for (Vec3 &p : root.positions)
            {
                p = rotate(inverse, subtract(p, first.position));
            }
            measure_speed(root, duration, motion);
        }

        bool loop = true;
        for (const SampledTrack &track : clip.tracks)
        {
            const auto bone = bone_of.find(track.controller);
            if (bone == bone_of.end() || bone->second == 0)
            {
                continue;
            }
            loop = loop &&
                   equivalent_per_component(track.rotations.front(), track.rotations.back(), CYCLE_COMPONENT_TOLERANCE);
        }
        report.loop = options.loop.value_or(loop);
        motion.flags = (report.loop ? CA_ASSET_CYCLE : 0U) | (options.additive ? CA_ASSET_ADDITIVE : 0U);
        report.distance = motion.distance;
        report.speed = motion.speed;
        report.turn = motion.turn;

        const double scale = std::sqrt(1.0e-8 * static_cast<double>(std::max(options.compression, 0)));
        const double position_tolerance = std::max(scale, 1.0e-6);
        const double rotation_tolerance = std::max(2.0 * scale, 1.0e-6);

        std::vector<Chunk> chunks;
        chunks.push_back(
            Chunk{
                .type = CHUNK_MOTION,
                .version = MOTION_VERSION,
            }
        );
        for (const SampledTrack &source : clip.tracks)
        {
            const auto bone = bone_of.find(source.controller);
            if (bone == bone_of.end())
            {
                ++report.unknown_bones;
                continue;
            }
            const bool is_root = bone->second == 0;
            if (is_root && options.additive)
            {
                ++report.dropped_tracks; // an additive clip leaves the root to the base layer
                continue;
            }
            std::vector<Quat> rotations = is_root ? root.rotations : source.rotations;
            std::vector<Vec3> positions = is_root ? root.positions : source.positions;
            // The bones skeleton mods move (back slots, the right weapon root) stay theirs unless the clip moves them:
            // a key there, even at the default pose, undoes such a mod while the clip plays.
            const bool placed_by_skeleton =
                std::ranges::find(SKELETON_PLACED_BONES, skeleton[bone->second].name) != SKELETON_PLACED_BONES.end();
            if ((options.tracks == TrackPolicy::DropUnchangedAtDefault || placed_by_skeleton) && !is_root)
            {
                const Location &home = defaults[bone->second];
                if (rotations_unchanged_at(rotations, home.rotation))
                {
                    rotations.clear();
                }
                if (!positions.empty() && positions_unchanged_at(positions, home.position))
                {
                    positions.clear();
                }
                if (rotations.empty() && positions.empty())
                {
                    ++report.dropped_tracks;
                    continue;
                }
            }
            double first_frame = 0.0;
            if (options.additive)
            {
                // Each key's difference from the first frame (rotation q * q0^-1, position p - p0). The first frame
                // goes, and the engine plays the clip from frame 1.
                const Quat base = conjugate(normalized(source.rotations.front()));
                std::vector<Quat> rotation_delta;
                for (std::size_t i = 1; i < rotations.size(); ++i)
                {
                    rotation_delta.push_back(normalized(multiply(rotations[i], base)));
                }
                rotations = std::move(rotation_delta);
                if (!positions.empty())
                {
                    const Vec3 origin = positions.front();
                    std::vector<Vec3> position_delta;
                    for (std::size_t i = 1; i < positions.size(); ++i)
                    {
                        position_delta.push_back(subtract(positions[i], origin));
                    }
                    positions = std::move(position_delta);
                }
                first_frame = 1.0;
                // A channel that never changes adds nothing on top of the base layer.
                if (std::ranges::all_of(
                        rotations,
                        [&](const Quat &q) { return angle_radians(q, Quat{}) <= rotation_tolerance; }
                    ))
                {
                    rotations.clear();
                }
                if (std::ranges::all_of(positions, [&](const Vec3 &p) { return length(p) <= position_tolerance; }))
                {
                    positions.clear();
                }
                if (rotations.empty() && positions.empty())
                {
                    ++report.dropped_tracks;
                    continue;
                }
            }
            for (std::size_t i = 1; i < rotations.size(); ++i)
            {
                if (dot(rotations[i - 1], rotations[i]) < 0.0)
                {
                    rotations[i] = negated(rotations[i]); // one hemisphere, so interpolation takes the short arc
                }
            }
            const Channel rotation = rotations.empty() ? Channel{}
                                                       : compress_rotations(
                                                             rotations,
                                                             first_frame,
                                                             is_root ? ROOT_ROTATION_TOLERANCE : rotation_tolerance,
                                                             is_root
                                                         );
            const Channel position = positions.empty() ? Channel{}
                                                       : compress_positions(
                                                             positions,
                                                             first_frame,
                                                             is_root ? ROOT_POSITION_TOLERANCE : position_tolerance
                                                         );
            report.keys_in += rotations.size() + positions.size();
            report.keys_out += rotation.rotations.size() + position.positions.size();
            if (rotation.rotations.size() > std::numeric_limits<std::uint16_t>::max() ||
                position.positions.size() > std::numeric_limits<std::uint16_t>::max())
            {
                return std::unexpected{
                    std::format("track {:#010x} has more keys than a controller holds", source.controller)
                };
            }
            chunks.push_back(
                Chunk{
                    .type = CHUNK_CONTROLLER,
                    .version = CONTROLLER_829,
                    .data = controller_chunk(source.controller, rotation, position),
                }
            );
            ++report.tracks;
        }
        chunks.front().data = motion_chunk(motion, options.compression);
        out.file = chunk_file(chunks);
        report.bytes = out.file.size();
        return out;
    }

    std::expected<SampledClip, std::string>
    read_intermediate_clip(std::span<const std::byte> file, std::span<const SkeletonBone> skeleton)
    {
        auto chunks = read_chunk_views(file);
        if (!chunks)
        {
            return std::unexpected{chunks.error()};
        }
        // Match controller ids to the skeleton by its ids, then by the CRC of each bone's exact name (exporters differ
        // on case).
        std::unordered_map<std::uint32_t, std::uint32_t> controller_of;
        for (const SkeletonBone &bone : skeleton)
        {
            controller_of.emplace(bone.controller, bone.controller);
            controller_of.emplace(bone_controller_id(bone.name), bone.controller);
        }
        /// One intermediate key, converted to metres and a rotation.
        struct Key
        {
            std::int32_t tick = 0;
            Vec3 position;
            Quat rotation;
        };
        /// One matched controller's keys, in tick order.
        struct Source
        {
            std::uint32_t controller = 0;
            std::vector<Key> keys;
        };
        std::vector<Source> sources;
        SampledClip clip;
        std::int64_t first_tick = std::numeric_limits<std::int64_t>::max();
        std::int64_t last_tick = std::numeric_limits<std::int64_t>::min();
        for (const ChunkView &chunk : *chunks)
        {
            if (chunk.type != CHUNK_CONTROLLER || chunk.version != CONTROLLER_827 || chunk.data.size() < 8)
            {
                continue;
            }
            std::uint32_t count = 0;
            std::uint32_t id = 0;
            std::memcpy(&count, chunk.data.data(), 4);
            std::memcpy(&id, chunk.data.data() + 4, 4);
            if (count == 0 || (chunk.data.size() - 8) / INTERMEDIATE_KEY_SIZE < count)
            {
                return std::unexpected{std::format("controller {:#010x}: its keys run past the chunk", id)};
            }
            const auto match = controller_of.find(id);
            if (match == controller_of.end())
            {
                ++clip.unknown_tracks;
                continue;
            }
            // One source per bone bounds the resampled tracks by the skeleton, also when chunk entries share data.
            if (std::ranges::find(sources, match->second, &Source::controller) != sources.end())
            {
                return std::unexpected{std::format("bone {:#010x} is keyed by two controller chunks", match->second)};
            }
            Source source{
                .controller = match->second,
            };
            Quat previous{};
            for (std::uint32_t i = 0; i < count; ++i)
            {
                const std::byte *p = chunk.data.data() + 8 + std::size_t{i} * INTERMEDIATE_KEY_SIZE;
                std::int32_t tick = 0;
                std::array<float, 6> v{};
                std::memcpy(&tick, p, 4);
                std::memcpy(v.data(), p + 4, sizeof(v));
                // The log of the conjugate rotation (the Resource Compiler stores Quat::log(!q)).
                Quat q = conjugate(quat_exp(
                    Vec3{
                        .x = v[3],
                        .y = v[4],
                        .z = v[5],
                    }
                ));
                if (i > 0 && dot(previous, q) < 0.0)
                {
                    q = negated(q);
                }
                previous = q;
                source.keys.push_back(
                    Key{
                        .tick = tick,
                        .position =
                            Vec3{
                                .x = v[0] / CENTIMETRES_PER_METRE,
                                .y = v[1] / CENTIMETRES_PER_METRE,
                                .z = v[2] / CENTIMETRES_PER_METRE
                            },
                        .rotation = q,
                    }
                );
            }
            std::ranges::sort(source.keys, {}, &Key::tick);
            first_tick = std::min<std::int64_t>(first_tick, source.keys.front().tick);
            last_tick = std::max<std::int64_t>(last_tick, source.keys.back().tick);
            sources.push_back(std::move(source));
        }
        if (sources.empty())
        {
            return std::unexpected{std::string{"no controller of the file keys a bone of the skeleton"}};
        }
        const double ticks_per_frame = TICKS_PER_SECOND / CLIP_FRAME_RATE;
        const double span = static_cast<double>(last_tick - first_tick) / ticks_per_frame;
        if (span > MAX_CLIP_SECONDS * CLIP_FRAME_RATE)
        {
            return std::unexpected{
                std::format("its keys span {:.0f} s, more than an hour: the file is damaged", span / CLIP_FRAME_RATE)
            };
        }
        clip.frames = static_cast<std::size_t>(std::floor(span + 0.5)) + 1;
        for (const Source &source : sources)
        {
            SampledTrack track{
                .controller = source.controller,
            };
            std::size_t k = 0;
            for (std::size_t frame = 0; frame < clip.frames; ++frame)
            {
                const double tick = static_cast<double>(first_tick) + static_cast<double>(frame) * ticks_per_frame;
                while (k + 1 < source.keys.size() && static_cast<double>(source.keys[k + 1].tick) <= tick)
                {
                    ++k;
                }
                const Key &a = source.keys[k];
                const Key &b = source.keys[std::min(k + 1, source.keys.size() - 1)];
                const double gap = static_cast<double>(b.tick) - static_cast<double>(a.tick);
                const double t = gap > 0.0 ? std::clamp((tick - static_cast<double>(a.tick)) / gap, 0.0, 1.0) : 0.0;
                track.rotations.push_back(nlerp(a.rotation, b.rotation, t));
                track.positions.push_back(lerp(a.position, b.position, t));
            }
            clip.tracks.push_back(std::move(track));
        }
        return clip;
    }
} // namespace AnimationFramework
