/**
 * @file detect/loot_scanner.hpp
 * @brief The native loot scan: classifies the actors, world items and containers around the player with the game's
 *        own interaction rules (engine/game_natives.hpp) and returns the highlight set, nearest first.
 *
 * The rules are the ones the game's interaction scripts apply before they offer an action:
 *
 * - Actors (BasicAIActions, BasicAnimal): a dead human is a corpse when the player may loot it (CanLoot) and it holds
 *   something; a dead animal is a carcass when it can be butchered or holds something, unless the
 *   animal_disableLootButcherActions script context forbids both. An unconscious human is a body under the same rules
 *   (the game lets the player loot one) as well as a living NPC; both records carry the Unconscious flag. Looting a
 *   body is a crime unless its soul is legal to loot. Living NPCs, animals, horses and dogs are reported in categories
 *   of their own; a living public enemy carries the Hostile flag.
 * - World items (PickableItem): an item held by an inventory (worn, wielded, carried, on a body) is never loot; an
 *   NPC-only item, an item in use and an item that is not a player item are skipped; stealing it (or taking shop
 *   goods) is a crime.
 * - Containers (Stash, StashCorpse, Nest, CartStash, DestroStash): not hidden, still interactive, not shot, with an
 *   inventory the player can use (or locked), not a shop stash; opening it is a crime when its owner is someone
 *   other than the player and not a public enemy.
 *
 * Objects that pass are reported with their tags (stolen, locked, empty, ...); the highlight groups decide which of
 * them show and in which colour (highlight/groups.hpp).
 *
 * Actors and items come from the game's actor and item maps, containers, flock animals (Boid entities: chickens, rats,
 * mice, birds, fish) and the entities a group's Class, Name or Model pattern matches from an index filled by an
 * entity-system walk that runs a slice per tick. The objects near the player are picked out of those level-wide sets in
 * ticks without a scan (advance_loot_picks), and every scan tests only them. Between two full walks the index follows
 * the entity system's insert counters: a tick whose counters did not move walks nothing, one whose counters moved
 * walks only the entities inserted since. Static world objects a Model pattern matches come from an octree query
 * (collect_model_brushes).
 *
 * Main thread only.
 */
#ifndef HENRYSENSES_LOOT_SCANNER_HPP
#define HENRYSENSES_LOOT_SCANNER_HPP

#include "game_structures.hpp"
#include "highlight/registry.hpp"

