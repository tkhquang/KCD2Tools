/**
 * @file packs/ini_document.hpp
 * @brief A small INI reader for sound pack definitions: sections of `key = value` lines, kept in file order.
 *
 * Pack files name their own sections (one per sound), so they cannot go through the bound-key config reader the
 * framework uses for its own settings.
 */
#ifndef SNDFW_PACKS_INI_DOCUMENT_HPP
#define SNDFW_PACKS_INI_DOCUMENT_HPP

#include <string>
#include <string_view>
#include <vector>

namespace SoundFramework::packs
{
    struct IniEntry
    {
        std::string key;
        std::string value;
        int line = 0;
    };

    struct IniSection
    {
        std::string name;
        int line = 0;
        std::vector<IniEntry> entries;
    };

    struct IniDocument
    {
        std::vector<IniSection> sections;
        /// One message per unreadable line, prefixed with its line number.
        std::vector<std::string> problems;
    };

    /**
     * @brief Reads INI text: `[section]` headers, `key = value` lines, and comments that start a line with `;` or `#`
     *        or follow whitespace after a value.
     * @note A UTF-8 byte order mark is skipped. Keys and section names keep their spelling; compare them with
     *       @ref iequals.
     */
    [[nodiscard]] IniDocument parse_ini(std::string_view text);

    /** @brief ASCII case-insensitive equality. */
    [[nodiscard]] bool iequals(std::string_view a, std::string_view b) noexcept;

    /** @brief @p text without leading and trailing spaces, tabs and line ends. */
    [[nodiscard]] std::string_view trim(std::string_view text) noexcept;
} // namespace SoundFramework::packs

#endif // SNDFW_PACKS_INI_DOCUMENT_HPP
