/**
 * @file packs/ini_document.cpp
 * @brief The pack INI reader.
 */

#include "packs/ini_document.hpp"

#include <algorithm>
#include <format>

namespace SoundFramework::packs
{
    namespace
    {
        constexpr std::string_view UTF8_BOM = "\xEF\xBB\xBF";

        [[nodiscard]] constexpr bool is_space(char c) noexcept
        {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n';
        }

        [[nodiscard]] constexpr char lower(char c) noexcept
        {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        }

        /** @brief Drops a trailing comment: `;` or `#` preceded by whitespace. */
        [[nodiscard]] std::string_view strip_inline_comment(std::string_view value) noexcept
        {
            for (std::size_t i = 1; i < value.size(); ++i)
            {
                if ((value[i] == ';' || value[i] == '#') && is_space(value[i - 1]))
                {
                    return value.substr(0, i);
                }
            }
            return value;
        }
    } // namespace

    std::string_view trim(std::string_view text) noexcept
    {
        while (!text.empty() && is_space(text.front()))
        {
            text.remove_prefix(1);
        }
        while (!text.empty() && is_space(text.back()))
        {
            text.remove_suffix(1);
        }
        return text;
    }

    bool iequals(std::string_view a, std::string_view b) noexcept
    {
        return std::ranges::equal(a, b, [](char x, char y) { return lower(x) == lower(y); });
    }

    IniDocument parse_ini(std::string_view text)
    {
        IniDocument document;
        if (text.starts_with(UTF8_BOM))
        {
            text.remove_prefix(UTF8_BOM.size());
        }
        int line_number = 0;
        while (!text.empty())
        {
            const std::size_t end = text.find('\n');
            const std::string_view raw = text.substr(0, end);
            text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
            ++line_number;

            const std::string_view line = trim(raw);
            if (line.empty() || line.front() == ';' || line.front() == '#')
            {
                continue;
            }
            if (line.front() == '[')
            {
                const std::size_t close = line.find(']');
                const std::string_view name =
                    close == std::string_view::npos ? std::string_view{} : trim(line.substr(1, close - 1));
                if (name.empty())
                {
                    document.problems.push_back(std::format("line {}: malformed section header", line_number));
                    continue;
                }
                document.sections.push_back(
                    IniSection{
                        .name = std::string(name),
                        .line = line_number,
                    }
                );
                continue;
            }
            const std::size_t equals = line.find('=');
            if (equals == std::string_view::npos)
            {
                document.problems.push_back(std::format("line {}: expected key = value", line_number));
                continue;
            }
            const std::string_view key = trim(line.substr(0, equals));
            if (key.empty())
            {
                document.problems.push_back(std::format("line {}: empty key", line_number));
                continue;
            }
            if (document.sections.empty())
            {
                document.problems.push_back(std::format("line {}: '{}' is outside any [section]", line_number, key));
                continue;
            }
            const std::string_view value = trim(strip_inline_comment(trim(line.substr(equals + 1))));
            document.sections.back().entries.push_back(
                IniEntry{
                    .key = std::string(key),
                    .value = std::string(value),
                    .line = line_number,
                }
            );
        }
        return document;
    }
} // namespace SoundFramework::packs
