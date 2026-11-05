/**
 * @file packs/zip_reader.cpp
 * @brief Zip directory and entry reads with a raw DEFLATE decoder (see zip_reader.hpp).
 *
 * The decoder follows RFC 1951: stored, fixed-Huffman and dynamic-Huffman blocks. Huffman codes decode through a
 * 9-bit lookup table for the short codes and a canonical walk for the rest. Every entry read is checked against the
 * central directory's CRC-32, so a decoder slip can never reach the catalog.
 */

#include "packs/zip_reader.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <span>
#include <string_view>
#include <utility>

namespace SoundFramework::packs
{
    namespace
    {
        constexpr std::uint32_t END_SIGNATURE = 0x06054B50;
        constexpr std::uint32_t CENTRAL_SIGNATURE = 0x02014B50;
        constexpr std::uint32_t LOCAL_SIGNATURE = 0x04034B50;
        constexpr std::size_t END_SIZE = 22;
        constexpr std::size_t CENTRAL_SIZE = 46;
        constexpr std::size_t LOCAL_SIZE = 30;
        constexpr std::size_t END_SEARCH = END_SIZE + 0xFFFF;
        constexpr std::uint32_t MAX_DIRECTORY = 64U * 1024U * 1024U;
        constexpr std::uint32_t MAX_ENTRY = 256U * 1024U * 1024U;

        template <typename T> [[nodiscard]] T load(const char *p) noexcept
        {
            T value{};
            std::memcpy(&value, p, sizeof(T));
            return value;
        }

        [[nodiscard]] bool read_at(std::ifstream &file, std::uint64_t offset, std::string &out)
        {
            file.clear();
            file.seekg(static_cast<std::streamoff>(offset));
            file.read(out.data(), static_cast<std::streamsize>(out.size()));
            return file.good();
        }

        /** @brief LSB-first bit reader over the compressed bytes. */
        class Bits
        {
        public:
            explicit Bits(std::span<const std::uint8_t> in) noexcept : m_in(in) {}

            [[nodiscard]] bool need(int count) noexcept
            {
                while (m_have < count && m_pos < m_in.size())
                {
                    m_buffer |= static_cast<std::uint64_t>(m_in[m_pos++]) << m_have;
                    m_have += 8;
                }
                return m_have >= count;
            }

            /** @brief The next @p count bits (count <= 32), or -1 past the end. */
            [[nodiscard]] std::int64_t take(int count) noexcept
            {
                if (count == 0)
                {
                    return 0;
                }
                if (!need(count))
                {
                    return -1;
                }
                const auto value = static_cast<std::int64_t>(m_buffer & ((1ULL << count) - 1));
                m_buffer >>= count;
                m_have -= count;
                return value;
            }

            /** @brief Up to 9 bits without consuming them (zero-padded at the end of the input). */
            [[nodiscard]] std::uint32_t peek9() noexcept
            {
                (void)need(9);
                return static_cast<std::uint32_t>(m_buffer & 0x1FF);
            }

            [[nodiscard]] int available() const noexcept { return m_have; }

            void drop(int count) noexcept
            {
                m_buffer >>= count;
                m_have -= count;
            }

            void align() noexcept { drop(m_have % 8); }

        private:
            std::span<const std::uint8_t> m_in;
            std::size_t m_pos = 0;
            std::uint64_t m_buffer = 0;
            int m_have = 0;
        };

        /** @brief A canonical Huffman code: per-length counts, symbols in code order, and a 9-bit fast table. */
        struct Huffman
        {
            std::array<std::uint16_t, 16> count{};
            std::array<std::uint16_t, 320> symbol{};
            // Fast entries: symbol in the low 16 bits, code length in the high 16 (0 = longer than 9 bits).
            std::array<std::uint32_t, 512> fast{};
        };

        [[nodiscard]] bool build(Huffman &h, const std::uint8_t *lengths, int n) noexcept
        {
            h.count.fill(0);
            h.fast.fill(0);
            for (int i = 0; i < n; ++i)
            {
                ++h.count[lengths[i]];
            }
            h.count[0] = 0;
            int left = 1;
            for (int len = 1; len < 16; ++len)
            {
                left <<= 1;
                left -= h.count[len];
                if (left < 0)
                {
                    return false; // over-subscribed
                }
            }
            std::array<std::uint16_t, 16> offset{};
            for (int len = 1; len < 15; ++len)
            {
                offset[len + 1] = static_cast<std::uint16_t>(offset[len] + h.count[len]);
            }
            for (int i = 0; i < n; ++i)
            {
                if (lengths[i] != 0)
                {
                    h.symbol[offset[lengths[i]]++] = static_cast<std::uint16_t>(i);
                }
            }
            // Canonical codes, bit-reversed into the fast table for every code of 9 bits or fewer.
            std::uint32_t code = 0;
            int index = 0;
            for (int len = 1; len <= 9; ++len)
            {
                for (int k = 0; k < h.count[len]; ++k, ++index, ++code)
                {
                    std::uint32_t reversed = 0;
                    for (int b = 0; b < len; ++b)
                    {
                        reversed |= ((code >> b) & 1U) << (len - 1 - b);
                    }
                    for (std::uint32_t fill = reversed; fill < 512; fill += (1U << len))
                    {
                        h.fast[fill] = h.symbol[index] | (static_cast<std::uint32_t>(len) << 16);
                    }
                }
                code <<= 1;
            }
            return true;
        }

