/**
 * @file core/clip_header.cpp
 * @brief Clip index headers built from compiled .caf files (see clip_header.hpp).
 *
 * Layouts (CryEngine CryHeaders.h, unchanged in KCD2):
 *   - MotionParams905 (132 bytes): asset flags +0, compression +4, ticks per frame +8, seconds per tick +12, start key
 *     +16, end key +20, move speed +24, turn speed +28, asset turn +32, distance +36, slope +40, start location
 *     (QuatT) +44, end location +72, eight foot-plant times +100.
 *   - CHUNK_GAHCAF_INFO (404 bytes): flags +0, path +4 (256), path CRC +260, DBA CRC +264, foot-plant times +268, start
 *     +300, end +304, duration +308, controller count +312, start location +316, last locator key +344, velocity
 *     +372, distance +384, speed +388, slope +392, turn speed +396, asset turn +400.
 */

#include "core/clip_header.hpp"
#include "core/text_util.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <utility>

namespace AnimationFramework
{
    namespace
    {
        // CityHash64 v1.1 (Google, MIT), the version the engine's Steam 0x1806A3014 implements: its short-string
        // multiplier is k2 + 2 * length and its long-string seeds read the bytes at length - 40, - 16 and - 56.
        constexpr std::uint64_t CITY_K0 = 0xC3A5C85C97CB3127ULL;
        constexpr std::uint64_t CITY_K1 = 0xB492B66FBE98F273ULL;
        constexpr std::uint64_t CITY_K2 = 0x9AE16A3B2F90404FULL;
        constexpr std::uint64_t CITY_MUL = 0x9DDFEA08EB382D69ULL;

        /// The little-endian 64-bit value at @p p.
        [[nodiscard]] std::uint64_t fetch64(const char *p) noexcept
        {
            std::uint64_t value = 0;
            std::memcpy(&value, p, sizeof(value));
            return value;
        }

        /// The little-endian 32-bit value at @p p, widened.
        [[nodiscard]] std::uint64_t fetch32(const char *p) noexcept
        {
            std::uint32_t value = 0;
            std::memcpy(&value, p, sizeof(value));
            return value;
        }

        /// CityHash's ShiftMix.
        [[nodiscard]] std::uint64_t shift_mix(std::uint64_t value) noexcept
        {
            return value ^ (value >> 47);
        }

        /// CityHash's HashLen16, which is Hash128to64 with the default multiplier.
        [[nodiscard]] std::uint64_t hash_len16(std::uint64_t u, std::uint64_t v, std::uint64_t mul = CITY_MUL) noexcept
        {
            std::uint64_t a = (u ^ v) * mul;
            a ^= a >> 47;
            std::uint64_t b = (v ^ a) * mul;
            b ^= b >> 47;
            return b * mul;
        }

        /// CityHash64 of 0 to 16 bytes.
        [[nodiscard]] std::uint64_t hash_len0to16(const char *s, std::size_t len) noexcept
        {
            if (len >= 8)
            {
                const std::uint64_t mul = CITY_K2 + len * 2;
                const std::uint64_t a = fetch64(s) + CITY_K2;
                const std::uint64_t b = fetch64(s + len - 8);
                const std::uint64_t c = std::rotr(b, 37) * mul + a;
                const std::uint64_t d = (std::rotr(a, 25) + b) * mul;
                return hash_len16(c, d, mul);
            }
            if (len >= 4)
            {
                const std::uint64_t mul = CITY_K2 + len * 2;
                const std::uint64_t a = fetch32(s);
                return hash_len16(len + (a << 3), fetch32(s + len - 4), mul);
            }
            if (len > 0)
            {
                const auto a = static_cast<std::uint8_t>(s[0]);
                const auto b = static_cast<std::uint8_t>(s[len >> 1]);
                const auto c = static_cast<std::uint8_t>(s[len - 1]);
                const std::uint32_t y = static_cast<std::uint32_t>(a) + (static_cast<std::uint32_t>(b) << 8);
                const std::uint32_t z = static_cast<std::uint32_t>(len) + (static_cast<std::uint32_t>(c) << 2);
                return shift_mix(y * CITY_K2 ^ z * CITY_K0) * CITY_K2;
            }
            return CITY_K2;
        }

        /// CityHash64 of 17 to 32 bytes.
        [[nodiscard]] std::uint64_t hash_len17to32(const char *s, std::size_t len) noexcept
        {
            const std::uint64_t mul = CITY_K2 + len * 2;
            const std::uint64_t a = fetch64(s) * CITY_K1;
            const std::uint64_t b = fetch64(s + 8);
            const std::uint64_t c = fetch64(s + len - 8) * mul;
            const std::uint64_t d = fetch64(s + len - 16) * CITY_K2;
            return hash_len16(std::rotr(a + b, 43) + std::rotr(c, 30) + d, a + std::rotr(b + CITY_K2, 18) + c, mul);
        }

