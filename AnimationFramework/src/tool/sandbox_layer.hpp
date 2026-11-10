/**
 * @file tool/sandbox_layer.hpp
 * @brief Turns a layer saved in the game's Sandbox editor (a `.lyr` file) into a scene AnimationFramework.Cutscene
 *        plays, without exporting the level.
 *
 * Sandbox keeps a layer as editor objects. The level export writes the game's runtime layer (`layers/<name>.xml`) from
 * them. The conversion does what that export does for the objects a scene uses, following the game's own cinematic
 * layers: entity objects become `<Entity>` elements (editor-only attributes and children dropped, groups baked into
 * world space, prefab instances expanded from the prefab library, areas, cameras and flocks given the runtime parts the
 * export adds), and the scene's sequence gets the export's defaults and its CleanupAreas (the SequenceAreas the
 * sequence object links as "mizeni": vanilla NPCs inside are hidden while the scene plays).
 *
 * Entities are numbered 1..N: AnimationFramework moves them (and every reference to them) to free entity ids each time
 * the layer streams in. The layer, the scene's sequence (the one named after the `.lyr` file, else the first with the
 * Cut Scene flag) and its sequence object take the scene's name, and the entities new GUIDs (stable for the name), so
 * a scene made from a vanilla cinematic does not clash with the original.
 */
#ifndef ANIMFW_TOOL_SANDBOX_LAYER_HPP
#define ANIMFW_TOOL_SANDBOX_LAYER_HPP

#include "core/mod_tools.hpp"

#include <expected>
#include <filesystem>
#include <string>

namespace AnimationFramework
{
    /** @brief What to convert and where. */
    struct SandboxLayerRequest
    {
        /// The game folder (holding Data/IPL_GameData.pak, Data/Levels and Data/Tables.pak).
        std::filesystem::path game_root;
        /// The mod's source folder: the scene goes to its Data folder.
        std::filesystem::path mod_folder;
        /// The `.lyr` file, inside `<level>/Layers/` of the Sandbox's level folder.
        std::filesystem::path layer_file;
        /// The scene's name (layer, sequence and row), or empty for the layer file's name.
        std::string name;
        /**
         * @brief A row of the game's cutscene table to copy, or empty for the row the level's CutsceneHolder names
         *        for a vanilla cinematic, else a plain row.
         */
        std::string row;
    };

    /**
     * @brief Writes the scene into the mod folder, reporting what it found to @p print.
     * @return The AnimationFramework.Cutscene.Register line for the scene, or the reason nothing was written.
     */
    [[nodiscard]] std::expected<std::string, std::string>
    convert_sandbox_layer(const SandboxLayerRequest &request, const ToolPrint &print);
} // namespace AnimationFramework

#endif // ANIMFW_TOOL_SANDBOX_LAYER_HPP
