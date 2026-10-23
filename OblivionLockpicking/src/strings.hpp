/**
 * @file strings.hpp
 * @brief Helpers for the INI's text values: case-insensitive names and trimmed list items.
 */
#ifndef OBLIVIONLP_STRINGS_HPP
#define OBLIVIONLP_STRINGS_HPP

#include <algorithm>
#include <string_view>

namespace OblivionLockpicking::strings
{
    /** @brief @p c in lower case, for an ASCII letter, or @p c unchanged. */
    [[nodiscard]] constexpr char lower(char c) noexcept
    {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }

    /** @brief True when @p a and @p b are the same text when the case of ASCII letters does not count. */
    [[nodiscard]] constexpr bool equals_ignoring_case(std::string_view a, std::string_view b) noexcept
    {
        return a.size() == b.size() &&
               std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return lower(x) == lower(y); });
    }

    /** @brief @p text without its leading and trailing spaces, tabs and line breaks. */
    [[nodiscard]] constexpr std::string_view trim(std::string_view text) noexcept
    {
        const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
        while (!text.empty() && blank(text.front()))
        {
            text.remove_prefix(1);
        }
        while (!text.empty() && blank(text.back()))
        {
            text.remove_suffix(1);
        }
        return text;
    }
} // namespace OblivionLockpicking::strings

#endif // OBLIVIONLP_STRINGS_HPP
