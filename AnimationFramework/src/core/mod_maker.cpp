/**
 * @file core/mod_maker.cpp
 * @brief New action patches (see mod_maker.hpp).
 */

#include "core/mod_maker.hpp"

#include "core/text_util.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <optional>

namespace AnimationFramework
{
    namespace
    {
        constexpr std::string_view XML_DECLARATION = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
        constexpr std::string_view SCOPE_TAG_PREFIX = "scope_";
        constexpr std::string_view LOOP_FLAG = "Loop";
        constexpr std::string_view UPPER_SUFFIX = "_u_";

        // Henry's joints from the chest up, named as in male.chr (the engine matches the CRC-32 of the exact name), as
        // the Gesture Wheel's upper-body twins keep them: the seat's or the legs' own lower back stays.
        constexpr std::array<std::string_view, 8>
            UPPER_CORE{"Spine3", "Spine4", "Neck", "Neck1", "Head", "HeadEnd", "eye_right_bone", "eye_left_bone"};
        constexpr std::array<std::string_view, 13> UPPER_ARM_PARTS{
            "Shoulder",
            "Arm",
            "ArmRoll",
            "ArmRoll1",
            "ForeArm",
            "Elbow",
            "ForeArmRoll",
            "ForeArmRoll1",
            "ForeArmRoll2",
            "Hand",
            "InHandPinky",
            "InHandRing",
            "HandSlot",
        };
        constexpr std::array<std::string_view, 5> UPPER_FINGERS{"Thumb", "Index", "Middle", "Ring", "Pinky"};
        constexpr std::array<std::string_view, 2> SIDES{"Left", "Right"};

        /** @brief Whether @p a and @p b are the same name, ASCII case ignored, as the game compares Mannequin names. */
        [[nodiscard]] bool same_name(std::string_view a, std::string_view b)
        {
            return lowercase(a) == lowercase(b);
        }

        /// The first child of @p parent tagged @p name (same_name), or null.
        [[nodiscard]] const XmlElement *child_named(const XmlElement &parent, std::string_view name)
        {
            const auto it =
                std::ranges::find_if(parent.children, [name](const XmlElement &c) { return same_name(c.tag, name); });
            return it != parent.children.end() ? &*it : nullptr;
        }

        /// The first child of @p parent tagged @p name (same_name), added empty at the end when there is none.
        [[nodiscard]] XmlElement &child_or_add(XmlElement &parent, std::string_view name)
        {
            const auto it =
                std::ranges::find_if(parent.children, [name](const XmlElement &c) { return same_name(c.tag, name); });
            if (it != parent.children.end())
            {
                return *it;
            }
            parent.children.push_back(
                XmlElement{
                    .tag = std::string{name},
                }
            );
            return parent.children.back();
        }

        /** @brief The mod's current patch document, a fresh @p root when there is none yet. */
        [[nodiscard]] std::expected<XmlElement, std::string>
        patch_document(std::string_view text, std::string_view root, std::string_view what)
        {
            if (text.empty())
            {
                return XmlElement{
                    .tag = std::string{root},
                };
            }
            std::optional<XmlElement> document = read_xml_elements(text);
            if (!document || !same_name(document->tag, root))
            {
                return std::unexpected{std::format("the mod's {} patch does not read as <{}>", what, root)};
            }
            return std::move(*document);
        }

        /// @p root written as a document, after an XML declaration.
        [[nodiscard]] std::string written(const XmlElement &root)
        {
            std::string out{XML_DECLARATION};
            write_xml_elements(root, out);
            return out;
        }

        /** @brief An option that only sets up another scope of the fragment (all its tags are `Scope_` tags). */
        [[nodiscard]] bool scope_helper(const XmlElement &option)
        {
            const std::vector<std::string> tags = split_tags(option.get("Tags"));
            return !tags.empty() && std::ranges::all_of(
                                        tags,
                                        [](const std::string &t) { return lowercase(t).starts_with(SCOPE_TAG_PREFIX); }
                                    );
        }

