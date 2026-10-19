/**
 * @file text_util.hpp
 * @brief Path normalization and UTF-8 / UTF-16 conversion shared by the watcher and the engine layer.
 */
#ifndef HOTPAK_TEXT_UTIL_HPP
#define HOTPAK_TEXT_UTIL_HPP

#include <windows.h>

#include <string>
#include <string_view>

namespace HotPak
{
    /**
     * @brief Lowercases ASCII and turns backslashes into forward slashes.
     * @details CryPak entry names, the skin cache keys and the paths this tool compares are all case-insensitive
     *          relative paths, so every comparison runs on this form.
     */
    [[nodiscard]] inline std::string normalize_path(std::string_view path)
    {
        std::string out(path);
        for (char &c : out)
        {
            if (c == '\\')
            {
                c = '/';
            }
            else if (c >= 'A' && c <= 'Z')
            {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        return out;
    }

    /** @brief Converts UTF-16 to UTF-8; an unconvertible input yields an empty string. */
    [[nodiscard]] inline std::string to_utf8(std::wstring_view text)
    {
        if (text.empty())
        {
            return {};
        }
        const int length =
            WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        if (length <= 0)
        {
            return {};
        }
        std::string out(static_cast<std::size_t>(length), '\0');
        WideCharToMultiByte(
            CP_UTF8,
            0,
            text.data(),
            static_cast<int>(text.size()),
            out.data(),
            length,
            nullptr,
            nullptr
        );
        return out;
    }

    /** @brief Converts UTF-8 to UTF-16; an unconvertible input yields an empty string. */
    [[nodiscard]] inline std::wstring to_wide(std::string_view text)
    {
        if (text.empty())
        {
            return {};
        }
        const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (length <= 0)
        {
            return {};
        }
        std::wstring out(static_cast<std::size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
        return out;
    }

    /** @brief Returns the file name part of a path with either separator. */
    [[nodiscard]] inline std::string_view file_name_of(std::string_view path) noexcept
    {
        const std::size_t slash = path.find_last_of("/\\");
        return slash == std::string_view::npos ? path : path.substr(slash + 1);
    }

    /** @brief Case-insensitive ASCII equality. */
    [[nodiscard]] inline bool iequals(std::string_view a, std::string_view b) noexcept
    {
        if (a.size() != b.size())
        {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            char x = a[i];
            char y = b[i];
            if (x >= 'A' && x <= 'Z')
            {
                x = static_cast<char>(x - 'A' + 'a');
            }
            if (y >= 'A' && y <= 'Z')
            {
                y = static_cast<char>(y - 'A' + 'a');
            }
            if (x != y)
            {
                return false;
            }
        }
        return true;
    }
} // namespace HotPak

#endif // HOTPAK_TEXT_UTIL_HPP
