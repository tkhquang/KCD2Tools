/**
 * @file core/mod_maker.hpp
 * @brief Builds the patches of an animation mod's new action: a new fragment that plays one clip, set up like a game
 *        fragment the modder names (its body layers, blends and procedural layers), as the modding guide's "Adding a
 *        new fragment" describes, with no clip index and no whole game file.
 */

#ifndef ANIMFW_CORE_MOD_MAKER_HPP
#define ANIMFW_CORE_MOD_MAKER_HPP

#include "core/xml_elements.hpp"
#include "core/zip_writer.hpp"

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace AnimationFramework
{
    /** @brief One action a mod adds. */
    struct ModAction
    {
        std::string modid;
        /// The new fragment's name (best started with the mod's name).
        std::string fragment;
        /// The clip it plays, by the name the game plays it by (its file name without `.caf`).
        std::string clip;
        /// Whether the clip loops until the fragment is stopped.
        bool loop = false;
        /**
         * @brief Plays on the upper body only, over what the character is doing (played with animfw_own_scopes): the
         *        clip goes through a blend space whose joint list keeps the chest up, as the Gesture Wheel's twins do.
         */
        bool upper = false;
    };

    /** @brief A mod's patch texts for Henry's fragment IDs, controller definitions and database (empty: none yet). */
    struct ModPatches
    {
        std::string fragment_ids;
        std::string controller_defs;
        std::string database;
        /// Further files the actions need, by path in the game's data (an upper-body action's blend space).
        std::vector<ZipSource> files;
    };

    /// The scope an upper-body action takes over.
    inline constexpr std::string_view UPPER_BODY_SCOPE = "UpperBody";
    /// The tag that plays a fragment on its own scopes, over what the character does.
    inline constexpr std::string_view OWN_SCOPES_TAG = "animfw_own_scopes";
    /// The console command that adds an action to a mod made in the DEV folder (mod_maker_console.cpp).
    inline constexpr const char *MAKE_MOD_COMMAND = "animfw_make_mod";
    /// The console command that picks up new and changed clips without a restart (hooks/anim_patch.cpp).
    inline constexpr const char *RELOAD_CLIPS_COMMAND = "animfw_reload_clips";

    /** @brief The name of @p modid's upper-body blend space for @p clip (what its option plays). */
    [[nodiscard]] std::string upper_blend_space_name(std::string_view modid, std::string_view clip);

    /** @brief Where that blend space goes in the game's data (lowercase, forward slashes). */
    [[nodiscard]] std::string upper_blend_space_path(std::string_view modid, std::string_view clip);

    /**
     * @brief A blend space that plays @p clip on Henry's chest, neck, head, arms and fingers only: the clip twice,
     *        with a joint list the engine applies (the joints it leaves out stay with the layers below).
     */
    [[nodiscard]] std::string upper_body_blend_space(std::string_view clip);

    /** @brief The `<FragmentList>` entry (the options) of @p fragment in a database, or null. */
    [[nodiscard]] const XmlElement *find_fragment_options(const XmlElement &database, std::string_view fragment);

    /** @brief The `<FragmentDefs>` entry of @p fragment in a controller definition, or null. */
    [[nodiscard]] const XmlElement *
    find_fragment_definition(const XmlElement &controller_defs, std::string_view fragment);

    /** @brief The files a database lists as its sub-databases (`SubADBs/SubADB File`), lowercase, forward slashes. */
    [[nodiscard]] std::vector<std::string> sub_databases(const XmlElement &database);

    /** @brief Whether a fragment ID list names @p fragment (case-insensitive). */
    [[nodiscard]] bool lists_fragment(const XmlElement &fragment_ids, std::string_view fragment);

    /**
     * @brief The option of @p like_options a new action copies: the first that plays a clip for no tags (else the
     *        first that plays a clip), or null.
     */
    [[nodiscard]] const XmlElement *template_option(const XmlElement &like_options);

    /** @brief The first clip @p option plays, or empty. */
    [[nodiscard]] std::string first_clip(const XmlElement &option);

    /**
     * @brief @p current with @p action added (or replaced, when the mod already has that fragment): its ID, a
     *        definition with @p like_definition's scopes and flags, and options copied from @p like_options (the
     *        template option playing the action's clip alone, and the game's per-scope helper options).
     */
    [[nodiscard]] std::expected<ModPatches, std::string> add_mod_action(
        const ModPatches &current,
        const ModAction &action,
        const XmlElement &like_definition,
        const XmlElement &like_options
    );

    /** @brief A GUID for option @p index of a mod's fragment: the same on every build of the mod. */
    [[nodiscard]] std::string option_guid(std::string_view modid, std::string_view fragment, int index);

    /** @brief A `mod.manifest` for @p modid. */
    [[nodiscard]] std::string mod_manifest(std::string_view modid, std::string_view created_on);
} // namespace AnimationFramework

#endif // ANIMFW_CORE_MOD_MAKER_HPP