        /// CityHash64 of 33 to 64 bytes.
        [[nodiscard]] std::uint64_t hash_len33to64(const char *s, std::size_t len) noexcept
        {
            const std::uint64_t mul = CITY_K2 + len * 2;
            std::uint64_t a = fetch64(s) * CITY_K2;
            std::uint64_t b = fetch64(s + 8);
            const std::uint64_t c = fetch64(s + len - 24);
            const std::uint64_t d = fetch64(s + len - 32);
            const std::uint64_t e = fetch64(s + 16) * CITY_K2;
            const std::uint64_t f = fetch64(s + 24) * 9;
            const std::uint64_t g = fetch64(s + len - 8);
            const std::uint64_t h = fetch64(s + len - 16) * mul;
            const std::uint64_t u = std::rotr(a + g, 43) + (std::rotr(b, 30) + c) * 9;
            const std::uint64_t v = ((a + g) ^ d) + f + 1;
            const std::uint64_t w = std::byteswap((u + v) * mul) + h;
            const std::uint64_t x = std::rotr(e + f, 42) + c;
            const std::uint64_t y = (std::byteswap((v + w) * mul) + g) * mul;
            const std::uint64_t z = e + f + c;
            a = std::byteswap((x + z) * mul + y) + b;
            b = shift_mix((z + a) * mul + d + h) * mul;
            return b + x;
        }

        /// CityHash's WeakHashLen32WithSeeds over the 32 bytes at @p s.
        [[nodiscard]] std::pair<std::uint64_t, std::uint64_t>
        weak_hash_len32(const char *s, std::uint64_t a, std::uint64_t b) noexcept
        {
            const std::uint64_t w = fetch64(s);
            const std::uint64_t x = fetch64(s + 8);
            const std::uint64_t y = fetch64(s + 16);
            const std::uint64_t z = fetch64(s + 24);
            a += w;
            b = std::rotr(b + a + z, 21);
            const std::uint64_t c = a;
            a += x;
            a += y;
            b += std::rotr(a, 44);
            return {a + z, b + c};
        }

        /// CityHash64 of the @p len bytes at @p s.
        [[nodiscard]] std::uint64_t city_hash64(const char *s, std::size_t len) noexcept
        {
            if (len <= 32)
            {
                return len <= 16 ? hash_len0to16(s, len) : hash_len17to32(s, len);
            }
            if (len <= 64)
            {
                return hash_len33to64(s, len);
            }
            std::uint64_t x = fetch64(s + len - 40);
            std::uint64_t y = fetch64(s + len - 16) + fetch64(s + len - 56);
            std::uint64_t z = hash_len16(fetch64(s + len - 48) + len, fetch64(s + len - 24));
            auto v = weak_hash_len32(s + len - 64, len, z);
            auto w = weak_hash_len32(s + len - 32, y + CITY_K1, x);
            x = x * CITY_K1 + fetch64(s);
            std::size_t left = (len - 1) & ~static_cast<std::size_t>(63);
            do
            {
                x = std::rotr(x + y + v.first + fetch64(s + 8), 37) * CITY_K1;
                y = std::rotr(y + v.second + fetch64(s + 48), 42) * CITY_K1;
                x ^= w.second;
                y += v.first + fetch64(s + 40);
                z = std::rotr(z + w.first, 33) * CITY_K1;
                v = weak_hash_len32(s, v.second * CITY_K1, x + w.first);
                w = weak_hash_len32(s + 32, z + w.second, y + fetch64(s + 16));
                std::swap(z, x);
                s += 64;
                left -= 64;
            } while (left != 0);
            return hash_len16(
                hash_len16(v.first, w.first) + shift_mix(y) * CITY_K1 + z,
                hash_len16(v.second, w.second) + x
            );
        }

        constexpr std::uint32_t ASSET_ON_DEMAND = 0x2000;
        constexpr float SECONDS_PER_FRAME = 1.0F / 30.0F;

        /// The value at @p offset of @p bytes. The caller checks the bounds.
        template <typename T> [[nodiscard]] T load(std::span<const std::byte> bytes, std::size_t offset) noexcept
        {
            T value{};
            std::memcpy(&value, bytes.data() + offset, sizeof(T));
            return value;
        }

