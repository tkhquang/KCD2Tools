/**
 * @file packs/voice_lines.cpp
 * @brief Builds and searches the voice line catalog (see voice_lines.hpp).
 *
 * The XML files are scanned as text: the dialogue files come to 150 MB and only a few of their elements matter here.
 */

#include "packs/voice_lines.hpp"
#include "packs/zip_reader.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace SoundFramework::packs
{
    namespace
    {
        constexpr std::string_view TEXT_FILE = "text_ui_dialog.xml";
        constexpr std::string_view DIALOGUE_FOLDER = "quests/";
        constexpr std::string_view DIALOGUE_EXTENSION = ".xml";
        constexpr std::string_view VOICE_FOLDER = "dialog/";
        constexpr std::string_view VOICE_EXTENSION = ".ogg";
        constexpr std::array<std::string_view, 2> HENRY_PREFIXES{"tmck_", "cin__henry_"};
        // The cast's name for a voice no actor recorded.
        constexpr std::string_view NO_ACTOR = "fejk";

        constexpr std::string_view ROW_OPEN = "<Row>";
        constexpr std::string_view ROW_CLOSE = "</Row>";
        constexpr std::string_view CELL_OPEN = "<Cell>";
        constexpr std::string_view CELL_CLOSE = "</Cell>";
        constexpr std::string_view CAST_OPEN = "<SelectedSoul Role=\"";
        constexpr std::string_view CAST_VOICE = "\" Voice=\"";
        constexpr std::string_view RESPONSE_MARK = "<Response ";
        constexpr std::string_view RESPONSE_OPEN = "<Response Role=\"";
        constexpr std::string_view RESPONSE_CLOSE = "</Response>";
        constexpr std::string_view DIRECTION_OPEN = "<Parenthetical>";
        constexpr std::string_view DIRECTION_CLOSE = "</Parenthetical>";
        constexpr std::string_view TEXT_OPEN = "<Text StringName=\"";

        constexpr std::array<std::pair<std::string_view, std::string_view>, 5> ENTITIES{{
            {"&lt;", "<"},
            {"&gt;", ">"},
            {"&quot;", "\""},
            {"&apos;", "'"},
            {"&amp;", "&"},
        }};

        constexpr char FIELD_SEPARATOR = '\t';
        // Letters in a row that make a text a word and not a stutter or punctuation.
        constexpr int WORD_LETTERS = 3;

        [[nodiscard]] constexpr char ascii_lower(char c) noexcept
        {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        }

        [[nodiscard]] constexpr char ascii_upper(char c) noexcept
        {
            return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
        }

        [[nodiscard]] std::string lowered(std::string_view text)
        {
            std::string out{text};
            std::ranges::transform(out, out.begin(), ascii_lower);
            return out;
        }

        [[nodiscard]] constexpr bool is_ascii_space(char c) noexcept
        {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
        }

        /** @brief The code point at @p at of valid UTF-8 and its length in bytes. A stray byte counts as itself. */
        [[nodiscard]] constexpr std::pair<std::uint32_t, std::size_t> code_point(std::string_view text, std::size_t at)
        {
            const auto lead = static_cast<std::uint8_t>(text[at]);
            const std::size_t length = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
            if (length == 1 || at + length > text.size())
            {
                return {lead, 1};
            }
            std::uint32_t value = lead & (0xFFU >> (length + 1));
            for (std::size_t i = 1; i < length; ++i)
            {
                value = (value << 6) | (static_cast<std::uint8_t>(text[at + i]) & 0x3FU);
            }
            return {value, length};
        }

        /** @brief True for the space characters beyond ASCII that the game's texts hold. */
        [[nodiscard]] constexpr bool is_wide_space(std::uint32_t c) noexcept
        {
            return c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 ||
                   c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
        }

        /**
         * @brief True for a letter. Beyond ASCII this leaves out the symbol and punctuation blocks: a text in any of
         *        the game's languages only has to count as words, not be classified exactly.
         */
        [[nodiscard]] constexpr bool is_letter(std::uint32_t c) noexcept
        {
            if (c < 0x80)
            {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
            }
            return c == 0xAA || c == 0xB5 || c == 0xBA ||
                   (c >= 0xC0 && c != 0xD7 && c != 0xF7 && !(c >= 0x2000 && c <= 0x2BFF) &&
                    !(c >= 0x3000 && c <= 0x303F) && !(c >= 0xFE00 && c <= 0xFE6F) && !(c >= 0xFF00 && c <= 0xFF20));
        }

        /** @brief What the sort orders a line by: its length in code points, and whether it has a word. */
        struct TextShape
        {
            std::size_t length = 0;
            bool has_word = false;
        };

        [[nodiscard]] TextShape text_shape(std::string_view text)
        {
            TextShape shape;
            int letters = 0;
            for (std::size_t at = 0; at < text.size();)
            {
                const auto [c, length] = code_point(text, at);
                letters = is_letter(c) ? letters + 1 : 0;
                shape.has_word = shape.has_word || letters >= WORD_LETTERS;
                ++shape.length;
                at += length;
            }
            return shape;
        }

        /** @brief @p text with its entities decoded and every run of space made one, without any at the ends. */
        [[nodiscard]] std::string clean_text(std::string_view text)
        {
            std::string decoded{text};
            for (const auto &[entity, character] : ENTITIES)
            {
                for (std::size_t at = decoded.find(entity); at != std::string::npos; at = decoded.find(entity, at + 1))
                {
                    decoded.replace(at, entity.size(), character);
                }
            }
            std::string out;
            out.reserve(decoded.size());
            bool pending_space = false;
            for (std::size_t at = 0; at < decoded.size();)
            {
                const auto [c, length] = code_point(decoded, at);
                if (is_ascii_space(decoded[at]) || is_wide_space(c))
                {
                    pending_space = !out.empty();
                }
                else
                {
                    if (pending_space)
                    {
                        out += ' ';
                        pending_space = false;
                    }
                    out.append(decoded, at, length);
                }
                at += length;
            }
            return out;
        }

        /** @brief @p text from @p at with its leading space skipped. */
        [[nodiscard]] std::size_t skip_space(std::string_view text, std::size_t at) noexcept
        {
            while (at < text.size() && is_ascii_space(text[at]))
            {
                ++at;
            }
            return at;
        }

        /** @brief A `<Cell>value</Cell>` at @p at, moving @p at past it, or nothing. */
        [[nodiscard]] std::optional<std::string_view> read_cell(std::string_view xml, std::size_t &at)
        {
            const std::size_t open = skip_space(xml, at);
            if (!xml.substr(open).starts_with(CELL_OPEN))
            {
                return std::nullopt;
            }
            const std::size_t start = open + CELL_OPEN.size();
            const std::size_t end = xml.find('<', start);
            if (end == std::string_view::npos || !xml.substr(end).starts_with(CELL_CLOSE))
            {
                return std::nullopt;
            }
            at = end + CELL_CLOSE.size();
            return xml.substr(start, end - start);
        }

        /** @brief The text between @p at and the next quote, or nothing. */
        [[nodiscard]] std::optional<std::string_view> quoted(std::string_view xml, std::size_t at)
        {
            const std::size_t end = xml.find('"', at);
            return end == std::string_view::npos ? std::nullopt : std::optional{xml.substr(at, end - at)};
        }

        /** @brief The first `<Text StringName="...">` of a response's body that no stage direction holds, or empty. */
        [[nodiscard]] std::string_view response_key(std::string_view body)
        {
            std::size_t at = 0;
            for (;;)
            {
                const std::size_t text = body.find(TEXT_OPEN, at);
                const std::size_t direction = body.find(DIRECTION_OPEN, at);
                if (text == std::string_view::npos)
                {
                    return {};
                }
                const std::size_t direction_end =
                    direction == std::string_view::npos ? direction : body.find(DIRECTION_CLOSE, direction);
                if (direction_end == std::string_view::npos || text < direction)
                {
                    return quoted(body, text + TEXT_OPEN.size()).value_or(std::string_view{});
                }
                at = direction_end + DIRECTION_CLOSE.size();
            }
        }

        /** @brief JAN_ZIZKA_Z_TROCNOVA as Jan Zizka Z Trocnova. */
        [[nodiscard]] std::string role_name(std::string_view role)
        {
            std::string out;
            for (const auto word : std::views::split(role, '_'))
            {
                if (word.empty())
                {
                    continue;
                }
                if (!out.empty())
                {
                    out += ' ';
                }
                bool first = true;
                for (const char c : word)
                {
                    out += first ? ascii_upper(c) : ascii_lower(c);
                    first = false;
                }
            }
            return out;
        }

        /** @brief tomMcKay as Tom McKay: a word starts at a capital after a small letter, except the K of Mc. */
        [[nodiscard]] std::string actor_name(std::string_view actor)
        {
            std::string out;
            for (std::size_t i = 0; i < actor.size(); ++i)
            {
                const char c = actor[i];
                const bool after_small = i > 0 && actor[i - 1] >= 'a' && actor[i - 1] <= 'z';
                const bool after_mc = i >= 2 && actor.substr(i - 2, 2) == "Mc";
                if (c >= 'A' && c <= 'Z' && after_small && !after_mc)
                {
                    out += ' ';
                }
                out += i == 0 ? ascii_upper(c) : c;
            }
            return out;
        }

        /** @brief Who was cast how often, in the order they first came: a tie goes to the first. */
        using Votes = std::vector<std::pair<std::string, int>>;

        void vote(Votes &votes, const std::string &actor)
        {
            const auto it = std::ranges::find(votes, actor, &std::pair<std::string, int>::first);
            if (it != votes.end())
            {
                ++it->second;
            }
            else
            {
                votes.emplace_back(actor, 1);
            }
        }

        [[nodiscard]] std::string_view most_cast(const Votes &votes)
        {
            std::string_view best;
            int most = 0;
            for (const auto &[actor, count] : votes)
            {
                if (count > most)
                {
                    best = actor;
                    most = count;
                }
            }
            return best;
        }

        [[nodiscard]] std::string_view
        winner(const std::unordered_map<std::string, Votes> &votes, const std::string &name)
        {
            const auto it = votes.find(name);
            return it != votes.end() ? most_cast(it->second) : std::string_view{};
        }
    } // namespace

    void VoiceLineBuilder::add_texts(std::string_view xml)
    {
        for (std::size_t at = xml.find(ROW_OPEN); at != std::string_view::npos; at = xml.find(ROW_OPEN, at))
        {
            at += ROW_OPEN.size();
            std::size_t cursor = at;
            const std::optional<std::string_view> key = read_cell(xml, cursor);
            const std::optional<std::string_view> original = key.has_value() ? read_cell(xml, cursor) : std::nullopt;
            const std::optional<std::string_view> text = original.has_value() ? read_cell(xml, cursor) : std::nullopt;
            if (text.has_value() && xml.substr(skip_space(xml, cursor)).starts_with(ROW_CLOSE))
            {
                m_texts.insert_or_assign(lowered(*key), clean_text(*text));
            }
        }
    }

    void VoiceLineBuilder::add_dialogue(std::string_view xml)
    {
        if (!xml.contains(RESPONSE_MARK))
        {
            return;
        }
        std::unordered_map<std::string_view, std::string_view> cast;
        for (std::size_t at = xml.find(CAST_OPEN); at != std::string_view::npos; at = xml.find(CAST_OPEN, at + 1))
        {
            const std::size_t start = at + CAST_OPEN.size();
            const std::optional<std::string_view> role = quoted(xml, start);
            if (role.has_value() && !role->empty() && xml.substr(start + role->size()).starts_with(CAST_VOICE))
            {
                const std::optional<std::string_view> voice = quoted(xml, start + role->size() + CAST_VOICE.size());
                if (voice.has_value())
                {
                    cast.insert_or_assign(*role, *voice);
                }
            }
        }
        std::size_t at = xml.find(RESPONSE_OPEN);
        while (at != std::string_view::npos)
        {
            const std::size_t start = at + RESPONSE_OPEN.size();
            const std::optional<std::string_view> role = quoted(xml, start);
            const std::size_t close = xml.find('>', start);
            if (!role.has_value() || close == std::string_view::npos)
            {
                return;
            }
            // An empty <Response ... /> has no body: reading on would take the next response's text.
            if (role->empty() || xml[close - 1] == '/')
            {
                at = xml.find(RESPONSE_OPEN, close);
                continue;
            }
            const std::size_t end = xml.find(RESPONSE_CLOSE, close);
            if (end == std::string_view::npos)
            {
                return;
            }
            const std::string_view key = response_key(xml.substr(close + 1, end - close - 1));
            if (!key.empty())
            {
                const auto actor = cast.find(*role);
                const std::string_view voice =
                    actor != cast.end() && actor->second != NO_ACTOR ? actor->second : std::string_view{};
                m_spoken.try_emplace(lowered(key), Spoken{std::string{*role}, std::string{voice}});
            }
            at = xml.find(RESPONSE_OPEN, end);
        }
    }

    void VoiceLineBuilder::add_voice_file(std::string_view name)
    {
        const std::string low_name = lowered(name);
        if (!low_name.starts_with(VOICE_FOLDER) || !low_name.ends_with(VOICE_EXTENSION))
        {
            return;
        }
        const std::string_view relative =
            name.substr(VOICE_FOLDER.size(), name.size() - VOICE_FOLDER.size() - VOICE_EXTENSION.size());
        const std::size_t slash = relative.find_last_of('/');
        const std::string_view stem = slash == std::string_view::npos ? relative : relative.substr(slash + 1);
        const std::string low = lowered(stem);
        const std::size_t underscore = low.find('_');
        const bool whole = m_texts.contains(low);
        const std::string key = whole || underscore == std::string::npos ? low : low.substr(underscore + 1);
        const auto text = m_texts.find(key);
        if (text == m_texts.end() || text->second.empty())
        {
            return;
        }
        File file;
        file.henry = std::ranges::any_of(HENRY_PREFIXES, [&low](std::string_view p) { return low.starts_with(p); });
        file.path = std::string{relative};
        file.code = key != low ? low.substr(0, underscore) : std::string{};
        file.key = key;
        file.text = text->second;
        m_files.push_back(std::move(file));
    }

    std::string VoiceLineBuilder::build() const
    {
        std::unordered_map<std::string, Votes> code_votes;
        std::unordered_map<std::string, Votes> role_votes;
        const Spoken nobody;
        const auto spoken_of = [this, &nobody](const std::string &key) -> const Spoken &
        {
            const auto it = m_spoken.find(key);
            return it != m_spoken.end() ? it->second : nobody;
        };
        for (const File &file : m_files)
        {
            const Spoken &spoken = spoken_of(file.key);
            if (!spoken.actor.empty())
            {
                if (!file.code.empty())
                {
                    vote(code_votes[file.code], spoken.actor);
                }
                vote(role_votes[spoken.role], spoken.actor);
            }
        }

        struct Ranked
        {
            const File *file = nullptr;
            bool has_word = false;
            std::size_t length = 0;
        };
        std::vector<Ranked> ranked;
        ranked.reserve(m_files.size());
        for (const File &file : m_files)
        {
            const TextShape shape = text_shape(file.text);
            ranked.push_back(Ranked{&file, shape.has_word, shape.length});
        }
        std::ranges::stable_sort(
            ranked,
            {},
            [](const Ranked &line)
            { return std::tuple{!line.file->henry, !line.has_word, line.length, line.file->text}; }
        );

        std::string text;
        for (const Ranked &line : ranked)
        {
            const File &file = *line.file;
            const Spoken &spoken = spoken_of(file.key);
            std::string_view actor = winner(code_votes, file.code);
            if (actor.empty())
            {
                actor = !spoken.actor.empty() ? std::string_view{spoken.actor} : winner(role_votes, spoken.role);
            }
            const std::string role = role_name(spoken.role);
            const std::string name = actor_name(actor);
            text += file.path;
            text += FIELD_SEPARATOR;
            text += role;
            if (!role.empty() && !name.empty())
            {
                text += " (" + name + ")";
            }
            else if (role.empty())
            {
                text += name;
            }
            text += FIELD_SEPARATOR;
            text += file.text;
            text += '\n';
        }
        return text;
    }

    std::optional<std::string> build_voice_lines(const VoiceLineSources &sources, const std::function<bool()> &stopped)
    {
        VoiceLineBuilder builder;
        {
            const std::optional<std::vector<ZipEntry>> index = read_zip_index(sources.text_pak);
            if (!index.has_value())
            {
                return std::nullopt;
            }
            const auto entry =
                std::ranges::find_if(*index, [](const ZipEntry &e) { return lowered(e.name) == TEXT_FILE; });
            std::ifstream file{sources.text_pak, std::ios::binary};
            const std::optional<std::string> text = entry != index->end() ? read_zip_file(file, *entry) : std::nullopt;
            if (!text.has_value())
            {
                return std::nullopt;
            }
            builder.add_texts(*text);
        }
        {
            const std::optional<std::vector<ZipEntry>> index = read_zip_index(sources.dialogue_pak);
            if (!index.has_value())
            {
                return std::nullopt;
            }
            std::ifstream file{sources.dialogue_pak, std::ios::binary};
            for (const ZipEntry &entry : *index)
            {
                if (stopped())
                {
                    return std::nullopt;
                }
                const std::string name = lowered(entry.name);
                if (name.starts_with(DIALOGUE_FOLDER) && name.ends_with(DIALOGUE_EXTENSION))
                {
                    if (const std::optional<std::string> text = read_zip_file(file, entry); text.has_value())
                    {
                        builder.add_dialogue(*text);
                    }
                }
            }
        }
        for (const std::filesystem::path &pak : sources.voice_paks)
        {
            if (stopped())
            {
                return std::nullopt;
            }
            const std::optional<std::vector<ZipEntry>> index = read_zip_index(pak);
            if (!index.has_value())
            {
                return std::nullopt;
            }
            for (const ZipEntry &entry : *index)
            {
                builder.add_voice_file(entry.name);
            }
        }
        return builder.build();
    }

    VoiceLines::VoiceLines(std::string text) : m_text(std::move(text))
    {
        m_search.reserve(m_text.size());
        const std::string_view all{m_text};
        for (std::size_t start = 0; start < all.size();)
        {
            const std::size_t end = std::min(all.find('\n', start), all.size());
            const std::string_view line = all.substr(start, end - start);
            const std::size_t tab = line.find(FIELD_SEPARATOR);
            const std::string_view path = line.substr(0, tab);
            m_starts.push_back(m_search.size());
            if (tab != std::string_view::npos)
            {
                m_search += lowered(line.substr(tab + 1));
            }
            m_search += '\n';
            m_lines.push_back(Line{.text = line, .path = path});
            start = end + 1;
        }
    }

    VoiceLinePage VoiceLines::find(std::string_view wanted, std::size_t first, std::size_t count) const
    {
        VoiceLinePage page;
        const auto add = [this, &page](std::size_t index)
        {
            page.lines += m_lines[index].text;
            page.lines += '\n';
        };
        if (wanted.empty())
        {
            page.total = m_lines.size();
            for (std::size_t i = first; i < m_lines.size() && i - first < count; ++i)
            {
                add(i);
            }
            return page;
        }
        const std::string needle = lowered(wanted);
        const std::string_view search{m_search};
        for (std::size_t at = search.find(needle); at != std::string_view::npos;)
        {
            const auto next = std::ranges::upper_bound(m_starts, at);
            const auto index = static_cast<std::size_t>(next - m_starts.begin()) - 1;
            if (page.total >= first && page.total - first < count)
            {
                add(index);
            }
            ++page.total;
            if (next == m_starts.end())
            {
                break;
            }
            at = search.find(needle, *next);
        }
        return page;
    }

    std::optional<std::string_view> VoiceLines::line(std::string_view path) const
    {
        const std::string wanted = lowered(path);
        for (const Line &line : m_lines)
        {
            if (line.path.size() == wanted.size() && std::ranges::equal(line.path, wanted, {}, ascii_lower))
            {
                return line.text;
            }
        }
        return std::nullopt;
    }
} // namespace SoundFramework::packs
