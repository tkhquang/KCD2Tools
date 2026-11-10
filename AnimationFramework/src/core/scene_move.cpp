/**
 * @file core/scene_move.cpp
 * @brief Moving a cutscene layer (see scene_move.hpp). The layer is edited as text, so everything that does not move
 *        stays byte for byte as the game wrote it.
 */

#include "core/scene_move.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <map>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace AnimationFramework
{
    namespace
    {
        constexpr std::string_view ENTITY_TAG = "<Entity ";
        constexpr std::string_view NODE_TAG = "<Node ";
        constexpr std::string_view NODE_END = "</Node>";
        constexpr std::string_view SEQUENCE_TAG = "<Sequence ";
        constexpr std::string_view SEQUENCE_END = "</Sequence>";
        constexpr std::string_view TRACK_END = "</Track>";
        constexpr std::string_view SUBTRACK_TAG = "<NewSubTrack";
        constexpr std::string_view SUBTRACK_END = "</NewSubTrack>";
        constexpr std::string_view KEY_TAG = "<Key ";
        constexpr std::string_view POSITION_TRACK = "<Track paramType=\"Position\"";
        constexpr std::string_view LINK_TRACK = "<Track paramType=\"LinkTo\"";
        constexpr std::string_view ANIMATION_TRACK = "paramType=\"Animation\"";
        /// Keys at the same time as a LinkTo key belong to the link that key starts (the game's own layers key both).
        constexpr double TIME_EPSILON = 1e-4;

        /** @brief One replacement of the text in [start, end). */
        struct Edit
        {
            std::size_t start = 0;
            std::size_t end = 0;
            std::string text;
        };

        /** @brief The span of the value of attribute @p name (` name="..."`) in @p text from @p from to @p to. */
        [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>>
        value_span(std::string_view text, std::string_view name, std::size_t from, std::size_t to)
        {
            const std::string key = std::format(" {}=\"", name);
            const std::size_t at = text.substr(0, to).find(key, from);
            if (at == std::string_view::npos)
            {
                return std::nullopt;
            }
            const std::size_t start = at + key.size();
            const std::size_t end = text.find('"', start);
            if (end == std::string_view::npos || end > to)
            {
                return std::nullopt;
            }
            return std::pair{start, end};
        }

        /** @brief The value of attribute @p name between @p from and @p to, or empty. */
        [[nodiscard]] std::string_view
        value_of(std::string_view text, std::string_view name, std::size_t from, std::size_t to)
        {
            const auto span = value_span(text, name, from, to);
            return span ? text.substr(span->first, span->second - span->first) : std::string_view{};
        }

        [[nodiscard]] std::optional<double> parse_double(std::string_view text) noexcept
        {
            double value = 0.0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size())
            {
                return std::nullopt;
            }
            return value;
        }

        /** @brief @p metres to a micrometre, without trailing zeros. */
        [[nodiscard]] std::string format_metres(double metres)
        {
            std::string text = std::format("{:.6f}", metres);
            text.erase(text.find_last_not_of('0') + 1);
            if (text.back() == '.')
            {
                text.pop_back();
            }
            return text == "-0" ? "0" : text;
        }

        /** @brief The end of the start tag that opens at @p open (past its `>`), or npos. */
        [[nodiscard]] std::size_t tag_end(std::string_view text, std::size_t open) noexcept
        {
            const std::size_t close = text.find('>', open);
            return close == std::string_view::npos ? close : close + 1;
        }

        /**
         * @brief Whether the start tag that ends at @p end closes itself (`/>`). A tag that never ends (npos, a
         *        truncated file) counts as closed, so no body is read past the text.
         */
        [[nodiscard]] bool self_closing(std::string_view text, std::size_t end) noexcept
        {
            return end > text.size() || (end >= 2 && text[end - 2] == '/');
        }

        /** @brief A top-level entity of the layer: one with a world position, not parented to another entity. */
        [[nodiscard]] bool is_placed(std::string_view text, std::size_t open, std::size_t end)
        {
            return value_span(text, "Pos", open, end).has_value() && !value_span(text, "ParentId", open, end);
        }

        /**
         * @brief The times a node's LinkTo track attaches it: (key time, attached) in time order, where attached is
         *        whether the key names a node to attach to (an empty name detaches).
         */
        [[nodiscard]] std::vector<std::pair<double, bool>>
        link_keys(std::string_view text, std::size_t body, std::size_t body_end)
        {
            std::vector<std::pair<double, bool>> keys;
            const std::size_t track = text.substr(0, body_end).find(LINK_TRACK, body);
            if (track == std::string_view::npos)
            {
                return keys;
            }
            const std::size_t track_end = text.substr(0, body_end).find(TRACK_END, track);
            for (std::size_t key = text.find(KEY_TAG, track); key != std::string_view::npos && key < track_end;
                 key = text.find(KEY_TAG, key + 1))
            {
                const std::size_t key_end = tag_end(text, key);
                const auto time = parse_double(value_of(text, "time", key, key_end));
                if (time)
                {
                    keys.emplace_back(*time, !value_of(text, "node", key, key_end).empty());
                }
            }
            std::ranges::sort(keys);
            return keys;
        }

        /** @brief Whether the LinkTo keys @p links hold the node attached at @p time. */
        [[nodiscard]] bool attached_at(const std::vector<std::pair<double, bool>> &links, double time) noexcept
        {
            bool attached = false;
            for (const auto &[at, links_to] : links)
            {
                if (at > time + TIME_EPSILON)
                {
                    break;
                }
                attached = links_to;
            }
            return attached;
        }

        /**
         * @brief The edit that shifts the `time,value` pair in attribute @p name of the tag [open, end) by @p by,
         *        unless the node is attached at that time.
         */
        void shift_pair(
            std::string_view text,
            std::string_view name,
            std::size_t open,
            std::size_t end,
            double by,
            const std::vector<std::pair<double, bool>> &links,
            std::vector<Edit> &edits
        )
        {
            const auto span = value_span(text, name, open, end);
            if (!span)
            {
                return;
            }
            const std::string_view pair = text.substr(span->first, span->second - span->first);
            const std::size_t comma = pair.find(',');
            if (comma == std::string_view::npos)
            {
                return;
            }
            const auto time = parse_double(pair.substr(0, comma));
            const auto value = parse_double(pair.substr(comma + 1));
            if (!time || !value || attached_at(links, *time))
            {
                return;
            }
            edits.push_back({span->first + comma + 1, span->second, format_metres(*value + by)});
        }

        /** @brief The edits that shift the world keys of the Position track in the node body [body, body_end). */
        void shift_track(
            std::string_view text,
            std::size_t body,
            std::size_t body_end,
            const ScenePoint &offset,
            std::vector<Edit> &edits
        )
        {
            const std::size_t track = text.substr(0, body_end).find(POSITION_TRACK, body);
            if (track == std::string_view::npos)
            {
                return;
            }
            const std::size_t track_end = text.substr(0, body_end).find(TRACK_END, track);
            const std::vector<std::pair<double, bool>> links = link_keys(text, body, body_end);
            const std::array<double, 3> by{offset.x, offset.y, offset.z};
            std::size_t axis = 0;
            for (std::size_t sub = text.find(SUBTRACK_TAG, track);
                 sub != std::string_view::npos && sub < track_end && axis < by.size();
                 sub = text.find(SUBTRACK_TAG, sub + 1), ++axis)
            {
                const std::size_t sub_tag_end = tag_end(text, sub);
                shift_pair(text, "defaultValue", sub, sub_tag_end, by[axis], links, edits);
                if (self_closing(text, sub_tag_end))
                {
                    continue;
                }
                const std::size_t sub_end = std::min(text.find(SUBTRACK_END, sub_tag_end), track_end);
                for (std::size_t key = text.find(KEY_TAG, sub_tag_end); key != std::string_view::npos && key < sub_end;
                     key = text.find(KEY_TAG, key + 1))
                {
                    shift_pair(text, "value", key, tag_end(text, key), by[axis], links, edits);
                }
            }
        }

        /** @brief @p text with @p edits (not overlapping) applied. */
        [[nodiscard]] std::string apply(std::string_view text, std::vector<Edit> &edits)
        {
            std::ranges::sort(edits, {}, &Edit::start);
            std::string out;
            out.reserve(text.size());
            std::size_t from = 0;
            for (const Edit &edit : edits)
            {
                out.append(text.substr(from, edit.start - from));
                out.append(edit.text);
                from = edit.end;
            }
            out.append(text.substr(from));
            return out;
        }
    } // namespace

    std::optional<ScenePoint> parse_scene_point(std::string_view text)
    {
        const std::size_t first = text.find(',');
        const std::size_t second = first == std::string_view::npos ? first : text.find(',', first + 1);
        if (second == std::string_view::npos)
        {
            return std::nullopt;
        }
        const auto x = parse_double(text.substr(0, first));
        const auto y = parse_double(text.substr(first + 1, second - first - 1));
        const auto z = parse_double(text.substr(second + 1));
        if (!x || !y || !z)
        {
            return std::nullopt;
        }
        return ScenePoint{*x, *y, *z};
    }

    std::optional<ScenePoint> scene_origin(std::string_view layer_xml, std::string_view sequence)
    {
        const std::string_view text = layer_xml;
        std::size_t open = text.find(SEQUENCE_TAG);
        while (open != std::string_view::npos && value_of(text, "Name", open, tag_end(text, open)) != sequence)
        {
            open = text.find(SEQUENCE_TAG, open + 1);
        }
        if (open == std::string_view::npos)
        {
            return std::nullopt;
        }
        const std::size_t sequence_end = text.find(SEQUENCE_END, open);
        std::unordered_set<std::string> animated;
        for (std::size_t node = text.find(NODE_TAG, open); node != std::string_view::npos && node < sequence_end;
             node = text.find(NODE_TAG, node + 1))
        {
            const std::size_t node_tag_end = tag_end(text, node);
            if (self_closing(text, node_tag_end))
            {
                continue;
            }
            const std::size_t node_end = text.find(NODE_END, node_tag_end);
            if (text.substr(node_tag_end, node_end - node_tag_end).find(ANIMATION_TRACK) != std::string_view::npos)
            {
                animated.emplace(value_of(text, "EntityGUID", node, node_tag_end));
            }
        }
        // The first-seen order breaks ties, so the answer does not depend on the hash map.
        std::map<std::string, std::pair<std::size_t, std::size_t>> counts;
        std::size_t order = 0;
        for (std::size_t entity = text.find(ENTITY_TAG); entity != std::string_view::npos;
             entity = text.find(ENTITY_TAG, entity + 1))
        {
            const std::size_t entity_end = tag_end(text, entity);
            if (value_of(text, "EntityClass", entity, entity_end) != "AnimChar" ||
                !animated.contains(std::string{value_of(text, "EntityGuid", entity, entity_end)}) ||
                !is_placed(text, entity, entity_end))
            {
                continue;
            }
            auto &[count, first] =
                counts.try_emplace(std::string{value_of(text, "Pos", entity, entity_end)}, 0, order++).first->second;
            ++count;
        }
        const auto best = std::ranges::max_element(
            counts,
            [](const auto &a, const auto &b)
            {
                return a.second.first != b.second.first ? a.second.first < b.second.first
                                                        : a.second.second > b.second.second;
            }
        );
        return best == counts.end() ? std::nullopt : parse_scene_point(best->first);
    }

    MovedScene move_scene(std::string_view layer_xml, ScenePoint offset)
    {
        const std::string_view text = layer_xml;
        MovedScene moved;
        std::vector<Edit> edits;
        std::unordered_set<std::string> placed;
        for (std::size_t entity = text.find(ENTITY_TAG); entity != std::string_view::npos;
             entity = text.find(ENTITY_TAG, entity + 1))
        {
            const std::size_t entity_end = tag_end(text, entity);
            if (!is_placed(text, entity, entity_end))
            {
                continue;
            }
            const auto span = value_span(text, "Pos", entity, entity_end);
            const auto point = parse_scene_point(text.substr(span->first, span->second - span->first));
            if (!point)
            {
                continue;
            }
            edits.push_back(
                {span->first,
                 span->second,
                 std::format(
                     "{},{},{}",
                     format_metres(point->x + offset.x),
                     format_metres(point->y + offset.y),
                     format_metres(point->z + offset.z)
                 )}
            );
            placed.emplace(value_of(text, "EntityGuid", entity, entity_end));
            ++moved.entities;
        }
        const std::size_t entity_edits = edits.size();
        for (std::size_t node = text.find(NODE_TAG); node != std::string_view::npos;
             node = text.find(NODE_TAG, node + 1))
        {
            const std::size_t node_tag_end = tag_end(text, node);
            if (self_closing(text, node_tag_end) ||
                !placed.contains(std::string{value_of(text, "EntityGUID", node, node_tag_end)}))
            {
                continue;
            }
            shift_track(text, node_tag_end, text.find(NODE_END, node_tag_end), offset, edits);
        }
        moved.keys = edits.size() - entity_edits;
        moved.layer_xml = apply(text, edits);
        return moved;
    }
} // namespace AnimationFramework