#include <DetourModKit/error.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace HenrySenses
{
    struct GroupPatterns;

    /**
     * @struct LootScanOptions
     * @brief What one scan looks for.
     */
    struct LootScanOptions
    {
        /// Scan centre (the player).
        game_structures::Vec3f origin{};
        /// Scan radius in metres (the widest reach of the groups that want these categories).
        float radius{20.0f};

        bool human_corpses{true};
        bool animals{true};
        bool items{true};
        bool containers{true};
        bool live_npcs{false};
        bool live_animals{false};
        bool horses{false};
        bool dogs{false};
        /// Flock animals (chickens, rats, mice, birds, fish).
        bool critters{false};
    };

    /**
     * @struct LootScanStats
     * @brief What one scan saw.
     */
    struct LootScanStats
    {
        std::size_t actors{0};
        std::size_t items{0};
        std::size_t indexed{0};
        std::size_t candidates{0};
        std::size_t records{0};
        double milliseconds{0.0};
        /// The parts of milliseconds: actors, items, containers and custom classes.
        double actor_ms{0.0};
        double item_ms{0.0};
        double index_ms{0.0};
        /// The container index has finished at least one walk.
        bool index_ready{false};
        /// Work was left for the next tick (objects not classified yet): scan again then.
        bool more{false};
    };

    /**
     * @struct LootWorkStats
     * @brief What the near picks and the index did between two take_loot_work_stats() calls (the status line).
     */
    struct LootWorkStats
    {
        std::size_t picks{0};
        double pick_ms{0.0};
        double pick_ms_max{0.0};
        /// Ticks the counter gate found nothing inserted since the index's claim (no walk at all).
        std::size_t gate_skips{0};
        /// Recent walks (the entities inserted since the claim), the entities they read and the ones they added.
        std::size_t recent_walks{0};
        std::size_t recent_entities{0};
        std::size_t recent_added{0};
        /// Full walks finished, and their busy time.
        std::size_t full_walks{0};
        double walk_ms{0.0};
    };

    /** @brief Prepares the scanner. */
    [[nodiscard]] DMK::Result<void> initialize_loot_scanner();

    /** @brief Releases the index walk. */
    void shutdown_loot_scanner() noexcept;

    /**
     * @brief Drops every cache (level change): the index, the shop verdicts, the verdict log.
     */
    void reset_loot_scanner() noexcept;

    /**
     * @brief Keeps the index (containers, critters and pattern matches) current: merges the entities inserted since
     *        its last update when the entity counters moved, or advances a full walk by one budgeted slice.
     * @details A full walk runs when there is no index, when the player moved away from a Name or Model pattern's
     *          walk centre, when the counters went back or more entities came than a recent walk takes (256), and as
     *          a safety walk every minute. Without usable counters, and while a Name or Model pattern exists and a
     *          group shows loot (those match only the entities near the walk centre, and one may walk into reach), the
     *          level is walked on the old timers.
     * @param active A group shows loot (the timers' cadence).
     * @param patterns The pattern targets of the enabled groups; a change rebuilds the index.
     * @param center The player position: Name and Model patterns are tested only around it.
     * @return True when a full walk finished this call.
     */
    bool
    advance_loot_index(bool active, const GroupPatterns &patterns, const std::optional<game_structures::Vec3f> &center);

    /**
     * @brief Runs at most one near pick that is due (a tick without a scan runs it, so a pick and a scan never add
     *        up in one frame).
     * @param options The options the next scan uses (its origin and radius).
     * @param blocking_only Run only a pick the next scan cannot do without: one never taken, taken for another reach
     *        or over an index that was replaced since, or one overdue by twice its clock or move limit because every
     *        tick had a scan due (the caller then holds the scan for a tick).
     * @return True when a pick ran.
     */
    bool advance_loot_picks(const LootScanOptions &options, bool blocking_only);

    /**
     * @brief Drops every remembered item, container and pattern verdict, so the next scan classifies them again.
     * @details For the moments a verdict may have changed under a long lifetime: the player leaving a minigame
     *          (such as lockpicking), a saved INI, the first scan after an idle stretch.
     * @param reason For the log line.
     */
    void flush_loot_verdicts(const char *reason) noexcept;

    /** @brief Returns what the picks and the index did since the last call, and starts counting again. */
    [[nodiscard]] LootWorkStats take_loot_work_stats() noexcept;

    /**
     * @struct ModelBrush
     * @brief A static world object whose model matched a Model pattern.
     */
    struct ModelBrush
    {
        std::uintptr_t brush{0};
        game_structures::Aabb bounds{};
        float distance{0.0f};
    };

    /**
     * @brief Finds the static world objects (brushes) within @p radius whose model path matches one of @p patterns.
     * @param keep The previous hits, offered again (a highlighted brush leaves the octree).
     * @param out Receives the hits, nearest first (cleared first).
     * @note Main thread.
     */
    void collect_model_brushes(
        const game_structures::Vec3f &center,
        float radius,
        std::span<const std::string> patterns,
        std::span<const ModelBrush> keep,
        std::vector<ModelBrush> &out
    );

    /**
     * @brief Runs one scan.
     * @param options What to look for.
     * @param out Receives the records, nearest first (cleared first).
     * @return What the scan saw.
     */
    LootScanStats run_loot_scan(const LootScanOptions &options, std::vector<HighlightRequest> &out);

    /** @brief Logs the index and the last scan (the state report). */
    void log_loot_scanner_state();

} // namespace HenrySenses

#endif // HENRYSENSES_LOOT_SCANNER_HPP
