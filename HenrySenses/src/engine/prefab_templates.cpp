/**
 * @file engine/prefab_templates.cpp
 * @brief The prefab template index and the placed-trigger match.
 */

#include "engine/prefab_templates.hpp"
#include "constants.hpp"
#include "global_state.hpp"
#include "rtti_types.hpp"

#include <DetourModKit.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace HenrySenses
{
    namespace
    {
        using game_structures::Vec3f;
        using Mat3 = std::array<float, 9>;

        // Caps on every loop over engine data. The largest shipped template holds 671 objects and an object about 20
        // attributes; a count past a cap is corruption, not a big template, and fails the index.
        constexpr std::uint64_t MAX_TEMPLATES = 65536;
        constexpr std::size_t MAX_CHILDREN = 4096;
        constexpr std::size_t MAX_ATTRIBUTES = 256;
        // Entity links kept per trigger, and Link nodes read under one EntityLinks node.
        constexpr std::size_t MAX_LINKS = 8;
        constexpr std::size_t MAX_LINK_NODES = 64;
        // Longest strings read: an attribute key (every key the index wants is shorter), an attribute value, a template
        // name. A longer key is simply not one of them; a longer value is skipped.
        constexpr std::size_t MAX_KEY_LENGTH = 31;
        constexpr std::size_t MAX_VALUE_LENGTH = 511;
        constexpr std::size_t MAX_NAME_LENGTH = 255;
        // String reads never cross a page, so a short string at the end of a mapped page still reads.
        constexpr std::size_t PAGE_SIZE = 4096;
        constexpr std::size_t TEXT_CHUNK = 64;
        // A template matches when its linked object lands this close to the placed one (level instances measure
        // under 1 mm).
        constexpr float MATCH_TOLERANCE = 0.03f;
        // How often a built index checks that the library is still the one it read.
        constexpr std::int64_t RECHECK_MS = 5000;

        constexpr std::array<std::string_view, 8> TRIGGER_CLASSES{
            "BedTrigger",
            "ActionTrigger",
            "WaterTubeActionTrigger",
            "KettleActionTrigger",
            "FoodProcessingTrigger",
            "SequenceTrigger",
            "SmartObjectTrigger",
            "InteractionTrigger",
        };

        // Parts of the entity classes that never show the object a trigger belongs to: triggers, AI helpers, prefab
        // ports and schedule hubs, pose ghosts, areas and shapes, lights, sound, effects, item slots and stashes.
        constexpr std::array<std::string_view, 24> HELPER_CLASS_PARTS{
            "Trigger",  "SmartObject", "Area",    "Shape", "Light",    "Sound",      "Audio",       "Particle",
            "Item",     "Port",        "Hub",     "Ghost", "TagPoint", "Navigation", "Stash",       "Link",
            "Settings", "Decal",       "Comment", "Fog",   "Spawn",    "Camera",     "Environment", "Proximity",
        };

        /**
         * @enum TextRead
         * @brief Outcome of read_text().
         */
        enum class TextRead : std::uint8_t
        {
            Ok,
            /// Readable, but longer than the buffer.
            TooLong,
            /// The address or the memory behind it is not readable.
            Unreadable,
        };

        /**
         * @enum Key
         * @brief The object attributes the index reads.
         */
        enum class Key : std::uint8_t
        {
            None,
            Type,
            EntityClass,
            Name,
            Id,
            Pos,
            Rotate,
            Scale,
            Prefab,
            TargetId,
        };

        /** @brief One template object as read, before the template is kept or dropped. */
        struct RawObject
        {
            std::string type{};
            std::string entity_class{};
            std::string name{};
            std::string id{};
            std::string model{};
            Vec3f position{};
            Quaternionf rotation{};
            Vec3f scale{1.0f, 1.0f, 1.0f};
            std::vector<std::string> link_ids{};
            bool trigger{false};
        };

        /** @brief The library as located this call. */
        struct LibraryView
        {
            std::uintptr_t library{0};
            std::uint64_t count{0};
            std::uintptr_t sentinel{0};
        };

        /** @brief Outcome of locate_library(). */
        enum class LibraryStatus : std::uint8_t
        {
            /// The context, the owner or the library does not exist yet.
            NotYet,
            Ok,
            /// It exists but does not have the expected layout.
            Bad,
        };

        /** @brief The build in progress, and the one that finished. */
        struct Build
        {
            TemplateIndexState state{TemplateIndexState::Idle};
            LibraryView view{};
            std::uintptr_t xml_vtable{0};
            // CXMLReadOnlyNode's vtable, 0 when the class did not resolve (then only CXmlNode trees are read).
            std::uintptr_t read_only_vtable{0};
            std::vector<std::uintptr_t> entries{};
            std::size_t next{0};
            std::size_t objects_read{0};
            std::size_t skipped{0};
            std::int64_t busy_us{0};
            std::int64_t started_ms{0};
            std::uint32_t frames{0};
            std::int64_t next_check_ms{0};
            bool waiting_logged{false};
            // The node whose vtable was neither XML node class's, named by the failure line.
            std::uintptr_t rejected_node{0};
            // The entry being read when the build failed.
            std::uintptr_t current_entry{0};
        };

        // Main-thread state.
        Build s_build;
        std::vector<PrefabTemplate> s_templates;
        // (trigger class, Name) -> the triggers of that class and name in every template.
        std::unordered_map<std::string, std::vector<TemplateTrigger>> s_index;
        std::size_t s_trigger_count = 0;
        // Scratch reused across templates.
        std::vector<RawObject> s_raw;
        std::vector<std::uintptr_t> s_children;
        std::vector<std::uintptr_t> s_attributes;
        std::vector<std::uintptr_t> s_link_nodes;
        std::vector<std::uintptr_t> s_links;
        // Read by prefab_template_state() off the main thread as well.
        std::atomic<TemplateIndexState> s_state{TemplateIndexState::Idle};

        [[nodiscard]] std::int64_t steady_us() noexcept
        {
            return std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now().time_since_epoch()
            )
                .count();
        }

        void set_state(TemplateIndexState state) noexcept
        {
            s_build.state = state;
            s_state.store(state, std::memory_order_relaxed);
        }

        [[nodiscard]] std::uintptr_t read_ptr(std::uintptr_t address) noexcept
        {
            const auto value = DMK::memory::read<std::uintptr_t>(DMK::Address{address});
            return value && DMK::memory::is_plausible_ptr(DMK::Address{*value}) ? *value : 0;
        }

        /**
         * @brief Copies a NUL-terminated engine string into @p out (at most @p capacity characters, then a NUL).
         * @param length Receives the length on success.
         */
        [[nodiscard]] TextRead
        read_text(std::uintptr_t address, char *out, std::size_t capacity, std::size_t &length) noexcept
        {
            length = 0;
            if (!DMK::memory::is_plausible_ptr(DMK::Address{address}))
            {
                return TextRead::Unreadable;
            }
            // Every pass reads at least one byte and at most capacity + 1 are read, so the loop is bounded.
            while (length <= capacity)
            {
                const std::uintptr_t cursor = address + length;
                const std::size_t to_page = PAGE_SIZE - static_cast<std::size_t>(cursor % PAGE_SIZE);
                const std::size_t want = std::min({TEXT_CHUNK, to_page, capacity + 1 - length});
                std::array<char, TEXT_CHUNK> chunk{};
                if (!DMK::memory::read_into(
                        DMK::Address{cursor},
                        std::as_writable_bytes(std::span{chunk.data(), want})
                    ))
                {
                    return TextRead::Unreadable;
                }
                const void *nul = std::memchr(chunk.data(), 0, want);
                if (nul == nullptr)
                {
                    if (length + want > capacity)
                    {
                        return TextRead::TooLong;
                    }
                    std::memcpy(out + length, chunk.data(), want);
                    length += want;
                    continue;
                }
                const auto take = static_cast<std::size_t>(static_cast<const char *>(nul) - chunk.data());
                std::memcpy(out + length, chunk.data(), take);
                length += take;
                out[length] = '\0';
                return TextRead::Ok;
            }
            return TextRead::TooLong;
        }

        [[nodiscard]] Key key_of(std::string_view key) noexcept
        {
            constexpr std::array<std::pair<std::string_view, Key>, 9> keys{{
                {"Type", Key::Type},
                {"EntityClass", Key::EntityClass},
                {"Name", Key::Name},
                {"Id", Key::Id},
                {"Pos", Key::Pos},
                {"Rotate", Key::Rotate},
                {"Scale", Key::Scale},
                {"Prefab", Key::Prefab},
                {"TargetId", Key::TargetId},
            }};
            for (const auto &[name, value] : keys)
            {
                if (name == key)
                {
                    return value;
                }
            }
            return Key::None;
        }

        /** @brief Parses up to @p count comma-separated floats; the ones missing keep their value. */
        void parse_floats(std::string_view text, std::span<float> values) noexcept
        {
            const char *cursor = text.data();
            const char *const end = text.data() + text.size();
            for (float &value : values)
            {
                while (cursor < end && (*cursor == ' ' || *cursor == ','))
                {
                    ++cursor;
                }
                float parsed = 0.0f;
                const auto [next, error] = std::from_chars(cursor, end, parsed);
                if (error != std::errc{} || !std::isfinite(parsed))
                {
                    return;
                }
                value = parsed;
                cursor = next;
            }
        }

        [[nodiscard]] Vec3f parse_vec3(std::string_view text, Vec3f fallback) noexcept
        {
            std::array<float, 3> v{fallback.x, fallback.y, fallback.z};
            parse_floats(text, v);
            return Vec3f{v[0], v[1], v[2]};
        }

        [[nodiscard]] Quaternionf parse_quaternion(std::string_view text) noexcept
        {
            std::array<float, 4> v{1.0f, 0.0f, 0.0f, 0.0f};
            parse_floats(text, v);
            return Quaternionf{v[0], v[1], v[2], v[3]};
        }

        [[nodiscard]] std::string lowercase(std::string_view text)
        {
            std::string lower(text);
            std::transform(
                lower.begin(),
                lower.end(),
                lower.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); }
            );
            return lower;
        }

        /** @brief A model path as the engine names a brush's statobj: lower case, '/' separators. */
        [[nodiscard]] std::string normalized_model(std::string_view path)
        {
            std::string model = lowercase(path);
            std::replace(model.begin(), model.end(), '\\', '/');
            return model;
        }

        [[nodiscard]] std::string index_key(std::string_view entity_class, std::string_view name)
        {
            std::string key;
            key.reserve(entity_class.size() + 1 + name.size());
            key.append(entity_class);
            key.push_back('\x1f');
            key.append(name);
            return key;
        }

        /** @brief The member layout of an XML node. */
        enum class NodeKind : std::uint8_t
        {
            /// Not an XML node the index can read.
            None,
            /// CXmlNode: its lists sit behind pointers to std::vectors.
            Tree,
            /// CXMLReadOnlyNode: its lists are inline arrays with u32 counts.
            ReadOnly,
        };

        /** @brief The list of a node that read_node_list() copies. */
        enum class NodeList : std::uint8_t
        {
            /// The child nodes (8-byte node pointers).
            Children,
            /// The 16-byte { key, value } attributes.
            Attributes,
        };

        /** @brief The layout of @p node, from its vtable; None for anything but the two XML node classes. */
        [[nodiscard]] NodeKind node_kind(std::uintptr_t node) noexcept
        {
            if (node == 0 || !DMK::memory::is_plausible_ptr(DMK::Address{node}))
            {
                return NodeKind::None;
            }
            const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{node});
            if (!vtable.has_value())
            {
                return NodeKind::None;
            }
            if (*vtable == s_build.xml_vtable)
            {
                return NodeKind::Tree;
            }
            if (s_build.read_only_vtable != 0 && *vtable == s_build.read_only_vtable)
            {
                return NodeKind::ReadOnly;
            }
            return NodeKind::None;
        }

        /** @brief True when @p node is an XML node of either layout (checked before every read of a node's members). */
        [[nodiscard]] bool is_xml_node(std::uintptr_t node) noexcept
        {
            if (node_kind(node) != NodeKind::None)
            {
                return true;
            }
            s_build.rejected_node = node;
            return false;
        }

        /**
         * @brief Finds where a CXmlNode keeps one of its lists: the member points at a std::vector { begin, end }.
         * @return False when the vector does not have the expected shape; true with @p count 0 when there is none.
         */
        [[nodiscard]] bool tree_node_list(
            std::uintptr_t node,
            std::ptrdiff_t member,
            std::size_t element_size,
            std::size_t cap,
            std::uintptr_t &begin,
            std::size_t &count
        ) noexcept
        {
            const auto vector = DMK::memory::read<std::uintptr_t>(DMK::Address{node + member});
            if (!vector)
            {
                return false;
            }
            if (*vector == 0)
            {
                return true;
            }
            if (!DMK::memory::is_plausible_ptr(DMK::Address{*vector}))
            {
                return false;
            }
            std::array<std::uintptr_t, 2> range{};
            if (!DMK::memory::read_into(DMK::Address{*vector}, std::as_writable_bytes(std::span{range})))
            {
                return false;
            }
            const auto [first, end] = range;
            if (first == end)
            {
                return true;
            }
            if (end < first || !DMK::memory::is_plausible_ptr(DMK::Address{first}))
            {
                return false;
            }
            const std::uintptr_t bytes = end - first;
            if (bytes % element_size != 0 || bytes / element_size > cap)
            {
                return false;
            }
            begin = first;
            count = static_cast<std::size_t>(bytes / element_size);
            return true;
        }

        /**
         * @brief Finds where a CXMLReadOnlyNode keeps one of its lists: an inline array pointer with a u32 count.
         * @return False when the list does not have the expected shape; true with @p count 0 when there is none.
         */
        [[nodiscard]] bool read_only_node_list(
            std::uintptr_t node,
            std::ptrdiff_t array_member,
            std::ptrdiff_t count_member,
            std::size_t cap,
            std::uintptr_t &begin,
            std::size_t &count
        ) noexcept
        {
            const auto array = DMK::memory::read<std::uintptr_t>(DMK::Address{node + array_member});
            const auto elements = DMK::memory::read<std::uint32_t>(DMK::Address{node + count_member});
            if (!array || !elements)
            {
                return false;
            }
            if (*elements == 0)
            {
                return true;
            }
            // A count past the cap is corruption, not a big template (the caps bound every loop of the build).
            if (*elements > cap || !DMK::memory::is_plausible_ptr(DMK::Address{*array}))
            {
                return false;
            }
            begin = *array;
            count = *elements;
            return true;
        }

        /**
         * @brief Copies the elements of a checked node's children or attributes as qwords.
         * @param node A checked XML node of either layout.
         * @param list The list to copy.
         * @param cap Most elements accepted.
         * @param out Receives the elements' qwords (two per attribute), cleared first.
         * @return False when the list does not have the expected shape; true with @p out empty when there is none.
         */
        [[nodiscard]] bool
        read_node_list(std::uintptr_t node, NodeList list, std::size_t cap, std::vector<std::uintptr_t> &out)
        {
            out.clear();
            const bool children = list == NodeList::Children;
            const std::size_t element_size = children ? constants::XML_CHILD_SIZE : constants::XML_ATTRIBUTE_SIZE;
            std::uintptr_t begin = 0;
            std::size_t count = 0;
            bool shaped = false;
            switch (node_kind(node))
            {
            case NodeKind::Tree:
                shaped = tree_node_list(
                    node,
                    children ? constants::XML_NODE_CHILDREN_OFFSET : constants::XML_NODE_ATTRIBUTES_OFFSET,
                    element_size,
                    cap,
                    begin,
                    count
                );
                break;
            case NodeKind::ReadOnly:
                shaped = read_only_node_list(
                    node,
                    children ? constants::XML_READ_ONLY_NODE_CHILDREN_OFFSET
                             : constants::XML_READ_ONLY_NODE_ATTRIBUTES_OFFSET,
                    children ? constants::XML_READ_ONLY_NODE_CHILD_COUNT_OFFSET
                             : constants::XML_READ_ONLY_NODE_ATTRIBUTE_COUNT_OFFSET,
                    cap,
                    begin,
                    count
                );
                break;
            case NodeKind::None:
            default:
                return false;
            }
            if (!shaped)
            {
                return false;
            }
            if (count == 0)
            {
                return true;
            }
            out.resize(count * element_size / sizeof(std::uintptr_t));
            return DMK::memory::read_into(DMK::Address{begin}, std::as_writable_bytes(std::span{out})).has_value();
        }

        /** @brief Reads a checked node's tag into @p out; false when unreadable. */
        [[nodiscard]] bool node_tag(std::uintptr_t node, std::array<char, MAX_KEY_LENGTH + 1> &out, std::size_t &length)
        {
            const auto tag = DMK::memory::read<std::uintptr_t>(DMK::Address{node + constants::XML_NODE_TAG_OFFSET});
            if (!tag)
            {
                return false;
            }
            const TextRead read = read_text(*tag, out.data(), MAX_KEY_LENGTH, length);
            if (read == TextRead::TooLong)
            {
                length = 0;
            }
            return read != TextRead::Unreadable;
        }

        /**
         * @brief Reads the attributes the index wants of one checked node into @p object (TargetId goes to its links).
         * @return Null on success, else what was wrong.
         */
        [[nodiscard]] const char *read_attributes(std::uintptr_t node, RawObject &object)
        {
            if (!read_node_list(node, NodeList::Attributes, MAX_ATTRIBUTES, s_attributes))
            {
                return "an attribute list is out of shape";
            }
            std::array<char, MAX_KEY_LENGTH + 1> key{};
            std::array<char, MAX_VALUE_LENGTH + 1> value{};
            for (std::size_t i = 0; i + 1 < s_attributes.size(); i += 2)
            {
                std::size_t key_length = 0;
                const TextRead key_read = read_text(s_attributes[i], key.data(), MAX_KEY_LENGTH, key_length);
                if (key_read == TextRead::Unreadable)
                {
                    return "an attribute key is unreadable";
                }
                const Key which =
                    key_read == TextRead::Ok ? key_of(std::string_view{key.data(), key_length}) : Key::None;
                if (which == Key::None)
                {
                    continue;
                }
                std::size_t value_length = 0;
                const TextRead value_read =
                    read_text(s_attributes[i + 1], value.data(), MAX_VALUE_LENGTH, value_length);
                if (value_read == TextRead::Unreadable)
                {
                    return "an attribute value is unreadable";
                }
                if (value_read == TextRead::TooLong)
                {
                    continue;
                }
                const std::string_view text{value.data(), value_length};
                switch (which)
                {
                case Key::Type:
                    object.type.assign(text);
                    break;
                case Key::EntityClass:
                    object.entity_class.assign(text);
                    break;
                case Key::Name:
                    object.name.assign(text);
                    break;
                case Key::Id:
                    object.id.assign(text);
                    break;
                case Key::Pos:
                    object.position = parse_vec3(text, Vec3f{0.0f, 0.0f, 0.0f});
                    break;
                case Key::Rotate:
                    object.rotation = parse_quaternion(text);
                    break;
                case Key::Scale:
                    object.scale = parse_vec3(text, Vec3f{1.0f, 1.0f, 1.0f});
                    break;
                case Key::Prefab:
                    object.model = normalized_model(text);
                    break;
                case Key::TargetId:
                    if (object.link_ids.size() < MAX_LINKS)
                    {
                        object.link_ids.emplace_back(text);
                    }
                    break;
                case Key::None:
                default:
                    break;
                }
            }
            return nullptr;
        }

        /**
         * @brief Reads the EntityLinks/Link TargetId values of a checked trigger object node.
         * @return Null on success, else what was wrong.
         */
        [[nodiscard]] const char *read_links(std::uintptr_t node, RawObject &object)
        {
            if (!read_node_list(node, NodeList::Children, MAX_CHILDREN, s_link_nodes))
            {
                return "an object's child list is out of shape";
            }
            std::array<char, MAX_KEY_LENGTH + 1> tag{};
            // The object's children are its Properties and EntityLinks nodes (a handful). The loops below read into
            // s_links and s_attributes, never into s_link_nodes.
            for (const std::uintptr_t child : s_link_nodes)
            {
                std::size_t length = 0;
                if (!is_xml_node(child) || !node_tag(child, tag, length))
                {
                    return "an object's child is not a readable XML node";
                }
                if (std::string_view{tag.data(), length} != "EntityLinks")
                {
                    continue;
                }
                if (!read_node_list(child, NodeList::Children, MAX_LINK_NODES, s_links))
                {
                    return "an EntityLinks list is out of shape";
                }
                for (const std::uintptr_t link : s_links)
                {
                    if (!is_xml_node(link))
                    {
                        return "a Link node is not an XML node";
                    }
                    // Only TargetId is wanted from a Link; its Name and DebugTargetName are ignored.
                    RawObject target{};
                    if (const char *failure = read_attributes(link, target); failure != nullptr)
                    {
                        return failure;
                    }
                    for (std::string &id : target.link_ids)
                    {
                        if (object.link_ids.size() < MAX_LINKS)
                        {
                            object.link_ids.push_back(std::move(id));
                        }
                    }
                }
            }
            return nullptr;
        }

        /** @brief True when a template object of this Type and class shows a mesh of its own. */
        [[nodiscard]] bool shows_mesh(std::string_view type, std::string_view entity_class) noexcept
        {
            if (type == "GeomEntity")
            {
                return true;
            }
            if (type != "Entity" || entity_class.empty())
            {
                return false;
            }
            return std::none_of(
                HELPER_CLASS_PARTS.begin(),
                HELPER_CLASS_PARTS.end(),
                [entity_class](std::string_view part) { return entity_class.find(part) != std::string_view::npos; }
            );
        }

        /** @brief Keeps one parsed template (it places at least one trigger) and indexes its triggers. */
        void keep_template(std::string name)
        {
            const std::size_t index = s_templates.size();
            PrefabTemplate kept{std::move(name), {}};
            for (const RawObject &raw : s_raw)
            {
                TemplateObjectKind kind{};
                if (raw.type == "Brush" && !raw.model.empty())
                {
                    kind = TemplateObjectKind::Brush;
                }
                else if (raw.type == "Entity" && raw.entity_class == "RuntimePrefab")
                {
                    kind = TemplateObjectKind::RuntimePrefab;
                }
                else if (shows_mesh(raw.type, raw.entity_class))
                {
                    kind = TemplateObjectKind::MeshEntity;
                }
                else
                {
                    continue;
                }
                kept.objects.push_back(
                    TemplateObject{
                        kind,
                        raw.name,
                        raw.type == "GeomEntity" && raw.entity_class.empty() ? std::string{"GeomEntity"}
                                                                             : raw.entity_class,
                        raw.model,
                        raw.position,
                        raw.rotation,
                        raw.scale
                    }
                );
            }
            for (const RawObject &raw : s_raw)
            {
                if (!raw.trigger)
                {
                    continue;
                }
                TemplateTrigger trigger{index, raw.position, raw.rotation, raw.scale, {}};
                for (const std::string &id : raw.link_ids)
                {
                    const auto target = std::find_if(
                        s_raw.begin(),
                        s_raw.end(),
                        [&id](const RawObject &other) { return !other.id.empty() && other.id == id; }
                    );
                    if (target != s_raw.end())
                    {
                        trigger.link_positions.push_back(target->position);
                    }
                }
                s_index[index_key(raw.entity_class, raw.name)].push_back(std::move(trigger));
                ++s_trigger_count;
            }
            s_templates.push_back(std::move(kept));
        }

        /**
         * @brief Reads one library entry and keeps it when it places a trigger.
         * @return Null on success (kept or not), else what was wrong with the library.
         */
        [[nodiscard]] const char *read_template(std::uintptr_t entry)
        {
            std::array<char, MAX_NAME_LENGTH + 1> name{};
            std::size_t name_length = 0;
            const TextRead name_read = read_text(
                read_ptr(entry + constants::PREFAB_ENTRY_NAME_OFFSET),
                name.data(),
                MAX_NAME_LENGTH,
                name_length
            );
            if (name_read == TextRead::Unreadable)
            {
                return "a template name is unreadable";
            }
            if (name_read == TextRead::TooLong)
            {
                ++s_build.skipped;
                return nullptr;
            }
            const auto objects =
                DMK::memory::read<std::uintptr_t>(DMK::Address{entry + constants::PREFAB_ENTRY_OBJECTS_OFFSET});
            if (!objects)
            {
                return "a template entry is unreadable";
            }
            if (*objects == 0)
            {
                // A template without an <Objects> node places nothing.
                return nullptr;
            }
            if (!is_xml_node(*objects))
            {
                return "an <Objects> node is not an XML node";
            }
            std::array<char, MAX_KEY_LENGTH + 1> tag{};
            std::size_t tag_length = 0;
            if (!node_tag(*objects, tag, tag_length) || std::string_view{tag.data(), tag_length} != "Objects")
            {
                return "a template's objects node is not named Objects";
            }
            if (!read_node_list(*objects, NodeList::Children, MAX_CHILDREN, s_children))
            {
                return "an <Objects> child list is out of shape";
            }
            s_raw.clear();
            bool any_trigger = false;
            for (const std::uintptr_t child : s_children)
            {
                if (!is_xml_node(child))
                {
                    return "an <Object> node is not an XML node";
                }
                RawObject &object = s_raw.emplace_back();
                if (const char *failure = read_attributes(child, object); failure != nullptr)
                {
                    return failure;
                }
                // TargetId belongs to Link nodes; one on an object itself is not a link.
                object.link_ids.clear();
                object.trigger = object.type == "Entity" && template_trigger_class(object.entity_class);
                if (object.trigger)
                {
                    any_trigger = true;
                    if (const char *failure = read_links(child, object); failure != nullptr)
                    {
                        return failure;
                    }
                }
            }
            s_build.objects_read += s_children.size();
            if (any_trigger)
            {
                keep_template(std::string{name.data(), name_length});
            }
            return nullptr;
        }

        /**
         * @brief Finds the library through the global context and checks its header.
         * @param reason Receives what was wrong for LibraryStatus::Bad.
         */
        [[nodiscard]] LibraryStatus locate_library(LibraryView &view, const char *&reason) noexcept
        {
            std::byte *const slot = g_global_context_ptr_address.load(std::memory_order_relaxed);
            const std::uintptr_t context = slot != nullptr ? read_ptr(reinterpret_cast<std::uintptr_t>(slot)) : 0;
            const std::uintptr_t owner = context != 0 ? read_ptr(context + constants::CONTEXT_PREFAB_OWNER_OFFSET) : 0;
            const std::uintptr_t library = owner != 0 ? read_ptr(owner + constants::PREFAB_OWNER_LIBRARY_OFFSET) : 0;
            if (library == 0)
            {
                return LibraryStatus::NotYet;
            }
            const auto load_factor =
                DMK::memory::read<float>(DMK::Address{library + constants::PREFAB_LIBRARY_LOAD_FACTOR_OFFSET});
            const auto count =
                DMK::memory::read<std::uint64_t>(DMK::Address{library + constants::PREFAB_LIBRARY_COUNT_OFFSET});
            const std::uintptr_t sentinel = read_ptr(library + constants::PREFAB_LIBRARY_LIST_OFFSET);
            // The map is built with a max load factor of exactly 1.0 and never changes it.
            if (!load_factor || *load_factor != 1.0f)
            {
                reason = "the library's load factor is not 1.0";
                return LibraryStatus::Bad;
            }
            if (!count || *count == 0 || *count > MAX_TEMPLATES)
            {
                reason = "the library's template count is out of range";
                return LibraryStatus::Bad;
            }
            if (sentinel == 0)
            {
                reason = "the library has no list sentinel";
                return LibraryStatus::Bad;
            }
            view = LibraryView{library, *count, sentinel};
            return LibraryStatus::Ok;
        }

        /**
         * @brief Copies the entry pointers of the library's list, which must close within its count.
         * @return Null on success, else what was wrong.
         */
        [[nodiscard]] const char *collect_entries()
        {
            s_build.entries.clear();
            s_build.entries.reserve(static_cast<std::size_t>(s_build.view.count));
            std::uintptr_t node = read_ptr(s_build.view.sentinel + constants::PREFAB_NODE_NEXT_OFFSET);
            while (node != s_build.view.sentinel)
            {
                if (node == 0 || s_build.entries.size() >= s_build.view.count)
                {
                    return "the template list does not close within its count";
                }
                const std::uintptr_t entry = read_ptr(node + constants::PREFAB_NODE_ENTRY_OFFSET);
                if (entry == 0)
                {
                    return "a template list node has no entry";
                }
                s_build.entries.push_back(entry);
                node = read_ptr(node + constants::PREFAB_NODE_NEXT_OFFSET);
            }
            if (s_build.entries.size() != s_build.view.count)
            {
                return "the template list is shorter than its count";
            }
            return nullptr;
        }

        void clear_index() noexcept
        {
            s_templates.clear();
            s_index.clear();
            s_trigger_count = 0;
            s_raw.clear();
        }

        void fail_build(const char *reason)
        {
            clear_index();
            s_build.entries.clear();
            set_state(TemplateIndexState::Failed);
            // The offending node's vtable, as an image offset, tells a binary XML node or another layout apart.
            std::string detail;
            if (s_build.rejected_node != 0)
            {
                const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{s_build.rejected_node});
                const std::uintptr_t base = module_info().base;
                detail = std::format(
                    " (node 0x{:X}, vtable 0x{:X}{})",
                    s_build.rejected_node,
                    vtable.value_or(0),
                    vtable && base != 0 && *vtable > base ? std::format(" = image +0x{:X}", *vtable - base)
                                                          : std::string{}
                );
            }
            (void)DMK::log().try_log(
                DMK::LogLevel::Info,
                "PrefabTemplates: {}{} in template entry 0x{:X} ({} of {} read); the template index is off and Places "
                "keep the mesh search only",
                reason,
                detail,
                s_build.current_entry,
                s_build.next,
                s_build.view.count
            );
        }

        [[nodiscard]] bool start_build(std::int64_t now_us)
        {
            const char *reason = nullptr;
            LibraryView view{};
            const LibraryStatus status = locate_library(view, reason);
            if (status == LibraryStatus::NotYet)
            {
                if (!s_build.waiting_logged)
                {
                    s_build.waiting_logged = true;
                    (void)DMK::log().try_log(DMK::LogLevel::Debug, "PrefabTemplates: waiting for the template library");
                }
                return false;
            }
            if (status == LibraryStatus::Bad)
            {
                fail_build(reason);
                return false;
            }
            const std::uintptr_t xml_vtable = class_vtable(GameClass::XmlNode);
            // The library keeps its templates as compact read-only nodes; CXmlNode trees are read as well.
            const std::uintptr_t read_only_vtable = class_vtable(GameClass::XmlReadOnlyNode);
            if (xml_vtable == 0 && read_only_vtable == 0)
            {
                fail_build("neither XML node class resolved");
                return false;
            }
            clear_index();
            s_build.view = view;
            s_build.xml_vtable = xml_vtable;
            s_build.read_only_vtable = read_only_vtable;
            s_build.next = 0;
            s_build.objects_read = 0;
            s_build.skipped = 0;
            s_build.busy_us = 0;
            s_build.frames = 0;
            s_build.started_ms = now_us / 1000;
            s_build.rejected_node = 0;
            s_build.current_entry = 0;
            if (const char *failure = collect_entries(); failure != nullptr)
            {
                fail_build(failure);
                return false;
            }
            set_state(TemplateIndexState::Building);
            return true;
        }

        void finish_build(std::int64_t now_us)
        {
            s_build.entries.clear();
            s_build.entries.shrink_to_fit();
            s_raw.clear();
            s_build.next_check_ms = now_us / 1000 + RECHECK_MS;
            set_state(TemplateIndexState::Ready);
            (void)DMK::log().try_log(
                DMK::LogLevel::Info,
                "PrefabTemplates: indexed {} of {} templates ({} trigger objects under {} names, {} objects read, {} "
                "skipped) in {:.2f} ms over {} frame(s)",
                s_templates.size(),
                s_build.view.count,
                s_trigger_count,
                s_index.size(),
                s_build.objects_read,
                s_build.skipped,
                static_cast<double>(s_build.busy_us) / 1000.0,
                s_build.frames
            );
        }

        [[nodiscard]] Mat3 quaternion_matrix(const Quaternionf &q) noexcept
        {
            const float norm = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
            if (!(norm > 1e-6f) || !std::isfinite(norm))
            {
                return Mat3{1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
            }
            const float w = q.w / norm;
            const float x = q.x / norm;
            const float y = q.y / norm;
            const float z = q.z / norm;
            return Mat3{
                1.0f - 2.0f * (y * y + z * z),
                2.0f * (x * y - w * z),
                2.0f * (x * z + w * y),
                2.0f * (x * y + w * z),
                1.0f - 2.0f * (x * x + z * z),
                2.0f * (y * z - w * x),
                2.0f * (x * z - w * y),
                2.0f * (y * z + w * x),
                1.0f - 2.0f * (x * x + y * y),
            };
        }

        /** @brief a * transpose(b): the rotation that takes b's frame to a's. */
        [[nodiscard]] Mat3 multiply_transposed(const Mat3 &a, const Mat3 &b) noexcept
        {
            Mat3 out{};
            for (int r = 0; r < 3; ++r)
            {
                for (int c = 0; c < 3; ++c)
                {
                    out[r * 3 + c] = a[r * 3] * b[c * 3] + a[r * 3 + 1] * b[c * 3 + 1] + a[r * 3 + 2] * b[c * 3 + 2];
                }
            }
            return out;
        }

        [[nodiscard]] Vec3f apply(const Mat3 &m, const Vec3f &v) noexcept
        {
            return Vec3f{
                m[0] * v.x + m[1] * v.y + m[2] * v.z,
                m[3] * v.x + m[4] * v.y + m[5] * v.z,
                m[6] * v.x + m[7] * v.y + m[8] * v.z,
            };
        }

        [[nodiscard]] float distance_between(const Vec3f &a, const Vec3f &b) noexcept
        {
            const float dx = a.x - b.x;
            const float dy = a.y - b.y;
            const float dz = a.z - b.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        [[nodiscard]] bool contains_word(std::string_view text, std::string_view word)
        {
            return !word.empty() && lowercase(text).find(word) != std::string::npos;
        }

        /** @brief The file name of a model path (everything after its last '/'). */
        [[nodiscard]] std::string_view file_name(std::string_view path) noexcept
        {
            const std::size_t slash = path.find_last_of('/');
            return slash == std::string_view::npos ? path : path.substr(slash + 1);
        }
    } // namespace

    void advance_prefab_templates(std::int64_t budget_us)
    {
        DMK_PROFILE_FUNCTION();
        const std::int64_t start_us = steady_us();
        switch (s_build.state)
        {
        case TemplateIndexState::Failed:
            return;
        case TemplateIndexState::Ready:
        {
            if (start_us / 1000 < s_build.next_check_ms)
            {
                return;
            }
            s_build.next_check_ms = start_us / 1000 + RECHECK_MS;
            // The library is loaded once at startup; a different pointer or count means it was rebuilt and the index
            // read memory that is gone.
            const char *reason = nullptr;
            LibraryView view{};
            if (locate_library(view, reason) != LibraryStatus::Ok ||
                (view.library == s_build.view.library && view.count == s_build.view.count))
            {
                return;
            }
            (void)DMK::log().try_log(
                DMK::LogLevel::Info,
                "PrefabTemplates: the template library changed; indexing it again"
            );
            clear_index();
            set_state(TemplateIndexState::Idle);
            return;
        }
        case TemplateIndexState::Idle:
            if (!start_build(start_us))
            {
                return;
            }
            break;
        case TemplateIndexState::Building:
        default:
            break;
        }
        ++s_build.frames;
        try
        {
            // One template at a time: the largest reads about ten thousand small values, a fraction of the budget.
            while (s_build.next < s_build.entries.size())
            {
                s_build.current_entry = s_build.entries[s_build.next];
                if (const char *failure = read_template(s_build.current_entry); failure != nullptr)
                {
                    s_build.busy_us += steady_us() - start_us;
                    fail_build(failure);
                    return;
                }
                ++s_build.next;
                if (steady_us() - start_us >= budget_us)
                {
                    break;
                }
            }
        }
        catch (const std::bad_alloc &)
        {
            fail_build("out of memory");
            return;
        }
        const std::int64_t end_us = steady_us();
        s_build.busy_us += end_us - start_us;
        if (s_build.next >= s_build.entries.size())
        {
            finish_build(end_us);
        }
    }

    TemplateIndexState prefab_template_state() noexcept
    {
        return s_state.load(std::memory_order_relaxed);
    }

    void reset_prefab_templates() noexcept
    {
        clear_index();
        s_build = Build{};
        s_state.store(TemplateIndexState::Idle, std::memory_order_relaxed);
    }

    bool template_trigger_class(std::string_view entity_class) noexcept
    {
        return std::find(TRIGGER_CLASSES.begin(), TRIGGER_CLASSES.end(), entity_class) != TRIGGER_CLASSES.end();
    }

    TemplateMatchResult match_prefab_template(
        std::string_view entity_class,
        std::string_view entity_name,
        const game_structures::Matrix34f &world,
        std::span<const game_structures::Vec3f> linked
    )
    {
        TemplateMatchResult result{};
        if (s_build.state != TemplateIndexState::Ready)
        {
            return result;
        }
        const std::string_view base = entity_name.substr(0, entity_name.find('['));
        const auto it = s_index.find(index_key(entity_class, base));
        if (it == s_index.end())
        {
            result.status = TemplateMatchStatus::NoTemplate;
            return result;
        }
        result.status = TemplateMatchStatus::NoLink;
        if (linked.empty())
        {
            return result;
        }
        // The world matrix is rotation times per-axis scale: each column's length is its axis scale.
        Mat3 rotation{};
        std::array<float, 3> scale{};
        for (int c = 0; c < 3; ++c)
        {
            const float x = world.m[0][c];
            const float y = world.m[1][c];
            const float z = world.m[2][c];
            const float length = std::sqrt(x * x + y * y + z * z);
            if (!(length > 1e-6f) || !std::isfinite(length))
            {
                return result;
            }
            scale[c] = length;
            rotation[c] = x / length;
            rotation[3 + c] = y / length;
            rotation[6 + c] = z / length;
        }
        const Vec3f translation{world.m[0][3], world.m[1][3], world.m[2][3]};
        float best = std::numeric_limits<float>::max();
        for (const TemplateTrigger &trigger : it->second)
        {
            if (trigger.link_positions.empty() || trigger.template_index >= s_templates.size())
            {
                continue;
            }
            ++result.candidates;
            // Instance = world * trigger_local^-1, the per-axis scale taken apart from the rotation.
            TemplateMatch match{};
            match.prefab = &s_templates[trigger.template_index];
            match.trigger = &trigger;
            match.rotation = multiply_transposed(rotation, quaternion_matrix(trigger.rotation));
            const std::array<float, 3> local_scale{trigger.scale.x, trigger.scale.y, trigger.scale.z};
            std::array<float, 3> instance_scale{};
            for (int axis = 0; axis < 3; ++axis)
            {
                instance_scale[axis] = std::abs(local_scale[axis]) > 1e-6f ? scale[axis] / local_scale[axis] : 1.0f;
            }
            match.scale = Vec3f{instance_scale[0], instance_scale[1], instance_scale[2]};
            const Vec3f offset = apply(
                match.rotation,
                Vec3f{
                    match.scale.x * trigger.position.x,
                    match.scale.y * trigger.position.y,
                    match.scale.z * trigger.position.z
                }
            );
            match.position = Vec3f{translation.x - offset.x, translation.y - offset.y, translation.z - offset.z};
            float error = std::numeric_limits<float>::max();
            for (const Vec3f &local : trigger.link_positions)
            {
                const Vec3f predicted = template_to_world(match, local);
                for (const Vec3f &placed : linked)
                {
                    error = std::min(error, distance_between(predicted, placed));
                }
            }
            match.error = error;
            if (error < best)
            {
                best = error;
                result.match = match;
            }
        }
        if (result.candidates == 0)
        {
            return result;
        }
        result.status = best <= MATCH_TOLERANCE ? TemplateMatchStatus::Matched : TemplateMatchStatus::Mismatch;
        return result;
    }

    game_structures::Vec3f template_to_world(const TemplateMatch &match, const game_structures::Vec3f &local) noexcept
    {
        const Vec3f turned =
            apply(match.rotation, Vec3f{match.scale.x * local.x, match.scale.y * local.y, match.scale.z * local.z});
        return Vec3f{match.position.x + turned.x, match.position.y + turned.y, match.position.z + turned.z};
    }

    const TemplateObject *
    pick_template_object(const TemplateMatch &match, std::span<const std::string> own_words, std::string_view &rule)
    {
        rule = "none";
        if (match.prefab == nullptr || match.trigger == nullptr)
        {
            return nullptr;
        }
        const std::vector<TemplateObject> &objects = match.prefab->objects;
        const TemplateObject *single = nullptr;
        std::size_t brushes = 0;
        std::size_t prefabs = 0;
        for (const TemplateObject &object : objects)
        {
            if (object.kind == TemplateObjectKind::Brush)
            {
                ++brushes;
                single = &object;
            }
            else if (object.kind == TemplateObjectKind::RuntimePrefab)
            {
                ++prefabs;
            }
        }
        if (brushes == 1 && prefabs == 0)
        {
            rule = "single brush";
            return single;
        }
        // The trigger's own name names its object ("trigger_pan" -> "pan_d1").
        if (!own_words.empty())
        {
            const std::string &word = own_words.back();
            for (const TemplateObject &object : objects)
            {
                if (object.kind != TemplateObjectKind::MeshEntity &&
                    (contains_word(object.name, word) || contains_word(file_name(object.model), word)))
                {
                    rule = "own word";
                    return &object;
                }
            }
        }
        const TemplateObject *nearest = nullptr;
        float nearest_distance = std::numeric_limits<float>::max();
        for (const TemplateObject &object : objects)
        {
            const float distance = distance_between(object.position, match.trigger->position);
            if (distance < nearest_distance)
            {
                nearest_distance = distance;
                nearest = &object;
            }
        }
        if (nearest != nullptr)
        {
            rule = "nearest";
        }
        return nearest;
    }

} // namespace HenrySenses
