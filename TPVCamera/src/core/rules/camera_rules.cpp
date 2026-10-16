/**
 * @file core/rules/camera_rules.cpp
 * @brief Parser, compiler and classifier of the camera rules (see core/rules/camera_rules.hpp).
 */

#include "core/rules/camera_rules.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace TPVCamera::Rules
{

    namespace
    {
        constexpr std::string_view k_whitespace = " \t\r\v\f";
        constexpr std::string_view k_bom = "\xEF\xBB\xBF";
        // The byte order marks of UTF-16, little and big endian.
        constexpr std::string_view k_utf16_le = "\xFF\xFE";
        constexpr std::string_view k_utf16_be = "\xFE\xFF";
        // Every kind, merged included: a rule without only= counts for all of them.
        constexpr std::uint8_t k_all_kinds = 0x1F;
        // The four kinds only= can name, for messages and the override check.
        constexpr std::uint8_t k_named_kinds = 0x0F;

        constexpr std::array<std::string_view, 9> k_behaviours = {
            "collide", "fade", "bush", "tree", "grass", "frame", "pass", "character", "exclude",
        };
        constexpr std::array<std::string_view, 4> k_kind_words = {
            "brush",
            "movable",
            "entity",
            "vegetation",
        };
        constexpr std::array<std::string_view, 5> k_exact_extensions = {
            ".cgf", ".cga", ".chr", ".cdf", ".skin",
        };

        [[nodiscard]] constexpr char lower(char c) noexcept
        {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
        }

        [[nodiscard]] std::string lower_copy(std::string_view text)
        {
            std::string out(text);
            std::ranges::transform(out, out.begin(), lower);
            return out;
        }

        [[nodiscard]] std::string_view trim(std::string_view text) noexcept
        {
            const std::size_t begin = text.find_first_not_of(k_whitespace);
            if (begin == std::string_view::npos)
            {
                return {};
            }
            return text.substr(begin, text.find_last_not_of(k_whitespace) - begin + 1);
        }

        [[nodiscard]] std::vector<std::string_view> split_words(std::string_view text)
        {
            std::vector<std::string_view> words;
            std::size_t at = 0;
            while (at < text.size())
            {
                const std::size_t begin = text.find_first_not_of(k_whitespace, at);
                if (begin == std::string_view::npos)
                {
                    break;
                }
                const std::size_t end = text.find_first_of(k_whitespace, begin);
                words.push_back(text.substr(begin, end == std::string_view::npos ? end : end - begin));
                at = end == std::string_view::npos ? text.size() : end;
            }
            return words;
        }

        /** @brief The words of @p list split at ',', with empty ones dropped. */
        [[nodiscard]] std::vector<std::string_view> split_commas(std::string_view list)
        {
            std::vector<std::string_view> items;
            std::size_t at = 0;
            while (at <= list.size())
            {
                const std::size_t end = list.find(',', at);
                const std::string_view item = list.substr(at, end == std::string_view::npos ? end : end - at);
                if (!item.empty())
                {
                    items.push_back(item);
                }
                if (end == std::string_view::npos)
                {
                    break;
                }
                at = end + 1;
            }
            return items;
        }

        [[nodiscard]] int behaviour_index(std::string_view word) noexcept
        {
            const auto found = std::ranges::find(k_behaviours, word);
            return found == k_behaviours.end() ? -1 : static_cast<int>(found - k_behaviours.begin());
        }

        [[nodiscard]] int kind_index(std::string_view word) noexcept
        {
            const auto found = std::ranges::find(k_kind_words, word);
            return found == k_kind_words.end() ? -1 : static_cast<int>(found - k_kind_words.begin());
        }

        /** @brief The State of a state word, or State::None for "default" and for a word that names no state. */
        [[nodiscard]] State state_of_word(std::string_view word) noexcept
        {
            if (word == "merged")
                return State::Merged;
            if (word == "unnamed")
                return State::Unnamed;
            if (word == "nodraw")
                return State::NoDraw;
            if (word == "actor")
                return State::Actor;
            if (word == "animated")
                return State::Animated;
            return State::None;
        }

        [[nodiscard]] constexpr bool takes_path(State state) noexcept
        {
            return state == State::NoDraw || state == State::Actor || state == State::Animated;
        }

        /** @brief The kinds an object in @p state can have, as 1 << Kind. */
        [[nodiscard]] constexpr std::uint8_t state_kinds(State state) noexcept
        {
            switch (state)
            {
            case State::Merged:
                return 1u << static_cast<int>(Kind::Merged);
            case State::Unnamed:
                return 1u << static_cast<int>(Kind::Vegetation);
            case State::NoDraw:
                return (1u << static_cast<int>(Kind::Brush)) | (1u << static_cast<int>(Kind::Movable));
            case State::Actor:
            case State::Animated:
                return 1u << static_cast<int>(Kind::Entity);
            case State::None:
                break;
            }
            return k_all_kinds;
        }

        [[nodiscard]] constexpr bool is_plant(Behaviour behaviour) noexcept
        {
            return behaviour == Behaviour::Tree || behaviour == Behaviour::Bush || behaviour == Behaviour::Grass;
        }

        /** @brief "brush,movable" for the named kinds in @p kinds. */
        [[nodiscard]] std::string kinds_text(std::uint8_t kinds)
        {
            std::string text;
            for (std::size_t i = 0; i < k_kind_words.size(); ++i)
            {
                if ((kinds >> i) & 1u)
                {
                    text += text.empty() ? "" : ",";
                    text += k_kind_words[i];
                }
            }
            return text;
        }

        /** @brief The first @p ch in @p data[at, end), or nullptr. */
        [[nodiscard]] const char *find_byte(const char *data, std::size_t at, std::size_t end, char ch) noexcept
        {
            return at < end ? static_cast<const char *>(std::memchr(data + at, ch, end - at)) : nullptr;
        }

        /**
         * @brief True when @p hay contains @p needle (lower case), ignoring the ASCII case of @p hay.
         * @details Candidates are the places of the needle's first byte in either case (memchr), so a name is not
         *          compared byte by byte at every position.
         */
        [[nodiscard]] bool contains_folded(std::string_view hay, std::string_view needle) noexcept
        {
            if (needle.empty())
            {
                return true;
            }
            if (needle.size() > hay.size())
            {
                return false;
            }
            const char first = needle[0];
            const char upper = (first >= 'a' && first <= 'z') ? static_cast<char>(first - ('a' - 'A')) : first;
            const std::size_t end = hay.size() - needle.size() + 1; // one past the last start
            const char *data = hay.data();
            const char *next_lower = find_byte(data, 0, end, first);
            const char *next_upper = upper != first ? find_byte(data, 0, end, upper) : nullptr;
            while (next_lower != nullptr || next_upper != nullptr)
            {
                const bool take_lower = next_upper == nullptr || (next_lower != nullptr && next_lower < next_upper);
                const char *candidate = take_lower ? next_lower : next_upper;
                const auto at = static_cast<std::size_t>(candidate - data);
                std::size_t j = 1;
                while (j < needle.size() && lower(data[at + j]) == needle[j])
                {
                    ++j;
                }
                if (j == needle.size())
                {
                    return true;
                }
                (take_lower ? next_lower : next_upper) = find_byte(data, at + 1, end, take_lower ? first : upper);
            }
            return false;
        }

        /** @brief One parsed rule before the layers merge (its layer is Rule::source). */
        struct Parsed
        {
            Rule rule;
            bool is_default = false;
        };

        /** @brief The identity of a match: two rules with the same one compete on the very same match. */
        using Selector = std::tuple<int, int, std::string, bool, int>;
        using Group = std::tuple<int, int, std::string>;

        [[nodiscard]] int state_code(const Parsed &parsed) noexcept
        {
            return parsed.is_default ? 6 : static_cast<int>(parsed.rule.state);
        }

        [[nodiscard]] Selector selector_of(const Parsed &parsed)
        {
            return {state_code(parsed), static_cast<int>(parsed.rule.match), parsed.rule.pattern, parsed.rule.has_only,
                    parsed.rule.has_only ? parsed.rule.kinds : 0};
        }

        [[nodiscard]] Group group_of(const Parsed &parsed)
        {
            return {state_code(parsed), static_cast<int>(parsed.rule.match), parsed.rule.pattern};
        }

        [[nodiscard]] std::string where_of(std::string_view source, std::uint32_t line)
        {
            return std::format("{}:{}", source, line);
        }

        /**
         * @brief @p text, UTF-16 behind its byte order mark (Notepad's "Unicode"), as UTF-8. A surrogate without its
         *        pair becomes U+FFFD and an odd last byte is dropped.
         */
        [[nodiscard]] std::string utf16_to_utf8(std::string_view text)
        {
            const bool big_endian = text.starts_with(k_utf16_be);
            std::string out;
            out.reserve(text.size() / 2);
            const auto unit = [text, big_endian](std::size_t at) -> std::uint32_t
            {
                const auto first = static_cast<unsigned char>(text[at]);
                const auto second = static_cast<unsigned char>(text[at + 1]);
                return big_endian ? (std::uint32_t{first} << 8 | second) : (std::uint32_t{second} << 8 | first);
            };
            const auto put = [&out](std::uint32_t code)
            {
                if (code < 0x80)
                {
                    out += static_cast<char>(code);
                }
                else if (code < 0x800)
                {
                    out += static_cast<char>(0xC0 | code >> 6);
                    out += static_cast<char>(0x80 | (code & 0x3F));
                }
                else if (code < 0x10000)
                {
                    out += static_cast<char>(0xE0 | code >> 12);
                    out += static_cast<char>(0x80 | (code >> 6 & 0x3F));
                    out += static_cast<char>(0x80 | (code & 0x3F));
                }
                else
                {
                    out += static_cast<char>(0xF0 | code >> 18);
                    out += static_cast<char>(0x80 | (code >> 12 & 0x3F));
                    out += static_cast<char>(0x80 | (code >> 6 & 0x3F));
                    out += static_cast<char>(0x80 | (code & 0x3F));
                }
            };
            for (std::size_t at = k_utf16_le.size(); at + 1 < text.size(); at += 2)
            {
                std::uint32_t code = unit(at);
                if (code >= 0xD800 && code < 0xDC00 && at + 3 < text.size() && unit(at + 2) >= 0xDC00 &&
                    unit(at + 2) < 0xE000)
                {
                    code = 0x10000 + ((code - 0xD800) << 10) + (unit(at + 2) - 0xDC00);
                    at += 2;
                }
                else if (code >= 0xD800 && code < 0xE000)
                {
                    code = 0xFFFD;
                }
                put(code);
            }
            return out;
        }

        /** @brief The kinds a rule's only= names, the four kinds without only=. */
        [[nodiscard]] std::uint8_t named_kinds(const Rule &rule) noexcept
        {
            return rule.has_only ? rule.kinds : k_named_kinds;
        }

        /** @brief How many kinds the precedence counts for a rule: without only=, four. */
        [[nodiscard]] int kind_count(const Rule &rule) noexcept
        {
            return rule.has_only ? std::popcount(rule.kinds) : 4;
        }

        /**
         * @brief Parses one layer's lines into @p out.
         * @param budget How many more rules the set may take; decremented per rule kept.
         */
        void parse_layer(std::string_view text, std::uint16_t layer, std::string_view source, std::vector<Parsed> &out,
                         std::vector<Diagnostic> &diagnostics, std::size_t &budget)
        {
            // Notepad's "Unicode" is UTF-16 behind a byte order mark: it is read as the same text in UTF-8.
            std::string converted;
            if (text.starts_with(k_utf16_le) || text.starts_with(k_utf16_be))
            {
                converted = utf16_to_utf8(text);
                text = converted;
            }
            // A zero byte left is no text the parser can read (UTF-16 without a byte order mark, or not a text file),
            // so no line would parse. One Error names the file instead of one per line.
            if (text.find('\0') != std::string_view::npos)
            {
                diagnostics.push_back(
                    {Diagnostic::Level::Error,
                     std::format("{}: the file holds zero bytes (UTF-16 without a byte order mark, or not a text "
                                 "file); none of its lines apply until it is saved as UTF-8, ANSI or UTF-16 text",
                                 source),
                     layer});
                return;
            }
            if (text.starts_with(k_bom))
            {
                text.remove_prefix(k_bom.size());
            }
            std::uint32_t number = 0;
            std::size_t at = 0;
            while (at < text.size())
            {
                const std::size_t end = text.find('\n', at);
                std::string_view raw = text.substr(at, end == std::string_view::npos ? end : end - at);
                at = end == std::string_view::npos ? text.size() : end + 1;
                ++number;
                if (raw.ends_with('\r'))
                {
                    raw.remove_suffix(1);
                }
                const std::string_view line = trim(raw.substr(0, raw.find('#')));
                if (line.empty())
                {
                    continue;
                }
                const auto error = [&](std::string_view message)
                {
                    diagnostics.push_back({Diagnostic::Level::Error,
                                           std::format("{}:{}: {}; line skipped", source, number, message), layer});
                };
                const auto warning = [&](std::string_view message)
                {
                    diagnostics.push_back(
                        {Diagnostic::Level::Warning, std::format("{}:{}: {}", source, number, message), layer});
                };

                const std::size_t equals = line.find('=');
                if (equals == std::string_view::npos)
                {
                    error("no '='");
                    continue;
                }
                const std::string_view left = trim(line.substr(0, equals));
                const std::vector<std::string_view> words = split_words(trim(line.substr(equals + 1)));
                const int behaviour = words.empty() ? -1 : behaviour_index(lower_copy(words[0]));
                if (behaviour < 0)
                {
                    error(std::format("unknown behaviour '{}'", words.empty() ? std::string_view{} : words[0]));
                    continue;
                }
                Rule rule{};
                rule.source = layer;
                rule.line = number;
                rule.behaviour = static_cast<Behaviour>(behaviour);
                rule.outcome = rule.behaviour;
                bool bad = false;
                for (std::size_t i = 1; i < words.size() && !bad; ++i)
                {
                    const std::string word = lower_copy(words[i]);
                    const std::string_view value = std::string_view{word}.substr(word.find('=') + 1);
                    if (word == "mesh")
                    {
                        rule.mesh = true;
                    }
                    else if (word.starts_with("if-wall=") || word.starts_with("if-bare="))
                    {
                        if (rule.check != Check::None)
                        {
                            error("a rule takes one of if-wall= and if-bare=");
                            bad = true;
                            break;
                        }
                        rule.check = word.starts_with("if-wall=") ? Check::Wall : Check::Bare;
                        const int outcome = behaviour_index(value);
                        if (outcome < 0 || static_cast<Behaviour>(outcome) == Behaviour::Character ||
                            static_cast<Behaviour>(outcome) == Behaviour::Exclude)
                        {
                            error(std::format("'{}': the outcome must be one of collide, fade, bush, tree, grass, "
                                              "frame, pass",
                                              words[i]));
                            bad = true;
                            break;
                        }
                        rule.outcome = static_cast<Behaviour>(outcome);
                    }
                    else if (word.starts_with("only="))
                    {
                        const std::vector<std::string_view> kinds = split_commas(value);
                        std::uint8_t mask = 0;
                        for (const std::string_view kind : kinds)
                        {
                            const int index = kind_index(kind);
                            if (index < 0)
                            {
                                mask = 0;
                                break;
                            }
                            mask = static_cast<std::uint8_t>(mask | (1u << index));
                        }
                        if (mask == 0)
                        {
                            error(std::format("'{}': kinds are brush, movable, entity, vegetation", words[i]));
                            bad = true;
                            break;
                        }
                        rule.has_only = true;
                        rule.kinds = mask;
                    }
                    else if (word.starts_with("twins="))
                    {
                        const std::vector<std::string_view> twins = split_commas(value);
                        if (twins.empty() ||
                            std::ranges::any_of(twins, [](std::string_view twin) { return twin != "character"; }))
                        {
                            error(std::format("'{}': twins= takes character", words[i]));
                            bad = true;
                            break;
                        }
                        rule.twins_character = true;
                    }
                    else
                    {
                        error(std::format("unknown option '{}'", words[i]));
                        bad = true;
                    }
                }
                if (bad)
                {
                    continue;
                }
                if (rule.behaviour == Behaviour::Exclude && rule.check != Check::None)
                {
                    error("an excluded object is never scanned, so no check can run on it");
                    continue;
                }

                Parsed parsed{.rule = rule};
                Rule &r = parsed.rule;
                bool has_state = false;
                std::vector<std::string_view> parts = split_words(left);
                if (!parts.empty() && parts[0].starts_with('@'))
                {
                    const std::string word = lower_copy(parts[0].substr(1));
                    const State state = state_of_word(word);
                    if (word != "default" && state == State::None)
                    {
                        error(std::format("unknown state '{}'", parts[0]));
                        continue;
                    }
                    has_state = true;
                    parsed.is_default = word == "default";
                    r.state = state;
                    parts.erase(parts.begin());
                    if (!parts.empty() && !takes_path(state))
                    {
                        error(std::format("@{} cannot take a model path", word));
                        continue;
                    }
                    if (parsed.is_default && r.has_only)
                    {
                        error("@default applies to every object; only= is not allowed");
                        continue;
                    }
                    if ((state == State::Merged || state == State::Unnamed) && r.has_only)
                    {
                        error(std::format("@{} has its own kind; only= is not allowed", word));
                        continue;
                    }
                }
                if (parts.size() > 1)
                {
                    error("more than one match");
                    continue;
                }
                if (!parts.empty())
                {
                    const std::string_view part = parts[0];
                    if (part.starts_with('*') || part.ends_with('*'))
                    {
                        if (has_state)
                        {
                            error("a state takes a model path, not a text match");
                            continue;
                        }
                        const std::string_view inner = part.size() >= 3 && part.starts_with('*') && part.ends_with('*')
                                                           ? part.substr(1, part.size() - 2)
                                                           : std::string_view{};
                        if (inner.empty() || inner.find('*') != std::string_view::npos)
                        {
                            error(std::format("'{}': a text match is *text* with no other *", part));
                            continue;
                        }
                        r.match = MatchType::Text;
                        r.pattern = lower_copy(inner);
                        std::ranges::replace(r.pattern, '\\', '/');
                    }
                    else
                    {
                        if (part.find('*') != std::string_view::npos)
                        {
                            error(std::format("'{}': * is only allowed around a text match", part));
                            continue;
                        }
                        r.pattern = lower_copy(part);
                        std::ranges::replace(r.pattern, '\\', '/');
                        r.pattern.erase(0, r.pattern.find_first_not_of('/'));
                        if (r.pattern.empty())
                        {
                            error("empty match");
                            continue;
                        }
                        r.match = std::ranges::any_of(k_exact_extensions,
                                                      [&r](std::string_view ext) { return r.pattern.ends_with(ext); })
                                      ? MatchType::Exact
                                      : MatchType::Prefix;
                    }
                    if (r.pattern.size() > MAX_MATCH)
                    {
                        error(std::format("the match is longer than {} characters", MAX_MATCH));
                        continue;
                    }
                }
                else if (!has_state)
                {
                    error("no match");
                    continue;
                }
                if (budget == 0)
                {
                    error(std::format("more than {} rules", MAX_RULES));
                    continue;
                }
                if (has_state && !parsed.is_default)
                {
                    r.tier = r.match == MatchType::None ? 3 : 1;
                }
                else if (parsed.is_default)
                {
                    r.tier = 5;
                }
                else
                {
                    r.tier = r.match == MatchType::Text ? 2 : 4;
                }
                if (!r.has_only)
                {
                    r.kinds = k_all_kinds;
                }
                if (has_state && !parsed.is_default && r.has_only && (r.kinds & state_kinds(r.state)) == 0)
                {
                    warning(
                        std::format("only= leaves no kind @{} can have; the rule never applies", state_name(r.state)));
                }
                const bool ends_in_collide =
                    r.behaviour == Behaviour::Collide || (r.check != Check::None && r.outcome == Behaviour::Collide);
                if (r.mesh && !ends_in_collide)
                {
                    warning("'mesh' has no effect: the rule never ends in collide");
                }
                const bool plant = is_plant(r.behaviour) || (r.check != Check::None && is_plant(r.outcome));
                const bool frame =
                    r.behaviour == Behaviour::Frame || (r.check != Check::None && r.outcome == Behaviour::Frame);
                if ((r.tier == 5 || (r.tier == 3 && takes_path(r.state))) && (plant || frame || r.twins_character))
                {
                    warning("a rule without a model path prepares no shader twins; the see-through window and the lens "
                            "cut open only models a path rule prepares");
                }
                if (plant && frame)
                {
                    // A model whose check result changes leaves its window or lens-cut track and takes the other.
                    warning(std::format("'{}' and '{}' use different shader twins (the see-through window and the lens "
                                        "cut); a model whose check changes moves from one to the other each time",
                                        behaviour_name(r.behaviour), behaviour_name(r.outcome)));
                }
                r.twin_bits = static_cast<std::uint8_t>((plant ? TWIN_PLANTS : 0) | (frame ? TWIN_FRAMES : 0));
                --budget;
                out.push_back(std::move(parsed));
            }
        }

        /**
         * @brief The precedence within one tier: a later layer whatever the length of its match, then the longer
         *        match, then fewer kinds, then a later line.
         */
        [[nodiscard]] bool precedes(const Rule &a, const Rule &b) noexcept
        {
            if (a.source != b.source)
            {
                return a.source > b.source;
            }
            if (a.pattern.size() != b.pattern.size())
            {
                return a.pattern.size() > b.pattern.size();
            }
            if (kind_count(a) != kind_count(b))
            {
                return kind_count(a) < kind_count(b);
            }
            return a.line > b.line;
        }

        /** @brief True when @p wide matches every name @p narrow matches (two rules of one tier). */
        [[nodiscard]] bool covers(const Rule &wide, const Rule &narrow) noexcept
        {
            if (wide.match == MatchType::Text)
            {
                return narrow.pattern.find(wide.pattern) != std::string::npos;
            }
            return (wide.match == MatchType::Prefix || wide.pattern == narrow.pattern) &&
                   narrow.pattern.starts_with(wide.pattern);
        }

    } // namespace

    const char *behaviour_name(Behaviour behaviour) noexcept
    {
        const auto index = static_cast<std::size_t>(behaviour);
        return index < k_behaviours.size() ? k_behaviours[index].data() : "?";
    }

    const char *kind_name(Kind kind) noexcept
    {
        const auto index = static_cast<std::size_t>(kind);
        return index < k_kind_words.size() ? k_kind_words[index].data() : "merged";
    }

    const char *state_name(State state) noexcept
    {
        switch (state)
        {
        case State::Merged:
            return "merged";
        case State::NoDraw:
            return "nodraw";
        case State::Actor:
            return "actor";
        case State::Animated:
            return "animated";
        case State::Unnamed:
            return "unnamed";
        case State::None:
            break;
        }
        return "";
    }

    std::string describe(const Verdict &verdict)
    {
        const auto word = [&verdict](Behaviour behaviour) -> std::string
        {
            return behaviour == Behaviour::Collide && verdict.mesh ? std::string{"collide mesh"}
                                                                   : std::string{behaviour_name(behaviour)};
        };
        std::string text = word(verdict.behaviour);
        if (verdict.check == Check::Wall)
        {
            text += " if-wall=" + word(verdict.outcome);
        }
        else if (verdict.check == Check::Bare)
        {
            text += " if-bare=" + word(verdict.outcome);
        }
        return text;
    }

    std::shared_ptr<const RuleSet> compile(std::span<const SourceText> layers, std::uint64_t generation,
                                           std::vector<Diagnostic> &diagnostics)
    {
        auto set = std::make_shared<RuleSet>();
        set->m_generation = generation;
        std::size_t budget = MAX_RULES;
        // The rules in force, by the slot their selector first took; a later rule on the same selector replaces it.
        std::vector<Parsed> current;
        std::map<Selector, std::size_t> by_selector;
        // Every rule a group (state and match) ever held, with its slot, for the override warning across another only=.
        std::map<Group, std::vector<std::pair<std::size_t, Rule>>> groups;
        // Rule::source and Diagnostic::layer hold a layer index as a std::uint16_t below NO_LAYER.
        const std::size_t layer_count = std::min(layers.size(), std::size_t{NO_LAYER});
        if (layer_count < layers.size())
        {
            diagnostics.push_back({Diagnostic::Level::Warning,
                                   std::format("{}: more than {} rules files; it and every later file are not read",
                                               layers[layer_count].name, NO_LAYER),
                                   NO_LAYER});
        }
        for (std::size_t layer = 0; layer < layer_count; ++layer)
        {
            const SourceText &source = layers[layer];
            const auto index = static_cast<std::uint16_t>(layer);
            set->m_sources.push_back(source.name);
            set->m_roles.push_back(layer == 0 ? SourceRole::Main : source.role);
            // Explorer hides known extensions, so a file named there can end in .txt twice: the User file named so is
            // read as a patch, and a later-named patch beats it.
            if (layer != 0 && source.role == SourceRole::Patch && lower_copy(source.name).ends_with(".txt.txt"))
            {
                diagnostics.push_back(
                    {Diagnostic::Level::Warning,
                     std::format("{}: the name ends in .txt twice (Windows hides known extensions), so it is read as a "
                                 "patch; rename it to {}",
                                 source.name, std::string_view{source.name}.substr(0, source.name.size() - 4)),
                     index});
            }
            std::vector<Parsed> parsed;
            parse_layer(source.text, index, source.name, parsed, diagnostics, budget);
            SourceCounts counts{.name = source.name};
            for (Parsed &entry : parsed)
            {
                const Rule &r = entry.rule;
                const std::string here = where_of(source.name, r.line);
                // The catch-all is the player's: the main file or the player's own file, never another mod's patch.
                if (entry.is_default && layer != 0 && source.role != SourceRole::User)
                {
                    diagnostics.push_back(
                        {Diagnostic::Level::Warning,
                         std::format("{}: @default belongs in the main file or in the User file; line ignored", here),
                         index});
                    ++budget;
                    continue;
                }
                const Selector selector = selector_of(entry);
                const auto old = by_selector.find(selector);
                if (old != by_selector.end())
                {
                    const Rule &previous = current[old->second].rule;
                    const std::string there = where_of(layers[previous.source].name, previous.line);
                    diagnostics.push_back({Diagnostic::Level::Warning,
                                           previous.source == r.source
                                               ? std::format("{} repeats {}; the later line wins", here, there)
                                               : std::format("{} overrides {} (same match)", here, there),
                                           index});
                }
                const Group group = group_of(entry);
                for (const auto &[slot, other] : groups[group])
                {
                    if (other.source == r.source || (old != by_selector.end() && slot == old->second))
                    {
                        continue;
                    }
                    const Rule &held = current[slot].rule;
                    if (held.source != other.source || held.line != other.line)
                    {
                        continue; // replaced since
                    }
                    auto both = static_cast<std::uint8_t>(named_kinds(r) & named_kinds(other));
                    if (r.tier == 1 || r.tier == 3)
                    {
                        both = static_cast<std::uint8_t>(both & state_kinds(r.state));
                    }
                    if (both != 0)
                    {
                        diagnostics.push_back(
                            {Diagnostic::Level::Warning,
                             std::format("{} overrides {} for {} (same match)", here,
                                         where_of(layers[other.source].name, other.line), kinds_text(both)),
                             index});
                    }
                }
                std::size_t slot = current.size();
                if (old != by_selector.end())
                {
                    slot = old->second;
                    current[slot] = entry;
                }
                else
                {
                    by_selector.emplace(selector, slot);
                    current.push_back(entry);
                }
                groups[group].emplace_back(slot, r);
                switch (r.tier)
                {
                case 1:
                    ++counts.state_path;
                    break;
                case 2:
                    ++counts.text;
                    break;
                case 3:
                    ++counts.state;
                    break;
                case 4:
                    ++counts.path;
                    break;
                default:
                    ++counts.defaults;
                    break;
                }
            }
            set->m_counts.push_back(std::move(counts));
        }

        set->m_rules.reserve(current.size());
        for (Parsed &entry : current)
        {
            if (entry.is_default)
            {
                set->m_default = static_cast<std::uint16_t>(set->m_rules.size());
            }
            set->m_rules.push_back(std::move(entry.rule));
        }
        if (set->m_default == NO_RULE)
        {
            diagnostics.push_back({Diagnostic::Level::Warning,
                                   std::format("{}: no @default rule; objects no rule matches are excluded",
                                               layers.empty() ? std::string_view{} : std::string_view{layers[0].name}),
                                   layers.empty() ? NO_LAYER : std::uint16_t{0}});
        }

        const auto order = [&set](std::uint16_t a, std::uint16_t b)
        { return precedes(set->m_rules[a], set->m_rules[b]); };
        // The trie: built with ordered child maps, then flattened so a walk reads plain arrays.
        struct BuildNode
        {
            std::map<char, std::uint32_t> children;
            std::vector<std::uint16_t> rules;
            std::uint32_t depth = 0;
        };
        std::vector<BuildNode> build(1);
        for (std::size_t i = 0; i < set->m_rules.size(); ++i)
        {
            const Rule &r = set->m_rules[i];
            const auto index = static_cast<std::uint16_t>(i);
            if (r.tier == 2)
            {
                set->m_text.push_back(index);
                set->m_text_twin_bits = static_cast<std::uint8_t>(set->m_text_twin_bits | r.twin_bits);
            }
            else if (r.tier == 3)
            {
                set->m_state[static_cast<std::size_t>(r.state)].push_back(index);
            }
            else if (r.tier == 1 || r.tier == 4)
            {
                std::uint32_t node = 0;
                for (const char byte : r.pattern)
                {
                    const auto found = build[node].children.find(byte);
                    if (found != build[node].children.end())
                    {
                        node = found->second;
                        continue;
                    }
                    const auto child = static_cast<std::uint32_t>(build.size());
                    const std::uint32_t depth = build[node].depth + 1;
                    build[node].children.emplace(byte, child);
                    build.push_back(BuildNode{.depth = depth});
                    node = child;
                }
                build[node].rules.push_back(index);
            }
        }
        std::ranges::stable_sort(set->m_text, order);
        for (auto &list : set->m_state)
        {
            std::ranges::stable_sort(list, order);
        }
        set->m_nodes.resize(build.size());
        for (std::size_t i = 0; i < build.size(); ++i)
        {
            BuildNode &from = build[i];
            RuleSet::TrieNode &to = set->m_nodes[i];
            to.depth = from.depth;
            to.first_edge = static_cast<std::uint32_t>(set->m_edges.size());
            to.edge_count = static_cast<std::uint32_t>(from.children.size());
            for (const auto &[byte, child] : from.children)
            {
                set->m_edges.push_back({.byte = byte, .child = child});
            }
            std::ranges::stable_sort(from.rules, order);
            to.first_rule = static_cast<std::uint32_t>(set->m_node_rules.size());
            to.rule_count = static_cast<std::uint32_t>(from.rules.size());
            set->m_node_rules.insert(set->m_node_rules.end(), from.rules.begin(), from.rules.end());
        }

        // A text match decides before every model path, so a path rule whose every model holds a text match's text
        // never decides the kinds both count for. Named once per path rule and kind, by the text match that wins.
        for (const Rule &path : set->m_rules)
        {
            if (path.tier != 4)
            {
                continue;
            }
            std::uint8_t taken = 0;
            for (const std::uint16_t index : set->m_text)
            {
                const Rule &text = set->m_rules[index];
                const auto kinds = static_cast<std::uint8_t>(named_kinds(path) & named_kinds(text) & ~taken);
                if (kinds == 0 || path.pattern.find(text.pattern) == std::string::npos)
                {
                    continue;
                }
                taken = static_cast<std::uint8_t>(taken | kinds);
                diagnostics.push_back(
                    {Diagnostic::Level::Warning,
                     std::format("{}: '{}' never decides {}: the text match {} '*{}*' decides those models first (a "
                                 "text match comes before every model path)",
                                 where_of(set->m_sources[path.source], path.line), path.pattern, kinds_text(kinds),
                                 where_of(set->m_sources[text.source], text.line), text.pattern),
                     path.source});
            }
        }

        // A later layer wins on its own tier whatever the length of its match, so a folder line of the User file
        // decides the models the main file lists one by one. The rule that now decides, for some kind, the models of
        // a longer match of an earlier layer is named once, with how many such lines of which file.
        std::vector<std::uint16_t> later;
        for (std::size_t i = 0; i < set->m_rules.size(); ++i)
        {
            const Rule &r = set->m_rules[i];
            if (r.source != 0 && (r.tier == 1 || r.tier == 2 || r.tier == 4))
            {
                later.push_back(static_cast<std::uint16_t>(i));
            }
        }
        // (layer, line, rule) of each such rule -> the longer lines it decides, per layer.
        std::map<std::tuple<std::uint16_t, std::uint32_t, std::uint16_t>, std::vector<std::size_t>> beaten;
        for (std::size_t e = 0; e < set->m_rules.size() && !later.empty(); ++e)
        {
            const Rule &earlier = set->m_rules[e];
            if (earlier.tier != 1 && earlier.tier != 2 && earlier.tier != 4)
            {
                continue;
            }
            auto kinds = named_kinds(earlier);
            if (earlier.tier == 1)
            {
                kinds = static_cast<std::uint8_t>(kinds & state_kinds(earlier.state));
            }
            // Per kind, the rule of a later layer that wins over every model of this one.
            std::array<std::uint16_t, 4> winners;
            winners.fill(NO_RULE);
            for (std::size_t k = 0; k < winners.size(); ++k)
            {
                if (((kinds >> k) & 1u) == 0)
                {
                    continue;
                }
                for (const std::uint16_t index : later)
                {
                    const Rule &r = set->m_rules[index];
                    if (r.source > earlier.source && r.tier == earlier.tier && r.state == earlier.state &&
                        ((named_kinds(r) >> k) & 1u) != 0 && covers(r, earlier) &&
                        (winners[k] == NO_RULE || precedes(r, set->m_rules[winners[k]])))
                    {
                        winners[k] = index;
                    }
                }
            }
            for (std::size_t k = 0; k < winners.size(); ++k)
            {
                const std::uint16_t index = winners[k];
                const auto seen = winners.begin() + static_cast<std::ptrdiff_t>(k);
                if (index == NO_RULE || set->m_rules[index].pattern.size() >= earlier.pattern.size() ||
                    std::find(winners.begin(), seen, index) != seen)
                {
                    continue; // none, counted for another kind, or as long (the same match, logged above)
                }
                const Rule &r = set->m_rules[index];
                std::vector<std::size_t> &per_layer = beaten[{r.source, r.line, index}];
                per_layer.resize(set->m_sources.size());
                ++per_layer[earlier.source];
            }
        }
        for (const auto &[key, per_layer] : beaten)
        {
            const auto [source, line, index] = key;
            std::vector<std::string> parts;
            for (std::size_t layer = 0; layer < per_layer.size(); ++layer)
            {
                if (per_layer[layer] != 0)
                {
                    parts.push_back(std::format("{} longer line{} of {}", per_layer[layer],
                                                per_layer[layer] == 1 ? "" : "s", set->m_sources[layer]));
                }
            }
            std::string lines = parts.front();
            for (std::size_t i = 1; i < parts.size(); ++i)
            {
                lines += (i + 1 == parts.size() ? " and " : ", ") + parts[i];
            }
            diagnostics.push_back({Diagnostic::Level::Warning,
                                   std::format("{}: '{}' overrides {} (a later file wins over an earlier one at the "
                                               "same level, whatever the length of the match)",
                                               where_of(set->m_sources[source], line), set->match_text(index), lines),
                                   source});
        }
        return set;
    }

    std::vector<RuleFile> rule_files(std::span<const std::string> names, std::string_view mod_name)
    {
        const std::string main = lower_copy(std::format("{}_rules.txt", mod_name));
        const std::string prefix = lower_copy(std::format("{}_rules_", mod_name));
        const std::string user = prefix + "user.txt";
        // (role order, name with A-Z folded, the file); the folded name sorts, the name as written breaks a tie.
        std::vector<std::tuple<int, std::string, RuleFile>> found;
        for (const std::string &name : names)
        {
            std::string key = lower_copy(name);
            if (key.ends_with(REFERENCE_SUFFIX))
            {
                continue; // a reference copy, such as the built-in rules ExportRules writes
            }
            if (key == main)
            {
                found.emplace_back(0, std::move(key), RuleFile{.name = name, .role = SourceRole::Main});
            }
            else if (key == user)
            {
                found.emplace_back(2, std::move(key), RuleFile{.name = name, .role = SourceRole::User});
            }
            else if (key.starts_with(prefix) && key.ends_with(".txt"))
            {
                found.emplace_back(1, std::move(key), RuleFile{.name = name, .role = SourceRole::Patch});
            }
        }
        std::ranges::sort(found,
                          [](const auto &a, const auto &b)
                          {
                              return std::tie(std::get<0>(a), std::get<1>(a), std::get<2>(a).name) <
                                     std::tie(std::get<0>(b), std::get<1>(b), std::get<2>(b).name);
                          });
        std::vector<RuleFile> files;
        files.reserve(found.size());
        for (auto &entry : found)
        {
            files.push_back(std::move(std::get<2>(entry)));
        }
        return files;
    }

    SetChanges compare(const RuleSet &before, const RuleSet &after)
    {
        // A rule's identity: its file (the main file and its built-in copy are one), state, match and only=.
        using Key = std::tuple<std::string, int, int, std::string, bool, int>;
        const auto key_of = [](const RuleSet &set, const Rule &r) -> Key
        {
            return {set.source_role(r.source) == SourceRole::Main ? std::string{} : lower_copy(set.source(r.source)),
                    r.tier == 5 ? 6 : static_cast<int>(r.state),
                    static_cast<int>(r.match),
                    r.pattern,
                    r.has_only,
                    r.has_only ? r.kinds : 0};
        };
        const auto same = [](const Rule &a, const Rule &b)
        {
            return a.behaviour == b.behaviour && a.check == b.check && a.outcome == b.outcome && a.mesh == b.mesh &&
                   a.twins_character == b.twins_character;
        };
        struct Line
        {
            std::uint16_t source;
            std::uint32_t line;
            std::string text;
        };
        std::map<Key, std::uint16_t> old;
        for (std::size_t i = 0; i < before.size(); ++i)
        {
            old.emplace(key_of(before, *before.rule(static_cast<std::uint16_t>(i))), static_cast<std::uint16_t>(i));
        }
        SetChanges changes;
        std::vector<Line> changed;
        std::vector<Line> added;
        std::vector<Line> removed;
        std::set<Key> kept;
        for (std::size_t i = 0; i < after.size(); ++i)
        {
            const auto index = static_cast<std::uint16_t>(i);
            const Rule &r = *after.rule(index);
            Key key = key_of(after, r);
            const auto found = old.find(key);
            if (found == old.end())
            {
                added.push_back(
                    {r.source, r.line, std::format("+ {}  {}", after.where(index), after.rule_text(index))});
            }
            else if (!same(*before.rule(found->second), r))
            {
                changed.push_back({r.source, r.line,
                                   std::format("~ {}  {} (was {})", after.where(index), after.rule_text(index),
                                               before.rule_text(found->second))});
            }
            else if (before.where(found->second) != after.where(index))
            {
                ++changes.moved;
            }
            kept.insert(std::move(key));
        }
        for (std::size_t i = 0; i < before.size(); ++i)
        {
            const auto index = static_cast<std::uint16_t>(i);
            const Rule &r = *before.rule(index);
            if (!kept.contains(key_of(before, r)))
            {
                removed.push_back(
                    {r.source, r.line, std::format("- {}  {}", before.where(index), before.rule_text(index))});
            }
        }
        for (std::vector<Line> *group : {&changed, &added, &removed})
        {
            std::ranges::sort(*group, [](const Line &a, const Line &b)
                              { return std::tie(a.source, a.line) < std::tie(b.source, b.line); });
            for (Line &line : *group)
            {
                changes.lines.push_back(std::move(line.text));
            }
        }
        return changes;
    }

    std::size_t RuleSet::walk(std::string_view name, std::span<std::uint32_t> hits) const noexcept
    {
        std::size_t count = 0;
        if (m_nodes.empty())
        {
            return 0;
        }
        std::uint32_t node = 0;
        for (const char raw : name)
        {
            const char byte = lower(raw);
            const TrieNode &at = m_nodes[node];
            std::uint32_t next = 0;
            for (std::uint32_t e = 0; e < at.edge_count; ++e)
            {
                const TrieEdge &edge = m_edges[at.first_edge + e];
                if (edge.byte == byte)
                {
                    next = edge.child;
                    break;
                }
            }
            if (next == 0)
            {
                break;
            }
            node = next;
            if (m_nodes[node].rule_count != 0 && count < hits.size())
            {
                hits[count++] = node;
            }
        }
        return count;
    }

    bool RuleSet::text_contained(std::string_view name, std::size_t order, TextHits &text) const noexcept
    {
        const std::uint64_t bit = order < 64 ? std::uint64_t{1} << order : 0;
        if ((text.known & bit) != 0)
        {
            return (text.contained & bit) != 0;
        }
        const bool found = contains_folded(name, m_rules[m_text[order]].pattern);
        text.known |= bit;
        text.contained |= found ? bit : 0;
        return found;
    }

    std::uint16_t RuleSet::decide(std::string_view name, Kind kind, State state, std::span<const std::uint32_t> hits,
                                  TextHits &text) const noexcept
    {
        const auto kind_bit = static_cast<std::uint8_t>(1u << static_cast<int>(kind));
        // A later layer wins whatever the length of its match; within one layer the longest match wins. The hits run
        // deepest first and each node's rules in precedence order (later layer first), so a shallower match replaces
        // the best only from a later layer, and the walk ends at a match of the last layer.
        const auto path_rule = [&](std::uint8_t tier, State wanted) -> std::uint16_t
        {
            std::uint16_t best = NO_RULE;
            for (std::size_t h = hits.size(); h-- > 0;)
            {
                const TrieNode &node = m_nodes[hits[h]];
                const bool whole = node.depth == name.size();
                for (std::uint32_t i = 0; i < node.rule_count; ++i)
                {
                    const std::uint16_t index = m_node_rules[node.first_rule + i];
                    const Rule &r = m_rules[index];
                    if (best != NO_RULE && r.source <= m_rules[best].source)
                    {
                        break;
                    }
                    if (r.tier == tier && r.state == wanted && (r.kinds & kind_bit) != 0 &&
                        (r.match == MatchType::Prefix || whole))
                    {
                        best = index;
                        break;
                    }
                }
                if (best != NO_RULE && m_rules[best].source + std::size_t{1} == m_sources.size())
                {
                    break;
                }
            }
            return best;
        };
        const auto state_rule = [&]() -> std::uint16_t
        {
            for (const std::uint16_t index : m_state[static_cast<std::size_t>(state)])
            {
                if ((m_rules[index].kinds & kind_bit) != 0)
                {
                    return index;
                }
            }
            return NO_RULE;
        };
        if (takes_path(state) && !name.empty())
        {
            if (const std::uint16_t found = path_rule(1, state); found != NO_RULE)
            {
                return found;
            }
        }
        if (state == State::Actor || state == State::Animated)
        {
            if (const std::uint16_t found = state_rule(); found != NO_RULE)
            {
                return found;
            }
        }
        if (!name.empty())
        {
            for (std::size_t i = 0; i < m_text.size(); ++i)
            {
                if ((m_rules[m_text[i]].kinds & kind_bit) != 0 && text_contained(name, i, text))
                {
                    return m_text[i];
                }
            }
        }
        if (state != State::None)
        {
            if (const std::uint16_t found = state_rule(); found != NO_RULE)
            {
                return found;
            }
        }
        if (!name.empty())
        {
            if (const std::uint16_t found = path_rule(4, State::None); found != NO_RULE)
            {
                return found;
            }
        }
        return m_default;
    }

    Verdict RuleSet::classify(std::string_view name, Kind kind, State state) const noexcept
    {
        std::array<std::uint32_t, MAX_MATCH + 1> hits;
        const std::size_t count = walk(name, hits);
        TextHits text;
        const std::uint16_t index = decide(name, kind, state, std::span{hits.data(), count}, text);
        if (index == NO_RULE)
        {
            return Verdict{};
        }
        const Rule &r = m_rules[index];
        return Verdict{
            .behaviour = r.behaviour,
            .check = r.check,
            .outcome = r.outcome,
            .mesh = r.mesh,
            .rule = index,
        };
    }

    std::uint8_t RuleSet::twin_scopes(std::string_view name) const noexcept
    {
        if (name.empty())
        {
            return 0;
        }
        std::array<std::uint32_t, MAX_MATCH + 1> hit_storage;
        const std::span<const std::uint32_t> hits{hit_storage.data(), walk(name, hit_storage)};
        TextHits text;
        // The bits any placement could give: those of the path rules on the walk and of every text rule. Placements
        // are decided only while their union is short of that bound, so most names decide none.
        auto bound = m_text_twin_bits;
        for (const std::uint32_t at : hits)
        {
            const TrieNode &node = m_nodes[at];
            for (std::uint32_t i = 0; i < node.rule_count; ++i)
            {
                const Rule &r = m_rules[m_node_rules[node.first_rule + i]];
                bound = static_cast<std::uint8_t>(
                    bound | ((r.match == MatchType::Prefix || node.depth == name.size()) ? r.twin_bits : 0));
            }
        }
        static constexpr std::pair<Kind, State> k_placements[] = {
            {Kind::Vegetation, State::None}, {Kind::Entity, State::None},     {Kind::Brush, State::None},
            {Kind::Movable, State::None},    {Kind::Brush, State::NoDraw},    {Kind::Movable, State::NoDraw},
            {Kind::Entity, State::Actor},    {Kind::Entity, State::Animated},
        };
        std::uint8_t bits = 0;
        for (std::size_t p = 0; p < std::size(k_placements) && (bits & bound) != bound; ++p)
        {
            const std::uint16_t index = decide(name, k_placements[p].first, k_placements[p].second, hits, text);
            if (index != NO_RULE && m_rules[index].tier != 3 && m_rules[index].tier != 5)
            {
                bits = static_cast<std::uint8_t>(bits | m_rules[index].twin_bits);
            }
        }
        // The close-up fade's preparation follows any rule that matches the path, not the camera's deciding rule.
        for (std::size_t h = 0; h < hits.size() && (bits & TWIN_CHARACTER) == 0; ++h)
        {
            const TrieNode &node = m_nodes[hits[h]];
            for (std::uint32_t i = 0; i < node.rule_count; ++i)
            {
                const Rule &r = m_rules[m_node_rules[node.first_rule + i]];
                if (r.twins_character && (r.match == MatchType::Prefix || node.depth == name.size()))
                {
                    bits = static_cast<std::uint8_t>(bits | TWIN_CHARACTER);
                    break;
                }
            }
        }
        for (std::size_t i = 0; i < m_text.size() && (bits & TWIN_CHARACTER) == 0; ++i)
        {
            if (m_rules[m_text[i]].twins_character && text_contained(name, i, text))
            {
                bits = static_cast<std::uint8_t>(bits | TWIN_CHARACTER);
            }
        }
        return bits;
    }

    const Rule *RuleSet::rule(std::uint16_t index) const noexcept
    {
        return index < m_rules.size() ? &m_rules[index] : nullptr;
    }

    std::string_view RuleSet::source(std::uint16_t index) const noexcept
    {
        return index < m_sources.size() ? std::string_view{m_sources[index]} : std::string_view{};
    }

    std::span<const SourceCounts> RuleSet::counts() const noexcept
    {
        return m_counts;
    }

    std::size_t RuleSet::size() const noexcept
    {
        return m_rules.size();
    }

    std::uint16_t RuleSet::catch_all() const noexcept
    {
        return m_default;
    }

    std::uint64_t RuleSet::generation() const noexcept
    {
        return m_generation;
    }

    std::string RuleSet::where(std::uint16_t index) const
    {
        const Rule *r = rule(index);
        return r != nullptr ? where_of(source(r->source), r->line) : std::string{"<no rule>"};
    }

    std::string RuleSet::match_text(std::uint16_t index) const
    {
        const Rule *r = rule(index);
        if (r == nullptr)
        {
            return {};
        }
        switch (r->tier)
        {
        case 1:
            return std::format("@{} {}", state_name(r->state), r->pattern);
        case 2:
            return std::format("*{}*", r->pattern);
        case 3:
            return std::format("@{}", state_name(r->state));
        case 5:
            return "@default";
        default:
            return r->pattern;
        }
    }

    std::string RuleSet::rule_text(std::uint16_t index) const
    {
        const Rule *r = rule(index);
        if (r == nullptr)
        {
            return {};
        }
        // describe() writes mesh after each collide; a mesh with no collide to follow is written at the end.
        std::string text = std::format(
            "{} = {}", match_text(index),
            describe(Verdict{.behaviour = r->behaviour, .check = r->check, .outcome = r->outcome, .mesh = r->mesh}));
        const bool ends_in_collide =
            r->behaviour == Behaviour::Collide || (r->check != Check::None && r->outcome == Behaviour::Collide);
        if (r->mesh && !ends_in_collide)
        {
            text += " mesh";
        }
        if (r->has_only)
        {
            text += " only=" + kinds_text(r->kinds);
        }
        if (r->twins_character)
        {
            text += " twins=character";
        }
        return text;
    }

    SourceRole RuleSet::source_role(std::uint16_t index) const noexcept
    {
        return index < m_roles.size() ? m_roles[index] : SourceRole::Main;
    }

} // namespace TPVCamera::Rules
