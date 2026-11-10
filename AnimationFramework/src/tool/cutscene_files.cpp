/**
 * @file tool/cutscene_files.cpp
 * @brief The cutscene tools' file helpers (see cutscene_files.hpp).
 */

#include "tool/cutscene_files.hpp"
#include "core/text_util.hpp"
#include "core/zip_reader.hpp"
#include "tool/offline_tools.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <set>
#include <system_error>

namespace fs = std::filesystem;

namespace AnimationFramework
{
    namespace
    {
        constexpr std::string_view MISSION_OBJECTS = "objects_mission0.xml";
        constexpr std::string_view CUTSCENE_TABLE = "libs/tables/ui/cutscene.xml";
        constexpr std::string_view XML_EXTENSION = ".xml";
        /// The table patch a mod starts from: the game's cutscene table with no row.
        constexpr std::string_view EMPTY_TABLE = "<?xml version=\"1.0\" encoding=\"us-ascii\"?>\n"
                                                 "<database name=\"barbora\" "
                                                 "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
                                                 "xsi:noNamespaceSchemaLocation=\"cutscene.xsd\">\n"
                                                 "\t<Cutscenes version=\"1\">\n"
                                                 "\t</Cutscenes>\n"
                                                 "</database>\n";
        constexpr std::string_view TABLE_CLOSE = "</Cutscenes>";
        constexpr std::string_view ROW_OPEN = "<IngameCutscene ";
        constexpr std::string_view ROW_CLOSE = "</IngameCutscene>";
        /// The game's audio controls, and the group its cutscene events sit in: `specific/<cutscene>`.
        constexpr std::string_view AUDIO_CONTROLS_FOLDER = "libs/gameaudio/";
        constexpr std::string_view SPECIFIC_EVENTS = "specific/";
        /// Where a cutscene's FMOD bank lives in Sounds.pak, named after the cutscene.
        constexpr std::string_view SOUND_BANK_FOLDER = "sounds/fmod/build/pc/";

        /**
         * @brief The end of the `<IngameCutscene>` row whose opening tag starts at @p start in @p table: past its `/>`,
         *        or past its `</IngameCutscene>` when it holds children (the `<Events>` of a title card).
         * @return The end, or npos when the text there is not such a row.
         */
        [[nodiscard]] std::size_t row_end(std::string_view table, std::size_t start) noexcept
        {
            if (start == std::string_view::npos || !table.substr(start).starts_with(ROW_OPEN))
            {
                return std::string_view::npos;
            }
            const std::size_t tag_end = table.find('>', start);
            if (tag_end == std::string_view::npos || table[tag_end - 1] == '/')
            {
                return tag_end == std::string_view::npos ? tag_end : tag_end + 1;
            }
            const std::size_t close = table.find(ROW_CLOSE, tag_end);
            return close == std::string_view::npos ? close : close + ROW_CLOSE.size();
        }

        /** @brief The `<Entity ...>` element (start tag to its end tag) that holds @p at, or empty. */
        [[nodiscard]] std::string_view entity_around(std::string_view text, std::size_t at)
        {
            if (at >= text.size())
            {
                return {};
            }
            const std::size_t open = text.rfind("<Entity ", at);
            const std::size_t close = text.find("</Entity>", at);
            if (open == std::string_view::npos || close == std::string_view::npos)
            {
                return {};
            }
            return text.substr(open, close - open);
        }

        /** @brief One file of a pak, by its lowercase path. */
        [[nodiscard]] std::optional<std::string> read_pak_file(const fs::path &pak, std::string_view path)
        {
            const std::optional<std::vector<ZipEntry>> index = read_zip_index(pak);
            if (!index)
            {
                return std::nullopt;
            }
            const auto found = std::ranges::find(*index, path, &ZipEntry::name);
            return found == index->end() ? std::nullopt : read_zip_file(pak, *found);
        }

        /** @brief A 64-bit FNV-1a hash. */
        [[nodiscard]] std::uint64_t fnv1a(std::string_view text) noexcept
        {
            std::uint64_t hash = 0xCBF29CE484222325ULL;
            for (const char c : text)
            {
                hash ^= static_cast<std::uint8_t>(c);
                hash *= 0x100000001B3ULL;
            }
            return hash;
        }

