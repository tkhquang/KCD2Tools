/**
 * @file packs/cutscene_table.cpp
 * @brief Cutscene speaker table parsing.
 */

#include "packs/cutscene_table.hpp"

#include <algorithm>
#include <optional>

namespace SoundFramework::packs
{
    namespace
    {
        [[nodiscard]] char lower(char c) noexcept
        {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        }

        [[nodiscard]] std::string_view trimmed(std::string_view text) noexcept
        {
            const std::size_t first = text.find_first_not_of(" \t\r");
            if (first == std::string_view::npos)
            {
                return {};
            }
            return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
        }

        [[nodiscard]] std::optional<CutsceneMode> mode_of(std::string_view name) noexcept
        {
            if (name == "speaker")
            {
                return CutsceneMode::Speaker;
            }
            if (name == "player")
            {
                return CutsceneMode::Player;
            }
            if (name == "faces")
            {
                return CutsceneMode::Faces;
            }
            return std::nullopt;
        }
    } // namespace

    CutsceneTable parse_cutscene_table(std::string_view text, std::size_t &rejected)
    {
        CutsceneTable table;
        rejected = 0;
        while (!text.empty())
        {
            const std::size_t end = text.find('\n');
            const std::string_view row = text.substr(0, end);
            text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
            if (trimmed(row).empty() || trimmed(row).front() == '#')
            {
                continue;
            }
            const std::size_t first_tab = row.find('\t');
            const std::size_t second_tab =
                first_tab == std::string_view::npos ? first_tab : row.find('\t', first_tab + 1);
            const std::string_view line = trimmed(row.substr(0, first_tab));
            const std::optional<CutsceneMode> mode =
                first_tab == std::string_view::npos
                    ? std::nullopt
                    : mode_of(trimmed(row.substr(first_tab + 1, second_tab - first_tab - 1)));
            if (line.empty() || !mode.has_value())
            {
                ++rejected;
                continue;
            }
            CutsceneEntry entry{
                .mode = *mode,
            };
            std::string_view actors =
                second_tab == std::string_view::npos ? std::string_view{} : row.substr(second_tab + 1);
            while (!actors.empty())
            {
                const std::size_t comma = actors.find(',');
                const std::string_view actor = trimmed(actors.substr(0, comma));
                if (!actor.empty())
                {
                    entry.actors.emplace_back(actor);
                }
                actors = comma == std::string_view::npos ? std::string_view{} : actors.substr(comma + 1);
            }
            if (entry.actors.empty() && entry.mode != CutsceneMode::Player)
            {
                ++rejected;
                continue;
            }
            table[cutscene_line_id(line)].push_back(std::move(entry));
        }
        return table;
    }

    std::string cutscene_line_id(std::string_view voice_path)
    {
        const std::size_t slash = voice_path.find_last_of("/\\");
        std::string_view name = slash == std::string_view::npos ? voice_path : voice_path.substr(slash + 1);
        const std::size_t dot = name.rfind('.');
        if (dot != std::string_view::npos)
        {
            name = name.substr(0, dot);
        }
        std::string id(name);
        std::ranges::transform(id, id.begin(), lower);
        return id;
    }
} // namespace SoundFramework::packs
