/**
 * @file engine/interactables.cpp
 * @brief Native detection of the interactive objects around a point.
 */

#include "engine/interactables.hpp"
#include "constants.hpp"
#include "engine/engine_env.hpp"
#include "engine/entity_access.hpp"
#include "engine/game_natives.hpp"
#include "engine/prefab_templates.hpp"
#include "engine/visual_resolver.hpp"
#include "rtti_types.hpp"

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
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace HenrySenses
{
    namespace
    {
        // What a class's entities need beyond their kind.
        // A helper without a visible mesh of its own, placed on the object it belongs to (every *Trigger class and a
        // few others): its mesh is looked up.
        constexpr std::uint8_t TRAIT_HELPER = 0x01;
        // A use trigger that is a Seat when it seats the player on a chair or bench, else a UseSpot.
        constexpr std::uint8_t TRAIT_SEAT_OR_USE = 0x02;
        // Interactive only when its script says so (a book the player can read in place).
        constexpr std::uint8_t TRAIT_READABLE = 0x04;

        /**
         * @brief An entity class whose script offers the player an action, its display group, and what else its
         *        entities need.
         */
        struct InteractiveClass
        {
            std::string_view name;
            InteractKind kind;
            std::uint8_t traits{0};
        };

        // Every class below defines GetActions in its entity script (Scripts.pak, Scripts/Entities). Actors, stashes,
        // pickable items and carryable items are loot, handled by the loot scan. Chair is not listed: its script has
        // no actions, and the ActionTrigger on it seats the player.
        constexpr std::array INTERACTIVE_CLASSES{
            InteractiveClass{"AnimDoor", InteractKind::Door},
            InteractiveClass{"Smithery", InteractKind::Station},
            InteractiveClass{"Grindstone", InteractKind::Station},
            InteractiveClass{"AlchemyTable", InteractKind::Station},
            InteractiveClass{"TranscriptionTable", InteractKind::Station},
            InteractiveClass{"ForgeBuilderTrigger", InteractKind::Station, TRAIT_HELPER},
            InteractiveClass{"DiceInteractor", InteractKind::Station},
            InteractiveClass{"DiceMinigameCup", InteractKind::Station},
            InteractiveClass{"StoneThrowingPile", InteractKind::Station},
            // A book offers reading in place only with Properties.bIsDirectlyReadable; any other book is an item.
            InteractiveClass{"Book", InteractKind::Station, TRAIT_READABLE},
            InteractiveClass{"RecipesBook", InteractKind::Station},
            InteractiveClass{"Bed", InteractKind::Bed},
            InteractiveClass{"BedTrigger", InteractKind::Bed, TRAIT_HELPER},
            // Bench, chair and table seats are ActionTriggers too; which one a trigger is shows on what it sits on.
            InteractiveClass{"ActionTrigger", InteractKind::UseSpot, TRAIT_HELPER | TRAIT_SEAT_OR_USE},
            InteractiveClass{"WaterTubeActionTrigger", InteractKind::UseSpot, TRAIT_HELPER},
            InteractiveClass{"KettleActionTrigger", InteractKind::UseSpot, TRAIT_HELPER},
            InteractiveClass{"FoodProcessingTrigger", InteractKind::UseSpot, TRAIT_HELPER},
            InteractiveClass{"InteractionTrigger", InteractKind::UseSpot, TRAIT_HELPER},
            InteractiveClass{"SequenceTrigger", InteractKind::UseSpot, TRAIT_HELPER},
            InteractiveClass{"SmartObjectTrigger", InteractKind::UseSpot, TRAIT_HELPER},
            InteractiveClass{"IndulgenceBoxTrigger", InteractKind::UseSpot, TRAIT_HELPER},
            InteractiveClass{"Ladder", InteractKind::Other},
            InteractiveClass{"Lockpickable", InteractKind::Other, TRAIT_HELPER},
            InteractiveClass{"Hole", InteractKind::Other, TRAIT_HELPER},
            InteractiveClass{"CarryItemPile", InteractKind::Other},
            InteractiveClass{"WHCartMountPoint", InteractKind::Other, TRAIT_HELPER},
            InteractiveClass{"InteractiveObjectEx", InteractKind::Other},
            // An inscription to read: an invisible box on a grave, a sign or a cross.
            InteractiveClass{"CaptionObject", InteractKind::Other, TRAIT_HELPER},
        };

        // Words of the model files of each group's objects, so the mesh a trigger sits on is recognised by name
        // before size and position are weighed. They match whole words of the file name, plural included.
        constexpr std::array<std::string_view, 1> DOOR_WORDS{"door"};
        constexpr std::array<std::string_view, 7>
            STATION_WORDS{"anvil", "grindstone", "alchemy", "forge", "workbench", "smith", "table"};
        constexpr std::array<std::string_view, 1> BED_WORDS{"bed"};
        constexpr std::array<std::string_view, 5> SEAT_WORDS{"chair", "bench", "stool", "seat", "throne"};
        constexpr std::array<std::string_view, 11> USE_WORDS{
            "tub",
            "trough",
            "wash",
            "washing",
            "well",
            "kettle",
            "cauldron",
            "chair",
            "bench",
            "stool",
            "shrine"
        };
        constexpr std::array<std::string_view, 4> OTHER_WORDS{"pillory", "ladder", "grave", "pile"};
        // Trigger classes that belong to one kind of object, so only its words name their mesh. Wash spots: a tub,
        // trough, washing stone or well; a spot at a pond or a river has none of these and keeps its marker. Kettle
        // triggers: the kettle over a fireplace, or the tap of a camp's wine barrel, never the chair beside it. Use
        // triggers: the use words and every seat. Inscriptions: what carries one.
        constexpr std::string_view WATER_TRIGGER_CLASS = "WaterTubeActionTrigger";
        constexpr std::array<std::string_view, 5> WATER_WORDS{"tub", "trough", "wash", "washing", "well"};
        constexpr std::string_view KETTLE_TRIGGER_CLASS = "KettleActionTrigger";
        constexpr std::array<std::string_view, 3> KETTLE_WORDS{"kettle", "cauldron", "barrel"};
        constexpr std::string_view ACTION_TRIGGER_CLASS = "ActionTrigger";
        constexpr std::array<std::string_view, 13> ACTION_WORDS{
            "tub",
            "trough",
            "wash",
            "washing",
            "well",
            "kettle",
            "cauldron",
            "chair",
            "bench",
            "stool",
            "seat",
            "throne",
            "shrine"
        };
        constexpr std::string_view CAPTION_CLASS = "CaptionObject";
        constexpr std::array<std::string_view, 6>
            CAPTION_WORDS{"grave", "gravestone", "tombstone", "sign", "cross", "plaque"};
        // What a bed may be made of: a makeshift bed of furs on planks names no bed at all.
        constexpr std::string_view BED_TRIGGER_CLASS = "BedTrigger";
        constexpr std::array<std::string_view, 5> BEDDING_WORDS{"fur", "straw", "pillow", "blanket", "mattress"};
        // The smart object a seat trigger links to (a bed trigger links one too, and stays a Bed).
        constexpr std::string_view STANCE_CLASS = "StanceSmartObject";
        constexpr std::string_view CHAIR_CLASS = "Chair";

        [[nodiscard]] std::span<const std::string_view> kind_words(InteractKind kind) noexcept
        {
            switch (kind)
            {
            case InteractKind::Door:
                return DOOR_WORDS;
            case InteractKind::Station:
                return STATION_WORDS;
            case InteractKind::Bed:
                return BED_WORDS;
            case InteractKind::Seat:
                return SEAT_WORDS;
            case InteractKind::UseSpot:
                return USE_WORDS;
            case InteractKind::Other:
            default:
                return OTHER_WORDS;
            }
        }

        /** @brief The words of the model paths a trigger of @p klass may sit on. */
        [[nodiscard]] std::span<const std::string_view>
        trigger_words(std::string_view klass, InteractKind kind) noexcept
        {
            if (klass == WATER_TRIGGER_CLASS)
            {
                return WATER_WORDS;
            }
            if (klass == KETTLE_TRIGGER_CLASS)
            {
                return KETTLE_WORDS;
            }
            if (klass == ACTION_TRIGGER_CLASS)
            {
                return ACTION_WORDS;
            }
            if (klass == CAPTION_CLASS)
            {
                return CAPTION_WORDS;
            }
            return kind_words(kind);
        }

        // Main-thread time a frame gives the mesh lookups (advance_interactable_visuals), and the starting estimate
        // of one lookup's cost the budget is checked against before the first measurement.
        constexpr std::int64_t INITIAL_RESOLVE_COST_US = 400;
        // A template that places a trigger's object with nothing at its pose while the trigger is farther than this is
        // early: interiors stream their furniture in within about 10 m of the house. It is retried after
        // PENDING_RETRY_MS or once the player is PENDING_APPROACH closer.
        constexpr float PENDING_DISTANCE = 12.0f;
        constexpr std::int64_t PENDING_RETRY_MS = 1500;
        constexpr float PENDING_APPROACH = 3.0f;
        // Any other miss (no template, the search found nothing) is looked at again after a growing delay (its object
        // may still appear: a phase change, a script). A miss farther than PENDING_DISTANCE is also looked at once
        // more when the player comes that close, where the interior would have streamed in.
        constexpr std::array<std::int64_t, 3> MISS_RETRY_MS{10000, 30000, 120000};
        // The heuristic search considers outlined meshes within this reach of the trigger.
        constexpr float OUTLINED_REACH = 4.0f;
        constexpr float OUTLINED_HEIGHT = 3.0f;
        // A pose lookup considers outlined brushes this far around the pose.
        constexpr float OUTLINED_POSE_REACH = 0.5f;
        // A runtime prefab entity stands this close to its template pose (level instances measure under 11 cm).
        constexpr float RUNTIME_PREFAB_TOLERANCE = 0.15f;
        // An entity mesh stands this close to its template pose.
        constexpr float ENTITY_POSE_TOLERANCE = 0.03f;
        // Entity links read per trigger.
        constexpr int MAX_LINKS = 8;
        // The world is walked again this long after the last walk finished, for the objects spawned or removed since.
        // A walk keeps every interactive entity of the level with its position, so moving needs no new walk (a walk
        // every 3 s and every 6 m cost ~6 ms of main-thread time each, back to back while riding). A collect reads the
        // live position of the candidates whose walk position lies within WALK_MARGIN of the reach, which also
        // covers an object that moved since (a cart's mount point).
        constexpr std::int64_t REWALK_MS = 10000;
        constexpr float WALK_MARGIN = 8.0f;
        // Main-thread time one frame may spend walking, and the entities taken between two clock reads.
        constexpr std::int64_t WALK_BUDGET_US = 1000;
        constexpr std::size_t WALK_BATCH = 1024;
        // A walk longer than this many entities is cut short (the world holds about 100k).
        constexpr std::size_t MAX_WALK_ENTITIES = 400000;
        // Entities without usable geometry (script triggers) get a box of this half size around their position.
        constexpr float MIN_HALF_EXTENT = 0.15f;
        constexpr float DEFAULT_HALF_EXTENT = 0.35f;
        // Bounds larger than this on any axis (trigger areas, merged prefabs) are replaced by the default box.
        constexpr float MAX_EXTENT = 12.0f;
        constexpr std::size_t SURVEY_TOP = 60;

        /** @brief A class's display group (-1: not interactive) and traits. */
        struct ClassInfo
        {
            std::int8_t kind{-1};
            std::uint8_t traits{0};
        };

        /** @brief A cached interactive entity of the last walk. */
        struct Candidate
        {
            EntityId id{0};
            InteractKind kind{InteractKind::Other};
            std::uint8_t traits{0};
            // Where the walk saw it.
            game_structures::Vec3f position{};
        };

        /** @brief How a candidate's mesh was found. */
        enum class VisualSource : std::uint8_t
        {
            /// Its own render proxy.
            Own,
            /// At the pose its prefab template predicts.
            Template,
            /// By the search around it.
            Search,
            /// The mesh its use spot faces at the trigger (resolve_faced_visual).
            Faced,
        };

        /** @brief The mesh found for one candidate, cached per entity. */
        struct VisualCache
        {
            EntityId owner{0};
            std::uintptr_t brush{0};
            game_structures::Aabb bounds{};
            // Nothing found; the marker stays at the trigger until the retry.
            bool none{false};
            // The template placed the object, but nothing stands at its pose yet (an interior not streamed in): the
            // retry comes soon.
            bool pending{false};
            // The brush was freed since (streaming recreates brushes); it is looked for again, first where it was.
            bool gone{false};
            // The mesh entity is kept invisible by the game for now (entity_flags_game_hidden): it keeps its outline
            // state but shows a marker until the game draws it again.
            bool game_hidden{false};
            VisualSource source{VisualSource::Search};
            // A template brush's predicted pivot: a recreated brush is found again there with one small query.
            std::optional<game_structures::Vec3f> pose{};
            // The found brush's model (a recreated brush found by search is recognised by it and its bounds) and the
            // template's model (the pose lookup's tie-break).
            std::string model{};
            std::string expected_model{};
            // The kind the candidate resolved to: a use trigger is a Seat or a UseSpot by what it sits on.
            InteractKind kind{InteractKind::Other};
            std::int64_t resolved_ms{0};
            // When a none is looked at again, or once the player is this close (0: no distance rule).
            std::int64_t retry_ms{0};
            float retry_distance{0.0f};
            // Consecutive misses that are not pending (the retry's back-off step).
            std::uint8_t misses{0};
        };

        /** @brief State of a cached mesh at the time of a collect. */
        enum class VisualState : std::uint8_t
        {
            Alive,
            // The owner entity is hidden right now; the cache stays.
            Hidden,
            // The mesh is gone or no longer matches; it is looked up again.
            Gone,
        };

        /** @brief Per-class tally of a survey walk. */
        struct SurveyEntry
        {
            std::uintptr_t sample_entity{0};
            std::size_t count{0};
        };

        /** @brief One target of the last collect and the mesh it shows. */
        struct TargetState
        {
            InteractTarget target{};
            bool has_visual{false};
            InteractVisual visual{};
        };

        /** @brief A trigger whose mesh is to be looked up, and how far it was at the collect. */
        struct QueuedLookup
        {
            Candidate candidate{};
            float distance{0.0f};
        };

        /** @brief The object a matched template names, for the Seat decision. */
        struct TemplatePick
        {
            bool valid{false};
            TemplateObjectKind kind{TemplateObjectKind::Brush};
            std::string name{};
            std::string entity_class{};
            std::string model{};
        };

        /** @brief What a trigger's entity links point at. */
        /** @brief Where the game puts the player to use a trigger: a linked smart object's position and facing. */
        struct UseSpot
        {
            game_structures::Vec3f position{};
            game_structures::Vec3f forward{};
        };

        struct LinkInfo
        {
            std::vector<game_structures::Vec3f> positions{};
            std::string first_class{};
            /// The first linked smart object (SmartObjectHolder, StanceSmartObject), when there is one.
            std::optional<UseSpot> spot{};
        };

        std::atomic<bool> s_available{false};
        std::atomic<bool> s_survey_requested{false};

        // Main-thread state.
        std::unordered_map<std::uintptr_t, ClassInfo> s_class_infos;
        // s_class_infos's fast path for the walk.
        ClassMemo<ClassInfo> s_class_memo;
        std::unordered_map<std::uintptr_t, std::string> s_class_names;
        std::unordered_map<EntityId, VisualCache> s_visuals;
        std::unordered_set<EntityId> s_logged_ids;
        // Whether a book can be read in place, per entity.
        std::unordered_map<EntityId, bool> s_readable_books;

        EntityWalk s_walk;
        std::vector<WalkedEntity> s_batch;
        std::vector<Candidate> s_pending;
        std::vector<Candidate> s_candidates;
        std::unordered_map<std::uintptr_t, SurveyEntry> s_survey;
        // The entities nearest the player in a survey walk (distance, id), listed by name.
        std::vector<std::pair<float, EntityId>> s_survey_nearest;
        constexpr std::size_t SURVEY_NEAREST = 20;
        bool s_surveying = false;
        game_structures::Vec3f s_pending_center{};
        float s_pending_radius = 0.0f;
        std::size_t s_walk_entities = 0;
        std::int64_t s_walk_started_ms = 0;
        std::int64_t s_walk_busy_us = 0;
        std::uint32_t s_walk_frames = 0;

        std::int64_t s_walk_done_ms = 0;
        bool s_walk_valid = false;
        std::size_t s_last_count = static_cast<std::size_t>(-1);

        // The last collect's targets, its kind mask and reach, and the lookups it queued (nearest first; the slice
        // takes them from s_queue_next on).
        std::vector<TargetState> s_targets;
        std::uint32_t s_collect_mask = 0;
        float s_collect_radius = 0.0f;
        std::vector<QueuedLookup> s_queue;
        std::size_t s_queue_next = 0;
        // Moving average of one lookup's cost, microseconds.
        float s_lookup_cost_us = static_cast<float>(INITIAL_RESOLVE_COST_US);

        [[nodiscard]] std::int64_t steady_us() noexcept
        {
            return std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now().time_since_epoch()
            )
                .count();
        }

        [[nodiscard]] float distance_between(const game_structures::Vec3f &a, const game_structures::Vec3f &b) noexcept
        {
            const float dx = a.x - b.x;
            const float dy = a.y - b.y;
            const float dz = a.z - b.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        /**
         * @brief Name of an entity's class, cached per class pointer.
         */
        [[nodiscard]] const std::string &class_name(std::uintptr_t klass, std::uintptr_t entity)
        {
            auto it = s_class_names.find(klass);
            if (it == s_class_names.end())
            {
                it = s_class_names.emplace(klass, entity_class_name(entity)).first;
            }
            return it->second;
        }

        /**
         * @brief Interactive group (-1: none) and traits of an entity's class, resolved once per class pointer.
         */
        [[nodiscard]] ClassInfo class_info(std::uintptr_t klass, std::uintptr_t entity)
        {
            if (const ClassInfo *memo = s_class_memo.find(klass))
            {
                return *memo;
            }
            if (const auto it = s_class_infos.find(klass); it != s_class_infos.end())
            {
                s_class_memo.store(klass, it->second);
                return it->second;
            }
            const std::string &name = class_name(klass, entity);
            ClassInfo info{};
            for (const InteractiveClass &entry : INTERACTIVE_CLASSES)
            {
                if (entry.name == name)
                {
                    info = ClassInfo{static_cast<std::int8_t>(entry.kind), entry.traits};
                    break;
                }
            }
            s_class_infos.emplace(klass, info);
            return info;
        }

        /**
         * @brief Logs how many entities of each class the survey walk found within the radius, most frequent first.
         */
        void log_survey()
        {
            std::vector<std::pair<std::size_t, std::string>> sorted;
            std::size_t within = 0;
            sorted.reserve(s_survey.size());
            for (const auto &[klass, entry] : s_survey)
            {
                within += entry.count;
                std::string name = class_name(klass, entry.sample_entity);
                if (const ClassInfo info = class_info(klass, entry.sample_entity); info.kind >= 0)
                {
                    name += std::format("[{}]", interact_kind_name(static_cast<InteractKind>(info.kind)));
                }
                sorted.emplace_back(entry.count, std::move(name));
            }
            std::sort(
                sorted.begin(),
                sorted.end(),
                [](const auto &a, const auto &b)
                { return a.first != b.first ? a.first > b.first : a.second < b.second; }
            );
            std::string text;
            for (std::size_t i = 0; i < sorted.size() && i < SURVEY_TOP; ++i)
            {
                text += std::format("{}{} x{}", i == 0 ? "" : ", ", sorted[i].second, sorted[i].first);
            }
            (void)DMK::log().try_log(
                DMK::LogLevel::Info,
                "Interactables: survey of {} entities within {:.0f} m ({} classes): {}",
                within,
                s_pending_radius,
                sorted.size(),
                text
            );
            std::sort(s_survey_nearest.begin(), s_survey_nearest.end());
            std::string nearest;
            for (std::size_t i = 0; i < s_survey_nearest.size() && i < SURVEY_NEAREST; ++i)
            {
                const auto [distance, id] = s_survey_nearest[i];
                // Resolved again: the walk ran over several frames.
                const std::uintptr_t entity = entity_from_id(id);
                if (entity == 0)
                {
                    continue;
                }
                nearest += std::format(
                    "\n    {:.1f} m {:#x} {} {}",
                    distance,
                    id,
                    entity_class_name(entity),
                    entity_name(entity)
                );
            }
            (void)DMK::log().try_log(DMK::LogLevel::Info, "Interactables: nearest entities:{}", nearest);
            s_survey_nearest.clear();
        }

        /**
         * @brief Classifies one batch of walked entities into the pending candidates (and the survey tally).
         */
        void process_batch()
        {
            for (std::size_t i = 0; i < s_batch.size(); ++i)
            {
                // The walk hands over the class; the survey reads every entity's position, otherwise only an
                // interactive class's is read.
                if (s_surveying && i + PREFETCH_DISTANCE < s_batch.size())
                {
                    prefetch_entity(s_batch[i + PREFETCH_DISTANCE].entity, true);
                }
                const std::uintptr_t entity = s_batch[i].entity;
                const std::uintptr_t klass = s_batch[i].klass;
                if (klass == 0)
                {
                    continue;
                }
                std::optional<game_structures::Vec3f> position{};
                if (s_surveying)
                {
                    position = entity_world_position(entity);
                    if (position.has_value() && distance_between(*position, s_pending_center) <= s_pending_radius)
                    {
                        SurveyEntry &entry = s_survey[klass];
                        s_survey_nearest.emplace_back(distance_between(*position, s_pending_center), s_batch[i].id);
                        entry.sample_entity = entity;
                        ++entry.count;
                    }
                }
                const ClassInfo info = class_info(klass, entity);
                if (info.kind < 0)
                {
                    continue;
                }
                if (!s_surveying)
                {
                    position = entity_world_position(entity);
                }
                if (!position.has_value())
                {
                    continue;
                }
                if (const EntityId id = s_batch[i].id; id != 0)
                {
                    s_pending.push_back(Candidate{id, static_cast<InteractKind>(info.kind), info.traits, *position});
                }
            }
            s_walk_entities += s_batch.size();
            s_batch.clear();
        }

        void finish_walk(bool complete)
        {
            s_candidates.swap(s_pending);
            s_pending.clear();
            s_walk_done_ms = steady_us() / 1000;
            s_walk_valid = true;
            (void)DMK::log().try_log(
                DMK::LogLevel::Debug,
                "Interactables: walked {} entities ({}{}) over {} frame(s), {:.2f} ms busy, {} ms "
                "wall; {} interactive in the level, {} classes known",
                s_walk_entities,
                complete ? "complete" : "cut short",
                s_surveying ? ", survey" : "",
                s_walk_frames,
                static_cast<double>(s_walk_busy_us) / 1000.0,
                s_walk_done_ms - s_walk_started_ms,
                s_candidates.size(),
                s_class_infos.size()
            );
            if (s_surveying)
            {
                log_survey();
                s_survey.clear();
                s_surveying = false;
            }
        }

        [[nodiscard]] bool walk_due() noexcept
        {
            return !s_walk_valid || steady_us() / 1000 - s_walk_done_ms >= REWALK_MS ||
                   s_survey_requested.load(std::memory_order_relaxed);
        }

        /**
         * @brief Marker bounds of an interactive entity: its world bounds, or a small box at its position when the
         *        bounds are empty, degenerate or an area far larger than the object.
         */
        [[nodiscard]] game_structures::Aabb
        marker_bounds(std::uintptr_t entity, const game_structures::Vec3f &position, bool &fallback) noexcept
        {
            const std::optional<game_structures::Aabb> bounds = entity_world_bounds(entity);
            if (bounds.has_value())
            {
                const float ex = bounds->max.x - bounds->min.x;
                const float ey = bounds->max.y - bounds->min.y;
                const float ez = bounds->max.z - bounds->min.z;
                if (std::isfinite(ex) && std::isfinite(ey) && std::isfinite(ez) && ex >= 0.0f && ey >= 0.0f &&
                    ez >= 0.0f && std::max({ex, ey, ez}) <= MAX_EXTENT &&
                    std::max({ex, ey, ez}) >= 2.0f * MIN_HALF_EXTENT)
                {
                    fallback = false;
                    return *bounds;
                }
            }
            fallback = true;
            const float h = DEFAULT_HALF_EXTENT;
            return game_structures::Aabb{
                {position.x - h, position.y - h, position.z},
                {position.x + h, position.y + h, position.z + 2.0f * h}
            };
        }

        void log_new_target(std::uintptr_t entity, const InteractTarget &target, bool fallback)
        {
            if (!DMK::log().is_enabled(DMK::LogLevel::Debug) || !s_logged_ids.insert(target.entity_id).second)
            {
                return;
            }
            const game_structures::Aabb &b = target.bounds;
            (void)DMK::log().try_log(
                DMK::LogLevel::Debug,
                "Interactables: + id={:#x} {} class={} name={} dist={:.1f} "
                "size={:.2f}x{:.2f}x{:.2f}{} node=0x{:016X}",
                target.entity_id,
                interact_kind_name(target.kind),
                entity_class_name(entity),
                entity_name(entity),
                target.distance,
                b.max.x - b.min.x,
                b.max.y - b.min.y,
                b.max.z - b.min.z,
                fallback ? " (position box)" : "",
                render_node_of(entity)
            );
        }

        /**
         * @brief Reads where a trigger's entity links point (at most MAX_LINKS): the targets' world positions, and
         *        the first target's class.
         */
        [[nodiscard]] LinkInfo read_links(std::uintptr_t entity)
        {
            LinkInfo info{};
            auto link = DMK::memory::read<std::uintptr_t>(DMK::Address{entity + constants::ENTITY_LINKS_OFFSET});
            for (int i = 0; i < MAX_LINKS && link && *link != 0 && DMK::memory::is_plausible_ptr(DMK::Address{*link});
                 ++i)
            {
                const std::uintptr_t node = *link;
                const auto target_id =
                    DMK::memory::read<std::uint32_t>(DMK::Address{node + constants::ENTITY_LINK_TARGET_OFFSET});
                if (const std::uintptr_t target = target_id && *target_id != 0 ? entity_from_id(*target_id) : 0;
                    target != 0)
                {
                    if (const std::optional<game_structures::Vec3f> position = entity_world_position(target))
                    {
                        info.positions.push_back(*position);
                    }
                    const std::string klass = entity_class_name(target);
                    if (!info.spot.has_value() && klass.find("SmartObject") != std::string::npos)
                    {
                        // The smart object's local +Y is where the player faces while using it (column 1 of its world
                        // matrix; the translation is column 3).
                        if (const auto world = DMK::memory::read<game_structures::Matrix34f>(
                                DMK::Address{target + constants::ENTITY_WORLD_MATRIX_OFFSET}
                            ))
                        {
                            const game_structures::Matrix34f &m = *world;
                            info.spot = UseSpot{{m.m[0][3], m.m[1][3], m.m[2][3]}, {m.m[0][1], m.m[1][1], m.m[2][1]}};
                        }
                    }
                    if (i == 0)
                    {
                        info.first_class = klass;
                    }
                }
                link = DMK::memory::read<std::uintptr_t>(DMK::Address{node + constants::ENTITY_LINK_NEXT_OFFSET});
            }
            return info;
        }

        /** @brief The nodes the mod keeps outlined off the octree within a box around @p center. */
        [[nodiscard]] std::vector<std::uintptr_t>
        outlined_nodes(OutlinedNodesFn outlined, const game_structures::Vec3f &center, float reach, float height)
        {
            std::vector<std::uintptr_t> nodes;
            if (outlined != nullptr)
            {
                outlined(
                    game_structures::Aabb{
                        {center.x - reach, center.y - reach, center.z - height},
                        {center.x + reach, center.y + reach, center.z + height}
                    },
                    nodes
                );
            }
            return nodes;
        }

        /** @brief Everything a lookup reads about its trigger once. */
        struct LookupInput
        {
            const Candidate *candidate{nullptr};
            std::uintptr_t entity{0};
            game_structures::Vec3f position{};
            float distance{0.0f};
            std::string klass{};
            std::string name{};
            std::string_view tag{};
            // Every word of the trigger's own name (the template pick's), and the words of its class ("bed").
            std::vector<std::string> name_words{};
            std::vector<std::string> class_words{};
            LinkInfo links{};
            OutlinedNodesFn outlined{nullptr};
            bool tracing{false};
        };

        /** @brief The keywords of a trigger's search: its class's words and its class name's nouns. */
        [[nodiscard]] std::vector<std::string> search_keywords(const LookupInput &in)
        {
            std::vector<std::string> keywords;
            for (const std::string_view word : trigger_words(in.klass, in.candidate->kind))
            {
                keywords.emplace_back(word);
            }
            for (const std::string &word : in.class_words)
            {
                if (std::find(keywords.begin(), keywords.end(), word) == keywords.end())
                {
                    keywords.push_back(word);
                }
            }
            return keywords;
        }

        [[nodiscard]] std::string point_text(const game_structures::Vec3f &p)
        {
            return std::format("({:.3f},{:.3f},{:.3f})", p.x, p.y, p.z);
        }

        /** @brief Outcome of the template lookup. */
        enum class TemplateOutcome : std::uint8_t
        {
            /// The object was found at its pose.
            Found,
            /// The template placed it, but nothing stands at the pose yet and the trigger is far: retry soon.
            Pending,
            /// No template fits, or nothing stands at the pose close by: search instead.
            Fallback,
        };

        /**
         * @brief Finds a trigger's object at the pose its prefab template predicts.
         * @param in The trigger.
         * @param out Receives the mesh on Found, the pending state on Pending.
         * @param pick Receives the template object that shows the trigger's object, when one was picked.
         */
        [[nodiscard]] TemplateOutcome resolve_by_template(const LookupInput &in, VisualCache &out, TemplatePick &pick)
        {
            if (!template_trigger_class(in.klass))
            {
                return TemplateOutcome::Fallback;
            }
            std::string log;
            auto finish = [&](TemplateOutcome outcome)
            {
                if (in.tracing)
                {
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Debug,
                        "Interactables: template for id={:#x}: {}",
                        in.candidate->id,
                        log
                    );
                }
                return outcome;
            };
            const auto world = DMK::memory::read<game_structures::Matrix34f>(
                DMK::Address{in.entity + constants::ENTITY_WORLD_MATRIX_OFFSET}
            );
            if (!world)
            {
                log = "world matrix unreadable; searching";
                return finish(TemplateOutcome::Fallback);
            }
            const std::string_view base = std::string_view{in.name}.substr(0, in.name.find('['));
            const TemplateMatchResult result = match_prefab_template(in.klass, in.name, *world, in.links.positions);
            switch (result.status)
            {
            case TemplateMatchStatus::NotReady:
                log = "template index not ready; searching";
                return finish(TemplateOutcome::Fallback);
            case TemplateMatchStatus::NoTemplate:
                log = std::format("no template places {} '{}'; searching", in.klass, base);
                return finish(TemplateOutcome::Fallback);
            case TemplateMatchStatus::NoLink:
                log = std::format(
                    "{} '{}' has no entity link to measure ({} link target(s)); searching",
                    in.klass,
                    base,
                    in.links.positions.size()
                );
                return finish(TemplateOutcome::Fallback);
            case TemplateMatchStatus::Mismatch:
                log = std::format(
                    "{} template(s) place {} '{}', none puts its linked object here (closest {} off by {:.1f} cm); "
                    "searching",
                    result.candidates,
                    in.klass,
                    base,
                    result.match.prefab != nullptr ? result.match.prefab->name : std::string{"-"},
                    result.match.error * 100.0f
                );
                return finish(TemplateOutcome::Fallback);
            case TemplateMatchStatus::Matched:
            default:
                break;
            }
            const TemplateMatch &match = result.match;
            std::string_view rule{};
            const TemplateObject *object = pick_template_object(match, in.name_words, rule);
            log = std::format(
                "{} ({} '{}', link off by {:.1f} mm)",
                match.prefab->name,
                in.klass,
                base,
                match.error * 1000.0f
            );
            if (object == nullptr)
            {
                log += " has no mesh object; searching";
                return finish(TemplateOutcome::Fallback);
            }
            pick = TemplatePick{true, object->kind, object->name, object->entity_class, object->model};
            const game_structures::Vec3f pose = template_to_world(match, object->position);
            log += std::format(
                " object {} {} ({}) pred={}",
                object->name,
                object->kind == TemplateObjectKind::Brush ? object->model : object->entity_class,
                rule,
                point_text(pose)
            );
            std::string trace;
            std::string *const trace_out = in.tracing ? &trace : nullptr;
            switch (object->kind)
            {
            case TemplateObjectKind::Brush:
            {
                const std::vector<std::uintptr_t> extra =
                    outlined_nodes(in.outlined, pose, OUTLINED_POSE_REACH, OUTLINED_POSE_REACH);
                if (const std::optional<PoseHit> hit =
                        find_brush_at_pose(pose, object->model, in.position, extra, trace_out))
                {
                    out.brush = hit->node;
                    out.bounds = hit->bounds;
                    out.pose = pose;
                    out.model = hit->model;
                    out.expected_model = object->model;
                    out.source = VisualSource::Template;
                    log += std::format(
                        " -> brush 0x{:016X} {} pivot {:.1f} cm off{}{}",
                        hit->node,
                        hit->model,
                        hit->offset * 100.0f,
                        // Level designers often put another model at the template's pose; the pose decides.
                        !same_model(hit->model, object->model)
                            ? std::format(" (model swapped: template {})", object->model)
                            : std::string{},
                        hit->matches > 1 ? std::format(", {} brushes at the pose", hit->matches) : std::string{}
                    );
                    return finish(TemplateOutcome::Found);
                }
                break;
            }
            case TemplateObjectKind::RuntimePrefab:
            {
                std::vector<std::string> words = search_keywords(in);
                words.insert(words.end(), in.name_words.begin(), in.name_words.end());
                const std::vector<std::uintptr_t> extra =
                    outlined_nodes(in.outlined, pose, OUTLINED_REACH, OUTLINED_HEIGHT);
                if (const std::optional<PrefabBrushHit> hit =
                        find_prefab_brush(pose, in.tag, RUNTIME_PREFAB_TOLERANCE, words, extra, trace_out))
                {
                    // The prefab respawns its brushes on a phase change (an empty cauldron, then soup), so no pose is
                    // kept: a freed one is looked up in full.
                    out.brush = hit->brush.node;
                    out.bounds = hit->brush.bounds;
                    out.model = hit->brush.model;
                    out.source = VisualSource::Template;
                    log += std::format(
                        " -> owned brush 0x{:016X} {} of prefab entity {:#x} {:.1f} cm off{}",
                        hit->brush.node,
                        hit->brush.model,
                        hit->owner_id,
                        hit->owner_offset * 100.0f,
                        trace
                    );
                    return finish(TemplateOutcome::Found);
                }
                break;
            }
            case TemplateObjectKind::MeshEntity:
            default:
            {
                if (const std::optional<EntityHit> hit =
                        find_entity_at_pose(pose, object->entity_class, in.tag, ENTITY_POSE_TOLERANCE, trace_out))
                {
                    out.owner = hit->id;
                    out.bounds = hit->bounds;
                    out.source = VisualSource::Template;
                    log += std::format(" -> entity {:#x} {:.1f} cm off", hit->id, hit->offset * 100.0f);
                    return finish(TemplateOutcome::Found);
                }
                break;
            }
            }
            if (in.distance > PENDING_DISTANCE)
            {
                out.pending = true;
                out.pose = pose;
                log += std::format(" -> nothing at the pose yet, {:.1f} m away; retrying{}", in.distance, trace);
                return finish(TemplateOutcome::Pending);
            }
            log += std::format(" -> nothing at the pose; searching{}", trace);
            return finish(TemplateOutcome::Fallback);
        }

        /**
         * @brief Finds the object no model name identifies from the trigger's use spot (resolve_faced_visual): the
         *        static mesh the spot faces at the trigger. A trigger without a linked smart object keeps its marker.
         */
        [[nodiscard]] std::optional<VisualCache>
        resolve_by_use_spot(const LookupInput &in, std::span<const std::uintptr_t> extra)
        {
            if (!in.links.spot.has_value())
            {
                return std::nullopt;
            }
            FacedSearch search{};
            search.trigger = in.position;
            search.spot = in.links.spot->position;
            search.forward = in.links.spot->forward;
            search.exclude_node = render_node_of(in.entity);
            search.extra_nodes = extra;
            std::string trace;
            const std::optional<VisualNode> found = resolve_faced_visual(search, in.tracing ? &trace : nullptr);
            if (in.tracing)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "Interactables: meshes faced from the use spot of id={:#x} at {}:{}",
                    in.candidate->id,
                    point_text(search.spot),
                    trace
                );
            }
            // A brush only: an entity found this way could be an item or a character standing at the spot.
            if (!found.has_value() || found->type == constants::RENDERNODE_TYPE_RENDER_PROXY)
            {
                return std::nullopt;
            }
            return VisualCache{
                .brush = found->node,
                .bounds = found->bounds,
                .source = VisualSource::Faced,
                .model = render_node_name(found->node),
            };
        }

        /**
         * @brief Finds a trigger's object by searching around it (no template fits, or nothing at the template pose).
         */
        [[nodiscard]] std::optional<VisualCache> resolve_by_search(const LookupInput &in)
        {
            std::string trace;
            // A word of the trigger's own name that its class also has ("bed" of BedTrigger12) names only the kind of
            // object, so it ranks as a keyword; the others ("pan" of trigger_pan) name the object itself.
            std::vector<std::string> own_words;
            for (const std::string &word : in.name_words)
            {
                if (std::find(in.class_words.begin(), in.class_words.end(), word) == in.class_words.end())
                {
                    own_words.push_back(word);
                }
            }
            const std::vector<std::string> keywords = search_keywords(in);
            const std::vector<std::uintptr_t> extra =
                outlined_nodes(in.outlined, in.position, OUTLINED_REACH, OUTLINED_HEIGHT);
            TriggerSearch search{};
            search.position = in.position;
            search.trigger_bounds = entity_world_bounds(in.entity);
            search.exclude_node = render_node_of(in.entity);
            search.keywords = keywords;
            if (in.klass == BED_TRIGGER_CLASS)
            {
                search.bedding_words = BEDDING_WORDS;
            }
            search.prefab_tag = in.tag;
            search.extra_nodes = extra;
            search.own_words = own_words;
            const std::optional<VisualNode> found = resolve_trigger_visual(search, in.tracing ? &trace : nullptr);
            if (in.tracing)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "Interactables: candidates for id={:#x}:{}",
                    in.candidate->id,
                    trace
                );
            }
            if (!found.has_value())
            {
                return resolve_by_use_spot(in, extra);
            }
            if (found->type != constants::RENDERNODE_TYPE_RENDER_PROXY)
            {
                return VisualCache{
                    .brush = found->node,
                    .bounds = found->bounds,
                    .source = VisualSource::Search,
                    .model = render_node_name(found->node),
                };
            }
            // An entity mesh: its owner is kept, and its proxy is resolved again on every use.
            const auto owner =
                DMK::memory::read<std::uintptr_t>(DMK::Address{found->node + constants::RENDER_PROXY_ENTITY_OFFSET});
            if (owner && *owner != 0 && object_is(GameClass::Entity, *owner))
            {
                if (const EntityId id = entity_id_of(*owner); id != 0)
                {
                    return VisualCache{
                        .owner = id,
                        .bounds = found->bounds,
                        .source = VisualSource::Search,
                    };
                }
            }
            return std::nullopt;
        }

        /**
         * @brief The kind a candidate shows as: a use trigger that links a stance smart object and sits on a chair,
         *        bench, stool or throne is a Seat, any other one a UseSpot; every other class keeps its kind.
         */
        [[nodiscard]] InteractKind
        classify(const Candidate &candidate, const LinkInfo &links, const VisualCache &cache, const TemplatePick &pick)
        {
            if ((candidate.traits & TRAIT_SEAT_OR_USE) == 0)
            {
                return candidate.kind;
            }
            if (links.first_class != STANCE_CLASS)
            {
                return InteractKind::UseSpot;
            }
            bool seat = false;
            if (!cache.none && cache.brush != 0)
            {
                seat = has_word(cache.model, std::span<const std::string_view>{SEAT_WORDS});
            }
            else if (!cache.none && cache.owner != 0 && cache.owner != candidate.id)
            {
                if (const std::uintptr_t owner = entity_from_id(cache.owner); owner != 0)
                {
                    seat = entity_class_name(owner) == CHAIR_CLASS ||
                           has_word(entity_name(owner), std::span<const std::string_view>{SEAT_WORDS});
                }
            }
            else if (pick.valid)
            {
                // Nothing found yet: the template names the object (its model, or a chair entity).
                seat = pick.kind == TemplateObjectKind::Brush
                           ? has_word(pick.model, std::span<const std::string_view>{SEAT_WORDS})
                           : pick.entity_class == CHAIR_CLASS ||
                                 has_word(pick.name, std::span<const std::string_view>{SEAT_WORDS});
            }
            return seat ? InteractKind::Seat : InteractKind::UseSpot;
        }

        /**
         * @brief Finds a recreated brush again where it was: at the template pose, or by its model and bounds.
         * @return True when @p cache now holds the new brush.
         */
        [[nodiscard]] bool rebind(VisualCache &cache, const LookupInput &in)
        {
            std::optional<PoseHit> hit{};
            if (cache.pose.has_value())
            {
                const std::vector<std::uintptr_t> extra =
                    outlined_nodes(in.outlined, *cache.pose, OUTLINED_POSE_REACH, OUTLINED_POSE_REACH);
                hit = find_brush_at_pose(*cache.pose, cache.expected_model, in.position, extra, nullptr);
            }
            else if (!cache.model.empty())
            {
                const game_structures::Vec3f centre{
                    0.5f * (cache.bounds.min.x + cache.bounds.max.x),
                    0.5f * (cache.bounds.min.y + cache.bounds.max.y),
                    0.5f * (cache.bounds.min.z + cache.bounds.max.z)
                };
                const std::vector<std::uintptr_t> extra =
                    outlined_nodes(in.outlined, centre, OUTLINED_REACH, OUTLINED_HEIGHT);
                hit = find_brush_like(cache.model, cache.bounds, extra);
            }
            if (!hit.has_value())
            {
                return false;
            }
            (void)DMK::log().try_log(
                DMK::LogLevel::Debug,
                "Interactables: mesh of id={:#x} found again after streaming: brush 0x{:016X} -> 0x{:016X} {}",
                in.candidate->id,
                cache.brush,
                hit->node,
                hit->model
            );
            cache.brush = hit->node;
            cache.bounds = hit->bounds;
            cache.model = hit->model;
            cache.gone = false;
            return true;
        }

        /**
         * @brief Finds the mesh that shows a candidate: its own render proxy for an entity with geometry, else the
         *        brush or entity mesh at its template pose or under it.
         * @param candidate The candidate.
         * @param entity Its entity.
         * @param position Its position.
         * @param distance Its distance to the player.
         * @param previous Its earlier result (a freed brush to find again, a miss count), or null.
         * @param outlined The nodes the mod keeps outlined off the octree.
         */
        [[nodiscard]] VisualCache resolve_visual(
            const Candidate &candidate,
            std::uintptr_t entity,
            const game_structures::Vec3f &position,
            float distance,
            const VisualCache *previous,
            OutlinedNodesFn outlined
        )
        {
            const std::int64_t now = steady_us() / 1000;
            if ((candidate.traits & TRAIT_HELPER) == 0 && render_node_of(entity) != 0)
            {
                bool fallback = false;
                const game_structures::Aabb own = marker_bounds(entity, position, fallback);
                if (!fallback)
                {
                    return VisualCache{
                        .owner = candidate.id,
                        .bounds = own,
                        .source = VisualSource::Own,
                        .kind = candidate.kind,
                        .resolved_ms = now,
                    };
                }
            }
            LookupInput in{};
            in.candidate = &candidate;
            in.entity = entity;
            in.position = position;
            in.distance = distance;
            in.klass = entity_class_name(entity);
            in.name = entity_name(entity);
            in.tag = prefab_instance_tag(in.name);
            in.name_words = trigger_name_words(in.name);
            in.class_words = trigger_name_words(in.klass);
            in.links = read_links(entity);
            in.outlined = outlined;
            // The per-candidate traces are built only for a log that shows them.
            in.tracing = DMK::log().is_enabled(DMK::LogLevel::Debug);

            if (previous != nullptr && previous->gone)
            {
                VisualCache again = *previous;
                if (rebind(again, in))
                {
                    again.resolved_ms = now;
                    return again;
                }
            }
            VisualCache cache{};
            TemplatePick pick{};
            const TemplateOutcome outcome = resolve_by_template(in, cache, pick);
            if (outcome == TemplateOutcome::Fallback)
            {
                if (std::optional<VisualCache> found = resolve_by_search(in))
                {
                    cache = std::move(*found);
                }
                else
                {
                    cache = VisualCache{.none = true};
                }
            }
            else if (outcome == TemplateOutcome::Pending)
            {
                cache.none = true;
            }
            cache.resolved_ms = now;
            if (cache.none)
            {
                // Only a template that placed the object with nothing at its pose yet (a far interior not streamed
                // in) is retried soon, or as soon as the player comes closer. Every other miss already searched the
                // box around the trigger, so its retries back off; a far one is also looked at once more when the
                // player reaches the distance interiors stream in at.
                if (cache.pending)
                {
                    cache.retry_ms = now + PENDING_RETRY_MS;
                    cache.retry_distance = std::max(0.0f, distance - PENDING_APPROACH);
                }
                else
                {
                    const std::uint8_t misses =
                        previous != nullptr && previous->none && !previous->pending ? previous->misses : 0;
                    cache.misses = static_cast<std::uint8_t>(std::min<int>(misses + 1, 255));
                    cache.retry_ms = now + MISS_RETRY_MS[std::min<std::size_t>(misses, MISS_RETRY_MS.size() - 1)];
                    cache.retry_distance = distance > PENDING_DISTANCE ? PENDING_DISTANCE : 0.0f;
                }
            }
            cache.kind = classify(candidate, in.links, cache, pick);
            return cache;
        }

        /**
         * @brief Re-checks a cached mesh, refreshing its bounds (a door swings, a movable brush moves) and, for an
         *        entity mesh, whether the game keeps it invisible for now.
         */
        [[nodiscard]] VisualState check_visual(VisualCache &cache) noexcept
        {
            if (cache.brush != 0)
            {
                // A CBrush or COwnedBrush must keep its bounds; a CMovableBrush must keep its model and may move.
                const std::optional<game_structures::Aabb> now =
                    brush_revalidate(cache.brush, cache.bounds, cache.model);
                if (!now.has_value())
                {
                    return VisualState::Gone;
                }
                cache.bounds = *now;
                return VisualState::Alive;
            }
            const std::uintptr_t owner = entity_from_id(cache.owner);
            if (owner == 0)
            {
                return VisualState::Gone;
            }
            // Unreadable flags count as hidden, as entity_is_hidden has them.
            const std::optional<std::uint32_t> flags = entity_flags(owner);
            if (!flags.has_value() || (*flags & constants::ENTITY_FLAG_HIDDEN) != 0)
            {
                return VisualState::Hidden;
            }
            // An invisible entity keeps its node and its outline state; it just is not drawn until the game shows it.
            cache.game_hidden = entity_flags_game_hidden(*flags);
            if (const std::optional<game_structures::Aabb> bounds = entity_world_bounds(owner); bounds.has_value())
            {
                cache.bounds = *bounds;
            }
            return VisualState::Alive;
        }

        /**
         * @brief True when a new lookup result shows differently from the cached one the last collect placed: another
         *        mesh, bounds or kind, a hit where there was none (or the reverse), or a cached mesh that was gone.
         */
        [[nodiscard]] bool visual_differs(const VisualCache &before, const VisualCache &after) noexcept
        {
            if (before.gone || before.none != after.none || before.brush != after.brush ||
                before.owner != after.owner || before.kind != after.kind)
            {
                return true;
            }
            if (after.none)
            {
                return false;
            }
            const game_structures::Aabb &a = before.bounds;
            const game_structures::Aabb &b = after.bounds;
            return a.min.x != b.min.x || a.min.y != b.min.y || a.min.z != b.min.z || a.max.x != b.max.x ||
                   a.max.y != b.max.y || a.max.z != b.max.z;
        }

        /**
         * @brief Describes a trigger's entity links (name, target class and name, and whether the target has a mesh),
         *        for the log.
         */
        [[nodiscard]] std::string describe_links(std::uintptr_t entity)
        {
            constexpr std::size_t max_link_name_length = 47;
            std::string text;
            auto link = DMK::memory::read<std::uintptr_t>(DMK::Address{entity + constants::ENTITY_LINKS_OFFSET});
            for (int i = 0; i < MAX_LINKS && link && *link != 0 && DMK::memory::is_plausible_ptr(DMK::Address{*link});
                 ++i)
            {
                const std::uintptr_t node = *link;
                std::string name;
                if (const auto text_ptr =
                        DMK::memory::read<std::uintptr_t>(DMK::Address{node + constants::ENTITY_LINK_NAME_OFFSET});
                    text_ptr && DMK::memory::is_plausible_ptr(DMK::Address{*text_ptr}))
                {
                    name = read_c_string(*text_ptr, max_link_name_length);
                }
                const auto target_id =
                    DMK::memory::read<std::uint32_t>(DMK::Address{node + constants::ENTITY_LINK_TARGET_OFFSET});
                const std::uintptr_t target = target_id ? entity_from_id(*target_id) : 0;
                std::string target_text = "-";
                if (target != 0)
                {
                    const std::uintptr_t target_node = render_node_of(target);
                    const std::optional<game_structures::Aabb> bounds = entity_world_bounds(target);
                    target_text = std::format(
                        "{} {} node={}{}",
                        entity_class_name(target),
                        entity_name(target),
                        DMK::format::format_address(target_node),
                        bounds.has_value() ? std::format(
                                                 " size={:.2f}x{:.2f}x{:.2f}",
                                                 bounds->max.x - bounds->min.x,
                                                 bounds->max.y - bounds->min.y,
                                                 bounds->max.z - bounds->min.z
                                             )
                                           : std::string{" no bounds"}
                    );
                }
                text += std::format("\n    link \"{}\" -> {:#x} {}", name, target_id ? *target_id : 0, target_text);
                link = DMK::memory::read<std::uintptr_t>(DMK::Address{node + constants::ENTITY_LINK_NEXT_OFFSET});
            }
            return text.empty() ? std::string{" (no links)"} : text;
        }

        [[nodiscard]] std::string_view source_name(VisualSource source) noexcept
        {
            switch (source)
            {
            case VisualSource::Own:
                return "own";
            case VisualSource::Template:
                return "template";
            case VisualSource::Faced:
                return "faced";
            case VisualSource::Search:
            default:
                return "search";
            }
        }

        void log_visual(std::uintptr_t entity, const Candidate &candidate, const VisualCache &cache)
        {
            // Its arguments read names and walk the trigger's links, so they are skipped for a log that drops the line.
            if (!DMK::log().is_enabled(DMK::LogLevel::Debug))
            {
                return;
            }
            const game_structures::Aabb &b = cache.bounds;
            std::string what;
            if (cache.none)
            {
                what = cache.pending
                           ? std::format(
                                 "none yet (marker at the trigger; retry in {} ms)",
                                 cache.retry_ms - cache.resolved_ms
                             )
                           : std::format(
                                 "none (marker at the trigger; miss {}, retry in {} s{})",
                                 cache.misses,
                                 (cache.retry_ms - cache.resolved_ms) / 1000,
                                 cache.retry_distance > 0.0f ? std::format(" or within {:.0f} m", cache.retry_distance)
                                                             : std::string{}
                             );
            }
            else if (cache.brush != 0)
            {
                what = std::format("brush {} {}", DMK::format::format_address(cache.brush), cache.model);
            }
            else if (cache.owner == candidate.id)
            {
                what = "own mesh";
            }
            else
            {
                const std::uintptr_t owner = entity_from_id(cache.owner);
                what = std::format(
                    "entity {:#x} {} {}",
                    cache.owner,
                    owner != 0 ? entity_class_name(owner) : std::string{"-"},
                    owner != 0 ? entity_name(owner) : std::string{"-"}
                );
            }
            (void)DMK::log().try_log(
                DMK::LogLevel::Debug,
                "Interactables: mesh of id={:#x} {}{} class={} -> {}{} [{}]{}",
                candidate.id,
                interact_kind_name(cache.kind),
                cache.kind != candidate.kind ? std::format(" (was {})", interact_kind_name(candidate.kind)) : "",
                entity_class_name(entity),
                what,
                cache.none ? std::string{}
                           : std::format(
                                 " size={:.2f}x{:.2f}x{:.2f}",
                                 b.max.x - b.min.x,
                                 b.max.y - b.min.y,
                                 b.max.z - b.min.z
                             ),
                source_name(cache.source),
                cache.source == VisualSource::Own ? std::string{} : describe_links(entity)
            );
        }

        /** @brief True when a candidate may show as one of the kinds in @p mask. */
        [[nodiscard]] bool kind_wanted(const Candidate &candidate, std::uint32_t mask) noexcept
        {
            if ((candidate.traits & TRAIT_SEAT_OR_USE) != 0)
            {
                return (mask & (interact_kind_bit(InteractKind::Seat) | interact_kind_bit(InteractKind::UseSpot))) != 0;
            }
            return (mask & interact_kind_bit(candidate.kind)) != 0;
        }

        /**
         * @brief True when a book can be read in place (only such a book is a station), read once per book.
         */
        [[nodiscard]] bool book_readable(EntityId id, std::uintptr_t entity)
        {
            if (const auto it = s_readable_books.find(id); it != s_readable_books.end())
            {
                return it->second;
            }
            const bool readable = script_table_bool(entity, "Properties", "bIsDirectlyReadable").value_or(false);
            s_readable_books.emplace(id, readable);
            (void)DMK::log().try_log(
                DMK::LogLevel::Debug,
                "Interactables: book {:#x} {} {}",
                id,
                entity_name(entity),
                readable ? "can be read in place (station)" : "is an item, not a place"
            );
            return readable;
        }

        /**
         * @brief Logs that the game started or stopped keeping a candidate's mesh entity invisible.
         */
        void log_game_hidden(const Candidate &candidate, const VisualCache &cache)
        {
            if (!DMK::log().is_enabled(DMK::LogLevel::Debug))
            {
                return;
            }
            const std::uintptr_t owner = entity_from_id(cache.owner);
            const std::string what = std::format(
                "id={:#x} {} '{}'{}",
                cache.owner,
                owner != 0 ? entity_class_name(owner) : std::string{"-"},
                owner != 0 ? entity_name(owner) : std::string{"-"},
                cache.owner != candidate.id ? std::format(" (mesh of id={:#x})", candidate.id) : std::string{}
            );
            if (cache.game_hidden)
            {
                const std::optional<std::uint32_t> flags = owner != 0 ? entity_flags(owner) : std::nullopt;
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "Interactables: {} hidden by the game{} -> marker",
                    what,
                    flags.has_value() && !entity_flags_active(*flags) ? " (inactive)" : ""
                );
            }
            else
            {
                (void)DMK::log().try_log(DMK::LogLevel::Debug, "Interactables: {} shown again -> outline", what);
            }
        }

        /**
         * @brief Adds one candidate in reach to the targets, from its cached mesh, and queues its lookup when the mesh
         *        is unknown, due for a retry or gone.
         * @param queue Queue a lookup (a collect); false when the lookup just ran.
         */
        void place_target(
            const Candidate &candidate,
            std::uintptr_t entity,
            const game_structures::Vec3f &position,
            float distance,
            std::int64_t now,
            bool queue
        )
        {
            bool fallback = false;
            InteractTarget target{candidate.id, candidate.kind, marker_bounds(entity, position, fallback), distance};
            log_new_target(entity, target, fallback);
            auto it = s_visuals.find(candidate.id);
            // An object with a mesh of its own (a door, a book) needs no search: it is taken at once.
            if (it == s_visuals.end() && (candidate.traits & TRAIT_HELPER) == 0 && !fallback &&
                render_node_of(entity) != 0)
            {
                VisualCache own = resolve_visual(candidate, entity, position, distance, nullptr, nullptr);
                log_visual(entity, candidate, own);
                it = s_visuals.insert_or_assign(candidate.id, std::move(own)).first;
            }
            bool lookup = false;
            bool alive = false;
            if (it == s_visuals.end())
            {
                lookup = true;
            }
            else
            {
                VisualCache &cache = it->second;
                if (cache.gone)
                {
                    lookup = true;
                }
                else if (cache.none)
                {
                    lookup = now >= cache.retry_ms || (cache.retry_distance > 0.0f && distance <= cache.retry_distance);
                }
                else
                {
                    const bool was_game_hidden = cache.game_hidden;
                    switch (check_visual(cache))
                    {
                    case VisualState::Alive:
                        alive = true;
                        if (cache.game_hidden != was_game_hidden)
                        {
                            log_game_hidden(candidate, cache);
                        }
                        break;
                    case VisualState::Gone:
                        cache.gone = true;
                        lookup = true;
                        break;
                    case VisualState::Hidden:
                    default:
                        break;
                    }
                }
            }
            if (lookup && queue)
            {
                s_queue.push_back(QueuedLookup{candidate, distance});
            }
            InteractKind kind = candidate.kind;
            if ((candidate.traits & TRAIT_SEAT_OR_USE) != 0)
            {
                // Seat or use spot is decided by the lookup; until the first one ran, the target would show in a
                // colour it may lose a frame later.
                if (it == s_visuals.end())
                {
                    return;
                }
                kind = it->second.kind;
            }
            if ((s_collect_mask & interact_kind_bit(kind)) == 0)
            {
                return;
            }
            target.kind = kind;
            TargetState state{target, false, {}};
            if (alive)
            {
                const VisualCache &cache = it->second;
                // The marker sits on the mesh the player sees, not on the trigger above it.
                state.target.bounds = cache.bounds;
                state.target.has_mesh = true;
                state.target.mesh_brush = cache.owner == 0;
                // The outline state stays on the mesh (the visual below), so it is back the frame the game draws the
                // entity again; meanwhile the target keeps its marker.
                state.target.game_hidden = cache.owner != 0 && cache.game_hidden;
                state.has_visual = true;
                state.visual =
                    InteractVisual{cache.owner, cache.owner != 0 ? 0 : cache.brush, cache.bounds, kind, distance};
            }
            s_targets.push_back(state);
        }

        /** @brief Writes the targets, nearest first, and their meshes (one per mesh) to the caller's vectors. */
        void publish(std::vector<InteractTarget> &out, std::vector<InteractVisual> *visuals)
        {
            std::sort(
                s_targets.begin(),
                s_targets.end(),
                [](const TargetState &a, const TargetState &b) { return a.target.distance < b.target.distance; }
            );
            out.clear();
            out.reserve(s_targets.size());
            for (const TargetState &state : s_targets)
            {
                out.push_back(state.target);
            }
            if (visuals != nullptr)
            {
                visuals->clear();
                // Two triggers on one object (the seats of a bench) share its mesh; the nearer one keeps it.
                std::unordered_set<std::uint64_t> seen;
                for (const TargetState &state : s_targets)
                {
                    if (!state.has_visual)
                    {
                        continue;
                    }
                    const InteractVisual &v = state.visual;
                    const std::uint64_t key = v.entity_id != 0 ? v.entity_id : static_cast<std::uint64_t>(v.brush);
                    if (seen.insert(key).second)
                    {
                        visuals->push_back(v);
                    }
                }
            }
            if (out.size() != s_last_count)
            {
                s_last_count = out.size();
                std::array<std::size_t, INTERACT_KIND_COUNT> per_kind{};
                std::size_t meshes = 0;
                for (const TargetState &state : s_targets)
                {
                    ++per_kind[static_cast<std::size_t>(state.target.kind)];
                    meshes += state.has_visual ? 1 : 0;
                }
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "Interactables: {} marker(s) within {:.1f} m, {} with a mesh (door={} station={} bed={} seat={} "
                    "use={} other={}), {} lookup(s) queued",
                    out.size(),
                    s_collect_radius,
                    meshes,
                    per_kind[0],
                    per_kind[1],
                    per_kind[2],
                    per_kind[3],
                    per_kind[4],
                    per_kind[5],
                    s_queue.size() - std::min(s_queue_next, s_queue.size())
                );
            }
        }
    } // namespace

    std::string_view interact_kind_name(InteractKind kind) noexcept
    {
        switch (kind)
        {
        case InteractKind::Door:
            return "door";
        case InteractKind::Station:
            return "station";
        case InteractKind::Bed:
            return "bed";
        case InteractKind::Seat:
            return "seat";
        case InteractKind::UseSpot:
            return "use";
        case InteractKind::Other:
        default:
            return "other";
        }
    }

    DMK::Result<void> initialize_interactables()
    {
        if (!entity_access_available())
        {
            (void)DMK::log().try_log(
                DMK::LogLevel::Warning,
                "Interactables: entity access is unavailable; interactive objects are off"
            );
            return std::unexpected(DMK::Error{DMK::ErrorCode::NoMatch, "interactables/entity_access"});
        }
        reset_interactables();
        s_available.store(true, std::memory_order_release);
        (void)DMK::log().try_log(
            DMK::LogLevel::Info,
            "Interactables: ready ({} interactive classes, walk budget {} us per frame)",
            INTERACTIVE_CLASSES.size(),
            WALK_BUDGET_US
        );
        return {};
    }

    void shutdown_interactables() noexcept
    {
        s_available.store(false, std::memory_order_release);
        reset_interactables();
        reset_prefab_templates();
    }

    bool interactables_available() noexcept
    {
        return s_available.load(std::memory_order_acquire);
    }

    void reset_interactables() noexcept
    {
        // Class objects live for the session, but a new level reuses entity ids and addresses. The template index
        // describes the game's prefab files and stays.
        s_walk.cancel();
        s_batch.clear();
        s_pending.clear();
        s_candidates.clear();
        s_survey.clear();
        s_surveying = false;
        s_logged_ids.clear();
        s_visuals.clear();
        s_readable_books.clear();
        s_targets.clear();
        s_queue.clear();
        s_queue_next = 0;
        s_walk_valid = false;
        s_last_count = static_cast<std::size_t>(-1);
    }

    void request_interactables_survey() noexcept
    {
        s_survey_requested.store(true, std::memory_order_relaxed);
    }

    bool advance_interactables(const game_structures::Vec3f &center, float radius)
    {
        DMK_PROFILE_FUNCTION();
        if (!interactables_available() || !(radius > 0.0f))
        {
            return false;
        }
        const std::int64_t start_us = steady_us();
        if (!s_walk.active())
        {
            if (!walk_due())
            {
                return false;
            }
            if (!s_walk.begin())
            {
                return false;
            }
            s_pending.clear();
            s_batch.clear();
            s_pending_center = center;
            s_pending_radius = radius;
            s_walk_entities = 0;
            s_walk_frames = 0;
            s_walk_busy_us = 0;
            s_walk_started_ms = start_us / 1000;
            s_surveying = s_survey_requested.exchange(false, std::memory_order_relaxed);
            s_survey.clear();
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

    std::size_t collect_interactables(
        const game_structures::Vec3f &center,
        float radius,
        std::uint32_t kind_mask,
        std::vector<InteractTarget> &out,
        std::vector<InteractVisual> *visuals
    )
    {
        DMK_PROFILE_FUNCTION();
        out.clear();
        if (visuals != nullptr)
        {
            visuals->clear();
        }
        s_targets.clear();
        s_queue.clear();
        s_queue_next = 0;
        if (!interactables_available() || kind_mask == 0 || !(radius > 0.0f))
        {
            return 0;
        }
        s_collect_mask = kind_mask;
        s_collect_radius = radius;
        const std::int64_t now = steady_us() / 1000;
        for (const Candidate &candidate : s_candidates)
        {
            // The walk's position rules out the rest of the level before anything is read.
            if (!kind_wanted(candidate, kind_mask) ||
                distance_between(candidate.position, center) > radius + WALK_MARGIN)
            {
                continue;
            }
            // Re-resolving by id drops an entity removed since the walk before anything reads it.
            const std::uintptr_t entity = entity_from_id(candidate.id);
            if (entity == 0 || entity_is_hidden(entity))
            {
                continue;
            }
            const std::optional<game_structures::Vec3f> position = entity_world_position(entity);
            if (!position.has_value())
            {
                continue;
            }
            const float distance = distance_between(*position, center);
            if (distance > radius)
            {
                continue;
            }
            if ((candidate.traits & TRAIT_READABLE) != 0 && !book_readable(candidate.id, entity))
            {
                continue;
            }
            place_target(candidate, entity, *position, distance, now, true);
        }
        // The nearest objects are looked up first, so what the player stands at outlines within a frame or two.
        std::sort(
            s_queue.begin(),
            s_queue.end(),
            [](const QueuedLookup &a, const QueuedLookup &b) { return a.distance < b.distance; }
        );
        publish(out, visuals);
        return out.size();
    }

    bool advance_interactable_visuals(
        const game_structures::Vec3f &center,
        std::vector<InteractTarget> &out,
        std::vector<InteractVisual> *visuals,
        std::int64_t budget_us,
        OutlinedNodesFn outlined
    )
    {
        DMK_PROFILE_FUNCTION();
        if (s_queue_next >= s_queue.size() || budget_us <= 0 || !interactables_available())
        {
            return false;
        }
        // A trigger is not searched for while the index that would place it is still being read (a few frames once
        // per session); it keeps its marker meanwhile.
        if (prefab_template_state() == TemplateIndexState::Building)
        {
            return false;
        }
        const std::int64_t start_us = steady_us();
        const std::int64_t now = start_us / 1000;
        bool changed = false;
        std::size_t looked_up = 0;
        while (s_queue_next < s_queue.size())
        {
            const std::int64_t elapsed = steady_us() - start_us;
            // A lookup starts only when the recent average still fits what is left of the budget; the first of a
            // frame always runs, so a slow lookup cannot stall the queue.
            if (looked_up != 0 &&
                (elapsed >= budget_us || elapsed + static_cast<std::int64_t>(s_lookup_cost_us) > budget_us))
            {
                break;
            }
            const QueuedLookup item = s_queue[s_queue_next++];
            const std::uintptr_t entity = entity_from_id(item.candidate.id);
            if (entity == 0 || entity_is_hidden(entity))
            {
                continue;
            }
            const std::optional<game_structures::Vec3f> position = entity_world_position(entity);
            if (!position.has_value())
            {
                continue;
            }
            const float distance = distance_between(*position, center);
            const auto previous = s_visuals.find(item.candidate.id);
            const std::int64_t lookup_start = steady_us();
            VisualCache cache = resolve_visual(
                item.candidate,
                entity,
                *position,
                distance,
                previous != s_visuals.end() ? &previous->second : nullptr,
                outlined
            );
            const auto cost = static_cast<float>(steady_us() - lookup_start);
            s_lookup_cost_us = 0.75f * s_lookup_cost_us + 0.25f * cost;
            ++looked_up;
            log_visual(entity, item.candidate, cache);
            // A retry that found what the last lookup found (most often nothing again) leaves the target as the collect
            // placed it, so the targets are not republished and the outlines not re-applied for it.
            const bool differs = previous == s_visuals.end() || visual_differs(previous->second, cache);
            s_visuals.insert_or_assign(item.candidate.id, std::move(cache));
            if (!differs)
            {
                continue;
            }
            std::erase_if(
                s_targets,
                [&item](const TargetState &state) { return state.target.entity_id == item.candidate.id; }
            );
            if (distance <= s_collect_radius)
            {
                place_target(item.candidate, entity, *position, distance, now, false);
            }
            changed = true;
        }
        if (changed)
        {
            publish(out, visuals);
        }
        return changed;
    }

} // namespace HenrySenses
