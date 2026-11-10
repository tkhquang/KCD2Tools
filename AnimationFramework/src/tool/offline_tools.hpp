/**
 * @file tool/offline_tools.hpp
 * @brief The tools that run without the game, shared by the command line, its MCP server and the Studio window: the
 *        game's folder, the game as read from its paks (with a mod folder's own clips first), and the steps on a mod's
 *        source folder (make an action, pack the mod, list its actions).
 */

#ifndef ANIMFW_TOOL_OFFLINE_TOOLS_HPP
#define ANIMFW_TOOL_OFFLINE_TOOLS_HPP

#include "core/clip_compiler.hpp"
#include "core/anim_catalog.hpp"
#include "core/game_paks.hpp"
#include "core/mod_tools.hpp"
#include "retarget/retarget.hpp"

#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AnimationFramework
{
    /** @brief The Steam libraries' copy of the game (Steam's own folder, then each library it lists), or nothing. */
    [[nodiscard]] std::optional<std::filesystem::path> find_steam_game();

    /** @brief The Steam libraries' copy of the game's Sandbox editor (the KCD2Mod tools folder), or nothing. */
    [[nodiscard]] std::optional<std::filesystem::path> find_steam_sandbox();

    /** @brief The bytes of @p file, or nothing when it does not open. */
    [[nodiscard]] std::optional<std::string> read_text(const std::filesystem::path &file);

    /**
     * @brief Writes @p text to @p file, creating its folders. The text goes to `<file>.tmp` first and replaces @p file
     *        by a rename, so a failed write leaves the old file whole.
     * @return Whether @p file now holds @p text.
     */
    [[nodiscard]] bool write_text(const std::filesystem::path &file, std::string_view text);

    /** @brief A clip from a `.caf` file on disk; its game path is the part from its `Animations` folder on, if any. */
    [[nodiscard]] std::expected<FoundClip, std::string> read_clip_file(const std::filesystem::path &file);

    /** @brief Whether @p text names an existing `.caf` file. */
    [[nodiscard]] bool is_clip_file(std::string_view text);

    /** @brief The mod id of the source folder @p mod: its manifest's, else the folder's name. */
    [[nodiscard]] std::string mod_id_of(const std::filesystem::path &mod);

    /** @brief The error for a mod id that is_mod_id refuses: the game skips such a mod. */
    [[nodiscard]] std::unexpected<std::string> bad_mod_id(std::string_view modid);

    /** @brief The root motion named @p name ("path", "line" or "in-place", ignoring case), or nothing. */
    [[nodiscard]] std::optional<RootMotion> root_motion_named(std::string_view name);

    /** @brief The game from its paks, a mod folder's clips searched first, and Henry's skeleton read once. */
    class OfflineGame
    {
    public:
        /// Reads the game from its opened @p paks.
        explicit OfflineGame(GamePaks paks);

        /// The game's paks.
        [[nodiscard]] const GamePaks &paks() const noexcept { return m_paks; }

        /** @brief The game as the clip check and the mod maker read it; @p mod_folder's clips come first. */
        [[nodiscard]] ModToolGame tools(std::optional<std::filesystem::path> mod_folder = std::nullopt);

        /** @brief A clip by `.caf` file, by a clip of @p mod_folder, or by a game clip's name or path. */
        [[nodiscard]] std::expected<FoundClip, std::string>
        find_clip(std::string_view name, const std::optional<std::filesystem::path> &mod_folder = std::nullopt);

        /** @brief The skeleton clips are made for (Henry's unless use_female_skeleton), or null when unreadable. */
        [[nodiscard]] const std::vector<SkeletonBone> *skeleton();

        /** @brief Makes clips (retarget, compile, check) for the women's skeleton when @p female, else Henry's. */
        void use_female_skeleton(bool female) noexcept;

        /** @brief The game's fragments whose catalog line holds @p text (ignoring case), at most @p limit. */
        [[nodiscard]] std::vector<std::string> find_fragments(std::string_view text, std::size_t limit);

        /** @brief The game's clips whose path holds @p text (ignoring case), at most @p limit. */
        [[nodiscard]] std::vector<std::string> find_clips(std::string_view text, std::size_t limit) const;

    private:
        GamePaks m_paks;
        std::optional<std::vector<SkeletonBone>> m_skeleton;
        bool m_skeleton_read = false;
        bool m_female = false;
        std::unique_ptr<AnimCatalog> m_catalog;
    };

    /** @brief An action to add to a mod's source folder. */
    struct MakeRequest
    {
        std::filesystem::path mod_folder;
        std::string fragment;
        /// A `.caf` file (copied into the mod) or the name of a clip the mod or the game has.
        std::string clip;
        /// The game fragment the action is set up like.
        std::string like;
        bool loop = false;
        /// Plays on the upper body only (see ModAction::upper).
        bool upper = false;
    };

    /**
     * @brief Adds an action to the mod in @p request's folder: copies its clip in, checks it (what it warns about goes
     *        to @p print), writes the patches and a manifest.
     * @return A one-line summary, or the reason the action was not made.
     */
    [[nodiscard]] std::expected<std::string, std::string>
    make_action_in_folder(OfflineGame &game, const MakeRequest &request, const ToolPrint &print);

    /**
     * @brief Writes the mod of source folder @p mod as installed into @p install, as kcd-pak-action packs a mod:
     *        `mod.manifest`, `Data/<modid>.pak` with everything under the source's `Data/` but `Levels/` and paks,
     *        `Data/Levels/<level>/<modid>.pak` with each level folder's files, and `Localization/<folder>.pak` with
     *        each folder of the source's `Localization/`.
     * @details A `Data/<modid>.pak` or level pak that an earlier pack wrote and this one does not is removed.
     * @return A one-line summary, or the reason the mod was not packed. Nothing is written when a pak cannot be built.
     */
    [[nodiscard]] std::expected<std::string, std::string>
    pack_mod_folder(const std::filesystem::path &mod, const std::filesystem::path &install);

    /**
     * @brief Compiles the intermediate animation @p source (`.i_caf`, as Blender and other exporters write it) into the
     *        game-ready clip @p target for Henry's skeleton, without the Resource Compiler, and checks the result.
     * @note A @p target that is not a `.caf` file is refused, so the overwrite never replaces a file of another kind.
     * @return 0 when written with no warning, 2 when written with warnings, 1 when not written.
     */
    [[nodiscard]] int compile_clip_file(
        OfflineGame &game,
        const std::filesystem::path &source,
        const std::filesystem::path &target,
        const CompileOptions &options,
        const ToolPrint &print
    );

    /**
     * @brief Compiles @p sampled into @p target (@p source names it in the output), prints the result and checks it.
     * @note A @p target that is not a `.caf` file is refused.
     * @return 0 when written with no warning, 2 when written with warnings or an unreadable check, 1 when not written.
     */
    [[nodiscard]] int compile_sampled_clip(
        OfflineGame &game,
        const SampledClip &sampled,
        const std::filesystem::path &source,
        const std::filesystem::path &target,
        const CompileOptions &options,
        const ToolPrint &print
    );

    /** @brief A motion capture or DCC animation to retarget onto Henry's skeleton and compile. */
    struct RetargetRequest
    {
        /// A `.bvh` or `.fbx` animation.
        std::filesystem::path source;
        std::filesystem::path target;
        /// The take to read from a file that holds several (empty: the first).
        std::string take;
        RetargetOptions options;
        CompileOptions compile;
        /// A game clip whose first frame poses the fingers when the source has none (empty: the default pose).
        std::string hands = "relaxed_idle_both_player";
        /// Retarget onto the women's skeleton (a female NPC or double) instead of Henry's.
        bool female = false;
    };

    /**
     * @brief Retargets a motion capture or DCC animation onto Henry's skeleton, compiles it into @p request.target and
     *        checks it. Prints the bone mapping and how the feet came out.
     * @note A target that is not a `.caf` file is refused before the source is read.
     * @return 0 when written with no warning, 2 when written with warnings, 1 when not written.
     */
    [[nodiscard]] int retarget_clip_file(OfflineGame &game, const RetargetRequest &request, const ToolPrint &print);

    /** @brief One action of a mod: its fragment and the clip its first option plays. */
    struct ModActionInfo
    {
        std::string fragment;
        std::string clip;
        bool loop = false;
    };

    /** @brief The actions the mod in @p mod has made so far. */
    [[nodiscard]] std::vector<ModActionInfo> list_mod_actions(const std::filesystem::path &mod);
} // namespace AnimationFramework

#endif // ANIMFW_TOOL_OFFLINE_TOOLS_HPP
