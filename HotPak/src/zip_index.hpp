/**
 * @file zip_index.hpp
 * @brief Reads a pak's central directory (names, CRC-32s and sizes) without inflating any entry.
 */
#ifndef HOTPAK_ZIP_INDEX_HPP
#define HOTPAK_ZIP_INDEX_HPP

#include <DetourModKit.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace HotPak
{
    /** @brief One central-directory record. */
    struct ZipEntry
    {
        /// Normalized entry name (lowercase, forward slashes).
        std::string name;
        std::uint32_t crc32 = 0;
        std::uint32_t size = 0;
    };

    /**
     * @brief Reads the central directory of the zip at @p path.
     * @details Only the end-of-central-directory record and the directory itself are read, so a 40 MB pak costs a
     *          few kilobytes of I/O. ZIP64 archives are rejected: a KCD pak never needs one.
     * @return The entries, or ErrorCode::Unknown with a short reason.
     */
    [[nodiscard]] DMK::Result<std::vector<ZipEntry>> read_zip_index(const std::wstring &path);

    /** @brief The entries of @p next that are new or differ from @p previous by CRC or size. */
    [[nodiscard]] std::vector<ZipEntry>
    changed_entries(const std::vector<ZipEntry> &previous, const std::vector<ZipEntry> &next);

} // namespace HotPak

#endif // HOTPAK_ZIP_INDEX_HPP
