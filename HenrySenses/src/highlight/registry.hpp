/**
 * @file highlight/registry.hpp
 * @brief The set of highlighted entities: category, flags, colour word, and the node state to restore.
 *
 * A loot scan replaces the set through apply_batch(), which diffs it against the
 * current set, writes or clears each render node's HUD silhouette word, invalidates the node's persistent render
 * object on every change, and holds the node's render overrides (update_render_node_overrides()) with what a restore
 * needs. clear_all_highlights() restores every node it touched; it runs before the hooks are removed, on level
 * change, and whenever a gate hides the highlight. Every call runs on the game main thread.
 */
#ifndef HENRYSENSES_REGISTRY_HPP
#define HENRYSENSES_REGISTRY_HPP

#include "game_structures.hpp"
#include "engine/entity_access.hpp"
#include "highlight/groups.hpp"

#include <DetourModKit/error.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace HenrySenses
{
    /**
     * @enum LootCategory
     * @brief What kind of object an entity is (the loot scan's classification; a highlight group maps it to a
     *        target).
     */
    enum class LootCategory : std::uint8_t
    {
        /// A dead or unconscious human with lootable inventory.
        HumanCorpse = 1,
        /// A dead animal that can be looted or butchered.
        AnimalCarcass = 2,
        /// A dropped PickableItem.
        Item = 3,
        /// A stash, chest, nest or other inventory container.
        Container = 4,
        /// A PickableArea herb.
        Herb = 5,
        /// An entity matched by a group's Class, Name or Model pattern.
        Custom = 6,
        /// A living human NPC.
        LiveNpc = 7,
        /// A living animal that is not a horse or a dog (game, livestock, birds).
        LiveAnimal = 8,
        /// A living horse, the player's included.
        Horse = 9,
        /// A living dog.
        Dog = 10,
        /// A flock animal (chicken, rat, mouse, bird, fish).
        Critter = 11,
    };

    /**
     * @brief Returns a short name for a category, for log lines.
     * @param category The category.
     * @return A static string.
     */
    [[nodiscard]] const char *loot_category_name(LootCategory category) noexcept;

    /**
     * @enum LootFlag
     * @brief Per-entity modifiers reported with the category.
     */
    enum class LootFlag : std::uint32_t
    {
        /// No modifier.
        None = 0,
        /// Taking it is a crime (the Stolen tag).
        Illegal = 0x001,
        /// A locked container.
        Locked = 0x002,
        /// Nothing left for the player; drawn dimmed.
        Empty = 0x004,
        /// An animal carcass that can be butchered.
        Butcherable = 0x008,
        /// An unconscious human: both a body (Corpses) and a living NPC (NPCs).
        Unconscious = 0x020,
        /// The emptiness test returned nothing.
        EmptyUnknown = 0x040,
        /// A StashCorpse, whose own mesh is invisible, so markers are preferred for it.
        StashCorpse = 0x100,
        /// A living NPC the game marks as a public enemy (bandits, Cumans).
        Hostile = 0x200,
    };

    /**
     * @brief Returns the raw bit value of a LootFlag.
     * @param flag The flag.
     * @return Its bit.
     */
    [[nodiscard]] constexpr std::uint32_t loot_flag_bit(LootFlag flag) noexcept
    {
        return static_cast<std::uint32_t>(flag);
    }

    /// Steps a highlight's intensity is quantised to in its word (a step change rewrites the word).
    inline constexpr float INTENSITY_STEPS = 32.0f;

    /**
     * @struct HighlightRequest
     * @brief One classified entity of a scan.
     */
    struct HighlightRequest
    {
        /// The entity to highlight.
        EntityId entity_id{0};
        /// Its loot category.
        LootCategory category{LootCategory::Item};
        /// OR of LootFlag bits.
        std::uint32_t flags{0};
        /// Distance to the player in metres when scanned, negative when unknown.
        float distance{-1.0f};
        /// Colour (0xRRGGBBAA) before the distance fade and the intensity; set by the highlight group.
        std::uint32_t color{0};
        /// Reach of the group, for the distance fade.
        float fade_radius{20.0f};
        /// The part of the group's fade the word carries, in [0, 1].
        float intensity{1.0f};
        /// The group's whole fade, for a marker's colour.
        float marker_intensity{1.0f};
        /// How the group draws it (Box: brackets only, no silhouette word).
        GroupStyle style{GroupStyle::Outline};
        /// The group's particle effect (intern_effect_name id; 0 = none).
        std::uint16_t effect{0};
        /// Uniform scale of that effect.
        float effect_scale{1.0f};
    };

    /**
     * @struct ApplyOptions
     * @brief How apply_batch() renders the set.
     */
    struct ApplyOptions
    {
        /// Write HUD silhouette words (the engine silhouette backend is live).
        bool write_words{false};
        /// Move highlighted nodes into the always-visible list (requires write_words).
        bool render_always{false};
        /// Player position for the distance fade; no fade without it.
        std::optional<game_structures::Vec3f> player_position{};
    };

    /**
     * @struct MarkerTarget
     * @brief One highlighted entity for the marker backend.
     */
    struct MarkerTarget
    {
        /// The entity.
        EntityId entity_id{0};
        /// Its colour word (with the intensity, before the distance fade).
        std::uint32_t color_word{0};
        /**
         * @brief Drawn as brackets: its group's Style is Box, or its own mesh cannot show a silhouette (StashCorpse, no
         * render node, an entity the game keeps invisible for now, or a mesh past the engine's draw distance). Every
         * batch reads the last two again.
         */
        bool marker_preferred{false};
    };

    /**
     * @brief Prepares the registry.
     * @details Keeps the entries an earlier, refused teardown of this image left applied, so they can still be
     *          restored (the controller does it on its first tick).
     * @return An empty value when the registry is ready.
     */
    [[nodiscard]] DMK::Result<void> initialize_registry();

    /**
     * @brief Replaces the highlighted set with @p batch, applying and clearing node state as needed.
     * @details A batch that would write exactly what the last one wrote (the same entities, tags, words at the
     *          scanned distances, marker colours and options) is skipped, except that a full apply runs at least every
     *          500 ms: it re-resolves every node, so a proxy recreated by streaming gets its word again, and checks
     *          every always-visible bit (the work maintain_highlights() does between scans). Every batch, skipped or
     *          not, re-reads whether the game keeps each entity invisible (such an entity shows its marker).
     * @param batch The classified entities of the latest scan, nearest first.
     * @param options How to render the set.
     * @note Main thread only.
     */
    void apply_batch(std::span<const HighlightRequest> batch, const ApplyOptions &options) noexcept;

    /**
     * @struct RegistryApplyStats
     * @brief How many batches applied in full and how many changed nothing, since the last take (the status line).
     */
    struct RegistryApplyStats
    {
        std::size_t full{0};
        std::size_t skipped{0};
    };

    /** @brief Returns the apply counts since the last call and starts counting again. */
    [[nodiscard]] RegistryApplyStats take_registry_apply_stats() noexcept;

    /**
     * @brief Restores every node the registry touched and empties the set.
     * @details A node whose entity is gone needs nothing: a destroyed node takes its word with it and the engine
     *          erases it from the always-visible list when it frees it. A node that survives (the entity still
     *          resolves to the same node) gets its word cleared and is moved back into the octree.
     * @note Main thread only.
     */
    void clear_all_highlights() noexcept;

    /**
     * @brief Re-asserts the always-visible bookkeeping of every tracked node, unless a full apply_batch() did within
     *        the last 500 ms (it runs the same checks).
     * @details A node whose ERF_RENDER_ALWAYS bit was cleared by someone else while it sits in the always-visible
     *          list would leave a dangling list entry when the engine later unregisters it, so it is moved back into
     *          the octree at once.
     * @note Main thread only.
     */
    void maintain_highlights() noexcept;

    /**
     * @brief Refreshes the persistent render object of every node that shows a word, so it is refilled with the
     *        current silhouette_object_flags() ([Render] SeeThrough changed).
     * @return The number of render objects refreshed.
     * @note Main thread only.
     */
    std::size_t refresh_highlight_render_objects() noexcept;

    /**
     * @brief Returns the number of currently highlighted entities.
     * @return The count.
     */
    [[nodiscard]] std::size_t highlighted_count() noexcept;

    /**
     * @brief Copies the highlighted entities for the marker backend.
     * @param out Receives the targets (cleared first).
     */
    void collect_marker_targets(std::vector<MarkerTarget> &out);

    /**
     * @brief Logs every tracked entity with its node, word and always-visible state (the state report).
     * @note Main thread only.
     */
    void log_registry_state();

    /**
     * @brief Turns a colour word into the word written to a node, the way apply_batch() does: the distance fade, the
     *        intensity (in INTENSITY_STEPS steps) and [Render] Style.
     * @param color_word The packed 0xRRGGBBAA colour.
     * @param distance Distance to the player in metres, negative when unknown.
     * @param fade_radius The reach the distance fade ends at.
     * @param intensity The group's intensity in [0, 1].
     * @param style Outline or Fill (the word's alpha byte tells the composite which).
     * @return The final, non-zero word.
     */
    [[nodiscard]] std::uint32_t styled_word(
        std::uint32_t color_word,
        float distance,
        float fade_radius,
        float intensity,
        GroupStyle style
    ) noexcept;

    /**
     * @brief Scales a colour word by a quantised intensity (the marker colour of a fading group).
     */
    [[nodiscard]] std::uint32_t intensity_scaled(std::uint32_t color_word, float intensity) noexcept;

    /**
     * @brief Reports whether the loot set currently holds state on @p node.
     * @param node A render node address (compared, never dereferenced).
     * @return True when a tracked entity's word or always-visible move was applied to it.
     */
    [[nodiscard]] bool registry_owns_node(std::uintptr_t node) noexcept;

} // namespace HenrySenses

#endif // HENRYSENSES_REGISTRY_HPP
