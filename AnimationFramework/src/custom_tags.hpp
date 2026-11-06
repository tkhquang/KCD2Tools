/**
 * @file custom_tags.hpp
 * @brief Custom tags: tags Henry's option and transition queries carry while their condition holds (the rules they
 *        follow are in core/tag_rules.hpp, the queries in hooks/view_variants.cpp).
 *
 * The framework's own `thirdPerson` is one, on while mod_camera_view says third person. A mod declares more in its
 * patch to Henry's tag definition, `Animations/Mannequin/ADB/kcd_male_tags__<modid>.xml`, as a plain tag (outside any
 * group) with an `animfw_cvar` attribute:
 *
 *     <Tag name="tired" animfw_cvar="mymod_tired" animfw_min="1" animfw_priority="0"/>
 *
 * The tag is on while that integer console variable is at least animfw_min (default 1). The framework finds the
 * variable or registers it (value 0, console_vars.hpp) once the console is up, so any script, menu or the console can
 * set it. animfw_priority (default 0) ranks the tag against the others, and the INI's [CustomTags] Priority and
 * Disabled lists have the final say. A name the game's own definition already has is refused.
 */
#ifndef ANIMFW_CUSTOM_TAGS_HPP
#define ANIMFW_CUSTOM_TAGS_HPP

#include "core/tag_rules.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace AnimationFramework
{
    /** @brief One declared tag. */
    struct CustomTagDeclaration
    {
        std::string name;
        /// The console variable that turns it on.
        std::string cvar;
        std::int32_t min_value = 1;
        std::int32_t priority = 0;
        /// The mod that declared it, for the log.
        std::string source;
    };

    /** @brief The ranked tags of one generation, as the queries read them. Immutable once published. */
    struct CustomTagSnapshot
    {
        std::uint32_t generation = 0;
        /// Tags held, in rising rank: index count - 1 ranks highest (an active mask's highest bit).
        std::uint32_t count = 0;
        /// Mannequin's CRC-32 of each lowered name (CTagDefinition::Find).
        std::array<std::uint32_t, tag_rules::MAX_CUSTOM_TAGS> crc{};
        /// Each name, NUL-terminated (truncated past 47 characters), for the log.
        std::array<std::array<char, 48>, tag_rules::MAX_CUSTOM_TAGS> name{};
    };

    /**
     * @brief Takes the INI's choices and publishes the built-in set.
     * @param third_person Whether the built-in thirdPerson tag is declared.
     * @param mod_tags Whether mods' declarations count.
     * @param priority The INI's priority list, highest first.
     * @param disabled The INI's list of tags that never turn on.
     * @note Setup only.
     */
    void configure_custom_tags(bool third_person, bool mod_tags, std::string_view priority, std::string_view disabled);

    /** @brief Whether mods' declarations count (CustomTags in the INI). */
    [[nodiscard]] bool mod_tags_enabled() noexcept;

    /**
     * @brief Publishes the tags declared by the mods' patches to Henry's tag definition, ranked with the built-in ones.
     *        Unchanged declarations keep the current generation.
     * @details Each call also marks the definition as reloaded, so the next poll_custom_tags() advances
     *          tag_definition_epoch().
     * @note The thread that loads Henry's tag definition (game start, mn_reload).
     */
    void declare_custom_tags(const std::vector<CustomTagDeclaration> &declared);

    /**
     * @brief Evaluates every tag's condition (registering mods' console variables on first use), publishes which
     *        are on and logs each change.
     * @param third_person_view The camera view, for thirdPerson.
     * @note Main thread, once per frame (the frame tick).
     */
    void poll_custom_tags(bool third_person_view) noexcept;

    /**
     * @brief Counts the rebuilds of Henry's tag definition, advanced by the first poll_custom_tags() after each load.
     * @details mn_reload clears and refills the same CTagDefinition object (CAnimationDatabaseManager::ReloadAll ->
     *          ReloadTagDefinition), so its address stays while its tags and bits can move. The tag file loads before
     *          the engine refills the object, and mn_reload finishes on the main thread before the next frame tick,
     *          so the epoch advances only once the refill is complete. A query caches what it reads from a definition
     *          under this epoch and reads it again once the epoch moves.
     * @note Any thread.
     */
    [[nodiscard]] std::uint32_t tag_definition_epoch() noexcept;

    /**
     * @brief The current snapshot, or nullptr before configure_custom_tags(). Any thread. The snapshot stays valid as
     *        long as this image.
     */
    [[nodiscard]] const CustomTagSnapshot *custom_tag_snapshot() noexcept;

    /**
     * @brief The tags of @p snapshot that are on (bit i: its tag i), or 0 when the last poll was for another
     *        generation. Any thread.
     */
    [[nodiscard]] std::uint32_t custom_tags_active(const CustomTagSnapshot &snapshot) noexcept;

    /** @brief Turns every tag off. Teardown only, once the hooks have retired. */
    void reset_custom_tags() noexcept;
} // namespace AnimationFramework

#endif // ANIMFW_CUSTOM_TAGS_HPP
