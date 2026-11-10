/**
 * @file tool/sandbox_layer.cpp
 * @brief The Sandbox layer conversion (see sandbox_layer.hpp).
 *
 * Each rule below matches what the game's own level export wrote for its cinematic layers (the Sandbox `.lyr` of a
 * vanilla cinematic against the shipped `layers/<name>_<guid>.xml`).
 */

#include "tool/sandbox_layer.hpp"
#include "core/text_util.hpp"
#include "core/xml_elements.hpp"
#include "core/zip_reader.hpp"
#include "tool/cutscene_files.hpp"
#include "tool/offline_tools.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <deque>
#include <format>
#include <optional>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace AnimationFramework
{
    namespace
    {
        using namespace cutscene_files;

        /// Editor-only object attributes the export never writes.
        const std::unordered_set<std::string_view> DROP_ATTRS{
            "Id",
            "Type",
            "Prefab",
            "PrefabTemplate",
            "PrefabTemplateName",
            "AttachmentTarget",
            "AttachmentType",
            "DebugParentName",
            "Parent",
            "LockedInPrefab",
            "LockTransformation",
            "CastShadowMinspec",
            "FOV",
            "FarZ",
            "NearZ",
            "Height",
            "ColorRGB",
            "PrefabMember",
            "SourceGeometry",
            "DisplayFilled",
            "Flags",
        };
        /// Child elements the editor keeps and the export does not.
        const std::unordered_set<std::string_view> DROP_CHILDREN{"ShakeParams", "SoundData", "DofParams"};
        /// Classes whose Properties carry no bSaved_by_game.
        const std::unordered_set<std::string_view> NO_SAVED_FLAG{
            "ItemSlot",
            "PlayerWeapon",
            "AnimDoor",
            "GeomCache",
            "AnimObject",
            "Birds",
            "Chickens",
        };
        /// Classes the export writes without Properties.
        const std::unordered_set<std::string_view> NO_PROPERTIES{"SequenceObject", "SchedulerHub", "ClipVolume"};
        /// Classes the export writes without child elements.
        const std::unordered_set<std::string_view> NO_CHILDREN{"SchedulerHub"};
        const std::unordered_set<std::string_view> LIGHT_CLASSES{"Light", "ReflexLight"};
        const std::unordered_set<std::string_view> AREA_CLASSES{"SequenceArea"};
        const std::unordered_set<std::string_view> BOID_CLASSES{"Birds", "Chickens"};
        /// Children the export writes before Properties, in this order.
        constexpr std::array<std::string_view, 2> BEFORE_PROPERTIES{"EntityLinks", "ClipVolume"};
        /// A camera's field of view when the editor object has no FOV: 60 degrees in radians.
        constexpr std::string_view DEFAULT_FOV = "1.047198";
        /// The link name of the SequenceArea whose NPCs a scene hides ("mizeni": disappearing).
        constexpr std::string_view CLEANUP_LINK = "mizeni";
        constexpr std::string_view PREFAB_PAK = "IPL_GameData.pak";
        constexpr std::string_view PREFAB_FOLDER = "prefabs/";
        /// The deepest prefab nesting expanded, so a prefab that holds itself cannot recurse without end.
        constexpr int MAX_PREFAB_DEPTH = 16;

        using Vec3 = std::array<double, 3>;
        /// A rotation quaternion in the editor's order: w, x, y, z.
        using Quat = std::array<double, 4>;
        constexpr Vec3 ORIGIN{0.0, 0.0, 0.0};
        constexpr Quat IDENTITY{1.0, 0.0, 0.0, 0.0};

        [[nodiscard]] Quat mul(const Quat &a, const Quat &b) noexcept
        {
            return {
                a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3],
                a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2],
                a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1],
                a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0],
            };
        }

        [[nodiscard]] Vec3 rotate(const Quat &q, const Vec3 &v) noexcept
        {
            const Quat p = mul(mul(q, {0.0, v[0], v[1], v[2]}), {q[0], -q[1], -q[2], -q[3]});
            return {p[1], p[2], p[3]};
        }

        /** @brief The comma-separated numbers of @p text, or nothing when it has another count or a non-number. */
        template <std::size_t N> [[nodiscard]] std::optional<std::array<double, N>> numbers(std::string_view text)
        {
            std::array<double, N> out{};
            for (std::size_t i = 0; i < N; ++i)
            {
                const std::size_t comma = text.find(',');
                // The last number ends the text, every other one a comma.
                if ((comma == std::string_view::npos) != (i + 1 == N))
                {
                    return std::nullopt;
                }
                const std::string_view part = text.substr(0, comma);
                const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), out[i]);
                if (error != std::errc{} || end != part.data() + part.size())
                {
                    return std::nullopt;
                }
                text.remove_prefix(comma == std::string_view::npos ? text.size() : comma + 1);
            }
            return out;
        }

        template <std::size_t N>
        [[nodiscard]] std::array<double, N>
        numbers_or(const XmlElement &element, std::string_view key, const std::array<double, N> &fallback)
        {
            const std::string *text = element.attr(key);
            return text != nullptr && !text->empty() ? numbers<N>(*text).value_or(fallback) : fallback;
        }

        /** @brief @p values as the export writes them: up to 7 significant digits each. */
        template <std::size_t N> [[nodiscard]] std::string format_numbers(const std::array<double, N> &values)
        {
            std::string out;
            for (std::size_t i = 0; i < N; ++i)
            {
                out += std::format("{}{:.7g}", i == 0 ? "" : ",", values[i]);
            }
            return out;
        }

        /** @brief Whether @p text is a scale of 1 on every axis. */
        [[nodiscard]] bool is_unit_scale(std::string_view text)
        {
            const std::optional<Vec3> scale = numbers<3>(text);
            return scale && std::ranges::all_of(*scale, [](double v) { return std::abs(v - 1.0) < 1e-3; });
        }

        /** @brief Whether @p text is the identity rotation (q or -q, within the export's rounding). */
        [[nodiscard]] bool is_identity_rotation(std::string_view text)
        {
            const std::optional<Quat> q = numbers<4>(text);
            if (!q)
            {
                return false;
            }
            const double sign = (*q)[0] < 0.0 ? -1.0 : 1.0;
            return std::abs(sign * (*q)[0] - 1.0) < 5e-4 && std::abs((*q)[1]) < 5e-4 && std::abs((*q)[2]) < 5e-4 &&
                   std::abs((*q)[3]) < 5e-4;
        }

        /** @brief The runtime EntityGuid of an editor object: the first 64 bits of its 128-bit GUID. */
        [[nodiscard]] std::string guid64(std::string_view guid)
        {
            return std::string{guid.size() >= 18 ? guid.substr(0, 18) : guid};
        }

        /**
         * @brief The GUID the editor gives a prefab member that its instance's ObjectGUIDs do not list: the template
         *        object's GUID XOR the instance's, digit by digit. Nothing when either is not a GUID's text.
         */
        [[nodiscard]] std::optional<std::string> prefab_member_guid(std::string_view member, std::string_view instance)
        {
            constexpr std::size_t guid_length = 36;
            if (member.size() != guid_length || instance.size() != guid_length)
            {
                return std::nullopt;
            }
            constexpr std::string_view hex = "0123456789abcdef";
            const auto value = [](char c) -> int
            {
                if (c >= '0' && c <= '9')
                {
                    return c - '0';
                }
                if (c >= 'a' && c <= 'f')
                {
                    return c - 'a' + 10;
                }
                return c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            };
            std::string out(guid_length, '-');
            for (std::size_t i = 0; i < guid_length; ++i)
            {
                if (member[i] == '-' && instance[i] == '-')
                {
                    continue;
                }
                const int a = value(member[i]);
                const int b = value(instance[i]);
                if (a < 0 || b < 0)
                {
                    return std::nullopt;
                }
                out[i] = hex[static_cast<std::size_t>(a ^ b)];
            }
            return out;
        }

        [[nodiscard]] XmlElement *find_child(XmlElement &element, std::string_view tag)
        {
            const auto found = std::ranges::find(element.children, tag, &XmlElement::tag);
            return found == element.children.end() ? nullptr : &*found;
        }

        [[nodiscard]] XmlElement
        make(std::string tag, std::initializer_list<std::pair<std::string_view, std::string>> attrs)
        {
            XmlElement element{.tag = std::move(tag)};
            for (const auto &[key, value] : attrs)
            {
                element.set(key, value);
            }
            return element;
        }

        /** @brief Calls @p visit on @p element and every element below it. */
        template <typename Element, typename Visit> void for_each_element(Element &element, const Visit &visit)
        {
            visit(element);
            for (Element &child : element.children)
            {
                for_each_element(child, visit);
            }
        }

        /** @brief The prefab library: every `<Prefab>` of `Prefabs/**.xml` in IPL_GameData.pak, by its Id. */
        class PrefabLibrary
        {
        public:
            explicit PrefabLibrary(fs::path pak) : m_pak{std::move(pak)} {}

            /** @brief The prefab with Id @p id, or null. The first call reads the library. */
            [[nodiscard]] const XmlElement *find(const std::string &id)
            {
                load();
                const auto found = m_prefabs.find(id);
                return found == m_prefabs.end() ? nullptr : &found->second;
            }

        private:
            void load()
            {
                if (m_loaded)
                {
                    return;
                }
                m_loaded = true;
                const std::optional<std::vector<ZipEntry>> index = read_zip_index(m_pak);
                for (const ZipEntry &entry : index.value_or(std::vector<ZipEntry>{}))
                {
                    if (!entry.name.starts_with(PREFAB_FOLDER) || !entry.name.ends_with(".xml"))
                    {
                        continue;
                    }
                    const std::optional<std::string> text = read_zip_file(m_pak, entry);
                    const std::optional<XmlElement> root = text ? read_xml_elements(*text) : std::nullopt;
                    if (root)
                    {
                        for_each_element(
                            *root,
                            [this](const XmlElement &element)
                            {
                                if (element.tag == "Prefab" && !element.get("Id").empty())
                                {
                                    m_prefabs.emplace(element.get("Id"), element);
                                }
                            }
                        );
                    }
                }
            }

            fs::path m_pak;
            bool m_loaded = false;
            std::unordered_map<std::string, XmlElement> m_prefabs;
        };

        /**
         * @brief The entities of the game's own layers of one level (its `level.pak`), by 64-bit GUID: what a link to
         *        an entity outside the scene's layer names. The game's export writes such a link as the target's
         *        level-wide EntityId and its GUID. Level ids start far above a scene's own ids 1..N (the game's
         *        lowest is 19649), so the framework's runtime renumbering of 1..N never touches them.
         */
        class LevelEntities
        {
        public:
            explicit LevelEntities(fs::path pak) : m_pak{std::move(pak)} {}

            /**
             * @brief The EntityId of the entity whose 64-bit GUID is @p guid, or null. The first call reads the level.
             */
            [[nodiscard]] const std::string *find(std::string_view guid)
            {
                load();
                const auto found = m_ids.find(lowercase(guid));
                return found == m_ids.end() ? nullptr : &found->second;
            }

        private:
            void load()
            {
                if (m_loaded)
                {
                    return;
                }
                m_loaded = true;
                constexpr std::string_view entity_tag = "<Entity ";
                const std::optional<std::vector<ZipEntry>> index = read_zip_index(m_pak);
                for (const ZipEntry &entry : index.value_or(std::vector<ZipEntry>{}))
                {
                    if (!entry.name.starts_with("layers/") || !entry.name.ends_with(".xml"))
                    {
                        continue;
                    }
                    const std::optional<std::string> text = read_zip_file(m_pak, entry);
                    const std::string_view layer = text ? std::string_view{*text} : std::string_view{};
                    for (std::size_t at = layer.find(entity_tag); at != std::string_view::npos;
                         at = layer.find(entity_tag, at + entity_tag.size()))
                    {
                        const std::string_view tag = layer.substr(at, layer.find('>', at) - at);
                        const std::string_view guid = attribute(tag, "EntityGuid");
                        const std::string_view id = attribute(tag, "EntityId");
                        if (!guid.empty() && !id.empty())
                        {
                            m_ids.emplace(lowercase(guid), std::string{id});
                        }
                    }
                }
            }

            fs::path m_pak;
            bool m_loaded = false;
            std::unordered_map<std::string, std::string> m_ids;
        };

        /** @brief The level a scene's links to outside entities resolve in, and what became of those links. */
        struct OutsideLinks
        {
            LevelEntities level;
            std::size_t resolved = 0;
            /// Links whose target neither the scene nor the game's level holds, such as a prefab's port.
            std::size_t dropped = 0;
        };

        /// A prefab instance's object GUIDs: template object Id -> (instance object Id, runtime name).
        using GuidMap =
            std::unordered_map<std::string, std::unordered_map<std::string, std::pair<std::string, std::string>>>;

        /**
         * @brief Appends to @p out every object of prefab instance @p instance, under the instance's object Ids and
         *        with the runtime names its ObjectGUIDs give. Nested prefabs expand in place.
         * @details Members are placed as the editor's export places them. A member whose parent is another member
         *          keeps its Pos and Rotate relative to that member. Every other member turns and moves with the
         *          instance and takes the instance's own Parent, so convert() bakes in the groups above the instance or
         *          attaches the member to the instance's parent entity. A member the instance's ObjectGUIDs do not list
         *          takes the GUID the editor gives it (prefab_member_guid) and the name the game's export gives it.
         *          Brushes and groups stay in @p out without being exported, so convert() can walk a child's parents
         *          through them.
         */
        void expand_prefab(
            PrefabLibrary &library,
            const XmlElement &instance,
            const GuidMap &guids,
            std::deque<XmlElement> &out,
            int depth
        )
        {
            const XmlElement *prefab = library.find(instance.get("PrefabTemplate"));
            const XmlElement *objects = prefab != nullptr ? prefab->child("Objects") : nullptr;
            if (objects == nullptr || depth > MAX_PREFAB_DEPTH)
            {
                return;
            }
            const std::string instance_id = instance.get("Id");
            const auto mapping = guids.find(instance_id);
            const auto mapped = [&](const std::string &template_id) -> const std::pair<std::string, std::string> *
            {
                if (mapping == guids.end())
                {
                    return nullptr;
                }
                const auto found = mapping->second.find(template_id);
                return found == mapping->second.end() ? nullptr : &found->second;
            };
            const auto member_id = [&](const std::string &template_id) -> std::optional<std::string>
            {
                const std::pair<std::string, std::string> *guid = mapped(template_id);
                return guid != nullptr ? std::optional{guid->first} : prefab_member_guid(template_id, instance_id);
            };
            const auto id_of = [&](const std::string &template_id) -> std::string
            { return member_id(template_id).value_or(std::format("{}/{}", instance_id, template_id)); };
            std::unordered_set<std::string> members;
            for (const XmlElement &object : objects->children)
            {
                members.insert(object.get("Id"));
            }
            const Vec3 position = numbers_or<3>(instance, "Pos", ORIGIN);
            const Quat rotation = numbers_or<4>(instance, "Rotate", IDENTITY);
            const std::string instance_parent = instance.get("Parent");
            for (const XmlElement &object : objects->children)
            {
                const std::string template_id = object.get("Id");
                const std::string parent = object.get("Parent");
                XmlElement copy = object;
                copy.set("Id", id_of(template_id));
                if (!parent.empty() && members.contains(parent))
                {
                    copy.set("Parent", id_of(parent));
                }
                else
                {
                    const Vec3 turned = rotate(rotation, numbers_or<3>(object, "Pos", ORIGIN));
                    copy.set(
                        "Pos",
                        format_numbers(Vec3{position[0] + turned[0], position[1] + turned[1], position[2] + turned[2]})
                    );
                    copy.set("Rotate", format_numbers(mul(rotation, numbers_or<4>(object, "Rotate", IDENTITY))));
                    if (instance_parent.empty())
                    {
                        copy.erase("Parent");
                    }
                    else
                    {
                        copy.set("Parent", instance_parent);
                    }
                }
                // A template's links name its own objects by their template ids.
                if (XmlElement *links = find_child(copy, "EntityLinks"))
                {
                    for (XmlElement &link : links->children)
                    {
                        if (const std::string target = link.get("TargetId"); members.contains(target))
                        {
                            link.set("TargetId", id_of(target));
                        }
                    }
                }
                if (object.get("Type") == "Prefab")
                {
                    expand_prefab(library, copy, guids, out, depth + 1);
                    continue;
                }
                if (const std::pair<std::string, std::string> *guid = mapped(template_id); guid != nullptr)
                {
                    copy.set("Name", guid->second);
                }
                else if (member_id(template_id).has_value())
                {
                    // The runtime name the game's export gives such a member.
                    copy.set("Name", std::format("{}[{}_{}]", object.get("Name"), instance.get("Name"), instance_id));
                }
                else
                {
                    copy.erase("EntityClass");
                }
                copy.set("PrefabMember", "1");
                out.push_back(std::move(copy));
            }
        }

        /** @brief Objects in export order, a group's or prefab's own objects right after it. */
        void flatten(const XmlElement &objects, std::vector<const XmlElement *> &out)
        {
            for (const XmlElement &object : objects.children)
            {
                out.push_back(&object);
                if (const XmlElement *inner = object.child("Objects"))
                {
                    flatten(*inner, out);
                }
            }
        }

        /** @brief A SequenceArea's runtime area: its Points (and SoundData sides) with Roof and Floor. */
        [[nodiscard]] XmlElement area_of(const XmlElement &object)
        {
            const XmlElement *sound = object.child("SoundData");
            const auto side = [&](std::string_view key)
            {
                const std::string *value = sound != nullptr ? sound->attr(key) : nullptr;
                return value != nullptr ? *value : std::string{"0"};
            };
            const std::string *height = object.attr("Height");
            XmlElement area = make(
                "Area",
                {{"Id", "0"},
                 {"Group", "0"},
                 {"Proximity", "0"},
                 {"Priority", "0"},
                 {"Height", height != nullptr ? *height : std::string{"3"}}}
            );
            XmlElement points{.tag = "Points"};
            std::size_t i = 0;
            for (const XmlElement &point : object.child("Points")->children)
            {
                ++i;
                points.children.push_back(
                    make("Point", {{"Pos", point.get("Pos")}, {"ObstructSound", side(std::format("Side{}", i))}})
                );
            }
            area.children.push_back(std::move(points));
            area.children.push_back(make("Roof", {{"ObstructSound", side("Roof")}}));
            area.children.push_back(make("Floor", {{"ObstructSound", side("Floor")}}));
            return area;
        }

        /// Editor object GUID -> local entity id.
        using IdMap = std::unordered_map<std::string, int>;

        /**
         * @brief A child of editor object @p owner as the export writes it: a sequence gets its defaults (and the
         *        CleanupAreas of the SequenceAreas @p owner links as "mizeni"), entity links point at local ids, or at
         *        the level's ids for targets outside the layer (@p outside). A link whose target neither holds goes.
         */
        void fix_child(XmlElement &child, const IdMap &ids, const XmlElement &owner, OutsideLinks &outside)
        {
            if (child.tag == "Sequence")
            {
                if (find_child(child, "CleanupAreas") == nullptr)
                {
                    XmlElement areas{.tag = "CleanupAreas"};
                    if (const XmlElement *links = owner.child("EntityLinks"))
                    {
                        for (const XmlElement &link : links->children)
                        {
                            if (link.get("Name") != CLEANUP_LINK)
                            {
                                continue;
                            }
                            const std::string target = link.get("TargetId");
                            if (const auto local = ids.find(target); local != ids.end())
                            {
                                areas.children.push_back(make("CleanupArea", {{"ID", std::to_string(local->second)}}));
                            }
                            else if (const std::string *level_id = outside.level.find(guid64(target)))
                            {
                                areas.children.push_back(make("CleanupArea", {{"ID", *level_id}}));
                            }
                        }
                    }
                    if (!areas.children.empty())
                    {
                        child.children.push_back(std::move(areas));
                    }
                }
                if (find_child(child, "Events") == nullptr)
                {
                    // Right after Nodes, else last.
                    const auto nodes = std::ranges::find(child.children, std::string_view{"Nodes"}, &XmlElement::tag);
                    child.children.insert(
                        nodes == child.children.end() ? nodes : nodes + 1,
                        XmlElement{.tag = "Events"}
                    );
                }
                if (find_child(child, "Nodes") == nullptr)
                {
                    child.children.insert(child.children.begin(), XmlElement{.tag = "Nodes"});
                }
                constexpr std::array<std::pair<std::string_view, std::string_view>, 6> defaults{{
                    {"SequenceVersion", "3"},
                    {"ID", "0"},
                    {"Flags", "0"},
                    {"StartTime", "0"},
                    {"EndTime", "10"},
                    {"Notice", ""},
                }};
                for (const auto &[key, value] : defaults)
                {
                    if (child.attr(key) == nullptr)
                    {
                        child.set(key, std::string{value});
                    }
                }
            }
            if (child.tag == "EntityLinks")
            {
                std::vector<XmlElement> kept;
                for (XmlElement &link : child.children)
                {
                    if (link.attr("Name") == nullptr)
                    {
                        link.set("Name", "");
                    }
                    link.erase("DebugTargetName");
                    const std::string target = link.get("TargetId");
                    if (const auto found = ids.find(target); found != ids.end())
                    {
                        link.set("TargetId", std::to_string(found->second));
                    }
                    else if (const std::string *level_id = outside.level.find(guid64(target)))
                    {
                        link.set("TargetId", *level_id);
                        ++outside.resolved;
                    }
                    else
                    {
                        ++outside.dropped;
                        continue;
                    }
                    link.set("TargetGuid", guid64(target));
                    kept.push_back(std::move(link));
                }
                child.children = std::move(kept);
            }
        }

        /** @brief Whether an object goes to the game: Properties without bExported_to_game="0". */
        [[nodiscard]] bool exported(const XmlElement &object)
        {
            const XmlElement *properties = object.child("Properties");
            return properties == nullptr || properties->get("bExported_to_game") != "0";
        }

        /**
         * @brief The runtime layer of the `.lyr` @p lyr, its entities numbered 1..N.
         * @param prefab_parts Grows by the number of entities expanded from prefab instances.
         * @param outside Resolves and counts the links to entities outside the layer.
         */
        [[nodiscard]] XmlElement convert(
            const XmlElement &lyr,
            std::string_view layer_name,
            PrefabLibrary &library,
            std::size_t &prefab_parts,
            OutsideLinks &outside
        )
        {
            const XmlElement *objects = lyr.child("LayerObjects");
            objects = objects != nullptr ? objects : lyr.child("Objects");
            std::vector<const XmlElement *> flat;
            if (objects != nullptr)
            {
                flatten(*objects, flat);
            }
            // A deque keeps the expanded objects in place as it grows, so the pointers in `every` stay valid.
            std::deque<XmlElement> expanded;
            std::vector<const XmlElement *> every;
            for (const XmlElement *object : flat)
            {
                if (object->get("Type") == "Prefab" && !object->get("PrefabTemplate").empty())
                {
                    GuidMap guids;
                    for_each_element(
                        *object,
                        [&](const XmlElement &element)
                        {
                            if (element.tag != "InstanceProperties")
                            {
                                return;
                            }
                            auto &map = guids[element.get("Id")];
                            for_each_element(
                                element,
                                [&](const XmlElement &guid)
                                {
                                    if (guid.tag == "ObjectGUID")
                                    {
                                        map[guid.get("Template")] = {guid.get("Instance"), guid.get("Name")};
                                    }
                                }
                            );
                        }
                    );
                    const std::size_t before = expanded.size();
                    expand_prefab(library, *object, guids, expanded, 0);
                    for (std::size_t i = before; i < expanded.size(); ++i)
                    {
                        every.push_back(&expanded[i]);
                    }
                    continue;
                }
                every.push_back(object);
            }
            std::unordered_map<std::string, const XmlElement *> by_id;
            for (const XmlElement *object : every)
            {
                by_id.emplace(object->get("Id"), object);
            }
            std::vector<const XmlElement *> entities;
            for (const XmlElement *object : every)
            {
                const std::string cls = object->get("EntityClass");
                if (!cls.empty() && cls != "PrefabPort" && exported(*object))
                {
                    entities.push_back(object);
                }
            }
            IdMap ids;
            for (std::size_t n = 0; n < entities.size(); ++n)
            {
                ids.emplace(entities[n]->get("Id"), static_cast<int>(n + 1));
            }

            XmlElement out{.tag = "Objects"};
            for (const XmlElement *object : entities)
            {
                const std::string cls = object->get("EntityClass");
                XmlElement entity{.tag = "Entity"};
                if (object->attr("PrefabMember") != nullptr)
                {
                    entity.set("Prefab", "1");
                    ++prefab_parts;
                }
                entity.set("Name", object->get("Name"));
                // The object's attributes, with Pos and Rotate in world space once a group's transform is baked in.
                XmlElement placed{
                    .attrs = object->attrs,
                };
                const std::string parent = object->get("Parent");
                if (!parent.empty() && ids.contains(parent))
                {
                    entity.set("ParentId", std::to_string(ids.at(parent)));
                    for (const std::string_view key : {"AttachmentType", "AttachmentTarget"})
                    {
                        if (const std::string *value = object->attr(key))
                        {
                            entity.set(key, *value);
                        }
                    }
                }
                else if (!parent.empty() && by_id.contains(parent))
                {
                    // A group or other editor-only parent: the export bakes the transforms of such parents in, up to
                    // the first exported entity above them, else into world space. The walk stops after as many steps
                    // as there are objects, so a parent cycle cannot hang it.
                    Vec3 position = numbers_or<3>(*object, "Pos", ORIGIN);
                    Quat rotation = numbers_or<4>(*object, "Rotate", IDENTITY);
                    const XmlElement *node = by_id.at(parent);
                    for (std::size_t step = 0; node != nullptr && !ids.contains(node->get("Id")) && step < by_id.size();
                         ++step)
                    {
                        const Quat turn = numbers_or<4>(*node, "Rotate", IDENTITY);
                        const Vec3 rotated = rotate(turn, position);
                        const Vec3 base = numbers_or<3>(*node, "Pos", ORIGIN);
                        position = {base[0] + rotated[0], base[1] + rotated[1], base[2] + rotated[2]};
                        rotation = mul(turn, rotation);
                        const auto up = by_id.find(node->get("Parent"));
                        node = up == by_id.end() ? nullptr : up->second;
                    }
                    // A group under an exported entity: the baked position is local to that entity, as a child
                    // entity's own position is, so the entity becomes the parent.
                    if (node != nullptr && ids.contains(node->get("Id")))
                    {
                        entity.set("ParentId", std::to_string(ids.at(node->get("Id"))));
                    }
                    // An identity Rotate is dropped below, as the export leaves it out.
                    placed.set("Pos", format_numbers(position));
                    placed.set("Rotate", format_numbers(rotation));
                }
                for (const XmlAttr &attr : placed.attrs)
                {
                    if (DROP_ATTRS.contains(attr.name) || attr.name == "Name" || attr.name == "EntityClass")
                    {
                        continue;
                    }
                    if ((attr.name == "Scale" && is_unit_scale(attr.value)) ||
                        (attr.name == "Rotate" && is_identity_rotation(attr.value)))
                    {
                        continue;
                    }
                    entity.set(attr.name == "NoStaticDecals" ? "NoDecals" : attr.name, attr.value);
                }
                const std::string *shadow = object->attr("CastShadowMinspec");
                entity.set("EntityClass", cls);
                entity.set("EntityId", std::to_string(ids.at(object->get("Id"))));
                entity.set("EntityGuid", guid64(object->get("Id")));
                entity.set(
                    "CastShadowMinSpec",
                    shadow != nullptr && !shadow->empty() ? *shadow : (LIGHT_CLASSES.contains(cls) ? "18" : "1")
                );
                entity.set("Layer", std::string{layer_name});
                entity.set("EditorLayer", std::string{layer_name});
                if (NO_CHILDREN.contains(cls))
                {
                    out.children.push_back(std::move(entity));
                    continue;
                }
                for (const std::string_view tag : BEFORE_PROPERTIES)
                {
                    for (const XmlElement &child : object->children)
                    {
                        if (child.tag == tag)
                        {
                            XmlElement copy = child;
                            fix_child(copy, ids, *object, outside);
                            entity.children.push_back(std::move(copy));
                        }
                    }
                }
                if (cls == "ClipVolume")
                {
                    // The level export bakes the clip geometry into the level. A mod layer points at its source mesh.
                    entity.children.push_back(
                        make("ClipVolume", {{"GeometryFileName", object->get("SourceGeometry")}, {"Flags", "68"}})
                    );
                }
                if (!NO_PROPERTIES.contains(cls))
                {
                    const XmlElement *source = object->child("Properties");
                    XmlElement properties = source != nullptr ? *source : XmlElement{.tag = "Properties"};
                    properties.erase("bExported_to_test");
                    if (!NO_SAVED_FLAG.contains(cls) && properties.attr("bSaved_by_game") == nullptr)
                    {
                        properties.set("bSaved_by_game", "0");
                    }
                    entity.children.push_back(std::move(properties));
                }
                if (cls == "CameraSource")
                {
                    const XmlElement *dof = object->child("DofParams");
                    const auto dof_value = [&](std::string_view key, std::string_view fallback)
                    {
                        const std::string *value = dof != nullptr ? dof->attr(key) : nullptr;
                        return value != nullptr ? *value : std::string{fallback};
                    };
                    const std::string *fov = object->attr("FOV");
                    entity.children.push_back(make(
                        "CameraProxy",
                        {{"Fov", fov != nullptr ? *fov : std::string{DEFAULT_FOV}},
                         {"NearZ", "0.25"},
                         {"FarZ", "1024"},
                         {"DofEnable", dof_value("Enable", "0")},
                         {"FocusDistance", dof_value("Distance", "0")},
                         {"FocusRange", dof_value("Range", "0")},
                         {"BlurAmount", dof_value("BlurAmount", "1")}}
                    ));
                }
                if (AREA_CLASSES.contains(cls) && object->child("Points") != nullptr)
                {
                    entity.children.push_back(area_of(*object));
                }
                if (BOID_CLASSES.contains(cls))
                {
                    // The export gives a flock a sphere area around itself, listing itself.
                    XmlElement area = make(
                        "Area",
                        {{"Id", "-1"},
                         {"Group", "-1"},
                         {"Proximity", "5"},
                         {"Priority", "0"},
                         {"SphereCenter", "0,0,0"},
                         {"SphereRadius", "40"}}
                    );
                    XmlElement listed{.tag = "Entities"};
                    listed.children.push_back(make("Entity", {{"Id", std::to_string(ids.at(object->get("Id")))}}));
                    area.children.push_back(std::move(listed));
                    entity.children.push_back(std::move(area));
                }
                for (const XmlElement &child : object->children)
                {
                    if (DROP_CHILDREN.contains(child.tag) ||
                        std::ranges::find(BEFORE_PROPERTIES, child.tag) != BEFORE_PROPERTIES.end() ||
                        child.tag == "Properties" || child.tag == "Points" || child.tag == "Objects")
                    {
                        continue;
                    }
                    XmlElement copy = child;
                    fix_child(copy, ids, *object, outside);
                    entity.children.push_back(std::move(copy));
                }
                out.children.push_back(std::move(entity));
            }
            return out;
        }

        /**
         * @brief The SequenceObject of @p layer whose sequence is the scene's: the one named @p layer_name (ignoring
         *        case), else the first cutscene (its Cut Scene flag, 8, on), else the first. Null when none holds one.
         * @details A layer can hold more sequences than the scene's, such as the level's `_LightAnimationSet`.
         */
        [[nodiscard]] XmlElement *find_scene_object(XmlElement &layer, std::string_view layer_name)
        {
            constexpr unsigned cut_scene_flag = 8;
            XmlElement *best = nullptr;
            int best_rank = 0;
            for (XmlElement &entity : layer.children)
            {
                const XmlElement *sequence =
                    entity.get("EntityClass") == "SequenceObject" ? entity.child("Sequence") : nullptr;
                if (sequence == nullptr)
                {
                    continue;
                }
                const std::string flags_text = sequence->get("Flags");
                unsigned flags = 0;
                const auto parsed = std::from_chars(flags_text.data(), flags_text.data() + flags_text.size(), flags);
                int rank = 1;
                if (lowercase(sequence->get("Name")) == layer_name)
                {
                    rank = 3;
                }
                else if (parsed.ec == std::errc{} && (flags & cut_scene_flag) != 0)
                {
                    rank = 2;
                }
                if (rank > best_rank)
                {
                    best = &entity;
                    best_rank = rank;
                }
            }
            return best;
        }

        /**
         * @brief The level folder name in @p lyr's path (`.../Levels/<level>/Layers/...`, ignoring case), or empty.
         * @details The match runs from the file up, so a `Levels` folder higher up the editor's path, or a layer
         *          subfolder of that name, does not name the level. A relative @p lyr resolves against the current
         *          folder.
         */
        [[nodiscard]] std::string level_of(const fs::path &lyr)
        {
            std::error_code error;
            const fs::path folder = fs::absolute(lyr, error).parent_path();
            const std::vector<fs::path> parts{folder.begin(), folder.end()};
            for (std::size_t end = parts.size(); end >= 3; --end)
            {
                if (lowercase(parts[end - 1].string()) == "layers" && lowercase(parts[end - 3].string()) == "levels")
                {
                    return parts[end - 2].string();
                }
            }
            return {};
        }
    } // namespace

    std::expected<std::string, std::string>
    convert_sandbox_layer(const SandboxLayerRequest &request, const ToolPrint &print)
    {
        const std::string name = request.name.empty() ? lowercase(request.layer_file.stem().string()) : request.name;
        if (!is_scene_name(name))
        {
            return std::unexpected{
                std::format("{}: name the scene with lowercase letters, digits and underscores (--as <name>)", name)
            };
        }
        const std::string level = level_of(request.layer_file);
        if (level.empty())
        {
            return std::unexpected{std::format(
                "{} is not inside a level folder (<Sandbox Data>/Levels/<level>/Layers/...)",
                request.layer_file.string()
            )};
        }
        const std::optional<std::string> text = read_text(request.layer_file);
        const std::optional<XmlElement> lyr = text ? read_xml_elements(*text) : std::nullopt;
        if (!lyr)
        {
            return std::unexpected{std::format("{} does not read as a Sandbox layer", request.layer_file.string())};
        }

        PrefabLibrary library{request.game_root / "Data" / std::string{PREFAB_PAK}};
        std::size_t prefab_parts = 0;
        OutsideLinks outside{
            .level = LevelEntities{request.game_root / "Data" / "Levels" / level / "level.pak"},
        };
        XmlElement layer = convert(*lyr, name, library, prefab_parts, outside);
        if (layer.children.empty())
        {
            return std::unexpected{std::format("{} holds no entities", request.layer_file.string())};
        }

        XmlElement *scene_object = find_scene_object(layer, lowercase(request.layer_file.stem().string()));
        if (scene_object == nullptr)
        {
            return std::unexpected{
                std::format("{} holds no SequenceObject with a sequence", request.layer_file.string())
            };
        }
        XmlElement &scene_sequence = *find_child(*scene_object, "Sequence");
        const std::string sequence = scene_sequence.get("Name");
        scene_sequence.set("Name", name);
        scene_object->set("Name", name);
        const XmlElement *cleanup = scene_sequence.child("CleanupAreas");
        const std::size_t areas = cleanup != nullptr ? cleanup->children.size() : 0;

        // New GUIDs for the layer's own entities.
        std::unordered_map<std::string, std::string> guids;
        for (const XmlElement &entity : layer.children)
        {
            guids.emplace(entity.get("EntityGuid"), new_guid(name, entity.get("EntityGuid")));
        }
        for (XmlElement &entity : layer.children)
        {
            for_each_element(
                entity,
                [&](XmlElement &element)
                {
                    for (const std::string_view key : GUID_ATTRIBUTES)
                    {
                        const std::string *value = element.attr(key);
                        const auto found = value != nullptr ? guids.find(*value) : guids.end();
                        if (found != guids.end())
                        {
                            element.set(key, found->second);
                        }
                    }
                }
            );
        }

        // The row: the one asked for, else the vanilla cinematic's own, else a plain row.
        std::string row_name = request.row;
        if (row_name.empty())
        {
            row_name = find_row(request.game_root / "Data" / "Levels" / level / "level.pak", sequence);
        }
        std::string row = std::format("<IngameCutscene Name=\"{}\" />", name);
        if (!row_name.empty())
        {
            std::optional<std::string> found = table_row(request.game_root, row_name, name);
            if (!found && !request.row.empty())
            {
                return std::unexpected{
                    std::format("the cutscene table (Data/Tables.pak) has no in-game cutscene row {}", row_name)
                };
            }
            if (found)
            {
                row = std::move(*found);
            }
            else
            {
                // The level names a row the table no longer has (cut content), or a video's or a TrackView scene's row,
                // which the in-game cutscene player cannot start: a plain row instead.
                print(std::format("  the cutscene table has no in-game cutscene row {}: a plain row", row_name));
                row_name.clear();
            }
        }

        std::string xml;
        write_xml_elements(layer, xml);
        const auto files = write_scene(request.mod_folder, level, name, xml, row);
        if (!files)
        {
            return std::unexpected{files.error()};
        }
        print(
            std::format(
                "  {} ({}): {} entities ({} from prefabs), sequence {} -> {}, {} cleanup area(s)",
                request.layer_file.filename().string(),
                level,
                layer.children.size(),
                prefab_parts,
                sequence,
                files->layer.string(),
                areas
            )
        );
        if (outside.resolved + outside.dropped > 0)
        {
            print(
                std::format(
                    "  links to entities outside the layer: {} kept, {} dropped (no such entity in the scene or the "
                    "game's level)",
                    outside.resolved,
                    outside.dropped
                )
            );
        }
        print(
            std::format(
                "  row {} -> {} in {}",
                row_name.empty() ? std::string{"(plain)"} : row_name,
                name,
                files->table.string()
            )
        );
        return register_line(name, sound_banks(request.game_root, xml));
    }
} // namespace AnimationFramework
