/**
 * @file core/zip_reader.hpp
 * @brief Reads entries straight out of a pak (a plain zip) without CryPak, so the animation framework
 *        can see every mod's own copy of a file, not only the copy CryPak serves.
 */
#ifndef ANIMFW_CORE_ZIP_READER_HPP
#define ANIMFW_CORE_ZIP_READER_HPP

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace AnimationFramework
{
    /** @brief One central-directory record. */
    struct ZipEntry
    {
        /// Lowercased, forward slashes.
        std::string name;
        std::uint16_t method = 0;
        std::uint32_t crc32 = 0;
        std::uint32_t compressed_size = 0;
        std::uint32_t size = 0;
        std::uint32_t local_offset = 0;
    };

    /** @brief The zip's central directory. Nothing for an unreadable file or a ZIP64 archive. */
    [[nodiscard]] std::optional<std::vector<ZipEntry>> read_zip_index(const std::filesystem::path &path);

    /**
     * @brief One entry's bytes: stored, or deflated (method 8) and inflated here, then checked against the
     *        directory's CRC-32. Nothing on any mismatch.
     */
    [[nodiscard]] std::optional<std::string> read_zip_file(const std::filesystem::path &path, const ZipEntry &entry);

    /** @brief The CRC-32 a zip records for @p data (IEEE, reflected: the zlib one). */
    [[nodiscard]] std::uint32_t zip_crc32(std::span<const std::uint8_t> data) noexcept;

} // namespace AnimationFramework

#endif // ANIMFW_CORE_ZIP_READER_HPP