        /// Whether @p option plays a named clip on one of its animation layers.
        [[nodiscard]] bool plays_clip(const XmlElement &option)
        {
            return !first_clip(option).empty();
        }

        /** @brief The template option made to play @p action's clip alone: one animation on its first layer. */
        [[nodiscard]] XmlElement action_option(const XmlElement &template_option, const ModAction &action)
        {
            XmlElement option = template_option;
            // An upper-body action plays its clip and nothing else on the UpperBody layers: the copied procedural
            // layers (colliders, putting the weapons away) belong to the full body.
            if (action.upper)
            {
                std::erase_if(
                    option.children,
                    [](const XmlElement &layer) { return !same_name(layer.tag, "AnimLayer"); }
                );
            }
            option.set("GUID", option_guid(action.modid, action.fragment, 0));
            option.set("Tags", "");
            option.erase("FragTags");
            bool kept_layer = false;
            std::erase_if(
                option.children,
                [&kept_layer](const XmlElement &layer)
                {
                    if (!same_name(layer.tag, "AnimLayer"))
                    {
                        return false;
                    }
                    const bool drop = kept_layer;
                    kept_layer = true;
                    return drop;
                }
            );
            for (XmlElement &layer : option.children)
            {
                if (!same_name(layer.tag, "AnimLayer"))
                {
                    continue;
                }
                // Keep the blend before the first animation, then that animation. Mannequin holds a fragment that
                // chains clips until its last clip starts.
                std::vector<XmlElement> kept;
                for (XmlElement &step : layer.children)
                {
                    if (same_name(step.tag, "Animation"))
                    {
                        step.set(
                            "name",
                            action.upper ? upper_blend_space_name(action.modid, action.clip) : action.clip
                        );
                        std::vector<std::string> flags = split_tags(step.get("flags"));
                        std::erase_if(flags, [](const std::string &f) { return same_name(f, LOOP_FLAG); });
                        if (action.loop)
                        {
                            flags.emplace_back(LOOP_FLAG);
                        }
                        std::string joined;
                        for (const std::string &f : flags)
                        {
                            joined += (joined.empty() ? "" : "+") + f;
                        }
                        if (joined.empty())
                        {
                            step.erase("flags");
                        }
                        else
                        {
                            step.set("flags", joined);
                        }
                        kept.push_back(std::move(step));
                        break;
                    }
                    kept.push_back(std::move(step));
                }
                layer.children = std::move(kept);
            }
            return option;
        }

        /** @brief FNV-1a, then the SplitMix64 finalizer: keys one character apart give unrelated GUIDs. */
        [[nodiscard]] std::uint64_t fnv1a(std::string_view text, std::uint64_t seed) noexcept
        {
            std::uint64_t hash = seed;
            for (const char c : text)
            {
                hash ^= static_cast<unsigned char>(c);
                hash *= 0x100000001B3ULL;
            }
            hash ^= hash >> 30U;
            hash *= 0xBF58476D1CE4E5B9ULL;
            hash ^= hash >> 27U;
            hash *= 0x94D049BB133111EBULL;
            hash ^= hash >> 31U;
            return hash;
        }
    } // namespace

    const XmlElement *find_fragment_options(const XmlElement &database, std::string_view fragment)
    {
        const XmlElement *list = child_named(database, "FragmentList");
        return list != nullptr ? child_named(*list, fragment) : nullptr;
    }

    const XmlElement *find_fragment_definition(const XmlElement &controller_defs, std::string_view fragment)
    {
        const XmlElement *defs = child_named(controller_defs, "FragmentDefs");
        return defs != nullptr ? child_named(*defs, fragment) : nullptr;
    }

    std::vector<std::string> sub_databases(const XmlElement &database)
    {
        std::vector<std::string> files;
        if (const XmlElement *subs = child_named(database, "SubADBs"); subs != nullptr)
        {
            for (const XmlElement &sub : subs->children)
            {
                std::string file = fold_path(sub.get("File"));
                if (!file.empty())
                {
                    files.push_back(std::move(file));
                }
            }
        }
        return files;
    }

