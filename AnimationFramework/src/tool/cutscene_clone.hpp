/**
 * @file tool/cutscene_clone.hpp
 * @brief Copies one of the game's in-game cutscenes into a mod under a new name, to play or change it with
 *        AnimationFramework.Cutscene.
 *
 * A vanilla in-game cutscene is a level layer `layers/<sequence>_<guid>.xml` (its cast and a SequenceObject holding the
 * timeline) and a row of `Libs/Tables/ui/cutscene.xml`, which a CutsceneHolder in the level's mission objects names
 * next to an IngameCutsceneData that names the sequence. The copy is a new layer file and a new row, so the original
 * stays as it is:
 *  - Every entity of the layer gets a new GUID (stable for the new name), and the sequence nodes and entity links that
 *    point at them follow. Nodes that point at world entities (a pillory, a door) keep theirs and drive the same ones.
 *  - The layer, the sequence and its SequenceObject take the new name. Other sequences of the layer keep theirs.
 *  - The row is the original row under the new name.
 *  - With a place to play at, the copy moves there (see core/scene_move.hpp): the point its characters are authored
 *    around lands on that place, and the scene keeps its heading.
 */
#ifndef ANIMFW_TOOL_CUTSCENE_CLONE_HPP
#define ANIMFW_TOOL_CUTSCENE_CLONE_HPP

#include "core/mod_tools.hpp"
#include "core/scene_move.hpp"

#include <expected>
#include <filesystem>
#include <optional>
#include <string>

namespace AnimationFramework
{
    /** @brief What to copy and where. */
    struct CutsceneCloneRequest
    {
        /// The game folder (holding Data/Levels and Data/Tables.pak).
        std::filesystem::path game_root;
        /// The mod's source folder: the copy goes to its Data folder.
        std::filesystem::path mod_folder;
        /// The vanilla sequence name (ignoring case), such as cin_s9921k_crime__branding_kutnahora.
        std::string sequence;
        /// The copy's name: its layer, sequence and row.
        std::string new_name;
        /// The vanilla row, or empty to take the one the level's CutsceneHolder names.
        std::string row;
        /// Where the copy plays, or nothing to play it where the original does.
        std::optional<ScenePoint> at;
    };

    /**
     * @brief Writes the copy into the mod folder, reporting what it found to @p print.
     * @return The AnimationFramework.Cutscene.Register line for the copy, or the reason nothing was written.
     */
    [[nodiscard]] std::expected<std::string, std::string>
    clone_cutscene(const CutsceneCloneRequest &request, const ToolPrint &print);
} // namespace AnimationFramework

#endif // ANIMFW_TOOL_CUTSCENE_CLONE_HPP
