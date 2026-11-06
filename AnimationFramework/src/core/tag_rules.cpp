/**
 * @file core/tag_rules.cpp
 * @brief The custom tag rules (see tag_rules.hpp).
 */

#include "core/tag_rules.hpp"
#include "core/text_util.hpp"

#include <algorithm>
#include <bit>

namespace AnimationFramework::tag_rules
{
    bool covers(std::span<const std::uint8_t> view, std::span<const std::uint8_t> plain) noexcept
    {
        if (view.size() < plain.size())
        {
            return false;
        }
        for (std::size_t i = 0; i < plain.size(); ++i)
        {
            if ((view[i] & plain[i]) != plain[i])
            {
                return false;
            }
        }
        return true;
    }

    std::uint32_t capped(std::uint32_t active) noexcept
    {
        while (std::popcount(active) > MAX_ACTIVE_TAGS)
        {
            // The lowest set bit is the lowest-ranked active tag.
            active &= active - 1u;
        }
        return active;
    }

    std::vector<std::string> parse_tag_list(std::string_view text)
    {
        std::vector<std::string> names;
        std::size_t start = 0;
        while (start < text.size())
        {
            const std::size_t end = text.find_first_of(",; \t", start);
            const std::string name = lowercase(text.substr(start, end == std::string_view::npos ? end : end - start));
            if (!name.empty() && std::ranges::find(names, name) == names.end())
            {
                names.push_back(name);
            }
            if (end == std::string_view::npos)
            {
                break;
            }
            start = end + 1;
        }
        return names;
    }

    std::vector<std::size_t> rank(
        std::span<const Declared> declared,
        std::span<const std::string> ini_priority,
        std::span<const std::string> disabled
    )
    {
        std::vector<std::size_t> kept;
        std::vector<std::string> seen;
        for (std::size_t i = 0; i < declared.size(); ++i)
        {
            const std::string name = lowercase(declared[i].name);
            if (name.empty() || std::ranges::find(seen, name) != seen.end() ||
                std::ranges::find(disabled, name) != disabled.end())
            {
                continue;
            }
            seen.push_back(name);
            kept.push_back(i);
        }
        // Highest first: INI position, then declared priority, then declaration order.
        const auto ini_position = [&](std::size_t index) -> std::size_t
        {
            const auto found = std::ranges::find(ini_priority, lowercase(declared[index].name));
            return found == ini_priority.end() ? ini_priority.size()
                                               : static_cast<std::size_t>(found - ini_priority.begin());
        };
        std::ranges::stable_sort(
            kept,
            [&](std::size_t a, std::size_t b) -> bool
            {
                const std::size_t pa = ini_position(a);
                const std::size_t pb = ini_position(b);
                if (pa != pb)
                {
                    return pa < pb;
                }
                return declared[a].priority > declared[b].priority;
            }
        );
        std::ranges::reverse(kept);
        return kept;
    }
} // namespace AnimationFramework::tag_rules
