/**
 * @file core/legacy_merge.cpp
 * @brief Whole-file database, controller definition, tag definition, event list and table overrides to deltas (see
 *        legacy_merge.hpp).
 */

#include "core/legacy_merge.hpp"
#include "core/text_util.hpp"
#include "core/xml_elements.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace AnimationFramework
{
    namespace
    {
        using Attr = XmlAttr;
        using Element = XmlElement;

        /** @brief Appends @p text with its length in front, so no text can read as the structure around it. */
        void canon_field(std::string_view text, std::string &out)
        {
            out += std::to_string(text.size());
            out += ':';
            out += text;
        }

        /** @brief A subtree's identity for "did the mod change it": tag, attributes in name order (GUID aside). */
        void canon(const Element &e, std::string &out)
        {
            out += '(';
            canon_field(e.tag, out);
            std::vector<const Attr *> sorted;
            for (const Attr &a : e.attrs)
            {
                if (a.name != "GUID")
                {
                    sorted.push_back(&a);
                }
            }
            std::ranges::sort(sorted, [](const Attr *x, const Attr *y) { return x->name < y->name; });
            for (const Attr *a : sorted)
            {
                out += ' ';
                canon_field(a->name, out);
                canon_field(a->value, out);
            }
            for (const Element &c : e.children)
            {
                canon(c, out);
            }
            out += ')';
        }

        /// The canon of every subtree of @p list, in order.
        [[nodiscard]] std::string canon_of(const std::vector<const Element *> &list)
        {
            std::string out;
            for (const Element *e : list)
            {
                canon(*e, out);
            }
            return out;
        }

        /** @brief Elements grouped by key, keys in first-seen order. */
        struct Grouped
        {
            std::vector<std::string> order;
            std::map<std::string, std::vector<const Element *>> items;

            /// The group of @p key, created empty (and placed last in order) when new.
            std::vector<const Element *> &slot(const std::string &key)
            {
                auto [it, inserted] = items.try_emplace(key);
                if (inserted)
                {
                    order.push_back(key);
                }
                return it->second;
            }
        };

        /** @brief A fragment's options by their Tags and FragTags tag sets. */
        [[nodiscard]] Grouped options_of(const Element *fragment)
        {
            Grouped groups;
            if (fragment != nullptr)
            {
                for (const Element &opt : fragment->children)
                {
                    if (opt.tag == "Fragment")
                    {
                        groups.slot(tag_set(opt.get("Tags")) + "|" + tag_set(opt.get("FragTags"))).push_back(&opt);
                    }
                }
            }
            return groups;
        }

        /**
         * @brief Each child of @p list by its lowercased tag, the first one winning: the merge matches fragment names
         *        in a FragmentList or FragmentDefs case-insensitively.
         */
        [[nodiscard]] std::map<std::string, const Element *> by_fragment_name(const Element *list)
        {
            std::map<std::string, const Element *> out;
            if (list != nullptr)
            {
                for (const Element &e : list->children)
                {
                    out.try_emplace(lowercase(e.tag), &e);
                }
            }
            return out;
        }

        /** @brief Variants by from/to tag sets. A repeated key keeps its first place and its last element. */
        [[nodiscard]] Grouped variants_of(const Element *blend)
        {
            Grouped groups;
            if (blend != nullptr)
            {
                for (const Element &v : blend->children)
                {
                    if (v.tag == "Variant")
                    {
                        groups.slot(tag_set(v.get("from")) + "|" + tag_set(v.get("to"))).assign(1, &v);
                    }
                }
            }
            return groups;
        }
    } // namespace

    std::optional<AdbDelta> make_adb_delta(std::string_view base_text, std::string_view mod_text)
    {
        std::optional<Element> base = read_xml_elements(base_text);
        std::optional<Element> mod = read_xml_elements(mod_text);
        if (!base || !mod || base->tag != "AnimDB" || mod->tag != "AnimDB")
        {
            return std::nullopt;
        }
        AdbDelta delta;
        Element out{
            .tag = "AnimDB",
            .attrs = mod->attrs,
            .children = {},
        };

        Element list{
            .tag = "FragmentList",
            .attrs = {},
            .children = {},
        };
        const std::map<std::string, const Element *> base_frags = by_fragment_name(base->child("FragmentList"));
        if (const Element *mod_list = mod->child("FragmentList"))
        {
            for (const Element &mod_frag : mod_list->children)
            {
                const auto stock_frag = base_frags.find(lowercase(mod_frag.tag));
                const Grouped base_opts = options_of(stock_frag != base_frags.end() ? stock_frag->second : nullptr);
                const Grouped mod_opts = options_of(&mod_frag);
                Element frag{
                    .tag = mod_frag.tag,
                    .attrs = {},
                    .children = {},
                };
                for (const std::string &key : mod_opts.order)
                {
                    const std::vector<const Element *> &opts = mod_opts.items.at(key);
                    const auto stock = base_opts.items.find(key);
                    if (stock != base_opts.items.end() && canon_of(stock->second) == canon_of(opts))
                    {
                        continue;
                    }
                    for (const Element *opt : opts)
                    {
                        frag.children.push_back(*opt);
                        ++delta.options;
                    }
                }
                for (const std::string &key : base_opts.order)
                {
                    if (mod_opts.items.contains(key))
                    {
                        continue;
                    }
                    const Element *first = base_opts.items.at(key).front();
                    Element gone{
                        .tag = "Fragment",
                        .attrs = {},
                        .children = {},
                    };
                    gone.set("Tags", first->get("Tags"));
                    gone.set("Mode", "remove");
                    if (const std::string *frag_tags = first->attr("FragTags"); frag_tags && !frag_tags->empty())
                    {
                        gone.set("FragTags", *frag_tags);
                    }
                    frag.children.push_back(std::move(gone));
                    ++delta.removed_options;
                }
                if (!frag.children.empty())
                {
                    list.children.push_back(std::move(frag));
                }
            }
        }
        if (!list.children.empty())
        {
            out.children.push_back(std::move(list));
        }

        Element blends{
            .tag = "FragmentBlendList",
            .attrs = {},
            .children = {},
        };
        std::map<std::pair<std::string, std::string>, const Element *> base_blends;
        if (const Element *base_blend_list = base->child("FragmentBlendList"))
        {
            for (const Element &b : base_blend_list->children)
            {
                base_blends.try_emplace({lowercase(b.get("from")), lowercase(b.get("to"))}, &b);
            }
        }
        if (const Element *mod_blend_list = mod->child("FragmentBlendList"))
        {
            for (const Element &mod_blend : mod_blend_list->children)
            {
                const auto found = base_blends.find({lowercase(mod_blend.get("from")), lowercase(mod_blend.get("to"))});
                const Grouped base_vars = variants_of(found != base_blends.end() ? found->second : nullptr);
                const Grouped mod_vars = variants_of(&mod_blend);
                Element blend{
                    .tag = mod_blend.tag,
                    .attrs = mod_blend.attrs,
                    .children = {},
                };
                for (const std::string &key : mod_vars.order)
                {
                    const Element *var = mod_vars.items.at(key).front();
                    const auto stock = base_vars.items.find(key);
                    if (stock != base_vars.items.end() && canon_of(stock->second) == canon_of({var}))
                    {
                        continue;
                    }
                    blend.children.push_back(*var);
                    ++delta.variants;
                }
                for (const std::string &key : base_vars.order)
                {
                    if (mod_vars.items.contains(key))
                    {
                        continue;
                    }
                    const Element *var = base_vars.items.at(key).front();
                    Element gone{
                        .tag = "Variant",
                        .attrs = {},
                        .children = {},
                    };
                    gone.set("from", var->get("from"));
                    gone.set("to", var->get("to"));
                    gone.set("Mode", "remove");
                    blend.children.push_back(std::move(gone));
                    ++delta.removed_variants;
                }
                if (!blend.children.empty())
                {
                    blends.children.push_back(std::move(blend));
                }
            }
        }
        if (!blends.children.empty())
        {
            out.children.push_back(std::move(blends));
        }

        std::set<std::string> base_subs;
        if (const Element *subs = base->child("SubADBs"))
        {
            for (const Element &s : subs->children)
            {
                base_subs.insert(lowercase(s.get("File")));
            }
        }
        Element subs_out{
            .tag = "SubADBs",
            .attrs = {},
            .children = {},
        };
        if (const Element *subs = mod->child("SubADBs"))
        {
            for (const Element &s : subs->children)
            {
                if (!base_subs.contains(lowercase(s.get("File"))))
                {
                    subs_out.children.push_back(s);
                    ++delta.sub_adbs;
                }
            }
        }
        if (!subs_out.children.empty())
        {
            out.children.push_back(std::move(subs_out));
        }
        write_xml_elements(out, delta.text);
        return delta;
    }

    namespace
    {
        /// An Override's key: its tags and fragTags as tag sets.
        [[nodiscard]] std::string override_key(const Element &e)
        {
            return tag_set(e.get("tags")) + "|" + tag_set(e.get("fragTags"));
        }

        /** @brief Each child of @p list by its tag (the first one wins), or by @p attr when given. */
        [[nodiscard]] std::map<std::string, const Element *> by_name(const Element *list, std::string_view attr = {})
        {
            std::map<std::string, const Element *> out;
            if (list != nullptr)
            {
                for (const Element &e : list->children)
                {
                    out.try_emplace(attr.empty() ? e.tag : lowercase(e.get(attr)), &e);
                }
            }
            return out;
        }

        /// The canon of @p e alone.
        [[nodiscard]] std::string canon_one(const Element &e)
        {
            std::string out;
            canon(e, out);
            return out;
        }

        /** @brief One fragment definition's changes: its scopes/flags (exact, Mode="replace") and its Overrides. */
        [[nodiscard]] Element fragment_def_delta(const Element &mod, const Element *base, ControllerDefsDelta &delta)
        {
            Element out{
                .tag = mod.tag,
                .attrs = {},
                .children = {},
            };
            if (base == nullptr)
            {
                ++delta.fragment_defs;
                delta.overrides += static_cast<int>(mod.children.size());
                out.attrs = mod.attrs;
                out.children = mod.children;
                return out;
            }
            if (mod.get("scopes") != base->get("scopes") || mod.get("flags") != base->get("flags"))
            {
                ++delta.fragment_defs;
                out.set("scopes", mod.get("scopes"));
                out.set("flags", mod.get("flags"));
                out.set("Mode", "replace");
            }
            std::map<std::string, const Element *> base_overrides;
            for (const Element &o : base->children)
            {
                base_overrides.try_emplace(override_key(o), &o);
            }
            std::set<std::string> seen;
            for (const Element &o : mod.children)
            {
                const std::string key = override_key(o);
                if (!seen.insert(key).second)
                {
                    continue;
                }
                const auto stock = base_overrides.find(key);
                if (stock == base_overrides.end() || canon_one(*stock->second) != canon_one(o))
                {
                    out.children.push_back(o);
                    ++delta.overrides;
                }
            }
            for (const auto &[key, o] : base_overrides)
            {
                if (seen.contains(key))
                {
                    continue;
                }
                Element gone{
                    .tag = o->tag,
                    .attrs = {},
                    .children = {},
                };
                gone.set("tags", o->get("tags"));
                if (const std::string *frag_tags = o->attr("fragTags"))
                {
                    gone.set("fragTags", *frag_tags);
                }
                gone.set("Mode", "remove");
                out.children.push_back(std::move(gone));
                ++delta.removed_overrides;
            }
            return out;
        }
    } // namespace

    std::optional<ControllerDefsDelta> make_controller_defs_delta(std::string_view base_text, std::string_view mod_text)
    {
        std::optional<Element> base = read_xml_elements(base_text);
        std::optional<Element> mod = read_xml_elements(mod_text);
        if (!base || !mod || base->tag != "ControllerDef" || mod->tag != "ControllerDef")
        {
            return std::nullopt;
        }
        ControllerDefsDelta delta;
        Element out{
            .tag = "ControllerDef",
            .attrs = mod->attrs,
            .children = {},
        };

        Element defs{
            .tag = "FragmentDefs",
            .attrs = {},
            .children = {},
        };
        const std::map<std::string, const Element *> base_defs = by_fragment_name(base->child("FragmentDefs"));
        if (const Element *mod_defs = mod->child("FragmentDefs"))
        {
            for (const Element &def : mod_defs->children)
            {
                const auto stock = base_defs.find(lowercase(def.tag));
                Element changed = fragment_def_delta(def, stock != base_defs.end() ? stock->second : nullptr, delta);
                if (!changed.attrs.empty() || !changed.children.empty())
                {
                    defs.children.push_back(std::move(changed));
                }
            }
        }
        if (!defs.children.empty())
        {
            out.children.push_back(std::move(defs));
        }

        // Scope definitions are carried whole when added or changed. Contexts and imports are carried only when added.
        const auto carry = [&](std::string_view section, std::string_view key_attr, bool compare, int &count) -> void
        {
            const std::map<std::string, const Element *> stock = by_name(base->child(section), key_attr);
            Element list{
                .tag = std::string{section},
                .attrs = {},
                .children = {},
            };
            if (const Element *mod_list = mod->child(section))
            {
                for (const Element &e : mod_list->children)
                {
                    const auto found = stock.find(key_attr.empty() ? e.tag : lowercase(e.get(key_attr)));
                    if (found == stock.end() || (compare && canon_one(*found->second) != canon_one(e)))
                    {
                        list.children.push_back(e);
                        ++count;
                    }
                }
            }
            if (!list.children.empty())
            {
                out.children.push_back(std::move(list));
            }
        };
        carry("ScopeDefs", {}, true, delta.scopes);
        carry("ScopeContextDefs", {}, false, delta.scope_contexts);
        carry("FragmentDefsImports", "filename", false, delta.imports);
        write_xml_elements(out, delta.text);
        return delta;
    }

    std::string anim_event_key(std::string_view clip_path)
    {
        std::string key = fold_path(clip_path);
        if (key.starts_with("./"))
        {
            key.erase(0, 2);
        }
        return key;
    }

    std::optional<AnimEventsDelta> make_anim_events_delta(std::string_view base_text, std::string_view mod_text)
    {
        std::optional<Element> base = read_xml_elements(base_text);
        std::optional<Element> mod = read_xml_elements(mod_text);
        if (!base || !mod || base->tag != "anim_event_list" || mod->tag != "anim_event_list")
        {
            return std::nullopt;
        }
        // The engine reads the first non-empty list per clip (AnimEventLoader), so that is each file's list.
        const auto lists = [](const Element &root) -> Grouped
        {
            Grouped out;
            for (const Element &a : root.children)
            {
                if (a.tag != "animation")
                {
                    continue;
                }
                std::vector<const Element *> &list = out.slot(anim_event_key(a.get("name")));
                if (list.empty() || (list.front()->children.empty() && !a.children.empty()))
                {
                    list.assign(1, &a);
                }
            }
            return out;
        };
        const Grouped base_lists = lists(*base);
        const Grouped mod_lists = lists(*mod);
        AnimEventsDelta delta;
        Element out{
            .tag = "anim_event_list",
            .attrs = {},
            .children = {},
        };
        for (const std::string &key : mod_lists.order)
        {
            const Element *list = mod_lists.items.at(key).front();
            const auto stock = base_lists.items.find(key);
            if (stock != base_lists.items.end() && canon_one(*stock->second.front()) == canon_one(*list))
            {
                continue;
            }
            out.children.push_back(*list);
            ++delta.animations;
        }
        for (const std::string &key : base_lists.order)
        {
            if (mod_lists.items.contains(key) || base_lists.items.at(key).front()->children.empty())
            {
                continue;
            }
            Element gone{
                .tag = "animation",
                .attrs = {},
                .children = {},
            };
            gone.set("name", base_lists.items.at(key).front()->get("name"));
            gone.set("Mode", "remove");
            out.children.push_back(std::move(gone));
            ++delta.removed;
        }
        write_xml_elements(out, delta.text);
        return delta;
    }

    namespace
    {
        /** @brief The tags an added Tag or Group brings: one for a tag, every tag inside for a group. */
        [[nodiscard]] int tags_in(const Element &e)
        {
            if (e.tag == "Tag")
            {
                return 1;
            }
            int count = 0;
            for (const Element &c : e.children)
            {
                count += tags_in(c);
            }
            return count;
        }

        /** @brief @p e with its name lowercased, as the tag merge matches names. */
        [[nodiscard]] Element with_lowercase_name(const Element &e)
        {
            Element out = e;
            for (Attr &a : out.attrs)
            {
                if (a.name == "name")
                {
                    a.value = lowercase(a.value);
                }
            }
            return out;
        }

        /** @brief The child of @p list of kind @p kind named @p name (case-insensitive), or nullptr. */
        [[nodiscard]] const Element *named_child(const Element *list, std::string_view kind, const std::string &name)
        {
            if (list != nullptr)
            {
                for (const Element &e : list->children)
                {
                    if (e.tag == kind && lowercase(e.get("name")) == name)
                    {
                        return &e;
                    }
                }
            }
            return nullptr;
        }

        /**
         * @brief Appends to @p out the tags and groups of @p mod that the stock level @p base lacks, merging groups
         *        both have level by level.
         */
        void tag_level_delta(const Element *base, const Element &mod, Element &out, TagDefinitionDelta &delta)
        {
            std::set<std::string> seen;
            for (const Element &e : mod.children)
            {
                if (e.tag != "Tag" && e.tag != "Group")
                {
                    continue;
                }
                const std::string name = lowercase(e.get("name"));
                seen.insert(e.tag + "|" + name);
                const Element *stock = named_child(base, e.tag, name);
                if (stock == nullptr)
                {
                    out.children.push_back(e);
                    delta.tags += tags_in(e);
                }
                else if (e.tag == "Group")
                {
                    Element group{
                        .tag = "Group",
                        .attrs = {},
                        .children = {},
                    };
                    group.set("name", e.get("name"));
                    tag_level_delta(stock, e, group, delta);
                    if (!group.children.empty())
                    {
                        out.children.push_back(std::move(group));
                    }
                }
                else if (canon_one(with_lowercase_name(*stock)) != canon_one(with_lowercase_name(e)))
                {
                    ++delta.not_carried;
                }
            }
            if (base != nullptr)
            {
                for (const Element &e : base->children)
                {
                    if ((e.tag == "Tag" || e.tag == "Group") && !seen.contains(e.tag + "|" + lowercase(e.get("name"))))
                    {
                        delta.not_carried += e.tag == "Tag" ? 1 : std::max(tags_in(e), 1);
                    }
                }
            }
        }
    } // namespace

    std::optional<TagDefinitionDelta> make_tag_definition_delta(std::string_view base_text, std::string_view mod_text)
    {
        std::optional<Element> base = read_xml_elements(base_text);
        std::optional<Element> mod = read_xml_elements(mod_text);
        if (!base || !mod || base->tag != "TagDefinition" || mod->tag != "TagDefinition")
        {
            return std::nullopt;
        }
        TagDefinitionDelta delta;
        Element out{
            .tag = "TagDefinition",
            .attrs = mod->attrs,
            .children = {},
        };
        if (const Element *mod_imports = mod->child("Imports"))
        {
            const std::map<std::string, const Element *> stock = by_name(base->child("Imports"), "filename");
            Element imports{
                .tag = "Imports",
                .attrs = {},
                .children = {},
            };
            for (const Element &e : mod_imports->children)
            {
                if (e.tag == "Import" && !stock.contains(lowercase(e.get("filename"))))
                {
                    imports.children.push_back(e);
                    ++delta.imports;
                }
            }
            if (!imports.children.empty())
            {
                out.children.push_back(std::move(imports));
            }
        }
        if (const Element *mod_tags = mod->child("Tags"))
        {
            Element tags{
                .tag = "Tags",
                .attrs = {},
                .children = {},
            };
            tag_level_delta(base->child("Tags"), *mod_tags, tags, delta);
            if (!tags.children.empty())
            {
                out.children.push_back(std::move(tags));
            }
        }
        write_xml_elements(out, delta.text);
        return delta;
    }

    namespace
    {
        /** @brief A table row's identity (see TableRowsDelta). */
        [[nodiscard]] std::string table_row_key(const Element &row)
        {
            std::string guid = row.get("fragment_guid");
            if (guid.empty())
            {
                guid = row.get("mn_fragment_guid");
            }
            return row.get("actor_class_hash") + "|" + lowercase(row.get("mn_fragment_id")) + "|" +
                   row.get("mn_option_index") + "|" + lowercase(guid);
        }
    } // namespace

    std::optional<TableRowsDelta> make_table_rows_delta(std::string_view base_text, std::string_view mod_text)
    {
        std::optional<Element> base = read_xml_elements(base_text);
        std::optional<Element> mod = read_xml_elements(mod_text);
        if (!base || !mod || base->tag != "database" || mod->tag != "database")
        {
            return std::nullopt;
        }
        TableRowsDelta delta;
        Element out{
            .tag = "database",
            .attrs = mod->attrs,
            .children = {},
        };
        for (const Element &mod_list : mod->children)
        {
            const Element *base_list = base->child(mod_list.tag);
            std::map<std::string, const Element *> base_rows;
            if (base_list != nullptr)
            {
                for (const Element &row : base_list->children)
                {
                    base_rows.try_emplace(table_row_key(row), &row);
                }
            }
            Element list{
                .tag = mod_list.tag,
                .attrs = mod_list.attrs,
                .children = {},
            };
            std::set<std::string> seen;
            for (const Element &row : mod_list.children)
            {
                const std::string key = table_row_key(row);
                if (!seen.insert(key).second)
                {
                    continue;
                }
                const auto stock = base_rows.find(key);
                if (stock == base_rows.end())
                {
                    list.children.push_back(row);
                    ++delta.added;
                }
                else if (canon_one(*stock->second) != canon_one(row))
                {
                    list.children.push_back(row);
                    ++delta.changed;
                }
            }
            for (const auto &[key, row] : base_rows)
            {
                if (!seen.contains(key))
                {
                    ++delta.dropped;
                }
            }
            if (!list.children.empty())
            {
                out.children.push_back(std::move(list));
            }
        }
        write_xml_elements(out, delta.text);
        return delta;
    }

} // namespace AnimationFramework
