/**
 * @file tool/cutscene_files.hpp
 * @brief The game files the cutscene tools read and the mod files they write: the level's cutscene holders, the
 *        cutscene table, a scene's layer and its table row.
 */
#ifndef ANIMFW_TOOL_CUTSCENE_FILES_HPP
#define ANIMFW_TOOL_CUTSCENE_FILES_HPP

#include <array>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AnimationFramework::cutscene_files
{
    /// The attributes of a runtime layer that hold entity GUIDs: entities, sequence nodes, entity links.
    inline constexpr std::array<std::string_view, 3> GUID_ATTRIBUTES{
        "EntityGuid",
        "EntityGUID",
        "TargetGuid",
    };

    /** @brief The text that opens the value of attribute @p name in a start tag: ` name="`. */
    [[nodiscard]] std::string attribute_key(std::string_view name);

    /** @brief The value of the first attribute @p name in the start tag @p tag (` name="..."`), or empty. */
    [[nodiscard]] std::string_view attribute(std::string_view tag, std::string_view name);

    /** @brief The `<...>` tag around offset @p at of @p text, or empty. */
    [[nodiscard]] std::string_view tag_around(std::string_view text, std::size_t at);

    /** @brief @p text with every @p from (not empty) replaced by @p to. */
    [[nodiscard]] std::string replace_all(std::string_view text, std::string_view from, std::string_view to);

    /** @brief A new entity GUID in the game's form (%08x-%04x-%04x), the same for the same name and old GUID. */
    [[nodiscard]] std::string new_guid(std::string_view name, std::string_view old);

    /**
     * @brief The row the level's CutsceneHolder names for @p sequence (the holder links, as cutsceneData, to the
     *        IngameCutsceneData whose esSequenceName is the sequence), or empty.
     */
    [[nodiscard]] std::string find_row(const std::filesystem::path &level_pak, std::string_view sequence);

    /**
     * @brief The `<IngameCutscene>` row @p row of the game's cutscene table renamed to @p name, with its `<Events>`
     *        (title cards) when it has them, or nothing.
     */
    [[nodiscard]] std::optional<std::string>
    table_row(const std::filesystem::path &game_root, std::string_view row, std::string_view name);

    /** @brief Whether @p name fits a layer, sequence and row name: lowercase letters, digits and underscores. */
    [[nodiscard]] bool is_scene_name(std::string_view name) noexcept;

    /** @brief The two files a scene takes in a mod's source folder. */
    struct SceneFiles
    {
        /// `Data/Levels/<level>/layers/<name>.xml`.
        std::filesystem::path layer;
        /// `Data/Libs/Tables/ui/cutscene__<modid>.xml`, the mod's patch of the cutscene table.
        std::filesystem::path table;
    };

    /**
     * @brief Writes scene @p name into the mod source folder @p mod: the runtime layer @p layer_xml of level @p level,
     *        and @p row into the mod's cutscene table patch in place of any row named @p name (other rows stay).
     * @return The files written, or the reason the scene was not written.
     */
    [[nodiscard]] std::expected<SceneFiles, std::string> write_scene(
        const std::filesystem::path &mod,
        std::string_view level,
        std::string_view name,
        std::string_view layer_xml,
        std::string_view row
    );

    /**
     * @brief The game's cutscene sound banks the Sound keys of @p layer_xml start events from.
     * @details The game files each cutscene's events under `specific/<cutscene>` in its audio controls
     *          (the XML files of `Libs/GameAudio/`) and loads that cutscene's bank (`Sounds/fmod/Build/pc/<cutscene>.bank`) only
     *          when a cutscene of that name starts. A renamed copy or a new scene therefore names these banks for
     *          AnimationFramework.Cutscene to load while it plays. Only banks the game ships are listed.
     */
    [[nodiscard]] std::vector<std::string>
    sound_banks(const std::filesystem::path &game_root, std::string_view layer_xml);

    /**
     * @brief The Lua line that registers scene @p name (layer, sequence and row all named @p name), with the sound
     *        banks @p banks when there are any.
     */
    [[nodiscard]] std::string register_line(std::string_view name, const std::vector<std::string> &banks);
} // namespace AnimationFramework::cutscene_files

#endif // ANIMFW_TOOL_CUTSCENE_FILES_HPP
