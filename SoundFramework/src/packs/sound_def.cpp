/**
 * @file packs/sound_def.cpp
 * @brief Pack INI parsing into sound definitions.
 */

#include "packs/sound_def.hpp"
#include "packs/ini_document.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <utility>

namespace SoundFramework::packs
{
    namespace
    {
        struct BusAlias
        {
            std::string_view alias;
            std::string_view path;
        };

        // Buses of the game's mixer (kcd.bank). The "dieg" (diegetic) buses pause with the game and sit under the
        // Sound volume slider; "music" sits under the music slider.
        constexpr std::array BUS_ALIASES = {
            BusAlias{"world", "bus:/dieg/w_obj/obj"},
            BusAlias{"world_loud", "bus:/dieg/w_obj/obj_loud"},
            BusAlias{"player", "bus:/dieg/w_obj/player"},
            BusAlias{"voice", "bus:/dieg/dialog"},
            BusAlias{"barks", "bus:/dieg/w_obj/barks"},
            BusAlias{"ambience", "bus:/dieg/w_beds"},
            BusAlias{"world_music", "bus:/dieg/w_obj/music"},
            BusAlias{"music", "bus:/music"},
        };

        [[nodiscard]] std::optional<float> parse_float(std::string_view text) noexcept
        {
            float value = 0.0f;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size())
            {
                return std::nullopt;
            }
            return value;
        }