        /** @brief One symbol, or -1. */
        [[nodiscard]] int decode(Bits &bits, const Huffman &h) noexcept
        {
            const std::uint32_t hit = h.fast[bits.peek9()];
            const int len = static_cast<int>(hit >> 16);
            if (len != 0 && len <= bits.available())
            {
                bits.drop(len);
                return static_cast<int>(hit & 0xFFFF);
            }
            int code = 0;
            int first = 0;
            int index = 0;
            for (int l = 1; l < 16; ++l)
            {
                const std::int64_t bit = bits.take(1);
                if (bit < 0)
                {
                    return -1;
                }
                code |= static_cast<int>(bit);
                const int count = h.count[l];
                if (code - count < first)
                {
                    return h.symbol[index + (code - first)];
                }
                index += count;
                first += count;
                first <<= 1;
                code <<= 1;
            }
            return -1;
        }

        constexpr std::array<std::uint16_t, 29> LENGTH_BASE = {3,  4,  5,  6,   7,   8,   9,   10,  11, 13,
                                                               15, 17, 19, 23,  27,  31,  35,  43,  51, 59,
                                                               67, 83, 99, 115, 131, 163, 195, 227, 258};
        constexpr std::array<std::uint8_t, 29> LENGTH_EXTRA = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                               2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        constexpr std::array<std::uint16_t, 30> DISTANCE_BASE = {1,    2,    3,    4,     5,     7,    9,    13,
                                                                 17,   25,   33,   49,    65,    97,   129,  193,
                                                                 257,  385,  513,  769,   1025,  1537, 2049, 3073,
                                                                 4097, 6145, 8193, 12289, 16385, 24577};
        constexpr std::array<std::uint8_t, 30> DISTANCE_EXTRA = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                                 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

        /** @brief Decodes one Huffman block into @p out, failing as soon as it would grow past @p limit bytes. */
        [[nodiscard]] bool
        codes(Bits &bits, std::string &out, const Huffman &lit, const Huffman &dist, std::size_t limit)
        {
            for (;;)
            {
                int sym = decode(bits, lit);
                if (sym < 0)
                {
                    return false;
                }
                if (sym < 256)
                {
                    if (out.size() >= limit)
                    {
                        return false;
                    }
                    out.push_back(static_cast<char>(sym));
                    continue;
                }
                if (sym == 256)
                {
                    return true;
                }
                sym -= 257;
                if (sym >= 29)
                {
                    return false;
                }
                const std::int64_t extra = bits.take(LENGTH_EXTRA[sym]);
                const int ds = decode(bits, dist);
                if (extra < 0 || ds < 0 || ds >= 30)
                {
                    return false;
                }
                const std::int64_t dextra = bits.take(DISTANCE_EXTRA[ds]);
                if (dextra < 0)
                {
                    return false;
                }
                const std::size_t length = LENGTH_BASE[sym] + static_cast<std::size_t>(extra);
                const std::size_t distance = DISTANCE_BASE[ds] + static_cast<std::size_t>(dextra);
                if (distance > out.size() || length > limit - out.size())
                {
                    return false;
                }
                const std::size_t from = out.size() - distance;
                for (std::size_t i = 0; i < length; ++i)
                {
                    out.push_back(out[from + i]);
                }
            }
        }

        [[nodiscard]] const std::pair<Huffman, Huffman> &fixed_tables()
        {
            static const std::pair<Huffman, Huffman> tables = []
            {
                std::pair<Huffman, Huffman> t;
                std::array<std::uint8_t, 288> lit{};
                std::fill(lit.begin(), lit.begin() + 144, std::uint8_t{8});
                std::fill(lit.begin() + 144, lit.begin() + 256, std::uint8_t{9});
                std::fill(lit.begin() + 256, lit.begin() + 280, std::uint8_t{7});
                std::fill(lit.begin() + 280, lit.end(), std::uint8_t{8});
                std::array<std::uint8_t, 30> dist{};
                dist.fill(5);
                (void)build(t.first, lit.data(), 288);
                (void)build(t.second, dist.data(), 30);
                return t;
            }();
            return tables;
        }

