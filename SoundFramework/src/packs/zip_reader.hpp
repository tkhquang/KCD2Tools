/**
 * @file packs/zip_reader.hpp
 * @brief Reads entries straight out of a game pak (a plain zip) without CryPak, from a worker thread and for paks the
 *        game has not mounted, so the voice line catalog can be built off the main thread.
 */
#ifndef SNDFW_PACKS_ZIP_READER_HPP
#define SNDFW_PACKS_ZIP_READER_HPP

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace SoundFramework::packs
{
    /** @brief One central-directory record. */
    struct ZipEntry
    {
        /// As the pak spells it, with forward slashes.
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
     * @brief One entry's bytes: stored, or deflated (method 8) and inflated here, then checked against the directory's
     *        CRC-32. Nothing on any mismatch.
     * @param file The pak, open in binary mode: a caller reading thousands of entries opens it once.
     */
    [[nodiscard]] std::optional<std::string> read_zip_file(std::ifstream &file, const ZipEntry &entry);
} // namespace SoundFramework::packs

#endif // SNDFW_PACKS_ZIP_READER_HPP