        [[nodiscard]] std::optional<int> parse_int(std::string_view text) noexcept
        {
            int value = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size())
            {
                return std::nullopt;
            }
            return value;
        }

        [[nodiscard]] std::optional<bool> parse_bool(std::string_view text) noexcept
        {
            if (iequals(text, "true") || iequals(text, "yes") || iequals(text, "on") || text == "1")
            {
                return true;
            }
            if (iequals(text, "false") || iequals(text, "no") || iequals(text, "off") || text == "0")
            {
                return false;
            }
            return std::nullopt;
        }

        [[nodiscard]] std::string lowered(std::string_view text)
        {
            std::string out(text);
            std::ranges::transform(
                out,
                out.begin(),
                [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
            );
            return out;
        }

        /** @brief Splits a comma-separated list into trimmed, non-empty items. */
        [[nodiscard]] std::vector<std::string_view> split_list(std::string_view text)
        {
            std::vector<std::string_view> items;
            while (!text.empty())
            {
                const std::size_t comma = text.find(',');
                const std::string_view item = trim(text.substr(0, comma));
                if (!item.empty())
                {
                    items.push_back(item);
                }
                text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
            }
            return items;
        }

        /** @brief Collects the problems of one section; the section is dropped when any is fatal. */
        class SectionReader
        {
        public:
            SectionReader(const IniSection &section, const std::filesystem::path &ini_path, PackParse &out)
                : m_section(section), m_ini_path(ini_path), m_out(out)
            {
            }

            void problem(int line, std::string_view message)
            {
                m_out.problems.push_back(
                    std::format("{}:{}: [{}] {}", path_text(m_ini_path), line, m_section.name, message)
                );
            }

            void fatal(int line, std::string_view message)
            {
                problem(line, message);
                m_failed = true;
            }

            [[nodiscard]] bool failed() const noexcept { return m_failed; }

            template <typename T> void number(const IniEntry &entry, T &target, T low, T high)
            {
                std::optional<T> value;
                if constexpr (std::is_same_v<T, int>)
                {
                    value = parse_int(entry.value);
                }
                else
                {
                    value = parse_float(entry.value);
                }
                if (!value.has_value())
                {
                    fatal(entry.line, std::format("{} = '{}' is not a number", entry.key, entry.value));
                    return;
                }
                if (*value < low || *value > high)
                {
                    problem(entry.line, std::format("{} = {} clamped to [{}, {}]", entry.key, *value, low, high));
                }
                target = std::clamp(*value, low, high);
            }

            void flag(const IniEntry &entry, bool &target)
            {
                const std::optional<bool> value = parse_bool(entry.value);
                if (!value.has_value())
                {
                    fatal(entry.line, std::format("{} = '{}' is not true or false", entry.key, entry.value));
                    return;
                }
                target = *value;
            }

        private:
            const IniSection &m_section;
            const std::filesystem::path &m_ini_path;
            PackParse &m_out;
            bool m_failed = false;
        };

        void read_entry(SectionReader &reader, const IniEntry &entry, SoundDef &sound, const std::filesystem::path &dir)
        {
            const std::string_view key = entry.key;
            if (iequals(key, "Files") || iequals(key, "File"))
            {
                for (const std::string_view item : split_list(entry.value))
                {
                    const std::filesystem::path relative =
                        std::filesystem::path(std::u8string(item.begin(), item.end()));
                    sound.files.push_back(relative.is_absolute() ? relative : (dir / relative).lexically_normal());
                }
            }
            else if (iequals(key, "Bus"))
            {
                std::optional<std::string> path = bus_path(entry.value);
                if (!path.has_value())
                {
                    reader.fatal(
                        entry.line,
                        std::format("Bus = '{}' is neither a known alias nor a bus:/ path", entry.value)
                    );
                    return;
                }
                sound.bus = std::move(*path);
            }
            else if (iequals(key, "Space"))
            {
                if (iequals(entry.value, "3d"))
                {
                    sound.space = Space::Positioned;
                }
                else if (iequals(entry.value, "2d"))
                {
                    sound.space = Space::Flat;
                }
                else
                {
                    reader.fatal(entry.line, std::format("Space = '{}' is neither 3d nor 2d", entry.value));
                }
            }
            else if (iequals(key, "Volume"))
            {
                reader.number(entry, sound.volume, 0.0f, 4.0f);
            }
            else if (iequals(key, "VolumeVariation"))
            {
                reader.number(entry, sound.volume_variation, 0.0f, 1.0f);
            }
            else if (iequals(key, "Pitch"))
            {
                reader.number(entry, sound.pitch, 0.25f, 4.0f);
            }
            else if (iequals(key, "PitchVariation"))
            {
                reader.number(entry, sound.pitch_variation, 0.0f, 0.75f);
            }
            else if (iequals(key, "MinDistance"))
            {
                reader.number(entry, sound.min_distance, 0.0f, 10000.0f);
            }
            else if (iequals(key, "MaxDistance"))
            {
                reader.number(entry, sound.max_distance, 0.0f, 10000.0f);
            }
            else if (iequals(key, "Loop"))
            {
                reader.flag(entry, sound.loop);
            }
            else if (iequals(key, "Stream"))
            {
                reader.flag(entry, sound.stream);
            }
            else if (iequals(key, "MaxInstances"))
            {
                reader.number(entry, sound.max_instances, 1, 64);
            }
            else if (iequals(key, "Cooldown"))
            {
                reader.number(entry, sound.cooldown, 0.0f, 3600.0f);
            }
            else if (iequals(key, "Replaces"))
            {
                for (const std::string_view item : split_list(entry.value))
                {
                    if (!item.starts_with("event:/") || item.size() <= 7)
                    {
                        reader.fatal(entry.line, std::format("Replaces item '{}' is not an event:/ path", item));
                        return;
                    }
                    sound.replaces.push_back(lowered(item));
                }
            }
            else if (iequals(key, "ReplacesFor"))
            {
                for (const std::string_view item : split_list(entry.value))
                {
                    sound.replaces_for.push_back(lowered(item));
                }
            }
            else if (iequals(key, "PlayAt"))
            {
                // "<parameter>" or "<parameter>:<lowest value>".
                const std::string_view value = entry.value;
                const std::size_t colon = value.find(':');
                const std::string_view parameter = trim(value.substr(0, colon));
                if (parameter.empty())
                {
                    reader.fatal(entry.line, "PlayAt needs a parameter name");
                    return;
                }
                if (colon != std::string_view::npos)
                {
                    const std::string_view number = trim(value.substr(colon + 1));
                    const std::optional<float> lowest = parse_float(number);
                    // from_chars also reads "nan" and "inf"; no game value ever reaches those.
                    if (!lowest.has_value() || !std::isfinite(*lowest))
                    {
                        reader.fatal(entry.line, std::format("PlayAt value '{}' is not a number", number));
                        return;
                    }
                    sound.play_at_value = *lowest;
                }
                sound.play_at = lowered(parameter);
            }
            else if (iequals(key, "KeepOriginal"))
            {
                reader.flag(entry, sound.keep_original);
            }
            else
            {
                reader.problem(entry.line, std::format("unknown key '{}' ignored", entry.key));
            }
        }
    } // namespace

    std::string path_text(const std::filesystem::path &path)
    {
        const std::u8string text = path.u8string();
        return std::string(text.begin(), text.end());
    }

    std::optional<std::string> bus_path(std::string_view value)
    {
        if (value.starts_with("bus:/"))
        {
            return std::string(value);
        }
        for (const BusAlias &alias : BUS_ALIASES)
        {
            if (iequals(alias.alias, value))
            {
                return std::string(alias.path);
            }
        }
        return std::nullopt;
    }

    bool valid_sound_name(std::string_view name) noexcept
    {
        return !name.empty() && name.size() <= 96 &&
               std::ranges::all_of(
                   name,
                   [](char c)
                   {
                       return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                              c == '.' || c == '-';
                   }
               );
    }

    bool wildcard_match(std::string_view pattern, std::string_view text) noexcept
    {
        // Greedy, backtracking only to the last '*'.
        std::size_t p = 0;
        std::size_t t = 0;
        std::size_t star = std::string_view::npos;
        std::size_t resume = 0;
        while (t < text.size())
        {
            if (p < pattern.size() && pattern[p] == '*')
            {
                star = p++;
                resume = t;
            }
            else if (p < pattern.size() && pattern[p] == text[t])
            {
                ++p;
                ++t;
            }
            else if (star != std::string_view::npos)
            {
                p = star + 1;
                t = ++resume;
            }
            else
            {
                return false;
            }
        }
        while (p < pattern.size() && pattern[p] == '*')
        {
            ++p;
        }
        return p == pattern.size();
    }

    namespace
    {
        [[nodiscard]] bool is_mood_section(const IniSection &section)
        {
            return std::ranges::any_of(
                section.entries,
                [](const IniEntry &entry) { return iequals(entry.key, "Poses"); }
            );
        }

        void read_mood(SectionReader &reader, const IniSection &section, MoodDef &mood)
        {
            for (const IniEntry &entry : section.entries)
            {
                if (iequals(entry.key, "Poses"))
                {
                    std::string error;
                    std::optional<std::vector<face::PoseWeight>> poses = face::parse_poses(entry.value, error);
                    if (!poses.has_value())
                    {
                        reader.fatal(entry.line, std::format("Poses: {}", error));
                        continue;
                    }
                    mood.poses = face::merge_poses(std::move(mood.poses), *poses);
                }
                else if (iequals(entry.key, "MaxStrength"))
                {
                    reader.number(entry, mood.max_strength, 0.0f, face::MAX_MOOD_STRENGTH);
                }
                else
                {
                    reader.problem(entry.line, std::format("unknown key '{}' ignored (mood)", entry.key));
                }
            }
        }

        [[nodiscard]] bool is_line_section(const IniSection &section)
        {
            return std::ranges::any_of(
                section.entries,
                [](const IniEntry &entry) { return iequals(entry.key, "Voice"); }
            );
        }

        void read_line(SectionReader &reader, const IniSection &section, LineDef &line)
        {
            bool display_set = false;
            for (const IniEntry &entry : section.entries)
            {
                const std::string_view key = entry.key;
                if (iequals(key, "Voice"))
                {
                    for (std::string_view item : split_list(entry.value))
                    {
                        if (item.size() > 4 && iequals(item.substr(item.size() - 4), ".ogg"))
                        {
                            item.remove_suffix(4);
                        }
                        line.voices.emplace_back(item);
                    }
                }
                else if (iequals(key, "TextKey"))
                {
                    for (const std::string_view item : split_list(entry.value))
                    {
                        line.text_keys.emplace_back(item);
                    }
                }
                else if (iequals(key, "Speaker"))
                {
                    line.speaker = entry.value;
                }
                else if (iequals(key, "Display"))
                {
                    display_set = true;
                    if (iequals(entry.value, "subtitle"))
                    {
                        line.display = Display::Subtitle;
                    }
                    else if (iequals(entry.value, "bubble"))
                    {
                        line.display = Display::Bubble;
                    }
                    else if (iequals(entry.value, "both"))
                    {
                        line.display = Display::Both;
                    }
                    else if (iequals(entry.value, "none"))
                    {
                        line.display = Display::None;
                    }
                    else
                    {
                        reader.fatal(
                            entry.line,
                            std::format("Display = '{}' is not subtitle, bubble, both or none", entry.value)
                        );
                    }
                }
                else if (iequals(key, "Cooldown"))
                {
                    reader.number(entry, line.cooldown, 0.0f, 3600.0f);
                }
                else if (iequals(key, "Mood"))
                {
                    // Resolved once every pack has loaded, since another mod may define it.
                    line.mood_name = entry.value;
                }
                else if (iequals(key, "MoodStrength"))
                {
                    reader.number(entry, line.mood_strength, 0.0f, face::MAX_MOOD_STRENGTH);
                }
                else if (iequals(key, "MoodPoses"))
                {
                    std::string error;
                    std::optional<std::vector<face::PoseWeight>> poses = face::parse_poses(entry.value, error);
                    if (!poses.has_value())
                    {
                        reader.fatal(entry.line, std::format("MoodPoses: {}", error));
                        continue;
                    }
                    line.mood_poses = std::move(*poses);
                }
                else if (iequals(key, "ReplacesVoice"))
                {
                    for (const std::string_view item : split_list(entry.value))
                    {
                        std::string pattern(item);
                        std::ranges::transform(
                            pattern,
                            pattern.begin(),
                            [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
                        );
                        line.replaces_voice.push_back(std::move(pattern));
                    }
                }
                else
                {
                    reader.problem(entry.line, std::format("unknown key '{}' ignored (voice line)", entry.key));
                }
            }
            if (line.voices.empty())
            {
                reader.fatal(section.line, "no Voice");
            }
            if (!line.text_keys.empty() && line.text_keys.size() != 1 && line.text_keys.size() != line.voices.size())
            {
                reader.fatal(
                    section.line,
                    std::format(
                        "{} TextKey(s) for {} Voice take(s): give one shared key or one per take",
                        line.text_keys.size(),
                        line.voices.size()
                    )
                );
            }
            if (!display_set)
            {
                // Text without an explicit Display shows the way ambient talk does: in a bubble.
                line.display = line.text_keys.empty() ? Display::None : Display::Bubble;
            }
            if (line.display != Display::None && line.text_keys.empty())
            {
                reader.problem(section.line, "Display needs a TextKey; nothing will show");
            }
        }
    } // namespace

    PackParse parse_pack(std::string_view text, const std::filesystem::path &ini_path)
    {
        PackParse out;
        const IniDocument document = parse_ini(text);
        for (const std::string &problem : document.problems)
        {
            out.problems.push_back(std::format("{}: {}", path_text(ini_path), problem));
        }
        const std::filesystem::path dir = ini_path.parent_path();
        for (const IniSection &section : document.sections)
        {
            SectionReader reader(section, ini_path, out);
            if (!valid_sound_name(section.name))
            {
                reader.fatal(section.line, "sound names may use only letters, digits, '_', '.' and '-'");
                continue;
            }
            if (is_mood_section(section))
            {
                MoodDef mood{
                    .name = section.name,
                    .origin = std::format("{}:{}", path_text(ini_path), section.line),
                };
                read_mood(reader, section, mood);
                if (!reader.failed())
                {
                    out.moods.push_back(std::move(mood));
                }
                continue;
            }
            if (is_line_section(section))
            {
                LineDef line{
                    .name = section.name,
                    .origin = std::format("{}:{}", path_text(ini_path), section.line),
                };
                read_line(reader, section, line);
                if (!reader.failed())
                {
                    out.lines.push_back(std::move(line));
                }
                continue;
            }
            SoundDef sound{
                .name = section.name,
                .bus = std::string(DEFAULT_BUS),
                .origin = std::format("{}:{}", path_text(ini_path), section.line),
            };
            for (const IniEntry &entry : section.entries)
            {
                read_entry(reader, entry, sound, dir);
            }
            if (sound.files.empty())
            {
                reader.fatal(section.line, "no Files");
            }
            if (!sound.replaces_for.empty() && sound.replaces.empty())
            {
                reader.problem(section.line, "ReplacesFor without Replaces has no effect");
            }
            if (!sound.play_at.empty() && sound.replaces.empty())
            {
                reader.problem(section.line, "PlayAt without Replaces has no effect");
            }
            if (sound.keep_original && sound.replaces.empty())
            {
                reader.problem(section.line, "KeepOriginal without Replaces has no effect");
            }
            if (sound.max_distance < sound.min_distance)
            {
                reader.problem(section.line, "MaxDistance below MinDistance; using MinDistance for both");
                sound.max_distance = sound.min_distance;
            }
            if (!reader.failed())
            {
                out.sounds.push_back(std::move(sound));
            }
        }
        return out;
    }
} // namespace SoundFramework::packs