        [[nodiscard]] bool dynamic(Bits &bits, std::string &out, std::size_t limit)
        {
            const std::int64_t hlit = bits.take(5);
            const std::int64_t hdist = bits.take(5);
            const std::int64_t hclen = bits.take(4);
            if (hlit < 0 || hdist < 0 || hclen < 0)
            {
                return false;
            }
            const int nlen = static_cast<int>(hlit) + 257;
            const int ndist = static_cast<int>(hdist) + 1;
            const int ncode = static_cast<int>(hclen) + 4;
            if (nlen > 286 || ndist > 30)
            {
                return false;
            }
            static constexpr std::array<std::uint8_t, 19> order =
                {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            std::array<std::uint8_t, 19> code_lengths{};
            for (int i = 0; i < ncode; ++i)
            {
                const std::int64_t v = bits.take(3);
                if (v < 0)
                {
                    return false;
                }
                code_lengths[order[i]] = static_cast<std::uint8_t>(v);
            }
            Huffman lencode;
            if (!build(lencode, code_lengths.data(), 19))
            {
                return false;
            }
            std::array<std::uint8_t, 320> lengths{};
            int index = 0;
            while (index < nlen + ndist)
            {
                int sym = decode(bits, lencode);
                if (sym < 0)
                {
                    return false;
                }
                if (sym < 16)
                {
                    lengths[index++] = static_cast<std::uint8_t>(sym);
                    continue;
                }
                std::uint8_t value = 0;
                std::int64_t repeat = 0;
                if (sym == 16)
                {
                    if (index == 0)
                    {
                        return false;
                    }
                    value = lengths[index - 1];
                    repeat = bits.take(2);
                    repeat = repeat < 0 ? -1 : repeat + 3;
                }
                else if (sym == 17)
                {
                    repeat = bits.take(3);
                    repeat = repeat < 0 ? -1 : repeat + 3;
                }
                else
                {
                    repeat = bits.take(7);
                    repeat = repeat < 0 ? -1 : repeat + 11;
                }
                if (repeat < 0 || index + repeat > nlen + ndist)
                {
                    return false;
                }
                while (repeat-- > 0)
                {
                    lengths[index++] = value;
                }
            }
            if (lengths[256] == 0)
            {
                return false;
            }
            Huffman lit;
            Huffman dist;
            if (!build(lit, lengths.data(), nlen) || !build(dist, lengths.data() + nlen, ndist))
            {
                return false;
            }
            return codes(bits, out, lit, dist, limit);
        }

        /** @brief The standard (zlib) CRC-32. */
        [[nodiscard]] std::uint32_t crc32_of(std::span<const std::uint8_t> data) noexcept
        {
            static const std::array<std::uint32_t, 256> table = []
            {
                std::array<std::uint32_t, 256> t{};
                for (std::uint32_t i = 0; i < 256; ++i)
                {
                    std::uint32_t c = i;
                    for (int k = 0; k < 8; ++k)
                    {
                        c = (c & 1U) != 0 ? 0xEDB88320U ^ (c >> 1) : c >> 1;
                    }
                    t[i] = c;
                }
                return t;
            }();
            std::uint32_t crc = 0xFFFFFFFFU;
            for (const std::uint8_t byte : data)
            {
                crc = table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
            }
            return ~crc;
        }

        /** @brief Raw DEFLATE (RFC 1951) decode of @p in, expecting @p size bytes out. */
        [[nodiscard]] std::optional<std::string> inflate_raw(std::span<const std::uint8_t> in, std::size_t size)
        {
            std::string out;
            out.reserve(size);
            Bits bits{in};
            for (bool last = false; !last;)
            {
                const std::int64_t final_bit = bits.take(1);
                const std::int64_t type = bits.take(2);
                if (final_bit < 0 || type < 0)
                {
                    return std::nullopt;
                }
                last = final_bit == 1;
                if (type == 0)
                {
                    bits.align();
                    const std::int64_t len = bits.take(16);
                    const std::int64_t nlen = bits.take(16);
                    if (len < 0 || nlen < 0 || (len ^ 0xFFFF) != nlen ||
                        static_cast<std::size_t>(len) > size - out.size())
                    {
                        return std::nullopt;
                    }
                    for (std::int64_t i = 0; i < len; ++i)
                    {
                        const std::int64_t byte = bits.take(8);
                        if (byte < 0)
                        {
                            return std::nullopt;
                        }
                        out.push_back(static_cast<char>(byte));
                    }
                }
                else if (type == 1)
                {
                    const auto &[lit, dist] = fixed_tables();
                    if (!codes(bits, out, lit, dist, size))
                    {
                        return std::nullopt;
                    }
                }
                else if (type == 2)
                {
                    if (!dynamic(bits, out, size))
                    {
                        return std::nullopt;
                    }
                }
                else
                {
                    return std::nullopt;
                }
            }
            if (out.size() != size)
            {
                return std::nullopt;
            }
            return out;
        }
    } // namespace

