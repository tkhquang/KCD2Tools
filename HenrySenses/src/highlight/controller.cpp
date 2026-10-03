/**
 * @file highlight/controller.cpp
 * @brief The highlight groups' activation and fade, gating, the loot, herb and interactable scans, the assignment of
 *        what they find to groups, and marker submission (main thread).
 */

#include "highlight/controller.hpp"
#include "aob_resolver.hpp"
#include "config.hpp"
#include "game_state.hpp"
#include "global_state.hpp"
#include "offset_heal.hpp"
#include "rtti_types.hpp"
#include "detect/loot_scanner.hpp"
#include "engine/engine_env.hpp"
#include "engine/entity_access.hpp"
#include "engine/herb_scan.hpp"
#include "engine/interactables.hpp"
#include "engine/prefab_templates.hpp"
#include "highlight/groups.hpp"
#include "highlight/loot_effects.hpp"
#include "highlight/registry.hpp"
#include "highlight/world_highlights.hpp"
#include "render/aux_markers.hpp"
#include "render/engine_silhouette.hpp"
#include "render/focus_effect.hpp"
#include "render/herb_outline.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace HenrySenses
{
    namespace
    {
        constexpr std::int64_t MAINTAIN_INTERVAL_MS = 500;
        // Herbs do not move and a pick only zeroes plants, so the set is refreshed at a walking pace.
        constexpr std::int64_t HERB_RESCAN_MS = 1000;
        // Interactive objects rarely move (a door swings inside its own bounds), so their markers refresh as often.
        // The walk behind them runs on its own, slower clock (see advance_interactables).
        constexpr std::int64_t INTERACT_RESCAN_MS = 1000;
        // Main-thread time a frame gives the interactables: the walk's slice and the collect come first, the mesh
        // lookups get what is left.
        constexpr std::int64_t INTERACT_FRAME_BUDGET_US = 1000;
        // Main-thread time a frame gives the prefab template index while it is being built (once per session, while
        // a group with interactables is enabled).
        constexpr std::int64_t TEMPLATE_BUILD_BUDGET_US = 1000;
        constexpr std::int64_t STATUS_INTERVAL_MS = 30000;
        // While no group can show, the tick reads the group set this often (a saved INI also marks the config dirty,
        // which runs a full tick at once).
        constexpr std::int64_t IDLE_GROUPS_REFRESH_MS = 1000;
        // The targets each scan serves.
        // Pattern targets need the loot scan (its index finds the entities they match); Model patterns also query
        // the static world objects.
        constexpr std::uint32_t LOOT_TARGETS =
            target_bit(Target::Corpses) | target_bit(Target::Carcasses) | target_bit(Target::Items) |
            target_bit(Target::Containers) | target_bit(Target::Npcs) | target_bit(Target::Animals) |
            target_bit(Target::Horses) | target_bit(Target::Dogs) | target_bit(Target::Critters) |
            target_bit(Target::Class) | target_bit(Target::Name) | target_bit(Target::Model);
        // Static world objects matched by Model patterns do not move, so they are searched for at a walking pace.
        constexpr std::int64_t MODEL_RESCAN_MS = 1500;
        constexpr std::uint32_t INTERACT_TARGETS = target_bit(Target::Doors) | target_bit(Target::Workstations) |
                                                   target_bit(Target::Beds) | target_bit(Target::Seats) |
                                                   target_bit(Target::UseSpots) | target_bit(Target::Objects);
        // Containers, critters and pattern matches live in the loot index.
        constexpr std::uint32_t INDEXED_TARGETS = target_bit(Target::Containers) | target_bit(Target::Animals) |
                                                  target_bit(Target::Critters) | target_bit(Target::Class) |
                                                  target_bit(Target::Name) | target_bit(Target::Model);
        // The pattern targets: a group with one reads the entity of every object it is offered.
        constexpr std::uint32_t PATTERN_TARGETS =
            target_bit(Target::Class) | target_bit(Target::Name) | target_bit(Target::Model);

        /**
         * @brief A group that is showing right now.
         */
        struct ActiveGroup
        {
            const HighlightGroup *group{nullptr};
            /// Its place in the file (the last matching group wins an object).
            std::size_t order{0};
            /// The part of its fade its objects' words carry (the rest is the composite strength).
            float intensity{1.0f};
            /// Its whole fade in [0, 1] (1 while switched on).
            float fade{1.0f};
        };

        /**
         * @brief What a group gives an object it claims.
         */
        struct Assignment
        {
            std::uint32_t color{0};
            float radius{20.0f};
            float intensity{1.0f};
            float fade{1.0f};
            GroupStyle style{GroupStyle::Outline};
            std::size_t order{0};
            std::uint16_t effect{0};
            float effect_scale{1.0f};
        };

        /**
         * @brief Main-thread controller state. Only controller_tick() touches it.
         */
        struct TickState
        {
            std::uintptr_t last_player{0};

            // Which groups show and since when; a change rescans at once.
            std::uint64_t activation_key{0};
            // The intensity step of every showing group; a change re-applies the words.
            std::uint64_t intensity_key{0};

            // A loot scan result is held (some group wants loot, creatures or classes).
            bool scan_active{false};
            std::int64_t next_scan_ms{0};
            // Per-category counts of the last applied set, to log a summary when they change.
            std::array<std::size_t, 12> scan_counts{};
            // Scan timing since the last status line.
            std::size_t scans_timed{0};
            double scan_ms_total{0.0};
            double scan_ms_max{0.0};
            // Totals of the scan parts: actors, items, containers.
            std::array<double, 3> scan_part_ms{};

            // No group could show and nothing was left to take down, so the last tick ran only the idle checks.
            bool idle{false};
            // Idle ticks since the last status line.
            std::size_t idle_ticks{0};
            std::int64_t next_groups_refresh_ms{0};

            std::uint32_t last_state_mask{0};

            std::int64_t next_maintain_ms{0};
            std::int64_t next_status_ms{0};
            std::size_t markers_last_frame{0};
            std::int64_t next_herb_scan_ms{0};
            std::int64_t next_model_scan_ms{0};
            std::int64_t next_interact_scan_ms{0};

            // [Render] SeeThrough the highlighted render objects were filled with (-1: not seen yet).
            int applied_see_through{-1};
        };

        /**
         * @enum ShutdownPhase
         * @brief The main-thread half of shutdown, handed between the teardown thread and the tick.
         */
        enum class ShutdownPhase : int
        {
            /// Normal ticking.
            Running,
            /// One tick owns the controller state.
            Ticking,
            /// The teardown thread asked the next tick to run the main-thread half.
            Requested,
            /// A tick claimed the request and is running it.
            InProgress,
            /// The main-thread half finished.
            Done,
            /// The main-thread cleanup threw before it restored all owned state.
            Failed,
            /// The teardown thread withdrew the request; every later tick does nothing.
            Abandoned,
        };

        TickState s_state{};
        std::atomic<bool> s_config_dirty{true};
        // A saved INI asks the next tick for the state report (LogLevel = DEBUG).
        std::atomic<bool> s_report_requested{false};
        std::atomic<ShutdownPhase> s_shutdown_phase{ShutdownPhase::Running};
        std::atomic<bool> s_ticked{false};
        // Captured before the frame hook arms. The teardown thread must not inspect the live registries.
        bool s_inherited_highlights{false};
        std::atomic<std::uint64_t> s_tick_count{0};

        // The group set of this tick and the groups showing, in file order (the last match wins an object).
        std::shared_ptr<const GroupSet> s_groups;
        std::vector<ActiveGroup> s_active;
        // The fade every showing group shares, carried by the composite strength each frame (smooth); 0 when none
        // shows.
        float s_composite_intensity = 0.0f;

        // Per-frame scratch, reused so the marker path does not allocate every frame.
        std::vector<MarkerTarget> s_marker_targets;
        std::vector<MarkerRequest> s_marker_requests;
        // Unpicked herb clusters around the player, refreshed every HERB_RESCAN_MS while herbs are shown.
        std::vector<HerbCluster> s_herb_clusters;
        // The plants of those clusters, outlined through the engine silhouette when it runs.
        std::vector<HerbPlant> s_herb_plants;
        std::vector<HerbOutlineItem> s_herb_items;
        // Interactive objects around the player, refreshed every INTERACT_RESCAN_MS while they are shown, and
        // patched in place as their meshes are looked up (a slice per frame).
        std::vector<InteractTarget> s_interact_targets;
        // The meshes of those objects, outlined through the engine silhouette.
        std::vector<InteractVisual> s_interact_visuals;
        // Bumped whenever the two change, so the markers drawn for them are decided again.
        std::uint64_t s_interact_generation = 0;

        /**
         * @brief What one interactive object draws as a marker: its box and the colour of the group that claims it,
         *        decided when the targets or the groups change; a frame only scales the colour by that group's fade.
         */
        struct InteractMark
        {
            game_structures::Aabb bounds{};
            std::uint32_t color{0};
            /// The claiming group's place in the file (ActiveGroup::order).
            std::size_t order{0};
        };
        std::vector<InteractMark> s_interact_marks;
        // What the marks were decided from (groups, fades' steps, targets, backend); 0 = not decided.
        std::uint64_t s_interact_marks_key = 0;
        std::vector<WorldHighlightRequest> s_world_requests;
        // Static world objects a Model pattern matched, refreshed every MODEL_RESCAN_MS while such a group shows.
        std::vector<ModelBrush> s_model_brushes;
        std::vector<ModelBrush> s_model_scratch;
        // The interactables, static objects and herb clusters whose group shows an Effect.
        std::vector<EffectTarget> s_effect_targets;
        // The records of the last loot scan, and the ones a group claimed.
        std::vector<HighlightRequest> s_scan_records;
        std::vector<HighlightRequest> s_assigned;
        // apply_loot_assignments scratch: the claiming group's order and the slot in s_assigned, per entity.
        std::unordered_map<EntityId, std::pair<std::size_t, std::size_t>> s_claimed;

        /**
         * @brief What decides whether one enabled group can show, copied out of the group set so the idle check reads
         *        one small array instead of every group.
         */
        struct GroupSwitch
        {
            const GroupActivation *activation{nullptr};
            GroupMode mode{GroupMode::Pulse};
            float duration{0.0f};
            float fade_out{0.0f};
        };
        std::vector<GroupSwitch> s_switches;
        // The group set the switches (and the two flags below) were copied from; held, so the activations they point
        // at stay alive and a new set can never reuse its address.
        std::shared_ptr<const GroupSet> s_switches_of;
        // An enabled group lists interactables (the template index builds), or targets the loot index holds.
        bool s_switch_interactables = false;
        bool s_switch_indexed = false;

        /**
         * @brief Returns the steady-clock time in milliseconds (the clock the group hotkeys stamp pulses with).
         */
        [[nodiscard]] std::int64_t now_ms() noexcept
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch()
            )
                .count();
        }

        [[nodiscard]] RenderBackend backend() noexcept
        {
            return static_cast<RenderBackend>(settings().backend.load(std::memory_order_relaxed));
        }

        /**
         * @brief The engine silhouette path renders the set (the stage is live and the backend allows it).
         */
        [[nodiscard]] bool engine_words_wanted() noexcept
        {
            const RenderBackend b = backend();
            return (b == RenderBackend::Engine || b == RenderBackend::Auto) && engine_silhouette_available();
        }

        /** @brief True for the styles drawn by the outline half of the composite (they share its strength). */
        [[nodiscard]] constexpr bool outlined(GroupStyle style) noexcept
        {
            return style == GroupStyle::Outline || style == GroupStyle::OutlineFill;
        }

        /**
         * @brief Markers render the set (or back up the silhouette for meshes that cannot show one).
         */
        [[nodiscard]] bool markers_wanted() noexcept
        {
            const RenderBackend b = backend();
            return b == RenderBackend::Markers || (b == RenderBackend::Auto && !engine_silhouette_available());
        }

        [[nodiscard]] std::uint64_t mix(std::uint64_t hash, std::uint64_t value) noexcept
        {
            return (hash ^ value) * 0x100000001B3ull;
        }

        // Groups

        /**
         * @brief The fade-out curve: 1 at @p since = 0, easing to 0 at @p length (smoothstep), 0 after.
         */
        [[nodiscard]] float fade_tail(std::int64_t since, std::int64_t length) noexcept
        {
            if (length <= 0 || since < 0 || since >= length)
            {
                return 0.0f;
            }
            const float t = static_cast<float>(since) / static_cast<float>(length);
            return 1.0f - t * t * (3.0f - 2.0f * t);
        }

        /**
         * @brief A group's intensity now: 1 while switched on or within a pulse's Duration, then easing to 0 over its
         *        FadeOut (after a pulse, a toggle off or a hold release).
         * @param activation The group's switch state.
         * @param mode Its Mode.
         * @param duration Its Duration, in seconds.
         * @param fade_out_seconds Its FadeOut, in seconds.
         * @param now Steady milliseconds.
         */
        [[nodiscard]] float activation_intensity(
            const GroupActivation &activation,
            GroupMode mode,
            float duration,
            float fade_out_seconds,
            std::int64_t now
        ) noexcept
        {
            const auto fade_out = static_cast<std::int64_t>(fade_out_seconds * 1000.0f);
            auto after_off = [&activation, now, fade_out]()
            {
                const std::int64_t off = activation.off_started_ms.load(std::memory_order_relaxed);
                return off != 0 ? fade_tail(now - off, fade_out) : 0.0f;
            };
            switch (mode)
            {
            case GroupMode::Always:
                return 1.0f;
            case GroupMode::Toggle:
                return activation.toggled.load(std::memory_order_relaxed) ? 1.0f : after_off();
            case GroupMode::Hold:
                return activation.held.load(std::memory_order_relaxed) ? 1.0f : after_off();
            case GroupMode::Pulse:
            default:
                break;
            }
            const std::int64_t started = activation.pulse_started_ms.load(std::memory_order_relaxed);
            if (started == 0 || now < started)
            {
                return 0.0f;
            }
            const auto full = static_cast<std::int64_t>(duration * 1000.0f);
            return now - started < full ? 1.0f : fade_tail(now - started - full, fade_out);
        }

        /** @brief activation_intensity() of a group. */
        [[nodiscard]] float group_intensity(const HighlightGroup &group, std::int64_t now) noexcept
        {
            return activation_intensity(*group.activation, group.mode, group.duration, group.fade_out, now);
        }

        /**
         * @brief Copies the switches of the enabled groups out of s_groups when it is a set they were not copied from.
         */
        void refresh_group_switches()
        {
            if (s_groups == nullptr || s_groups == s_switches_of)
            {
                return;
            }
            s_switches_of = s_groups;
            s_switches.clear();
            s_switch_interactables = false;
            s_switch_indexed = false;
            for (const HighlightGroup &group : s_groups->groups)
            {
                if (!group.enabled)
                {
                    continue;
                }
                s_switches.push_back(GroupSwitch{group.activation.get(), group.mode, group.duration, group.fade_out});
                s_switch_interactables = s_switch_interactables || (group.target_mask & INTERACT_TARGETS) != 0;
                s_switch_indexed = s_switch_indexed || (group.target_mask & INDEXED_TARGETS) != 0;
            }
        }

        /**
         * @brief Rebuilds the list of showing groups for this tick and splits their fades between the composite and
         *        the words.
         * @details The Outline composite scales by a strength every frame, so the fade every showing group shares
         *          goes there and stays smooth; the words carry only a group's fade relative to the brightest one,
         *          which changes nothing while the groups fade together. The Fill composite ignores the strength, so
         *          with Fill the words carry the whole fade.
         * @return True when which groups show, or a pulse start, changed.
         */
        bool update_active_groups(std::int64_t now, std::uint32_t state_mask)
        {
            s_groups = current_groups();
            s_active.clear();
            std::uint64_t activation = 0xCBF29CE484222325ull;
            float brightest_outline = 0.0f;
            if (settings().enabled.load(std::memory_order_relaxed))
            {
                for (std::size_t order = 0; order < s_groups->groups.size(); ++order)
                {
                    const HighlightGroup &group = s_groups->groups[order];
                    // Each group hides in the game states its own gates name.
                    if (!group.enabled || (state_mask & group.gate_mask) != 0)
                    {
                        continue;
                    }
                    const float value = group_intensity(group, now);
                    if (value <= 0.0f)
                    {
                        continue;
                    }
                    s_active.push_back(ActiveGroup{&group, order, value, value});
                    if (outlined(group.style))
                    {
                        brightest_outline = std::max(brightest_outline, value);
                    }
                    activation = mix(activation, reinterpret_cast<std::uintptr_t>(group.activation.get()));
                    activation = mix(
                        activation,
                        static_cast<std::uint64_t>(group.activation->pulse_started_ms.load(std::memory_order_relaxed))
                    );
                }
            }
            // The outlined groups share the composite strength; Fill and Box groups carry their whole fade themselves.
            s_composite_intensity = s_active.empty() ? 0.0f : (brightest_outline > 0.0f ? brightest_outline : 1.0f);
            // A new group set (an INI change) counts as a change too.
            activation = mix(activation, reinterpret_cast<std::uintptr_t>(s_groups.get()));
            std::uint64_t intensity = activation;
            for (ActiveGroup &active : s_active)
            {
                if (outlined(active.group->style))
                {
                    active.intensity = std::clamp(active.fade / brightest_outline, 0.0f, 1.0f);
                }
                intensity = mix(intensity, static_cast<std::uint64_t>(std::ceil(active.intensity * INTENSITY_STEPS)));
            }
            const bool changed = activation != s_state.activation_key;
            s_state.activation_key = activation;
            s_state.intensity_key = intensity;
            return changed;
        }

        /** @brief True when a showing group has the given style. */
        [[nodiscard]] bool style_showing(GroupStyle style) noexcept
        {
            return std::any_of(
                s_active.begin(),
                s_active.end(),
                [style](const ActiveGroup &active) { return active.group->style == style; }
            );
        }

        /**
         * @brief The focus strength: the brightest fade among the showing groups with Focus = true, 0 when none shows
         *        or the silhouette mask the focus is cut from is not drawn (markers backend).
         */
        [[nodiscard]] float focus_strength() noexcept
        {
            if (!engine_words_wanted())
            {
                return 0.0f;
            }
            float strength = 0.0f;
            for (const ActiveGroup &active : s_active)
            {
                if (active.group->focus)
                {
                    strength = std::max(strength, active.fade);
                }
            }
            return strength;
        }

        /** @brief The union of the targets of the showing groups. */
        [[nodiscard]] std::uint32_t active_targets() noexcept
        {
            std::uint32_t mask = 0;
            for (const ActiveGroup &active : s_active)
            {
                mask |= active.group->target_mask;
            }
            return mask;
        }

        /** @brief The widest reach among the showing groups with any of @p targets (0 when none). */
        [[nodiscard]] float active_radius(std::uint32_t targets) noexcept
        {
            float radius = 0.0f;
            for (const ActiveGroup &active : s_active)
            {
                if ((active.group->target_mask & targets) != 0)
                {
                    radius = std::max(radius, active.group->radius);
                }
            }
            return radius;
        }

        /**
         * @brief True when a showing group matches patterns (a Class, Name or Model target, or an Only or Except
         *        pattern): only then does claiming an object read its entity.
         */
        [[nodiscard]] bool patterns_showing() noexcept
        {
            return std::any_of(
                s_active.begin(),
                s_active.end(),
                [](const ActiveGroup &active)
                {
                    const HighlightGroup &group = *active.group;
                    return (group.target_mask & PATTERN_TARGETS) != 0 || !group.only_patterns.empty() ||
                           !group.except_patterns.empty();
                }
            );
        }

        /**
         * @brief Finds the showing group that claims an object: the last one in the file, so a general group comes
         *        first and a group placed after it overrides it.
         * @param object What the object is (kind, tags, and the class, name and model its patterns ask for).
         * @param distance Its distance in metres, negative when unknown.
         */
        [[nodiscard]] std::optional<Assignment> assign(ObjectFacts &object, float distance)
        {
            for (auto it = s_active.rbegin(); it != s_active.rend(); ++it)
            {
                const ActiveGroup &active = *it;
                const HighlightGroup &group = *active.group;
                if (distance >= 0.0f && distance > group.radius)
                {
                    continue;
                }
                if (const std::optional<std::uint32_t> color = group.claims(object); color.has_value())
                {
                    return Assignment{
                        *color,
                        group.radius,
                        active.intensity,
                        active.fade,
                        group.style,
                        active.order,
                        group.effect,
                        group.effect_scale,
                    };
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] Target target_of(LootCategory category) noexcept
        {
            switch (category)
            {
            case LootCategory::HumanCorpse:
                return Target::Corpses;
            case LootCategory::AnimalCarcass:
                return Target::Carcasses;
            case LootCategory::Item:
                return Target::Items;
            case LootCategory::Container:
                return Target::Containers;
            case LootCategory::Herb:
                return Target::Herbs;
            case LootCategory::LiveNpc:
                return Target::Npcs;
            case LootCategory::LiveAnimal:
                return Target::Animals;
            case LootCategory::Horse:
                return Target::Horses;
            case LootCategory::Dog:
                return Target::Dogs;
            case LootCategory::Critter:
                return Target::Critters;
            case LootCategory::Custom:
            default:
                return Target::Class;
            }
        }

        [[nodiscard]] Target target_of(InteractKind kind) noexcept
        {
            switch (kind)
            {
            case InteractKind::Door:
                return Target::Doors;
            case InteractKind::Station:
                return Target::Workstations;
            case InteractKind::Bed:
                return Target::Beds;
            case InteractKind::Seat:
                return Target::Seats;
            case InteractKind::UseSpot:
                return Target::UseSpots;
            case InteractKind::Other:
            default:
                return Target::Objects;
            }
        }

        /** @brief The InteractKind mask the showing groups want. */
        [[nodiscard]] std::uint32_t interact_kind_mask() noexcept
        {
            const std::uint32_t targets = active_targets();
            std::uint32_t mask = 0;
            for (std::size_t kind = 0; kind < INTERACT_KIND_COUNT; ++kind)
            {
                if ((targets & target_bit(target_of(static_cast<InteractKind>(kind)))) != 0)
                {
                    mask |= interact_kind_bit(static_cast<InteractKind>(kind));
                }
            }
            return mask;
        }

        // Loot scan

        /**
         * @brief Builds the ApplyOptions for the current backend and render settings.
         * @param player_position The player position for the distance fade.
         */
        [[nodiscard]] ApplyOptions apply_options(std::optional<game_structures::Vec3f> player_position) noexcept
        {
            const LiveSettings &s = settings();
            ApplyOptions options{};
            options.write_words = engine_words_wanted();
            // Without see-through an occluded object shows nothing, so occlusion culling may skip it.
            options.render_always =
                s.render_always.load(std::memory_order_relaxed) && s.see_through.load(std::memory_order_relaxed);
            options.player_position = player_position;
            return options;
        }

        /**
         * @brief Returns the player's world position.
         */
        [[nodiscard]] std::optional<game_structures::Vec3f> player_position() noexcept
        {
            const std::uintptr_t entity = player_entity(s_state.last_player);
            return entity != 0 ? entity_world_position(entity) : std::nullopt;
        }

        /**
         * @brief Drops the loot scan result and restores every node it highlighted.
         * @param reason Log reason.
         */
        void stop_loot_highlight(std::string_view reason)
        {
            s_state.scan_active = false;
            s_state.next_scan_ms = 0;
            s_scan_records.clear();
            s_assigned.clear();
            clear_loot_effects(EffectChannel::Loot, false);
            if (highlighted_count() > 0)
            {
                clear_all_highlights();
                (void)DMK::log().try_log(DMK::LogLevel::Debug, "Controller: loot highlight stopped ({})", reason);
            }
        }

        /**
         * @brief Logs a one-line summary of the applied set when its per-category counts changed (or always when
         *        asked).
         */
        void log_scan_summary(const LootScanStats &stats)
        {
            std::array<std::size_t, 12> counts{};
            for (const HighlightRequest &record : s_assigned)
            {
                const auto index = static_cast<std::size_t>(record.category);
                if (index < counts.size())
                {
                    ++counts[index];
                }
            }
            if (counts == s_state.scan_counts)
            {
                return;
            }
            s_state.scan_counts = counts;
            std::string text;
            for (std::size_t index = 1; index < counts.size(); ++index)
            {
                if (counts[index] != 0)
                {
                    text += std::format(
                        "{}{}={}",
                        text.empty() ? "" : " ",
                        loot_category_name(static_cast<LootCategory>(index)),
                        counts[index]
                    );
                }
            }
            (void)DMK::log().try_log(
                DMK::LogLevel::Debug,
                "Controller: loot scan: {} of {} records shown ({}) of {} candidates in range; "
                "{} actors, {} items, {} indexed{}; {:.2f} ms",
                s_assigned.size(),
                stats.records,
                text.empty() ? "none" : text,
                stats.candidates,
                stats.actors,
                stats.items,
                stats.indexed,
                stats.index_ready ? "" : " (index not ready)",
                stats.milliseconds
            );
        }

        /**
         * @brief Hands every record of the last scan to the showing group that claims it and applies the result.
         */
        void apply_loot_assignments()
        {
            DMK_PROFILE_FUNCTION();
            s_assigned.clear();
            // An entity may come as several records (a container and a pattern match); the last group in the file
            // that claims any of them wins it.
            s_claimed.clear();
            // Only a pattern reads the entity (its class, name or model), so without one showing it is not looked up.
            const bool patterns = patterns_showing();
            for (const HighlightRequest &record : s_scan_records)
            {
                const std::optional<Target> kind = record.category == LootCategory::Custom
                                                       ? std::nullopt
                                                       : std::optional<Target>{target_of(record.category)};
                ObjectFacts object(kind, record.flags, patterns ? entity_from_id(record.entity_id) : 0, 0);
                const std::optional<Assignment> assignment = assign(object, record.distance);
                if (!assignment.has_value())
                {
                    continue;
                }
                HighlightRequest request = record;
                request.color = assignment->color;
                request.fade_radius = assignment->radius;
                request.intensity = assignment->intensity;
                request.marker_intensity = assignment->fade;
                request.style = assignment->style;
                request.effect = assignment->effect;
                request.effect_scale = assignment->effect_scale;
                const auto [it, inserted] =
                    s_claimed.try_emplace(record.entity_id, assignment->order, s_assigned.size());
                if (inserted)
                {
                    s_assigned.push_back(request);
                }
                else if (assignment->order > it->second.first)
                {
                    it->second.first = assignment->order;
                    s_assigned[it->second.second] = request;
                }
            }
            apply_batch(s_assigned, apply_options(player_position()));
            sync_loot_effects(s_assigned);
        }

        /**
         * @brief What the showing groups want the loot scan to look for, around the player.
         * @return The options, or std::nullopt without a player position.
         */
        [[nodiscard]] std::optional<LootScanOptions> loot_scan_options()
        {
            const std::optional<game_structures::Vec3f> origin = player_position();
            if (!origin.has_value())
            {
                return std::nullopt;
            }
            const std::uint32_t targets = active_targets();
            const float radius = active_radius(LOOT_TARGETS);
            auto wants = [targets](Target target) { return (targets & target_bit(target)) != 0; };
            // Animals covers horses, dogs and critters too.
            LootScanOptions options{};
            options.origin = *origin;
            options.radius = std::clamp(radius, 2.0f, 100.0f);
            options.human_corpses = wants(Target::Corpses);
            options.animals = wants(Target::Carcasses);
            options.items = wants(Target::Items);
            options.containers = wants(Target::Containers);
            options.live_npcs = wants(Target::Npcs);
            options.live_animals = wants(Target::Animals);
            options.horses = wants(Target::Horses) || wants(Target::Animals);
            options.dogs = wants(Target::Dogs) || wants(Target::Animals);
            options.critters = wants(Target::Critters) || wants(Target::Animals);
            return options;
        }

        /**
         * @brief Runs one loot scan for the showing groups and replaces the highlighted set with the result.
         */
        void run_scan(const LootScanOptions &options)
        {
            DMK_PROFILE_FUNCTION();
            const LootScanStats stats = run_loot_scan(options, s_scan_records);
            ++s_state.scans_timed;
            s_state.scan_ms_total += stats.milliseconds;
            s_state.scan_ms_max = std::max(s_state.scan_ms_max, stats.milliseconds);
            s_state.scan_part_ms[0] += stats.actor_ms;
            s_state.scan_part_ms[1] += stats.item_ms;
            s_state.scan_part_ms[2] += stats.index_ms;
            // The scan spread its work (new objects to classify) and finishes it next tick.
            if (stats.more)
            {
                s_state.next_scan_ms = 0;
            }
            apply_loot_assignments();
            log_scan_summary(stats);
        }

        /**
         * @brief Runs the loot scan every ScanIntervalMs while a showing group wants loot, creatures or classes, keeps
         *        the scan's near sets current in the ticks between, and re-applies the held result when a group's fade
         *        steps.
         * @details A tick runs either a scan or a near pick, never both (together they made the longest ticks). A due
         *          scan waits a tick for a pick it cannot do without (a near set never taken, taken for another reach,
         *          or over an index that was just replaced), or for one left overdue because every tick had a scan due;
         *          every other pick runs in a tick without a scan.
         * @param rescan Scan now (a group switched on or a pulse started).
         * @param restyle Re-apply the held result (an intensity step changed).
         * @return True when a scan or a pick ran this tick.
         */
        bool update_loot(std::int64_t now, bool rescan, bool restyle)
        {
            DMK_PROFILE_FUNCTION();
            if ((active_targets() & LOOT_TARGETS) == 0)
            {
                if (s_state.scan_active || highlighted_count() > 0)
                {
                    stop_loot_highlight("no group wants loot");
                }
                return false;
            }
            const std::optional<LootScanOptions> options = loot_scan_options();
            if (!options.has_value())
            {
                stop_loot_highlight("no player position");
                return false;
            }
            if (!s_state.scan_active || rescan || now >= s_state.next_scan_ms)
            {
                if (advance_loot_picks(*options, true))
                {
                    // The scan runs next tick, over the new near set.
                    s_state.next_scan_ms = 0;
                    if (restyle && s_state.scan_active)
                    {
                        apply_loot_assignments();
                    }
                    return true;
                }
                s_state.scan_active = true;
                s_state.next_scan_ms =
                    now + std::clamp(settings().scan_interval_ms.load(std::memory_order_relaxed), 50, 5000);
                run_scan(*options);
                return true;
            }
            const bool picked = advance_loot_picks(*options, false);
            if (restyle)
            {
                apply_loot_assignments();
            }
            return picked;
        }

        // State report

        /**
         * @brief Logs the groups, the scanner, the registry and the render state (a saved INI at LogLevel = DEBUG).
         */
        void write_state_report()
        {
            DMK::Logger &logger = DMK::log();
            const LiveSettings &s = settings();
            std::string active;
            for (const ActiveGroup &group : s_active)
            {
                active += std::format("{}{}({:.2f})", active.empty() ? "" : ", ", group.group->name, group.fade);
            }
            logger.info(
                "Controller: ticks={} player={} situations=[{}] showing=[{}] scan={} backend={} "
                "engineWords={} markers={}",
                s_tick_count.load(),
                DMK::format::format_address(s_state.last_player),
                state_list_text(s_state.last_state_mask),
                active,
                s_state.scan_active,
                s.backend.load(),
                engine_words_wanted(),
                markers_wanted()
            );
            log_groups();
            log_engine_silhouette_state();
            log_focus_effect_state();
            log_loot_scanner_state();
            log_registry_state();
            log_loot_effects_state();
            logger.info(
                "Controller: markers drawn last frame={}, herb clusters={} (scan {}), interactables={} "
                "(mask {:#x})",
                s_state.markers_last_frame,
                s_herb_clusters.size(),
                herb_scan_available() ? "ready" : "off",
                s_interact_targets.size(),
                interact_kind_mask()
            );
        }

        void log_state_report() noexcept
        {
            try
            {
                write_state_report();
            }
            catch (...)
            {
                (void)DMK::log().log_noexcept(DMK::LogLevel::Error, "Controller: state report failed");
            }
        }

        // Render-object refresh

        /**
         * @brief Refreshes the highlighted render objects when [Render] SeeThrough changes.
         * @details The depth-test flag, like the word, reaches a persistent render object only when it is filled.
         * @return True when it changed (the interactive set re-applies its options).
         */
        bool refresh_on_see_through_change()
        {
            const int see_through = settings().see_through.load(std::memory_order_relaxed) ? 1 : 0;
            if (see_through == s_state.applied_see_through)
            {
                return false;
            }
            const bool first = s_state.applied_see_through < 0;
            s_state.applied_see_through = see_through;
            if (first)
            {
                return false;
            }
            const std::size_t refreshed = refresh_highlight_render_objects() + refresh_world_highlight_render_objects();
            (void)DMK::log().try_log(
                DMK::LogLevel::Info,
                "Controller: SeeThrough {}; {} render object(s) refreshed",
                see_through != 0 ? "on" : "off",
                refreshed
            );
            return true;
        }

        // Herbs and interactables

        /**
         * @brief Herbs are outlined through the engine silhouette (the stage runs, the static-mesh hook is in and
         *        CStatObj::Render resolved).
         */
        [[nodiscard]] bool herb_outline_wanted() noexcept
        {
            return engine_words_wanted() && engine_brush_outline_available() && feature_ready(Feature::HerbOutline);
        }

        [[nodiscard]] float
        distance_to_bounds(const game_structures::Vec3f &from, const game_structures::Aabb &b) noexcept
        {
            const float dx = 0.5f * (b.min.x + b.max.x) - from.x;
            const float dy = 0.5f * (b.min.y + b.max.y) - from.y;
            const float dz = 0.5f * (b.min.z + b.max.z) - from.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        /**
         * @brief Refreshes the herb clusters around the player, or drops them when no showing group wants herbs.
         * @param radius The widest herb reach among the showing groups (0: none).
         * @param now Steady milliseconds.
         * @param rescan Scan now (a group switched on or a pulse started).
         * @return True when the set was refreshed or dropped this tick.
         */
        bool update_herb_clusters(float radius, std::int64_t now, bool rescan)
        {
            DMK_PROFILE_FUNCTION();
            if (radius <= 0.0f || !herb_scan_available())
            {
                if (!s_herb_clusters.empty() || !s_herb_plants.empty())
                {
                    s_herb_clusters.clear();
                    s_herb_plants.clear();
                    (void)DMK::log().try_log(DMK::LogLevel::Debug, "Controller: herb markers cleared");
                }
                s_state.next_herb_scan_ms = 0;
                return true;
            }
            if (!rescan && now < s_state.next_herb_scan_ms)
            {
                return false;
            }
            s_state.next_herb_scan_ms = now + HERB_RESCAN_MS;
            const std::optional<game_structures::Vec3f> position = player_position();
            if (!position.has_value())
            {
                s_herb_clusters.clear();
                s_herb_plants.clear();
                return true;
            }
            (void)collect_herb_clusters(
                *position,
                std::clamp(radius, 2.0f, 100.0f),
                s_herb_clusters,
                herb_outline_wanted() ? &s_herb_plants : nullptr
            );
            if (!herb_outline_wanted())
            {
                s_herb_plants.clear();
            }
            return true;
        }

        /**
         * @brief Publishes the herbs to outline, each in the colour of the group that claims it, or clears them.
         * @param refreshed The herb set or the group fades changed this tick.
         */
        void update_herb_outline(bool refreshed)
        {
            DMK_PROFILE_FUNCTION();
            if (s_herb_plants.empty() || !herb_outline_wanted())
            {
                if (herb_outline_count() != 0)
                {
                    publish_herb_outline({});
                    (void)DMK::log().try_log(DMK::LogLevel::Debug, "Controller: herb outlines cleared");
                }
                return;
            }
            if (!refreshed)
            {
                return;
            }
            const ApplyOptions options = apply_options(player_position());
            s_herb_items.clear();
            for (const HerbPlant &plant : s_herb_plants)
            {
                ObjectFacts object(Target::Herbs, 0, 0, 0);
                const std::optional<Assignment> assignment = assign(object, plant.distance);
                // A Box group's herbs keep the cluster brackets only.
                if (assignment.has_value() && assignment->style != GroupStyle::Box)
                {
                    s_herb_items.push_back(
                        HerbOutlineItem{
                            plant.world,
                            plant.stat_obj,
                            styled_word(
                                assignment->color,
                                plant.distance,
                                assignment->radius,
                                assignment->intensity,
                                assignment->style
                            )
                        }
                    );
                }
            }
            const std::size_t before = herb_outline_count();
            publish_herb_outline(s_herb_items);
            if (before != s_herb_items.size())
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "Controller: {} herb(s) outlined (of {} in range)",
                    s_herb_items.size(),
                    s_herb_plants.size()
                );
            }
        }

        /** @brief True when an enabled group (showing or not) lists interactables. */
        [[nodiscard]] bool interactables_enabled() noexcept
        {
            return s_groups != nullptr && std::any_of(
                                              s_groups->groups.begin(),
                                              s_groups->groups.end(),
                                              [](const HighlightGroup &group)
                                              { return group.enabled && (group.target_mask & INTERACT_TARGETS) != 0; }
                                          );
        }

        /** @brief Drops the interactive objects; true when there were any. */
        bool drop_interactables()
        {
            if (s_interact_targets.empty() && s_interact_visuals.empty())
            {
                return false;
            }
            s_interact_targets.clear();
            s_interact_visuals.clear();
            ++s_interact_generation;
            return true;
        }

        /**
         * @brief Refreshes the interactive objects around the player, or drops them when no showing group wants any.
         * @details While a group with interactables is enabled, the prefab template index is built first (once per
         *          session, a slice per frame), so its first press already places the triggers' meshes. While one
         *          shows, the walk advances a slice, the markers are collected every INTERACT_RESCAN_MS, and the
         *          trigger meshes still unknown are looked up with what is left of the frame's budget, nearest first.
         * @param radius The widest interactable reach among the showing groups (0: none).
         * @param now Steady milliseconds.
         * @param rescan Collect now (a group switched on or a pulse started).
         * @return True when the set was refreshed, patched or dropped this tick.
         */
        bool update_interactables(float radius, std::int64_t now, bool rescan)
        {
            DMK_PROFILE_FUNCTION();
            if (s_state.last_player != 0 && interactables_available() && interactables_enabled())
            {
                advance_prefab_templates(TEMPLATE_BUILD_BUDGET_US);
            }
            const std::uint32_t mask = interact_kind_mask();
            if (radius <= 0.0f || mask == 0 || !interactables_available())
            {
                const bool dropped = drop_interactables();
                if (dropped)
                {
                    (void)DMK::log().try_log(DMK::LogLevel::Debug, "Controller: interactable markers cleared");
                }
                s_state.next_interact_scan_ms = 0;
                return dropped;
            }
            const std::optional<game_structures::Vec3f> position = player_position();
            if (!position.has_value())
            {
                return drop_interactables();
            }
            const auto start = std::chrono::steady_clock::now();
            const float reach = std::clamp(radius, 2.0f, 100.0f);
            // A due world walk runs a slice every frame; its end refreshes the markers at once.
            const bool walked = advance_interactables(*position, reach);
            bool refreshed = false;
            if (walked || rescan || now >= s_state.next_interact_scan_ms)
            {
                s_state.next_interact_scan_ms = now + INTERACT_RESCAN_MS;
                (void)collect_interactables(*position, reach, mask, s_interact_targets, &s_interact_visuals);
                refreshed = true;
            }
            const std::int64_t spent =
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
            // A found mesh patches the targets in place; the caller re-applies the outlines once for the frame.
            if (advance_interactable_visuals(
                    *position,
                    s_interact_targets,
                    &s_interact_visuals,
                    INTERACT_FRAME_BUDGET_US - spent,
                    &world_highlight_nodes_in
                ))
            {
                refreshed = true;
            }
            if (refreshed)
            {
                ++s_interact_generation;
            }
            return refreshed;
        }

        /**
         * @brief Refreshes the static world objects a showing group's Model pattern matches, or drops them.
         * @param now Steady milliseconds.
         * @param rescan Search now (a group switched on or a pulse started).
         * @return True when the set was refreshed or dropped this tick.
         */
        bool update_model_brushes(std::int64_t now, bool rescan)
        {
            DMK_PROFILE_FUNCTION();
            const float radius = active_radius(target_bit(Target::Model));
            if (radius <= 0.0f || s_groups->patterns.models.empty())
            {
                const bool had = !s_model_brushes.empty();
                s_model_brushes.clear();
                s_state.next_model_scan_ms = 0;
                return had;
            }
            if (!rescan && now < s_state.next_model_scan_ms)
            {
                return false;
            }
            s_state.next_model_scan_ms = now + MODEL_RESCAN_MS;
            const std::optional<game_structures::Vec3f> position = player_position();
            if (!position.has_value())
            {
                s_model_brushes.clear();
                return true;
            }
            collect_model_brushes(
                *position,
                std::clamp(radius, 2.0f, 100.0f),
                s_groups->patterns.models,
                s_model_brushes,
                s_model_scratch
            );
            if (s_model_scratch.size() != s_model_brushes.size())
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "Controller: {} static object(s) match a Model pattern",
                    s_model_scratch.size()
                );
            }
            s_model_brushes.swap(s_model_scratch);
            return true;
        }

        /**
         * @brief Outlines the meshes of the interactive objects and of the static objects a Model pattern matched,
         *        each in the colour and style of the group that claims it, or restores them when that path is off.
         * @param refreshed Those sets, the group fades or the render options changed this tick.
         */
        void update_world_highlights(bool refreshed)
        {
            DMK_PROFILE_FUNCTION();
            if (!engine_words_wanted() || (s_interact_visuals.empty() && s_model_brushes.empty()))
            {
                if (world_highlight_count() != 0)
                {
                    clear_world_highlights();
                }
                return;
            }
            if (!refreshed)
            {
                return;
            }
            // Without the CBrush::Render hook a brush's word would never reach the silhouette pass; it keeps its box.
            const bool brushes = engine_brush_outline_available();
            s_world_requests.clear();
            auto add = [](std::uintptr_t entity_id,
                          std::uintptr_t brush,
                          const game_structures::Aabb &bounds,
                          float distance,
                          const Assignment &assignment)
            {
                // A Box group's objects get brackets only (draw_frame_markers).
                if (assignment.style != GroupStyle::Box)
                {
                    s_world_requests.push_back(
                        WorldHighlightRequest{
                            static_cast<EntityId>(entity_id),
                            brush,
                            bounds,
                            assignment.color,
                            distance,
                            assignment.radius,
                            assignment.intensity,
                            assignment.style
                        }
                    );
                }
            };
            for (const InteractVisual &visual : s_interact_visuals)
            {
                if (visual.entity_id == 0 && !brushes)
                {
                    continue;
                }
                ObjectFacts object(
                    target_of(visual.kind),
                    0,
                    visual.entity_id != 0 ? entity_from_id(visual.entity_id) : 0,
                    visual.entity_id != 0 ? 0 : visual.brush
                );
                if (const std::optional<Assignment> assignment = assign(object, visual.distance))
                {
                    add(visual.entity_id, visual.brush, visual.bounds, visual.distance, *assignment);
                }
            }
            if (brushes)
            {
                for (const ModelBrush &hit : s_model_brushes)
                {
                    ObjectFacts object(std::nullopt, 0, 0, hit.brush);
                    if (const std::optional<Assignment> assignment = assign(object, hit.distance))
                    {
                        add(0, hit.brush, hit.bounds, hit.distance, *assignment);
                    }
                }
            }
            apply_world_highlights(s_world_requests, apply_options(player_position()));
        }

        /**
         * @brief Gives the interactive objects, the static objects a Model pattern matched and the herb clusters the
         *        Effect of the group that claims them (free-standing emitters, whatever the group's Style), or drops
         *        them when no showing group has one.
         * @param refreshed Those sets or the group fades changed this tick.
         */
        void update_world_effects(bool refreshed)
        {
            DMK_PROFILE_FUNCTION();
            const bool any_effect = std::any_of(
                s_active.begin(),
                s_active.end(),
                [](const ActiveGroup &active) { return active.group->effect != 0; }
            );
            if (!any_effect)
            {
                if (!s_effect_targets.empty())
                {
                    s_effect_targets.clear();
                    sync_world_effects(s_effect_targets);
                }
                return;
            }
            if (!refreshed)
            {
                return;
            }
            s_effect_targets.clear();
            // Keys tell the object kinds apart: an interactable's trigger entity, a brush, a herb cluster.
            constexpr std::uint64_t interact_key = std::uint64_t{1} << 56;
            constexpr std::uint64_t brush_key = std::uint64_t{2} << 56;
            constexpr std::uint64_t herb_key = std::uint64_t{3} << 56;
            constexpr std::uint64_t key_mask = (std::uint64_t{1} << 56) - 1;
            auto add = [](std::uint64_t key, const game_structures::Aabb &bounds, const Assignment &assignment)
            {
                if (assignment.effect != 0 && assignment.fade >= effect_min_intensity())
                {
                    s_effect_targets.push_back(EffectTarget{key, bounds, assignment.effect, assignment.effect_scale});
                }
            };
            const bool patterns = patterns_showing();
            for (const InteractTarget &target : s_interact_targets)
            {
                ObjectFacts object(target_of(target.kind), 0, patterns ? entity_from_id(target.entity_id) : 0, 0);
                if (const std::optional<Assignment> assignment = assign(object, target.distance))
                {
                    add(interact_key | target.entity_id, target.bounds, *assignment);
                }
            }
            for (const ModelBrush &hit : s_model_brushes)
            {
                ObjectFacts object(std::nullopt, 0, 0, hit.brush);
                if (const std::optional<Assignment> assignment = assign(object, hit.distance))
                {
                    add(brush_key | (hit.brush & key_mask), hit.bounds, *assignment);
                }
            }
            if (const std::optional<game_structures::Vec3f> origin = player_position(); origin.has_value())
            {
                for (const HerbCluster &cluster : s_herb_clusters)
                {
                    const game_structures::Vec3f centre{
                        (cluster.bounds.min.x + cluster.bounds.max.x) * 0.5f,
                        (cluster.bounds.min.y + cluster.bounds.max.y) * 0.5f,
                        (cluster.bounds.min.z + cluster.bounds.max.z) * 0.5f,
                    };
                    const float dx = centre.x - origin->x;
                    const float dy = centre.y - origin->y;
                    const float dz = centre.z - origin->z;
                    ObjectFacts object(Target::Herbs, 0, 0, 0);
                    if (const std::optional<Assignment> assignment =
                            assign(object, std::sqrt(dx * dx + dy * dy + dz * dz)))
                    {
                        // A cluster is the plants of one species one pick harvests: its species and its centre to the
                        // decimetre name it from one herb scan to the next.
                        const auto decimetres = [](float value)
                        { return static_cast<std::uint64_t>(static_cast<std::int64_t>(std::lround(value * 10.0f))); };
                        const auto species = static_cast<std::uint64_t>(static_cast<std::uint32_t>(cluster.group));
                        const std::uint64_t hash = (species * 0x9E3779B97F4A7C15ull) ^
                                                   (decimetres(centre.x) * 0xC2B2AE3D27D4EB4Full) ^
                                                   (decimetres(centre.y) * 0x165667B19E3779F9ull);
                        add(herb_key | (hash & key_mask), cluster.bounds, *assignment);
                    }
                }
            }
            sync_world_effects(s_effect_targets);
        }

        // Markers

        /**
         * @brief Appends the marker requests of the highlighted entities the backend draws as markers.
         */
        void append_entity_markers()
        {
            const bool all = backend() == RenderBackend::Markers || !engine_words_wanted();
            collect_marker_targets(s_marker_targets);
            for (const MarkerTarget &target : s_marker_targets)
            {
                if (!all && !target.marker_preferred)
                {
                    continue;
                }
                const std::uintptr_t entity = entity_from_id(target.entity_id);
                if (entity == 0)
                {
                    continue;
                }
                const std::optional<game_structures::Aabb> bounds = entity_world_bounds(entity);
                if (!bounds.has_value())
                {
                    continue;
                }
                s_marker_requests.push_back(MarkerRequest{*bounds, target.color_word});
            }
        }

        /** @brief The fade of the showing group at @p order in the file, 0 when it no longer shows. */
        [[nodiscard]] float active_fade(std::size_t order) noexcept
        {
            for (const ActiveGroup &active : s_active)
            {
                if (active.order == order)
                {
                    return active.fade;
                }
            }
            return 0.0f;
        }

        /**
         * @brief Appends the markers of the interactive objects that keep a box: those without a mesh to outline (or
         *        whose mesh entity the game keeps invisible for now), a Box group's, and all of them when boxes are
         *        asked for.
         * @details Which group claims each object, and whether it keeps a box, change only with the targets, the
         *          showing groups or the backend, so they are decided then (a group match reads the entity); a frame
         *          only scales each colour by its group's current fade.
         * @param all_boxes Every object keeps a box (markers backend, or no engine outline).
         * @param brushes A brush mesh can be outlined (the static-mesh render hook is in).
         */
        void append_interact_markers(bool all_boxes, bool brushes)
        {
            std::uint64_t key = mix(s_state.activation_key, s_interact_generation);
            key = mix(key, (all_boxes ? 1u : 0u) | (brushes ? 2u : 0u));
            // 0 is the "not decided" value.
            key = key == 0 ? 1 : key;
            if (key != s_interact_marks_key)
            {
                s_interact_marks_key = key;
                s_interact_marks.clear();
                for (const InteractTarget &target : s_interact_targets)
                {
                    // An entity mesh the game keeps invisible cannot show its outline until the game draws it again.
                    const bool outlined = target.has_mesh && !target.game_hidden && (!target.mesh_brush || brushes);
                    ObjectFacts object(target_of(target.kind), 0, entity_from_id(target.entity_id), 0);
                    const std::optional<Assignment> assignment = assign(object, target.distance);
                    if (assignment.has_value() && (all_boxes || !outlined || assignment->style == GroupStyle::Box))
                    {
                        s_interact_marks.push_back(InteractMark{target.bounds, assignment->color, assignment->order});
                    }
                }
            }
            for (const InteractMark &mark : s_interact_marks)
            {
                if (const float fade = active_fade(mark.order); fade > 0.0f)
                {
                    s_marker_requests.push_back(MarkerRequest{mark.bounds, intensity_scaled(mark.color, fade)});
                }
            }
        }

        /**
         * @brief Queues the markers for this frame: the highlighted entities the backend draws as markers, and every
         *        herb cluster (a merged-mesh plant has no render node of its own to outline) and every interactive
         *        object that keeps a box.
         */
        void draw_frame_markers()
        {
            DMK_PROFILE_FUNCTION();
            s_state.markers_last_frame = 0;
            if (!aux_markers_available() || s_active.empty())
            {
                return;
            }
            s_marker_requests.clear();
            if (highlighted_count() != 0)
            {
                append_entity_markers();
            }
            // An object whose mesh is outlined keeps a box only when boxes are asked for.
            const bool all_boxes = backend() == RenderBackend::Markers || !engine_words_wanted();
            const std::optional<game_structures::Vec3f> position = player_position();
            if (position.has_value())
            {
                const bool no_herb_outline = all_boxes || herb_outline_count() == 0;
                for (const HerbCluster &cluster : s_herb_clusters)
                {
                    ObjectFacts object(Target::Herbs, 0, 0, 0);
                    const std::optional<Assignment> assignment =
                        assign(object, distance_to_bounds(*position, cluster.bounds));
                    // A cluster none of whose plants could be outlined (no model) keeps its bracket.
                    if (assignment.has_value() &&
                        (no_herb_outline || assignment->style == GroupStyle::Box || cluster.marker_fallback))
                    {
                        s_marker_requests.push_back(
                            MarkerRequest{cluster.bounds, intensity_scaled(assignment->color, assignment->fade)}
                        );
                    }
                }
            }
            const bool brushes = engine_brush_outline_available();
            append_interact_markers(all_boxes, brushes);
            for (const ModelBrush &hit : s_model_brushes)
            {
                ObjectFacts object(std::nullopt, 0, 0, hit.brush);
                const std::optional<Assignment> assignment = assign(object, hit.distance);
                if (assignment.has_value() && (all_boxes || !brushes || assignment->style == GroupStyle::Box))
                {
                    s_marker_requests.push_back(
                        MarkerRequest{hit.bounds, intensity_scaled(assignment->color, assignment->fade)}
                    );
                }
            }
            if (!s_marker_requests.empty())
            {
                // Each colour carries its group's whole fade.
                s_state.markers_last_frame = draw_markers(s_marker_requests, 1.0f);
            }
        }

        // Shutdown

        /**
         * @brief Main-thread half of shutdown.
         */
        void shutdown_on_main_thread()
        {
            release_focus_effect();
            stop_loot_highlight("shutdown");
            clear_all_highlights();
            // Every slot now, a load still pending included: the next generation never sees these entries.
            shutdown_loot_effects();
            clear_world_highlights();
            reset_loot_scanner();
            s_herb_clusters.clear();
            s_herb_plants.clear();
            publish_herb_outline({});
            s_interact_targets.clear();
            s_interact_visuals.clear();
            s_model_brushes.clear();
            publish_silhouette_params(SilhouetteFrameParams{.intensity = 0.0f});
            s_active.clear();
            s_switches.clear();
            s_switches_of.reset();
            s_groups.reset();
        }

        /**
         * @brief Runs the claimed main-thread cleanup; failures retain the generation instead of allowing unload.
         */
        void run_main_thread_shutdown() noexcept
        {
            try
            {
                shutdown_on_main_thread();
            }
            catch (...)
            {
                (void)DMK::log().log_noexcept(DMK::LogLevel::Error, "Controller: main-thread shutdown failed");
                s_shutdown_phase.store(ShutdownPhase::Failed, std::memory_order_release);
                return;
            }
            s_shutdown_phase.store(ShutdownPhase::Done, std::memory_order_release);
        }

        /**
         * @brief Waits until cleanup succeeds, fails, or the timeout expires.
         * @param timeout The bound.
         * @return True when the phase reached Done.
         */
        [[nodiscard]] bool wait_for_shutdown_done(std::chrono::milliseconds timeout) noexcept
        {
            const std::int64_t deadline = now_ms() + timeout.count();
            while (now_ms() < deadline)
            {
                const ShutdownPhase phase = s_shutdown_phase.load(std::memory_order_acquire);
                if (phase == ShutdownPhase::Done || phase == ShutdownPhase::Failed)
                {
                    return phase == ShutdownPhase::Done;
                }
                Sleep(5);
            }
            return s_shutdown_phase.load(std::memory_order_acquire) == ShutdownPhase::Done;
        }

        // Tick

        /**
         * @brief Drops everything that belongs to the previous player (a level change or the main menu).
         */
        void on_player_changed(std::uintptr_t c_player)
        {
            if (s_state.last_player != 0)
            {
                // Entities that are already gone need nothing; a node that survives the change (a same-level reload,
                // a transient player gap) is still the node the mod moved and is restored in full, or it would stay
                // out of the octree with nobody tracking it.
                stop_loot_highlight("level change");
            }
            // A level unload may already have destroyed the free emitters; each is only killed once found live again.
            clear_all_loot_effects(false);
            release_focus_effect();
            s_state.scan_counts = {};
            s_state.last_player = c_player;
            (void)drop_interactables();
            clear_world_highlights();
            s_herb_plants.clear();
            s_herb_clusters.clear();
            s_model_brushes.clear();
            publish_herb_outline({});
            reset_interactables();
            reset_loot_scanner();
            if (c_player != 0)
            {
                (void)DMK::log().try_log(DMK::LogLevel::Info, "Controller: local player 0x{:016X}", c_player);
            }
            else
            {
                (void)DMK::log().try_log(DMK::LogLevel::Info, "Controller: local player gone (menu or loading)");
            }
        }

        /**
         * @brief True when no group can show and nothing is left on screen or to take down, so the tick only has to
         *        notice a key, a saved INI or a level change.
         * @details A group that can show (switched on, held, within a pulse or a fade-out, or Always) needs the full
         *          tick even when a HideIn situation hides it (only the full tick reads the situations), and so does
         *          anything a full tick still has to clear.
         */
        [[nodiscard]] bool tick_can_idle(std::int64_t now)
        {
            if (s_config_dirty.load(std::memory_order_relaxed) || s_report_requested.load(std::memory_order_relaxed) ||
                !s_active.empty() || s_state.scan_active || highlighted_count() != 0 || world_highlight_count() != 0 ||
                herb_outline_count() != 0 || !s_herb_clusters.empty() || !s_herb_plants.empty() ||
                !s_interact_targets.empty() || !s_interact_visuals.empty() || !s_model_brushes.empty() ||
                loot_effect_count() != 0)
            {
                return false;
            }
            if (now >= s_state.next_groups_refresh_ms)
            {
                s_state.next_groups_refresh_ms = now + IDLE_GROUPS_REFRESH_MS;
                s_groups = current_groups();
            }
            refresh_group_switches();
            if (!settings().enabled.load(std::memory_order_relaxed))
            {
                return true;
            }
            return std::none_of(
                s_switches.begin(),
                s_switches.end(),
                [now](const GroupSwitch &group)
                {
                    return activation_intensity(*group.activation, group.mode, group.duration, group.fade_out, now) >
                           0.0f;
                }
            );
        }

        /**
         * @brief The tick while nothing can show: the background work a group's first press relies on (the prefab
         *        template index, the loot index, a focus release still sending its white mask), nothing else.
         * @details The situations are not read and nothing is scanned, applied, drawn or published (the composite
         *          strength stays at the 0 the last full tick published, so the render hooks stay idle too).
         */
        void idle_tick(std::int64_t now)
        {
            DMK_PROFILE_FUNCTION();
            if (!s_state.idle)
            {
                s_state.idle = true;
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "Controller: idle (no group can show); the tick watches the keys, the level and the index only"
                );
            }
            ++s_state.idle_ticks;
            if (s_switch_interactables && interactables_available())
            {
                advance_prefab_templates(TEMPLATE_BUILD_BUDGET_US);
            }
            if (s_switch_indexed)
            {
                (void)advance_loot_index(false, s_groups->patterns, player_position());
            }
            update_focus_effect(
                0.0f,
                settings().focus_darken.load(std::memory_order_relaxed),
                settings().focus_tint.load(std::memory_order_relaxed),
                settings().focus_fade_in.load(std::memory_order_relaxed),
                now
            );
        }

        /**
         * @brief Logs the status line every STATUS_INTERVAL_MS (LogLevel = DEBUG): what shows, the scans, the picks,
         *        the applies and the index since the last one.
         */
        void log_status(std::int64_t now, std::uintptr_t c_player)
        {
            if (now < s_state.next_status_ms)
            {
                return;
            }
            s_state.next_status_ms = now + STATUS_INTERVAL_MS;
            auto part_average = [](std::size_t part)
            {
                return s_state.scans_timed != 0 ? s_state.scan_part_ms[part] / static_cast<double>(s_state.scans_timed)
                                                : 0.0;
            };
            const LootWorkStats work = take_loot_work_stats();
            const RegistryApplyStats applies = take_registry_apply_stats();
            (void)DMK::log().try_log(
                DMK::LogLevel::Debug,
                "Controller: status player=0x{:016X} tracked={} showing={} situations=[{}] engineWords={} markers={} "
                "idleTicks={} scans={} avg={:.2f} ms max={:.2f} ms (actors {:.2f}, items {:.2f}, containers {:.2f}); "
                "picks={} avg={:.2f} ms max={:.2f} ms; applies full={} unchanged={}; index: counters unchanged={}, "
                "recent walks={} ({} entities read, {} added), full walks={} ({:.2f} ms busy)",
                c_player,
                highlighted_count(),
                s_active.size(),
                state_list_text(s_state.last_state_mask),
                engine_words_wanted(),
                markers_wanted(),
                s_state.idle_ticks,
                s_state.scans_timed,
                s_state.scans_timed != 0 ? s_state.scan_ms_total / static_cast<double>(s_state.scans_timed) : 0.0,
                s_state.scan_ms_max,
                part_average(0),
                part_average(1),
                part_average(2),
                work.picks,
                work.picks != 0 ? work.pick_ms / static_cast<double>(work.picks) : 0.0,
                work.pick_ms_max,
                applies.full,
                applies.skipped,
                work.gate_skips,
                work.recent_walks,
                work.recent_entities,
                work.recent_added,
                work.full_walks,
                work.walk_ms
            );
            s_state.scan_part_ms = {};
            s_state.scans_timed = 0;
            s_state.scan_ms_total = 0.0;
            s_state.scan_ms_max = 0.0;
            s_state.idle_ticks = 0;
        }

        /**
         * @brief Validates the entity system's lookup once for the tick and drops it when the tick ends, however it
         *        ends.
         */
        struct EntityTickScope
        {
            EntityTickScope() noexcept { begin_entity_tick(); }
            ~EntityTickScope() { end_entity_tick(); }
            EntityTickScope(const EntityTickScope &) = delete;
            EntityTickScope &operator=(const EntityTickScope &) = delete;
        };

        /**
         * @brief The tick body.
         */
        void tick()
        {
            DMK_PROFILE_FUNCTION();
            const std::int64_t now = now_ms();
            s_tick_count.fetch_add(1, std::memory_order_relaxed);
            // The identity checks' image stamp is read once here rather than on every check.
            refresh_game_types();
            const EntityTickScope entity_scope;
            if (!s_ticked.exchange(true, std::memory_order_acq_rel))
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Info,
                    "Controller: first main-thread tick (thread {})",
                    GetCurrentThreadId()
                );
                // Entries left by a refused teardown of this image: no tick state refers to them any more, so their
                // nodes are restored before this init applies anything of its own.
                if (const std::size_t inherited = highlighted_count(); inherited != 0)
                {
                    clear_all_highlights();
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Info,
                        "Controller: restored {} highlight(s) left by an earlier teardown of this "
                        "image",
                        inherited
                    );
                }
                if (const std::size_t inherited = world_highlight_count(); inherited != 0)
                {
                    clear_world_highlights();
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Info,
                        "Controller: restored {} object outline(s) left by an earlier teardown of "
                        "this image",
                        inherited
                    );
                }
                if (const std::size_t inherited = loot_effect_count(); inherited != 0)
                {
                    clear_all_loot_effects(true);
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Info,
                        "Controller: removed {} loot effect(s) left by an earlier teardown of this image",
                        inherited
                    );
                }
            }

            offset_heal_tick();
            const std::uintptr_t c_player = resolve_c_player();
            if (c_player != s_state.last_player)
            {
                on_player_changed(c_player);
            }
            maintain_loot_effects();

            const bool see_through_changed = refresh_on_see_through_change();
            if (c_player != 0 && tick_can_idle(now))
            {
                idle_tick(now);
                log_status(now, c_player);
                return;
            }
            bool any_active = false;
            if (c_player != 0)
            {
                // A saved INI applies at once: the next tick scans again.
                const bool config_changed = s_config_dirty.exchange(false, std::memory_order_relaxed);
                if (s_state.idle)
                {
                    // Loot changes and minigames can go unseen while the controller is idle.
                    s_state.idle = false;
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Debug,
                        "Controller: full ticks resume (a group can show, or the INI was saved)"
                    );
                    flush_loot_verdicts("the first full tick after an idle stretch");
                }

                const std::uint32_t state_mask = poll_game_state(c_player);
                if (state_mask != s_state.last_state_mask)
                {
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Debug,
                        "Controller: situations [{}] -> [{}]",
                        state_list_text(s_state.last_state_mask),
                        state_list_text(state_mask)
                    );
                }
                // Lockpicking can change a container's verdict before its 5 s cache expires. Leaving a minigame
                // drops the remembered verdicts, as does saving the INI.
                if ((s_state.last_state_mask & ~state_mask & state_bit(GameState::Minigame)) != 0)
                {
                    flush_loot_verdicts("left a minigame");
                }
                else if (config_changed)
                {
                    flush_loot_verdicts("INI saved");
                }
                s_state.last_state_mask = state_mask;

                const std::uint64_t previous_intensity = s_state.intensity_key;
                const bool activation_changed = update_active_groups(now, state_mask);
                refresh_group_switches();
                const bool restyle = s_state.intensity_key != previous_intensity || see_through_changed;
                const bool rescan = activation_changed || config_changed;
                // The survey of the entity classes around the player reads every entity's position (~20 ms of
                // main-thread time over a dozen frames), so only a saved INI asks for it
                // (controller_notify_config_changed), never a group key.
                any_active = !s_active.empty();

                const bool scanned = update_loot(now, rescan, restyle);

                // The container index follows the entity system while any enabled group lists something it holds,
                // so a new press finds the containers at once. Keep index work out of scan or pick ticks to bound
                // the main-thread work per frame. A finished walk rescans at once.
                if (!scanned && s_switch_indexed &&
                    advance_loot_index(s_state.scan_active, s_groups->patterns, player_position()) &&
                    s_state.scan_active)
                {
                    s_state.next_scan_ms = 0;
                }
                if (s_report_requested.exchange(false, std::memory_order_relaxed) &&
                    DMK::log().is_enabled(DMK::LogLevel::Debug))
                {
                    log_state_report();
                }

                const bool herbs_refreshed =
                    update_herb_clusters(active_radius(target_bit(Target::Herbs)), now, rescan);
                const bool interact_refreshed = update_interactables(active_radius(INTERACT_TARGETS), now, rescan);
                const bool models_refreshed = update_model_brushes(now, rescan);
                update_world_highlights(interact_refreshed || models_refreshed || restyle || rescan);
                update_herb_outline(herbs_refreshed || restyle || rescan);
                update_world_effects(interact_refreshed || models_refreshed || herbs_refreshed || restyle || rescan);

                if (now >= s_state.next_maintain_ms)
                {
                    s_state.next_maintain_ms = now + MAINTAIN_INTERVAL_MS;
                    maintain_highlights();
                    maintain_world_highlights();
                }
                // A group its HideIn hides is not showing, so it draws no marker (and darkens nothing).
                draw_frame_markers();
                update_focus_effect(
                    focus_strength(),
                    settings().focus_darken.load(std::memory_order_relaxed),
                    settings().focus_tint.load(std::memory_order_relaxed),
                    settings().focus_fade_in.load(std::memory_order_relaxed),
                    now
                );
            }
            else
            {
                s_active.clear();
                release_focus_effect();
                (void)update_herb_clusters(0.0f, now, false);
                (void)update_interactables(0.0f, now, false);
                update_world_highlights(true);
                update_herb_outline(true);
            }

            // The interior opacity reaches only the OutlineFill words (their interior flag); the stock composite, used
            // when the mod's shader cannot load, tints every outline with it.
            const bool interior = style_showing(GroupStyle::OutlineFill);
            publish_silhouette_params(
                SilhouetteFrameParams{
                    .intensity = any_active ? s_composite_intensity : 0.0f,
                    .interior_opacity =
                        interior ? std::clamp(settings().fill_opacity.load(std::memory_order_relaxed), 0.0f, 1.0f)
                                 : 0.0f,
                }
            );

            log_status(now, c_player);
        }
    } // namespace

    DMK::Result<void> initialize_controller()
    {
        s_state = TickState{};
        s_config_dirty.store(true, std::memory_order_relaxed);
        s_shutdown_phase.store(ShutdownPhase::Running, std::memory_order_relaxed);
        s_ticked.store(false, std::memory_order_relaxed);
        s_inherited_highlights = highlighted_count() != 0 || world_highlight_count() != 0 || loot_effect_count() != 0;
        s_report_requested.store(false, std::memory_order_relaxed);
        s_groups = current_groups();
        s_active.clear();
        return {};
    }

    void controller_tick() noexcept
    {
        ShutdownPhase expected = ShutdownPhase::Running;
        if (s_shutdown_phase.compare_exchange_strong(expected, ShutdownPhase::Ticking, std::memory_order_acq_rel))
        {
            try
            {
                tick();
            }
            catch (const std::exception &e)
            {
                (void)DMK::log().try_log(DMK::LogLevel::Error, "Controller: tick failed: {}", e.what());
            }
            catch (...)
            {
                (void)DMK::log().log_noexcept(DMK::LogLevel::Error, "Controller: tick failed");
            }
            expected = ShutdownPhase::Ticking;
            if (s_shutdown_phase.compare_exchange_strong(expected, ShutdownPhase::Running, std::memory_order_acq_rel))
            {
                return;
            }
        }
        // A shutdown request can arrive inside tick(). Claim it only after that tick releases the state.
        expected = ShutdownPhase::Requested;
        if (s_shutdown_phase.compare_exchange_strong(expected, ShutdownPhase::InProgress, std::memory_order_acq_rel))
        {
            run_main_thread_shutdown();
        }
    }

    void controller_notify_config_changed() noexcept
    {
#ifdef DMK_ENABLE_PROFILING
        // A profiling build (-DDMK_ENABLE_PROFILING=ON) writes the recorded samples as a Chrome trace on every INI
        // save.
        try
        {
            const std::filesystem::path trace =
                std::filesystem::path(DMK::filesystem::get_runtime_directory()) / "KCD2_HenrySenses.trace.json";
            (void)DMK::Profiler::get_instance().export_to_file(trace.string());
        }
        catch (...)
        {
        }
#endif
        s_config_dirty.store(true, std::memory_order_relaxed);
        // A saved INI also logs which entity classes are around the player (the moment someone writes patterns), and
        // at LogLevel = DEBUG the state report.
        request_interactables_survey();
        s_report_requested.store(true, std::memory_order_relaxed);
    }

    MainThreadShutdown controller_request_shutdown(std::chrono::milliseconds timeout) noexcept
    {
        ShutdownPhase expected = ShutdownPhase::Running;
        // The shortcut needs more than "no tick ran": a refused teardown of this image can leave nodes highlighted for
        // this init to inherit, and only a tick can undo them.
        if (!s_ticked.load(std::memory_order_acquire) && !s_inherited_highlights)
        {
            // No tick ran, so nothing was registered or applied; withdrawing first keeps a tick that starts during
            // teardown from doing anything.
            if (s_shutdown_phase.compare_exchange_strong(expected, ShutdownPhase::Abandoned, std::memory_order_acq_rel))
            {
                return MainThreadShutdown::NeverTicked;
            }
        }
        expected = s_shutdown_phase.load(std::memory_order_acquire);
        while (expected == ShutdownPhase::Running || expected == ShutdownPhase::Ticking)
        {
            if (s_shutdown_phase.compare_exchange_weak(expected, ShutdownPhase::Requested, std::memory_order_acq_rel))
            {
                break;
            }
        }
        if (wait_for_shutdown_done(timeout))
        {
            return MainThreadShutdown::Completed;
        }
        // Withdraw the request so a tick that arrives after teardown moved on does nothing. It fails only when a
        // tick already claimed the work, which is then given the same window to finish.
        expected = ShutdownPhase::Requested;
        if (s_shutdown_phase.compare_exchange_strong(expected, ShutdownPhase::Abandoned, std::memory_order_acq_rel) ||
            expected == ShutdownPhase::Abandoned)
        {
            return MainThreadShutdown::Abandoned;
        }
        return wait_for_shutdown_done(timeout) ? MainThreadShutdown::Completed : MainThreadShutdown::Unfinished;
    }

} // namespace HenrySenses
