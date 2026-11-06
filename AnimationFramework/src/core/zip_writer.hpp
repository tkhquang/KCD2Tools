/**
 * @file core/zip_writer.hpp
 * @brief Writes a plain zip of stored (uncompressed) entries, the form of pak CryPak mounts, so the framework can hand
 *        the game loose files from its DEV folder.
 */

#ifndef ANIMFW_CORE_ZIP_WRITER_HPP
#define ANIMFW_CORE_ZIP_WRITER_HPP

#include <optional>
#include <span>
#include <string>

namespace AnimationFramework
{
    /** @brief One file for write_stored_zip. */
    struct ZipSource
    {
        /**
         * @brief The entry's path inside the zip, forward slashes, written byte for byte. A non-ASCII name that is
         *        well-formed UTF-8 carries the zip's UTF-8 flag. Any other name carries no encoding flag.
         */
        std::string name;
        std::string data;
    };

    /**
     * @brief A zip holding @p files stored, in their order. Nothing when it needs ZIP64 (65,535 entries or more, or
     *        4 GB).
     */
    [[nodiscard]] std::optional<std::string> write_stored_zip(std::span<const ZipSource> files);
} // namespace AnimationFramework

#endif // ANIMFW_CORE_ZIP_WRITER_HPP