    std::optional<std::vector<ZipEntry>> read_zip_index(const std::filesystem::path &path)
    {
        std::ifstream file{path, std::ios::binary | std::ios::ate};
        if (!file)
        {
            return std::nullopt;
        }
        const auto file_size = static_cast<std::uint64_t>(file.tellg());
        if (file_size < END_SIZE)
        {
            return std::nullopt;
        }
        std::string tail(static_cast<std::size_t>(std::min<std::uint64_t>(file_size, END_SEARCH)), '\0');
        if (!read_at(file, file_size - tail.size(), tail))
        {
            return std::nullopt;
        }
        std::size_t end_record = std::string::npos;
        for (std::size_t i = tail.size() - END_SIZE + 1; i-- > 0;)
        {
            if (load<std::uint32_t>(tail.data() + i) == END_SIGNATURE)
            {
                end_record = i;
                break;
            }
        }
        if (end_record == std::string::npos)
        {
            return std::nullopt;
        }
        const auto directory_size = load<std::uint32_t>(tail.data() + end_record + 12);
        const auto directory_offset = load<std::uint32_t>(tail.data() + end_record + 16);
        // The record's entry count is not read: the game's voice paks hold exactly 65535 entries, the value that would
        // mean a ZIP64 archive, and the directory's size says where it ends anyway.
        if (directory_offset == 0xFFFFFFFF || directory_size > MAX_DIRECTORY ||
            static_cast<std::uint64_t>(directory_offset) + directory_size > file_size)
        {
            return std::nullopt;
        }
        std::string directory(directory_size, '\0');
        if (!read_at(file, directory_offset, directory))
        {
            return std::nullopt;
        }
        std::vector<ZipEntry> entries;
        std::size_t at = 0;
        while (at < directory.size())
        {
            if (at + CENTRAL_SIZE > directory.size() || load<std::uint32_t>(directory.data() + at) != CENTRAL_SIGNATURE)
            {
                return std::nullopt;
            }
            const char *h = directory.data() + at;
            const auto name_length = load<std::uint16_t>(h + 28);
            const auto extra_length = load<std::uint16_t>(h + 30);
            const auto comment_length = load<std::uint16_t>(h + 32);
            if (at + CENTRAL_SIZE + name_length > directory.size())
            {
                return std::nullopt;
            }
            ZipEntry entry;
            entry.name.assign(h + CENTRAL_SIZE, name_length);
            std::ranges::replace(entry.name, '\\', '/');
            entry.method = load<std::uint16_t>(h + 10);
            entry.crc32 = load<std::uint32_t>(h + 16);
            entry.compressed_size = load<std::uint32_t>(h + 20);
            entry.size = load<std::uint32_t>(h + 24);
            entry.local_offset = load<std::uint32_t>(h + 42);
            entries.push_back(std::move(entry));
            at += CENTRAL_SIZE + name_length + extra_length + comment_length;
        }
        return entries;
    }

    std::optional<std::string> read_zip_file(std::ifstream &file, const ZipEntry &entry)
    {
        if (entry.size > MAX_ENTRY || entry.compressed_size > MAX_ENTRY)
        {
            return std::nullopt;
        }
        std::string header(LOCAL_SIZE, '\0');
        // A failed read leaves the stream failed; read_at clears that, so one bad entry does not fail the rest.
        if (!file.is_open() || !read_at(file, entry.local_offset, header) ||
            load<std::uint32_t>(header.data()) != LOCAL_SIGNATURE)
        {
            return std::nullopt;
        }
        const auto name_length = load<std::uint16_t>(header.data() + 26);
        const auto extra_length = load<std::uint16_t>(header.data() + 28);
        std::string packed(entry.compressed_size, '\0');
        if (!read_at(
                file,
                static_cast<std::uint64_t>(entry.local_offset) + LOCAL_SIZE + name_length + extra_length,
                packed
            ))
        {
            return std::nullopt;
        }
        std::optional<std::string> data;
        if (entry.method == 0)
        {
            data = std::move(packed);
        }
        else if (entry.method == 8)
        {
            data = inflate_raw({reinterpret_cast<const std::uint8_t *>(packed.data()), packed.size()}, entry.size);
        }
        if (!data || data->size() != entry.size ||
            crc32_of({reinterpret_cast<const std::uint8_t *>(data->data()), data->size()}) != entry.crc32)
        {
            return std::nullopt;
        }
        return data;
    }

} // namespace SoundFramework::packs
