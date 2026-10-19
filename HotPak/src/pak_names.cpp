/**
 * @file pak_names.cpp
 * @brief Pak entry, shadow-copy and INI pak-list name rules.
 */

#include "pak_names.hpp"
#include "constants.hpp"
#include "text_util.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace HotPak
{
    namespace
    {
        constexpr std::string_view TEXTURE_EXTENSION = ".dds";
        constexpr std::string_view SKIN_EXTENSION = ".skin";
        constexpr std::string_view MATERIAL_EXTENSION = ".mtl";
        constexpr std::string_view SCRIPT_EXTENSION = ".lua";
        constexpr std::string_view TABLE_EXTENSION = ".xml";
        constexpr std::string_view TABLES_PREFIX = "libs/tables/";
        constexpr std::string_view TABLE_PATCH_SEPARATOR = "__";
        constexpr std::string_view LOD_MARKER = "_lod";
        constexpr std::string_view PAK_EXTENSION = ".pak";
        constexpr std::wstring_view SHADOW_EXTENSION = L".pak";
        constexpr std::wstring_view SHADOW_GENERATION_MARKER = L".g";
        // A uint32 generation has at most ten digits, and nine always fit without overflow.
        constexpr std::size_t MAX_GENERATION_DIGITS = 9;

        [[nodiscard]] std::string_view trim(std::string_view text) noexcept
        {
            constexpr std::string_view blank = " \t\r\n\"";
            const std::size_t begin = text.find_first_not_of(blank);
            if (begin == std::string_view::npos)
            {
                return {};
            }
            const std::size_t end = text.find_last_not_of(blank);
            return text.substr(begin, end - begin + 1);
        }

        [[nodiscard]] bool ends_with_ignoring_case(std::string_view text, std::string_view suffix) noexcept
        {
            return text.size() >= suffix.size() && iequals(text.substr(text.size() - suffix.size()), suffix);
        }
    } // namespace

    std::string texture_base(std::string_view name)
    {
        const std::size_t at = name.rfind(TEXTURE_EXTENSION);
        if (at == std::string_view::npos)
        {
            return {};
        }
        const std::string_view tail = name.substr(at + TEXTURE_EXTENSION.size());
        if (!tail.empty() && tail.front() != '.')
        {
            return {};
        }
        return std::string(name.substr(0, at + TEXTURE_EXTENSION.size()));
    }

    std::string skin_base(std::string_view name)
    {
        if (!name.ends_with(SKIN_EXTENSION))
        {
            return {};
        }
        std::string_view stem = name.substr(0, name.size() - SKIN_EXTENSION.size());
        const std::size_t lod_length = LOD_MARKER.size() + 1;
        if (stem.size() > lod_length && stem.substr(stem.size() - lod_length, LOD_MARKER.size()) == LOD_MARKER &&
            stem.back() >= '0' && stem.back() <= '9' && stem[stem.size() - lod_length - 1] != '/')
        {
            stem.remove_suffix(lod_length);
        }
        return std::string(stem) + std::string(SKIN_EXTENSION);
    }

    std::string material_key(std::string_view name)
    {
        if (!name.ends_with(MATERIAL_EXTENSION) || name.size() == MATERIAL_EXTENSION.size())
        {
            return {};
        }
        return std::string(name.substr(0, name.size() - MATERIAL_EXTENSION.size()));
    }

    std::string_view mod_script_id(std::string_view name) noexcept
    {
        const std::string_view prefix = constants::MOD_SCRIPTS_PREFIX;
        if (!name.starts_with(prefix) || !name.ends_with(SCRIPT_EXTENSION))
        {
            return {};
        }
        const std::string_view id = name.substr(prefix.size(), name.size() - prefix.size() - SCRIPT_EXTENSION.size());
        return id.find('/') == std::string_view::npos ? id : std::string_view{};
    }

    bool is_plain_script_id(std::string_view id) noexcept
    {
        return !id.empty() && std::all_of(
                                  id.begin(),
                                  id.end(),
                                  [](char c)
                                  {
                                      return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                             (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
                                  }
                              );
    }

    std::optional<TableRef> table_of(std::string_view name) noexcept
    {
        if (!name.starts_with(TABLES_PREFIX) || !name.ends_with(TABLE_EXTENSION))
        {
            return std::nullopt;
        }
        const std::string_view relative = name.substr(TABLES_PREFIX.size());
        const std::size_t slash = relative.rfind('/');
        const std::string_view subfolder =
            slash == std::string_view::npos ? std::string_view{} : relative.substr(0, slash);
        std::string_view stem = relative.substr(slash == std::string_view::npos ? 0 : slash + 1);
        stem.remove_suffix(TABLE_EXTENSION.size());
        if (const std::size_t patch = stem.find(TABLE_PATCH_SEPARATOR); patch != std::string_view::npos)
        {
            stem = stem.substr(0, patch);
        }
        if (stem.empty())
        {
            return std::nullopt;
        }
        return TableRef{
            .subfolder = subfolder,
            .name = stem,
        };
    }

    std::wstring shadow_file_name(std::wstring_view source_path, std::uint32_t generation)
    {
        const std::size_t slash = source_path.find_last_of(L"\\/");
        std::wstring_view stem = slash == std::wstring_view::npos ? source_path : source_path.substr(slash + 1);
        if (const std::size_t dot = stem.find_last_of(L'.'); dot != std::wstring_view::npos)
        {
            stem = stem.substr(0, dot);
        }
        return std::format(L"{}{}{:04}{}", stem, SHADOW_GENERATION_MARKER, generation, SHADOW_EXTENSION);
    }

    std::uint32_t shadow_generation_of(std::wstring_view file_name) noexcept
    {
        if (!file_name.ends_with(SHADOW_EXTENSION))
        {
            return 0;
        }
        const std::wstring_view name = file_name.substr(0, file_name.size() - SHADOW_EXTENSION.size());
        const std::size_t marker = name.rfind(SHADOW_GENERATION_MARKER);
        if (marker == std::wstring_view::npos)
        {
            return 0;
        }
        const std::wstring_view digits = name.substr(marker + SHADOW_GENERATION_MARKER.size());
        if (digits.empty() || digits.size() > MAX_GENERATION_DIGITS)
        {
            return 0;
        }
        std::uint32_t value = 0;
        for (const wchar_t c : digits)
        {
            if (c < L'0' || c > L'9')
            {
                return 0;
            }
            value = value * 10 + static_cast<std::uint32_t>(c - L'0');
        }
        return value;
    }

    PakList parse_pak_list(std::string_view text)
    {
        PakList list;
        while (!text.empty())
        {
            const std::size_t separator = text.find(';');
            const std::string_view item = trim(text.substr(0, separator));
            if (!item.empty())
            {
                if (ends_with_ignoring_case(item, PAK_EXTENSION))
                {
                    list.paths.push_back(to_wide(item));
                }
                else
                {
                    list.rejected.emplace_back(item);
                }
            }
            if (separator == std::string_view::npos)
            {
                break;
            }
            text.remove_prefix(separator + 1);
        }
        return list;
    }

} // namespace HotPak