        /**
         * @brief @p table (the mod's cutscene table patch) with @p row in place of the row named @p name, else before
         *        the end of the table.
         * @return The new table text, or why the row does not fit in.
         */
        [[nodiscard]] std::expected<std::string, std::string>
        add_row(std::string table, std::string_view name, std::string_view row)
        {
            const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
            // Where the blanks before `at` start: the start of its line when only blanks precede it there.
            const auto blanks_before = [&](std::size_t at) -> std::size_t
            {
                while (at > 0 && blank(table[at - 1]))
                {
                    --at;
                }
                return at;
            };
            if (const std::size_t at = table.find(std::format(" Name=\"{}\"", name)); at != std::string::npos)
            {
                std::size_t start = table.rfind('<', at);
                std::size_t end = row_end(table, start);
                if (end == std::string::npos)
                {
                    return std::unexpected{std::format("the row {} is not an IngameCutscene row", name)};
                }
                // A row alone on its line takes the whole line with it.
                const std::size_t line_start = blanks_before(start);
                std::size_t line_end = end;
                while (line_end < table.size() && blank(table[line_end]))
                {
                    ++line_end;
                }
                if ((line_start == 0 || table[line_start - 1] == '\n') && line_end < table.size() &&
                    table[line_end] == '\n')
                {
                    start = line_start;
                    end = line_end + 1;
                }
                table.erase(start, end - start);
            }
            const std::size_t close = table.find(TABLE_CLOSE);
            if (close == std::string::npos)
            {
                return std::unexpected{std::format("no {} closes the table", TABLE_CLOSE)};
            }
            const std::string_view newline = table.find("\r\n") != std::string::npos ? "\r\n" : "\n";
            table.insert(blanks_before(close), std::format("\t\t{}{}", row, newline));
            return table;
        }
    } // namespace

    namespace cutscene_files
    {
        std::string attribute_key(std::string_view name)
        {
            return std::format(" {}=\"", name);
        }

        std::string_view tag_around(std::string_view text, std::size_t at)
        {
            if (at >= text.size())
            {
                return {};
            }
            const std::size_t open = text.rfind('<', at);
            const std::size_t close = text.find('>', at);
            if (open == std::string_view::npos || close == std::string_view::npos)
            {
                return {};
            }
            return text.substr(open, close - open + 1);
        }

        std::string replace_all(std::string_view text, std::string_view from, std::string_view to)
        {
            std::string out;
            std::size_t start = 0;
            for (std::size_t at = text.find(from); at != std::string_view::npos; at = text.find(from, start))
            {
                out.append(text.substr(start, at - start));
                out.append(to);
                start = at + from.size();
            }
            out.append(text.substr(start));
            return out;
        }

        std::string_view attribute(std::string_view tag, std::string_view name)
        {
            const std::string key = attribute_key(name);
            const std::size_t at = tag.find(key);
            if (at == std::string_view::npos)
            {
                return {};
            }
            const std::size_t start = at + key.size();
            const std::size_t end = tag.find('"', start);
            return end == std::string_view::npos ? std::string_view{} : tag.substr(start, end - start);
        }

        std::string new_guid(std::string_view name, std::string_view old)
        {
            const std::uint64_t hash = fnv1a(std::format("{}:{}", name, old));
            return std::format(
                "{:08x}-{:04x}-{:04x}",
                static_cast<std::uint32_t>(hash >> 32),
                static_cast<std::uint16_t>(hash >> 16),
                static_cast<std::uint16_t>(hash)
            );
        }

        std::string find_row(const fs::path &level_pak, std::string_view sequence)
        {
            const std::optional<std::string> objects = read_pak_file(level_pak, MISSION_OBJECTS);
            if (!objects)
            {
                return {};
            }
            const std::string_view text{*objects};
            const std::string key = std::format("esSequenceName=\"{}\"", sequence);
            for (std::size_t at = text.find(key); at != std::string_view::npos; at = text.find(key, at + 1))
            {
                const std::string_view data = entity_around(text, at);
                const std::string_view guid = attribute(tag_around(data, 0), "EntityGuid");
                if (guid.empty())
                {
                    continue;
                }
                const std::size_t linked = text.find(std::format("TargetGuid=\"{}\" Name=\"cutsceneData\"", guid));
                const std::string_view row = attribute(entity_around(text, linked), "esCutsceneName");
                if (!row.empty())
                {
                    return std::string{row};
                }
            }
            return {};
        }

        std::optional<std::string> table_row(const fs::path &game_root, std::string_view row, std::string_view name)
        {
            const std::optional<std::string> table = read_pak_file(game_root / "Data" / "Tables.pak", CUTSCENE_TABLE);
            if (!table)
            {
                return std::nullopt;
            }
            const std::string old_name = std::format(" Name=\"{}\"", row);
            const std::size_t at = table->find(old_name);
            // A RenderedCutscene row of the same table plays a video, not a layer.
            const std::size_t start = at == std::string::npos ? at : table->rfind('<', at);
            const std::size_t end = row_end(*table, start);
            if (end == std::string::npos)
            {
                return std::nullopt;
            }
            return replace_all(
                std::string_view{*table}.substr(start, end - start),
                old_name,
                std::format(" Name=\"{}\"", name)
            );
        }