    bool lists_fragment(const XmlElement &fragment_ids, std::string_view fragment)
    {
        const XmlElement *tags = child_named(fragment_ids, "Tags");
        return tags != nullptr && std::ranges::any_of(
                                      tags->children,
                                      [fragment](const XmlElement &t) { return same_name(t.get("name"), fragment); }
                                  );
    }

    const XmlElement *template_option(const XmlElement &like_options)
    {
        const XmlElement *any = nullptr;
        for (const XmlElement &option : like_options.children)
        {
            if (!same_name(option.tag, "Fragment") || !plays_clip(option))
            {
                continue;
            }
            if (split_tags(option.get("Tags")).empty())
            {
                return &option;
            }
            any = any != nullptr ? any : &option;
        }
        return any;
    }

    std::string first_clip(const XmlElement &option)
    {
        for (const XmlElement &layer : option.children)
        {
            if (!same_name(layer.tag, "AnimLayer"))
            {
                continue;
            }
            for (const XmlElement &step : layer.children)
            {
                if (same_name(step.tag, "Animation") && !step.get("name").empty())
                {
                    return step.get("name");
                }
            }
        }
        return {};
    }

    std::expected<ModPatches, std::string> add_mod_action(
        const ModPatches &current,
        const ModAction &action,
        const XmlElement &like_definition,
        const XmlElement &like_options
    )
    {
        const XmlElement *model = template_option(like_options);
        if (model == nullptr)
        {
            return std::unexpected{std::format("{} has no option that plays a clip to copy", like_options.tag)};
        }

        auto ids = patch_document(current.fragment_ids, "TagDefinition", "fragment ID");
        auto defs = patch_document(current.controller_defs, "ControllerDef", "controller definition");
        auto database = patch_document(current.database, "AnimDB", "database");
        if (!ids || !defs || !database)
        {
            return std::unexpected{!ids ? ids.error() : !defs ? defs.error() : database.error()};
        }

        if (ids->attr("version") == nullptr)
        {
            ids->set("version", "2");
        }
        if (!lists_fragment(*ids, action.fragment))
        {
            XmlElement tag{
                .tag = "Tag",
            };
            tag.set("name", action.fragment);
            child_or_add(*ids, "Tags").children.push_back(std::move(tag));
        }

        XmlElement definition{
            .tag = action.fragment,
        };
        if (action.upper)
        {
            // Its scope comes from its own definition: the engine adds an action's forced scopes to its fragment's.
            definition.set("scopes", std::string{UPPER_BODY_SCOPE});
        }
        else
        {
            for (const XmlAttr &attr : like_definition.attrs)
            {
                definition.attrs.push_back(attr);
            }
        }
        XmlElement &fragment_defs = child_or_add(*defs, "FragmentDefs");
        std::erase_if(
            fragment_defs.children,
            [&action](const XmlElement &d) { return same_name(d.tag, action.fragment); }
        );
        fragment_defs.children.push_back(std::move(definition));

        XmlElement options{
            .tag = action.fragment,
        };
        options.children.push_back(action_option(*model, action));
        int index = 1;
        for (const XmlElement &option : like_options.children)
        {
            if (!action.upper && same_name(option.tag, "Fragment") && scope_helper(option))
            {
                XmlElement helper = option;
                helper.set("GUID", option_guid(action.modid, action.fragment, index++));
                options.children.push_back(std::move(helper));
            }
        }
        XmlElement &list = child_or_add(*database, "FragmentList");
        std::erase_if(list.children, [&action](const XmlElement &o) { return same_name(o.tag, action.fragment); });
        list.children.push_back(std::move(options));

        std::vector<ZipSource> files = current.files;
        if (action.upper)
        {
            const std::string path = upper_blend_space_path(action.modid, action.clip);
            std::erase_if(files, [&path](const ZipSource &file) { return lowercase(file.name) == path; });
            files.push_back(
                ZipSource{
                    .name = path,
                    .data = upper_body_blend_space(action.clip),
                }
            );
        }
        return ModPatches{
            .fragment_ids = written(*ids),
            .controller_defs = written(*defs),
            .database = written(*database),
            .files = std::move(files),
        };
    }

