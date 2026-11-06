/**
 * @file core/zip_writer.cpp
 * @brief Stored-entry zip writer (see zip_writer.hpp).
 */

#include "core/zip_writer.hpp"

#include "core/zip_reader.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace AnimationFramework
{
    namespace
    {
        constexpr std::uint32_t LOCAL_SIGNATURE = 0x04034B50;
        constexpr std::uint32_t CENTRAL_SIGNATURE = 0x02014B50;
        constexpr std::uint32_t END_SIGNATURE = 0x06054B50;
        /// Version 2.0, the version that reads stored entries.
        constexpr std::uint16_t ZIP_VERSION = 20;
        constexpr std::uint16_t METHOD_STORED = 0;
        /**
         * @brief General-purpose bit 11, the language encoding flag: the name is UTF-8. CryPak reads names as bytes and
         *        never tests the bit, so it only tells other zip tools how to show a non-ASCII name.
         */
        constexpr std::uint16_t FLAG_UTF8_NAME = 1U << 11U;
        /// The DOS date 1980-01-01, the first date a zip can hold.
        constexpr std::uint16_t DOS_DATE = (1U << 5U) | 1U;
        constexpr std::size_t LOCAL_HEADER_SIZE = 30;
        constexpr std::size_t END_RECORD_SIZE = 22;
        constexpr std::uint64_t MAX_OFFSET = std::numeric_limits<std::uint32_t>::max();
        constexpr std::size_t MAX_ENTRIES = std::numeric_limits<std::uint16_t>::max();
        constexpr std::size_t MAX_NAME = std::numeric_limits<std::uint16_t>::max();

        /// Appends @p value to @p out little-endian.
        template <typename T> void put(std::string &out, T value)
        {
            for (std::size_t i = 0; i < sizeof(T); ++i)
            {
                out.push_back(static_cast<char>((value >> (8U * i)) & 0xFFU));
            }
        }

        /** @brief True when @p text holds a byte past ASCII and is well-formed UTF-8 throughout. */
        [[nodiscard]] bool is_non_ascii_utf8(std::string_view text) noexcept
        {
            bool non_ascii = false;
            for (std::size_t i = 0; i < text.size();)
            {
                const auto lead = static_cast<std::uint8_t>(text[i]);
                if (lead < 0x80U)
                {
                    ++i;
                    continue;
                }
                non_ascii = true;
                // The second byte's range rules out overlong forms, UTF-16 surrogates and code points past U+10FFFF.
                std::size_t length = 0;
                std::uint8_t low = 0x80U;
                std::uint8_t high = 0xBFU;
                if (lead >= 0xC2U && lead <= 0xDFU)
                {
                    length = 2;
                }
                else if (lead >= 0xE0U && lead <= 0xEFU)
                {
                    length = 3;
                    low = lead == 0xE0U ? 0xA0U : low;
                    high = lead == 0xEDU ? 0x9FU : high;
                }
                else if (lead >= 0xF0U && lead <= 0xF4U)
                {
                    length = 4;
                    low = lead == 0xF0U ? 0x90U : low;
                    high = lead == 0xF4U ? 0x8FU : high;
                }
                else
                {
                    return false;
                }
                if (text.size() - i < length)
                {
                    return false;
                }
                for (std::size_t k = 1; k < length; ++k)
                {
                    const auto next = static_cast<std::uint8_t>(text[i + k]);
                    const std::uint8_t min = k == 1 ? low : 0x80U;
                    const std::uint8_t max = k == 1 ? high : 0xBFU;
                    if (next < min || next > max)
                    {
                        return false;
                    }
                }
                i += length;
            }
            return non_ascii;
        }

        /** @brief The fields a local header and a central record share, from the version needed to the extra length. */
        void put_entry_fields(
            std::string &out,
            std::uint16_t flags,
            std::uint32_t crc,
            std::uint32_t size,
            std::uint16_t name_length
        )
        {
            put<std::uint16_t>(out, ZIP_VERSION);
            put<std::uint16_t>(out, flags);
            put<std::uint16_t>(out, METHOD_STORED);
            put<std::uint16_t>(out, 0); // time
            put<std::uint16_t>(out, DOS_DATE);
            put<std::uint32_t>(out, crc);
            put<std::uint32_t>(out, size); // compressed size
            put<std::uint32_t>(out, size);
            put<std::uint16_t>(out, name_length);
            put<std::uint16_t>(out, 0); // extra length
        }
    } // namespace

    std::optional<std::string> write_stored_zip(std::span<const ZipSource> files)
    {
        if (files.size() >= MAX_ENTRIES)
        {
            return std::nullopt;
        }
        std::string out;
        std::string central;
        for (const ZipSource &file : files)
        {
            const std::uint64_t end =
                std::uint64_t{out.size()} + LOCAL_HEADER_SIZE + file.name.size() + file.data.size();
            if (file.name.size() > MAX_NAME || end > MAX_OFFSET)
            {
                return std::nullopt;
            }
            const auto offset = static_cast<std::uint32_t>(out.size());
            const auto size = static_cast<std::uint32_t>(file.data.size());
            const auto name_length = static_cast<std::uint16_t>(file.name.size());
            const std::uint32_t crc =
                zip_crc32({reinterpret_cast<const std::uint8_t *>(file.data.data()), file.data.size()});
            const std::uint16_t flags = is_non_ascii_utf8(file.name) ? FLAG_UTF8_NAME : std::uint16_t{0};

            put<std::uint32_t>(out, LOCAL_SIGNATURE);
            put_entry_fields(out, flags, crc, size, name_length);
            out += file.name;
            out += file.data;

            put<std::uint32_t>(central, CENTRAL_SIGNATURE);
            put<std::uint16_t>(central, ZIP_VERSION); // made by
            put_entry_fields(central, flags, crc, size, name_length);
            put<std::uint16_t>(central, 0); // comment length
            put<std::uint16_t>(central, 0); // disk
            put<std::uint16_t>(central, 0); // internal attributes
            put<std::uint32_t>(central, 0); // external attributes
            put<std::uint32_t>(central, offset);
            central += file.name;
        }
        if (std::uint64_t{out.size()} + central.size() + END_RECORD_SIZE > MAX_OFFSET)
        {
            return std::nullopt;
        }
        const auto central_offset = static_cast<std::uint32_t>(out.size());
        const auto count = static_cast<std::uint16_t>(files.size());
        out += central;
        put<std::uint32_t>(out, END_SIGNATURE);
        put<std::uint16_t>(out, 0); // this disk
        put<std::uint16_t>(out, 0); // the central directory's disk
        put<std::uint16_t>(out, count);
        put<std::uint16_t>(out, count);
        put<std::uint32_t>(out, static_cast<std::uint32_t>(central.size()));
        put<std::uint32_t>(out, central_offset);
        put<std::uint16_t>(out, 0); // comment length
        return out;
    }
} // namespace AnimationFramework
