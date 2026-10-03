/**
 * @file detect/loot_scanner.cpp
 * @brief The native loot scan (see loot_scanner.hpp for the rules).
 */

#include "detect/loot_scanner.hpp"
#include "constants.hpp"
#include "rtti_types.hpp"
#include "engine/engine_env.hpp"
#include "engine/entity_access.hpp"
#include "engine/game_natives.hpp"
#include "engine/octree_query.hpp"
#include "engine/visual_resolver.hpp"
#include "highlight/groups.hpp"

#include <DetourModKit.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace HenrySenses
{
    namespace
    {
        constexpr std::size_t MAX_ACTORS = 16384;
        constexpr std::size_t MAX_ITEMS = 262144;
        // The index walk: a slice per tick within this budget.
        constexpr std::int64_t WALK_BUDGET_US = 1000;
        constexpr std::size_t WALK_BATCH = 1024;
        constexpr std::size_t MAX_WALK_ENTITIES = 400000;
        // Without the entity system's insert counters (unreadable, or not trusted), the whole level is walked again
        // this often. So it is with them while a Name or Model pattern exists and a group shows loot: those are tested
        // only on entities near the walk centre at walk time, and the counters report inserts, not an NPC or a horse
        // that walked into the reach since.
        constexpr std::int64_t REWALK_ACTIVE_MS = 4000;
        constexpr std::int64_t REWALK_IDLE_MS = 30000;
        // With them, a tick whose counters did not move walks nothing, and one whose counters moved walks only the
        // entities inserted since (they sit at the head of the used list). A full walk still runs this often, to catch
        // an entity a lock-free read raced past, and at most this often when the counters ask for one (they went back,
        // or too many entities came at once).
        constexpr std::int64_t SAFETY_WALK_MS = 60000;
        constexpr std::int64_t MIN_COUNTER_WALK_MS = 4000;
        // A recent walk runs whole in one tick (it has no slices), so a larger burst (a town streaming in) takes the
        // sliced full walk instead.
        constexpr std::uint64_t MAX_RECENT_WALK = 256;
        // Ids the recent walks classified, so an insert walked again before the claim passes it is not classified
        // twice; the set is emptied past this size (the next walk then classifies its entities once more).
        constexpr std::size_t MAX_RECENT_CLASSIFIED = 4 * MAX_RECENT_WALK;
        // A full walk claims the counters read at its start less this many inserts, so the first recent walk after it
        // looks again at the inserts that were in flight when it started.
        constexpr std::uint64_t CLAIM_OVERLAP = 16;
        // A recent walk that keeps meeting a used slot without an entity (one being spawned or freed) claims its
        // inserts anyway after this long; the safety walk covers whatever it missed.
        constexpr std::int64_t MAX_UNSETTLED_MS = 2000;
        // Counters that go back more often than this in a session are not trusted (the timers walk instead).
        constexpr int MAX_COUNTER_RESETS = 3;
        // The actor and item maps and the index hold the whole level, so the objects near the player are picked out
        // of them and every scan tests only those. A pick runs in a tick without a scan (one per tick), so a pick and a
        // scan never add up in one frame. A pick is due when it was never taken or was taken for another reach, or
        // over an index that was replaced (the scan waits for those), when the player moved its move limit away from
        // where it was taken, when what it picks from changed (the item map's size; entities added to the index), or
        // on its own clock: actors move, so theirs is the shortest; items and indexed entities rarely do (the clock
        // is for thrown items and flocks). The margin covers the move and, for actors, their own movement between two
        // picks. The actor pick is the costliest (a list of the level's ~2400 actors).
        constexpr std::array<std::int64_t, 3> PICK_REFRESH_MS{1500, 5000, 5000};
        constexpr float ACTOR_MARGIN = 20.0f;
        constexpr float ACTOR_MOVE_LIMIT = 8.0f;
        constexpr float OBJECT_MARGIN = 6.0f;
        constexpr float OBJECT_MOVE_LIMIT = 3.0f;
        // A scan can be due in every tick (a low frame rate with a short ScanIntervalMs, or a scan that keeps deferring
        // classification in a dense town), which leaves no tick for the picks. A pick overdue by this factor (its
        // clock, or the player's move from where it was taken) then takes a scan's tick, as a blocking one does.
        constexpr std::int64_t PICK_OVERDUE_FACTOR = 2;
        // Shop membership changes only when the player trades, so it is looked up again this often.
        constexpr std::int64_t SHOP_CACHE_MS = 5000;
        constexpr std::size_t MAX_LOGGED_VERDICTS = 16384;
        constexpr std::size_t MAX_CACHED_ANSWERS = 16384;
        // Items keep their verdict this long (plus a per-id stagger), containers longer. Minigame exits, saved
        // settings and the first full tick after idle invalidate all verdicts through flush_loot_verdicts. A hidden
        // or removed object is still dropped at once. Backdate a first answer by a per-key share of its lifetime so
        // objects found in one scan do not all expire together and repeat the classification work in one frame.
        constexpr std::int64_t VERDICT_TTL_MS = 750;
        constexpr std::int64_t CONTAINER_VERDICT_TTL_MS = 5000;
        constexpr std::int64_t STAGGER_MS = 256;
        // The role bits of a verdict key (above the entity id).
        constexpr std::uint64_t ROLE_CONTAINER = 1ull << 32;
        constexpr std::uint64_t ROLE_PATTERN = 2ull << 32;
        // The time one scan may spend on verdicts; past it an expired verdict serves until a later scan and a new
        // object waits for a scan next tick. A group switching on otherwise classified every object near the player in
        // one frame (~3.6 ms in a town).
        constexpr std::int64_t CLASSIFY_BUDGET_US = 1000;

        // The soul script context that switches off the loot and butcher actions of a carcass (BasicAnimal).
        constexpr std::string_view ANIMAL_NO_LOOT_CONTEXT = "animal_disableLootButcherActions";

        // The entity walk reads names and model paths (for Name and Model patterns) only this far past the widest
        // reach of the groups that use them, and walks again once the player moved half of it.
        constexpr float PATTERN_MARGIN = 30.0f;
        // The class of every individual flock animal (chicken, rat, mouse, bird, fish).
        constexpr std::string_view BOID_CLASS = "Boid";
        // Static world objects a Model pattern may pick, and the node cap of one octree query (a corruption guard far
        // above the brushes of any real box: a real count past it would drop matches inside the radius).
        constexpr std::uint32_t MAX_MODEL_NODES = 1u << 20;
        constexpr float MAX_MODEL_EXTENT = 30.0f;
        // The node cap's overflow is logged once per run of scans that hit it.
        bool s_model_overflow_logged = false;

        /** @brief What an indexed class is. */
        enum class IndexKind : std::int8_t
        {
            None = -1,
            /// Stash, Nest, CartStash.
            Stash,
            /// StashCorpse (its own mesh is invisible).
            StashCorpse,
            /// DestroStash (offers nothing once shot).
            DestroStash,
            /// A flock animal (Boid).
            Critter,
        };

        struct ContainerClass
        {
            std::string_view name;
            IndexKind kind;
        };

        // The classes the engine registers as stashes.
        constexpr std::array<ContainerClass, 5> CONTAINER_CLASSES{{
            {"Stash", IndexKind::Stash},
            {"StashCorpse", IndexKind::StashCorpse},
            {"Nest", IndexKind::Stash},
            {"CartStash", IndexKind::Stash},
            {"DestroStash", IndexKind::DestroStash},
        }};

        struct IndexEntry
        {
            EntityId id{0};
            std::uintptr_t entity{0};
            std::uintptr_t klass{0};
            IndexKind kind{IndexKind::None};
            /// A Class, Name or Model pattern of an enabled group matches the entity.
            bool pattern{false};
        };

        struct Verdict
        {
            bool accepted{false};
            LootCategory category{LootCategory::Item};
            std::uint32_t flags{0};
            const char *reason{""};
            /// Logged only at LogLevel = TRACE (the common, uninteresting rejects).
            bool quiet{false};
        };

        struct ScanContext
        {
            const LootScanOptions &options;
            std::uintptr_t player_actor{0};
            EntityId player_id{0};
            std::uintptr_t player_soul{0};
            Wuid player_soul_wuid{0};
            std::int64_t now_ms{0};
        };

        struct CachedAnswer
        {
            std::optional<bool> value{};
            std::int64_t at_ms{0};
        };

        struct CachedVerdict
        {
            Verdict verdict{};
            std::int64_t at_ms{0};
        };

        struct Found
        {
            float distance;
            HighlightRequest request;
        };

        enum class Fill : std::uint8_t
        {
            HasItems,
            Empty,
            Unknown,
        };

        std::atomic<bool> s_available{false};

        // Main-thread state.
        std::unordered_map<std::uintptr_t, IndexKind> s_class_kinds;
        // Whether a class matches a Class pattern, per class pointer.
        std::unordered_map<std::uintptr_t, bool> s_class_patterns;
        // The two maps' fast path for the walk, cleared with them.
        ClassMemo<IndexKind> s_kind_memo;
        ClassMemo<bool> s_pattern_memo;
        // The patterns the index was built for, and the centre and reach of the walk's Name and Model tests.
        GroupPatterns s_patterns;
        game_structures::Vec3f s_walk_center{};
        game_structures::Vec3f s_index_center{};

        EntityWalk s_walk;
        std::vector<WalkedEntity> s_batch;
        std::vector<IndexEntry> s_pending;
        std::vector<IndexEntry> s_index;
        bool s_index_valid = false;
        std::int64_t s_index_done_ms = 0;
        std::size_t s_walk_entities = 0;
        std::uint32_t s_walk_frames = 0;
        std::int64_t s_walk_busy_us = 0;
        std::int64_t s_walk_started_ms = 0;
        // Why the running (or last) full walk started, for its log line.
        const char *s_walk_reason = "";

        /**
         * @brief The counter gate in front of the index walk: which inserts the index already covers, and what the
         *        last read saw.
         */
        struct CounterGate
        {
            /// The counters can be read and have behaved (else the timers walk the level).
            bool trusted{true};
            /// Why they are not, for the state report.
            const char *distrust_reason{""};
            /// The index covers every entity inserted up to these counts.
            EntityCounters claimed{};
            bool claimed_valid{false};
            /**
             * @brief The counters read when the running (or last) full walk started, and what it claims when it
             * finishes (those, less CLAIM_OVERLAP inserts).
             */
            EntityCounters walk_read{};
            EntityCounters walk_claim{};
            bool walk_claim_valid{false};
            /// The previous read (at least a tick old when the next one comes).
            EntityCounters last{};
            bool last_valid{false};
            /// When recent walks started to meet a slot without an entity (0: they did not).
            std::int64_t unsettled_since_ms{0};
            /// How often the counters went back this session.
            int resets{0};
        };
        CounterGate s_gate;
        // The entities of the last recent walk (scratch).
        std::vector<WalkedEntity> s_recent;
        // The ids the recent walks classified since the last full walk (MAX_RECENT_CLASSIFIED).
        std::unordered_set<EntityId> s_recent_classified;
        // Each indexed id with its entity, for the recent walks' duplicate test; rebuilt from s_index when it was
        // replaced or trimmed.
        std::unordered_map<EntityId, std::uintptr_t> s_index_ids;
        bool s_index_ids_valid = false;
        // Bumped when a recent walk adds entities to the index (the index pick runs again, without holding a scan).
        std::uint32_t s_index_additions = 0;
        // What the index and the picks did since the last take_loot_work_stats().
        LootWorkStats s_work{};

        std::unordered_map<EntityId, CachedAnswer> s_shop_items;
        // Keyed by entity id and category: an unconscious human is logged once as a body and once as an NPC.
        std::unordered_map<std::uint64_t, std::uint64_t> s_logged;
        // Item and container verdicts, and the options they were made under.
        // Keyed by entity id and role (item, container, pattern match).
        std::unordered_map<std::uint64_t, CachedVerdict> s_verdicts;
        std::uint32_t s_verdict_options = 0;
        // Owner vtables of items held by something unknown, logged once each.
        std::unordered_set<std::uintptr_t> s_unknown_owner_types;

        // Bumped when a walk replaces the index, so the near set is picked again before the next scan.
        std::uint32_t s_index_generation = 0;

        // The objects near the player, picked from the maps and the index (advance_loot_picks).
        std::vector<ActorView> s_near_actors;
        std::vector<ItemView> s_near_items;
        std::vector<IndexEntry> s_near_index;
        /**
         * @brief Where, for which reach and when one near pick was taken, and the state of what it picked from.
         */
        struct NearPick
        {
            game_structures::Vec3f center{};
            float radius{0.0f};
            /// 0 = never taken.
            std::int64_t at_ms{0};
            /// Items: the item map's size; index: s_index_generation and s_index_additions.
            std::uint64_t source{0};
            std::uint64_t source_extra{0};
        };
        /** @brief The three picks, in the order a tie runs them. */
        enum Pick : std::size_t
        {
            PickActors,
            PickItems,
            PickIndex,
        };
        std::array<NearPick, 3> s_near_picks{};
        std::size_t s_level_actors = 0;
        std::size_t s_level_items = 0;
        std::vector<Found> s_found;
        LootScanStats s_last_stats{};

        [[nodiscard]] std::int64_t steady_us() noexcept
        {
            return std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now().time_since_epoch()
            )
                .count();
        }

        [[nodiscard]] constexpr std::uint32_t bit(LootFlag flag) noexcept
        {
            return loot_flag_bit(flag);
        }

        [[nodiscard]] std::uintptr_t class_of(std::uintptr_t entity) noexcept
        {
            const auto klass = DMK::memory::read<std::uintptr_t>(DMK::Address{entity + constants::ENTITY_CLASS_OFFSET});
            return klass && DMK::memory::is_plausible_ptr(DMK::Address{*klass}) ? *klass : 0;
        }

        [[nodiscard]] std::string_view trim(std::string_view text) noexcept
        {
            constexpr std::string_view whitespace = " \t\r\n";
            const std::size_t begin = text.find_first_not_of(whitespace);
            if (begin == std::string_view::npos)
            {
                return {};
            }
            return text.substr(begin, text.find_last_not_of(whitespace) - begin + 1);
        }

        /** @brief The kind of an entity class, resolved once per class pointer. */
        [[nodiscard]] IndexKind class_kind(std::uintptr_t klass, std::uintptr_t entity)
        {
            if (const IndexKind *memo = s_kind_memo.find(klass))
            {
                return *memo;
            }
            if (const auto it = s_class_kinds.find(klass); it != s_class_kinds.end())
            {
                s_kind_memo.store(klass, it->second);
                return it->second;
            }
            const std::string name = entity_class_name(entity);
            IndexKind kind = IndexKind::None;
            if (name == BOID_CLASS)
            {
                kind = IndexKind::Critter;
            }
            for (const ContainerClass &entry : CONTAINER_CLASSES)
            {
                if (entry.name == name)
                {
                    kind = entry.kind;
                    break;
                }
            }
            s_class_kinds.emplace(klass, kind);
            return kind;
        }

        [[nodiscard]] bool any_match(const std::vector<std::string> &patterns, std::string_view text) noexcept
        {
            return !text.empty() && std::any_of(
                                        patterns.begin(),
                                        patterns.end(),
                                        [text](const std::string &pattern) { return wildcard_match(pattern, text); }
                                    );
        }

        /** @brief Whether an entity class matches a Class pattern, resolved once per class pointer. */
        [[nodiscard]] bool class_matches(std::uintptr_t klass, std::uintptr_t entity)
        {
            if (s_patterns.classes.empty())
            {
                return false;
            }
            if (const bool *memo = s_pattern_memo.find(klass))
            {
                return *memo;
            }
            if (const auto it = s_class_patterns.find(klass); it != s_class_patterns.end())
            {
                s_pattern_memo.store(klass, it->second);
                return it->second;
            }
            const bool matched = any_match(s_patterns.classes, entity_class_name(entity));
            s_class_patterns.emplace(klass, matched);
            return matched;
        }

        /** @brief Whether an entity near the walk centre matches a Name or Model pattern. */
        [[nodiscard]] bool entity_matches(std::uintptr_t entity)
        {
            if (s_patterns.names.empty() && s_patterns.models.empty())
            {
                return false;
            }
            const std::optional<game_structures::Vec3f> position = entity_world_position(entity);
            if (!position.has_value())
            {
                return false;
            }
            const float reach = s_patterns.reach + PATTERN_MARGIN;
            const float dx = position->x - s_walk_center.x;
            const float dy = position->y - s_walk_center.y;
            const float dz = position->z - s_walk_center.z;
            if (dx * dx + dy * dy + dz * dz > reach * reach)
            {
                return false;
            }
            return (!s_patterns.names.empty() && any_match(s_patterns.names, entity_name(entity))) ||
                   (!s_patterns.models.empty() && any_match(s_patterns.models, entity_model_path(entity)));
        }

        void drop_index()
        {
            if (s_walk.active())
            {
                s_walk.cancel();
            }
            s_pending.clear();
            s_index.clear();
            s_batch.clear();
            s_index_valid = false;
            s_index_ids.clear();
            s_index_ids_valid = false;
            s_recent_classified.clear();
            ++s_index_generation;
            s_near_actors.clear();
            s_near_items.clear();
            s_near_index.clear();
            s_near_picks = {};
            // The counters' trust outlives the index; what it covers does not.
            s_gate.claimed_valid = false;
            s_gate.walk_claim_valid = false;
            s_gate.last_valid = false;
            s_gate.unsettled_since_ms = 0;
        }

        /**
         * @brief The index entry of a walked entity: a container or critter class, or a pattern match; std::nullopt
         *        for every other entity.
         */
        [[nodiscard]] std::optional<IndexEntry> index_entry_of(const WalkedEntity &walked)
        {
            if (walked.klass == 0 || walked.id == 0)
            {
                return std::nullopt;
            }
            const IndexKind kind = class_kind(walked.klass, walked.entity);
            const bool pattern = class_matches(walked.klass, walked.entity) || entity_matches(walked.entity);
            if (kind == IndexKind::None && !pattern)
            {
                return std::nullopt;
            }
            return IndexEntry{walked.id, walked.entity, walked.klass, kind, pattern};
        }

        void process_batch()
        {
            // The walk hands over each entity's class and id; only Name and Model patterns read the entity itself (its
            // position, near the walk centre).
            const bool positions = !s_patterns.names.empty() || !s_patterns.models.empty();
            for (std::size_t i = 0; i < s_batch.size(); ++i)
            {
                if (positions && i + PREFETCH_DISTANCE < s_batch.size())
                {
                    prefetch_entity(s_batch[i + PREFETCH_DISTANCE].entity, true);
                }
                if (const std::optional<IndexEntry> entry = index_entry_of(s_batch[i]); entry.has_value())
                {
                    s_pending.push_back(*entry);
                }
            }
            s_walk_entities += s_batch.size();
            s_batch.clear();
        }

        void finish_walk(bool complete)
        {
            s_index.swap(s_pending);
            s_pending.clear();
            s_index_valid = true;
            // The next recent walk looks again at the inserts made while this walk ran, which it may have missed.
            s_index_ids_valid = false;
            s_recent_classified.clear();
            s_index_center = s_walk_center;
            ++s_index_generation;
            s_index_done_ms = steady_us() / 1000;
            ++s_work.full_walks;
            s_work.walk_ms += static_cast<double>(s_walk_busy_us) / 1000.0;
            // A complete walk covers every insert the counters had counted when it started (less the overlap).
            s_gate.claimed_valid = complete && s_gate.walk_claim_valid;
            s_gate.claimed = s_gate.walk_claim;
            s_gate.unsettled_since_ms = 0;
            (void)DMK::log().try_log(
                DMK::LogLevel::Debug,
                "LootScan: index walked {} entities ({}, {}) over {} frame(s), {:.2f} ms busy, {} ms "
                "wall; {} container or custom entities, {} classes known; entity counters {}",
                s_walk_entities,
                complete ? "complete" : "cut short",
                s_walk_reason,
                s_walk_frames,
                static_cast<double>(s_walk_busy_us) / 1000.0,
                s_index_done_ms - s_walk_started_ms,
                s_index.size(),
                s_class_kinds.size(),
                s_gate.walk_claim_valid ? std::format(
                                              "inserts={} deletes={} at its start (inserts - deletes = {})",
                                              s_gate.walk_read.inserts,
                                              s_gate.walk_read.deletes,
                                              static_cast<std::int64_t>(s_gate.walk_read.inserts) -
                                                  static_cast<std::int64_t>(s_gate.walk_read.deletes)
                                          )
                                        : std::string{s_gate.trusted ? "unread" : s_gate.distrust_reason}
            );
        }

        /**
         * @brief Stops trusting the entity counters for the session; the index is then walked on the timers.
         */
        void distrust_counters(const char *reason)
        {
            if (!s_gate.trusted)
            {
                return;
            }
            s_gate.trusted = false;
            s_gate.distrust_reason = reason;
            s_gate.claimed_valid = false;
            s_gate.walk_claim_valid = false;
            s_gate.last_valid = false;
            (void)DMK::log().try_log(
                DMK::LogLevel::Info,
                "LootScan: the entity system's insert counters are not used ({}); the container index is walked "
                "every {} s while a group shows loot, every {} s otherwise",
                reason,
                REWALK_ACTIVE_MS / 1000,
                REWALK_IDLE_MS / 1000
            );
        }

        /**
         * @brief Adds the recent walk's entities the index lacks (a container, critter or pattern match spawned since
         *        the last walk).
         * @return How many were added.
         */
        std::size_t merge_recent()
        {
            const bool debug = DMK::log().is_enabled(DMK::LogLevel::Debug);
            if (!s_index_ids_valid)
            {
                s_index_ids.clear();
                s_index_ids.reserve(s_index.size());
                for (const IndexEntry &indexed : s_index)
                {
                    s_index_ids.insert_or_assign(indexed.id, indexed.entity);
                }
                s_index_ids_valid = true;
            }
            std::size_t added = 0;
            for (const WalkedEntity &walked : s_recent)
            {
                // Every insert is walked again until the claim passes it (the claim lags a tick, and a walk that met
                // an unsettled slot claims nothing), so an entity classified before is not classified again.
                // One whose class could not be read yet (still being spawned) is left for the next walk.
                if (walked.klass == 0 || walked.id == 0)
                {
                    continue;
                }
                if (s_recent_classified.size() >= MAX_RECENT_CLASSIFIED)
                {
                    s_recent_classified.clear();
                }
                if (!s_recent_classified.insert(walked.id).second)
                {
                    continue;
                }
                const std::optional<IndexEntry> entry = index_entry_of(walked);
                if (!entry.has_value())
                {
                    continue;
                }
                // A full walk may have seen it already.
                const auto [known, inserted] = s_index_ids.try_emplace(entry->id, entry->entity);
                if (!inserted && known->second == entry->entity)
                {
                    continue;
                }
                known->second = entry->entity;
                s_index.push_back(*entry);
                ++added;
                if (debug)
                {
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Debug,
                        "LootScan: index + {:#x} {} '{}' (inserted since the last walk{})",
                        entry->id,
                        entity_class_name(entry->entity),
                        entity_name(entry->entity),
                        entry->pattern ? ", pattern match" : ""
                    );
                }
            }
            return added;
        }

        /**
         * @enum GateStep
         * @brief What the counter gate asks of the index this tick.
         */
        enum class GateStep : std::uint8_t
        {
            /// The index is current (nothing inserted, or the recent entities were merged).
            Current,
            /// A full walk (the counters went back, or more entities came than a recent walk takes).
            FullWalk,
            /// The counters cannot be used; the timers decide.
            Timers,
        };

        /**
         * @brief Reads the entity counters and, when they moved, walks the entities inserted since the index's claim
         *        and merges them.
         * @details Guarded reads only, no engine lock. A read can race a writer that counted an insert but has not
         *          linked it yet, so a tick claims only the counters the previous tick read (every insert they
         *          counted is linked by now); the next tick walks the rest again.
         */
        [[nodiscard]] GateStep advance_counter_gate(std::int64_t now_ms)
        {
            const std::optional<EntityCounters> now = read_entity_counters();
            if (!now.has_value())
            {
                distrust_counters("the counters could not be read");
                return GateStep::Timers;
            }
            const EntityCounters previous = s_gate.last;
            const bool previous_valid = s_gate.last_valid;
            s_gate.last = *now;
            s_gate.last_valid = true;
            if (!s_gate.claimed_valid)
            {
                return GateStep::FullWalk;
            }
            if (now->inserts < s_gate.claimed.inserts || now->deletes < s_gate.claimed.deletes)
            {
                // The engine zeroes the counters when it resets the buffer; values that jump around are not counters.
                s_gate.claimed_valid = false;
                if (++s_gate.resets > MAX_COUNTER_RESETS)
                {
                    distrust_counters("they went back too often");
                    return GateStep::Timers;
                }
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "LootScan: entity counters went back (inserts {} -> {}, deletes {} -> {}); walking the level",
                    s_gate.claimed.inserts,
                    now->inserts,
                    s_gate.claimed.deletes,
                    now->deletes
                );
                return GateStep::FullWalk;
            }
            const std::uint64_t delta = now->inserts - s_gate.claimed.inserts;
            if (delta == 0)
            {
                ++s_work.gate_skips;
                s_gate.unsettled_since_ms = 0;
                return GateStep::Current;
            }
            if (delta > MAX_RECENT_WALK)
            {
                // The full walk that follows claims afresh; until it runs, the gate asks for it without a new line.
                s_gate.claimed_valid = false;
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "LootScan: {} entities inserted since the last walk (more than {}); walking the level",
                    delta,
                    MAX_RECENT_WALK
                );
                return GateStep::FullWalk;
            }
            s_recent.clear();
            const RecentWalk result = walk_recent_entities(static_cast<std::size_t>(delta), s_recent);
            if (result == RecentWalk::Failed)
            {
                distrust_counters("the used list could not be read");
                return GateStep::Timers;
            }
            ++s_work.recent_walks;
            s_work.recent_entities += s_recent.size();
            if (const std::size_t added = merge_recent(); added != 0)
            {
                s_work.recent_added += added;
                ++s_index_additions;
            }
            bool claim = result == RecentWalk::Complete;
            if (claim)
            {
                s_gate.unsettled_since_ms = 0;
            }
            else if (s_gate.unsettled_since_ms == 0)
            {
                s_gate.unsettled_since_ms = now_ms;
            }
            else if (now_ms - s_gate.unsettled_since_ms >= MAX_UNSETTLED_MS)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "LootScan: recent walks kept meeting a used slot without an entity for {} ms; its inserts are "
                    "claimed, the safety walk covers them",
                    now_ms - s_gate.unsettled_since_ms
                );
                s_gate.unsettled_since_ms = 0;
                claim = true;
            }
            if (claim && previous_valid && previous.inserts >= s_gate.claimed.inserts &&
                previous.inserts <= now->inserts && previous.deletes >= s_gate.claimed.deletes &&
                previous.deletes <= now->deletes)
            {
                s_gate.claimed = previous;
            }
            return GateStep::Current;
        }

        /**
         * @brief Why the index should be walked again now, or nullptr when it is current.
         * @param active A group shows loot (the timers' cadence without the counters, and with Name or Model patterns).
         * @param moved Name and Model patterns need the walk around the player's new position.
         */
        [[nodiscard]] const char *full_walk_reason(bool active, bool moved, std::int64_t now_ms)
        {
            const std::int64_t age_ms = now_ms - s_index_done_ms;
            if (!s_index_valid)
            {
                return "no index";
            }
            if (moved)
            {
                return "moved";
            }
            if (s_gate.trusted)
            {
                if (age_ms >= SAFETY_WALK_MS)
                {
                    return "safety";
                }
                // A Name or Model match is position-bound (entity_matches), so a moving entity that entered the reach
                // since the last walk needs a new one; the counters cannot tell.
                if (active && (!s_patterns.names.empty() || !s_patterns.models.empty()) && age_ms >= REWALK_ACTIVE_MS)
                {
                    return "name/model patterns";
                }
                const GateStep step = advance_counter_gate(now_ms);
                if (step == GateStep::Current)
                {
                    return nullptr;
                }
                if (step == GateStep::FullWalk)
                {
                    return age_ms >= MIN_COUNTER_WALK_MS ? "counters" : nullptr;
                }
            }
            return age_ms >= (active ? REWALK_ACTIVE_MS : REWALK_IDLE_MS) ? "timer" : nullptr;
        }

        // Verdict log

        [[nodiscard]] std::uint64_t verdict_key(const Verdict &verdict) noexcept
        {
            return (static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(verdict.reason)) << 20) ^
                   (static_cast<std::uint64_t>(verdict.flags) << 8) ^
                   (static_cast<std::uint64_t>(verdict.category) << 1) ^ (verdict.accepted ? 1u : 0u);
        }

        /**
         * @brief Logs a verdict that changed since the entity's last one: LogLevel = DEBUG logs the accepts and the
         *        telling rejects, TRACE also the quiet ones (a disabled category, a hidden entity).
         */
        void log_verdict(EntityId id, std::uintptr_t entity, float distance, const Verdict &verdict)
        {
            const DMK::LogLevel level = verdict.quiet ? DMK::LogLevel::Trace : DMK::LogLevel::Debug;
            if (!DMK::log().is_enabled(level))
            {
                return;
            }
            const std::uint64_t key = verdict_key(verdict);
            const std::uint64_t logged_key =
                static_cast<std::uint64_t>(id) | (static_cast<std::uint64_t>(verdict.category) << 32);
            const auto it = s_logged.find(logged_key);
            if (it != s_logged.end() && it->second == key)
            {
                return;
            }
            if (it != s_logged.end())
            {
                it->second = key;
            }
            else if (s_logged.size() < MAX_LOGGED_VERDICTS)
            {
                s_logged.emplace(logged_key, key);
            }
            (void)DMK::log().try_log(
                level,
                "Verdict: {:#x} {} '{}' {:.1f} m: {} {} flags={:#x} ({})",
                id,
                entity_class_name(entity),
                entity_name(entity),
                distance,
                verdict.accepted ? "ACCEPT" : "REJECT",
                loot_category_name(verdict.category),
                verdict.flags,
                verdict.reason
            );
        }

        // Shared tests

        [[nodiscard]] Verdict
        reject(LootCategory category, std::uint32_t flags, const char *reason, bool quiet = false) noexcept
        {
            return Verdict{false, category, flags, reason, quiet};
        }

        [[nodiscard]] Verdict accept(LootCategory category, std::uint32_t flags, const char *reason) noexcept
        {
            return Verdict{true, category, flags, reason, false};
        }

        /**
         * @brief The time to stamp an answer with: now, or for a first answer now backdated by a per-key share of its
         *        lifetime, so the answers one scan makes fall due in different scans later.
         */
        [[nodiscard]] std::int64_t answer_stamp(bool first, std::uint64_t key, std::int64_t now_ms, std::int64_t ttl)
        {
            if (!first || ttl <= 0)
            {
                return now_ms;
            }
            const std::uint64_t spread = ((key * 0x9E3779B97F4A7C15ull) >> 32) % static_cast<std::uint64_t>(ttl);
            return now_ms - static_cast<std::int64_t>(spread);
        }

        /** @brief A cached shop answer, looked up again once stale. */
        template <typename Lookup>
        [[nodiscard]] std::optional<bool>
        cached(std::unordered_map<EntityId, CachedAnswer> &cache, EntityId id, std::int64_t now_ms, Lookup lookup)
        {
            if (cache.size() >= MAX_CACHED_ANSWERS && !cache.contains(id))
            {
                cache.clear();
            }
            CachedAnswer &answer = cache[id];
            const std::int64_t ttl = SHOP_CACHE_MS + static_cast<std::int64_t>(id % 1024);
            if (answer.at_ms == 0 || now_ms - answer.at_ms >= ttl)
            {
                answer.value = lookup();
                answer.at_ms = answer_stamp(answer.at_ms == 0, id, now_ms, ttl);
            }
            return answer.value;
        }

        /** @brief Whether the inventory holds nothing the player can take. */
        [[nodiscard]] Fill player_visible_fill(std::uintptr_t inventory) noexcept
        {
            const std::optional<bool> empty = inventory_is_empty_for_player(inventory);
            if (!empty.has_value())
            {
                return Fill::Unknown;
            }
            return *empty ? Fill::Empty : Fill::HasItems;
        }

        /** @brief Whether the inventory holds any item at all. */
        [[nodiscard]] Fill any_fill(std::uintptr_t inventory) noexcept
        {
            const std::optional<std::size_t> count = inventory != 0 ? inventory_item_count(inventory) : std::nullopt;
            if (!count.has_value())
            {
                return Fill::Unknown;
            }
            return *count != 0 ? Fill::HasItems : Fill::Empty;
        }

        /** @brief Flags an emptiness verdict (a highlight group decides whether an empty object shows). */
        void apply_fill(Fill fill, std::uint32_t &flags) noexcept
        {
            if (fill == Fill::Empty)
            {
                flags |= bit(LootFlag::Empty);
            }
            else if (fill == Fill::Unknown)
            {
                flags |= bit(LootFlag::EmptyUnknown);
            }
        }

        /** @brief The player may loot the body (C_Actor::CanLoot, the rule the loot action checks). */
        [[nodiscard]] Verdict can_loot_verdict(
            const ScanContext &ctx,
            EntityId id,
            LootCategory category,
            std::uint32_t flags,
            bool &allowed
        ) noexcept
        {
            const std::optional<bool> can = actor_can_loot(ctx.player_actor, id);
            allowed = can.value_or(false);
            if (allowed)
            {
                return {};
            }
            return reject(category, flags, can.has_value() ? "cannot_loot" : "cannot_loot(unknown)");
        }

        // Actors

        [[nodiscard]] Verdict classify_human_corpse(const ScanContext &ctx, const ActorView &view, std::uint32_t flags)
        {
            bool allowed = false;
            if (Verdict denied = can_loot_verdict(ctx, view.id, LootCategory::HumanCorpse, flags, allowed); !allowed)
            {
                return denied;
            }
            // The loot window lists only the items the player may see, so that is what counts; an inventory the
            // test cannot read falls back to "any item".
            const std::uintptr_t inventory = soul_inventory(view.soul);
            Fill fill = inventory != 0 ? player_visible_fill(inventory) : Fill::Unknown;
            if (fill == Fill::Unknown)
            {
                fill = any_fill(inventory);
            }
            apply_fill(fill, flags);
            return accept(LootCategory::HumanCorpse, flags, "corpse");
        }

        [[nodiscard]] Verdict classify_carcass(const ScanContext &ctx, const ActorView &view, std::uint32_t flags)
        {
            if (view.soul != 0 && soul_has_script_context(view.soul, ANIMAL_NO_LOOT_CONTEXT).value_or(false))
            {
                return reject(LootCategory::AnimalCarcass, flags, "loot_disabled_by_design");
            }
            // The butcher action is offered whatever the carcass still holds.
            if (view.soul != 0 && soul_can_be_butchered(view.soul).value_or(false))
            {
                return accept(LootCategory::AnimalCarcass, flags | bit(LootFlag::Butcherable), "butcherable");
            }
            bool allowed = false;
            if (Verdict denied = can_loot_verdict(ctx, view.id, LootCategory::AnimalCarcass, flags, allowed); !allowed)
            {
                return denied;
            }
            apply_fill(any_fill(soul_inventory(view.soul)), flags);
            return accept(LootCategory::AnimalCarcass, flags, "carcass_loot");
        }

        [[nodiscard]] LootCategory live_category(ActorKind kind) noexcept
        {
            switch (kind)
            {
            case ActorKind::Human:
                return LootCategory::LiveNpc;
            case ActorKind::Horse:
                return LootCategory::Horse;
            case ActorKind::Dog:
                return LootCategory::Dog;
            default:
                return LootCategory::LiveAnimal;
            }
        }

        [[nodiscard]] bool live_enabled(const LootScanOptions &options, LootCategory category) noexcept
        {
            switch (category)
            {
            case LootCategory::LiveNpc:
                return options.live_npcs;
            case LootCategory::Horse:
                return options.horses;
            case LootCategory::Dog:
                return options.dogs;
            default:
                return options.live_animals;
            }
        }

        /**
         * @struct ActorCondition
         * @brief Whether an actor is dead, and whether a living one is unconscious.
         */
        struct ActorCondition
        {
            bool dead{false};
            bool unconscious{false};
        };

        [[nodiscard]] ActorCondition condition_of(const ActorView &view) noexcept
        {
            const bool human = view.kind == ActorKind::Human;
            // Animals count as dead at zero health as well (BasicAnimal); humans only through IsDead.
            ActorCondition condition{.dead = actor_is_dead(view.actor).value_or(false)};
            if (!condition.dead && !human)
            {
                const std::optional<float> health = actor_health(view.actor);
                condition.dead = health.has_value() && *health <= 0.0f;
            }
            if (!condition.dead)
            {
                condition.unconscious = view.soul != 0 && soul_is_unconscious(view.soul).value_or(false);
            }
            return condition;
        }

        /** @brief A living actor (an unconscious one tagged Unconscious) as an NPC, animal, horse or dog. */
        [[nodiscard]] Verdict classify_living(const ScanContext &ctx, const ActorView &view, bool unconscious)
        {
            const LootScanOptions &options = ctx.options;
            const bool human = view.kind == ActorKind::Human;
            const LootCategory category = live_category(view.kind);
            if (!live_enabled(options, category))
            {
                return reject(category, 0, unconscious ? "unconscious" : "alive", true);
            }
            if (entity_is_hidden(view.entity))
            {
                return reject(category, 0, "hidden", true);
            }
            if (actor_despawned(view.actor))
            {
                return reject(category, 0, "despawned");
            }
            // The town crowd keeps NPCs without a render proxy (kkut_additive_*): nothing to outline, and their bounds
            // are empty, so no bracket either; kept, each cost a GetWorldBounds call every frame for nothing.
            if (render_node_of(view.entity) == 0)
            {
                return reject(category, 0, "not_rendered", true);
            }
            std::uint32_t flags = unconscious ? bit(LootFlag::Unconscious) : 0u;
            if (human && view.soul != 0 && soul_is_public_enemy(view.soul).value_or(false))
            {
                flags |= bit(LootFlag::Hostile);
            }
            return accept(category, flags, "alive");
        }

        /**
         * @brief A body under the loot rules: a dead actor, or an unconscious human (the game lets the player loot one,
         *        and the Unconscious tag tells it apart).
         */
        [[nodiscard]] Verdict classify_body(const ScanContext &ctx, const ActorView &view, bool unconscious)
        {
            const LootScanOptions &options = ctx.options;
            const bool human = view.kind == ActorKind::Human;
            std::uint32_t flags = unconscious ? bit(LootFlag::Unconscious) : 0u;
            const LootCategory category = human ? LootCategory::HumanCorpse : LootCategory::AnimalCarcass;
            if (!(human ? options.human_corpses : options.animals))
            {
                return reject(category, flags, "category_disabled", true);
            }
            if (entity_is_hidden(view.entity))
            {
                return reject(category, flags, "hidden");
            }
            // A body the game disposed of keeps its entity, invisible like one asleep out of view: no bracket for it.
            if (actor_despawned(view.actor))
            {
                return reject(category, flags, "despawned");
            }
            if (render_node_of(view.entity) == 0)
            {
                return reject(category, flags, "not_rendered", true);
            }
            if (view.soul != 0 && soul_is_legal_to_loot(view.soul) == std::optional<bool>{false})
            {
                flags |= bit(LootFlag::Illegal);
            }
            return human ? classify_human_corpse(ctx, view, flags) : classify_carcass(ctx, view, flags);
        }

        // World items

        /** @brief Logs, once per owner class, an item whose owner is neither an inventory nor a world slot. */
        void log_unknown_owner(const ItemView &item, std::uintptr_t entity)
        {
            const std::uintptr_t owner = item_owner(item);
            std::uintptr_t key = 0;
            if (owner != 0)
            {
                const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{owner});
                key = vtable ? *vtable : 0;
            }
            if (!s_unknown_owner_types.insert(key).second)
            {
                return;
            }
            (void)DMK::log().try_log(
                DMK::LogLevel::Info,
                "LootScan: item {:#x} '{}' is held by an owner with unknown vtable {:#x} ({:#x}); items held by "
                "that type are skipped",
                item.id,
                entity_name(entity),
                key,
                owner
            );
        }

        [[nodiscard]] Verdict classify_item(const ScanContext &ctx, const ItemView &item, std::uintptr_t entity)
        {
            constexpr LootCategory item_category = LootCategory::Item;
            if (entity_is_hidden(entity))
            {
                return reject(item_category, 0, "hidden");
            }
            // Worn, wielded, carried or on a body: an inventory holds it.
            switch (item_holder(item))
            {
            case ItemHolder::Inventory:
                return reject(item_category, 0, "carried", true);
            case ItemHolder::Borrowed:
                return reject(item_category, 0, "held", true);
            case ItemHolder::Attached:
                return reject(item_category, 0, "attached", true);
            case ItemHolder::Unknown:
                log_unknown_owner(item, entity);
                return reject(item_category, 0, "holder_unknown");
            default:
                break;
            }
            if (item_is_npc_only(item).value_or(false))
            {
                return reject(item_category, 0, "npc_only");
            }
            const std::optional<bool> in_use = item_in_use(item);
            if (in_use.value_or(true))
            {
                return reject(item_category, 0, in_use.has_value() ? "in_use" : "state_unknown");
            }
            if (!item_is_pickable(item).value_or(false))
            {
                return reject(item_category, 0, "cannot_use");
            }
            // Shop display goods are loot the game offers to steal (PickableItem never checks IsFromShop), so they
            // are kept and flagged.
            std::uint32_t flags = 0;
            const bool steal =
                item_can_steal(item, ctx.player_id).value_or(false) ||
                cached(s_shop_items, item.id, ctx.now_ms, [&item] { return item_is_from_shop(item); }).value_or(false);
            if (steal)
            {
                flags |= bit(LootFlag::Illegal);
            }
            return accept(item_category, flags, "item");
        }

        // Containers and custom classes

        /** @brief The inventory the container opens: its stash's master inventory, else its own. */
        [[nodiscard]] std::uintptr_t container_inventory(EntityId id, std::uintptr_t entity) noexcept
        {
            if (const std::uintptr_t stash = stash_of(id); stash != 0)
            {
                if (const std::uintptr_t inventory = stash_inventory(stash); inventory != 0)
                {
                    return inventory;
                }
            }
            // ShootableStashBase (Nest, DestroStash) opens the inventory its script holds.
            const std::optional<Wuid> wuid = script_handle(entity, "inventoryId");
            return wuid.has_value() && *wuid != 0 ? inventory_from_wuid(*wuid) : 0;
        }

        /**
         * @brief Stash:UsesStealUiPrompt: opening is a crime when the inventory has an owner who is not the player
         *        and not a public enemy.
         */
        [[nodiscard]] bool uses_steal_prompt(const ScanContext &ctx, std::uintptr_t inventory) noexcept
        {
            const std::optional<Wuid> owner = inventory_owner(inventory);
            if (!owner.has_value() || *owner == 0 || *owner == ctx.player_soul_wuid)
            {
                return false;
            }
            const std::uintptr_t soul = soul_from_wuid(*owner);
            if (soul != 0 && soul == ctx.player_soul)
            {
                return false;
            }
            // RPG.IsPublicEnemy answers nil for an owner it cannot resolve, which the prompt test reads as "not an
            // enemy".
            return soul == 0 || !soul_is_public_enemy(soul).value_or(false);
        }

        [[nodiscard]] Verdict classify_container(const ScanContext &ctx, const IndexEntry &entry, std::uintptr_t entity)
        {
            constexpr LootCategory container = LootCategory::Container;
            const LootScanOptions &options = ctx.options;
            if (!options.containers)
            {
                return reject(container, 0, "category_disabled", true);
            }
            if (entity_is_hidden(entity))
            {
                return reject(container, 0, "hidden");
            }
            std::uint32_t flags = entry.kind == IndexKind::StashCorpse ? bit(LootFlag::StashCorpse) : 0u;
            if (entry.kind == IndexKind::DestroStash && script_number(entity, "shot").value_or(0.0f) != 0.0f)
            {
                return reject(container, flags, "destroyed");
            }
            if (script_bool(entity, "interactive") == std::optional<bool>{false})
            {
                return reject(container, flags, "not_interactive");
            }
            const std::uintptr_t inventory = container_inventory(entry.id, entity);
            if (inventory == 0)
            {
                return reject(container, flags, "no_inventory");
            }
            const bool locked = script_bool(entity, "bLocked").value_or(false);
            if (locked)
            {
                flags |= bit(LootFlag::Locked);
            }
            else if (inventory_is_usable(inventory) == std::optional<bool>{false})
            {
                return reject(container, flags, "inventory_unusable");
            }
            if (uses_steal_prompt(ctx, inventory))
            {
                flags |= bit(LootFlag::Illegal);
            }
            apply_fill(player_visible_fill(inventory), flags);
            return accept(container, flags, locked ? "container_locked" : "container");
        }

        [[nodiscard]] Verdict classify_critter(std::uintptr_t entity)
        {
            if (entity_is_hidden(entity))
            {
                return reject(LootCategory::Critter, 0, "hidden", true);
            }
            return accept(LootCategory::Critter, 0, "alive");
        }

        [[nodiscard]] Verdict classify_custom(const IndexEntry &entry, std::uintptr_t entity)
        {
            constexpr LootCategory custom = LootCategory::Custom;
            if (entity_is_hidden(entity))
            {
                return reject(custom, 0, "hidden");
            }
            if (const std::optional<ItemView> item = item_of(entry.id); item.has_value())
            {
                const ItemHolder holder = item_holder(*item);
                if (holder == ItemHolder::Inventory || holder == ItemHolder::Borrowed || holder == ItemHolder::Attached)
                {
                    return reject(custom, 0, "carried");
                }
            }
            return accept(custom, 0, "pattern");
        }

        // Scan

        [[nodiscard]] float
        distance_sq_between(const game_structures::Vec3f &a, const game_structures::Vec3f &b) noexcept
        {
            const float dx = a.x - b.x;
            const float dy = a.y - b.y;
            const float dz = a.z - b.z;
            return dx * dx + dy * dy + dz * dz;
        }

        /**
         * @brief The pick a tick without a scan should run, and why.
         */
        struct PickChoice
        {
            /// The pick, or std::nullopt when none is due.
            std::optional<Pick> pick{};
            /**
             * @brief The next scan must wait for it (never taken, taken for another reach, over a replaced index, or
             * overdue while every tick had a scan due).
             */
            bool blocking{false};
            const char *reason{""};
        };

        /**
         * @brief Decides which near pick is due: a blocking one first, then one the player's move or a change of its
         *        source asks for, then the stalest one whose clock ran out.
         * @param blocking_only Look for a pick that holds the scan only (a tick with a scan due): a blocking one, or
         *        one overdue by PICK_OVERDUE_FACTOR (the scans left it no tick of its own).
         */
        [[nodiscard]] PickChoice choose_pick(const LootScanOptions &options, std::int64_t now_ms, bool blocking_only)
        {
            constexpr std::array<float, 3> move_limits{ACTOR_MOVE_LIMIT, OBJECT_MOVE_LIMIT, OBJECT_MOVE_LIMIT};
            constexpr auto overdue_factor = static_cast<float>(PICK_OVERDUE_FACTOR);
            PickChoice changed{};
            PickChoice timed{};
            PickChoice overdue{};
            std::int64_t timed_at = 0;
            for (std::size_t index = 0; index < s_near_picks.size(); ++index)
            {
                const auto pick = static_cast<Pick>(index);
                const NearPick &near = s_near_picks[index];
                if (near.at_ms == 0)
                {
                    return PickChoice{pick, true, "never taken"};
                }
                if (near.radius != options.radius)
                {
                    return PickChoice{pick, true, "reach changed"};
                }
                if (pick == PickIndex && near.source != s_index_generation)
                {
                    return PickChoice{pick, true, "index walked"};
                }
                const float moved_sq = distance_sq_between(options.origin, near.center);
                if (blocking_only)
                {
                    const float far = move_limits[index] * overdue_factor;
                    if (!overdue.pick.has_value())
                    {
                        if (moved_sq > far * far)
                        {
                            overdue = PickChoice{pick, true, "moved, overdue"};
                        }
                        else if (now_ms - near.at_ms >= PICK_REFRESH_MS[index] * PICK_OVERDUE_FACTOR)
                        {
                            overdue = PickChoice{pick, true, "clock, overdue"};
                        }
                    }
                    continue;
                }
                if (changed.pick.has_value())
                {
                    continue;
                }
                if (moved_sq > move_limits[index] * move_limits[index])
                {
                    changed = PickChoice{pick, false, "moved"};
                    continue;
                }
                if (pick == PickItems && item_map_size().value_or(near.source) != near.source)
                {
                    changed = PickChoice{pick, false, "item map changed"};
                    continue;
                }
                if (pick == PickIndex && near.source_extra != s_index_additions)
                {
                    changed = PickChoice{pick, false, "entities added"};
                    continue;
                }
                if (now_ms - near.at_ms >= PICK_REFRESH_MS[index] && (!timed.pick.has_value() || near.at_ms < timed_at))
                {
                    timed = PickChoice{pick, false, "clock"};
                    timed_at = near.at_ms;
                }
            }
            if (blocking_only)
            {
                return overdue;
            }
            return changed.pick.has_value() ? changed : timed;
        }

        /**
         * @brief Picks the actors, items or indexed entities within the radius plus a margin out of the level-wide
         *        maps; the index pick also drops the entries whose entity is gone.
         */
        void run_pick(Pick pick, const LootScanOptions &options, std::int64_t now_ms)
        {
            DMK_PROFILE_FUNCTION();
            NearPick taken{options.origin, options.radius, now_ms};
            if (pick == PickActors)
            {
                (void)collect_actors_near(
                    s_near_actors,
                    options.origin,
                    options.radius + ACTOR_MARGIN,
                    MAX_ACTORS,
                    &s_level_actors
                );
            }
            else if (pick == PickItems)
            {
                // The size is read before the walk: an item added during it is picked up by the next change.
                taken.source = item_map_size().value_or(0);
                (void)collect_items_near(
                    s_near_items,
                    options.origin,
                    options.radius + OBJECT_MARGIN,
                    MAX_ITEMS,
                    &s_level_items
                );
            }
            else
            {
                taken.source = s_index_generation;
                taken.source_extra = s_index_additions;
                s_near_index.clear();
                std::size_t kept = 0;
                std::size_t gone = 0;
                // The first few ids dropped, for the log line (the entity itself can no longer be read).
                constexpr std::size_t logged_gone = 8;
                std::array<EntityId, logged_gone> gone_ids{};
                for (std::size_t i = 0; i < s_index.size(); ++i)
                {
                    // Independent entities: loading a few ahead overlaps their misses.
                    if (i + PREFETCH_DISTANCE < s_index.size())
                    {
                        prefetch_entity(s_index[i + PREFETCH_DISTANCE].entity, true);
                    }
                    const IndexEntry &entry = s_index[i];
                    const EntityPresence presence = entity_presence(
                        entry.entity,
                        entry.id,
                        entry.klass,
                        options.origin,
                        options.radius + OBJECT_MARGIN
                    );
                    // Between two full walks the index only grows (recent walks add, nothing removes), so an entry
                    // whose entity is gone is dropped here.
                    if (presence == EntityPresence::Gone)
                    {
                        if (gone < logged_gone)
                        {
                            gone_ids[gone] = entry.id;
                        }
                        ++gone;
                        continue;
                    }
                    if (presence == EntityPresence::Near)
                    {
                        s_near_index.push_back(entry);
                    }
                    s_index[kept++] = entry;
                }
                s_index.resize(kept);
                if (gone != 0)
                {
                    s_index_ids_valid = false;
                }
                if (gone != 0 && DMK::log().is_enabled(DMK::LogLevel::Debug))
                {
                    std::string ids;
                    for (std::size_t i = 0; i < std::min(gone, logged_gone); ++i)
                    {
                        ids += std::format("{}{:#x}", ids.empty() ? "" : " ", gone_ids[i]);
                    }
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Debug,
                        "LootScan: {} index entr{} dropped, the entity is gone (deleted or its slot reused): {}{}",
                        gone,
                        gone == 1 ? "y" : "ies",
                        ids,
                        gone > logged_gone ? " ..." : ""
                    );
                }
            }
            s_near_picks[pick] = taken;
        }

        // The end of this scan's classification budget (steady microseconds), and whether the budget left a new object
        // for the next scan.
        std::int64_t s_classify_deadline_us = 0;
        bool s_classify_deferred = false;

        /**
         * @brief The verdict of a static object (item, container, custom class): the remembered one while it is fresh
         *        and the entity is still shown, else a new one from @p classify.
         * @details Past the scan's classification budget an expired verdict is kept until a later scan, and a new
         * object
         *          gets none yet (std::nullopt) and asks for a scan next tick, so it shows a frame or two late.
         */
        template <typename Classify>
        [[nodiscard]] std::optional<Verdict>
        remembered(std::uint64_t key, std::uintptr_t entity, std::int64_t now_ms, Classify classify)
        {
            if (s_verdicts.size() >= MAX_CACHED_ANSWERS && !s_verdicts.contains(key))
            {
                s_verdicts.clear();
            }
            CachedVerdict &cached_verdict = s_verdicts[key];
            const bool first = cached_verdict.at_ms == 0;
            const std::int64_t base_ttl =
                (key & ~0xFFFFFFFFull) == ROLE_CONTAINER ? CONTAINER_VERDICT_TTL_MS : VERDICT_TTL_MS;
            const std::int64_t ttl = base_ttl + static_cast<std::int64_t>((key & 0xFFFFFFFFu) % STAGGER_MS);
            const bool hidden = entity_is_hidden(entity);
            if (first || now_ms - cached_verdict.at_ms >= ttl || hidden)
            {
                // A hidden object is rejected at once whatever the budget (the test is one read).
                if (!hidden && steady_us() >= s_classify_deadline_us)
                {
                    if (!first)
                    {
                        return cached_verdict.verdict;
                    }
                    s_classify_deferred = true;
                    return std::nullopt;
                }
                cached_verdict.verdict = classify();
                cached_verdict.at_ms = answer_stamp(first, key, now_ms, ttl);
            }
            return cached_verdict.verdict;
        }

        /** @brief The options a remembered verdict depends on, packed for a change test. */
        [[nodiscard]] std::uint32_t verdict_options(const LootScanOptions &options) noexcept
        {
            return (options.items ? 1u : 0u) | (options.containers ? 2u : 0u);
        }

        /** @brief The distance from the scan origin to the entity, or std::nullopt beyond the radius. */
        [[nodiscard]] std::optional<float> distance_to(const LootScanOptions &options, std::uintptr_t entity) noexcept
        {
            const std::optional<game_structures::Vec3f> position = entity_world_position(entity);
            if (!position.has_value())
            {
                return std::nullopt;
            }
            const float distance_sq = distance_sq_between(*position, options.origin);
            if (!(distance_sq <= options.radius * options.radius))
            {
                return std::nullopt;
            }
            return std::sqrt(distance_sq);
        }

        /** @brief Logs a verdict and keeps an accepted object. */
        void consider(EntityId id, std::uintptr_t entity, float distance, const Verdict &verdict)
        {
            log_verdict(id, entity, distance, verdict);
            if (verdict.accepted)
            {
                s_found.push_back(
                    Found{
                        distance,
                        HighlightRequest{
                            .entity_id = id,
                            .category = verdict.category,
                            .flags = verdict.flags,
                            .distance = distance,
                        }
                    }
                );
            }
        }

        /** @brief consider() for a remembered verdict, which the classification budget may have left for later. */
        void consider(EntityId id, std::uintptr_t entity, float distance, const std::optional<Verdict> &verdict)
        {
            if (verdict.has_value())
            {
                consider(id, entity, distance, *verdict);
            }
        }
    } // namespace

    DMK::Result<void> initialize_loot_scanner()
    {
        s_class_kinds.clear();
        s_kind_memo.clear();
        s_class_patterns.clear();
        s_pattern_memo.clear();
        s_patterns = GroupPatterns{};
        drop_index();
        s_shop_items.clear();
        s_logged.clear();
        s_unknown_owner_types.clear();
        s_verdicts.clear();
        s_last_stats = LootScanStats{};
        s_gate = CounterGate{};
        s_work = LootWorkStats{};
        if (!entity_access_available())
        {
            return std::unexpected(DMK::Error{DMK::ErrorCode::NoMatch, "loot_scanner/entity_access"});
        }
        s_available.store(true, std::memory_order_release);
        return {};
    }

    void shutdown_loot_scanner() noexcept
    {
        s_available.store(false, std::memory_order_release);
        if (s_walk.active())
        {
            s_walk.cancel();
        }
    }

    void reset_loot_scanner() noexcept
    {
        try
        {
            drop_index();
            s_class_kinds.clear();
            s_kind_memo.clear();
            s_shop_items.clear();
            s_logged.clear();
            s_unknown_owner_types.clear();
            s_verdicts.clear();
        }
        catch (...)
        {
            (void)DMK::log().log_noexcept(DMK::LogLevel::Error, "LootScan: reset failed");
        }
    }

    bool
    advance_loot_index(bool active, const GroupPatterns &patterns, const std::optional<game_structures::Vec3f> &center)
    {
        DMK_PROFILE_FUNCTION();
        if (!s_available.load(std::memory_order_acquire))
        {
            return false;
        }
        if (patterns.signature != s_patterns.signature)
        {
            s_patterns = patterns;
            s_class_patterns.clear();
            s_pattern_memo.clear();
            drop_index();
            (void)DMK::log()
                .try_log(DMK::LogLevel::Info, "LootScan: patterns [{}]; the index is rebuilt", s_patterns.signature);
        }
        const std::int64_t start_us = steady_us();
        if (!s_walk.active())
        {
            // Name and Model tests cover only the area around the walk centre, so moving away walks again sooner.
            bool moved = false;
            if (center.has_value() && s_index_valid && (!s_patterns.names.empty() || !s_patterns.models.empty()))
            {
                const float dx = center->x - s_index_center.x;
                const float dy = center->y - s_index_center.y;
                moved = dx * dx + dy * dy > (PATTERN_MARGIN * 0.5f) * (PATTERN_MARGIN * 0.5f);
            }
            const char *reason = full_walk_reason(active, moved, start_us / 1000);
            if (reason == nullptr)
            {
                return false;
            }
            s_walk_center = center.value_or(s_walk_center);
            if (!s_walk.begin())
            {
                return false;
            }
            s_walk_reason = reason;
            // The walk covers what the counters count now; the overlap walks the inserts in flight once more.
            s_gate.walk_claim_valid = false;
            if (s_gate.trusted)
            {
                if (const std::optional<EntityCounters> counters = read_entity_counters(); counters.has_value())
                {
                    s_gate.walk_read = *counters;
                    s_gate.walk_claim = EntityCounters{
                        counters->inserts > CLAIM_OVERLAP ? counters->inserts - CLAIM_OVERLAP : 0,
                        counters->deletes
                    };
                    s_gate.walk_claim_valid = true;
                    s_gate.last = *counters;
                    s_gate.last_valid = true;
                }
            }
            s_pending.clear();
            s_batch.clear();
            s_walk_entities = 0;
            s_walk_frames = 0;
            s_walk_busy_us = 0;
            s_walk_started_ms = start_us / 1000;
        }
        ++s_walk_frames;
        bool finished = false;
        for (;;)
        {
            const EntityWalk::Step step = s_walk.step(s_batch, WALK_BATCH);
            process_batch();
            if (step != EntityWalk::Step::More)
            {
                finish_walk(step == EntityWalk::Step::Done);
                finished = true;
                break;
            }
            if (s_walk_entities >= MAX_WALK_ENTITIES)
            {
                s_walk.cancel();
                finish_walk(false);
                finished = true;
                break;
            }
            if (steady_us() - start_us >= WALK_BUDGET_US)
            {
                break;
            }
        }
        s_walk_busy_us += steady_us() - start_us;
        return finished;
    }

    LootScanStats run_loot_scan(const LootScanOptions &options, std::vector<HighlightRequest> &out)
    {
        DMK_PROFILE_FUNCTION();
        const std::int64_t start_us = steady_us();
        out.clear();
        s_found.clear();
        LootScanStats stats{};
        stats.index_ready = s_index_valid;
        stats.indexed = s_index.size();
        if (!s_available.load(std::memory_order_acquire))
        {
            s_last_stats = stats;
            return stats;
        }

        ScanContext ctx{options};
        ctx.now_ms = start_us / 1000;
        ctx.player_actor = resolve_c_player();
        const std::uintptr_t player = ctx.player_actor != 0 ? player_entity(ctx.player_actor) : 0;
        ctx.player_id = player != 0 ? entity_id_of(player) : 0;
        ctx.player_soul = actor_soul(ctx.player_actor);
        ctx.player_soul_wuid = soul_wuid(ctx.player_soul);

        // The near sets come from advance_loot_picks(), in ticks without a scan.
        const std::int64_t picked_us = steady_us();
        s_classify_deadline_us = picked_us + CLASSIFY_BUDGET_US;
        s_classify_deferred = false;
        const EntityLookup entities;
        if (const std::uint32_t packed = verdict_options(options); packed != s_verdict_options)
        {
            s_verdict_options = packed;
            s_verdicts.clear();
        }
        stats.actors = s_level_actors;
        stats.items = s_level_items;

        const bool any_actor = options.human_corpses || options.animals || options.live_npcs || options.live_animals ||
                               options.horses || options.dogs;
        if (any_actor)
        {
            for (const ActorView &view : s_near_actors)
            {
                // An actor removed since the pick no longer resolves to the same object.
                if (view.actor == ctx.player_actor || actor_of(view.id) != view.actor)
                {
                    continue;
                }
                const std::optional<float> distance = distance_to(options, view.entity);
                if (!distance.has_value())
                {
                    continue;
                }
                ++stats.candidates;
                // An unconscious human is both a living NPC and a lootable body; each is its own record, and the
                // highlight groups decide which one shows.
                const ActorCondition condition = condition_of(view);
                if (condition.dead)
                {
                    consider(view.id, view.entity, *distance, classify_body(ctx, view, false));
                    continue;
                }
                consider(view.id, view.entity, *distance, classify_living(ctx, view, condition.unconscious));
                if (condition.unconscious && view.kind == ActorKind::Human)
                {
                    consider(view.id, view.entity, *distance, classify_body(ctx, view, true));
                }
            }
        }

        const std::int64_t actors_us = steady_us();
        stats.actor_ms = static_cast<double>(actors_us - picked_us) / 1000.0;
        if (options.items)
        {
            for (const ItemView &item : s_near_items)
            {
                // The id still names a live entity, and the item picked with it is still that entity's.
                const std::uintptr_t entity = entities.find(item.id);
                if (!item_still_on(item, entity))
                {
                    continue;
                }
                const std::optional<float> distance = distance_to(options, entity);
                if (!distance.has_value())
                {
                    continue;
                }
                ++stats.candidates;
                consider(
                    item.id,
                    entity,
                    *distance,
                    remembered(item.id, entity, ctx.now_ms, [&] { return classify_item(ctx, item, entity); })
                );
            }
        }

        const std::int64_t items_us = steady_us();
        stats.item_ms = static_cast<double>(items_us - actors_us) / 1000.0;
        for (const IndexEntry &entry : s_near_index)
        {
            const bool container = entry.kind != IndexKind::None && entry.kind != IndexKind::Critter;
            const bool wanted = entry.pattern || (container && options.containers) ||
                                (entry.kind == IndexKind::Critter && options.critters);
            if (!wanted)
            {
                continue;
            }
            // The index is up to a walk old: an entity removed since, or an id reused by another class, is skipped.
            const std::uintptr_t entity = entities.find(entry.id);
            if (entity == 0 || class_of(entity) != entry.klass)
            {
                continue;
            }
            const std::optional<float> distance = distance_to(options, entity);
            if (!distance.has_value())
            {
                continue;
            }
            ++stats.candidates;
            // One entity may be a container (or a critter) and a pattern match at once; each role is its own record
            // and the highlight groups decide which one shows.
            if (entry.pattern)
            {
                consider(
                    entry.id,
                    entity,
                    *distance,
                    remembered(
                        static_cast<std::uint64_t>(entry.id) | ROLE_PATTERN,
                        entity,
                        ctx.now_ms,
                        [&] { return classify_custom(entry, entity); }
                    )
                );
            }
            if (container && options.containers)
            {
                consider(
                    entry.id,
                    entity,
                    *distance,
                    remembered(
                        static_cast<std::uint64_t>(entry.id) | ROLE_CONTAINER,
                        entity,
                        ctx.now_ms,
                        [&] { return classify_container(ctx, entry, entity); }
                    )
                );
            }
            if (entry.kind == IndexKind::Critter && options.critters)
            {
                consider(entry.id, entity, *distance, classify_critter(entity));
            }
        }

        stats.index_ms = static_cast<double>(steady_us() - items_us) / 1000.0;
        std::sort(
            s_found.begin(),
            s_found.end(),
            [](const Found &a, const Found &b) { return a.distance < b.distance; }
        );
        out.reserve(s_found.size());
        for (const Found &found : s_found)
        {
            out.push_back(found.request);
        }
        stats.records = out.size();
        stats.more = s_classify_deferred;
        stats.milliseconds = static_cast<double>(steady_us() - start_us) / 1000.0;
        s_last_stats = stats;
        return stats;
    }

    bool advance_loot_picks(const LootScanOptions &options, bool blocking_only)
    {
        DMK_PROFILE_FUNCTION();
        if (!s_available.load(std::memory_order_acquire))
        {
            return false;
        }
        const std::int64_t start_us = steady_us();
        const PickChoice choice = choose_pick(options, start_us / 1000, blocking_only);
        if (!choice.pick.has_value() || (blocking_only && !choice.blocking))
        {
            return false;
        }
        run_pick(*choice.pick, options, start_us / 1000);
        const double ms = static_cast<double>(steady_us() - start_us) / 1000.0;
        ++s_work.picks;
        s_work.pick_ms += ms;
        s_work.pick_ms_max = std::max(s_work.pick_ms_max, ms);
        constexpr std::array<const char *, 3> pick_names{"actors", "items", "index"};
        (void)DMK::log().try_log(
            DMK::LogLevel::Trace,
            "LootScan: picked {} ({}{}) in {:.2f} ms: {} of {} actors, {} of {} items, {} of {} indexed",
            pick_names[*choice.pick],
            choice.reason,
            choice.blocking ? ", the scan waited" : "",
            ms,
            s_near_actors.size(),
            s_level_actors,
            s_near_items.size(),
            s_level_items,
            s_near_index.size(),
            s_index.size()
        );
        return true;
    }

    void flush_loot_verdicts(const char *reason) noexcept
    {
        const std::size_t dropped = s_verdicts.size();
        s_verdicts.clear();
        if (dropped != 0)
        {
            (void)DMK::log()
                .try_log(DMK::LogLevel::Debug, "LootScan: {} remembered verdict(s) dropped ({})", dropped, reason);
        }
    }

    LootWorkStats take_loot_work_stats() noexcept
    {
        const LootWorkStats taken = s_work;
        s_work = LootWorkStats{};
        return taken;
    }

    void collect_model_brushes(
        const game_structures::Vec3f &center,
        float radius,
        std::span<const std::string> patterns,
        std::span<const ModelBrush> keep,
        std::vector<ModelBrush> &out
    )
    {
        DMK_PROFILE_FUNCTION();
        out.clear();
        if (patterns.empty() || !(radius > 0.0f))
        {
            return;
        }
        const game_structures::Aabb box{
            {center.x - radius, center.y - radius, center.z - radius},
            {center.x + radius, center.y + radius, center.z + radius}
        };
        // Only static brushes carry a model path of their own, so the octree skips the vegetation filling the box.
        constexpr std::array<std::uint32_t, 2> brush_types{
            constants::RENDERNODE_TYPE_BRUSH,
            constants::RENDERNODE_TYPE_MOVABLE_BRUSH
        };
        std::vector<std::uintptr_t> nodes;
        if (const OctreeQueryResult result = query_render_nodes_of_types(box, brush_types, nodes, MAX_MODEL_NODES);
            result != OctreeQueryResult::Ok)
        {
            nodes.clear();
            if (result == OctreeQueryResult::TooMany && !std::exchange(s_model_overflow_logged, true))
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Warning,
                    "LootScanner: more than {} static brushes within {:.1f} m; Model patterns skipped this scan",
                    MAX_MODEL_NODES,
                    radius
                );
            }
        }
        else
        {
            s_model_overflow_logged = false;
        }
        // A highlighted brush leaves the octree (it is moved to the always-visible list), so the last hits are
        // offered again. A brush that does not move must keep its bounds; a movable brush may move, and the pattern
        // test below reads its model again anyway.
        for (const ModelBrush &hit : keep)
        {
            const bool same = is_movable_brush_node(hit.brush) || brush_matches(hit.brush, hit.bounds);
            if (same && std::find(nodes.begin(), nodes.end(), hit.brush) == nodes.end())
            {
                nodes.push_back(hit.brush);
            }
        }
        const float radius_sq = radius * radius;
        for (const std::uintptr_t node : nodes)
        {
            if (!(object_is(GameClass::Brush, node) || object_is(GameClass::OwnedBrush, node) ||
                  object_is(GameClass::MovableBrush, node)))
            {
                continue;
            }
            const std::optional<game_structures::Aabb> bounds = render_node_bounds(node);
            if (!bounds.has_value())
            {
                continue;
            }
            const game_structures::Aabb &b = *bounds;
            if (!(b.max.x - b.min.x <= MAX_MODEL_EXTENT && b.max.y - b.min.y <= MAX_MODEL_EXTENT &&
                  b.max.z - b.min.z <= MAX_MODEL_EXTENT))
            {
                continue;
            }
            // The nearest point of the bounds, so a long object counts from its near end.
            const float dx = std::max({0.0f, b.min.x - center.x, center.x - b.max.x});
            const float dy = std::max({0.0f, b.min.y - center.y, center.y - b.max.y});
            const float dz = std::max({0.0f, b.min.z - center.z, center.z - b.max.z});
            const float distance_sq = dx * dx + dy * dy + dz * dz;
            if (distance_sq > radius_sq)
            {
                continue;
            }
            const std::string path = render_node_name(node);
            if (!path.empty() && std::any_of(
                                     patterns.begin(),
                                     patterns.end(),
                                     [&path](const std::string &pattern) { return wildcard_match(pattern, path); }
                                 ))
            {
                out.push_back(ModelBrush{node, b, std::sqrt(distance_sq)});
            }
        }
        std::sort(
            out.begin(),
            out.end(),
            [](const ModelBrush &a, const ModelBrush &b) { return a.distance < b.distance; }
        );
    }

    void log_loot_scanner_state()
    {
        const LootScanStats &stats = s_last_stats;
        std::string counters = "not claimed";
        if (!s_gate.trusted)
        {
            counters = std::format("off ({})", s_gate.distrust_reason);
        }
        else if (s_gate.claimed_valid)
        {
            counters = std::format(
                "covering inserts={} deletes={}, resets={}",
                s_gate.claimed.inserts,
                s_gate.claimed.deletes,
                s_gate.resets
            );
        }
        (void)DMK::log().try_log(
            DMK::LogLevel::Info,
            "LootScan: index {} ({} entities, {} ms old, walk {}, entity counters {}), classes known={}, "
            "patterns=[{}]; last scan: {} actors, {} items, {} candidates in range, {} records, {:.2f} ms; shop "
            "cache items={}",
            s_index_valid ? "ready" : "not ready",
            s_index.size(),
            s_index_valid ? steady_us() / 1000 - s_index_done_ms : -1,
            s_walk.active() ? "running" : "idle",
            counters,
            s_class_kinds.size(),
            s_patterns.signature,
            stats.actors,
            stats.items,
            stats.candidates,
            stats.records,
            stats.milliseconds,
            s_shop_items.size()
        );
    }

} // namespace HenrySenses
