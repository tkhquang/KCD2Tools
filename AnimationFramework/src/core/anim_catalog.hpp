/**
 * @file core/anim_catalog.hpp
 * @brief What Henry can play: every fragment of his databases and each option of them, read from the game's Mannequin
 *        files and the mods' patches of them, so a pick list needs no copy of its own and follows game updates and
 *        mods.
 *
 * The catalog reads the files, not the loaded databases: it costs nothing until a mod asks, and it needs no game
 * address. A patch merges as it does at game load (see hooks/anim_patch.hpp): the options with the same tags are
 * replaced in place, Mode="append" adds after them, Mode="remove" drops them, and a new fragment or SubADB is added.
 *
 * Lines hold tab-separated fields:
 *   - A fragment: its name, the tags it plays on standing, "partial" when its definition leaves out the full body,
 *     "seat:<tags>" and "horse:<tags>" when it has an option there, "women" when the women's database has it. Only
 *     fragments Henry has an option for are listed: the fragment IDs are shared with the women and the dog.
 *   - An option that plays a clip: "@" and its fragment, its first clip, its tags, its number among the fragment's
 *     options with those tags, its set ("stand" for the tags the fragment plays on standing, "seat", "horse", else
 *     empty), "partial", the rest of its clips. A scope's own part (Scope_ tags) is left out.
 *
 * Tags that keep a fragment from the bind pose are its option's: the option kept is, in order, not one for another
 * scope, one with an animation layer, one on the fewest tags. A fragment with no standing option stands on its best
 * option of all.
 */
#ifndef ANIMFW_CORE_ANIM_CATALOG_HPP
#define ANIMFW_CORE_ANIM_CATALOG_HPP

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace AnimationFramework
{
    /// Henry's database, controller definition and fragment ID list: what the catalog reads and a new action patches.
    inline constexpr std::string_view MALE_DATABASE = "animations/mannequin/adb/kcd_male_database.adb";
    inline constexpr std::string_view MALE_CONTROLLER_DEFS = "animations/mannequin/adb/kcd_male_controllerdefs.xml";
    inline constexpr std::string_view MALE_FRAGMENT_IDS = "animations/mannequin/adb/kcd_male_fragmentids.xml";

    /** @brief Where the catalog reads the game's files. */
    struct AnimCatalogFiles
    {
        /// A file by its path (lowercase, forward slashes): the copy the game loads, or nothing.
        std::function<std::optional<std::string>(const std::string &)> read;
        /// The texts of a file's `<stem>__<modid>.<ext>` patches, in the order they merge.
        std::function<std::vector<std::string>(const std::string &)> patches;
    };

    /** @brief One search of the catalog. */
    struct AnimCatalogQuery
    {
        /// Found in any field, ignoring ASCII case. Empty matches every line.
        std::string_view text;
        /// Lists the options too, after the fragments.
        bool options = false;
        /// Only what has a version in this posture ("seat", "horse"). Empty lists all.
        std::string_view posture;
        /// The page: how many matches to skip, and how many to return.
        std::size_t first = 0;
        std::size_t count = 0;
    };

    /** @brief One page of a search. */
    struct AnimCatalogPage
    {
        /// Every match of the search, not only this page's.
        std::size_t total = 0;
        /// The page's lines, each ending in a newline.
        std::string lines;
    };

    /**
     * @brief The catalog, built once from the files. Its lines point into its own text, so it neither copies nor moves.
     */
    class AnimCatalog
    {
    public:
        /// Reads Henry's databases, fragment IDs and controller definitions through @p files.
        explicit AnimCatalog(const AnimCatalogFiles &files);
        AnimCatalog(const AnimCatalog &) = delete;
        AnimCatalog &operator=(const AnimCatalog &) = delete;
        AnimCatalog(AnimCatalog &&) = delete;
        AnimCatalog &operator=(AnimCatalog &&) = delete;
        ~AnimCatalog() = default;

        /// The number of fragment lines.
        [[nodiscard]] std::size_t fragment_count() const noexcept { return m_fragment_count; }
        /// The number of option lines.
        [[nodiscard]] std::size_t option_count() const noexcept { return m_lines.size() - m_fragment_count; }

        /** @brief Every line, the fragments and then the options, each ending in a newline. */
        [[nodiscard]] std::string_view text() const noexcept { return m_text; }

        /**
         * @brief One page of the lines that match @p query.
         * @param hidden Name prefixes of fragments the search leaves out, with their options: a mod's internal
         *        fragments, which only it plays.
         */
        [[nodiscard]] AnimCatalogPage find(const AnimCatalogQuery &query, std::span<const std::string> hidden) const;

        /** @brief The line of the fragment named @p name, without its newline, or nothing. */
        [[nodiscard]] std::optional<std::string_view> fragment_line(std::string_view name) const;

    private:
        /// One line, without its newline, as written and lowercased (views into m_text and m_lowered).
        struct Line
        {
            std::string_view text;
            std::string_view lowered;
        };

        std::string m_text;
        std::string m_lowered;
        std::vector<Line> m_lines;
        std::size_t m_fragment_count = 0;
    };
} // namespace AnimationFramework

#endif // ANIMFW_CORE_ANIM_CATALOG_HPP
