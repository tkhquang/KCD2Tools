/**
 * @file core/legacy_merge.hpp
 * @brief Turns a mod's whole-file .adb, controller definition, tag definition or animation event list override into
 *        the delta the animation patch merge applies, so mods that each ship a full copy of the same file merge instead
 *        of the last one winning.
 *
 * A fragment option set (keyed by its Tags and FragTags as tag sets, inside a fragment matched by name
 * case-insensitively) the mod adds or changes is carried whole, and one the mod dropped becomes Mode="remove". Blend
 * variants (keyed by their from/to tag sets inside their Blend from/to, case-insensitive, as the merge matches them)
 * are replaced whole or removed. SubADBs the mod adds are carried. A small XML reader keeps attribute values exactly as
 * written, so nothing is re-encoded on the way through.
 */
#ifndef ANIMFW_CORE_LEGACY_MERGE_HPP
#define ANIMFW_CORE_LEGACY_MERGE_HPP

#include <optional>
#include <string>
#include <string_view>

namespace AnimationFramework
{
    /** @brief A mod's changes to one database, as delta .adb text, with what it holds. */
    struct AdbDelta
    {
        std::string text;
        int options = 0;
        int removed_options = 0;
        int variants = 0;
        int removed_variants = 0;
        int sub_adbs = 0;

        /// Whether the delta changes nothing.
        [[nodiscard]] bool empty() const noexcept
        {
            return options + removed_options + variants + removed_variants + sub_adbs == 0;
        }
    };

    /** @brief The delta that turns @p base into @p modded. Nothing when either text is not a well-formed AnimDB. */
    [[nodiscard]] std::optional<AdbDelta> make_adb_delta(std::string_view base, std::string_view modded);

    /**
     * @brief A mod's changes to one Mannequin controller definition (`*_controllerdefs.xml`), as delta text.
     * @details Delta format, also what a mod writes by hand as `<stem>__<modid>.xml`:
     *          `<ControllerDef><FragmentDefs><Frag scopes flags [Mode="replace"]><Override tags fragTags scopes
     *          [Mode="remove"]/></Frag></FragmentDefs><ScopeDefs><Scope layer numLayers context Tags/></ScopeDefs>
     *          <ScopeContextDefs><Ctx/></ScopeContextDefs><FragmentDefsImports><FragmentDefsImport filename/>
     *          </FragmentDefsImports></ControllerDef>`. A fragment definition is matched by name, case-insensitive. Its
     *          attributes apply when non-empty, or exactly as written (empty flags included) with Mode="replace". An
     *          Override is keyed by its tags and fragTags as tag sets and replaces the stock one with that key, or is
     *          removed with Mode="remove".
     */
    struct ControllerDefsDelta
    {
        std::string text;
        /// Fragment definitions added or whose scopes/flags changed.
        int fragment_defs = 0;
        int overrides = 0;
        int removed_overrides = 0;
        /// Scope definitions added or changed.
        int scopes = 0;
        int scope_contexts = 0;
        int imports = 0;

        /// Whether the delta changes nothing.
        [[nodiscard]] bool empty() const noexcept
        {
            return fragment_defs + overrides + removed_overrides + scopes + scope_contexts + imports == 0;
        }
    };

    /** @brief The delta that turns @p base into @p modded. Nothing when either text is not a ControllerDef. */
    [[nodiscard]] std::optional<ControllerDefsDelta>
    make_controller_defs_delta(std::string_view base, std::string_view modded);

    /**
     * @brief A mod's changes to one animation event list (`*.animevents`), as delta text.
     * @details Delta format: `<anim_event_list><animation name="<clip path>">events</animation>
     *          <animation name="..." Mode="remove"/></anim_event_list>`. An animation is keyed by its clip path
     *          (case-insensitive, either slash) and carries the whole event list it replaces. Mode="append" adds its
     *          events after the stock ones. The engine reads the first non-empty list per clip, so a merged document
     *          keeps one element per clip.
     */
    struct AnimEventsDelta
    {
        std::string text;
        /// Clips whose event list the mod adds or changes.
        int animations = 0;
        int removed = 0;

        /// Whether the delta changes nothing.
        [[nodiscard]] bool empty() const noexcept { return animations + removed == 0; }
    };

    /** @brief The delta that turns @p base into @p modded. Nothing when either text is not an anim_event_list. */
    [[nodiscard]] std::optional<AnimEventsDelta> make_anim_events_delta(std::string_view base, std::string_view modded);

    /** @brief An animation event list's clip key: lowercase, forward slashes, no leading "./". */
    [[nodiscard]] std::string anim_event_key(std::string_view clip_path);

    /**
     * @brief A mod's additions to one Mannequin tag definition (`*_tags.xml`, `*_fragmentids.xml`), as delta text.
     * @details Delta format: `<TagDefinition><Imports><Import filename/></Imports><Tags><Tag name/>
     *          <Group name><Tag name/></Group></Tags></TagDefinition>`. A tag is keyed by its name at its level and a
     *          group merges into the stock group of its name, both case-insensitive. The merge only ever adds, so every
     *          stock tag keeps its index (a fragment ID is its tag's index): a tag the mod changed or dropped is not
     *          carried, only counted.
     */
    struct TagDefinitionDelta
    {
        std::string text;
        /// Tags added, those inside an added group included.
        int tags = 0;
        int imports = 0;
        /// Stock tags the mod changed or dropped.
        int not_carried = 0;

        /// Whether the delta adds nothing (the stock tags it does not carry aside).
        [[nodiscard]] bool empty() const noexcept { return tags + imports == 0; }
    };

    /** @brief The additions that turn @p base into @p modded. Nothing when either text is not a TagDefinition. */
    [[nodiscard]] std::optional<TagDefinitionDelta>
    make_tag_definition_delta(std::string_view base, std::string_view modded);

    /**
     * @brief A mod's changes to one animation action table (`anim_fragment.xml`, `actor_anim_action.xml`), as a row
     *        patch: `<database><anim_fragments version=".."><anim_fragment .../></anim_fragments></database>`.
     * @details A row is keyed by its actor class, fragment, option position and option GUID (`fragment_guid` or
     *          `mn_fragment_guid`), unique in the game's tables. Rows the mod adds or changes are carried, and the
     *          merge replaces a row with the same key or appends it. Rows the mod dropped are only counted: the merge
     *          never removes rows.
     */
    struct TableRowsDelta
    {
        std::string text;
        int added = 0;
        int changed = 0;
        int dropped = 0;

        /// Whether the delta carries no row (the rows it dropped aside, which the merge never removes).
        [[nodiscard]] bool empty() const noexcept { return added + changed == 0; }
    };

    /** @brief The rows that turn @p base into @p modded. Nothing when either text is not a table database. */
    [[nodiscard]] std::optional<TableRowsDelta> make_table_rows_delta(std::string_view base, std::string_view modded);

} // namespace AnimationFramework

#endif // ANIMFW_CORE_LEGACY_MERGE_HPP