        /// Writes @p value at @p offset of @p header.
        template <typename T> void store(ClipHeader &header, std::size_t offset, T value) noexcept
        {
            std::memcpy(header.data() + offset, &value, sizeof(T));
        }

        /// Copies @p count bytes at @p offset of @p from to @p to of @p header.
        void copy_bytes(
            ClipHeader &header,
            std::size_t to,
            std::span<const std::byte> from,
            std::size_t offset,
            std::size_t count
        ) noexcept
        {
            std::memcpy(header.data() + to, from.data() + offset, count);
        }
    } // namespace

    std::uint64_t clip_path_hash(std::string_view path) noexcept
    {
        std::array<char, 1024> lowered{};
        const std::size_t length = std::min(path.size(), lowered.size());
        std::ranges::transform(path.substr(0, length), lowered.begin(), ascii_lower);
        return city_hash64(lowered.data(), length);
    }

    std::optional<ClipHeader> build_clip_header(
        std::span<const std::byte> motion_chunk,
        std::uint32_t controller_count,
        std::string_view path
    ) noexcept
    {
        if (motion_chunk.size() < CAF_MOTION_CHUNK_SIZE || path.empty() || path.size() >= CLIP_HEADER_PATH_SIZE)
        {
            return std::nullopt;
        }
        const auto flags = load<std::uint32_t>(motion_chunk, 0);
        const auto start_key = load<std::int32_t>(motion_chunk, 16);
        const auto end_key = load<std::int32_t>(motion_chunk, 20);
        // The resource compiler rebases the keys to start at 0 and keeps at least one frame. The key span is taken in
        // 64 bits so hostile keys cannot overflow it.
        float end_seconds = static_cast<float>(std::int64_t{end_key} - start_key) * SECONDS_PER_FRAME;
        if (end_seconds <= 0.0F)
        {
            end_seconds = SECONDS_PER_FRAME;
        }

        ClipHeader header{};
        store<std::uint32_t>(header, 0, flags | ASSET_ON_DEMAND);
        std::memcpy(header.data() + CLIP_HEADER_PATH_OFFSET, path.data(), path.size());
        copy_bytes(header, CLIP_HEADER_FOOT_PLANTS_OFFSET, motion_chunk, 100, 32);
        store<float>(header, 300, 0.0F);
        store<float>(header, 304, end_seconds);
        store<float>(header, CLIP_HEADER_DURATION_OFFSET, end_seconds);
        store<std::uint32_t>(header, CLIP_HEADER_CONTROLLERS_OFFSET, controller_count);
        copy_bytes(header, 316, motion_chunk, 44, 28);
        copy_bytes(header, 344, motion_chunk, 72, 28);
        store<float>(header, 384, load<float>(motion_chunk, 36));
        store<float>(header, 388, load<float>(motion_chunk, 24));
        store<float>(header, 392, load<float>(motion_chunk, 40));
        store<float>(header, 396, load<float>(motion_chunk, 28));
        store<float>(header, 400, load<float>(motion_chunk, 32));
        return header;
    }

    std::vector<ClipHeader> read_clip_image(std::span<const std::byte> data)
    {
        std::vector<ClipHeader> headers;
        if (data.size() < 16 || std::memcmp(data.data(), "CrCh", 4) != 0 || load<std::uint32_t>(data, 4) != 0x746)
        {
            return headers;
        }
        const auto count = load<std::uint32_t>(data, 8);
        const auto table = load<std::uint32_t>(data, 12);
        if (static_cast<std::uint64_t>(table) + static_cast<std::uint64_t>(count) * 16 > data.size())
        {
            return headers;
        }
        headers.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const std::size_t entry = table + static_cast<std::size_t>(i) * 16;
            const auto type = load<std::uint16_t>(data, entry);
            const auto version = load<std::uint16_t>(data, entry + 2);
            const auto size = load<std::uint32_t>(data, entry + 8);
            const auto offset = load<std::uint32_t>(data, entry + 12);
            if (type != CLIP_HEADER_CHUNK_TYPE || version != CLIP_HEADER_CHUNK_VERSION || size != CLIP_HEADER_SIZE ||
                static_cast<std::uint64_t>(offset) + size > data.size())
            {
                continue;
            }
            ClipHeader header{};
            std::memcpy(header.data(), data.data() + offset, CLIP_HEADER_SIZE);
            headers.push_back(header);
        }
        return headers;
    }

    std::string clip_header_path(const ClipHeader &header)
    {
        const char *text = reinterpret_cast<const char *>(header.data() + CLIP_HEADER_PATH_OFFSET);
        return fold_path(std::string_view{text, strnlen(text, CLIP_HEADER_PATH_SIZE)});
    }

} // namespace AnimationFramework
