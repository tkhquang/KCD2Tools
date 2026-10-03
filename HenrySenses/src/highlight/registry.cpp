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
            // The mod moved the node into the always-visible list.
            bool render_always_applied{false};
            // The distance to the player at the last apply, for logs.
            float distance{-1.0f};
            // The game keeps the entity invisible for now (entity_flags_game_hidden): nothing is drawn, so it shows
            // its marker until the game draws it again. The word and the always-visible move stay applied, so the
            // outline is back the frame the game shows it.
            bool game_hidden{false};
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
                // The entity or its proxy is gone; a destroyed node takes its word with it, and a node that carried
                // ERF_RENDER_ALWAYS is purged from the always-visible list by the engine's own unregister.
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
            if (entry.render_always_applied && node == entry.node)
            {
                const RenderAlwaysResult result = remove_render_always(node, false);
                if (result != RenderAlwaysResult::Applied)
                {
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Warning,
                        "Registry: id={:#x} could not leave the always-visible list ({})",
                        entry.id,
                        render_always_name(result)
                    );
                }
            }
            entry.applied_word = 0;
            entry.render_always_applied = false;
            entry.node = 0;
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
                entry.render_always_applied = false;
                entry.game_hidden = false;
                return;
            }
            const std::uintptr_t node = render_node_of(entity);
            note_game_hidden(entry, entity);

            entry.distance = -1.0f;
            if (options.player_position.has_value())
            {
                if (const auto position = entity_world_position(entity); position.has_value())
                {
                    entry.distance = distance_between(*options.player_position, *position);
                }
            }
            entry.color_word = intensity_scaled(entry.color, entry.marker_intensity);
            // A Box group draws brackets only: no word, and no move out of the octree.
            const bool words = options.write_words && entry.style != GroupStyle::Box;

            if (node != entry.node)
            {
                // A new proxy never carries the old one's state.
                entry.applied_word = 0;
                entry.render_always_applied = false;
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

            const bool want_always = node != 0 && words && options.render_always;
            RenderAlwaysResult always_result = RenderAlwaysResult::NotRegistered;
            // A node whose ERF_RENDER_ALWAYS bit someone else cleared while it sits in the always-visible list would
            // leave a dangling list entry when the engine later unregisters it, so it is moved back into the octree at
            // once; the next full apply moves it out again.
            bool corrected = false;
            if (entry.render_always_applied && node != 0)
            {
                const std::optional<bool> bit = read_render_always(node);
                if (bit.has_value() && !*bit)
                {
                    const RenderAlwaysResult result = remove_render_always(node, false);
                    entry.render_always_applied = false;
                    corrected = true;
                    s_applied_signature = 0;
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Warning,
                        "Registry: id={:#x} lost ERF_RENDER_ALWAYS outside the mod; moved back into "
                        "the octree ({})",
                        entry.id,
                        render_always_name(result)
                    );
                }
            }
            if (want_always && !entry.render_always_applied && !corrected)
            {
                always_result = apply_render_always(node);
                entry.render_always_applied = always_result == RenderAlwaysResult::Applied;
            }
            else if (!want_always && entry.render_always_applied && node != 0)
            {
                (void)remove_render_always(node, false);
                entry.render_always_applied = false;
            }
            entry.node = node;

            if (created)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "Registry: + id={:#x} {} flags={:#x} class={} dist={:.1f} node=0x{:016X} word={:#010x} always={}",
                    entry.id,
                    loot_category_name(entry.category),
                    entry.flags,
                    entity_class_name(entity),
                    entry.distance,
                    node,
                    entry.applied_word,
                    want_always ? render_always_name(always_result) : "off"
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
                // The game hides and shows entities without any change to the batch, so their flag is read on every
                // batch: a skipped apply must not hold a marker back (or keep one up) until the next full apply.
                for (Entry &entry : s_entries)
                {
                    if (const std::uintptr_t entity = entity_from_id(entry.id); entity != 0)
                    {
                        note_game_hidden(entry, entity);
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
                always += entry.render_always_applied ? 1 : 0;
            }
            s_count.store(s_entries.size(), std::memory_order_relaxed);

            (void)DMK::log().try_log(
                added != 0 || removed != 0 ? DMK::LogLevel::Debug : DMK::LogLevel::Trace,
                "Registry: batch of {} -> {} tracked ({} added, {} removed), {} words, {} always-visible",
                batch.size(),
                s_entries.size(),
                added,
                removed,
                words,
                always
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
            if (!entry.render_always_applied)
            {
                continue;
            }
            const std::uintptr_t entity = entity_from_id(entry.id);
            const std::uintptr_t node = entity != 0 ? render_node_of(entity) : 0;
            if (node == 0 || node != entry.node)
            {
                // The node the state was applied to is gone; the engine purged it from the list on destruction. The
                // next batch applies in full (a new node gets its word).
                entry.render_always_applied = false;
                entry.applied_word = 0;
                entry.node = node;
                s_applied_signature = 0;
                continue;
            }
            const std::optional<bool> bit = read_render_always(node);
            if (bit.has_value() && !*bit)
            {
                const RenderAlwaysResult result = remove_render_always(node, false);
                entry.render_always_applied = false;
                s_applied_signature = 0;
                (void)DMK::log().try_log(
                    DMK::LogLevel::Warning,
                    "Registry: id={:#x} lost ERF_RENDER_ALWAYS outside the mod; moved back into "
                    "the octree ({})",
                    entry.id,
                    render_always_name(result)
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
            // An entity the game keeps invisible is not drawn, so its outline cannot show until the game draws it.
            const bool preferred = entry.style == GroupStyle::Box || has_flag(entry.flags, LootFlag::StashCorpse) ||
                                   entry.node == 0 || entry.game_hidden;
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
            logger.info(
                "  id={:#x} {} flags={:#x} class={} name={} dist={:.1f} entity={} node={}{} word={:#010x} "
                "live={} always(mod={}, bit={}){}",
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
                entry.render_always_applied,
                always.has_value() ? (*always ? "1" : "0") : "-",
                entry.game_hidden ? " hidden by the game (marker)" : ""
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
            { return entry.node == node && (entry.applied_word != 0 || entry.render_always_applied); }
        );
    }

} // namespace HenrySenses
