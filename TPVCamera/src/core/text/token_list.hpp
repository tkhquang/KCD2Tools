/**
 * @file core/text/token_list.hpp
 * @brief The comma-separated token lists of the INI settings (SuppressTPVState, HideScreenEffectsTPV, ...): split,
 *        trim and an ASCII case-insensitive compare that does not depend on the C locale.
 */
#ifndef TPVCAMERA_CORE_TEXT_TOKEN_LIST_HPP
#define TPVCAMERA_CORE_TEXT_TOKEN_LIST_HPP

#include <concepts>
#include <cstddef>
#include <string_view>

namespace TPVCamera::TokenList
{
    /** @brief @p text without its leading and trailing spaces, tabs and line breaks. */
    [[nodiscard]] constexpr std::string_view trim(std::string_view text) noexcept
    {
        constexpr std::string_view whitespace = " \t\r\n";
        const std::size_t begin = text.find_first_not_of(whitespace);
        if (begin == std::string_view::npos)
        {
            return {};
        }
        return text.substr(begin, text.find_last_not_of(whitespace) - begin + 1);
    }

    /** @brief True when @p token equals @p lower_literal with ASCII letters compared case-insensitively. */
    [[nodiscard]] constexpr bool equals(std::string_view token, std::string_view lower_literal) noexcept
    {
        if (token.size() != lower_literal.size())
        {
            return false;
        }
        for (std::size_t i = 0; i < token.size(); ++i)
        {
            const char c = token[i];
            const char folded = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
            if (folded != lower_literal[i])
            {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief Calls @p visit with each trimmed, non-empty token of the comma-separated list @p csv, in order.
     * @details An empty list or a list of separators calls nothing.
     */
    template <std::invocable<std::string_view> Visit> constexpr void for_each(std::string_view csv, Visit &&visit)
    {
        std::size_t start = 0;
        while (start <= csv.size())
        {
            const std::size_t comma = csv.find(',', start);
            const std::size_t end = comma == std::string_view::npos ? csv.size() : comma;
            const std::string_view token = trim(csv.substr(start, end - start));
            if (!token.empty())
            {
                visit(token);
            }
            if (comma == std::string_view::npos)
            {
                return;
            }
            start = comma + 1;
        }
    }
} // namespace TPVCamera::TokenList

#endif // TPVCAMERA_CORE_TEXT_TOKEN_LIST_HPP
