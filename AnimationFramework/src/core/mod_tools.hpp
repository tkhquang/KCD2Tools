/**
 * @file core/mod_tools.hpp
 * @brief What the clip check and the mod maker do, apart from where the game's files come from: the framework's
 *        console commands read them through the running game, the offline tool (animfw_tool) from the game's paks.
 */

#ifndef ANIMFW_CORE_MOD_TOOLS_HPP
#define ANIMFW_CORE_MOD_TOOLS_HPP

#include "core/anim_catalog.hpp"
#include "core/caf_reader.hpp"
#include "core/mod_maker.hpp"

#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace AnimationFramework
{
    /// Henry's skeleton.
    inline constexpr std::string_view MALE_SKELETON_PATH = "objects/characters/humans/male/skeleton/male.chr";
    /// The women's skeleton (its clips load from `Animations/humans/female/`).
    inline constexpr std::string_view FEMALE_SKELETON_PATH = "objects/characters/humans/female/skeleton/female.chr";
    /// The patch file names of a mod's actions, `<stem>__<modid>.<extension>` beside these game files.
    inline constexpr std::string_view FRAGMENT_IDS_STEM = "kcd_male_fragmentids";
    inline constexpr std::string_view CONTROLLER_DEFS_STEM = "kcd_male_controllerdefs";
    inline constexpr std::string_view DATABASE_STEM = "kcd_male_database";

    /** @brief A clip read down to its keys, and where it came from. */
    struct FoundClip
    {
        CafClip clip;
        /// Its path in the game's data, lowercase.
        std::string path;
        /// The pak, pack (`.dba`) or file it was read from.
        std::string source;
    };

    /**
     * @brief How the error of a find_clip that no source answers starts, followed by the name asked for. The mod
     *        maker tests for it to tell the modder where a new clip goes.
     */
    inline constexpr std::string_view NO_CLIP_ERROR_PREFIX = "no clip named ";

    /** @brief The game as the tools read it. */
    struct ModToolGame
    {
        /// Its files and their patches.
        AnimCatalogFiles files;
        /**
         * @brief A clip by its name (file name without `.caf`) or path. The error starts with NO_CLIP_ERROR_PREFIX
         *        when no source holds the clip.
         */
        std::function<std::expected<FoundClip, std::string>(std::string_view)> find_clip;
        /// Henry's skeleton, or null when it cannot be read.
        std::function<const std::vector<SkeletonBone> *()> skeleton;
    };

    /// Where the tools print their output, one line per call.
    using ToolPrint = std::function<void(std::string_view)>;

    /// One clip check: the clip by name or path, an optional reference clip, and whether the clip loops.
    struct ClipCheckRequest
    {
        std::string clip;
        /// A clip whose bones a full-body clip in its place must cover too, or empty for none.
        std::string reference;
        bool loops = false;
    };

    /**
     * @brief Checks a clip on Henry's skeleton and prints, one line each, what breaks it in game.
     * @return The warnings printed, or -1 when the clip, the reference or the skeleton cannot be read.
     */
    int report_clip_check(const ModToolGame &game, const ClipCheckRequest &request, const ToolPrint &print);

    /** @brief A new action made: the mod's patches with it, and the game fragment it copies. */
    struct MadeAction
    {
        ModPatches patches;
        /// The body layers the copied fragment takes over.
        std::string scopes;
    };

    /**
     * @brief Adds @p action to @p current, set up like the game fragment @p like, after printing what its clip
     *        check warns about.
     * @return The patches, or why the action cannot be made (an unknown clip, a game fragment's name, ...).
     */
    [[nodiscard]] std::expected<MadeAction, std::string> make_mod_action(
        const ModToolGame &game,
        const ModAction &action,
        std::string_view like,
        const ModPatches &current,
        const ToolPrint &print
    );
} // namespace AnimationFramework

#endif // ANIMFW_CORE_MOD_TOOLS_HPP
