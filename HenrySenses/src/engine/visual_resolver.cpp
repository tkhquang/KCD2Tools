/**
 * @file engine/visual_resolver.cpp
 * @brief Finds the mesh the player sees for an interaction trigger.
 */

#include "engine/visual_resolver.hpp"
#include "constants.hpp"
#include "engine/engine_env.hpp"
#include "engine/entity_access.hpp"
#include "engine/octree_query.hpp"
#include "rtti_types.hpp"
#include "engine/seh.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace HenrySenses
{
    namespace
    {
        using GetNameFn = const char *(__fastcall *)(std::uintptr_t node);

        // The probe point must lie within this horizontal distance of a candidate's footprint (a trigger sits on or
        // around its object, never beside it), and the candidate's bounds must span the probe's height band.
        constexpr float MAX_HORIZONTAL_GAP = 0.15f;
        // A mesh named after the trigger's object may stand beside it: a wash trigger covers where the player stands.
        constexpr float NAMED_MAX_HORIZONTAL_GAP = 1.0f;
        // The search box around a trigger: its footprint widened by SEARCH_MARGIN, and at least
        // NAMED_MAX_HORIZONTAL_GAP from its centre so that every named mesh the gap test accepts is found (a bed
        // trigger stands beside the bed), from SEARCH_BELOW under it to SEARCH_ABOVE over it (triggers sit on,
        // above or around the object).
        constexpr float SEARCH_HALF_WIDTH = NAMED_MAX_HORIZONTAL_GAP;
        constexpr float SEARCH_MARGIN = 0.25f;
        constexpr float SEARCH_BELOW = 1.6f;
        constexpr float SEARCH_ABOVE = 0.6f;
        // A trigger area larger than this on either horizontal axis is not an object footprint.
        constexpr float MAX_AREA_EXTENT = 4.0f;
        // The only meshes a trigger's object can be: static brushes (movable ones too) and entity render proxies.
        constexpr std::array<std::uint32_t, 3> MESH_TYPES{
            constants::RENDERNODE_TYPE_BRUSH,
            constants::RENDERNODE_TYPE_MOVABLE_BRUSH,
            constants::RENDERNODE_TYPE_RENDER_PROXY
        };
        constexpr std::array<std::uint32_t, 2> BRUSH_TYPES{
            constants::RENDERNODE_TYPE_BRUSH,
            constants::RENDERNODE_TYPE_MOVABLE_BRUSH
        };
        constexpr std::array<std::uint32_t, 1> OWNED_BRUSH_TYPES{constants::RENDERNODE_TYPE_BRUSH};
        constexpr std::array<std::uint32_t, 1> PROXY_TYPES{constants::RENDERNODE_TYPE_RENDER_PROXY};
        constexpr float BAND_BELOW = 1.4f;
        constexpr float BAND_ABOVE = 0.4f;
        // Plausible object sizes: anything larger is a floor, wall, roof or merged prefab. A mesh named after the
        // object may be larger (a long wall bench, a canopy bed).
        constexpr float MIN_EXTENT = 0.15f;
        constexpr float MAX_EXTENT = 4.0f;
        constexpr float MAX_VOLUME = 10.0f;
        constexpr float NAMED_MAX_EXTENT = 6.0f;
        constexpr float NAMED_MAX_VOLUME = 20.0f;
        // A mesh this flat relative to its footprint, and this wide, is a floor, platform or plank.
        constexpr float FLOOR_MAX_HEIGHT = 0.25f;
        constexpr float FLOOR_MIN_WIDTH = 1.0f;
        constexpr std::uint32_t MAX_NODES = 2048;
        constexpr std::size_t MAX_NAME_LENGTH = 160;
        constexpr std::size_t MAX_TRACE_CANDIDATES = 16;
        // A mesh whose model file names the trigger's object beats any unnamed one. Among named meshes, the one whose
        // file has the trigger's last own word and covers the trigger (its object: "trigger_pan") ranks first, an
        // entity of the same prefab instance second, a model file named by the object of the trigger's prefab
        // ("bed_bench16" -> a bench, "camperWineBarrel15" -> a barrel) third, a file with another of its own words
        // fourth, a keyword (the kind's words, the class's noun: "bed" of BedTrigger) fifth, and bedding last.
        constexpr float BEDDING_BONUS = 90.0f;
        constexpr float NAMED_BONUS = 100.0f;
        constexpr float OWN_NAMED_BONUS = 110.0f;
        constexpr float PREFAB_OBJECT_BONUS = 120.0f;
        constexpr float SAME_PREFAB_BONUS = 150.0f;
        constexpr float OWN_OBJECT_BONUS = 200.0f;
        // The furniture a prefab's name may call its object. Only these count: the rest of a prefab name is
        // qualifiers ("high", "double", "home") and places ("fireplace", "camper") that would name other meshes.
        constexpr std::array<std::string_view, 13> PREFAB_OBJECTS{
            "bed",
            "bench",
            "chair",
            "stool",
            "throne",
            "tub",
            "trough",
            "kettle",
            "cauldron",
            "barrel",
            "anvil",
            "grindstone",
            "pillory"
        };

        // The faced object (resolve_faced_visual): the use spot's facing ray must enter its footprint within this
        // reach, the footprint must cover the trigger point widened by this tolerance, its height range must overlap
        // the band a kneeling or standing player faces, and a second mesh entered within this margin makes it
        // ambiguous.
        constexpr float FACED_REACH = 2.5f;
        constexpr float FACED_TRIGGER_TOLERANCE = 0.35f;
        constexpr float FACED_BAND_BELOW = 0.3f;
        constexpr float FACED_BAND_ABOVE = 2.0f;
        constexpr float FACED_AMBIGUITY = 0.2f;

        // A pose match: a pivot within this distance of the prediction is the object (level instances measure under
        // 1 mm), searched for in a box this far around it (a brush's bounds may not reach its pivot).
        constexpr float POSE_TOLERANCE = 0.03f;
        constexpr float POSE_BOX = 0.5f;
        constexpr std::uint32_t MAX_POSE_NODES = 512;
        // A runtime prefab's spawned brushes are searched for this far around the prefab entity.
        constexpr float OWNED_BOX_HALF = 1.5f;
        constexpr float OWNED_BOX_BELOW = 1.5f;
        constexpr float OWNED_BOX_ABOVE = 2.0f;
        // The entity class that spawns runtime prefabs; it draws nothing itself.
        constexpr std::string_view RUNTIME_PREFAB_CLASS = "RuntimePrefab";
        // A brush found again after streaming recreated it matches the one seen before within this on every bound.
        constexpr float BOUNDS_TOLERANCE = 0.01f;
        // Brushes listed near a pose in the trace.
        constexpr std::size_t POSE_TRACE_NEAREST = 4;

        // Engine helper geometry (occluders, planes, markers), never the object itself.
        constexpr std::string_view SPECIAL_PATH = "objects/special/";
        // Entity classes that stand near a trigger without being its object: actors, loot and carried items.
        constexpr std::array<std::string_view, 11> REJECTED_ENTITY_CLASSES{
            "Player",
            "NPC",
            "NPC_Female",
            "Horse",
            "Dog",
            "Fish",
            "Animal",
            "PickableItem",
            "MissileWeapon",
            "Torch",
            "ItemSlot"
        };

        [[nodiscard]] const char *call_get_name(std::uintptr_t fn, std::uintptr_t node) noexcept
        {
            __try
            {
                return reinterpret_cast<GetNameFn>(fn)(node);
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return nullptr;
            }
        }

        [[nodiscard]] bool node_hidden(std::uintptr_t node) noexcept
        {
            const auto flags =
                DMK::memory::read<std::uint64_t>(DMK::Address{node + constants::RENDERNODE_RNDFLAGS_OFFSET});
            return !flags || (*flags & constants::ERF_HIDDEN) != 0;
        }

        [[nodiscard]] bool mesh_type(std::uint32_t type) noexcept
        {
            return std::find(MESH_TYPES.begin(), MESH_TYPES.end(), type) != MESH_TYPES.end();
        }

        [[nodiscard]] bool plausible_size(const game_structures::Aabb &b, bool named) noexcept
        {
            const float ex = b.max.x - b.min.x;
            const float ey = b.max.y - b.min.y;
            const float ez = b.max.z - b.min.z;
            const float largest = std::max({ex, ey, ez});
            return largest >= MIN_EXTENT && largest <= (named ? NAMED_MAX_EXTENT : MAX_EXTENT) &&
                   ex * ey * ez <= (named ? NAMED_MAX_VOLUME : MAX_VOLUME);
        }

        [[nodiscard]] bool floor_like(const game_structures::Aabb &b) noexcept
        {
            const float width = std::max(b.max.x - b.min.x, b.max.y - b.min.y);
            return b.max.z - b.min.z < FLOOR_MAX_HEIGHT && width >= FLOOR_MIN_WIDTH;
        }

        [[nodiscard]] float volume_of(const game_structures::Aabb &b) noexcept
        {
            return (b.max.x - b.min.x) * (b.max.y - b.min.y) * (b.max.z - b.min.z);
        }

        [[nodiscard]] float distance_between(const game_structures::Vec3f &a, const game_structures::Vec3f &b) noexcept
        {
            const float dx = a.x - b.x;
            const float dy = a.y - b.y;
            const float dz = a.z - b.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        [[nodiscard]] std::string lowercase(std::string text)
        {
            std::transform(
                text.begin(),
                text.end(),
                text.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); }
            );
            return text;
        }

        /** @brief A brush's world pivot: the translation of its m_Matrix. */
        [[nodiscard]] std::optional<game_structures::Vec3f> brush_pivot(std::uintptr_t brush) noexcept
        {
            const auto matrix =
                DMK::memory::read<game_structures::Matrix34f>(DMK::Address{brush + constants::BRUSH_MATRIX_OFFSET});
            if (!matrix)
            {
                return std::nullopt;
            }
            const game_structures::Vec3f pivot{matrix->m[0][3], matrix->m[1][3], matrix->m[2][3]};
            if (!std::isfinite(pivot.x) || !std::isfinite(pivot.y) || !std::isfinite(pivot.z))
            {
                return std::nullopt;
            }
            return pivot;
        }

        /**
         * @brief The words of a model path's file name or of an entity name before its prefab tag, lower case, split
         *        at every non-letter and camel-case capital ("fur_c_benched" -> fur, c, benched).
         */
        [[nodiscard]] std::vector<std::string> word_tokens(std::string_view text)
        {
            text = text.substr(0, text.find('['));
            if (const std::size_t slash = text.find_last_of("/\\"); slash != std::string_view::npos)
            {
                text.remove_prefix(slash + 1);
            }
            std::vector<std::string> tokens;
            std::string token;
            for (const char c : text)
            {
                const bool upper = c >= 'A' && c <= 'Z';
                const bool alpha = upper || (c >= 'a' && c <= 'z');
                if (!alpha || (upper && !token.empty()))
                {
                    if (!token.empty())
                    {
                        tokens.push_back(std::move(token));
                        token.clear();
                    }
                }
                if (alpha)
                {
                    token.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
                }
            }
            if (!token.empty())
            {
                tokens.push_back(std::move(token));
            }
            return tokens;
        }

        /** @brief The token is @p word or its plural ("bench", "benches"); "benched" is not "bench". */
        [[nodiscard]] bool token_is(std::string_view token, std::string_view word) noexcept
        {
            if (word.empty() || !token.starts_with(word))
            {
                return false;
            }
            const std::string_view rest = token.substr(word.size());
            return rest.empty() || rest == "s" || rest == "es";
        }

        template <typename Words>
        [[nodiscard]] bool tokens_have(const std::vector<std::string> &tokens, const Words &words)
        {
            return std::any_of(
                words.begin(),
                words.end(),
                [&tokens](const auto &word)
                {
                    return std::any_of(
                        tokens.begin(),
                        tokens.end(),
                        [&word](const std::string &token) { return token_is(token, word); }
                    );
                }
            );
        }

        /**
         * @brief Why a candidate mesh is not the trigger's object, or nullptr when it may be.
         */
        [[nodiscard]] const char *reject_reason(std::uintptr_t node, std::uint32_t type, const std::string &name)
        {
            if (type != constants::RENDERNODE_TYPE_RENDER_PROXY)
            {
                return lowercase(name).starts_with(SPECIAL_PATH) ? "helper" : nullptr;
            }
            const auto owner =
                DMK::memory::read<std::uintptr_t>(DMK::Address{node + constants::RENDER_PROXY_ENTITY_OFFSET});
            if (!owner || *owner == 0 || !object_is(GameClass::Entity, *owner))
            {
                return "no entity";
            }
            const std::string klass = entity_class_name(*owner);
            // Another interaction trigger or an AI smart-object helper: invisible, and never the object itself.
            if (klass.ends_with("Trigger") || klass.find("SmartObject") != std::string::npos)
            {
                return "trigger";
            }
            // A runtime prefab only spawns its objects (brushes found here on their own) and draws nothing itself.
            if (klass == RUNTIME_PREFAB_CLASS)
            {
                return "prefab container";
            }
            for (const std::string_view rejected : REJECTED_ENTITY_CLASSES)
            {
                if (klass.starts_with(rejected))
                {
                    return "actor or item";
                }
            }
            return nullptr;
        }

        /**
         * @brief The furniture words of the name of a trigger's prefab: "[Bed/bed_bench16_<guid>]" -> {"bed",
         *        "bench"}, "[profession/camper/camperWineBarrel15_<guid>]" -> {"barrel"}.
         */
        [[nodiscard]] std::vector<std::string> prefab_object_words(std::string_view tag)
        {
            constexpr std::size_t guid_length = 36;
            std::string_view name = tag;
            if (name.starts_with('['))
            {
                name.remove_prefix(1);
            }
            if (name.ends_with(']'))
            {
                name.remove_suffix(1);
            }
            if (name.size() > guid_length && name[name.size() - guid_length - 1] == '_')
            {
                name.remove_suffix(guid_length + 1);
            }
            std::vector<std::string> words = word_tokens(name);
            std::erase_if(
                words,
                [](const std::string &word)
                { return std::find(PREFAB_OBJECTS.begin(), PREFAB_OBJECTS.end(), word) == PREFAB_OBJECTS.end(); }
            );
            return words;
        }

        [[nodiscard]] std::string size_text(const game_structures::Aabb &b)
        {
            return std::format("{:.2f}x{:.2f}x{:.2f}", b.max.x - b.min.x, b.max.y - b.min.y, b.max.z - b.min.z);
        }

        /** @brief A nearby brush seen by a pose lookup, for the trace. */
        struct NearBrush
        {
            float offset{0.0f};
            std::uintptr_t node{0};
        };

        void append_nearest(std::string &trace, std::vector<NearBrush> &nearby)
        {
            std::sort(
                nearby.begin(),
                nearby.end(),
                [](const NearBrush &a, const NearBrush &b) { return a.offset < b.offset; }
            );
            for (std::size_t i = 0; i < nearby.size() && i < POSE_TRACE_NEAREST; ++i)
            {
                trace += std::format(
                    "\n      {} pivot {:.1f} cm off{}",
                    render_node_name(nearby[i].node),
                    nearby[i].offset * 100.0f,
                    node_hidden(nearby[i].node) ? " (hidden)" : ""
                );
            }
        }

        /** @brief Appends the nodes of @p extra not already in @p nodes. */
        void add_extra_nodes(std::vector<std::uintptr_t> &nodes, std::span<const std::uintptr_t> extra)
        {
            for (const std::uintptr_t node : extra)
            {
                if (node != 0 && std::find(nodes.begin(), nodes.end(), node) == nodes.end())
                {
                    nodes.push_back(node);
                }
            }
        }

        [[nodiscard]] const char *query_failure(OctreeQueryResult result) noexcept
        {
            switch (result)
            {
            case OctreeQueryResult::Unavailable:
                return "octree query unavailable";
            case OctreeQueryResult::TooMany:
                return "too many nodes in the box";
            case OctreeQueryResult::Faulted:
                return "octree query faulted";
            case OctreeQueryResult::Ok:
            default:
                return "octree query failed";
            }
        }

        /** @brief True when every bound of @p a lies within BOUNDS_TOLERANCE of the same bound of @p b. */
        [[nodiscard]] bool same_bounds(const game_structures::Aabb &a, const game_structures::Aabb &b) noexcept
        {
            return std::abs(a.min.x - b.min.x) <= BOUNDS_TOLERANCE && std::abs(a.min.y - b.min.y) <= BOUNDS_TOLERANCE &&
                   std::abs(a.min.z - b.min.z) <= BOUNDS_TOLERANCE && std::abs(a.max.x - b.max.x) <= BOUNDS_TOLERANCE &&
                   std::abs(a.max.y - b.max.y) <= BOUNDS_TOLERANCE && std::abs(a.max.z - b.max.z) <= BOUNDS_TOLERANCE;
        }
    } // namespace

    bool same_model(std::string_view a, std::string_view b) noexcept
    {
        if (a.size() != b.size())
        {
            return false;
        }
        auto fold = [](char c)
        { return c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            if (fold(a[i]) != fold(b[i]))
            {
                return false;
            }
        }
        return true;
    }

    bool is_brush_node(std::uintptr_t node) noexcept
    {
        if (node == 0 || !DMK::memory::is_plausible_ptr(DMK::Address{node}))
        {
            return false;
        }
        const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{node});
        return vtable && (vtable_is(GameClass::Brush, *vtable) || vtable_is(GameClass::OwnedBrush, *vtable) ||
                          vtable_is(GameClass::MovableBrush, *vtable));
    }

    bool brush_matches(std::uintptr_t brush, const game_structures::Aabb &bounds) noexcept
    {
        // A freed brush whose memory was reused fails the identity check; one reused by another brush fails the
        // bounds check.
        if (!is_brush_node(brush))
        {
            return false;
        }
        const std::optional<game_structures::Aabb> now = render_node_bounds(brush);
        return now.has_value() && same_bounds(*now, bounds);
    }

    bool is_movable_brush_node(std::uintptr_t node) noexcept
    {
        if (node == 0 || !DMK::memory::is_plausible_ptr(DMK::Address{node}))
        {
            return false;
        }
        const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{node});
        return vtable && vtable_is(GameClass::MovableBrush, *vtable);
    }

    std::optional<game_structures::Aabb>
    brush_revalidate(std::uintptr_t brush, const game_structures::Aabb &bounds, std::string_view model) noexcept
    {
        if (brush == 0 || !DMK::memory::is_plausible_ptr(DMK::Address{brush}))
        {
            return std::nullopt;
        }
        const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{brush});
        if (!vtable)
        {
            return std::nullopt;
        }
        const bool movable = vtable_is(GameClass::MovableBrush, *vtable);
        if (!movable && !vtable_is(GameClass::Brush, *vtable) && !vtable_is(GameClass::OwnedBrush, *vtable))
        {
            return std::nullopt;
        }
        const std::optional<game_structures::Aabb> now = render_node_bounds(brush);
        if (!now.has_value())
        {
            return std::nullopt;
        }
        // Without a model to compare, a movable brush is held to its bounds as well (the entry was made for a brush
        // that does not move, and another object took its memory).
        if (!movable || model.empty())
        {
            return same_bounds(*now, bounds) ? now : std::nullopt;
        }
        // A movable brush that moved still shows its model: the name identifies it where the bounds cannot.
        try
        {
            if (!same_model(render_node_name(brush), model))
            {
                return std::nullopt;
            }
        }
        catch (...)
        {
            return std::nullopt;
        }
        return now;
    }

    bool has_word(std::string_view text, std::span<const std::string_view> words)
    {
        return tokens_have(word_tokens(text), words);
    }

    bool has_word(std::string_view text, std::span<const std::string> words)
    {
        return tokens_have(word_tokens(text), words);
    }

    std::vector<std::string> trigger_name_words(std::string_view entity_name)
    {
        constexpr std::size_t min_word = 3;
        constexpr std::array<std::string_view, 12> generic{
            "trigger",
            "action",
            "sequence",
            "interaction",
            "smart",
            "object",
            "use",
            "area",
            "proximity",
            "shape",
            "helper",
            "spot"
        };
        std::vector<std::string> words;
        for (std::string &word : word_tokens(entity_name))
        {
            if (word.size() >= min_word && std::find(generic.begin(), generic.end(), word) == generic.end() &&
                std::find(words.begin(), words.end(), word) == words.end())
            {
                words.push_back(std::move(word));
            }
        }
        return words;
    }

    std::string_view prefab_instance_tag(std::string_view entity_name) noexcept
    {
        // A tag without a GUID-length body ("[1]") says nothing about the instance.
        constexpr std::size_t min_tag_length = 24;
        const std::size_t open = entity_name.find('[');
        if (open == std::string_view::npos || entity_name.size() - open < min_tag_length || entity_name.back() != ']')
        {
            return {};
        }
        return entity_name.substr(open);
    }

    std::optional<VisualNode> resolve_trigger_visual(const TriggerSearch &search, std::string *trace)
    {
        // An area trigger (a wash tub's) is placed around its object: the probe is its footprint's centre, not its
        // origin.
        game_structures::Vec3f probe = search.position;
        float half_x = SEARCH_HALF_WIDTH;
        float half_y = SEARCH_HALF_WIDTH;
        if (search.trigger_bounds.has_value())
        {
            const game_structures::Aabb &t = *search.trigger_bounds;
            const float ex = t.max.x - t.min.x;
            const float ey = t.max.y - t.min.y;
            if (ex >= 0.0f && ey >= 0.0f && ex <= MAX_AREA_EXTENT && ey <= MAX_AREA_EXTENT)
            {
                probe.x = 0.5f * (t.min.x + t.max.x);
                probe.y = 0.5f * (t.min.y + t.max.y);
                probe.z = t.min.z;
                half_x = std::max(half_x, 0.5f * ex + SEARCH_MARGIN);
                half_y = std::max(half_y, 0.5f * ey + SEARCH_MARGIN);
            }
        }
        const game_structures::Aabb box{
            {probe.x - half_x, probe.y - half_y, probe.z - SEARCH_BELOW},
            {probe.x + half_x, probe.y + half_y, probe.z + SEARCH_ABOVE}
        };
        std::vector<std::uintptr_t> nodes;
        if (const OctreeQueryResult result = query_render_nodes_of_types(box, MESH_TYPES, nodes, MAX_NODES);
            result != OctreeQueryResult::Ok)
        {
            if (trace != nullptr)
            {
                *trace = std::format("\n    {}", query_failure(result));
            }
            return std::nullopt;
        }
        add_extra_nodes(nodes, search.extra_nodes);
        const std::vector<std::string> prefab_words = prefab_object_words(search.prefab_tag);
        std::optional<VisualNode> best{};
        float best_score = 0.0f;
        // Trace lines, and whether the node got past the inspection (visible mesh with bounds): a found mesh lists
        // the first few of those, a miss every node.
        std::vector<std::pair<std::string, bool>> lines;
        for (const std::uintptr_t node : nodes)
        {
            if (node == search.exclude_node)
            {
                continue;
            }
            const char *inspection = nullptr;
            std::uint32_t type = 0;
            std::optional<game_structures::Aabb> bounds{};
            if (node_hidden(node))
            {
                inspection = "hidden";
            }
            else
            {
                type = render_node_type(node);
                if (!mesh_type(type))
                {
                    inspection = "not a mesh";
                }
                else if (bounds = render_node_bounds(node); !bounds.has_value())
                {
                    inspection = "no bounds";
                }
            }
            if (inspection != nullptr)
            {
                if (trace != nullptr)
                {
                    lines.emplace_back(
                        std::format(
                            "\n    {} type={:#x}{} {}",
                            render_node_name(node),
                            type,
                            bounds.has_value() ? " " + size_text(*bounds) : std::string{},
                            inspection
                        ),
                        false
                    );
                }
                continue;
            }
            const game_structures::Aabb &b = *bounds;
            const bool brush = type != constants::RENDERNODE_TYPE_RENDER_PROXY;
            const std::string name = render_node_name(node);
            const char *reason = reject_reason(node, type, name);
            float score = 0.0f;
            // Words are whole tokens of the model's file name (an entity's name before its tag), plural allowed:
            // "fur_c_benched" is not a bench, and a "beds/" folder names nothing.
            const std::vector<std::string> tokens = word_tokens(name);
            // An entity placed in the same prefab instance as the trigger is its object by construction; any other
            // mesh must be named after it.
            const bool same_prefab =
                !brush && !search.prefab_tag.empty() && prefab_instance_tag(name) == search.prefab_tag;
            const bool own_named = brush && tokens_have(tokens, search.own_words);
            const bool own_last = own_named && std::any_of(
                                                   tokens.begin(),
                                                   tokens.end(),
                                                   [&search](const std::string &token)
                                                   { return token_is(token, search.own_words.back()); }
                                               );
            const bool prefab_object = brush && tokens_have(tokens, prefab_words);
            const bool keyword = tokens_have(tokens, search.keywords);
            const bool bedding = brush && tokens_have(tokens, search.bedding_words);
            const bool named = same_prefab || own_named || prefab_object || keyword || bedding;
            // With keywords, only a mesh named after the object counts: size and position alone would pick the
            // floor, a beam or a neighbouring object, and a marker is better than the wrong mesh.
            if (reason == nullptr && !search.keywords.empty() && !named)
            {
                reason = "unnamed";
            }
            if (reason == nullptr && !plausible_size(b, named))
            {
                reason = "size";
            }
            if (reason == nullptr)
            {
                if (!named && floor_like(b))
                {
                    reason = "floor";
                }
                else if (b.max.z < probe.z - BAND_BELOW || b.min.z > probe.z + BAND_ABOVE)
                {
                    reason = "height";
                }
            }
            bool own_object = false;
            if (reason == nullptr)
            {
                const float gx = std::max({0.0f, b.min.x - probe.x, probe.x - b.max.x});
                const float gy = std::max({0.0f, b.min.y - probe.y, probe.y - b.max.y});
                const float gap = std::sqrt(gx * gx + gy * gy);
                if (gap > (named ? NAMED_MAX_HORIZONTAL_GAP : MAX_HORIZONTAL_GAP))
                {
                    reason = "beside";
                }
                else
                {
                    // The trigger's own object covers the trigger: a mesh of that name one metre away belongs to the
                    // neighbouring piece of furniture.
                    own_object = own_last && gap <= 0.0f;
                    // The mesh centred under the probe wins; among those the smaller one, so a table under a bench
                    // seat loses to the bench.
                    const float cx = 0.5f * (b.min.x + b.max.x) - probe.x;
                    const float cy = 0.5f * (b.min.y + b.max.y) - probe.y;
                    const float bonus = own_object      ? OWN_OBJECT_BONUS
                                        : same_prefab   ? SAME_PREFAB_BONUS
                                        : prefab_object ? PREFAB_OBJECT_BONUS
                                        : own_named     ? OWN_NAMED_BONUS
                                        : keyword       ? NAMED_BONUS
                                        : bedding       ? BEDDING_BONUS
                                                        : 0.0f;
                    score = std::sqrt(cx * cx + cy * cy) + 0.1f * volume_of(b) - bonus;
                    if (!best.has_value() || score < best_score)
                    {
                        best = VisualNode{node, type, b};
                        best_score = score;
                    }
                }
            }
            if (trace != nullptr)
            {
                lines.emplace_back(
                    std::format(
                        "\n    {}{} {} {}",
                        brush ? "" : "entity ",
                        name,
                        size_text(b),
                        reason != nullptr ? std::string{reason}
                                          : std::format(
                                                "score {:.2f}{}",
                                                score,
                                                own_object      ? " (own object)"
                                                : same_prefab   ? " (same prefab)"
                                                : prefab_object ? " (prefab object)"
                                                : own_named     ? " (own name)"
                                                : keyword       ? " (named)"
                                                : bedding       ? " (bedding)"
                                                                : ""
                                            )
                    ),
                    true
                );
            }
        }
        if (trace != nullptr)
        {
            std::size_t traced = 0;
            for (const auto &[text, inspected] : lines)
            {
                // A found mesh is explained by the first few candidates; a miss by every node in the box, including
                // those dropped before naming (hidden, not a mesh), so a streaming gap reads differently from a miss.
                if (best.has_value() && (!inspected || traced >= MAX_TRACE_CANDIDATES))
                {
                    continue;
                }
                *trace += text;
                ++traced;
            }
            if (nodes.size() > traced)
            {
                *trace += std::format("\n    ({} node(s) in the box)", nodes.size());
            }
            if (nodes.empty())
            {
                *trace += "\n    (no node in the box)";
            }
        }
        return best;
    }

    std::optional<VisualNode> resolve_faced_visual(const FacedSearch &search, std::string *trace)
    {
        const float fx = search.forward.x;
        const float fy = search.forward.y;
        const float flat = std::sqrt(fx * fx + fy * fy);
        if (!(flat > 0.5f))
        {
            if (trace != nullptr)
            {
                *trace = "\n    the use spot does not face level";
            }
            return std::nullopt;
        }
        const float dx = fx / flat;
        const float dy = fy / flat;
        const game_structures::Vec3f &s = search.spot;
        const game_structures::Vec3f &t = search.trigger;
        const game_structures::Aabb box{
            {std::min(s.x, t.x) - FACED_REACH, std::min(s.y, t.y) - FACED_REACH, std::min(s.z, t.z) - FACED_BAND_BELOW},
            {std::max(s.x, t.x) + FACED_REACH, std::max(s.y, t.y) + FACED_REACH, std::max(s.z, t.z) + FACED_BAND_ABOVE}
        };
        std::vector<std::uintptr_t> nodes;
        if (const OctreeQueryResult result = query_render_nodes_of_types(box, MESH_TYPES, nodes, MAX_NODES);
            result != OctreeQueryResult::Ok)
        {
            if (trace != nullptr)
            {
                *trace = std::format("\n    {}", query_failure(result));
            }
            return std::nullopt;
        }
        add_extra_nodes(nodes, search.extra_nodes);
        struct Faced
        {
            VisualNode node;
            float entry;
        };
        std::optional<Faced> best{};
        std::optional<Faced> second{};
        std::vector<std::string> lines;
        for (const std::uintptr_t node : nodes)
        {
            if (node == search.exclude_node || node_hidden(node))
            {
                continue;
            }
            const std::uint32_t type = render_node_type(node);
            if (!mesh_type(type))
            {
                continue;
            }
            const std::optional<game_structures::Aabb> bounds = render_node_bounds(node);
            if (!bounds.has_value())
            {
                continue;
            }
            const game_structures::Aabb &b = *bounds;
            const std::string name = render_node_name(node);
            const char *reason = reject_reason(node, type, name);
            float entry = 0.0f;
            if (reason == nullptr && !plausible_size(b, false))
            {
                reason = "size";
            }
            if (reason == nullptr && floor_like(b))
            {
                reason = "floor";
            }
            if (reason == nullptr && (b.max.z < s.z - FACED_BAND_BELOW || b.min.z > s.z + FACED_BAND_ABOVE))
            {
                reason = "height";
            }
            if (reason == nullptr &&
                (t.x < b.min.x - FACED_TRIGGER_TOLERANCE || t.x > b.max.x + FACED_TRIGGER_TOLERANCE ||
                 t.y < b.min.y - FACED_TRIGGER_TOLERANCE || t.y > b.max.y + FACED_TRIGGER_TOLERANCE))
            {
                reason = "not at the trigger";
            }
            if (reason == nullptr && s.x >= b.min.x && s.x <= b.max.x && s.y >= b.min.y && s.y <= b.max.y)
            {
                reason = "under the spot";
            }
            if (reason == nullptr)
            {
                // Slab test of the facing ray against the footprint: where it enters, if it does within reach.
                float ray_enter = 0.0f;
                float ray_leave = FACED_REACH;
                const float origin[2] = {s.x, s.y};
                const float direction[2] = {dx, dy};
                const float low[2] = {b.min.x, b.min.y};
                const float high[2] = {b.max.x, b.max.y};
                for (int axis = 0; axis < 2 && ray_enter <= ray_leave; ++axis)
                {
                    if (std::fabs(direction[axis]) < 1e-6f)
                    {
                        if (origin[axis] < low[axis] || origin[axis] > high[axis])
                        {
                            ray_leave = -1.0f;
                        }
                        continue;
                    }
                    float a = (low[axis] - origin[axis]) / direction[axis];
                    float c = (high[axis] - origin[axis]) / direction[axis];
                    if (a > c)
                    {
                        std::swap(a, c);
                    }
                    ray_enter = std::max(ray_enter, a);
                    ray_leave = std::min(ray_leave, c);
                }
                if (ray_enter > ray_leave)
                {
                    reason = "not faced";
                }
                else
                {
                    entry = ray_enter;
                    const Faced candidate{VisualNode{node, type, b}, entry};
                    if (!best.has_value() || entry < best->entry)
                    {
                        second = best;
                        best = candidate;
                    }
                    else if (!second.has_value() || entry < second->entry)
                    {
                        second = candidate;
                    }
                }
            }
            // A faced mesh is always listed: the level's HLOD tiles alone can fill the trace before it.
            if (trace != nullptr && (reason == nullptr || lines.size() < MAX_TRACE_CANDIDATES))
            {
                lines.push_back(
                    std::format(
                        "\n    {}{} {} {}",
                        type == constants::RENDERNODE_TYPE_RENDER_PROXY ? "entity " : "",
                        name,
                        size_text(b),
                        reason != nullptr ? std::string{reason} : std::format("faced at {:.2f} m", entry)
                    )
                );
            }
        }
        if (trace != nullptr)
        {
            for (const std::string &line : lines)
            {
                *trace += line;
            }
            *trace += std::format("\n    ({} node(s) around the use spot)", nodes.size());
        }
        if (best.has_value() && second.has_value() && second->entry - best->entry < FACED_AMBIGUITY)
        {
            if (trace != nullptr)
            {
                *trace += std::format(
                    "\n    ambiguous: two meshes faced within {:.2f} m of each other",
                    second->entry - best->entry
                );
            }
            return std::nullopt;
        }
        return best.has_value() ? std::optional<VisualNode>{best->node} : std::nullopt;
    }

    std::optional<PoseHit> find_brush_at_pose(
        const game_structures::Vec3f &pose,
        std::string_view expected_model,
        const game_structures::Vec3f &trigger,
        std::span<const std::uintptr_t> extra_nodes,
        std::string *trace
    )
    {
        const game_structures::Aabb box{
            {pose.x - POSE_BOX, pose.y - POSE_BOX, pose.z - POSE_BOX},
            {pose.x + POSE_BOX, pose.y + POSE_BOX, pose.z + POSE_BOX}
        };
        std::vector<std::uintptr_t> nodes;
        if (const OctreeQueryResult result = query_render_nodes_of_types(box, BRUSH_TYPES, nodes, MAX_POSE_NODES);
            result != OctreeQueryResult::Ok)
        {
            if (trace != nullptr)
            {
                *trace += std::format(" ({})", query_failure(result));
            }
            return std::nullopt;
        }
        add_extra_nodes(nodes, extra_nodes);
        std::vector<PoseHit> hits;
        std::vector<NearBrush> nearby;
        for (const std::uintptr_t node : nodes)
        {
            if (!is_brush_node(node))
            {
                continue;
            }
            const std::optional<game_structures::Vec3f> pivot = brush_pivot(node);
            if (!pivot.has_value())
            {
                continue;
            }
            const float offset = distance_between(*pivot, pose);
            if (trace != nullptr)
            {
                nearby.push_back(NearBrush{offset, node});
            }
            // A hidden brush shows nothing to outline.
            if (offset > POSE_TOLERANCE || node_hidden(node))
            {
                continue;
            }
            const std::optional<game_structures::Aabb> bounds = render_node_bounds(node);
            if (!bounds.has_value())
            {
                continue;
            }
            hits.push_back(PoseHit{node, render_node_type(node), *bounds, render_node_name(node), offset, 0});
        }
        if (hits.empty())
        {
            if (trace != nullptr)
            {
                *trace += std::format(" ({} brush(es) in the box", nearby.size());
                if (!nearby.empty())
                {
                    *trace += ", nearest:";
                    append_nearest(*trace, nearby);
                }
                *trace += ")";
            }
            return std::nullopt;
        }
        // Two brushes on one pivot (a duplicate bench, a washing stone on its pier): the template's own model first,
        // then the one whose footprint holds the trigger, then the smaller.
        auto rank = [&](const PoseHit &hit)
        {
            const bool model = same_model(hit.model, expected_model);
            const bool holds = trigger.x >= hit.bounds.min.x && trigger.x <= hit.bounds.max.x &&
                               trigger.y >= hit.bounds.min.y && trigger.y <= hit.bounds.max.y;
            return std::tuple{!model, !holds, volume_of(hit.bounds)};
        };
        const auto chosen = std::min_element(
            hits.begin(),
            hits.end(),
            [&](const PoseHit &a, const PoseHit &b) { return rank(a) < rank(b); }
        );
        PoseHit hit = *chosen;
        hit.matches = hits.size();
        return hit;
    }

    std::optional<PrefabBrushHit> find_prefab_brush(
        const game_structures::Vec3f &pose,
        std::string_view prefab_tag,
        float tolerance,
        std::span<const std::string> words,
        std::span<const std::uintptr_t> extra_nodes,
        std::string *trace
    )
    {
        // The spawned brushes sit around the prefab's origin (a kettle hangs above the cauldron prefab's pivot).
        const game_structures::Aabb box{
            {pose.x - OWNED_BOX_HALF, pose.y - OWNED_BOX_HALF, pose.z - OWNED_BOX_BELOW},
            {pose.x + OWNED_BOX_HALF, pose.y + OWNED_BOX_HALF, pose.z + OWNED_BOX_ABOVE}
        };
        std::vector<std::uintptr_t> nodes;
        if (const OctreeQueryResult result = query_render_nodes_of_types(box, OWNED_BRUSH_TYPES, nodes, MAX_NODES);
            result != OctreeQueryResult::Ok)
        {
            if (trace != nullptr)
            {
                *trace += std::format(" ({})", query_failure(result));
            }
            return std::nullopt;
        }
        add_extra_nodes(nodes, extra_nodes);

        // Each owner is judged once: a prefab usually spawns a few brushes, and the checks read its class and name.
        struct Owner
        {
            std::uintptr_t entity;
            bool accepted;
            float offset;
        };
        std::vector<Owner> owners;
        struct Owned
        {
            std::uintptr_t owner;
            PoseHit hit;
        };
        std::vector<Owned> owned;
        std::size_t owned_seen = 0;
        for (const std::uintptr_t node : nodes)
        {
            if (!object_is(GameClass::OwnedBrush, node) || node_hidden(node))
            {
                continue;
            }
            ++owned_seen;
            const auto node_owner =
                DMK::memory::read<std::uintptr_t>(DMK::Address{node + constants::OWNED_BRUSH_OWNER_OFFSET});
            if (!node_owner || *node_owner == 0)
            {
                continue;
            }
            auto known = std::find_if(
                owners.begin(),
                owners.end(),
                [entity = *node_owner](const Owner &owner) { return owner.entity == entity; }
            );
            if (known == owners.end())
            {
                Owner judged{*node_owner, false, 0.0f};
                if (object_is(GameClass::Entity, *node_owner) && !entity_is_hidden(*node_owner) &&
                    entity_class_name(*node_owner) == RUNTIME_PREFAB_CLASS &&
                    (prefab_tag.empty() || prefab_instance_tag(entity_name(*node_owner)) == prefab_tag))
                {
                    if (const std::optional<game_structures::Vec3f> position = entity_world_position(*node_owner);
                        position.has_value())
                    {
                        judged.offset = distance_between(*position, pose);
                        judged.accepted = judged.offset <= tolerance;
                    }
                }
                owners.push_back(judged);
                known = std::prev(owners.end());
            }
            if (!known->accepted)
            {
                continue;
            }
            const std::optional<game_structures::Aabb> bounds = render_node_bounds(node);
            if (!bounds.has_value())
            {
                continue;
            }
            owned.push_back(
                Owned{
                    known->entity,
                    PoseHit{node, constants::RENDERNODE_TYPE_BRUSH, *bounds, render_node_name(node), 0.0f, 0}
                }
            );
        }

        // The prefab entity standing nearest the predicted position owns the object.
        const auto nearest = std::min_element(
            owners.begin(),
            owners.end(),
            [](const Owner &a, const Owner &b) { return a.accepted != b.accepted ? a.accepted : a.offset < b.offset; }
        );
        if (trace != nullptr)
        {
            *trace += std::format(
                " ({} owned brush(es) near the pose, {} of a matching prefab entity",
                owned_seen,
                owned.size()
            );
            for (const Owned &entry : owned)
            {
                *trace += std::format(" {}", entry.hit.model);
            }
            *trace += ")";
        }
        if (nearest == owners.end() || !nearest->accepted)
        {
            return std::nullopt;
        }
        const std::uintptr_t owner = nearest->entity;
        std::vector<const PoseHit *> mine;
        for (const Owned &entry : owned)
        {
            if (entry.owner == owner)
            {
                mine.push_back(&entry.hit);
            }
        }
        const auto named =
            std::find_if(mine.begin(), mine.end(), [words](const PoseHit *hit) { return has_word(hit->model, words); });
        // A prefab that spawned one brush spawned the object; of several unnamed ones none is certain.
        const PoseHit *chosen = named != mine.end() ? *named : (mine.size() == 1 ? mine.front() : nullptr);
        if (chosen == nullptr)
        {
            return std::nullopt;
        }
        PrefabBrushHit hit{*chosen, entity_id_of(owner), nearest->offset};
        hit.brush.matches = mine.size();
        return hit;
    }

    std::optional<EntityHit> find_entity_at_pose(
        const game_structures::Vec3f &pose,
        std::string_view entity_class,
        std::string_view prefab_tag,
        float tolerance,
        std::string *trace
    )
    {
        const float half = std::max(POSE_BOX, tolerance + POSE_BOX);
        const game_structures::Aabb box{
            {pose.x - half, pose.y - half, pose.z - half},
            {pose.x + half, pose.y + half, pose.z + half}
        };
        std::vector<std::uintptr_t> nodes;
        if (const OctreeQueryResult result = query_render_nodes_of_types(box, PROXY_TYPES, nodes, MAX_POSE_NODES);
            result != OctreeQueryResult::Ok)
        {
            if (trace != nullptr)
            {
                *trace += std::format(" ({})", query_failure(result));
            }
            return std::nullopt;
        }
        std::optional<EntityHit> best{};
        std::size_t seen = 0;
        for (const std::uintptr_t node : nodes)
        {
            const auto owner =
                DMK::memory::read<std::uintptr_t>(DMK::Address{node + constants::RENDER_PROXY_ENTITY_OFFSET});
            if (!owner || *owner == 0 || !object_is(GameClass::Entity, *owner) ||
                entity_class_name(*owner) != entity_class)
            {
                continue;
            }
            ++seen;
            if (entity_is_hidden(*owner) ||
                (!prefab_tag.empty() && prefab_instance_tag(entity_name(*owner)) != prefab_tag))
            {
                continue;
            }
            const std::optional<game_structures::Vec3f> position = entity_world_position(*owner);
            if (!position.has_value())
            {
                continue;
            }
            const float offset = distance_between(*position, pose);
            if (offset > tolerance || (best.has_value() && offset >= best->offset))
            {
                continue;
            }
            const std::optional<game_structures::Aabb> bounds = entity_world_bounds(*owner);
            best = EntityHit{
                *owner,
                entity_id_of(*owner),
                *position,
                bounds.value_or(game_structures::Aabb{*position, *position}),
                offset
            };
        }
        if (trace != nullptr)
        {
            *trace += std::format(" ({} {} entit{} near the pose)", seen, entity_class, seen == 1 ? "y" : "ies");
        }
        if (best.has_value() && best->id == 0)
        {
            return std::nullopt;
        }
        return best;
    }

    std::optional<PoseHit> find_brush_like(
        std::string_view model,
        const game_structures::Aabb &bounds,
        std::span<const std::uintptr_t> extra_nodes
    )
    {
        if (model.empty())
        {
            return std::nullopt;
        }
        std::vector<std::uintptr_t> nodes;
        if (query_render_nodes_of_types(bounds, BRUSH_TYPES, nodes, MAX_POSE_NODES) != OctreeQueryResult::Ok)
        {
            return std::nullopt;
        }
        add_extra_nodes(nodes, extra_nodes);
        for (const std::uintptr_t node : nodes)
        {
            if (!brush_matches(node, bounds) || node_hidden(node))
            {
                continue;
            }
            if (std::string name = render_node_name(node); same_model(name, model))
            {
                const std::optional<game_structures::Aabb> now = render_node_bounds(node);
                return PoseHit{node, render_node_type(node), now.value_or(bounds), std::move(name), 0.0f, 1};
            }
        }
        return std::nullopt;
    }

    std::string render_node_name(std::uintptr_t node)
    {
        const std::uintptr_t fn = read_vtable_slot(node, constants::RENDERNODE_VTABLE_GET_NAME_OFFSET);
        if (fn == 0)
        {
            return {};
        }
        const char *text = call_get_name(fn, node);
        return text != nullptr ? read_c_string(reinterpret_cast<std::uintptr_t>(text), MAX_NAME_LENGTH) : std::string{};
    }

} // namespace HenrySenses
