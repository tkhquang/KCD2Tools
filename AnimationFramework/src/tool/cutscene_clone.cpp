/**
 * @file tool/cutscene_clone.cpp
 * @brief The vanilla cutscene copy (see cutscene_clone.hpp).
 */

#include "tool/cutscene_clone.hpp"
#include "core/text_util.hpp"
#include "core/zip_reader.hpp"
#include "tool/cutscene_files.hpp"
#include "tool/offline_tools.hpp"

#include <algorithm>
#include <format>
#include <optional>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace AnimationFramework
{
    namespace
    {
        using cutscene_files::attribute;
        using cutscene_files::attribute_key;
        using cutscene_files::tag_around;

        constexpr std::string_view LAYER_FOLDER = "layers/";
        constexpr std::string_view XML_EXTENSION = ".xml";
        /// The prefix of the game's cinematic layers, searched for a sequence whose layer is named otherwise.
        constexpr std::string_view CINEMATIC_PREFIX = "cin_";
        constexpr std::string_view SEQUENCE_TAG = "<Sequence ";

        /** @brief Whether @p text is a GUID as layer file names end with: 8-4-4-4-12 hexadecimal digits. */
        [[nodiscard]] bool is_guid(std::string_view text) noexcept
        {
            constexpr std::size_t guid_length = 36;
            if (text.size() != guid_length)
            {
                return false;
            }
            for (std::size_t i = 0; i < text.size(); ++i)
            {
                const char c = text[i];
                const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
                const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
                if (dash ? c != '-' : !hex)
                {
                    return false;
                }
            }
            return true;
        }

        /** @brief The offset of the `<Sequence ...>` tag named @p name (ignoring case) in @p text, or npos. */
        [[nodiscard]] std::size_t find_sequence(std::string_view text, std::string_view name)
        {
            const std::string wanted = lowercase(name);
            for (std::size_t at = text.find(SEQUENCE_TAG); at != std::string_view::npos;
                 at = text.find(SEQUENCE_TAG, at + 1))
            {
                if (lowercase(attribute(tag_around(text, at), "Name")) == wanted)
                {
                    return at;
                }
            }
            return std::string_view::npos;
        }

        /** @brief Every value of the attribute @p name in @p text. */
        [[nodiscard]] std::unordered_set<std::string> values_of(std::string_view text, std::string_view name)
        {
            const std::string key = attribute_key(name);
            std::unordered_set<std::string> values;
            for (std::size_t at = text.find(key); at != std::string_view::npos; at = text.find(key, at + 1))
            {
                const std::size_t start = at + key.size();
                const std::size_t end = text.find('"', start);
                if (end != std::string_view::npos)
                {
                    values.emplace(text.substr(start, end - start));
                }
            }
            return values;
        }

        /** @brief @p text with every value of attribute @p name that @p map holds replaced by its new value. */
        [[nodiscard]] std::string remap_values(
            std::string_view text,
            std::string_view name,
            const std::unordered_map<std::string, std::string> &map
        )
        {
            const std::string key = attribute_key(name);
            std::string out;
            out.reserve(text.size());
            std::size_t from = 0;
            for (std::size_t at = text.find(key); at != std::string_view::npos; at = text.find(key, at + 1))
            {
                const std::size_t start = at + key.size();
                const std::size_t end = text.find('"', start);
                if (end == std::string_view::npos)
                {
                    break;
                }
                const auto found = map.find(std::string{text.substr(start, end - start)});
                if (found == map.end())
                {
                    continue;
                }
                out.append(text.substr(from, start - from));
                out.append(found->second);
                from = end;
            }
            out.append(text.substr(from));
            return out;
        }

        /** @brief The vanilla cutscene's layer: its level, the level's pak, its text and where its sequence starts. */
        struct FoundLayer
        {
            std::string level;
            fs::path pak;
            std::string text;
            std::size_t sequence_at = 0;
        };

        /**
         * @brief The level layer that holds the sequence @p sequence: the one named after it (`layers/<sequence>.xml`
         *        or `layers/<sequence>_<guid>.xml`), else the first cinematic layer (`layers/cin_*`) that holds it.
         * @details A layer can hold more than one sequence, and a few cinematic layers keep an older name than their
         *          sequence, so the layer must hold a sequence of that name, whatever its own name.
         */
        [[nodiscard]] std::optional<FoundLayer> find_layer(const fs::path &game_root, std::string_view sequence)
        {
            const std::string wanted = lowercase(sequence);
            const std::string guid_prefix = wanted + '_';
            const auto holds = [&](const fs::path &pak, const ZipEntry &entry) -> std::optional<FoundLayer>
            {
                std::optional<std::string> text = read_zip_file(pak, entry);
                const std::size_t at = text ? find_sequence(*text, sequence) : std::string_view::npos;
                if (at == std::string_view::npos)
                {
                    return std::nullopt;
                }
                return FoundLayer{
                    .level = pak.parent_path().filename().string(),
                    .pak = pak,
                    .text = std::move(*text),
                    .sequence_at = at,
                };
            };
            std::vector<std::pair<fs::path, ZipEntry>> cinematics;
            std::error_code error;
            for (const fs::directory_entry &level : fs::directory_iterator(game_root / "Data" / "Levels", error))
            {
                const fs::path pak = level.path() / "level.pak";
                const std::optional<std::vector<ZipEntry>> index = read_zip_index(pak);
                if (!index)
                {
                    continue;
                }
                for (const ZipEntry &entry : *index)
                {
                    const std::string_view name{entry.name};
                    if (!name.starts_with(LAYER_FOLDER) || !name.ends_with(XML_EXTENSION))
                    {
                        continue;
                    }
                    const std::string_view stem =
                        name.substr(LAYER_FOLDER.size(), name.size() - LAYER_FOLDER.size() - XML_EXTENSION.size());
                    const bool named =
                        stem == wanted || (stem.starts_with(guid_prefix) && is_guid(stem.substr(guid_prefix.size())));
                    if (named)
                    {
                        if (std::optional<FoundLayer> found = holds(pak, entry))
                        {
                            return found;
                        }
                    }
                    else if (stem.starts_with(CINEMATIC_PREFIX))
                    {
                        cinematics.emplace_back(pak, entry);
                    }
                }
            }
            for (const auto &[pak, entry] : cinematics)
            {
                if (std::optional<FoundLayer> found = holds(pak, entry))
                {
                    return found;
                }
            }
            return std::nullopt;
        }
    } // namespace

    std::expected<std::string, std::string> clone_cutscene(const CutsceneCloneRequest &request, const ToolPrint &print)
    {
        using namespace cutscene_files;
        const std::string &name = request.new_name;
        if (!is_scene_name(name))
        {
            return std::unexpected{std::format("{}: use lowercase letters, digits and underscores", name)};
        }
        const std::optional<FoundLayer> layer = find_layer(request.game_root, request.sequence);
        if (!layer)
        {
            return std::unexpected{std::format("no level layer holds the sequence {}", request.sequence)};
        }
        const std::string_view text{layer->text};
        const std::string_view sequence_tag = tag_around(text, layer->sequence_at);
        const std::string sequence{attribute(sequence_tag, "Name")};
        const std::size_t layer_at = text.find(attribute_key("Layer"));
        const std::string old_layer{attribute(tag_around(text, layer_at), "Layer")};
        if (old_layer.empty())
        {
            return std::unexpected{std::format("the layer of {} names no layer on its entities", sequence)};
        }

        const std::string row_name = request.row.empty() ? find_row(layer->pak, sequence) : request.row;
        if (row_name.empty())
        {
            return std::unexpected{std::format(
                "no CutsceneHolder of {} names a row for {}; pass the row with --row <name>",
                layer->level,
                sequence
            )};
        }
        const std::optional<std::string> row = table_row(request.game_root, row_name, name);
        if (!row)
        {
            return std::unexpected{std::format(
                "the cutscene table (Data/Tables.pak) has no in-game cutscene row {}; pass one with --row <name>",
                row_name
            )};
        }

        // Only the chosen sequence takes the new name: the layer can hold others (a light animation set), and keys
        // that name an audio trigger after the sequence keep it.
        std::string copy{text};
        const std::string old_name = std::format(" Name=\"{}\"", sequence);
        const std::string new_name = std::format(" Name=\"{}\"", name);
        copy.replace(layer->sequence_at + sequence_tag.find(old_name), old_name.size(), new_name);
        // New GUIDs for the layer's own entities. References to anything else (a pillory, a door) stay.
        const std::unordered_set<std::string> own = values_of(text, GUID_ATTRIBUTES[0]);
        std::unordered_map<std::string, std::string> guids;
        for (const std::string &guid : own)
        {
            guids.emplace(guid, new_guid(name, guid));
        }
        const auto world = std::ranges::count_if(
            values_of(text, GUID_ATTRIBUTES[1]),
            [&](const std::string &reference) { return !own.contains(reference); }
        );
        for (const std::string_view attribute_name : GUID_ATTRIBUTES)
        {
            copy = remap_values(copy, attribute_name, guids);
        }
        copy = replace_all(copy, std::format(" Layer=\"{}\"", old_layer), std::format(" Layer=\"{}\"", name));
        const std::string sequence_object = std::format("<Entity Name=\"{}\"", name);
        copy = replace_all(copy, std::format("<Entity Name=\"{}\"", sequence), sequence_object);
        if (copy.find(sequence_object) == std::string::npos)
        {
            return std::unexpected{std::format("the layer of {} has no sequence object named after it", sequence)};
        }

        std::string moved_note;
        if (request.at)
        {
            const std::optional<ScenePoint> origin = scene_origin(copy, name);
            if (!origin)
            {
                return std::unexpected{
                    std::format("{} animates no character, so --at has no point to place", sequence)
                };
            }
            const ScenePoint &at = *request.at;
            MovedScene moved = move_scene(copy, ScenePoint{at.x - origin->x, at.y - origin->y, at.z - origin->z});
            copy = std::move(moved.layer_xml);
            moved_note = std::format(
                "  moved from {:.2f},{:.2f},{:.2f} to {:.2f},{:.2f},{:.2f}: {} entities, {} position keys",
                origin->x,
                origin->y,
                origin->z,
                at.x,
                at.y,
                at.z,
                moved.entities,
                moved.keys
            );
        }

        const auto files = write_scene(request.mod_folder, layer->level, name, copy, *row);
        if (!files)
        {
            return std::unexpected{files.error()};
        }
        print(std::format("  layer {} ({}) -> {}", old_layer, layer->level, files->layer.string()));
        print(std::format("  {} entities with new GUIDs, {} world reference(s) kept", own.size(), world));
        if (!moved_note.empty())
        {
            print(moved_note);
        }
        print(std::format("  row {} -> {} in {}", row_name, name, files->table.string()));
        return register_line(name, sound_banks(request.game_root, copy));
    }
} // namespace AnimationFramework
