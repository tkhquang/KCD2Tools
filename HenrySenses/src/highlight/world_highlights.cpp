/**
 * @file highlight/world_highlights.cpp
 * @brief Outline set for interactive world objects.
 */

#include "highlight/world_highlights.hpp"
#include "engine/visual_resolver.hpp"

#include <DetourModKit.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace HenrySenses
{
    namespace
    {
        /**
         * @brief One outlined object and the state the mod changed on its node.
         */
        struct Entry
        {
            EntityId entity_id{0};
            std::uintptr_t brush{0};
            game_structures::Aabb brush_bounds{};
            // A CMovableBrush's model when the entry was made: such a brush may move, so it is recognised by its model
            // instead of its bounds (empty for every other brush).
            std::string movable_model{};
            std::uint32_t color_word{0};
            float distance{-1.0f};
            float fade_radius{20.0f};
            float intensity{1.0f};
            GroupStyle style{GroupStyle::Outline};
            // The word currently written to the node (0 = none written).
            std::uint32_t applied_word{0};
            // The node the state was applied to; compared, never dereferenced, after the frame it was resolved in.
            std::uintptr_t node{0};
            bool render_always_applied{false};
            bool wanted{false};
        };

        std::vector<Entry> s_entries;
        std::atomic<std::size_t> s_count{0};

        [[nodiscard]] bool same_object(const Entry &entry, const WorldHighlightRequest &request) noexcept
        {
            return request.entity_id != 0 ? entry.entity_id == request.entity_id
                                          : entry.entity_id == 0 && entry.brush == request.brush;
        }

        /**
         * @brief Resolves the node an entry shows now: the entity's render proxy, or the brush when it is still the
         *        brush the entry was made for (the same bounds, or for a movable brush the same model).
         */
        [[nodiscard]] std::uintptr_t resolve_node(const Entry &entry) noexcept
        {
            if (entry.entity_id != 0)
            {
                const std::uintptr_t entity = entity_from_id(entry.entity_id);
                return entity != 0 ? render_node_of(entity) : 0;
            }
            return brush_revalidate(entry.brush, entry.brush_bounds, entry.movable_model).has_value() ? entry.brush : 0;
        }

        void forget(Entry &entry) noexcept
        {
            entry.applied_word = 0;
            entry.render_always_applied = false;
            entry.node = 0;
        }

        /**
         * @brief Restores one entry's node state.
         * @details Runs only while the object still resolves to the very node the mod changed and the loot set has not
         *          taken that node over; a gone node took its word with it and was purged from the always-visible list
         *          by the engine.
         */
        void restore_entry(Entry &entry) noexcept
        {
            const std::uintptr_t node = resolve_node(entry);
            if (node == 0 || node != entry.node || registry_owns_node(node))
            {
                forget(entry);
                return;
            }
            if (entry.applied_word != 0)
            {
                (void)write_hud_word(node, 0);
                (void)invalidate_render_object(node);
            }
            if (entry.render_always_applied)
            {
                const RenderAlwaysResult result = remove_render_always(node, false);
                if (result != RenderAlwaysResult::Applied)
                {
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Warning,
                        "WorldHighlights: 0x{:016X} could not leave the always-visible list",
                        node
                    );
                }
            }
            forget(entry);
        }

        void apply_entry(Entry &entry, const ApplyOptions &options)
        {
            const std::uintptr_t node = resolve_node(entry);
            const bool first_time = entry.node == 0 && entry.applied_word == 0 && !entry.render_always_applied;
            if (node != entry.node)
            {
                // A new proxy never carries the old one's state, and a brush that stopped matching is gone.
                forget(entry);
            }
            if (node == 0)
            {
                return;
            }
            if (registry_owns_node(node))
            {
                // The loot set outlines this node already, in its own colour.
                forget(entry);
                return;
            }

            if (options.write_words)
            {
                const std::uint32_t word =
                    styled_word(entry.color_word, entry.distance, entry.fade_radius, entry.intensity, entry.style);
                if (word != entry.applied_word && write_hud_word(node, word))
                {
                    (void)invalidate_render_object(node);
                    entry.applied_word = word;
                }
            }
            else if (entry.applied_word != 0)
            {
                (void)write_hud_word(node, 0);
                (void)invalidate_render_object(node);
                entry.applied_word = 0;
            }

            const bool want_always = options.write_words && options.render_always;
            RenderAlwaysResult always_result = RenderAlwaysResult::NotRegistered;
            if (want_always && !entry.render_always_applied)
            {
                always_result = apply_render_always(node);
                entry.render_always_applied = always_result == RenderAlwaysResult::Applied;
            }
            else if (!want_always && entry.render_always_applied)
            {
                (void)remove_render_always(node, false);
                entry.render_always_applied = false;
            }
            entry.node = node;

            if (first_time)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "WorldHighlights: + {} {} dist={:.1f} node=0x{:016X} word={:#010x} always={}",
                    entry.entity_id != 0 ? "entity" : "brush",
                    entry.entity_id != 0 ? std::format("{:#x}", entry.entity_id) : render_node_name(node),
                    entry.distance,
                    node,
                    entry.applied_word,
                    entry.render_always_applied
                );
            }
        }
    } // namespace

    void apply_world_highlights(std::span<const WorldHighlightRequest> requests, const ApplyOptions &options) noexcept
    {
        DMK_PROFILE_FUNCTION();
        try
        {
            for (Entry &entry : s_entries)
            {
                entry.wanted = false;
            }
            std::size_t added = 0;
            for (const WorldHighlightRequest &request : requests)
            {
                if (request.entity_id == 0 && request.brush == 0)
                {
                    continue;
                }
                auto it = std::find_if(
                    s_entries.begin(),
                    s_entries.end(),
                    [&](const Entry &entry) { return same_object(entry, request); }
                );
                if (it == s_entries.end())
                {
                    const bool movable = request.entity_id == 0 && is_movable_brush_node(request.brush);
                    s_entries.push_back(
                        Entry{
                            .entity_id = request.entity_id,
                            .brush = request.entity_id != 0 ? 0 : request.brush,
                            .brush_bounds = request.brush_bounds,
                            .movable_model = movable ? render_node_name(request.brush) : std::string{},
                        }
                    );
                    it = std::prev(s_entries.end());
                    ++added;
                }
                else if (it->wanted)
                {
                    // Two triggers on one object (the seats of a bench) keep the nearer one's distance.
                    continue;
                }
                else if (request.entity_id == 0)
                {
                    // The requester re-checks its brushes and follows a movable one's bounds.
                    it->brush_bounds = request.brush_bounds;
                }
                it->wanted = true;
                it->color_word = request.color_word;
                it->distance = request.distance;
                it->fade_radius = request.fade_radius;
                it->intensity = request.intensity;
                it->style = request.style;
            }

            std::size_t removed = 0;
            for (Entry &entry : s_entries)
            {
                if (!entry.wanted)
                {
                    restore_entry(entry);
                    ++removed;
                }
            }
            std::erase_if(s_entries, [](const Entry &entry) { return !entry.wanted; });

            std::size_t words = 0;
            for (Entry &entry : s_entries)
            {
                apply_entry(entry, options);
                words += entry.applied_word != 0 ? 1 : 0;
            }
            s_count.store(s_entries.size(), std::memory_order_relaxed);
            if (added != 0 || removed != 0)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "WorldHighlights: {} object(s) ({} added, {} removed), {} outlined",
                    s_entries.size(),
                    added,
                    removed,
                    words
                );
            }
        }
        catch (...)
        {
            (void)DMK::log().log_noexcept(DMK::LogLevel::Error, "WorldHighlights: apply failed (allocation)");
        }
    }

    void clear_world_highlights() noexcept
    {
        for (Entry &entry : s_entries)
        {
            restore_entry(entry);
        }
        if (!s_entries.empty())
        {
            (void)DMK::log().try_log(DMK::LogLevel::Debug, "WorldHighlights: cleared {} object(s)", s_entries.size());
        }
        s_entries.clear();
        s_count.store(0, std::memory_order_relaxed);
    }

    void maintain_world_highlights() noexcept
    {
        DMK_PROFILE_FUNCTION();
        for (Entry &entry : s_entries)
        {
            if (!entry.render_always_applied)
            {
                continue;
            }
            const std::uintptr_t node = resolve_node(entry);
            if (node == 0 || node != entry.node)
            {
                forget(entry);
                continue;
            }
            const std::optional<bool> bit = read_render_always(node);
            if (bit.has_value() && !*bit)
            {
                (void)remove_render_always(node, false);
                entry.render_always_applied = false;
                (void)DMK::log().try_log(
                    DMK::LogLevel::Warning,
                    "WorldHighlights: 0x{:016X} lost ERF_RENDER_ALWAYS outside the mod; moved back into "
                    "the octree",
                    node
                );
            }
        }
    }

    std::size_t refresh_world_highlight_render_objects() noexcept
    {
        std::size_t refreshed = 0;
        for (const Entry &entry : s_entries)
        {
            if (entry.applied_word == 0)
            {
                continue;
            }
            const std::uintptr_t node = resolve_node(entry);
            if (node != 0 && node == entry.node && invalidate_render_object(node))
            {
                ++refreshed;
            }
        }
        return refreshed;
    }

    std::size_t world_highlight_count() noexcept
    {
        return s_count.load(std::memory_order_relaxed);
    }

    void world_highlight_nodes_in(const game_structures::Aabb &box, std::vector<std::uintptr_t> &out) noexcept
    {
        try
        {
            for (const Entry &entry : s_entries)
            {
                // Only a node moved to the always-visible list has left the octree; the rest is found by the query.
                if (!entry.render_always_applied || entry.node == 0)
                {
                    continue;
                }
                // The bounds the object was requested with (a door swings within them) rule out the far entries
                // before anything is read.
                const game_structures::Aabb &b = entry.brush_bounds;
                if (b.max.x < box.min.x || b.min.x > box.max.x || b.max.y < box.min.y || b.min.y > box.max.y ||
                    b.max.z < box.min.z || b.min.z > box.max.z)
                {
                    continue;
                }
                if (resolve_node(entry) == entry.node && std::find(out.begin(), out.end(), entry.node) == out.end())
                {
                    out.push_back(entry.node);
                }
            }
        }
        catch (...)
        {
            (void)DMK::log().log_noexcept(DMK::LogLevel::Error, "WorldHighlights: node lookup failed (allocation)");
        }
    }

} // namespace HenrySenses
