/**
 * @file highlight/registry.cpp
 * @brief Highlight set bookkeeping: per-node word writes, invalidation, the always-visible move, and restore.
 */

#include "highlight/registry.hpp"
#include "config.hpp"
#include "engine/entity_access.hpp"

#include <DetourModKit.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace HenrySenses
{
    namespace
    {
        /**
         * @brief One tracked entity and the state the mod changed on its render node.
         */
        struct Entry
        {
            EntityId id{0};
            LootCategory category{LootCategory::Item};
            std::uint32_t flags{0};
            // The word currently written to the node (0 = none written).
            std::uint32_t applied_word{0};
            // The group colour, its reach and intensity.
            std::uint32_t color{0};
            float fade_radius{20.0f};
            float intensity{1.0f};
            float marker_intensity{1.0f};
            GroupStyle style{GroupStyle::Outline};
            // The colour with the intensity, before the distance fade, for markers.
            std::uint32_t color_word{0};
            // The node the state was applied to; compared, never dereferenced, after the frame it was resolved in.
            std::uintptr_t node{0};
            // The always-visible move and the raised view distance on that node.
            RenderNodeOverrides overrides{};
            // The distance to the player at the last apply, for logs.
            float distance{-1.0f};
            // The game keeps the entity invisible for now (entity_flags_game_hidden): nothing is drawn, so it shows
            // its marker until the game draws it again. The word and the always-visible move stay applied, so the
            // outline is back the frame the game shows it.
            bool game_hidden{false};
            // The node's mesh is past the engine's draw distance, so the entry shows its marker. A raised node never
            // gets here, so this covers a node that the raise does not reach.
            bool beyond_reach{false};
        };

        // The main thread owns the entries. Setup runs before the frame hook arms.
        std::vector<Entry> s_entries;
        std::atomic<std::size_t> s_count{0};

        // A batch equal to the last one applied (same entities, tags, words and options) changes nothing on the nodes,
        // so it is skipped, but a full apply still runs this often: it re-resolves every node (a proxy recreated by
        // streaming gets its word again) and checks every always-visible bit, so maintain_highlights() has nothing to
        // do while the scans run.
        constexpr std::int64_t REVALIDATE_MS = 500;
        // The signature of the last batch applied in full (0: none, the next batch applies), and when.
        std::uint64_t s_applied_signature = 0;
        std::int64_t s_full_apply_ms = 0;
        RegistryApplyStats s_apply_stats{};
        // Scratch containers retain capacity across batches. Hash entries allocate when rebuilt.
        std::vector<HighlightRequest> s_wanted;
        std::unordered_set<EntityId> s_wanted_ids;
        std::unordered_map<EntityId, std::size_t> s_entry_slots;

        // The engine measures a node's draw distance from the camera, which sits behind the player in third person. The
        // marker starts this many metres early, so a camera up to that far back never shows a culled mesh without it.
        constexpr float REACH_MARGIN = 5.0f;

        // Number of fade steps; a word changes (and a persistent render object is invalidated) only on a step change.
        constexpr float FADE_STEPS = 8.0f;
        constexpr std::uint32_t FILL_ALPHA = 0x00;
        constexpr std::uint32_t OUTLINE_ALPHA = 0xFF;
        // An OutlineFill word: the lowest bit of the blue byte (1/255 of blue). The mask pass writes the colour bytes
        // unchanged but its alpha only as 0 or 1, so the composite shader reads the interior flag from here.
        constexpr std::uint32_t INTERIOR_FLAG = 0x00000100u;

        /**
         * @brief Tests one LootFlag in a flag set.
         * @param flags OR of LootFlag bits.
         * @param flag The flag to test.
         * @return True when set.
         */
        [[nodiscard]] constexpr bool has_flag(std::uint32_t flags, LootFlag flag) noexcept
        {
            return (flags & loot_flag_bit(flag)) != 0;
        }

        /**
         * @brief Scales the RGB channels of a word, keeping its alpha.
         * @param word Packed 0xRRGGBBAA.
         * @param factor Scale in [0, 1].
         * @return The scaled word.
         */
        [[nodiscard]] std::uint32_t scale_rgb(std::uint32_t word, float factor) noexcept
        {
            const float f = std::clamp(factor, 0.0f, 1.0f);
            auto channel = [&](int shift)
            {
                const float value = static_cast<float>((word >> shift) & 0xFFu) * f;
                return static_cast<std::uint32_t>(std::lround(value)) & 0xFFu;
            };
            return (channel(24) << 24) | (channel(16) << 16) | (channel(8) << 8) | (word & 0xFFu);
        }

        /**
         * @brief Applies a style to a word (alpha 0 = fill, else an outline; the interior flag for OutlineFill) and
         *        keeps the word non-zero.
         * @param word Packed 0xRRGGBBAA.
         * @param style The group's style.
         * @return The styled word.
         */
        [[nodiscard]] std::uint32_t apply_style(std::uint32_t word, GroupStyle style) noexcept
        {
            const bool fill = style == GroupStyle::Fill;
            std::uint32_t styled = (word & 0xFFFFFF00u & ~INTERIOR_FLAG) | (fill ? FILL_ALPHA : OUTLINE_ALPHA);
            if (style == GroupStyle::OutlineFill)
            {
                styled |= INTERIOR_FLAG;
            }
            // Zero is "no highlight" to the engine; a black fill is meaningless, so it becomes a faint outline.
            if (styled == 0)
            {
                styled = 0x000000FFu;
            }
            return styled;
        }

        /**
         * @brief Returns the quantised distance-fade factor for an entity.
         * @param distance Distance to the player in metres, negative when unknown.
         * @param radius The reach the fade ends at.
         * @return A factor in [MinOpacity, 1] in steps of 1/8.
         */
        [[nodiscard]] float fade_factor(float distance, float radius) noexcept
        {
            const LiveSettings &s = settings();
            const float start = s.fade_start.load(std::memory_order_relaxed);
            const float min_opacity = std::clamp(s.min_opacity.load(std::memory_order_relaxed), 0.0f, 1.0f);
            const float power = std::max(0.05f, s.fade_power.load(std::memory_order_relaxed));
            if (distance < 0.0f || distance <= start || radius <= start)
            {
                return 1.0f;
            }
            const float t = std::clamp((distance - start) / (radius - start), 0.0f, 1.0f);
            const float f = std::max(min_opacity, 1.0f - std::pow(t, power));
            return std::ceil(f * FADE_STEPS) / FADE_STEPS;
        }

        /**
         * @brief Quantises an intensity, never reaching zero while a highlight is shown.
         * @param intensity The intensity in [0, 1].
         * @return A factor in [1/INTENSITY_STEPS, 1] in steps of 1/INTENSITY_STEPS.
         */
        [[nodiscard]] float intensity_factor(float intensity) noexcept
        {
            const float stepped = std::ceil(std::clamp(intensity, 0.0f, 1.0f) * INTENSITY_STEPS);
            return std::max(1.0f, stepped) / INTENSITY_STEPS;
        }

        /**
         * @brief Computes the word written for one entity.
         * @param entry The entry.
         * @return The final word.
         */
        [[nodiscard]] std::uint32_t final_word(const Entry &entry) noexcept
        {
            std::uint32_t word = entry.color;
            if (has_flag(entry.flags, LootFlag::Empty))
            {
                word = scale_rgb(word, 0.5f);
            }
            return styled_word(word, entry.distance, entry.fade_radius, entry.intensity, entry.style);
        }

        /** @brief Steady-clock milliseconds. */
        [[nodiscard]] std::int64_t steady_ms() noexcept
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch()
            )
                .count();
        }

        /**
         * @brief What a batch would write: its entities with their tags, the word each would get at the distance the
         *        scan measured, the marker colour, and the options.
         * @details The scan measures from the player's position in the tick it runs, as the apply that follows does,
         *          so an unchanged signature means an unchanged set of words.
         */
        [[nodiscard]] std::uint64_t
        batch_signature(std::span<const HighlightRequest> wanted, const ApplyOptions &options) noexcept
        {
            std::uint64_t hash = 0xCBF29CE484222325ull;
            auto mix = [&hash](std::uint64_t value) { hash = (hash ^ value) * 0x100000001B3ull; };
            mix((options.write_words ? 1u : 0u) | (options.render_always ? 2u : 0u) |
                (options.player_position.has_value() ? 4u : 0u));
            for (const HighlightRequest &request : wanted)
            {
                mix(static_cast<std::uint64_t>(request.entity_id) |
                    (static_cast<std::uint64_t>(request.category) << 32) |
                    (static_cast<std::uint64_t>(request.style) << 40));
                mix(request.flags);
                const Entry probe{
                    .flags = request.flags,
                    .color = request.color,
                    .fade_radius = request.fade_radius,
                    .intensity = request.intensity,
                    .style = request.style,
                    .distance = options.player_position.has_value() ? request.distance : -1.0f,
                };
                mix(final_word(probe));
                mix(intensity_scaled(request.color, request.marker_intensity));
            }
            // 0 is the "apply in full" value.
            return hash == 0 ? 1 : hash;
        }

        /**
         * @brief Returns the distance between two points.
         */
        [[nodiscard]] float distance_between(const game_structures::Vec3f &a, const game_structures::Vec3f &b) noexcept
        {
            const float dx = a.x - b.x;
            const float dy = a.y - b.y;
            const float dz = a.z - b.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        /**
         * @brief Names a RenderAlwaysResult for log lines.
         */
        [[nodiscard]] const char *render_always_name(RenderAlwaysResult result) noexcept
        {
            switch (result)
            {
            case RenderAlwaysResult::Applied:
                return "applied";
            case RenderAlwaysResult::AlreadyAlways:
                return "already-always";
            case RenderAlwaysResult::NotRegistered:
                return "not-registered";
            case RenderAlwaysResult::Failed:
            default:
                return "failed";
            }
        }

        /**
         * @brief Restores one entry's node state. Main thread only.
         * @details The 3D-engine re-registration runs only while the entity still resolves to the very node the mod
         *          moved, so a node that is being torn down is never touched.
         * @param entry The entry.
         */
        void restore_entry(Entry &entry) noexcept
        {
            const std::uintptr_t entity = entity_from_id(entry.id);
            const std::uintptr_t node = entity != 0 ? render_node_of(entity) : 0;
            if (node == 0 || node != entry.node)
            {
                // The entity or its proxy is gone. A destroyed node takes its word and view-distance ratio with it, and
                // the engine's own unregister purges a flagged node from the always-visible list.
                entry = Entry{
                    .id = entry.id,
                };
                return;
            }
            if (entry.applied_word != 0)
            {
                (void)write_hud_word(node, 0);
                (void)invalidate_render_object(node);
            }
            const std::optional<RenderAlwaysResult> result = restore_render_node_overrides(node, entry.overrides);
            if (result.has_value() && *result != RenderAlwaysResult::Applied)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Warning,
                    "Registry: id={:#x} could not leave the always-visible list ({})",
                    entry.id,
                    render_always_name(*result)
                );
            }
            entry.applied_word = 0;
            entry.node = 0;
        }

        /**
         * @brief Returns the player's distance to an entity.
         * @return The distance in metres, negative when the player position or the entity position is unknown.
         */
        [[nodiscard]] float player_distance(std::uintptr_t entity, const ApplyOptions &options) noexcept
        {
            if (!options.player_position.has_value())
            {
                return -1.0f;
            }
            const std::optional<game_structures::Vec3f> position = entity_world_position(entity);
            return position.has_value() ? distance_between(*options.player_position, *position) : -1.0f;
        }

        /**
         * @brief Reads again whether an entry's mesh is past the engine's draw distance, and logs a change.
         * @details A silhouette shows only on a mesh that the engine draws, and update_render_node_overrides() raises
         *          the draw distance of every outlined node past any radius. The check catches a node that the raise
         *          does not reach, such as a node that the engine keeps in its own always-visible list. Such an entry
         *          shows its marker. The engine measures from the camera to the node's box, scaled by the zoom. The
         *          player's distance to the entity plus REACH_MARGIN stands in for that measure.
         * @param entry The entry.
         * @param node Its node, resolved this tick (0: none).
         * @param distance The player's distance to the entity, negative when unknown.
         * @param outlined The entry shows a silhouette: words are written and its group's Style is not Box.
         * @note Main thread only.
         */
        void note_reach(Entry &entry, std::uintptr_t node, float distance, bool outlined)
        {
            const std::optional<float> reach =
                outlined && node != 0 && distance >= 0.0f ? read_max_view_dist(node) : std::nullopt;
            if (!reach.has_value())
            {
                // Nothing to measure (no silhouette, no node, no distance): the other marker reasons decide.
                entry.beyond_reach = false;
                return;
            }
            // A reach that is not a number is never compared true, so it shows the marker too.
            const bool beyond = !(distance + REACH_MARGIN <= *reach);
            if (beyond == entry.beyond_reach)
            {
                return;
            }
            entry.beyond_reach = beyond;
            if (!DMK::log().is_enabled(DMK::LogLevel::Debug))
            {
                return;
            }
            const std::uintptr_t entity = entity_from_id(entry.id);
            (void)DMK::log().try_log(
                DMK::LogLevel::Debug,
                "Registry: id={:#x} {} '{}' {} the engine's draw distance ({:.1f} m reach at {:.1f} m) -> {}",
                entry.id,
                loot_category_name(entry.category),
                entity != 0 ? entity_name(entity) : std::string{"-"},
                beyond ? "past" : "back within",
                *reach,
                distance,
                beyond ? "marker" : "outline"
            );
        }

        /**
         * @brief Re-reads whether the game keeps an entry's entity invisible and logs a change. Main thread only.
         * @details Only the flag changes: the word and the always-visible move stay, since the game draws the entity
         *          again by clearing the flag alone. An unreadable flag word keeps the last state.
         * @param entry The entry.
         * @param entity Its entity, resolved this tick.
         */
        void note_game_hidden(Entry &entry, std::uintptr_t entity)
        {
            const std::optional<std::uint32_t> flags = entity_flags(entity);
            if (!flags.has_value())
            {
                return;
            }
            const bool hidden = entity_flags_game_hidden(*flags);
            if (hidden == entry.game_hidden)
            {
                return;
            }
            entry.game_hidden = hidden;
            if (!DMK::log().is_enabled(DMK::LogLevel::Debug))
            {
                return;
            }
            if (hidden)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "Registry: id={:#x} {} {} '{}' hidden by the game{} -> marker",
                    entry.id,
                    loot_category_name(entry.category),
                    entity_class_name(entity),
                    entity_name(entity),
                    entity_flags_active(*flags) ? "" : " (inactive)"
                );
            }
            else
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "Registry: id={:#x} {} {} '{}' shown again -> {}",
                    entry.id,
                    loot_category_name(entry.category),
                    entity_class_name(entity),
                    entity_name(entity),
                    entry.style == GroupStyle::Box ? "marker" : "outline"
                );
            }
        }

        /**
         * @brief Applies one request to its entry. Main thread only.
         * @param entry The entry (created or existing).
         * @param options The apply options.
         * @param created The entry was created by this batch (it is logged once).
         */
        void apply_entry(Entry &entry, const ApplyOptions &options, bool created)
        {
            const std::uintptr_t entity = entity_from_id(entry.id);
            if (entity == 0)
            {
                entry.node = 0;
                entry.applied_word = 0;
                entry.overrides = RenderNodeOverrides{};
                entry.game_hidden = false;
                entry.beyond_reach = false;
                return;
            }
            const std::uintptr_t node = render_node_of(entity);
            note_game_hidden(entry, entity);

            entry.distance = player_distance(entity, options);
            entry.color_word = intensity_scaled(entry.color, entry.marker_intensity);
            // A Box group draws brackets only: no word and no render overrides.
            const bool words = options.write_words && entry.style != GroupStyle::Box;

            if (node != entry.node)
            {
                // A new proxy never carries the old one's state.
                entry.applied_word = 0;
                entry.overrides = RenderNodeOverrides{};
            }

            if (node != 0 && words)
            {
                const std::uint32_t word = final_word(entry);
                if (word != entry.applied_word)
                {
                    if (write_hud_word(node, word))
                    {
                        (void)invalidate_render_object(node);
                        entry.applied_word = word;
                    }
                }
            }
            else if (node != 0 && entry.applied_word != 0)
            {
                (void)write_hud_word(node, 0);
                (void)invalidate_render_object(node);
                entry.applied_word = 0;
            }

            // Every silhouette needs the raised view distance, since the outline shows only on a mesh that the engine
            // draws.
            const RenderNodeWants wants{
                .always_visible = words && options.render_always,
                .raise_view_distance = words,
            };
            std::optional<RenderAlwaysResult> always_result{};
            // A node whose ERF_RENDER_ALWAYS bit someone else cleared while it sits in the always-visible list would
            // leave a dangling list entry when the engine later unregisters it, so it is moved back into the octree at
            // once; the next full apply moves it out again.
            bool corrected = false;
            if (entry.overrides.always_visible && node != 0)
            {
                const std::optional<bool> bit = read_render_always(node);
                if (bit.has_value() && !*bit)
                {
                    const std::optional<RenderAlwaysResult> result =
                        restore_render_node_overrides(node, entry.overrides);
                    corrected = true;
                    s_applied_signature = 0;
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Warning,
                        "Registry: id={:#x} lost ERF_RENDER_ALWAYS outside the mod; moved back into "
                        "the octree ({})",
                        entry.id,
                        render_always_name(result.value_or(RenderAlwaysResult::Failed))
                    );
                }
            }
            if (node != 0 && !corrected)
            {
                always_result = update_render_node_overrides(node, entry.overrides, wants);
            }
            entry.node = node;
            note_reach(entry, node, entry.distance, words);

            if (created)
            {
                // A new entry holds no overrides, so a node that wants the always-visible move always reports one here.
                const std::optional<float> reach = node != 0 ? read_max_view_dist(node) : std::nullopt;
                const std::optional<std::uint8_t> own_ratio = entry.overrides.own_view_dist_ratio;
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "Registry: + id={:#x} {} flags={:#x} class={} dist={:.1f} node=0x{:016X} word={:#010x} always={} "
                    "reach={}{}",
                    entry.id,
                    loot_category_name(entry.category),
                    entry.flags,
                    entity_class_name(entity),
                    entry.distance,
                    node,
                    entry.applied_word,
                    always_result.has_value() ? render_always_name(*always_result) : "off",
                    reach.has_value() ? std::format("{:.0f}", *reach) : std::string{"-"},
                    own_ratio.has_value() ? std::format(" (raised from ratio {})", *own_ratio) : std::string{}
                );
            }
        }
    } // namespace

    const char *loot_category_name(LootCategory category) noexcept
    {
        switch (category)
        {
        case LootCategory::HumanCorpse:
            return "corpse";
        case LootCategory::AnimalCarcass:
            return "animal";
        case LootCategory::Item:
            return "item";
        case LootCategory::Container:
            return "container";
        case LootCategory::Herb:
            return "herb";
        case LootCategory::LiveNpc:
            return "npc";
        case LootCategory::LiveAnimal:
            return "live-animal";
        case LootCategory::Horse:
            return "horse";
        case LootCategory::Dog:
            return "dog";
        case LootCategory::Critter:
            return "critter";
        case LootCategory::Custom:
        default:
            return "custom";
        }
    }

    DMK::Result<void> initialize_registry()
    {
        // Entries are kept: after a refused teardown this init runs again on the same image, and every entry is a
        // node whose word and always-visible state are still applied. The first main-thread tick restores them.
        s_count.store(s_entries.size(), std::memory_order_relaxed);
        if (!s_entries.empty())
        {
            (void)DMK::log().try_log(
                DMK::LogLevel::Warning,
                "Registry: {} highlight(s) left by an earlier teardown of this image; the first "
                "main-thread tick restores them",
                s_entries.size()
            );
        }
        return {};
    }

    void apply_batch(std::span<const HighlightRequest> batch, const ApplyOptions &options) noexcept
    {
        DMK_PROFILE_FUNCTION();
        try
        {

            // Every distinct id in the batch (the radius alone decides what the scan put in it).
            s_wanted.clear();
            s_wanted_ids.clear();
            for (const HighlightRequest &request : batch)
            {
                if (request.entity_id != 0 && s_wanted_ids.insert(request.entity_id).second)
                {
                    s_wanted.push_back(request);
                }
            }

            const std::uint64_t signature = batch_signature(s_wanted, options);
            const std::int64_t now = steady_ms();
            if (signature == s_applied_signature && s_full_apply_ms != 0 && now - s_full_apply_ms < REVALIDATE_MS)
            {
                // The game hides and shows entities without a change to the batch, and the player walks across a node's
                // draw distance. Every batch reads both again, so a skipped apply never holds a marker back or keeps
                // one up until the next full apply.
                for (Entry &entry : s_entries)
                {
                    if (const std::uintptr_t entity = entity_from_id(entry.id); entity != 0)
                    {
                        note_game_hidden(entry, entity);
                        note_reach(
                            entry,
                            render_node_of(entity),
                            player_distance(entity, options),
                            options.write_words && entry.style != GroupStyle::Box
                        );
                    }
                }
                ++s_apply_stats.skipped;
                return;
            }

            // Restore and drop every entry the new set no longer contains.
            std::size_t removed = 0;
            for (Entry &entry : s_entries)
            {
                if (!s_wanted_ids.contains(entry.id))
                {
                    restore_entry(entry);
                    (void)DMK::log().try_log(DMK::LogLevel::Debug, "Registry: - id={:#x}", entry.id);
                    entry.id = 0;
                    ++removed;
                }
            }
            std::erase_if(s_entries, [](const Entry &entry) { return entry.id == 0; });
            s_entry_slots.clear();
            for (std::size_t slot = 0; slot < s_entries.size(); ++slot)
            {
                s_entry_slots.emplace(s_entries[slot].id, slot);
            }

            // A corrected always-visible bit clears the signature again (apply_entry), so the next batch applies.
            s_applied_signature = signature;
            s_full_apply_ms = now;
            ++s_apply_stats.full;

            std::size_t words = 0;
            std::size_t always = 0;
            std::size_t raised = 0;
            std::size_t added = 0;
            for (const HighlightRequest &request : s_wanted)
            {
                const auto [slot_it, created] = s_entry_slots.try_emplace(request.entity_id, s_entries.size());
                if (created)
                {
                    s_entries.push_back(
                        Entry{
                            .id = request.entity_id,
                        }
                    );
                    ++added;
                }
                Entry &entry = s_entries[slot_it->second];
                entry.category = request.category;
                entry.flags = request.flags;
                entry.color = request.color;
                entry.fade_radius = request.fade_radius;
                entry.intensity = request.intensity;
                entry.marker_intensity = request.marker_intensity;
                entry.style = request.style;
                apply_entry(entry, options, created);
                words += entry.applied_word != 0 ? 1 : 0;
                always += entry.overrides.always_visible ? 1 : 0;
                raised += entry.overrides.own_view_dist_ratio.has_value() ? 1 : 0;
            }
            s_count.store(s_entries.size(), std::memory_order_relaxed);

            (void)DMK::log().try_log(
                added != 0 || removed != 0 ? DMK::LogLevel::Debug : DMK::LogLevel::Trace,
                "Registry: batch of {} -> {} tracked ({} added, {} removed), {} words, {} always-visible, {} view "
                "distance raised",
                batch.size(),
                s_entries.size(),
                added,
                removed,
                words,
                always,
                raised
            );
        }
        catch (...)
        {
            s_applied_signature = 0;
            s_full_apply_ms = 0;
            s_count.store(s_entries.size(), std::memory_order_relaxed);
            (void)DMK::log().log_noexcept(DMK::LogLevel::Error, "Registry: apply_batch failed (allocation)");
        }
    }

    void clear_all_highlights() noexcept
    {
        s_applied_signature = 0;
        s_full_apply_ms = 0;
        for (Entry &entry : s_entries)
        {
            restore_entry(entry);
        }
        if (!s_entries.empty())
        {
            (void)DMK::log().try_log(DMK::LogLevel::Debug, "Registry: cleared {} highlight(s)", s_entries.size());
        }
        s_entries.clear();
        s_count.store(0, std::memory_order_relaxed);
    }

    void maintain_highlights() noexcept
    {
        DMK_PROFILE_FUNCTION();
        // A full apply did this for every entry within the interval.
        if (s_full_apply_ms != 0 && steady_ms() - s_full_apply_ms < REVALIDATE_MS)
        {
            return;
        }
        for (Entry &entry : s_entries)
        {
            if (!entry.overrides.always_visible)
            {
                continue;
            }
            const std::uintptr_t entity = entity_from_id(entry.id);
            const std::uintptr_t node = entity != 0 ? render_node_of(entity) : 0;
            if (node == 0 || node != entry.node)
            {
                // The node the state was applied to is gone; the engine purged it from the list on destruction. The
                // next batch applies in full (a new node gets its word).
                entry.overrides = RenderNodeOverrides{};
                entry.applied_word = 0;
                entry.node = node;
                s_applied_signature = 0;
                continue;
            }
            const std::optional<bool> bit = read_render_always(node);
            if (bit.has_value() && !*bit)
            {
                const std::optional<RenderAlwaysResult> result = restore_render_node_overrides(node, entry.overrides);
                s_applied_signature = 0;
                (void)DMK::log().try_log(
                    DMK::LogLevel::Warning,
                    "Registry: id={:#x} lost ERF_RENDER_ALWAYS outside the mod; moved back into "
                    "the octree ({})",
                    entry.id,
                    render_always_name(result.value_or(RenderAlwaysResult::Failed))
                );
            }
        }
    }

    std::size_t refresh_highlight_render_objects() noexcept
    {
        std::size_t refreshed = 0;
        for (const Entry &entry : s_entries)
        {
            if (entry.applied_word == 0 || entry.node == 0)
            {
                continue;
            }
            const std::uintptr_t entity = entity_from_id(entry.id);
            const std::uintptr_t node = entity != 0 ? render_node_of(entity) : 0;
            if (node == entry.node && invalidate_render_object(node))
            {
                ++refreshed;
            }
        }
        return refreshed;
    }

    std::size_t highlighted_count() noexcept
    {
        return s_count.load(std::memory_order_relaxed);
    }

    RegistryApplyStats take_registry_apply_stats() noexcept
    {
        const RegistryApplyStats taken = s_apply_stats;
        s_apply_stats = RegistryApplyStats{};
        return taken;
    }

    void collect_marker_targets(std::vector<MarkerTarget> &out)
    {
        out.clear();
        out.reserve(s_entries.size());
        for (const Entry &entry : s_entries)
        {
            // The engine does not draw a mesh that the game keeps invisible or that lies past its draw distance, so the
            // outline of such an entity cannot show.
            const bool preferred = entry.style == GroupStyle::Box || has_flag(entry.flags, LootFlag::StashCorpse) ||
                                   entry.node == 0 || entry.game_hidden || entry.beyond_reach;
            out.push_back(MarkerTarget{entry.id, entry.color_word, preferred});
        }
    }

    void log_registry_state()
    {
        DMK::Logger &logger = DMK::log();
        logger.info("Registry: {} tracked entit{}", s_entries.size(), s_entries.size() == 1 ? "y" : "ies");
        for (const Entry &entry : s_entries)
        {
            const std::uintptr_t entity = entity_from_id(entry.id);
            const std::uintptr_t node = entity != 0 ? render_node_of(entity) : 0;
            const std::optional<std::uint32_t> live_word = node != 0 ? read_hud_word(node) : std::nullopt;
            const std::optional<bool> always = node != 0 ? read_render_always(node) : std::nullopt;
            const std::optional<float> reach = node != 0 ? read_max_view_dist(node) : std::nullopt;
            const std::optional<std::uint8_t> ratio = node != 0 ? read_view_dist_ratio(node) : std::nullopt;
            logger.info(
                "  id={:#x} {} flags={:#x} class={} name={} dist={:.1f} entity={} node={}{} word={:#010x} "
                "live={} always(mod={}, bit={}) reach={} ratio(own={}, live={}){}{}",
                entry.id,
                loot_category_name(entry.category),
                entry.flags,
                entity != 0 ? entity_class_name(entity) : std::string{"-"},
                entity != 0 ? entity_name(entity) : std::string{"-"},
                entry.distance,
                DMK::format::format_address(entity),
                DMK::format::format_address(node),
                node != entry.node ? " (changed)" : "",
                entry.applied_word,
                live_word.has_value() ? std::format("{:#010x}", *live_word) : std::string{"-"},
                entry.overrides.always_visible,
                always.has_value() ? (*always ? "1" : "0") : "-",
                reach.has_value() ? std::format("{:.1f}", *reach) : std::string{"-"},
                entry.overrides.own_view_dist_ratio.has_value()
                    ? std::format("{}", *entry.overrides.own_view_dist_ratio)
                    : std::string{"-"},
                ratio.has_value() ? std::format("{}", *ratio) : std::string{"-"},
                entry.game_hidden ? " hidden by the game (marker)" : "",
                entry.beyond_reach ? " past the engine's draw distance (marker)" : ""
            );
        }
    }

    std::uint32_t
    styled_word(std::uint32_t color_word, float distance, float fade_radius, float intensity, GroupStyle style) noexcept
    {
        std::uint32_t word = scale_rgb(color_word, fade_factor(distance, fade_radius) * intensity_factor(intensity));
        const bool fill = style == GroupStyle::Fill;
        if (fill)
        {
            // The fill composite writes the colour as it is, so its opacity is the colour's brightness.
            word = scale_rgb(word, std::clamp(settings().fill_opacity.load(std::memory_order_relaxed), 0.0f, 1.0f));
        }
        return apply_style(word, style);
    }

    std::uint32_t intensity_scaled(std::uint32_t color_word, float intensity) noexcept
    {
        return scale_rgb(color_word, intensity_factor(intensity));
    }

    bool registry_owns_node(std::uintptr_t node) noexcept
    {
        if (node == 0)
        {
            return false;
        }
        return std::any_of(
            s_entries.begin(),
            s_entries.end(),
            [node](const Entry &entry)
            { return entry.node == node && (entry.applied_word != 0 || entry.overrides.changed()); }
        );
    }

} // namespace HenrySenses
