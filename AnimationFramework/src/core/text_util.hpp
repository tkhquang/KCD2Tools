/**
 * @file core/text_util.hpp
 * @brief ASCII case folding, Mannequin tag sets and allocation-free path tests shared by the merge and the detours.
 *
 * Every fold is ASCII only and ignores the C locale, so a path compares the same however the game set its locale.
 */
#ifndef ANIMFW_CORE_TEXT_UTIL_HPP
#define ANIMFW_CORE_TEXT_UTIL_HPP

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace AnimationFramework
{
    /** @brief @p c with 'A' to 'Z' lowered. */
    [[nodiscard]] constexpr char ascii_lower(char c) noexcept
    {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }

    /** @brief @p text with 'A' to 'Z' lowered. */
    [[nodiscard]] inline std::string lowercase(std::string_view text)
    {
        std::string out{text};
        std::ranges::transform(out, out.begin(), ascii_lower);
        return out;
    }

    /**
     * @brief True for a mod's id as the game takes it: lowercase letters and underscores.
     * @details The game skips a mod whose manifest mod id holds anything else, digits included ("ModId accepts only
     *          lowercase letters and underscore").
     */
    [[nodiscard]] constexpr bool is_mod_id(std::string_view text) noexcept
    {
        constexpr std::size_t MAX_LENGTH = 64;
        if (text.empty() || text.size() > MAX_LENGTH)
        {
            return false;
        }
        return std::ranges::all_of(text, [](char c) { return (c >= 'a' && c <= 'z') || c == '_'; });
    }

    /** @brief @p c lowered, with a backslash read as a forward slash. */
    [[nodiscard]] constexpr char path_fold(char c) noexcept
    {
        return c == '\\' ? '/' : ascii_lower(c);
    }

    /** @brief @p path with path_fold applied to every character: lowercase, forward slashes. */
    [[nodiscard]] inline std::string fold_path(std::string_view path)
    {
        std::string out{path};
        std::ranges::transform(out, out.begin(), path_fold);
        return out;
    }

    /** @brief True when @p a and @p b name the same path text, ignoring ASCII case and the slash direction. */
    [[nodiscard]] constexpr bool path_equals(std::string_view a, std::string_view b) noexcept
    {
        return a.size() == b.size() &&
               std::ranges::equal(a, b, [](char x, char y) { return path_fold(x) == path_fold(y); });
    }

    /** @brief True when @p path ends with @p suffix, ignoring ASCII case and the slash direction. */
    [[nodiscard]] constexpr bool path_ends_with(std::string_view path, std::string_view suffix) noexcept
    {
        return path.size() >= suffix.size() && path_equals(path.substr(path.size() - suffix.size()), suffix);
    }

    /** @brief True when @p path contains @p needle, ignoring ASCII case and the slash direction. */
    [[nodiscard]] constexpr bool path_contains(std::string_view path, std::string_view needle) noexcept
    {
        if (needle.size() > path.size())
        {
            return false;
        }
        for (std::size_t at = 0; at + needle.size() <= path.size(); ++at)
        {
            if (path_equals(path.substr(at, needle.size()), needle))
            {
                return true;
            }
        }
        return false;
    }

    /**
     * @brief The mod id of the pak CryPak mounted at @p pak, lowercased, or empty for a pak that is not a mod's.
     * @details A mod pak mounts as `mods\<modid>\...` relative to the game root, or as that path under @p game_root
     *          when CryPak holds it absolute. Only the part after @p game_root counts, so a game installed under a
     *          folder named `mods` does not make the game's own `data\` paks read as mods'. Case and slash direction
     *          are ignored.
     */
    [[nodiscard]] inline std::string pak_mod_id(std::string_view pak, std::string_view game_root)
    {
        while (!game_root.empty() && path_fold(game_root.back()) == '/')
        {
            game_root.remove_suffix(1);
        }
        if (!game_root.empty() && pak.size() > game_root.size() && path_fold(pak[game_root.size()]) == '/' &&
            path_equals(pak.substr(0, game_root.size()), game_root))
        {
            pak.remove_prefix(game_root.size() + 1);
        }
        constexpr std::string_view MODS = "mods/";
        if (pak.size() <= MODS.size() || !path_equals(pak.substr(0, MODS.size()), MODS))
        {
            return {};
        }
        pak.remove_prefix(MODS.size());
        const std::size_t end = pak.find_first_of("\\/");
        return end == std::string_view::npos || end == 0 ? std::string{} : lowercase(pak.substr(0, end));
    }

    /** @brief The parts of a '+'-joined Mannequin tag list as written, empty parts ("a++b", an outer '+') dropped. */
    [[nodiscard]] inline std::vector<std::string> split_tags(std::string_view tags)
    {
        std::vector<std::string> parts;
        std::size_t start = 0;
        while (start <= tags.size())
        {
            const std::size_t end = std::min(tags.find('+', start), tags.size());
            if (end > start)
            {
                parts.emplace_back(tags.substr(start, end - start));
            }
            start = end + 1;
        }
        return parts;
    }

    /** @brief A Mannequin tag list as a set: split_tags lowercased, sorted and '+' joined, so "b+a" equals "A+B". */
    [[nodiscard]] inline std::string tag_set(std::string_view tags)
    {
        std::vector<std::string> parts = split_tags(tags);
        for (std::string &part : parts)
        {
            std::ranges::transform(part, part.begin(), ascii_lower);
        }
        std::ranges::sort(parts);
        std::string out;
        for (const std::string &part : parts)
        {
            if (!out.empty())
            {
                out += '+';
            }
            out += part;
        }
        return out;
    }
} // namespace AnimationFramework

#endif // ANIMFW_CORE_TEXT_UTIL_HPP