        bool is_scene_name(std::string_view name) noexcept
        {
            const auto name_char = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; };
            return !name.empty() && std::ranges::all_of(name, name_char);
        }

        std::expected<SceneFiles, std::string> write_scene(
            const fs::path &mod,
            std::string_view level,
            std::string_view name,
            std::string_view layer_xml,
            std::string_view row
        )
        {
            const std::string modid = mod_id_of(mod);
            if (!is_mod_id(modid))
            {
                return bad_mod_id(modid);
            }
            const fs::path data = mod / "Data";
            const SceneFiles files{
                .layer = data / "Levels" / level / "layers" / std::format("{}.xml", name),
                .table = data / "Libs" / "Tables" / "ui" / std::format("cutscene__{}.xml", modid),
            };
            // A table patch that exists but does not read is never replaced: it holds the mod's other rows.
            std::optional<std::string> current = read_text(files.table);
            std::error_code error;
            if (!current && fs::exists(files.table, error))
            {
                return std::unexpected{std::format("could not read {}", files.table.string())};
            }
            const auto table = add_row(current.value_or(std::string{EMPTY_TABLE}), name, row);
            if (!table)
            {
                return std::unexpected{std::format("{}: {}", files.table.string(), table.error())};
            }
            if (!write_text(files.layer, layer_xml))
            {
                return std::unexpected{std::format("could not write {}", files.layer.string())};
            }
            if (!write_text(files.table, *table))
            {
                return std::unexpected{std::format(
                    "wrote {} but could not write {}, so the scene has no row yet: close what holds that file and run "
                    "the command again",
                    files.layer.string(),
                    files.table.string()
                )};
            }
            return files;
        }

        std::vector<std::string> sound_banks(const fs::path &game_root, std::string_view layer_xml)
        {
            std::set<std::string> triggers;
            const std::string trigger_key = attribute_key("StartTrigger");
            for (std::size_t at = layer_xml.find(trigger_key); at != std::string_view::npos;
                 at = layer_xml.find(trigger_key, at + trigger_key.size()))
            {
                const std::size_t start = at + trigger_key.size();
                const std::size_t end = layer_xml.find('"', start);
                if (end != std::string_view::npos && end > start)
                {
                    triggers.emplace(layer_xml.substr(start, end - start));
                }
            }
            const fs::path controls = game_root / "Data" / "IPL_GameData.pak";
            const std::optional<std::vector<ZipEntry>> control_index =
                triggers.empty() ? std::nullopt : read_zip_index(controls);
            const std::optional<std::vector<ZipEntry>> sound_index =
                control_index ? read_zip_index(game_root / "Data" / "Sounds.pak") : std::nullopt;
            if (!control_index || !sound_index)
            {
                return {};
            }
            std::set<std::string> banks;
            for (const ZipEntry &entry : *control_index)
            {
                if (!entry.name.starts_with(AUDIO_CONTROLS_FOLDER) || !entry.name.ends_with(XML_EXTENSION))
                {
                    continue;
                }
                const std::optional<std::string> text = read_zip_file(controls, entry);
                if (!text)
                {
                    continue;
                }
                for (const std::string &trigger : triggers)
                {
                    const std::size_t at = text->find(std::format("atl_name=\"{}\"", trigger));
                    const std::string_view path = attribute(tag_around(*text, at), "path");
                    if (!path.starts_with(SPECIFIC_EVENTS))
                    {
                        continue;
                    }
                    const std::string bank = lowercase(path.substr(SPECIFIC_EVENTS.size()));
                    const std::string bank_file = std::format("{}{}.bank", SOUND_BANK_FOLDER, bank);
                    if (std::ranges::find(*sound_index, bank_file, &ZipEntry::name) != sound_index->end())
                    {
                        banks.insert(bank);
                    }
                }
            }
            return {banks.begin(), banks.end()};
        }

        std::string register_line(std::string_view name, const std::vector<std::string> &banks)
        {
            std::string sound_bank;
            if (banks.size() == 1)
            {
                sound_bank = std::format(", soundBank = \"{}\"", banks.front());
            }
            else if (banks.size() > 1)
            {
                std::string list;
                for (const std::string &bank : banks)
                {
                    list += std::format("{}\"{}\"", list.empty() ? "" : ", ", bank);
                }
                sound_bank = std::format(", soundBank = {{ {} }}", list);
            }
            return std::format(
                "AnimationFramework.Cutscene.Register({{ name = \"{0}\", row = \"{0}\", layer = \"{0}\", sequence = "
                "\"{0}\"{1} }})",
                name,
                sound_bank
            );
        }
    } // namespace cutscene_files
} // namespace AnimationFramework