    std::string upper_blend_space_name(std::string_view modid, std::string_view clip)
    {
        return lowercase(modid) + std::string{UPPER_SUFFIX} + lowercase(clip);
    }

    std::string upper_blend_space_path(std::string_view modid, std::string_view clip)
    {
        return std::format(
            "animations/humans/male/blendspace/{}/{}.bspace",
            lowercase(modid),
            upper_blend_space_name(modid, clip)
        );
    }

    std::string upper_body_blend_space(std::string_view clip)
    {
        std::string joints;
        const auto add = [&joints](std::string_view joint)
        { joints += std::format("    <Joint Name=\"{}\" />\n", joint); };
        for (const std::string_view joint : UPPER_CORE)
        {
            add(joint);
        }
        for (const std::string_view side : SIDES)
        {
            for (const std::string_view part : UPPER_ARM_PARTS)
            {
                add(std::string{side} + std::string{part});
            }
        }
        for (const std::string_view side : SIDES)
        {
            for (const std::string_view finger : UPPER_FINGERS)
            {
                for (int index = 1; index <= 4; ++index)
                {
                    add(std::format("{}Hand{}{}", side, finger, index));
                }
            }
        }
        return std::format(
            "<?xml version=\"1.0\" encoding=\"us-ascii\"?>\n"
            "<ParaGroup>\n"
            "  <Dimensions>\n"
            "    <Param Name=\"BlendWeight2\" Min=\"0\" Max=\"1\" Cells=\"9\" />\n"
            "  </Dimensions>\n"
            "  <ExampleList>\n"
            "    <Example SetPara0=\"0\" AName=\"{0}\" />\n"
            "    <Example SetPara0=\"1\" AName=\"{0}\" />\n"
            "  </ExampleList>\n"
            "  <Blendable>\n"
            "    <Face p0=\"0\" p1=\"1\" />\n"
            "  </Blendable>\n"
            "  <JointList>\n"
            "{1}"
            "  </JointList>\n"
            "</ParaGroup>\n",
            clip,
            joints
        );
    }

    std::string option_guid(std::string_view modid, std::string_view fragment, int index)
    {
        const std::string key = std::format("{}|{}|{}", lowercase(modid), lowercase(fragment), index);
        const std::uint64_t high = fnv1a(key, 0xCBF29CE484222325ULL);
        const std::uint64_t low = fnv1a(key, 0x84222325CBF29CE4ULL);
        return std::format(
            "{:08x}-{:04x}-{:04x}-{:04x}-{:012x}",
            static_cast<std::uint32_t>(high >> 32U),
            static_cast<std::uint32_t>((high >> 16U) & 0xFFFFU),
            static_cast<std::uint32_t>((high & 0x0FFFU) | 0x4000U),
            static_cast<std::uint32_t>(((low >> 48U) & 0x3FFFU) | 0x8000U),
            low & 0xFFFFFFFFFFFFULL
        );
    }

    std::string mod_manifest(std::string_view modid, std::string_view created_on)
    {
        return std::format(
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<kcd_mod>\n"
            "  <info>\n"
            "    <name>{0}</name>\n"
            "    <modid>{0}</modid>\n"
            "    <description>Made with the Animation Framework's mod maker.</description>\n"
            "    <author></author>\n"
            "    <version>1.0.0</version>\n"
            "    <created_on>{1}</created_on>\n"
            "    <modifies_level>false</modifies_level>\n"
            "  </info>\n"
            "</kcd_mod>\n",
            modid,
            created_on
        );
    }
} // namespace AnimationFramework
