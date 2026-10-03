/**
 * @file highlight/groups.cpp
 * @brief [Highlight.<Name>] parsing, group hotkeys and the published group set.
 */

#include "highlight/groups.hpp"
#include "config.hpp"
#include "constants.hpp"
#include "game_state.hpp"
#include "engine/entity_access.hpp"
#include "engine/visual_resolver.hpp"
#include "highlight/registry.hpp"

#include <DetourModKit.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace HenrySenses
{
    namespace
    {
        constexpr std::string_view SECTION_PREFIX = "highlight.";
        constexpr std::string_view BINDING_PREFIX = "HenrySenses.Highlight.";

        struct TargetName
        {
            std::string_view name;
            Target target;
        };

        constexpr std::array<TargetName, 16> TARGET_NAMES{{
            {"Corpses", Target::Corpses},
            {"Carcasses", Target::Carcasses},
            {"Items", Target::Items},
            {"Containers", Target::Containers},
            {"Herbs", Target::Herbs},
            {"NPCs", Target::Npcs},
            {"Animals", Target::Animals},
            {"Horses", Target::Horses},
            {"Dogs", Target::Dogs},
            {"Critters", Target::Critters},
            {"Doors", Target::Doors},
            {"Workstations", Target::Workstations},
            {"Beds", Target::Beds},
            {"Seats", Target::Seats},
            {"UseSpots", Target::UseSpots},
            {"Objects", Target::Objects},
        }};

        struct TagName
        {
            std::string_view name;
            LootFlag flag;
        };

        constexpr std::array<TagName, 6> TAG_NAMES{{
            {"Stolen", LootFlag::Illegal},
            {"Locked", LootFlag::Locked},
            {"Empty", LootFlag::Empty},
            {"Butcherable", LootFlag::Butcherable},
            {"Hostile", LootFlag::Hostile},
            {"Unconscious", LootFlag::Unconscious},
        }};

        struct ModeName
        {
            std::string_view name;
            GroupMode mode;
        };

        constexpr std::array<ModeName, 4> MODE_NAMES{{
            {"Pulse", GroupMode::Pulse},
            {"Toggle", GroupMode::Toggle},
            {"Hold", GroupMode::Hold},
            {"Always", GroupMode::Always},
        }};

        // Effect names by id (id = index + 1). Groups are read on the init and INI-watcher threads and the names are
        // looked up on the main thread, so the table is locked; it only grows, one entry per distinct name.
        std::mutex s_effect_names_mutex;
        std::vector<std::string> s_effect_names;

        struct PatternPrefix
        {
            std::string_view prefix;
            Target target;
        };

        constexpr std::array<PatternPrefix, 3> PATTERN_PREFIXES{{
            {"Class:", Target::Class},
            {"Name:", Target::Name},
            {"Model:", Target::Model},
        }};

        struct StyleName
        {
            std::string_view name;
            GroupStyle style;
        };

        constexpr std::array<StyleName, 4> STYLE_NAMES{{
            {"Outline", GroupStyle::Outline},
            {"Fill", GroupStyle::Fill},
            {"OutlineFill", GroupStyle::OutlineFill},
            {"Box", GroupStyle::Box},
        }};

        /**
         * @struct RawSection
         * @brief One [Highlight.<Name>] section as written: its keys in file order.
         */
        struct RawSection
        {
            std::string name;
            std::vector<std::pair<std::string, std::string>> keys;
        };

        std::mutex s_mutex;
        std::atomic<std::shared_ptr<const GroupSet>> s_groups{std::make_shared<const GroupSet>()};
        /**
         * @struct HotkeyBindings
         * @brief The hotkeys of the current set: the guards that gate their callbacks off and the binding names that
         *        remove them.
         */
        struct HotkeyBindings
        {
            std::vector<DMK::input::BindingGuard> guards;
            std::vector<std::string> names;
        };

        // Allocated on the init thread and never destroyed by the C runtime: a guard release can wait on the input
        // poll thread, which process-exit static destruction (under the loader lock) must not do. shutdown_groups()
        // releases and frees them on the control thread.
        HotkeyBindings *s_hotkeys = nullptr;
        // The input engine's settings, from init (set_group_input_settings()).
        std::optional<DMK::input::Input::Settings> s_input_settings;
        // The canonical text of the current set; an unchanged reload keeps the hotkeys.
        std::string s_signature;

        [[nodiscard]] std::int64_t steady_ms() noexcept
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch()
            )
                .count();
        }

        [[nodiscard]] std::string_view trim(std::string_view text) noexcept
        {
            constexpr std::string_view whitespace = " \t\r\n";
            const std::size_t begin = text.find_first_not_of(whitespace);
            if (begin == std::string_view::npos)
            {
                return {};
            }
            return text.substr(begin, text.find_last_not_of(whitespace) - begin + 1);
        }

        [[nodiscard]] bool iequals(std::string_view a, std::string_view b) noexcept
        {
            return a.size() == b.size() && std::equal(
                                               a.begin(),
                                               a.end(),
                                               b.begin(),
                                               [](char x, char y) { return ascii_lower(x) == ascii_lower(y); }
                                           );
        }

        [[nodiscard]] bool istarts_with(std::string_view text, std::string_view prefix) noexcept
        {
            return text.size() >= prefix.size() && iequals(text.substr(0, prefix.size()), prefix);
        }

        [[nodiscard]] std::string lowercase(std::string_view text)
        {
            std::string out(text);
            std::transform(out.begin(), out.end(), out.begin(), ascii_lower);
            return out;
        }

        /** @brief Splits @p text at @p separator, trimming each part and dropping empty ones. */
        [[nodiscard]] std::vector<std::string_view> split(std::string_view text, char separator)
        {
            std::vector<std::string_view> parts;
            while (true)
            {
                const std::size_t cut = text.find(separator);
                if (const std::string_view part = trim(text.substr(0, cut)); !part.empty())
                {
                    parts.push_back(part);
                }
                if (cut == std::string_view::npos)
                {
                    break;
                }
                text.remove_prefix(cut + 1);
            }
            return parts;
        }

        /** @brief The INI next to the running module, where DetourModKit reads it. */
        [[nodiscard]] std::filesystem::path ini_path()
        {
            return std::filesystem::path(DMK::filesystem::get_runtime_directory()) / constants::get_config_filename();
        }

        /** @brief Reads every [Highlight.<Name>] section of the INI in file order. */
        [[nodiscard]] std::vector<RawSection> read_sections(const std::filesystem::path &path, bool &read_ok)
        {
            std::vector<RawSection> sections;
            std::ifstream file(path);
            read_ok = file.good();
            if (!read_ok)
            {
                return sections;
            }
            std::string line;
            RawSection *current = nullptr;
            bool first_line = true;
            while (std::getline(file, line))
            {
                // SimpleIni accepts a UTF-8 BOM, so the group reader must accept the same file.
                if (first_line && line.starts_with("\xEF\xBB\xBF"))
                {
                    line.erase(0, 3);
                }
                first_line = false;
                const std::string_view text = trim(line);
                if (text.empty() || text.front() == ';' || text.front() == '#')
                {
                    continue;
                }
                if (text.front() == '[')
                {
                    current = nullptr;
                    const std::size_t close = text.find(']');
                    const std::string_view section =
                        trim(text.substr(1, close == std::string_view::npos ? std::string_view::npos : close - 1));
                    if (istarts_with(section, SECTION_PREFIX))
                    {
                        sections.push_back(RawSection{std::string(trim(section.substr(SECTION_PREFIX.size()))), {}});
                        current = &sections.back();
                    }
                    continue;
                }
                const std::size_t equals = text.find('=');
                if (current == nullptr || equals == std::string_view::npos)
                {
                    continue;
                }
                current->keys.emplace_back(
                    std::string(trim(text.substr(0, equals))),
                    std::string(trim(text.substr(equals + 1)))
                );
            }
            read_ok = !file.bad();
            return sections;
        }

        /** @brief Parses "RRGGBB" or "RRGGBBAA" (optional "#" or "0x"). */
        [[nodiscard]] std::optional<std::uint32_t> parse_color(std::string_view text) noexcept
        {
            std::string_view digits = trim(text);
            if (digits.starts_with('#'))
            {
                digits.remove_prefix(1);
            }
            else if (istarts_with(digits, "0x"))
            {
                digits.remove_prefix(2);
            }
            if (digits.size() != 6 && digits.size() != 8)
            {
                return std::nullopt;
            }
            std::uint32_t value = 0;
            const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value, 16);
            if (error != std::errc{} || end != digits.data() + digits.size())
            {
                return std::nullopt;
            }
            if (digits.size() == 6)
            {
                value = (value << 8) | 0xFFu;
            }
            return value != 0 ? std::optional<std::uint32_t>{value} : std::nullopt;
        }

        [[nodiscard]] std::optional<float> parse_float(std::string_view text) noexcept
        {
            text = trim(text);
            float value = 0.0f;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value))
            {
                return std::nullopt;
            }
            return value;
        }

        [[nodiscard]] std::optional<bool> parse_bool(std::string_view text) noexcept
        {
            text = trim(text);
            if (iequals(text, "true") || iequals(text, "1") || iequals(text, "yes") || iequals(text, "on"))
            {
                return true;
            }
            if (iequals(text, "false") || iequals(text, "0") || iequals(text, "no") || iequals(text, "off"))
            {
                return false;
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<DMK::InputCode> parse_input_token(std::string_view token)
        {
            if (istarts_with(token, "0x") && token.size() > 2)
            {
                int vk = 0;
                const auto [end, error] = std::from_chars(token.data() + 2, token.data() + token.size(), vk, 16);
                if (error == std::errc{} && end == token.data() + token.size() && vk > 0 && vk < 256)
                {
                    return DMK::keyboard_key(vk);
                }
                return std::nullopt;
            }
            return DMK::parse_input_name(token);
        }

        /**
         * @brief Parses a key setting: alternatives separated by ',', each "Modifier+...+Key" (the last part is the
         *        key). Empty or NONE is unbound.
         */
        [[nodiscard]] DMK::input::KeyComboList
        parse_keys(std::string_view text, std::string_view group, std::vector<std::string> &problems)
        {
            DMK::input::KeyComboList combos;
            text = trim(text);
            if (text.empty() || iequals(text, "none"))
            {
                return combos;
            }
            for (const std::string_view alternative : split(text, ','))
            {
                const std::vector<std::string_view> parts = split(alternative, '+');
                if (parts.empty())
                {
                    continue;
                }
                DMK::input::KeyCombo combo;
                bool valid = true;
                for (std::size_t i = 0; i < parts.size(); ++i)
                {
                    const std::optional<DMK::InputCode> code = parse_input_token(parts[i]);
                    if (!code.has_value())
                    {
                        problems.push_back(std::format("[Highlight.{}] Key: unknown input '{}'", group, parts[i]));
                        valid = false;
                        break;
                    }
                    (i + 1 == parts.size() ? combo.keys : combo.modifiers).push_back(*code);
                }
                if (valid)
                {
                    combos.push_back(std::move(combo));
                }
            }
            return combos;
        }

        /**
         * @brief Parses a pattern entry ("Class:<pattern>", "Name:<pattern>", "Model:<pattern>"), without a colour.
         * @return std::nullopt when @p text is not a pattern entry.
         */
        [[nodiscard]] std::optional<GroupTarget> parse_pattern(std::string_view text)
        {
            for (const PatternPrefix &entry : PATTERN_PREFIXES)
            {
                if (istarts_with(text, entry.prefix))
                {
                    return GroupTarget{entry.target, lowercase(trim(text.substr(entry.prefix.size()))), 0};
                }
            }
            return std::nullopt;
        }

        /**
         * @brief Parses an Only / Except list: tags into @p bits, patterns into @p patterns.
         */
        void parse_filter(
            std::string_view text,
            std::string_view group,
            std::string_view key,
            std::uint32_t &bits,
            std::vector<GroupTarget> &patterns,
            std::vector<std::string> &problems
        )
        {
            bits = 0;
            patterns.clear();
            for (const std::string_view token : split(text, ','))
            {
                if (std::optional<GroupTarget> pattern = parse_pattern(token))
                {
                    if (pattern->pattern.empty())
                    {
                        problems.push_back(std::format("[Highlight.{}] {}: '{}' has no pattern", group, key, token));
                        continue;
                    }
                    patterns.push_back(std::move(*pattern));
                    continue;
                }
                const auto it = std::find_if(
                    TAG_NAMES.begin(),
                    TAG_NAMES.end(),
                    [token](const TagName &entry) { return iequals(entry.name, token); }
                );
                if (it == TAG_NAMES.end())
                {
                    problems.push_back(std::format("[Highlight.{}] {}: unknown tag '{}'", group, key, token));
                    continue;
                }
                bits |= loot_flag_bit(it->flag);
            }
        }

        /**
         * @brief Parses one Targets entry: "Kind", "Kind:RRGGBBAA", "Class:<pattern>", "Name:<pattern>",
         *        "Model:<pattern>", each pattern optionally followed by ":RRGGBBAA".
         */
        [[nodiscard]] std::optional<GroupTarget>
        parse_target(std::string_view token, std::string_view group, std::vector<std::string> &problems)
        {
            std::uint32_t color = 0;
            std::string_view name = token;
            // A trailing ":<colour>" is the target's own colour.
            if (const std::size_t colon = token.rfind(':'); colon != std::string_view::npos)
            {
                if (const std::optional<std::uint32_t> parsed = parse_color(token.substr(colon + 1));
                    parsed.has_value())
                {
                    color = *parsed;
                    name = trim(token.substr(0, colon));
                }
            }
            if (std::optional<GroupTarget> pattern = parse_pattern(name))
            {
                if (pattern->pattern.empty())
                {
                    problems.push_back(std::format("[Highlight.{}] Targets: '{}' has no pattern", group, token));
                    return std::nullopt;
                }
                pattern->color = color;
                return pattern;
            }
            const auto it = std::find_if(
                TARGET_NAMES.begin(),
                TARGET_NAMES.end(),
                [name](const TargetName &entry) { return iequals(entry.name, name); }
            );
            if (it == TARGET_NAMES.end())
            {
                problems.push_back(std::format("[Highlight.{}] Targets: unknown target '{}'", group, token));
                return std::nullopt;
            }
            return GroupTarget{it->target, {}, color};
        }

        /** @brief Builds one group from its section, with the [Settings] fallbacks. */
        [[nodiscard]] HighlightGroup build_group(const RawSection &section, std::vector<std::string> &problems)
        {
            const LiveSettings &s = settings();
            HighlightGroup group{};
            group.name = section.name;
            group.key_text = default_key();
            group.consume = s.key_consume.load(std::memory_order_relaxed);
            group.mode = static_cast<GroupMode>(s.mode.load(std::memory_order_relaxed));
            group.duration = s.duration.load(std::memory_order_relaxed);
            group.fade_out = s.fade_out.load(std::memory_order_relaxed);
            group.radius = s.radius.load(std::memory_order_relaxed);
            switch (static_cast<RenderStyle>(s.style.load(std::memory_order_relaxed)))
            {
            case RenderStyle::Fill:
                group.style = GroupStyle::Fill;
                break;
            case RenderStyle::OutlineFill:
                group.style = GroupStyle::OutlineFill;
                break;
            default:
                group.style = GroupStyle::Outline;
                break;
            }
            group.gate_mask = default_hide_mask();
            for (const auto &[key, value] : section.keys)
            {
                if (iequals(key, "HideIn"))
                {
                    const StateList parsed = parse_state_list(value);
                    if (!parsed.unknown.empty())
                    {
                        problems.push_back(
                            std::format(
                                "[Highlight.{}] HideIn: unknown situation(s) {} (known: Dialogue, Combat, "
                                "Minigame)",
                                group.name,
                                parsed.unknown
                            )
                        );
                    }
                    group.gate_mask = parsed.mask;
                    continue;
                }
                if (iequals(key, "Enabled"))
                {
                    const std::optional<bool> enabled = parse_bool(value);
                    if (!enabled.has_value())
                    {
                        problems.push_back(
                            std::format("[Highlight.{}] Enabled: '{}' is not true or false", group.name, value)
                        );
                    }
                    group.enabled = enabled.value_or(true);
                }
                else if (iequals(key, "Focus"))
                {
                    const std::optional<bool> focus = parse_bool(value);
                    if (!focus.has_value())
                    {
                        problems.push_back(
                            std::format("[Highlight.{}] Focus: '{}' is not true or false", group.name, value)
                        );
                    }
                    group.focus = focus.value_or(false);
                }
                else if (iequals(key, "Targets"))
                {
                    for (const std::string_view token : split(value, ','))
                    {
                        if (std::optional<GroupTarget> target = parse_target(token, group.name, problems))
                        {
                            group.target_mask |= target_bit(target->target);
                            group.targets.push_back(std::move(*target));
                        }
                    }
                }
                else if (iequals(key, "Color"))
                {
                    const std::optional<std::uint32_t> color = parse_color(value);
                    if (!color.has_value())
                    {
                        problems.push_back(
                            std::format("[Highlight.{}] Color: '{}' is not RRGGBB or RRGGBBAA", group.name, value)
                        );
                    }
                    group.color = color.value_or(group.color);
                }
                else if (iequals(key, "Only"))
                {
                    parse_filter(value, group.name, key, group.only, group.only_patterns, problems);
                }
                else if (iequals(key, "Except"))
                {
                    parse_filter(value, group.name, key, group.except, group.except_patterns, problems);
                }
                else if (iequals(key, "Style"))
                {
                    const auto it = std::find_if(
                        STYLE_NAMES.begin(),
                        STYLE_NAMES.end(),
                        [&value](const StyleName &entry) { return iequals(entry.name, trim(value)); }
                    );
                    if (it == STYLE_NAMES.end())
                    {
                        if (!value.empty())
                        {
                            problems.push_back(
                                std::format("[Highlight.{}] Style: unknown style '{}'", group.name, value)
                            );
                        }
                    }
                    else
                    {
                        group.style = it->style;
                    }
                }
                else if (iequals(key, "Key"))
                {
                    group.key_text = value;
                }
                else if (iequals(key, "Key.Consume"))
                {
                    const std::optional<bool> consume = parse_bool(value);
                    if (!consume.has_value() && !value.empty())
                    {
                        problems.push_back(
                            std::format("[Highlight.{}] Key.Consume: '{}' is not true or false", group.name, value)
                        );
                    }
                    group.consume = consume.value_or(group.consume);
                }
                else if (iequals(key, "Mode"))
                {
                    const std::optional<GroupMode> mode = parse_group_mode(value);
                    if (!mode.has_value() && !value.empty())
                    {
                        problems.push_back(std::format("[Highlight.{}] Mode: unknown mode '{}'", group.name, value));
                    }
                    group.mode = mode.value_or(group.mode);
                }
                else if (iequals(key, "Duration"))
                {
                    if (const std::optional<float> duration = parse_float(value); duration.has_value())
                    {
                        group.duration = *duration;
                    }
                    else if (!value.empty())
                    {
                        problems.push_back(
                            std::format("[Highlight.{}] Duration: '{}' is not a number", group.name, value)
                        );
                    }
                }
                else if (iequals(key, "FadeOut"))
                {
                    if (const std::optional<float> fade_out = parse_float(value); fade_out.has_value())
                    {
                        group.fade_out = *fade_out;
                    }
                    else if (!value.empty())
                    {
                        problems.push_back(
                            std::format("[Highlight.{}] FadeOut: '{}' is not a number", group.name, value)
                        );
                    }
                }
                else if (iequals(key, "Radius"))
                {
                    if (const std::optional<float> radius = parse_float(value); radius.has_value())
                    {
                        group.radius = *radius;
                    }
                    else if (!value.empty())
                    {
                        problems.push_back(
                            std::format("[Highlight.{}] Radius: '{}' is not a number", group.name, value)
                        );
                    }
                }
                else if (iequals(key, "Effect"))
                {
                    // "<library>.<effect>" as the game's Libs/Particles libraries name it; None or empty = off.
                    const std::string_view name = trim(value);
                    if (name.empty() || iequals(name, "None"))
                    {
                        group.effect = 0;
                    }
                    else if (name.find('.') == std::string_view::npos)
                    {
                        problems.push_back(
                            std::format(
                                "[Highlight.{}] Effect: '{}' is not <library>.<effect> (e.g. "
                                "particles_smithery.particles_smithery.sparks)",
                                group.name,
                                value
                            )
                        );
                    }
                    else
                    {
                        group.effect = intern_effect_name(name);
                    }
                }
                else if (iequals(key, "EffectScale"))
                {
                    if (const std::optional<float> scale = parse_float(value); scale.has_value())
                    {
                        group.effect_scale = *scale;
                    }
                    else if (!value.empty())
                    {
                        problems.push_back(
                            std::format("[Highlight.{}] EffectScale: '{}' is not a number", group.name, value)
                        );
                    }
                }
                else
                {
                    problems.push_back(std::format("[Highlight.{}] unknown key '{}'", group.name, key));
                }
            }
            group.duration = std::clamp(group.duration, 0.5f, 600.0f);
            group.fade_out = std::clamp(group.fade_out, 0.0f, 10.0f);
            group.radius = std::clamp(group.radius, 2.0f, 100.0f);
            group.effect_scale = std::clamp(group.effect_scale, 0.1f, 10.0f);
            group.keys = parse_keys(group.key_text, group.name, problems);
            if (group.targets.empty())
            {
                problems.push_back(std::format("[Highlight.{}] has no Targets; the group is off", group.name));
                group.enabled = false;
            }
            return group;
        }

        /** @brief One line per group with every resolved value, to detect a change. */
        [[nodiscard]] std::string signature_of(const GroupSet &set)
        {
            std::string text;
            for (const HighlightGroup &group : set.groups)
            {
                text += std::format(
                    "{}|{}|{}|{:#x}|{:#x}|{:#x}|{}|{}|{}|{}|{}|{}|{}|{:#x}|{}|{}|",
                    lowercase(group.name),
                    group.enabled,
                    group.focus,
                    group.color,
                    group.only,
                    group.except,
                    lowercase(group.key_text),
                    group.consume,
                    static_cast<int>(group.mode),
                    group.duration,
                    group.fade_out,
                    group.radius,
                    static_cast<int>(group.style),
                    group.gate_mask,
                    group.effect,
                    group.effect_scale
                );
                for (const std::vector<GroupTarget> *list :
                     {&group.targets, &group.only_patterns, &group.except_patterns})
                {
                    for (const GroupTarget &target : *list)
                    {
                        text +=
                            std::format("{}:{}:{:#x},", static_cast<int>(target.target), target.pattern, target.color);
                    }
                    text += ';';
                }
                text += '\n';
            }
            return text;
        }

        /**
         * @brief Releases the current hotkeys and removes their bindings. input::register_combo appends, so a binding
         *        whose guard was released stays registered (holding its callable) until it is removed by name.
         */
        void release_hotkeys() noexcept
        {
            if (s_hotkeys == nullptr)
            {
                return;
            }
            s_hotkeys->guards.clear();
            for (const std::string &name : s_hotkeys->names)
            {
                (void)DMK::input::Input::instance().remove_bindings_by_name(name);
            }
            s_hotkeys->names.clear();
        }

        /** @brief Registers the hotkey of every enabled group that has one. */
        void register_hotkeys(const GroupSet &set)
        {
            for (const HighlightGroup &group : set.groups)
            {
                if (!group.enabled || group.keys.empty() || group.mode == GroupMode::Always)
                {
                    continue;
                }
                DMK::input::ComboBinding binding{};
                binding.name = std::string(BINDING_PREFIX) + group.name;
                binding.combos = group.keys;
                // The input engine hides only a digital controller button or the mouse wheel used as the trigger, and
                // installs its XInput interception only once a consume binding holds such a controller button: a
                // keyboard-only key with Key.Consume set changes nothing.
                binding.consume = group.consume;
                const std::shared_ptr<GroupActivation> activation = group.activation;
                const std::string name = group.name;
                if (group.mode == GroupMode::Hold)
                {
                    binding.trigger = DMK::input::Trigger::Hold;
                    // The release edge is always honoured, so a hold can never strand the group on.
                    binding.on_state_change = [activation, name](bool pressed)
                    {
                        if (pressed && !settings().enabled.load(std::memory_order_relaxed))
                        {
                            (void)DMK::log()
                                .try_log(DMK::LogLevel::Debug, "Highlight.{} key ignored (mod disabled)", name);
                            return;
                        }
                        activation->off_started_ms.store(pressed ? 0 : steady_ms(), std::memory_order_relaxed);
                        activation->held.store(pressed, std::memory_order_relaxed);
                        (void)DMK::log()
                            .try_log(DMK::LogLevel::Debug, "Highlight.{} {}", name, pressed ? "held" : "released");
                    };
                }
                else
                {
                    const GroupMode mode = group.mode;
                    binding.trigger = DMK::input::Trigger::Press;
                    binding.on_press = [activation, mode, name]()
                    {
                        if (!settings().enabled.load(std::memory_order_relaxed))
                        {
                            (void)DMK::log()
                                .try_log(DMK::LogLevel::Debug, "Highlight.{} key ignored (mod disabled)", name);
                            return;
                        }
                        if (mode == GroupMode::Toggle)
                        {
                            const bool on = !activation->toggled.load(std::memory_order_relaxed);
                            activation->off_started_ms.store(on ? 0 : steady_ms(), std::memory_order_relaxed);
                            activation->toggled.store(on, std::memory_order_relaxed);
                            (void)DMK::log().try_log(DMK::LogLevel::Info, "Highlight.{} {}", name, on ? "ON" : "OFF");
                            return;
                        }
                        activation->pulse_started_ms.store(steady_ms(), std::memory_order_relaxed);
                        (void)DMK::log().try_log(DMK::LogLevel::Debug, "Highlight.{} pulse", name);
                    };
                }
                std::string binding_name = binding.name;
                DMK::Result<DMK::input::BindingGuard> guard = DMK::input::register_combo(std::move(binding));
                if (guard.has_value())
                {
                    s_hotkeys->guards.push_back(std::move(*guard));
                    s_hotkeys->names.push_back(std::move(binding_name));
                }
                else
                {
                    DMK::log().error(
                        "Highlight.{}: key '{}' could not be registered ({})",
                        group.name,
                        group.key_text,
                        guard.error().message()
                    );
                }
            }
        }

        /**
         * @brief Reads the INI, builds the set and, when it changed, publishes it and re-registers the hotkeys.
         * @param first The first load (logs the set even when empty).
         */
        void load_groups(bool first)
        {
            const std::filesystem::path path = ini_path();
            bool read_ok = false;
            const std::vector<RawSection> sections = read_sections(path, read_ok);
            if (!read_ok)
            {
                DMK::log().warning("Groups: could not read {}; the groups stay as they were", path.string());
                return;
            }

            const std::lock_guard lock(s_mutex);
            // Activation state is kept by group name across a reload.
            std::unordered_map<std::string, std::shared_ptr<GroupActivation>> previous;
            const std::shared_ptr<const GroupSet> current = s_groups.load(std::memory_order_acquire);
            for (const HighlightGroup &group : current->groups)
            {
                previous.emplace(lowercase(group.name), group.activation);
            }

            auto set = std::make_shared<GroupSet>();
            std::vector<std::string> problems;
            for (const RawSection &section : sections)
            {
                if (section.name.empty())
                {
                    problems.emplace_back("[Highlight.] has no name; skipped");
                    continue;
                }
                const bool duplicate = std::any_of(
                    set->groups.begin(),
                    set->groups.end(),
                    [&section](const HighlightGroup &group) { return iequals(group.name, section.name); }
                );
                if (duplicate)
                {
                    problems.push_back(
                        std::format("[Highlight.{}] appears twice; the second is skipped", section.name)
                    );
                    continue;
                }
                HighlightGroup group = build_group(section, problems);
                const auto kept = previous.find(lowercase(group.name));
                group.activation = kept != previous.end() ? kept->second : std::make_shared<GroupActivation>();
                if (group.enabled)
                {
                    GroupPatterns &patterns = set->patterns;
                    for (const GroupTarget &target : group.targets)
                    {
                        std::vector<std::string> *list = target.target == Target::Class   ? &patterns.classes
                                                         : target.target == Target::Name  ? &patterns.names
                                                         : target.target == Target::Model ? &patterns.models
                                                                                          : nullptr;
                        if (list == nullptr)
                        {
                            continue;
                        }
                        if (std::find(list->begin(), list->end(), target.pattern) == list->end())
                        {
                            list->push_back(target.pattern);
                        }
                        if (target.target != Target::Class)
                        {
                            patterns.reach = std::max(patterns.reach, group.radius);
                        }
                    }
                }
                set->groups.push_back(std::move(group));
            }
            {
                GroupPatterns &patterns = set->patterns;
                for (std::vector<std::string> *list : {&patterns.classes, &patterns.names, &patterns.models})
                {
                    std::sort(list->begin(), list->end());
                }
                auto join = [](const std::vector<std::string> &list)
                {
                    std::string text;
                    for (const std::string &item : list)
                    {
                        text += item + ',';
                    }
                    return text;
                };
                patterns.signature = std::format(
                    "c:{}|n:{}|m:{}|r:{}",
                    join(patterns.classes),
                    join(patterns.names),
                    join(patterns.models),
                    patterns.reach
                );
            }

            std::string signature = signature_of(*set);
            if (!first && signature == s_signature)
            {
                return;
            }
            for (const std::string &problem : problems)
            {
                DMK::log().warning("Groups: {}", problem);
            }
            if (s_hotkeys == nullptr)
            {
                s_hotkeys = new HotkeyBindings();
            }
            // Reserve both bookkeeping arrays before any binding is installed. A successful registration must
            // always retain its guard and name so teardown can revoke the callback and remove the binding.
            s_hotkeys->guards.reserve(set->groups.size());
            s_hotkeys->names.reserve(set->groups.size());
            release_hotkeys();
            register_hotkeys(*set);
            // A reload that registers hotkeys starts the input engine in case init found none to start it with
            // (start() is a no-op while it runs).
            if (!first && !s_hotkeys->names.empty() && s_input_settings.has_value())
            {
                if (auto started = DMK::input::Input::instance().start(*s_input_settings); !started.has_value())
                {
                    DMK::log().error(
                        "Groups: the input engine did not start ({}); hotkeys unavailable",
                        started.error().message()
                    );
                }
            }
            s_signature = std::move(signature);
            s_groups.store(set, std::memory_order_release);

            std::size_t enabled = 0;
            for (const HighlightGroup &group : set->groups)
            {
                enabled += group.enabled ? 1 : 0;
            }
            DMK::log().info(
                "Groups: {} highlight group(s), {} enabled, {} hotkey(s){}",
                set->groups.size(),
                enabled,
                s_hotkeys->guards.size(),
                set->groups.empty() ? "; nothing is highlighted until a [Highlight.<Name>] section "
                                      "is added"
                                    : ""
            );
            for (const HighlightGroup &group : set->groups)
            {
                std::string targets;
                for (const GroupTarget &target : group.targets)
                {
                    targets += std::format(
                        "{}{}{}{}",
                        targets.empty() ? "" : ", ",
                        target_name(target.target),
                        is_pattern_target(target.target) ? ":" + target.pattern : std::string{},
                        target.color != 0 ? std::format(":{:08X}", target.color) : std::string{}
                    );
                }
                DMK::log().info(
                    "Groups:   {} {} [{}] color={:08X} only={:#x}+{} except={:#x}+{} style={} hideIn=[{}] "
                    "key='{}'{} mode={} duration={:.1f} fadeOut={:.1f} radius={:.0f}{}{}",
                    group.name,
                    group.enabled ? "on" : "off",
                    targets,
                    group.color,
                    group.only,
                    group.only_patterns.size(),
                    group.except,
                    group.except_patterns.size(),
                    group_style_name(group.style),
                    state_list_text(group.gate_mask),
                    group.key_text,
                    group.consume ? " consume" : "",
                    group_mode_name(group.mode),
                    group.duration,
                    group.fade_out,
                    group.radius,
                    group.focus ? " focus" : "",
                    group.effect != 0
                        ? std::format(" effect='{}' x{:.2f}", effect_name(group.effect), group.effect_scale)
                        : std::string{}
                );
            }
        }
    } // namespace

    const char *target_name(Target target) noexcept
    {
        switch (target)
        {
        case Target::Class:
            return "Class";
        case Target::Name:
            return "Name";
        case Target::Model:
            return "Model";
        default:
            break;
        }
        for (const TargetName &entry : TARGET_NAMES)
        {
            if (entry.target == target)
            {
                return entry.name.data();
            }
        }
        return "?";
    }

    bool wildcard_match(std::string_view pattern, std::string_view text) noexcept
    {
        // Iterative glob with single-star backtracking: linear in practice, no recursion.
        std::size_t p = 0;
        std::size_t t = 0;
        std::size_t star = std::string_view::npos;
        std::size_t resume = 0;
        auto lower = ascii_lower;
        while (t < text.size())
        {
            if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == lower(text[t])))
            {
                ++p;
                ++t;
            }
            else if (p < pattern.size() && pattern[p] == '*')
            {
                star = p++;
                resume = t;
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

    const char *group_style_name(GroupStyle style) noexcept
    {
        for (const StyleName &entry : STYLE_NAMES)
        {
            if (entry.style == style)
            {
                return entry.name.data();
            }
        }
        return "?";
    }

    const std::string &ObjectFacts::klass()
    {
        if (!m_klass.has_value())
        {
            m_klass = m_entity != 0 ? entity_class_name(m_entity) : std::string{};
        }
        return *m_klass;
    }

    const std::string &ObjectFacts::name()
    {
        if (!m_name.has_value())
        {
            m_name = m_entity != 0 ? entity_name(m_entity) : std::string{};
        }
        return *m_name;
    }

    const std::string &ObjectFacts::model()
    {
        if (!m_model.has_value())
        {
            m_model = m_entity != 0 ? entity_model_path(m_entity) : m_brush != 0 ? render_node_name(m_brush) : "";
        }
        return *m_model;
    }

    bool ObjectFacts::matches(const GroupTarget &entry)
    {
        switch (entry.target)
        {
        case Target::Class:
            return !klass().empty() && wildcard_match(entry.pattern, klass());
        case Target::Name:
            return !name().empty() && wildcard_match(entry.pattern, name());
        case Target::Model:
            return !model().empty() && wildcard_match(entry.pattern, model());
        default:
            return false;
        }
    }

    const char *group_mode_name(GroupMode mode) noexcept
    {
        for (const ModeName &entry : MODE_NAMES)
        {
            if (entry.mode == mode)
            {
                return entry.name.data();
            }
        }
        return "?";
    }

    std::optional<GroupMode> parse_group_mode(std::string_view text) noexcept
    {
        text = trim(text);
        for (const ModeName &entry : MODE_NAMES)
        {
            if (iequals(entry.name, text))
            {
                return entry.mode;
            }
        }
        return std::nullopt;
    }

    bool HighlightGroup::wants(Target kind) const noexcept
    {
        const bool animal = kind == Target::Horses || kind == Target::Dogs || kind == Target::Critters;
        return (target_mask & (target_bit(kind) | (animal ? target_bit(Target::Animals) : 0u))) != 0;
    }

    std::optional<std::uint32_t> HighlightGroup::claims(ObjectFacts &object) const
    {
        if (!accepts_flags(object.flags()))
        {
            return std::nullopt;
        }
        for (const GroupTarget &pattern : except_patterns)
        {
            if (object.matches(pattern))
            {
                return std::nullopt;
            }
        }
        for (const GroupTarget &pattern : only_patterns)
        {
            if (!object.matches(pattern))
            {
                return std::nullopt;
            }
        }
        auto colour = [this](const GroupTarget &entry) { return entry.color != 0 ? entry.color : color; };
        const std::optional<Target> kind = object.target();
        // The first listed target that names the object wins; Animals (the parent of horses, dogs and critters)
        // only when no entry names the animal itself.
        for (const GroupTarget &entry : targets)
        {
            if (is_pattern_target(entry.target) ? object.matches(entry) : kind == entry.target)
            {
                return colour(entry);
            }
        }
        if (kind == Target::Horses || kind == Target::Dogs || kind == Target::Critters)
        {
            for (const GroupTarget &entry : targets)
            {
                if (entry.target == Target::Animals)
                {
                    return colour(entry);
                }
            }
        }
        return std::nullopt;
    }

    void set_group_input_settings(const DMK::input::Input::Settings &settings)
    {
        const std::lock_guard lock(s_mutex);
        s_input_settings = settings;
    }

    DMK::Result<void> initialize_groups()
    {
        try
        {
            load_groups(true);
        }
        catch (const std::exception &e)
        {
            DMK::log().error("Groups: loading failed: {}", e.what());
            return std::unexpected(DMK::Error{DMK::ErrorCode::InvalidArg, "groups/load"});
        }
        return {};
    }

    void reload_groups()
    {
        try
        {
            load_groups(false);
        }
        catch (const std::exception &e)
        {
            DMK::log().error("Groups: reloading failed: {}", e.what());
        }
    }

    void shutdown_groups() noexcept
    {
        const std::lock_guard lock(s_mutex);
        release_hotkeys();
        delete s_hotkeys;
        s_hotkeys = nullptr;
    }

    std::shared_ptr<const GroupSet> current_groups() noexcept
    {
        return s_groups.load(std::memory_order_acquire);
    }

    void log_groups()
    {
        const std::shared_ptr<const GroupSet> set = current_groups();
        const std::int64_t now = steady_ms();
        for (const HighlightGroup &group : set->groups)
        {
            const GroupActivation &activation = *group.activation;
            const std::int64_t started = activation.pulse_started_ms.load(std::memory_order_relaxed);
            DMK::log().info(
                "Groups: {} {} mode={} key='{}' toggled={} held={} lastPulseMsAgo={} radius={:.0f}",
                group.name,
                group.enabled ? "on" : "off",
                group_mode_name(group.mode),
                group.key_text,
                activation.toggled.load(),
                activation.held.load(),
                started != 0 ? now - started : -1,
                group.radius
            );
        }
    }

    std::uint16_t intern_effect_name(std::string_view name)
    {
        if (name.empty())
        {
            return 0;
        }
        const std::scoped_lock lock(s_effect_names_mutex);
        for (std::size_t index = 0; index < s_effect_names.size(); ++index)
        {
            if (iequals(s_effect_names[index], name))
            {
                return static_cast<std::uint16_t>(index + 1);
            }
        }
        // One entry per distinct name the INI ever held; far below the id range.
        if (s_effect_names.size() >= 0xFFFE)
        {
            return 0;
        }
        s_effect_names.emplace_back(name);
        return static_cast<std::uint16_t>(s_effect_names.size());
    }

    std::string effect_name(std::uint16_t id)
    {
        const std::scoped_lock lock(s_effect_names_mutex);
        return id != 0 && id <= s_effect_names.size() ? s_effect_names[id - 1] : std::string{};
    }

} // namespace HenrySenses
