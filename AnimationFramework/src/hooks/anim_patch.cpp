/**
 * @file hooks/anim_patch.cpp
 * @brief Merges mods' delta .adb files and clips into what the game loads (see anim_patch.hpp).
 *
 * Every database file (a first load, mn_reload, each SubADB) is read through CXmlUtils::LoadXmlFromFile, which the
 * game asks for an XmlParserReadOnly tree, whose CXMLReadOnlyNode ignores every child edit (its addChild to
 * replaceChild slots are empty). For a patchable file with patches (is_patchable) the detour asks for the mutable
 * CXmlNode tree instead, merges the patches into it through its virtuals and returns it, so the engine builds the
 * database from the merged document exactly as from a stock file. Any other file loads untouched. These slots are the
 * same on the Steam 1.5.6, GOG 1.5 and Game Pass 1.4 builds:
 *   - CXmlNode: 4 Release, 6 getTag, 33 getAttr(key) (the value, "" when absent), 38 addChild, 39 newChild,
 *     40 removeChild, 41 insertChild, 43 removeAllChilds, 44 getChildCount, 45 getChild, 46 findChild, 78 setAttr(key,
 *     value) (the first-declared setAttr overload, last of the group). A returned XmlNodeRef comes back through a
 *     hidden out-pointer holding one reference.
 *   - CXmlUtils: 1 LoadXmlFromFile(out, name, patch, clean, read-only, reuse), the hooked function, and 2
 *     LoadXmlFromBuffer(out, buffer, size, read-only, reuse) with a 32-bit size. The read-only flag selects
 *     XmlParserReadOnly. Patches load on the CXmlUtils the detour receives, a file through the trampoline (so the
 *     detour never sees its own loads), never on one fetched from CSystem, whose GetXmlUtils slot is 132 on Steam and
 *     131 on GOG and Game Pass.
 *   - CCryPak: 6 OpenPack(binding root, path, flags, data, full path out), 33 GetPakInfo, 34 FreePakInfo, and
 *     FindFirst/FindNext/FindClose at byte offsets 0x1F8/0x200/0x208 (the "*.xml" folder search at Steam
 *     0x18060CAC0), with _finddata64i32_t's name at +0x24.
 */

#include "hooks/anim_patch.hpp"

#include "console_vars.hpp"
#include "constants.hpp"
#include "core/caf_reader.hpp"
#include "core/chr_merge.hpp"
#include "core/clip_check.hpp"
#include "core/clip_cache.hpp"
#include "core/clip_header.hpp"
#include "core/legacy_merge.hpp"
#include "core/mod_maker.hpp"
#include "core/text_util.hpp"
#include "core/zip_reader.hpp"
#include "core/zip_writer.hpp"
#include "custom_tags.hpp"
#include "game_memory.hpp"
#include "hooks/hook_install.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace AnimationFramework
{
    namespace
    {
        constexpr std::size_t XML_UTILS_LOAD_BUFFER_SLOT = 2;
        constexpr std::size_t CRYPAK_FIND_FIRST_SLOT = 0x1F8 / 8;
        constexpr std::size_t CRYPAK_FIND_NEXT_SLOT = 0x200 / 8;
        constexpr std::size_t CRYPAK_FIND_CLOSE_SLOT = 0x208 / 8;
        constexpr std::ptrdiff_t FIND_DATA_NAME_OFFSET = 0x24;
        constexpr std::size_t FIND_DATA_SIZE = 0x130;

        constexpr std::size_t XML_NODE_RELEASE_SLOT = 4;
        constexpr std::size_t XML_NODE_GET_TAG_SLOT = 6;
        constexpr std::size_t XML_NODE_GET_ATTR_SLOT = 33;
        constexpr std::size_t XML_NODE_ADD_CHILD_SLOT = 38;
        constexpr std::size_t XML_NODE_NEW_CHILD_SLOT = 39;
        constexpr std::size_t XML_NODE_REMOVE_CHILD_SLOT = 40;
        constexpr std::size_t XML_NODE_INSERT_CHILD_SLOT = 41;
        constexpr std::size_t XML_NODE_REMOVE_ALL_CHILDREN_SLOT = 43;
        constexpr std::size_t XML_NODE_CHILD_COUNT_SLOT = 44;
        constexpr std::size_t XML_NODE_GET_CHILD_SLOT = 45;
        constexpr std::size_t XML_NODE_FIND_CHILD_SLOT = 46;
        constexpr std::size_t XML_NODE_SET_ATTR_SLOT = 78;

        using LoadXmlFileFunc = void **(__fastcall *)(void *utils,
                                                      void **out,
                                                      const char *name,
                                                      bool patch,
                                                      bool clean,
                                                      bool read_only,
                                                      bool reuse);
        std::atomic<LoadXmlFileFunc> s_load_xml_file_original{nullptr};
        // Atomic: the install stores it on the loader thread after the frame tick hook is armed, and the frame work's
        // consoles read it on the main thread. A read before the store sees 0, and every gEnv member then reads as
        // null.
        std::atomic<std::uintptr_t> s_genv{0};
        // Settings::third_person_variants: Henry's tag definition gets the thirdPerson tag as it loads. Set before the
        // XML loader hook arms, and only its detour reads it.
        bool s_add_view_tag = false;

        /** @brief One XmlNodeRef: the node and the reference it holds. */
        class XmlRef
        {
        public:
            XmlRef() noexcept = default;
            XmlRef(const XmlRef &) = delete;
            XmlRef &operator=(const XmlRef &) = delete;
            XmlRef(XmlRef &&other) noexcept : m_node(std::exchange(other.m_node, nullptr)) {}
            XmlRef &operator=(XmlRef &&other) noexcept
            {
                if (this != &other)
                {
                    reset();
                    m_node = std::exchange(other.m_node, nullptr);
                }
                return *this;
            }
            ~XmlRef() noexcept { reset(); }

            void reset() noexcept
            {
                if (m_node != nullptr)
                {
                    vcall<void>(std::exchange(m_node, nullptr), XML_NODE_RELEASE_SLOT);
                }
            }
            [[nodiscard]] void *get() const noexcept { return m_node; }
            /** @brief Hands the reference over (to a caller's XmlNodeRef out-parameter). */
            [[nodiscard]] void *release() noexcept { return std::exchange(m_node, nullptr); }
            [[nodiscard]] void **out() noexcept
            {
                reset();
                return &m_node;
            }
            explicit operator bool() const noexcept { return m_node != nullptr; }

        private:
            void *m_node = nullptr;
        };

        [[nodiscard]] std::string_view tag_of(void *node) noexcept
        {
            const char *tag = vcall<const char *>(node, XML_NODE_GET_TAG_SLOT);
            return tag != nullptr ? std::string_view{tag} : std::string_view{};
        }

        [[nodiscard]] std::string_view attr_of(void *node, const char *key) noexcept
        {
            const char *value = vcall<const char *>(node, XML_NODE_GET_ATTR_SLOT, key);
            return value != nullptr ? std::string_view{value} : std::string_view{};
        }

        [[nodiscard]] int child_count(void *node) noexcept
        {
            return vcall<int>(node, XML_NODE_CHILD_COUNT_SLOT);
        }

        [[nodiscard]] XmlRef child_at(void *node, int index) noexcept
        {
            XmlRef child;
            vcall<void **>(node, XML_NODE_GET_CHILD_SLOT, child.out(), index);
            return child;
        }

        [[nodiscard]] XmlRef find_child(void *node, const char *tag) noexcept
        {
            XmlRef child;
            vcall<void **>(node, XML_NODE_FIND_CHILD_SLOT, child.out(), tag);
            return child;
        }

        [[nodiscard]] XmlRef find_or_add_child(void *node, const char *tag) noexcept
        {
            XmlRef child = find_child(node, tag);
            if (!child)
            {
                vcall<void **>(node, XML_NODE_NEW_CHILD_SLOT, child.out(), tag);
            }
            return child;
        }

        void insert_child(void *parent, int index, void *child) noexcept
        {
            void *ref = child;
            vcall<void>(parent, XML_NODE_INSERT_CHILD_SLOT, index, &ref);
        }

        void add_child(void *parent, void *child) noexcept
        {
            void *ref = child;
            vcall<void>(parent, XML_NODE_ADD_CHILD_SLOT, &ref);
        }

        void remove_child(void *parent, void *child) noexcept
        {
            void *ref = child;
            vcall<void>(parent, XML_NODE_REMOVE_CHILD_SLOT, &ref);
        }

        [[nodiscard]] std::string option_key(void *fragment)
        {
            return tag_set(attr_of(fragment, "Tags")) + "|" + tag_set(attr_of(fragment, "FragTags"));
        }

        [[nodiscard]] std::string variant_key(void *variant)
        {
            return tag_set(attr_of(variant, "from")) + ">" + tag_set(attr_of(variant, "to"));
        }

        [[nodiscard]] std::string blend_key(void *blend)
        {
            return lowercase(attr_of(blend, "from")) + ">" + lowercase(attr_of(blend, "to"));
        }

        /** @brief What one patch did to one database. */
        struct MergeCounts
        {
            int replaced = 0;
            int appended = 0;
            int removed = 0;
            int added = 0;
            int blends = 0;
            int sub_adbs = 0;
            /// Controller definitions: Override entries set.
            int overrides = 0;
            /// Controller definitions: scope definitions added or changed.
            int scopes = 0;
            /// The keys the patch set (fragment|tags|fragtags, blend|variant), for the conflict report.
            std::vector<std::string> touched;
            /// The chain_warning text of each option that holds its fragment, logged with the patch's name.
            std::vector<std::string> warnings;
        };

        /** @brief Detaches every child of @p parent (in order), so they can move into the stock tree. */
        [[nodiscard]] std::vector<XmlRef> take_children(void *parent)
        {
            std::vector<XmlRef> children;
            const int count = child_count(parent);
            children.reserve(static_cast<std::size_t>(std::max(count, 0)));
            for (int i = 0; i < count; ++i)
            {
                children.push_back(child_at(parent, i));
            }
            // removeAllChilds (slot 43) in one call: removing one at a time costs a search per child.
            if (!children.empty())
            {
                vcall<void>(parent, XML_NODE_REMOVE_ALL_CHILDREN_SLOT);
            }
            return children;
        }

        /** @brief The named clips in an option's first AnimLayer, and whether the last of them loops. */
        struct LayerClips
        {
            int count = 0;
            bool last_loops = false;
        };

        [[nodiscard]] LayerClips first_layer_clips(void *option)
        {
            LayerClips clips;
            XmlRef layer = find_child(option, "AnimLayer");
            if (!layer)
            {
                return clips;
            }
            for (int k = 0; k < child_count(layer.get()); ++k)
            {
                XmlRef child = child_at(layer.get(), k);
                if (tag_of(child.get()) == "Animation" && !attr_of(child.get(), "name").empty())
                {
                    ++clips.count;
                    clips.last_loops = lowercase(attr_of(child.get(), "flags")).find("loop") != std::string::npos;
                }
            }
            return clips;
        }

        /**
         * @brief A warning for a patch option that chains several clips in a fragment whose game options all loop, or
         *        empty. Mannequin holds a fragment until its last clip starts, so a tag change for that fragment (a
         *        crouch, an item draw) waits for the whole chain. A looping option changes at once.
         */
        [[nodiscard]] std::string chain_warning(std::string_view fragment, bool fragment_loops, void *option)
        {
            if (!fragment_loops)
            {
                return {};
            }
            const LayerClips clips = first_layer_clips(option);
            if (clips.count < 2)
            {
                return {};
            }
            return std::string{fragment} + " option '" + std::string{attr_of(option, "Tags")} + "' chains " +
                   std::to_string(clips.count) +
                   " clips where the game's options loop: the game holds the fragment until the last clip starts, so "
                   "a change such as crouching waits for the whole chain. Bake the chain into one looping clip.";
        }

        /**
         * @brief Merges @p patch_fragment's options into @p stock_fragment by option key (tag sets of Tags and
         *        FragTags). A key the patch sets replaces every stock option with that key, in place of the first one.
         *        Mode="append" adds after the last one, Mode="remove" drops them, and a new key goes at the end.
         * @details One pass: both lists are detached once, each key computed once, and the merged list re-attached in
         *          order. A database fragment can hold thousands of options, and per-option inserts and removes cost
         *          seconds.
         */
        void merge_options(std::string_view fragment, void *stock_fragment, void *patch_fragment, MergeCounts &counts)
        {
            struct Group
            {
                std::string mode;
                std::vector<XmlRef> options;
            };
            std::vector<std::string> order;
            std::map<std::string, Group> groups;
            for (XmlRef &option : take_children(patch_fragment))
            {
                const std::string key = option_key(option.get());
                auto [it, inserted] = groups.try_emplace(key);
                if (inserted)
                {
                    order.push_back(key);
                    it->second.mode = lowercase(attr_of(option.get(), "Mode"));
                    // An append keeps what earlier patches put under the key, so it overrides nobody.
                    if (it->second.mode != "append")
                    {
                        counts.touched.push_back(std::string{fragment} + "|" + key);
                    }
                }
                it->second.options.push_back(std::move(option));
            }

            std::vector<std::pair<std::string, XmlRef>> stock;
            for (XmlRef &option : take_children(stock_fragment))
            {
                std::string key = option_key(option.get());
                stock.emplace_back(std::move(key), std::move(option));
            }
            std::map<std::string, std::size_t> last;
            for (std::size_t i = 0; i < stock.size(); ++i)
            {
                last[stock[i].first] = i;
            }
            int stock_layers = 0;
            int stock_loops = 0;
            for (const auto &[key, option] : stock)
            {
                const LayerClips clips = first_layer_clips(option.get());
                stock_layers += clips.count > 0 ? 1 : 0;
                stock_loops += clips.count > 0 && clips.last_loops ? 1 : 0;
            }
            const bool fragment_loops = stock_layers > 0 && stock_loops == stock_layers;
            for (const std::string &key : order)
            {
                for (const XmlRef &option : groups.at(key).options)
                {
                    if (std::string warning = chain_warning(fragment, fragment_loops, option.get()); !warning.empty())
                    {
                        counts.warnings.push_back(std::move(warning));
                    }
                }
            }

            std::vector<void *> merged;
            merged.reserve(stock.size() + order.size());
            std::map<std::string, bool> placed;
            const auto place = [&merged](Group &group)
            {
                for (XmlRef &option : group.options)
                {
                    merged.push_back(option.get());
                }
            };
            for (std::size_t i = 0; i < stock.size(); ++i)
            {
                auto &[key, option] = stock[i];
                const auto group = groups.find(key);
                if (group == groups.end())
                {
                    merged.push_back(option.get());
                    continue;
                }
                if (group->second.mode == "append")
                {
                    merged.push_back(option.get());
                    if (last[key] == i)
                    {
                        place(group->second);
                        counts.appended += static_cast<int>(group->second.options.size());
                        placed[key] = true;
                    }
                    continue;
                }
                if (group->second.mode == "remove")
                {
                    ++counts.removed;
                }
                if (!placed[key])
                {
                    placed[key] = true;
                    if (group->second.mode != "remove")
                    {
                        place(group->second);
                        counts.replaced += static_cast<int>(group->second.options.size());
                    }
                }
            }
            for (const std::string &key : order)
            {
                Group &group = groups.at(key);
                if (placed[key] || group.mode == "remove")
                {
                    continue;
                }
                place(group);
                (group.mode == "append" ? counts.appended : counts.added) += static_cast<int>(group.options.size());
            }
            for (void *option : merged)
            {
                add_child(stock_fragment, option);
            }
        }

        /**
         * @brief A list's children by key, read once. A patch can add thousands of entries (an animation mod's
         *        fragment IDs, definitions and fragments), and a walk of the list's children through the node's
         *        virtuals for each one is quadratic.
         * @details The first child with a key wins, as a walk from the start finds it. The merges only append to a
         *          list (appended()), so an index stays a child's position.
         */
        class ChildIndex
        {
        public:
            template <typename Key> ChildIndex(void *list, Key key) : m_count(child_count(list))
            {
                m_positions.reserve(static_cast<std::size_t>(m_count));
                for (int k = 0; k < m_count; ++k)
                {
                    XmlRef child = child_at(list, k);
                    m_positions.try_emplace(key(child.get()), k);
                }
            }

            /** @brief The position of the first child with @p key, or nothing. */
            [[nodiscard]] std::optional<int> find(const std::string &key) const
            {
                const auto it = m_positions.find(key);
                return it != m_positions.end() ? std::optional{it->second} : std::nullopt;
            }

            /** @brief Records a child added at the end of the list. */
            void appended(std::string key) { m_positions.try_emplace(std::move(key), m_count++); }

        private:
            std::unordered_map<std::string, int> m_positions;
            int m_count = 0;
        };

        /** @brief A node's tag as the game's XML compares tags (case-insensitive), the key of a by-name list. */
        [[nodiscard]] std::string tag_key(void *node)
        {
            return lowercase(tag_of(node));
        }

        void merge_fragment_list(void *stock_list, void *patch_list, MergeCounts &counts)
        {
            ChildIndex index{stock_list, tag_key};
            for (XmlRef &patch_fragment : take_children(patch_list))
            {
                const std::string name{tag_of(patch_fragment.get())};
                const std::optional<int> at = index.find(lowercase(name));
                XmlRef stock_fragment = at.has_value() ? child_at(stock_list, *at) : XmlRef{};
                if (!stock_fragment)
                {
                    add_child(stock_list, patch_fragment.get());
                    index.appended(lowercase(name));
                    counts.added += child_count(patch_fragment.get());
                    counts.touched.push_back(name + "|*");
                    continue;
                }
                merge_options(name, stock_fragment.get(), patch_fragment.get(), counts);
            }
        }

        void merge_blend_list(void *stock_list, void *patch_list, MergeCounts &counts)
        {
            for (XmlRef &patch_blend : take_children(patch_list))
            {
                const std::string key = blend_key(patch_blend.get());
                XmlRef stock_blend;
                for (int k = 0; k < child_count(stock_list) && !stock_blend; ++k)
                {
                    XmlRef candidate = child_at(stock_list, k);
                    if (blend_key(candidate.get()) == key)
                    {
                        stock_blend = std::move(candidate);
                    }
                }
                if (!stock_blend)
                {
                    add_child(stock_list, patch_blend.get());
                    counts.blends += child_count(patch_blend.get());
                    continue;
                }
                for (XmlRef &patch_variant : take_children(patch_blend.get()))
                {
                    const std::string variant = variant_key(patch_variant.get());
                    counts.touched.push_back(key + "|" + variant);
                    int at = -1;
                    for (int k = child_count(stock_blend.get()) - 1; k >= 0; --k)
                    {
                        XmlRef candidate = child_at(stock_blend.get(), k);
                        if (variant_key(candidate.get()) == variant)
                        {
                            remove_child(stock_blend.get(), candidate.get());
                            at = k;
                        }
                    }
                    if (lowercase(attr_of(patch_variant.get(), "Mode")) == "remove")
                    {
                        counts.removed += at >= 0 ? 1 : 0;
                        continue;
                    }
                    if (at >= 0)
                    {
                        insert_child(stock_blend.get(), at, patch_variant.get());
                    }
                    else
                    {
                        add_child(stock_blend.get(), patch_variant.get());
                    }
                    ++counts.blends;
                }
            }
        }

        void merge_sub_adbs(void *stock_list, void *patch_list, MergeCounts &counts)
        {
            for (XmlRef &patch_sub : take_children(patch_list))
            {
                const std::string file = lowercase(attr_of(patch_sub.get(), "File"));
                bool present = false;
                for (int k = 0; k < child_count(stock_list) && !present; ++k)
                {
                    XmlRef candidate = child_at(stock_list, k);
                    present = lowercase(attr_of(candidate.get(), "File")) == file;
                }
                if (!present)
                {
                    add_child(stock_list, patch_sub.get());
                    ++counts.sub_adbs;
                }
            }
        }

        [[nodiscard]] MergeCounts merge_patch(void *stock_root, void *patch_root)
        {
            MergeCounts counts;
            if (XmlRef patch_list = find_child(patch_root, "FragmentList"))
            {
                XmlRef stock_list = find_or_add_child(stock_root, "FragmentList");
                merge_fragment_list(stock_list.get(), patch_list.get(), counts);
            }
            if (XmlRef patch_list = find_child(patch_root, "FragmentBlendList"))
            {
                XmlRef stock_list = find_or_add_child(stock_root, "FragmentBlendList");
                merge_blend_list(stock_list.get(), patch_list.get(), counts);
            }
            if (XmlRef patch_list = find_child(patch_root, "SubADBs"))
            {
                XmlRef stock_list = find_or_add_child(stock_root, "SubADBs");
                merge_sub_adbs(stock_list.get(), patch_list.get(), counts);
            }
            return counts;
        }

        /** @brief A TagDefinition entry's key: its kind and its lowered name (an Import's file name). */
        [[nodiscard]] std::string tag_entry_key(void *node)
        {
            const std::string kind{tag_of(node)};
            return kind + "|" + lowercase(attr_of(node, kind == "Import" ? "filename" : "name"));
        }

        /**
         * @brief Merges a TagDefinition patch (tags or fragment IDs): an `<Import>` the stock file lacks is added, a
         *        `<Tag>` is appended unless one of that name exists at its level, and a `<Group>` merges into the stock
         *        group of the same name or is appended whole. Nothing is removed or reordered, so every stock tag keeps
         *        its index. Returns the tags (and imports) added.
         */
        [[nodiscard]] int merge_tags(void *stock_list, void *patch_list, std::vector<std::string> &touched)
        {
            int added = 0;
            ChildIndex index{stock_list, tag_entry_key};
            for (XmlRef &patch_tag : take_children(patch_list))
            {
                const std::string kind{tag_of(patch_tag.get())};
                std::string key = tag_entry_key(patch_tag.get());
                const std::optional<int> at = index.find(key);
                XmlRef stock_tag = at.has_value() ? child_at(stock_list, *at) : XmlRef{};
                touched.push_back(key);
                if (!stock_tag)
                {
                    add_child(stock_list, patch_tag.get());
                    index.appended(std::move(key));
                    ++added;
                }
                else if (kind == "Group")
                {
                    added += merge_tags(stock_tag.get(), patch_tag.get(), touched);
                }
            }
            return added;
        }

        [[nodiscard]] MergeCounts merge_tag_definition(void *stock_root, void *patch_root)
        {
            MergeCounts counts;
            for (const char *section : {"Imports", "Tags"})
            {
                if (XmlRef patch_list = find_child(patch_root, section))
                {
                    XmlRef stock_list = find_or_add_child(stock_root, section);
                    counts.added += merge_tags(stock_list.get(), patch_list.get(), counts.touched);
                }
            }
            return counts;
        }

        void set_attr(void *node, const char *key, const std::string &value) noexcept
        {
            vcall<void>(node, XML_NODE_SET_ATTR_SLOT, key, value.c_str());
        }

        [[nodiscard]] std::string override_key(void *override_node)
        {
            return tag_set(attr_of(override_node, "tags")) + "|" + tag_set(attr_of(override_node, "fragTags"));
        }

        /**
         * @brief Merges one patch fragment definition into the stock one of the same name (core/legacy_merge.hpp
         *        ControllerDefsDelta): scopes/flags when stated (exactly with Mode="replace"), each Override by its
         *        tags/fragTags key in place of the stock one, or removed with Mode="remove".
         */
        void merge_fragment_def(void *stock_def, void *patch_def, std::string_view name, MergeCounts &counts)
        {
            const bool replace = lowercase(attr_of(patch_def, "Mode")) == "replace";
            bool changed = false;
            for (const char *key : {"scopes", "flags"})
            {
                const std::string value{attr_of(patch_def, key)};
                if ((replace || !value.empty()) && value != attr_of(stock_def, key))
                {
                    set_attr(stock_def, key, value);
                    changed = true;
                }
            }
            if (changed)
            {
                ++counts.replaced;
                counts.touched.push_back("FragmentDefs|" + std::string{name});
            }
            for (XmlRef &patch_override : take_children(patch_def))
            {
                const std::string key = override_key(patch_override.get());
                counts.touched.push_back(std::string{name} + "|" + key);
                int at = -1;
                for (int k = child_count(stock_def) - 1; k >= 0; --k)
                {
                    XmlRef candidate = child_at(stock_def, k);
                    if (override_key(candidate.get()) == key)
                    {
                        remove_child(stock_def, candidate.get());
                        at = k;
                    }
                }
                if (lowercase(attr_of(patch_override.get(), "Mode")) == "remove")
                {
                    counts.removed += at >= 0 ? 1 : 0;
                    continue;
                }
                if (at >= 0)
                {
                    insert_child(stock_def, at, patch_override.get());
                }
                else
                {
                    add_child(stock_def, patch_override.get());
                }
                ++counts.overrides;
            }
        }

        /**
         * @brief Merges a controller definition patch (`*_controllerdefs.xml`): fragment definitions by name (a new
         *        one is added whole), scope definitions by name (a stated attribute replaces the stock one), and scope
         *        contexts and imports the stock file lacks.
         * @details A move of a stock scope's layers applies with a warning: it shifts what every option on that scope
         *          drives.
         */
        [[nodiscard]] MergeCounts merge_controller_defs(std::string_view file, void *stock_root, void *patch_root)
        {
            MergeCounts counts;
            if (XmlRef patch_list = find_child(patch_root, "FragmentDefs"))
            {
                XmlRef stock_list = find_or_add_child(stock_root, "FragmentDefs");
                ChildIndex index{stock_list.get(), tag_key};
                for (XmlRef &patch_def : take_children(patch_list.get()))
                {
                    const std::string name{tag_of(patch_def.get())};
                    const std::optional<int> at = index.find(lowercase(name));
                    XmlRef stock_def = at.has_value() ? child_at(stock_list.get(), *at) : XmlRef{};
                    if (!stock_def)
                    {
                        counts.overrides += child_count(patch_def.get());
                        add_child(stock_list.get(), patch_def.get());
                        index.appended(lowercase(name));
                        ++counts.added;
                        counts.touched.push_back("FragmentDefs|" + name);
                        continue;
                    }
                    merge_fragment_def(stock_def.get(), patch_def.get(), name, counts);
                }
            }
            if (XmlRef patch_list = find_child(patch_root, "ScopeDefs"))
            {
                XmlRef stock_list = find_or_add_child(stock_root, "ScopeDefs");
                for (XmlRef &patch_scope : take_children(patch_list.get()))
                {
                    const std::string name{tag_of(patch_scope.get())};
                    counts.touched.push_back("ScopeDefs|" + name);
                    XmlRef stock_scope = find_child(stock_list.get(), name.c_str());
                    if (!stock_scope)
                    {
                        add_child(stock_list.get(), patch_scope.get());
                        ++counts.scopes;
                        continue;
                    }
                    bool changed = false;
                    for (const char *key : {"layer", "numLayers", "context", "Tags"})
                    {
                        const std::string value{attr_of(patch_scope.get(), key)};
                        const std::string stock{attr_of(stock_scope.get(), key)};
                        if (value.empty() || value == stock)
                        {
                            continue;
                        }
                        if (std::string_view{key} == "layer" || std::string_view{key} == "numLayers")
                        {
                            DMK::log().warning(
                                "AnimPatch: {}: scope {} {} {} -> {}; every option on it now drives other layers",
                                file,
                                name,
                                key,
                                stock,
                                value
                            );
                        }
                        set_attr(stock_scope.get(), key, value);
                        changed = true;
                    }
                    counts.scopes += changed ? 1 : 0;
                }
            }
            const auto add_missing = [&](const char *section, const char *key_attr)
            {
                XmlRef patch_list = find_child(patch_root, section);
                if (!patch_list)
                {
                    return;
                }
                XmlRef stock_list = find_or_add_child(stock_root, section);
                for (XmlRef &patch_entry : take_children(patch_list.get()))
                {
                    const auto key_of = [key_attr](void *node)
                    { return key_attr != nullptr ? lowercase(attr_of(node, key_attr)) : std::string{tag_of(node)}; };
                    const std::string key = key_of(patch_entry.get());
                    bool present = false;
                    for (int k = 0; k < child_count(stock_list.get()) && !present; ++k)
                    {
                        XmlRef candidate = child_at(stock_list.get(), k);
                        present = key_of(candidate.get()) == key;
                    }
                    if (!present)
                    {
                        add_child(stock_list.get(), patch_entry.get());
                        ++counts.added;
                        counts.touched.push_back(std::string{section} + "|" + key);
                    }
                }
            };
            add_missing("ScopeContextDefs", nullptr);
            add_missing("FragmentDefsImports", "filename");
            return counts;
        }

        /**
         * @brief Merges an animation event list patch (`*.animevents`): each `<animation>` by its clip path replaces
         *        the stock list for that clip (one element per clip stays: the engine reads the first non-empty one),
         *        Mode="append" adds its events to the stock list, Mode="remove" drops it, a new clip goes at the end.
         * @details One pass over the stock children (the male list has about 15,000), like merge_options.
         */
        [[nodiscard]] MergeCounts merge_anim_events(void *stock_root, void *patch_root)
        {
            MergeCounts counts;
            struct Entry
            {
                std::string mode;
                XmlRef node;
                bool placed = false;
            };
            std::vector<std::string> order;
            std::map<std::string, Entry> patches;
            for (XmlRef &patch_anim : take_children(patch_root))
            {
                if (tag_of(patch_anim.get()) != "animation")
                {
                    continue;
                }
                const std::string key = anim_event_key(attr_of(patch_anim.get(), "name"));
                const std::string mode = lowercase(attr_of(patch_anim.get(), "Mode"));
                auto [it, inserted] = patches.try_emplace(key);
                if (inserted)
                {
                    order.push_back(key);
                    // An append keeps the list earlier patches set for the clip, so it overrides nobody.
                    if (mode != "append")
                    {
                        counts.touched.push_back("animation|" + key);
                    }
                }
                it->second.mode = mode;
                it->second.node = std::move(patch_anim); // a later duplicate in one patch wins
            }
            if (patches.empty())
            {
                return counts;
            }
            std::vector<XmlRef> kept;
            for (XmlRef &stock_anim : take_children(stock_root))
            {
                const auto it = tag_of(stock_anim.get()) == "animation"
                                    ? patches.find(anim_event_key(attr_of(stock_anim.get(), "name")))
                                    : patches.end();
                if (it == patches.end())
                {
                    kept.push_back(std::move(stock_anim));
                    continue;
                }
                Entry &entry = it->second;
                if (entry.placed)
                {
                    continue; // a stock duplicate of a clip the patch already set
                }
                entry.placed = true;
                if (entry.mode == "remove")
                {
                    ++counts.removed;
                }
                else if (entry.mode == "append")
                {
                    for (XmlRef &event : take_children(entry.node.get()))
                    {
                        add_child(stock_anim.get(), event.get());
                    }
                    ++counts.appended;
                    kept.push_back(std::move(stock_anim));
                }
                else
                {
                    ++counts.replaced;
                    kept.push_back(std::move(entry.node));
                }
            }
            for (const std::string &key : order)
            {
                Entry &entry = patches.at(key);
                if (!entry.placed && entry.mode != "remove")
                {
                    ++counts.added;
                    kept.push_back(std::move(entry.node));
                }
            }
            for (XmlRef &node : kept)
            {
                add_child(stock_root, node.get());
            }
            return counts;
        }

        /**
         * @brief The animation action tables (Libs/Tables): one row per actor class, fragment, option position and
         *        option GUID, baked data the game's action system needs before it plays a fragment.
         * @details Neither declares a primary key, so the game's own `<table>__<mod>.xml` patches fail on them
         *          ("tables can't be loaded"). The framework merges rows into the XML the table reader loads (through
         *          the same LoadXmlFile) instead.
         */
        constexpr std::array<std::string_view, 2> ROW_TABLES{
            "libs/tables/animation/anim_fragment.xml",
            "libs/tables/action/actor_anim_action.xml",
        };
        /**
         * @brief Where mods put row patches (`<table>__<modid>.xml`): a folder the game's table loader never scans, so
         *        a game without the framework ignores them instead of failing.
         */
        constexpr std::string_view ROW_PATCH_DIR = "Animations/Mannequin/Tables/";

        [[nodiscard]] bool is_row_table(std::string_view filename) noexcept
        {
            for (const std::string_view table : ROW_TABLES)
            {
                if (path_ends_with(filename, table))
                {
                    return true;
                }
            }
            return false;
        }

        /** @brief A row's identity: actor class, fragment, option position and option GUID (unique in both tables). */
        [[nodiscard]] std::string row_key(void *row)
        {
            std::string_view guid = attr_of(row, "fragment_guid");
            if (guid.empty())
            {
                guid = attr_of(row, "mn_fragment_guid");
            }
            return std::string{attr_of(row, "actor_class_hash")} + "|" + lowercase(attr_of(row, "mn_fragment_id")) +
                   "|" + std::string{attr_of(row, "mn_option_index")} + "|" + lowercase(guid);
        }

        /**
         * @brief Merges a row patch into an animation action table: each row of the patch's row list (the root's child
         *        of the same name as the table's, such as `<anim_fragments>`) replaces the table's row with the same
         *        identity in place, or is appended.
         */
        [[nodiscard]] MergeCounts merge_table_rows(void *stock_root, void *patch_root)
        {
            MergeCounts counts;
            for (XmlRef &patch_list : take_children(patch_root))
            {
                const std::string list_tag{tag_of(patch_list.get())};
                XmlRef stock_list = find_child(stock_root, list_tag.c_str());
                if (!stock_list)
                {
                    continue;
                }
                std::map<std::string, int> stock_rows;
                const int count = child_count(stock_list.get());
                for (int i = 0; i < count; ++i)
                {
                    XmlRef row = child_at(stock_list.get(), i);
                    stock_rows.try_emplace(row_key(row.get()), i);
                }
                for (XmlRef &row : take_children(patch_list.get()))
                {
                    const std::string key = row_key(row.get());
                    counts.touched.push_back(list_tag + "|" + key);
                    if (const auto it = stock_rows.find(key); it != stock_rows.end())
                    {
                        XmlRef stock_row = child_at(stock_list.get(), it->second);
                        remove_child(stock_list.get(), stock_row.get());
                        insert_child(stock_list.get(), it->second, row.get());
                        ++counts.replaced;
                    }
                    else
                    {
                        stock_rows.emplace(key, child_count(stock_list.get()));
                        add_child(stock_list.get(), row.get());
                        ++counts.added;
                    }
                }
            }
            return counts;
        }

        /**
         * @brief A file from the game's paks as a mutable CXmlNode tree, read by the engine's loader through the
         *        hook's trampoline, so the read never re-enters detour_load_xml_file.
         */
        [[nodiscard]] XmlRef load_pak_xml(void *utils, const std::string &path) noexcept
        {
            XmlRef root;
            if (const LoadXmlFileFunc original = s_load_xml_file_original.load(std::memory_order_acquire))
            {
                original(utils, root.out(), path.c_str(), false, true, false, false);
            }
            return root;
        }

        /** @brief @p text parsed as a mutable CXmlNode tree. */
        [[nodiscard]] XmlRef load_buffer_xml(void *utils, const std::string &text) noexcept
        {
            XmlRef root;
            vcall<void **>(
                utils,
                XML_UTILS_LOAD_BUFFER_SLOT,
                root.out(),
                text.data(),
                static_cast<unsigned>(text.size()),
                false,
                false
            );
            return root;
        }

        [[nodiscard]] std::uintptr_t genv_member(std::ptrdiff_t offset) noexcept
        {
            return read_ptr(s_genv.load(std::memory_order_acquire) + offset);
        }

        /** @brief The patch names CryPak finds for @p pattern ("dir/stem__*.adb"), from every mounted pak. */
        [[nodiscard]] std::vector<std::string> find_pak_files(const std::string &pattern)
        {
            std::vector<std::string> names;
            void *cry_pak = reinterpret_cast<void *>(genv_member(constants::GENV_CRYPAK_OFFSET));
            if (cry_pak == nullptr)
            {
                return names;
            }
            alignas(8) std::array<std::byte, FIND_DATA_SIZE> data{};
            const auto handle =
                vcall<std::intptr_t>(cry_pak, CRYPAK_FIND_FIRST_SLOT, pattern.c_str(), data.data(), 0u, false);
            if (handle == -1)
            {
                return names;
            }
            try
            {
                do
                {
                    const char *name = reinterpret_cast<const char *>(data.data() + FIND_DATA_NAME_OFFSET);
                    names.emplace_back(name, strnlen(name, FIND_DATA_SIZE - FIND_DATA_NAME_OFFSET));
                } while (vcall<int>(cry_pak, CRYPAK_FIND_NEXT_SLOT, handle, data.data()) == 0);
            }
            catch (...)
            {
                vcall<int>(cry_pak, CRYPAK_FIND_CLOSE_SLOT, handle);
                throw;
            }
            vcall<int>(cry_pak, CRYPAK_FIND_CLOSE_SLOT, handle);
            std::ranges::sort(names);
            return names;
        }

        // CCryPak slot 33 GetPakInfo() copies the mounted pak list (std::vector<PackDesc> at +0x120/+0x128, in mount
        // order) under CryPak's own read lock into one malloc'd block: the count as a uint32 at +0, then one 0x20-byte
        // entry per pak from +8, each with a copy of the pak's full path at +0 and of its binding root at +8. Slot 34
        // FreePakInfo(info) frees the copies and the block. Steam 0x18241AFA4 / 0x182419E14, GOG 0x18240B794 /
        // 0x18240A604 and Game Pass 0x1824B57D0 / 0x1824B4154 build it the same way. The path itself sits at
        // zip + *(zip+0x70) + 0x9A8 on Steam and GOG but at zip + *(zip+0x58) + 0x990 on Game Pass, so the framework
        // never reads the ZipDir::Cache directly. Paks mount in mod order and CryPak searches the newest first, so the
        // mount order of `mods\<modid>\` paks is the load order a later patch wins in.
        constexpr std::size_t CRYPAK_GET_PAK_INFO_SLOT = 33;
        constexpr std::size_t CRYPAK_FREE_PAK_INFO_SLOT = 34;
        constexpr std::ptrdiff_t PAK_INFO_ENTRIES_OFFSET = 8;
        constexpr std::size_t PAK_INFO_ENTRY_STRIDE = 0x20;

        /** @brief The mounted paks' paths in mount order, lowercased with backslashes. */
        [[nodiscard]] std::vector<std::string> mounted_pak_paths()
        {
            std::vector<std::string> paths;
            void *cry_pak = reinterpret_cast<void *>(genv_member(constants::GENV_CRYPAK_OFFSET));
            void *info = cry_pak != nullptr ? vcall<void *>(cry_pak, CRYPAK_GET_PAK_INFO_SLOT) : nullptr;
            if (info == nullptr)
            {
                return paths;
            }
            const auto *bytes = static_cast<const std::byte *>(info);
            std::uint32_t count = 0;
            std::memcpy(&count, bytes, sizeof(count));
            try
            {
                for (std::uint32_t i = 0; i < count; ++i)
                {
                    const char *text = nullptr;
                    std::memcpy(&text, bytes + PAK_INFO_ENTRIES_OFFSET + i * PAK_INFO_ENTRY_STRIDE, sizeof(text));
                    std::string path = text != nullptr ? lowercase(text) : std::string{};
                    std::ranges::replace(path, '/', '\\');
                    if (!path.empty())
                    {
                        paths.push_back(std::move(path));
                    }
                }
            }
            catch (...)
            {
                vcall<void>(cry_pak, CRYPAK_FREE_PAK_INFO_SLOT, info);
                throw;
            }
            vcall<void>(cry_pak, CRYPAK_FREE_PAK_INFO_SLOT, info);
            return paths;
        }

        /** @brief The folder of the game executable, `<root>\Bin\<platform>`. */
        [[nodiscard]] std::filesystem::path game_bin_dir()
        {
            std::array<wchar_t, MAX_PATH> exe{};
            const DWORD length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
            return std::filesystem::path{std::wstring_view{exe.data(), length}}.parent_path();
        }

        /** @brief `<root>` for `<root>\Bin\<platform>\KingdomCome.exe`, the base of CryPak's relative pak paths. */
        [[nodiscard]] std::filesystem::path game_root()
        {
            return game_bin_dir().parent_path().parent_path();
        }

        /**
         * @brief A mod pak's mod id (pak_mod_id, relative to the game root), or empty for any other pak.
         * @details A root the narrow code page cannot spell compares as no root, so only CryPak's relative mod paths
         *          (the form it mounts mods with) count then.
         */
        [[nodiscard]] std::string pak_modid(std::string_view path)
        {
            std::string root;
            try
            {
                root = game_root().string();
            }
            catch (const std::system_error &)
            {
                root.clear();
            }
            return pak_mod_id(path, root);
        }

        /** @brief The mod ids of the mounted mod paks, in mount (load) order, each once. */
        [[nodiscard]] std::vector<std::string> mod_load_order()
        {
            std::vector<std::string> order;
            for (const std::string &path : mounted_pak_paths())
            {
                std::string modid = pak_modid(path);
                if (!modid.empty() && std::ranges::find(order, modid) == order.end())
                {
                    order.push_back(std::move(modid));
                }
            }
            return order;
        }

        /** @brief A patch's mod id: the part of its stem after `__`. */
        [[nodiscard]] std::string patch_modid(std::string_view name)
        {
            const std::size_t mark = name.find("__");
            const std::size_t dot = name.find_last_of('.');
            return mark == std::string_view::npos
                       ? std::string{}
                       : lowercase(name.substr(mark + 2, (dot > mark ? dot : name.size()) - mark - 2));
        }

        /** @brief The DEV folder of loose patches and test paks: `<game bin>/KCD2_AnimationFramework_Dev`. */
        [[nodiscard]] std::filesystem::path dev_patch_dir()
        {
            return game_bin_dir() / constants::DEV_PAK_DIR;
        }

        /**
         * @brief The regular files directly in @p folder, or none when the folder cannot be listed.
         * @details The walk takes error codes at every step, so a listing failure ends it instead of throwing.
         */
        [[nodiscard]] std::vector<std::filesystem::path> files_in(const std::filesystem::path &folder)
        {
            std::vector<std::filesystem::path> files;
            std::error_code error;
            for (std::filesystem::directory_iterator it{folder, error};
                 !error && it != std::filesystem::directory_iterator{};
                 it.increment(error))
            {
                std::error_code file_error;
                if (it->is_regular_file(file_error))
                {
                    files.push_back(it->path());
                }
            }
            return files;
        }

        /**
         * @brief The DEV folder's loose patches whose lowercase file name starts with @p prefix and ends with
         *        @p extension, sorted by path.
         */
        [[nodiscard]] std::vector<std::filesystem::path>
        dev_loose_patches(std::string_view prefix, std::string_view extension)
        {
            std::vector<std::filesystem::path> loose;
            for (std::filesystem::path &file : files_in(dev_patch_dir()))
            {
                const std::string name = lowercase(file.filename().string());
                if (name.starts_with(prefix) && name.ends_with(extension))
                {
                    loose.push_back(std::move(file));
                }
            }
            std::ranges::sort(loose);
            return loose;
        }

        /**
         * @brief Names, sizes and write times of the DEV folder's event list patches (`*.animevents`), sorted, as one
         *        string that changes whenever one of them is added, edited or removed.
         */
        [[nodiscard]] std::string dev_event_patch_state()
        {
            std::vector<std::string> files;
            for (const std::filesystem::path &file : files_in(dev_patch_dir()))
            {
                if (lowercase(file.extension().string()) != ".animevents")
                {
                    continue;
                }
                std::error_code error;
                files.push_back(
                    std::format(
                        "{}|{}|{}",
                        lowercase(file.filename().string()),
                        std::filesystem::file_size(file, error),
                        std::filesystem::last_write_time(file, error).time_since_epoch().count()
                    )
                );
            }
            std::ranges::sort(files);
            std::string state;
            for (const std::string &file : files)
            {
                state += file;
                state += ';';
            }
            return state;
        }

        // The DEV folder's event list patches as the game last merged them (dev_event_patch_state), or nothing before
        // the first merge. The game reads event lists again only when it reloads an animation set, so a clip reload
        // that finds them changed reloads the sets. Written on the loading thread, read on the main thread.
        std::mutex s_merged_event_patches_mutex;
        std::optional<std::string> s_merged_event_patches;

        void note_event_patches_merged(std::string state) noexcept
        {
            const std::scoped_lock lock{s_merged_event_patches_mutex};
            s_merged_event_patches = std::move(state);
        }

        /** @brief True, once, when the DEV folder's event list patches differ from the ones last merged. */
        [[nodiscard]] bool take_event_patch_change()
        {
            std::string state = dev_event_patch_state();
            const std::scoped_lock lock{s_merged_event_patches_mutex};
            const bool changed = s_merged_event_patches.has_value() ? *s_merged_event_patches != state : !state.empty();
            if (changed)
            {
                s_merged_event_patches = std::move(state);
            }
            return changed;
        }

        void log_merge(std::string_view database, std::string_view patch, const MergeCounts &counts)
        {
            DMK::log().info(
                "AnimPatch: {} <- {}: options replaced {}, appended {}, removed {}, added {}; blend "
                "variants {}; sub-ADBs {}",
                database,
                patch,
                counts.replaced,
                counts.appended,
                counts.removed,
                counts.added,
                counts.blends,
                counts.sub_adbs
            );
            for (const std::string &warning : counts.warnings)
            {
                DMK::log().warning("AnimPatch: {} <- {}: {}", database, patch, warning);
            }
        }

        /** @brief One patch document and the name it is logged by. */
        struct Patch
        {
            std::string name;
            XmlRef root;
        };

        /** @brief Every tag name in a TagDefinition's `<Tags>` (groups' tags included), lowered. */
        void collect_tag_names(void *list, std::vector<std::string> &names)
        {
            for (int k = 0; k < child_count(list); ++k)
            {
                XmlRef child = child_at(list, k);
                const std::string_view kind = tag_of(child.get());
                if (kind == "Tag")
                {
                    names.push_back(lowercase(attr_of(child.get(), "name")));
                }
                else if (kind == "Group")
                {
                    collect_tag_names(child.get(), names);
                }
            }
        }

        /** @brief An integer attribute of @p node, or @p fallback (with @p bad set) when it is not a whole number. */
        [[nodiscard]] std::int32_t int_attr(void *node, const char *key, std::int32_t fallback, bool &bad) noexcept
        {
            const std::string_view text = attr_of(node, key);
            if (text.empty())
            {
                return fallback;
            }
            std::int32_t value = fallback;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size())
            {
                bad = true;
                return fallback;
            }
            return value;
        }

        /**
         * @brief Reads the custom tags the mods' patches to Henry's tag definition declare (custom_tags.hpp) and
         *        publishes them. Runs before the patches merge, against the game's own tags in @p stock_root.
         */
        void declare_mod_custom_tags(void *stock_root, const std::vector<Patch> &patches)
        {
            std::vector<CustomTagDeclaration> declared;
            if (mod_tags_enabled())
            {
                std::vector<std::string> game_tags;
                if (XmlRef stock_tags = find_child(stock_root, "Tags"))
                {
                    collect_tag_names(stock_tags.get(), game_tags);
                }
                for (const Patch &entry : patches)
                {
                    XmlRef tags = find_child(entry.root.get(), "Tags");
                    if (!tags)
                    {
                        continue;
                    }
                    for (int k = 0; k < child_count(tags.get()); ++k)
                    {
                        XmlRef tag = child_at(tags.get(), k);
                        const std::string_view kind = tag_of(tag.get());
                        if (kind == "Group")
                        {
                            for (int j = 0; j < child_count(tag.get()); ++j)
                            {
                                XmlRef member = child_at(tag.get(), j);
                                if (!attr_of(member.get(), constants::CUSTOM_TAG_CVAR_ATTR).empty())
                                {
                                    DMK::log().warning(
                                        "CustomTags: {} declares '{}' inside group '{}'; a custom tag must be a plain "
                                        "tag outside any group, so it is ignored",
                                        entry.name,
                                        attr_of(member.get(), "name"),
                                        attr_of(tag.get(), "name")
                                    );
                                }
                            }
                            continue;
                        }
                        const std::string_view cvar = attr_of(tag.get(), constants::CUSTOM_TAG_CVAR_ATTR);
                        if (kind != "Tag" || cvar.empty())
                        {
                            continue;
                        }
                        const std::string name{attr_of(tag.get(), "name")};
                        const std::string lowered = lowercase(name);
                        if (name.empty() || lowered == lowercase(constants::THIRD_PERSON_TAG) ||
                            std::ranges::find(game_tags, lowered) != game_tags.end())
                        {
                            DMK::log().warning(
                                "CustomTags: {} declares '{}', which the game or the framework already defines; "
                                "ignored",
                                entry.name,
                                name
                            );
                            continue;
                        }
                        bool bad = false;
                        CustomTagDeclaration declaration{
                            .name = name,
                            .cvar = std::string{cvar},
                            .min_value = int_attr(tag.get(), constants::CUSTOM_TAG_MIN_ATTR, 1, bad),
                            .priority = int_attr(tag.get(), constants::CUSTOM_TAG_PRIORITY_ATTR, 0, bad),
                            .source = entry.name,
                        };
                        if (bad)
                        {
                            DMK::log().warning(
                                "CustomTags: {} gives '{}' a {} or {} that is not a whole number; the default is used",
                                entry.name,
                                name,
                                constants::CUSTOM_TAG_MIN_ATTR,
                                constants::CUSTOM_TAG_PRIORITY_ATTR
                            );
                        }
                        declared.push_back(std::move(declaration));
                    }
                }
            }
            declare_custom_tags(declared);
        }

        /**
         * @brief Every patch for @p filename: the game's paks in mod load order, then the DEV folder's loose files in
         *        name order, each parsed on @p utils.
         */
        [[nodiscard]] std::vector<Patch> find_patches(void *utils, std::string_view filename)
        {
            std::vector<Patch> patches;
            const std::size_t slash = filename.find_last_of("/\\");
            const std::string dir{
                is_row_table(filename)            ? ROW_PATCH_DIR
                : slash == std::string_view::npos ? std::string_view{}
                                                  : filename.substr(0, slash + 1)
            };
            std::string_view file = slash == std::string_view::npos ? filename : filename.substr(slash + 1);
            const std::size_t dot = file.find_last_of('.');
            const std::string stem{file.substr(0, dot)};
            const std::string extension{dot == std::string_view::npos ? std::string_view{} : file.substr(dot)};
            if (stem.find("__") != std::string::npos)
            {
                return patches;
            }

            std::vector<std::string> names = find_pak_files(dir + stem + "__*" + extension);
            if (names.size() > 1)
            {
                // Mod load order, so the last-loaded mod's patch merges last and wins. A mod the list lacks goes
                // after the known ones, by name.
                const std::vector<std::string> order = mod_load_order();
                const auto rank = [&order](const std::string &name)
                {
                    const auto at = std::ranges::find(order, patch_modid(name));
                    return static_cast<std::size_t>(at - order.begin());
                };
                std::ranges::stable_sort(
                    names,
                    [&rank](const std::string &a, const std::string &b) { return rank(a) < rank(b); }
                );
            }
            for (const std::string &name : names)
            {
                XmlRef patch = load_pak_xml(utils, dir + name);
                if (!patch)
                {
                    DMK::log().warning("AnimPatch: {}{} did not load", dir, name);
                    continue;
                }
                patches.push_back(Patch{.name = name, .root = std::move(patch)});
            }

            for (const std::filesystem::path &path : dev_loose_patches(lowercase(stem + "__"), lowercase(extension)))
            {
                std::ifstream in{path, std::ios::binary};
                const std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
                XmlRef patch = load_buffer_xml(utils, text);
                if (!patch)
                {
                    DMK::log().warning("AnimPatch: {} did not parse", path.string());
                    continue;
                }
                patches.push_back(Patch{.name = path.filename().string() + " (dev)", .root = std::move(patch)});
            }
            return patches;
        }

        /** @brief A whole copy of a game file that one mounted mod pak ships. */
        struct WholeCopy
        {
            std::string modid;
            std::string text;
        };

        // Each mounted pak's central directory, read once (a mounted pak does not change while the game runs).
        std::mutex s_zip_cache_mutex;
        std::map<std::string, std::shared_ptr<const std::vector<ZipEntry>>> s_zip_cache;

        [[nodiscard]] std::shared_ptr<const std::vector<ZipEntry>> zip_index_of(const std::filesystem::path &file)
        {
            const std::string key = lowercase(file.string());
            const std::lock_guard lock{s_zip_cache_mutex};
            if (const auto it = s_zip_cache.find(key); it != s_zip_cache.end())
            {
                return it->second;
            }
            std::optional<std::vector<ZipEntry>> index = read_zip_index(file);
            auto shared = index ? std::make_shared<const std::vector<ZipEntry>>(std::move(*index)) : nullptr;
            s_zip_cache.emplace(key, shared);
            return shared;
        }

        [[nodiscard]] const ZipEntry *find_entry(const std::vector<ZipEntry> &entries, std::string_view name)
        {
            const auto it = std::ranges::find_if(entries, [name](const ZipEntry &entry) { return entry.name == name; });
            return it != entries.end() ? &*it : nullptr;
        }

        /** @brief A mounted mod pak: a `mods\<modid>\...` mount, or a DEV folder pak named by its stem. */
        struct ModPak
        {
            std::string modid;
            /// The path CryPak mounted it by, lowercased with backslashes.
            std::string mounted;
            std::filesystem::path file;
        };

        /// The game's own paks that hold its animation files, by file name prefix.
        constexpr std::array<std::string_view, 1> ANIMATION_BASE_PAKS{"animations"};
        /// The game's own paks that hold its character skeletons (male.chr is in IPL_Characters-part3.pak).
        constexpr std::array<std::string_view, 2> SKELETON_BASE_PAKS{
            "characters",
            "ipl_characters",
        };
        /// Base paks the game's own tables come from (Tables.pak).
        constexpr std::array<std::string_view, 1> TABLE_BASE_PAKS{"tables"};

        /**
         * @brief The mounted mod paks in mount (load) order, and the newest last.
         * @param bases Receives the game's own base paks whose file name starts with one of @p base_prefixes, in mount
         *        order, when not null.
         */
        [[nodiscard]] std::vector<ModPak> mounted_mod_paks(
            std::vector<std::filesystem::path> *bases = nullptr,
            std::span<const std::string_view> base_prefixes = ANIMATION_BASE_PAKS
        )
        {
            const std::filesystem::path root = game_root();
            const std::string dev = lowercase(dev_patch_dir().string());
            std::vector<ModPak> mods;
            for (std::string &pak : mounted_pak_paths())
            {
                const std::filesystem::path file{pak};
                std::filesystem::path full = file.is_absolute() ? file : root / file;
                // A DEV folder pak mounts by its absolute path outside Mods, and its file name is its mod id.
                std::string modid = pak.starts_with(dev) ? lowercase(file.stem().string()) : pak_modid(pak);
                if (!modid.empty())
                {
                    mods.push_back(
                        ModPak{.modid = std::move(modid), .mounted = std::move(pak), .file = std::move(full)}
                    );
                }
                else if (bases != nullptr)
                {
                    const std::string stem = lowercase(file.filename().string());
                    if (std::ranges::any_of(
                            base_prefixes,
                            [&stem](std::string_view prefix) { return stem.starts_with(prefix); }
                        ))
                    {
                        bases->push_back(std::move(full));
                    }
                }
            }
            return mods;
        }

        /** @brief @p name (lowercase, forward slashes) from the newest of @p bases that holds it, read from the zip. */
        [[nodiscard]] std::optional<std::string>
        read_base_file(const std::vector<std::filesystem::path> &bases, std::string_view name)
        {
            for (auto it = bases.rbegin(); it != bases.rend(); ++it)
            {
                const auto index = zip_index_of(*it);
                const ZipEntry *entry = index ? find_entry(*index, name) : nullptr;
                if (entry == nullptr)
                {
                    continue;
                }
                if (std::optional<std::string> text = read_zip_file(*it, *entry))
                {
                    return text;
                }
            }
            return std::nullopt;
        }

        /** @brief Every mounted mod pak's whole copy of @p name (lowercase, forward slashes), in mount order. */
        [[nodiscard]] std::vector<WholeCopy> read_mod_copies(const std::vector<ModPak> &mods, std::string_view name)
        {
            std::vector<WholeCopy> copies;
            for (const ModPak &mod : mods)
            {
                const auto index = zip_index_of(mod.file);
                const ZipEntry *entry = index ? find_entry(*index, name) : nullptr;
                if (entry == nullptr)
                {
                    continue;
                }
                std::optional<std::string> text = read_zip_file(mod.file, *entry);
                if (!text)
                {
                    DMK::log().warning("AnimCompat: could not read {} from {}", name, mod.mounted);
                    continue;
                }
                copies.push_back(WholeCopy{mod.modid, std::move(*text)});
            }
            return copies;
        }

        /** @brief @p name as a zip entry name: lowercase, forward slashes, no leading `./`. */
        [[nodiscard]] std::string zip_entry_name(std::string_view name)
        {
            std::string path = lowercase(name);
            std::ranges::replace(path, '\\', '/');
            if (path.starts_with("./"))
            {
                path.erase(0, 2);
            }
            return path;
        }

        /**
         * @brief The game's own copy of @p name and every mounted mod pak's whole copy of it, in mount (load) order,
         *        when two or more mods ship one. CryPak serves only the newest copy, so without this the last mod
         *        wins and the others' changes are lost. One copy alone loads as it is.
         * @details The game's copy comes from the newest base pak named after one of @p base_prefixes (`animations*` by
         *          default), read straight from the zip like the mods' copies.
         */
        [[nodiscard]] std::optional<std::pair<std::string, std::vector<WholeCopy>>>
        find_whole_copies(std::string_view name, std::span<const std::string_view> base_prefixes = ANIMATION_BASE_PAKS)
        {
            const std::string path = zip_entry_name(name);
            std::vector<std::filesystem::path> bases;
            std::vector<WholeCopy> copies = read_mod_copies(mounted_mod_paks(&bases, base_prefixes), path);
            if (copies.size() < 2)
            {
                return std::nullopt;
            }
            if (std::optional<std::string> base = read_base_file(bases, path))
            {
                return std::pair{std::move(*base), std::move(copies)};
            }
            DMK::log().warning(
                "AnimCompat: {} mods ship {} whole, but the game's own copy was not found; the last one wins as usual",
                copies.size(),
                path
            );
            return std::nullopt;
        }

        /**
         * @brief Logs the keys @p patch sets that an earlier patch of the same database also set: the later one wins.
         * @param owners Each key's last patch so far, updated with @p patch's keys.
         */
        void report_conflicts(
            std::string_view database,
            const std::string &patch,
            const MergeCounts &counts,
            std::map<std::string, std::string> &owners
        )
        {
            std::map<std::string, std::vector<std::string>> lost;
            for (const std::string &key : counts.touched)
            {
                auto [owner, inserted] = owners.try_emplace(key, patch);
                if (!inserted && owner->second != patch)
                {
                    lost[owner->second].push_back(key);
                    owner->second = patch;
                }
            }
            for (const auto &[loser, keys] : lost)
            {
                DMK::log().warning(
                    "AnimPatch: {}: {} overrides {} on {} key(s), e.g. {}",
                    database,
                    patch,
                    loser,
                    keys.size(),
                    keys.front()
                );
            }
        }

        [[nodiscard]] bool is_database(std::string_view filename) noexcept
        {
            return path_ends_with(filename, ".adb");
        }

        /** @brief Henry's tag definition (kcd_male_tags.xml), which gets the thirdPerson tag. */
        [[nodiscard]] bool is_player_tag_definition(std::string_view filename) noexcept
        {
            return path_ends_with(filename, constants::PLAYER_TAG_DEFINITION);
        }

        /** @brief A Mannequin controller definition (`*_controllerdefs.xml`): fragment and scope definitions. */
        [[nodiscard]] bool is_controller_definition(std::string_view filename) noexcept
        {
            return path_ends_with(filename, "controllerdefs.xml");
        }

        /** @brief An animation event list (`*.animevents`), which the engine reads through the same XML loader. */
        [[nodiscard]] bool is_anim_events(std::string_view filename) noexcept
        {
            return path_ends_with(filename, ".animevents");
        }

        /** @brief A Mannequin tag definition: a tag list (`*tags.xml`) or a fragment ID list (`*fragmentids.xml`). */
        [[nodiscard]] bool is_tag_definition(std::string_view filename) noexcept
        {
            return path_contains(filename, "mannequin/adb/") &&
                   (path_ends_with(filename, "tags.xml") || path_ends_with(filename, "fragmentids.xml"));
        }

        /** @brief A file whose whole copies from several mods are merged (each mod's changes over the game's copy). */
        [[nodiscard]] bool merges_whole_copies(std::string_view filename) noexcept
        {
            return is_database(filename) || is_controller_definition(filename) || is_anim_events(filename) ||
                   is_tag_definition(filename) || is_row_table(filename);
        }

        /**
         * @brief A file patches may target: a Mannequin database, a tag, fragment-ID or controller definition beside
         *        one, an animation event list, or an animation action table.
         * @details A `__` name is itself a patch (no stock file of these kinds has one), so it loads as it is.
         * @note Allocation free: the XML loader detour runs it on every XML file the game reads.
         */
        [[nodiscard]] bool is_patchable(std::string_view filename) noexcept
        {
            if (filename.find("__") != std::string_view::npos)
            {
                return false;
            }
            return is_database(filename) || is_anim_events(filename) || is_row_table(filename) ||
                   (path_contains(filename, "mannequin/adb/") && path_ends_with(filename, ".xml"));
        }

        /**
         * @brief The delta text of @p copy's changes to @p name over the game's @p base, logged with what it holds.
         *        Empty when the copy changes nothing, nothing when the copy or the base does not parse.
         */
        [[nodiscard]] std::optional<std::string>
        whole_copy_delta(std::string_view name, const std::string &base, const WholeCopy &copy)
        {
            DMK::Logger &logger = DMK::log();
            if (is_controller_definition(name))
            {
                const std::optional<ControllerDefsDelta> delta = make_controller_defs_delta(base, copy.text);
                if (!delta)
                {
                    return std::nullopt;
                }
                logger.info(
                    "AnimCompat: {} from {}: {} fragment definition(s) added or changed, {} override(s) set, {} "
                    "removed, {} scope(s), {} scope context(s), {} import(s)",
                    name,
                    copy.modid,
                    delta->fragment_defs,
                    delta->overrides,
                    delta->removed_overrides,
                    delta->scopes,
                    delta->scope_contexts,
                    delta->imports
                );
                return delta->empty() ? std::string{} : delta->text;
            }
            if (is_row_table(name))
            {
                const std::optional<TableRowsDelta> delta = make_table_rows_delta(base, copy.text);
                if (!delta)
                {
                    return std::nullopt;
                }
                logger.info(
                    "AnimCompat: {} from {}: {} row(s) added, {} changed; {} row(s) it dropped are kept",
                    name,
                    copy.modid,
                    delta->added,
                    delta->changed,
                    delta->dropped
                );
                return delta->empty() ? std::string{} : delta->text;
            }
            if (is_tag_definition(name))
            {
                const std::optional<TagDefinitionDelta> delta = make_tag_definition_delta(base, copy.text);
                if (!delta)
                {
                    return std::nullopt;
                }
                logger.info(
                    "AnimCompat: {} from {}: {} tag(s) and {} import(s) added; {} tag(s) it changed or dropped are not "
                    "carried (tags are only ever added, so every tag keeps its index)",
                    name,
                    copy.modid,
                    delta->tags,
                    delta->imports,
                    delta->not_carried
                );
                return delta->empty() ? std::string{} : delta->text;
            }
            if (is_anim_events(name))
            {
                const std::optional<AnimEventsDelta> delta = make_anim_events_delta(base, copy.text);
                if (!delta)
                {
                    return std::nullopt;
                }
                logger.info(
                    "AnimCompat: {} from {}: {} clip event list(s) added or changed, {} removed",
                    name,
                    copy.modid,
                    delta->animations,
                    delta->removed
                );
                return delta->empty() ? std::string{} : delta->text;
            }
            const std::optional<AdbDelta> delta = make_adb_delta(base, copy.text);
            if (!delta)
            {
                return std::nullopt;
            }
            logger.info(
                "AnimCompat: {} from {}: {} option(s) changed, {} option set(s) removed, {} blend variant(s) changed, "
                "{} removed, {} sub-ADB(s) added",
                name,
                copy.modid,
                delta->options,
                delta->removed_options,
                delta->variants,
                delta->removed_variants,
                delta->sub_adbs
            );
            return delta->empty() ? std::string{} : delta->text;
        }

        // The skeleton merge detour is armed (see detour_chunk_file_read), so the overlap report lists shared skeletons
        // as merged. Atomic: the install sets it after the XML loader hook is live, and that detour reads it on the
        // loading thread.
        std::atomic<bool> s_skeleton_merge_on{false};

        // Once per logic generation: the mounted mods' overlapping animation files, logged at the first patchable
        // load (mod paks mount before the game reads its first Mannequin file).
        std::atomic<bool> s_overlaps_reported{false};
        constexpr std::size_t MAX_LOGGED_OVERLAPS = 40;

        /** @brief A path the overlap report covers: animation data and character skeletons/animation lists. */
        [[nodiscard]] bool is_reported_path(std::string_view path) noexcept
        {
            if (path.find("__") != std::string_view::npos)
            {
                return false;
            }
            return path.starts_with("animations/") || (path.starts_with("objects/characters/") &&
                                                       (path.ends_with(".chr") || path.ends_with(".chrparams")));
        }

        /**
         * @brief The time the framework added to the game's loads of animation files, summed until the frame work
         *        logs it. The game loads its databases as it needs them, across seconds of loading, so the sum is
         *        reported once no file has loaded for MERGE_REPORT_QUIET: one line per loading burst (startup, a save
         *        load, mn_reload).
         */
        struct MergeTimes
        {
            using Duration = std::chrono::steady_clock::duration;

            int files = 0;
            Duration total{};
            /// The slowest files, slowest first.
            std::vector<std::pair<Duration, std::string>> slowest;
            /// The first load's look through every mod pak for files two mods ship whole, part of total.
            Duration overlap_scan{};
            std::chrono::steady_clock::time_point last{};
        };

        constexpr std::chrono::seconds MERGE_REPORT_QUIET{5};
        constexpr std::size_t MERGE_REPORT_SLOWEST = 3;

        std::mutex s_merge_times_mutex;
        MergeTimes s_merge_times;
        // Read by the frame work every frame without the mutex, so a frame with nothing to report costs one load.
        std::atomic<bool> s_merge_times_pending{false};

        /** @brief Adds one patched file's load time. Any thread the game loads on. */
        void record_merge_time(std::string_view name, MergeTimes::Duration elapsed)
        {
            const std::lock_guard lock{s_merge_times_mutex};
            MergeTimes &times = s_merge_times;
            ++times.files;
            times.total += elapsed;
            times.last = std::chrono::steady_clock::now();
            auto &slowest = times.slowest;
            const auto at =
                std::ranges::find_if(slowest, [elapsed](const auto &entry) { return entry.first < elapsed; });
            slowest.emplace(at, elapsed, std::string{name});
            if (slowest.size() > MERGE_REPORT_SLOWEST)
            {
                slowest.pop_back();
            }
            s_merge_times_pending.store(true, std::memory_order_release);
        }

        [[nodiscard]] double milliseconds(MergeTimes::Duration duration) noexcept
        {
            return std::chrono::duration<double, std::milli>(duration).count();
        }

        /**
         * @brief Logs the summed load times once loading goes quiet.
         * @note Main thread, every frame. A frame that finds the mutex held by a loading thread skips the check
         *       instead of a wait.
         */
        void report_merge_times()
        {
            if (!s_merge_times_pending.load(std::memory_order_acquire))
            {
                return;
            }
            MergeTimes times;
            {
                const std::unique_lock lock{s_merge_times_mutex, std::try_to_lock};
                if (!lock.owns_lock() || std::chrono::steady_clock::now() - s_merge_times.last < MERGE_REPORT_QUIET)
                {
                    return;
                }
                times = std::exchange(s_merge_times, MergeTimes{});
                s_merge_times_pending.store(false, std::memory_order_relaxed);
            }
            std::string slowest;
            for (const auto &[elapsed, name] : times.slowest)
            {
                slowest += std::format("{}{} {:.1f} ms", slowest.empty() ? "" : ", ", name, milliseconds(elapsed));
            }
            std::string scan;
            if (times.overlap_scan != MergeTimes::Duration{})
            {
                scan = std::format(
                    ", of which {:.1f} ms reading the mod paks' file lists",
                    milliseconds(times.overlap_scan)
                );
            }
            DMK::log().info(
                "AnimPatch: {} animation file(s) patched as the game loaded them, {:.1f} ms in all{}; slowest: {}",
                times.files,
                milliseconds(times.total),
                scan,
                slowest
            );
        }

        /**
         * @brief Logs the mod load order, then every animation file that two or more mounted mods ship whole: merged
         *        (the framework merges each mod's changes) or not (CryPak serves the last-loaded mod's copy and the
         *        others' changes are lost), so a silent override becomes one log line naming both mods.
         */
        void report_overlapping_files()
        {
            std::map<std::string, std::vector<std::string>> owners;
            // Every DEV folder pak counts as one source: a mounted pak stays locked until the game exits, so each
            // iteration of a test mod is a new pak shipping the same files, and the newest is meant to win.
            const std::string dev = lowercase(dev_patch_dir().string());
            for (const ModPak &mod : mounted_mod_paks())
            {
                const auto index = zip_index_of(mod.file);
                if (!index)
                {
                    continue;
                }
                const std::string source = mod.mounted.starts_with(dev) ? std::string{"DEV folder"} : mod.modid;
                for (const ZipEntry &entry : *index)
                {
                    if (is_reported_path(entry.name))
                    {
                        std::vector<std::string> &mods = owners[entry.name];
                        if (std::ranges::find(mods, source) == mods.end())
                        {
                            mods.push_back(source);
                        }
                    }
                }
            }
            std::vector<std::pair<std::string, std::string>> merged;
            std::vector<std::pair<std::string, std::string>> lost;
            for (const auto &[path, mods] : owners)
            {
                if (mods.size() < 2)
                {
                    continue;
                }
                std::string list;
                for (const std::string &modid : mods)
                {
                    list += list.empty() ? modid : ", " + modid;
                }
                const bool merges = merges_whole_copies(path) || path == "animations/animations.img" ||
                                    (s_skeleton_merge_on.load(std::memory_order_relaxed) && path.ends_with(".chr"));
                (merges ? merged : lost).emplace_back(path, std::move(list));
            }
            DMK::Logger &logger = DMK::log();
            // Logged here, not at install: the framework installs before the game mounts the mods' paks.
            std::string order;
            for (const std::string &modid : mod_load_order())
            {
                order += order.empty() ? modid : ", " + modid;
            }
            logger.info("AnimPatch: mod load order (patches merge in it, the last wins): {}", order);
            logger.info(
                "AnimCompat: {} animation file(s) shipped whole by two or more mods: {} merged, {} last-loaded wins",
                merged.size() + lost.size(),
                merged.size(),
                lost.size()
            );
            for (std::size_t i = 0; i < merged.size() && i < MAX_LOGGED_OVERLAPS; ++i)
            {
                logger.info("AnimCompat: {} from {}: merged", merged[i].first, merged[i].second);
            }
            for (std::size_t i = 0; i < lost.size() && i < MAX_LOGGED_OVERLAPS; ++i)
            {
                logger.warning(
                    "AnimCompat: {} from {}: not merged; the last one loaded wins and the others' changes are lost",
                    lost[i].first,
                    lost[i].second
                );
            }
            if (lost.size() > MAX_LOGGED_OVERLAPS || merged.size() > MAX_LOGGED_OVERLAPS)
            {
                logger.info("AnimCompat: (only the first {} of each kind listed)", MAX_LOGGED_OVERLAPS);
            }
        }

        // Skeleton merge. Every chunk file the engine reads from disk goes through CReadOnlyChunkFile::Read. For a
        // skeleton (.chr) that two or more mounted mods ship whole, the detour builds the bone-by-bone merge once
        // (core/chr_merge.hpp) and has the same chunk file read it from memory instead.
        using ChunkFileReadFunc = bool(__fastcall *)(void *chunk_file, const char *path);
        using ChunkFileReadFromMemoryFunc = bool(__fastcall *)(void *chunk_file, const void *data, int size);
        std::atomic<ChunkFileReadFunc> s_chunk_file_read_original{nullptr};
        ChunkFileReadFromMemoryFunc s_chunk_file_read_from_memory = nullptr;

        /** @brief A merged skeleton the engine reads in place: never freed (see merged_skeleton). */
        struct MergedBytes
        {
            const char *data = nullptr;
            int size = 0;
        };

        std::mutex s_skeleton_mutex;
        /// Normalized skeleton path -> its merged bytes, or no data when it loads as it is. Built once per path.
        std::map<std::string, MergedBytes> s_skeletons;

        /** @brief A copy of @p bytes on the process heap, never freed, or no data when it cannot allocate. */
        [[nodiscard]] MergedBytes leak_to_process_heap(std::string_view bytes) noexcept
        {
            if (bytes.empty() || bytes.size() > static_cast<std::size_t>(INT_MAX))
            {
                return {};
            }
            auto *block = static_cast<char *>(HeapAlloc(GetProcessHeap(), 0, bytes.size()));
            if (block == nullptr)
            {
                return {};
            }
            std::memcpy(block, bytes.data(), bytes.size());
            return MergedBytes{.data = block, .size = static_cast<int>(bytes.size())};
        }

        /** @brief Merges the mods' whole copies of @p path (normalized) and logs the result. */
        [[nodiscard]] MergedBytes build_merged_skeleton(const std::string &path)
        {
            DMK::Logger &logger = DMK::log();
            auto whole = find_whole_copies(path, SKELETON_BASE_PAKS);
            if (!whole)
            {
                return {};
            }
            const auto &[base, copies] = *whole;
            std::vector<SkeletonCopy> inputs;
            inputs.reserve(copies.size());
            for (const WholeCopy &copy : copies)
            {
                inputs.push_back(SkeletonCopy{.source = copy.modid, .bytes = copy.text});
            }
            const auto merged = merge_skeletons(base, inputs);
            if (!merged)
            {
                logger.warning(
                    "SkeletonMerge: {}: {} mods ship it whole but it cannot merge bone by bone ({}: {}); "
                    "the copy of {} loads as it is",
                    path,
                    copies.size(),
                    merged.error().source,
                    merged.error().reason,
                    copies.back().modid
                );
                return {};
            }
            std::string summary;
            for (const SkeletonContribution &contribution : merged->contributions)
            {
                std::string bones;
                for (const std::string &bone : contribution.bones)
                {
                    bones += (bones.empty() ? "" : ", ") + bone;
                }
                summary += std::format(
                    "{}{} ({} bone(s){}{}{})",
                    summary.empty() ? "" : "; ",
                    contribution.source,
                    contribution.bones.size(),
                    bones.empty() ? "" : ": " + bones,
                    contribution.same_as_earlier != 0
                        ? std::format(", {} the same as an earlier mod", contribution.same_as_earlier)
                        : "",
                    contribution.chunks.empty() ? "" : std::format(", {} other chunk(s)", contribution.chunks.size())
                );
            }
            logger.info("SkeletonMerge: {}: merged {} mods' copies bone by bone: {}", path, copies.size(), summary);
            for (const SkeletonConflict &conflict : merged->conflicts)
            {
                logger.warning(
                    "SkeletonMerge: {}: {} overrides {} on {} (both changed it)",
                    path,
                    conflict.winner,
                    conflict.loser,
                    conflict.what
                );
            }
            const MergedBytes bytes = leak_to_process_heap(merged->bytes);
            if (bytes.data == nullptr)
            {
                logger.warning(
                    "SkeletonMerge: {}: out of memory; the copy of {} loads as it is",
                    path,
                    copies.back().modid
                );
            }
            return bytes;
        }

        /**
         * @brief The merged bytes for skeleton @p raw_path, or no data when it loads as it is (fewer than two mods ship
         *        it, or their copies cannot merge).
         * @details Built on the first read of each path and kept for the session. The bytes live on the process heap
         *          and are never freed: ReadFromMemory keeps the pointer while its chunk file lives, which a DEV
         *          reload of this module must not cut short.
         */
        [[nodiscard]] MergedBytes merged_skeleton(std::string_view raw_path) noexcept
        {
            try
            {
                std::string path = zip_entry_name(raw_path);
                const std::lock_guard lock{s_skeleton_mutex};
                if (const auto it = s_skeletons.find(path); it != s_skeletons.end())
                {
                    return it->second;
                }
                const MergedBytes bytes = build_merged_skeleton(path);
                s_skeletons.emplace(std::move(path), bytes);
                return bytes;
            }
            catch (...)
            {
                (void)DMK::log().log_noexcept(
                    DMK::LogLevel::Error,
                    "SkeletonMerge: merging a skeleton failed; the copy CryPak serves loads as it is"
                );
                return {};
            }
        }

        bool __fastcall detour_chunk_file_read(void *chunk_file, const char *path) noexcept
        {
            const ChunkFileReadFunc original = s_chunk_file_read_original.load(std::memory_order_acquire);
            if (path != nullptr && path_ends_with(path, ".chr") && s_chunk_file_read_from_memory != nullptr)
            {
                if (const MergedBytes merged = merged_skeleton(path); merged.data != nullptr)
                {
                    return s_chunk_file_read_from_memory(chunk_file, merged.data, merged.size);
                }
            }
            return original(chunk_file, path);
        }

        /** @brief One clip swap: every database fragment that plays `from` plays `to` instead. */
        struct ClipSwap
        {
            std::string from;
            std::string to;
        };

        /**
         * @brief The swaps of every `Animations/Mannequin/ADB/clipswaps__<modid>.xml` (`<ClipSwaps><Swap from to/>`),
         *        found like patches. A later file's swap of the same clip wins.
         */
        [[nodiscard]] std::vector<ClipSwap> find_clip_swaps(void *utils)
        {
            std::vector<ClipSwap> swaps;
            for (Patch &document : find_patches(utils, "Animations/Mannequin/ADB/clipswaps.xml"))
            {
                for (int i = 0; i < child_count(document.root.get()); ++i)
                {
                    XmlRef swap = child_at(document.root.get(), i);
                    const std::string from = lowercase(attr_of(swap.get(), "from"));
                    const std::string_view to = attr_of(swap.get(), "to");
                    if (!from.empty() && !to.empty())
                    {
                        swaps.push_back(ClipSwap{.from = from, .to = std::string{to}});
                    }
                }
            }
            return swaps;
        }

        /** @brief Renames every `<Animation name>` under @p node that a swap names. Returns how many. */
        [[nodiscard]] int apply_clip_swaps(void *node, const std::vector<ClipSwap> &swaps)
        {
            if (tag_of(node) == "Animation")
            {
                const std::string name = lowercase(attr_of(node, "name"));
                const auto swap = std::ranges::find(swaps | std::views::reverse, name, &ClipSwap::from);
                if (swap == (swaps | std::views::reverse).end())
                {
                    return 0;
                }
                set_attr(node, "name", swap->to);
                return 1;
            }
            int swapped = 0;
            for (int i = 0; i < child_count(node); ++i)
            {
                XmlRef child = child_at(node, i);
                swapped += apply_clip_swaps(child.get(), swaps);
            }
            return swapped;
        }

        /** @brief What one load of a patchable file merges, in merge order. */
        struct PatchPlan
        {
            /// The game's own copy, parsed mutable, when two or more mods ship the file whole: the tree to merge into.
            XmlRef whole_base;
            /// Each whole copy's delta, the framework's own tag, then the delta patches.
            std::vector<Patch> patches;
            std::vector<ClipSwap> swaps;

            [[nodiscard]] bool empty() const noexcept { return patches.empty() && swaps.empty() && !whole_base; }
        };

        /**
         * @brief Gathers what the load of @p name merges. Mods that each ship the whole file become one delta per mod
         *        over the game's copy, in load order and ahead of the delta patches. Henry's tag definition then gets
         *        the framework's own tag, ahead of every mod's delta patch. A database also gets the clip swaps.
         * @throws What the pak and DEV folder reads throw. The detour then loads the file as the game ships it.
         */
        [[nodiscard]] PatchPlan plan_patches(void *utils, const char *name)
        {
            PatchPlan plan;
            if (merges_whole_copies(name))
            {
                if (auto whole =
                        is_row_table(name) ? find_whole_copies(name, TABLE_BASE_PAKS) : find_whole_copies(name))
                {
                    plan.whole_base = load_buffer_xml(utils, whole->first);
                    std::string mod_list;
                    for (const WholeCopy &copy : whole->second)
                    {
                        mod_list += mod_list.empty() ? copy.modid : ", " + copy.modid;
                        const std::optional<std::string> delta = whole_copy_delta(name, whole->first, copy);
                        if (!delta)
                        {
                            DMK::log().warning(
                                "AnimCompat: {}'s copy of {} could not be read; it is left out",
                                copy.modid,
                                name
                            );
                            continue;
                        }
                        if (delta->empty())
                        {
                            continue;
                        }
                        if (XmlRef root = load_buffer_xml(utils, *delta))
                        {
                            plan.patches.push_back(
                                Patch{.name = copy.modid + " (whole file)", .root = std::move(root)}
                            );
                        }
                    }
                    DMK::log().info(
                        "AnimCompat: {}: {} mods ship the whole file ({}); merging what each one "
                        "changed over the game's copy",
                        name,
                        whole->second.size(),
                        mod_list
                    );
                }
            }
            if (s_add_view_tag && is_player_tag_definition(name))
            {
                const std::string view_tag = std::string{"<TagDefinition version=\"2\"><Tags><Tag name=\""} +
                                             constants::THIRD_PERSON_TAG + "\"/></Tags></TagDefinition>";
                if (XmlRef root = load_buffer_xml(utils, view_tag))
                {
                    plan.patches.push_back(Patch{.name = "AnimationFramework (built-in)", .root = std::move(root)});
                }
            }
            for (Patch &entry : find_patches(utils, name))
            {
                plan.patches.push_back(std::move(entry));
            }
            if (is_database(name))
            {
                plan.swaps = find_clip_swaps(utils);
            }
            return plan;
        }

        /**
         * @brief Merges @p entry into @p root, the loaded tree of @p name whose root element is @p root_tag, and logs
         *        what the merge did.
         */
        [[nodiscard]] MergeCounts
        merge_patch_into(std::string_view name, std::string_view root_tag, void *root, Patch &entry)
        {
            DMK::Logger &logger = DMK::log();
            if (root_tag == "TagDefinition")
            {
                MergeCounts counts = merge_tag_definition(root, entry.root.get());
                logger.info("AnimPatch: {} <- {}: {} tag(s) or import(s) added", name, entry.name, counts.added);
                return counts;
            }
            if (root_tag == "ControllerDef")
            {
                MergeCounts counts = merge_controller_defs(name, root, entry.root.get());
                logger.info(
                    "AnimPatch: {} <- {}: {} fragment definition(s), scope context(s) or import(s) added, {} "
                    "fragment definition(s) changed, {} override(s) set, {} removed, {} scope(s) set",
                    name,
                    entry.name,
                    counts.added,
                    counts.replaced,
                    counts.overrides,
                    counts.removed,
                    counts.scopes
                );
                return counts;
            }
            if (root_tag == "database" && is_row_table(name))
            {
                MergeCounts counts = merge_table_rows(root, entry.root.get());
                logger.info(
                    "AnimPatch: {} <- {}: {} row(s) added, {} replaced",
                    name,
                    entry.name,
                    counts.added,
                    counts.replaced
                );
                return counts;
            }
            if (root_tag == "anim_event_list")
            {
                MergeCounts counts = merge_anim_events(root, entry.root.get());
                logger.info(
                    "AnimPatch: {} <- {}: {} clip event list(s) replaced, {} added, {} appended to, {} removed",
                    name,
                    entry.name,
                    counts.replaced,
                    counts.added,
                    counts.appended,
                    counts.removed
                );
                return counts;
            }
            MergeCounts counts = merge_patch(root, entry.root.get());
            log_merge(name, entry.name, counts);
            return counts;
        }

        /**
         * @brief CXmlUtils::LoadXmlFromFile detour: a patchable file (database, Mannequin definition, event list,
         *        action table) with patches loads as a mutable tree with them merged in.
         * @note The thread that loads the file (a level load, a character spawn, mn_reload).
         */
        void **__fastcall detour_load_xml_file(
            void *utils,
            void **out,
            const char *name,
            bool patch,
            bool clean,
            bool read_only,
            bool reuse
        ) noexcept
        {
            const LoadXmlFileFunc original = s_load_xml_file_original.load(std::memory_order_acquire);
            if (original == nullptr)
            {
                // The caller's XmlNodeRef is uninitialized: hand back an empty reference, as a failed load does.
                *out = nullptr;
                return out;
            }
            if (name == nullptr || !is_patchable(name))
            {
                return original(utils, out, name, patch, clean, read_only, reuse);
            }
            const auto started = std::chrono::steady_clock::now();
            if (!s_overlaps_reported.exchange(true, std::memory_order_acq_rel))
            {
                // The report is a diagnostic in its own try, so its failure leaves this file's patches alone.
                try
                {
                    const auto scan_started = std::chrono::steady_clock::now();
                    report_overlapping_files();
                    const std::lock_guard lock{s_merge_times_mutex};
                    s_merge_times.overlap_scan += std::chrono::steady_clock::now() - scan_started;
                }
                catch (...)
                {
                    (void)DMK::log().log_noexcept(
                        DMK::LogLevel::Error,
                        "AnimCompat: the report of files that several mods ship failed"
                    );
                }
            }
            PatchPlan plan;
            try
            {
                plan = plan_patches(utils, name);
            }
            catch (...)
            {
                (void)DMK::log().log_noexcept(
                    DMK::LogLevel::Error,
                    "AnimPatch: reading the patches failed; the database loads as the game ships it"
                );
            }
            if (plan.empty())
            {
                return original(utils, out, name, patch, clean, read_only, reuse);
            }
            if (plan.whole_base)
            {
                *out = plan.whole_base.release();
            }
            else
            {
                original(utils, out, name, patch, clean, false, reuse);
            }
            if (*out == nullptr)
            {
                return original(utils, out, name, patch, clean, read_only, reuse);
            }
            if (is_player_tag_definition(name))
            {
                try
                {
                    declare_mod_custom_tags(*out, plan.patches);
                }
                catch (...)
                {
                    (void)DMK::log().log_noexcept(
                        DMK::LogLevel::Error,
                        "CustomTags: reading the mods' tag declarations failed; only the built-in tags count"
                    );
                }
            }
            const std::string_view root_tag = tag_of(*out);
            try
            {
                std::map<std::string, std::string> owners;
                for (Patch &entry : plan.patches)
                {
                    const MergeCounts counts = merge_patch_into(name, root_tag, *out, entry);
                    report_conflicts(name, entry.name, counts, owners);
                }
                if (!plan.swaps.empty())
                {
                    if (const int swapped = apply_clip_swaps(*out, plan.swaps); swapped != 0)
                    {
                        DMK::log().info("AnimPatch: {}: {} clip(s) swapped", name, swapped);
                    }
                }
                // Noted after the merges, so a failed read of the DEV folder costs no merge.
                if (root_tag == "anim_event_list" && !plan.patches.empty())
                {
                    note_event_patches_merged(dev_event_patch_state());
                }
                const MergeTimes::Duration elapsed = std::chrono::steady_clock::now() - started;
                DMK::log().debug("AnimPatch: {} parsed and merged in {:.1f} ms", name, milliseconds(elapsed));
                record_merge_time(name, elapsed);
            }
            catch (...)
            {
                (void)DMK::log().log_noexcept(
                    DMK::LogLevel::Error,
                    "AnimPatch: a merge failed part way; the database "
                    "loads with the patches merged so far"
                );
            }
            return out;
        }

        // Clip registration (Settings::mod_clips). The CharacterManager (gEnv+0xD0) loads animations/animations.img
        // lazily before its first model (Steam 0x18091C948): +0x8C is non-zero once it ran. Its global header table is
        // the DynArray at +0x280 (168-byte GlobalAnimationHeaderCAF, count at element pointer - 4): the CAF and facial
        // indexes, then headers the engine adds on demand past the facial total at +0x805A4, so a new index appends
        // from the table's own count, not from that total. The facial index load (Steam 0x1820F3490) shows
        // the sequence this repeats: an I3DEngine chunk file (CreateChunkFile, refcount at +8), the CAF image loader
        // from that count, then a CHRPARAMS reload of every animation set the manager lists at +0x90/+0x98 (16-byte
        // entries, set first), whose CHRPARAMS path is at set+0x258. The loader reserves 200 entries past the new
        // count, so a late call reallocates the table: animation jobs that hold a header across it can fault. Every
        // offset here reads the same on the Steam, GOG and Game Pass builds.
        constexpr std::ptrdiff_t CM_HEADERS_READY_OFFSET = 0x8C;
        constexpr std::ptrdiff_t CM_HEADER_TABLE_OFFSET = 0x280;
        // A global clip header is 0xA8 bytes, keyed by its path hash at +0x10 (clip_path_hash).
        constexpr std::size_t GLOBAL_HEADER_STRIDE = 0xA8;
        constexpr std::ptrdiff_t GLOBAL_HEADER_HASH_OFFSET = 0x10;
        constexpr std::ptrdiff_t CM_SETS_BEGIN_OFFSET = 0x90;
        constexpr std::ptrdiff_t CM_SETS_END_OFFSET = 0x98;
        constexpr std::size_t CM_SET_STRIDE = 16;
        constexpr std::ptrdiff_t SET_CHRPARAMS_OFFSET = 0x258;
        // The manager's CHRPARAMS parse cache: the CHRPARAMS reload passes `lea rcx, [rbp+3A8h]` (the manager) to the
        // parse, whose list is a DynArray (count at element pointer - 4) of 0x90-byte animation lists, each starting
        // with its file name (a CryString, so the member is its char pointer). The parse looks a name up by an exact
        // compare that steps 0x90 (Steam 0x181315D10) and reuses a list it finds.
        constexpr std::ptrdiff_t CM_PARAM_LISTS_OFFSET = 0x3A8;
        constexpr std::size_t PARAM_LIST_STRIDE = 0x90;
        // More headers than any clip index holds (the game's has about 320,000), so a stray count reads as no table.
        constexpr std::uint32_t MAX_GLOBAL_HEADERS = 0x400000;
        constexpr std::size_t MAX_ANIMATION_SETS = 0x10000;
        constexpr std::ptrdiff_t CHUNK_FILE_REFCOUNT_OFFSET = 8;
        constexpr std::size_t CHUNK_FILE_DELETING_DESTRUCTOR_SLOT = 0;
        constexpr std::size_t CRYPAK_OPEN_PACK_SLOT = 6;
        constexpr unsigned OVERLAY_PAK_FLAGS = 0x10400;

        using ClipImageLoadFunc =
            bool(__fastcall *)(void *manager, const char *path, void *chunk_file, std::uint32_t *count);
        using ChrParamsReloadFunc = bool(__fastcall *)(void *set, const char *path, bool load);
        // The engine's CAF image loader: the hook's trampoline once detour_clip_image_load is armed, so the framework's
        // own index loads never reach the detour, else the function itself.
        ClipImageLoadFunc s_clip_image_load = nullptr;
        std::atomic<ClipImageLoadFunc> s_clip_image_load_original{nullptr};
        // Set when this generation fed the game its startup clip index (mods' replacements and whole copies merged).
        std::atomic<bool> s_image_merged{false};
        // Off when the .dba mismatch check stays on: that check ends the game once a packed clip is replaced.
        bool s_packed_replacements = true;
        ChrParamsReloadFunc s_chrparams_reload = nullptr;
        // Set last by the install, after the clip state above. The frame tick arms before the install, so a clip
        // reload reads that state only once this is set.
        std::atomic<bool> s_mod_clips_on{false};
        std::atomic<bool> s_clip_work_pending{false};
        // Main thread only (the frame tick): the late path logs its wait for the clip index once.
        bool s_clip_index_wait_logged = false;

        /** @brief The manager's global header table: its first GlobalAnimationHeaderCAF and its count. */
        struct HeaderTable
        {
            std::uintptr_t base = 0;
            std::uint32_t count = 0;
        };

        /**
         * @brief The global header table of @p manager, or an empty table when it does not read as one.
         * @details Every header of the table must be readable, so the scans over it read the headers directly.
         * @note Clip index loads and clip reloads only: the range check can query the OS once per memory region.
         */
        [[nodiscard]] HeaderTable header_table(void *manager) noexcept
        {
            const std::uintptr_t base = read_ptr(reinterpret_cast<std::uintptr_t>(manager) + CM_HEADER_TABLE_OFFSET);
            const std::optional<std::uint32_t> count =
                base != 0 ? read_value<std::uint32_t>(base + constants::DYN_ARRAY_COUNT_OFFSET) : std::nullopt;
            if (!count || *count > MAX_GLOBAL_HEADERS || !DMK::memory::is_plausible_ptr(DMK::Address{base}))
            {
                return {};
            }
            const DMK::Region headers{
                .base = DMK::Address{base},
                .size = std::size_t{*count} * GLOBAL_HEADER_STRIDE,
            };
            if (*count != 0 && !DMK::memory::is_readable(headers))
            {
                return {};
            }
            return HeaderTable{
                .base = base,
                .count = *count,
            };
        }

        // Loose clips: compiled .caf files under `<dev>/Animations/` at their game paths. The game reads mods' files
        // from paks, so the framework packs them into a pak of its own under `<dev>/loose_clips/`, with a new name each
        // time because a mounted pak stays locked until the game exits. CryPak serves the newest mount, so the latest
        // pack wins.
        constexpr std::string_view LOOSE_CLIP_ROOT = "animations";
        constexpr std::string_view LOOSE_CLIP_PAK_DIR = "loose_clips";
        constexpr std::string_view LOOSE_CLIP_PAK_PREFIX = "loose_clips_";
        constexpr int MAX_LOOSE_CLIP_PAKS = 100000;

        // Serializes the DEV folder mounts and guards the pack state below: the startup mount runs inside the clip
        // index load, a clip reload on the frame tick.
        std::mutex s_dev_pak_mutex;
        // The loose clips' names, sizes and write times when they were last packed: an unchanged folder packs nothing.
        std::string s_loose_clip_state;
        // Loose blend spaces (.bspace, .comb) packed so far, and whether a pack brought a new one since
        // take_new_blend_spaces: the animation sets list blend spaces when they parse their CHRPARAMS, so a new one
        // needs the sets reloaded.
        std::unordered_set<std::string> s_loose_blend_spaces;
        bool s_new_blend_spaces = false;
        constexpr std::array<std::string_view, 3> LOOSE_CLIP_EXTENSIONS{
            ".caf",
            ".bspace",
            ".comb",
        };

        /** @brief A loose clip: its path in the game's data, lowercase with forward slashes, and its file. */
        struct LooseClip
        {
            std::string name;
            std::filesystem::path file;
            std::uintmax_t size = 0;
            std::filesystem::file_time_type written{};
        };

        /** @brief Every compiled clip and blend space under `<dev>/Animations/`, by name. */
        [[nodiscard]] std::vector<LooseClip> find_loose_clips(const std::filesystem::path &dev)
        {
            std::vector<LooseClip> clips;
            std::error_code error;
            const std::filesystem::path root = dev / LOOSE_CLIP_ROOT;
            if (!std::filesystem::is_directory(root, error))
            {
                return clips;
            }
            using Walk = std::filesystem::recursive_directory_iterator;
            for (Walk it{root, std::filesystem::directory_options::skip_permission_denied, error};
                 !error && it != Walk{};
                 it.increment(error))
            {
                std::error_code file_error;
                const std::string extension = lowercase(it->path().extension().string());
                if (!it->is_regular_file(file_error) ||
                    std::ranges::find(LOOSE_CLIP_EXTENSIONS, extension) == LOOSE_CLIP_EXTENSIONS.end())
                {
                    continue;
                }
                clips.push_back(
                    LooseClip{
                        .name = std::string{LOOSE_CLIP_ROOT} + "/" +
                                lowercase(it->path().lexically_relative(root).generic_string()),
                        .file = it->path(),
                        .size = it->file_size(file_error),
                        .written = it->last_write_time(file_error),
                    }
                );
            }
            std::ranges::sort(clips, {}, &LooseClip::name);
            return clips;
        }

        /**
         * @brief Deletes the packs of loose clips no game has open: an earlier session's. A pack this session mounted
         *        stays locked until the game exits, so its delete fails and it stays.
         */
        void delete_unused_loose_clip_paks(const std::filesystem::path &dev)
        {
            for (const std::filesystem::path &file : files_in(dev / LOOSE_CLIP_PAK_DIR))
            {
                const std::string name = lowercase(file.filename().string());
                if (name.starts_with(LOOSE_CLIP_PAK_PREFIX) && name.ends_with(".pak"))
                {
                    std::error_code locked;
                    std::filesystem::remove(file, locked);
                }
            }
        }

        /** @brief A pak path under `<dev>/loose_clips/` no file has. */
        [[nodiscard]] std::optional<std::filesystem::path> next_loose_clip_pak(const std::filesystem::path &dev)
        {
            std::error_code error;
            const std::filesystem::path folder = dev / LOOSE_CLIP_PAK_DIR;
            std::filesystem::create_directories(folder, error);
            for (int n = 1; n < MAX_LOOSE_CLIP_PAKS; ++n)
            {
                std::filesystem::path pak = folder / std::format("{}{}.pak", LOOSE_CLIP_PAK_PREFIX, n);
                if (!std::filesystem::exists(pak, error))
                {
                    return pak;
                }
            }
            return std::nullopt;
        }

        /**
         * @brief Packs every loose clip into a new pak when any changed since the last pack. Returns the pak to mount,
         *        or nothing. The pak holds every loose clip, so the newest pack serves them all.
         */
        [[nodiscard]] std::optional<std::filesystem::path> pack_loose_clips(const std::filesystem::path &dev)
        {
            DMK::Logger &logger = DMK::log();
            const std::vector<LooseClip> clips = find_loose_clips(dev);
            std::string state;
            for (const LooseClip &clip : clips)
            {
                state += std::format("{}|{}|{}\n", clip.name, clip.size, clip.written.time_since_epoch().count());
            }
            if (clips.empty() || state == s_loose_clip_state)
            {
                return std::nullopt;
            }
            std::vector<ZipSource> files;
            files.reserve(clips.size());
            // A clip that does not read whole (an exporter still writing it) is left out and the state not kept, so
            // the next reload packs it again.
            bool whole = true;
            for (const LooseClip &clip : clips)
            {
                std::ifstream in{clip.file, std::ios::binary};
                std::string data =
                    in.is_open() ? std::string(std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{})
                                 : std::string{};
                if (!in.is_open() || data.size() != clip.size)
                {
                    logger.warning("AnimClips: could not read the loose clip {}", clip.file.string());
                    whole = false;
                    continue;
                }
                files.push_back(ZipSource{.name = clip.name, .data = std::move(data)});
            }
            if (files.empty())
            {
                return std::nullopt;
            }
            const std::optional<std::string> zip = write_stored_zip(files);
            const std::optional<std::filesystem::path> pak = zip ? next_loose_clip_pak(dev) : std::nullopt;
            if (!pak)
            {
                logger.warning("AnimClips: could not pack the {} loose clip(s) of the DEV folder", clips.size());
                return std::nullopt;
            }
            {
                std::ofstream out{*pak, std::ios::binary | std::ios::trunc};
                out.write(zip->data(), static_cast<std::streamsize>(zip->size()));
                if (!out.good())
                {
                    logger.warning("AnimClips: could not write {}", pak->string());
                    return std::nullopt;
                }
            }
            s_loose_clip_state = whole ? std::move(state) : std::string{};
            for (const ZipSource &file : files)
            {
                if (!file.name.ends_with(".caf") && s_loose_blend_spaces.insert(file.name).second)
                {
                    s_new_blend_spaces = true;
                }
            }
            logger.info("AnimClips: packed {} loose clip(s) from the DEV folder into {}", files.size(), pak->string());
            return pak;
        }

        /**
         * @brief Mounts each DEV folder pak CryPak does not have open yet as a mod overlay ("data\\" bind root), as the
         *        mod manager mounts paks, then a new pack of the loose clips when they changed. Returns the paks
         *        mounted.
         */
        [[nodiscard]] int mount_dev_paks()
        {
            const std::lock_guard lock{s_dev_pak_mutex};
            void *cry_pak = reinterpret_cast<void *>(genv_member(constants::GENV_CRYPAK_OFFSET));
            std::error_code error;
            const std::filesystem::path dev = dev_patch_dir();
            if (cry_pak == nullptr || !std::filesystem::is_directory(dev, error))
            {
                return 0;
            }
            std::vector<std::filesystem::path> paks;
            for (std::filesystem::path &file : files_in(dev))
            {
                if (lowercase(file.extension().string()) == ".pak")
                {
                    paks.push_back(std::move(file));
                }
            }
            delete_unused_loose_clip_paks(dev);
            if (std::optional<std::filesystem::path> loose = pack_loose_clips(dev))
            {
                paks.push_back(*std::move(loose));
            }
            const std::vector<std::string> open = mounted_pak_paths();
            int count = 0;
            for (const std::filesystem::path &pak : paks)
            {
                const std::string path = pak.string();
                if (std::ranges::find(open, lowercase(path)) != open.end())
                {
                    continue;
                }
                const bool mounted = vcall<bool>(
                    cry_pak,
                    CRYPAK_OPEN_PACK_SLOT,
                    "data\\",
                    path.c_str(),
                    OVERLAY_PAK_FLAGS,
                    static_cast<void *>(nullptr),
                    static_cast<void *>(nullptr)
                );
                DMK::log().info("AnimClips: {} {}", mounted ? "mounted" : "could not mount", path);
                count += mounted ? 1 : 0;
            }
            return count;
        }

        /** @brief True, once, when a pack of the loose clips brought a blend space no earlier pack had. */
        [[nodiscard]] bool take_new_blend_spaces() noexcept
        {
            const std::lock_guard lock{s_dev_pak_mutex};
            return std::exchange(s_new_blend_spaces, false);
        }

        /**
         * @brief One reference to a new read-only engine chunk file (I3DEngine::CreateChunkFile), or none when the 3D
         *        engine is not up. The chunk file, with the file data it read, is deleted with its last reference.
         */
        class ChunkFileRef
        {
        public:
            ChunkFileRef() noexcept
            {
                void *engine = reinterpret_cast<void *>(genv_member(constants::GENV_3D_ENGINE_OFFSET));
                if (engine != nullptr)
                {
                    m_file = vcall<void *>(engine, constants::THREE_D_ENGINE_CREATE_CHUNK_FILE_SLOT, true);
                }
                if (m_file != nullptr)
                {
                    ++refs();
                }
            }
            ChunkFileRef(const ChunkFileRef &) = delete;
            ChunkFileRef &operator=(const ChunkFileRef &) = delete;
            ~ChunkFileRef() noexcept
            {
                if (m_file != nullptr && --refs() <= 0)
                {
                    vcall<void>(m_file, CHUNK_FILE_DELETING_DESTRUCTOR_SLOT, 1);
                }
            }

            [[nodiscard]] void *get() const noexcept { return m_file; }
            explicit operator bool() const noexcept { return m_file != nullptr; }

        private:
            [[nodiscard]] int &refs() const noexcept
            {
                return *reinterpret_cast<int *>(static_cast<std::byte *>(m_file) + CHUNK_FILE_REFCOUNT_OFFSET);
            }

            void *m_file = nullptr;
        };

        /** @brief Mounts the DEV folder's paks once per generation. */
        void mount_dev_paks_once()
        {
            static std::atomic<bool> mounted{false};
            if (!mounted.exchange(true, std::memory_order_acq_rel))
            {
                (void)mount_dev_paks();
            }
        }

        /**
         * @brief Appends every `animations/animations__*.img` to the global header table through the engine's loader,
         *        from the table's current count. Returns the headers appended.
         */
        [[nodiscard]] std::uint32_t append_clip_indexes(void *manager)
        {
            DMK::Logger &logger = DMK::log();
            const std::vector<std::string> images = find_pak_files("animations/animations__*.img");
            std::uint32_t appended = 0;
            for (const std::string &name : images)
            {
                const std::string path = "animations/" + name;
                const HeaderTable table = header_table(manager);
                const ChunkFileRef chunk_file;
                if (table.base == 0 || !chunk_file)
                {
                    logger.warning("AnimClips: no chunk file or header table for {}", path);
                    continue;
                }
                std::uint32_t next = table.count;
                const bool loaded = s_clip_image_load(manager, path.c_str(), chunk_file.get(), &next);
                appended += next - table.count;
                logger.info(
                    "AnimClips: {} {} ({} header(s), {} -> {})",
                    loaded ? "registered" : "could not load",
                    path,
                    next - table.count,
                    table.count,
                    next
                );
            }
            return appended;
        }

        /** @brief @p chrparams with the case of its letters spelling @p serial in binary, one letter per bit. */
        [[nodiscard]] std::string reload_alias(std::string_view chrparams, std::uint32_t serial)
        {
            std::string alias{chrparams};
            for (char &letter : alias)
            {
                const char lowered = ascii_lower(letter);
                if (lowered >= 'a' && lowered <= 'z')
                {
                    letter = (serial & 1U) != 0 ? static_cast<char>(lowered - 'a' + 'A') : lowered;
                    serial >>= 1;
                }
            }
            return alias;
        }

        /**
         * @brief How many parsed CHRPARAMS lists the manager caches. Every reload under a new alias adds to it and
         *        nothing removes from it, so it grows over the whole process, across the framework's generations.
         */
        [[nodiscard]] std::uint32_t cached_param_list_count(std::uintptr_t manager) noexcept
        {
            const std::uintptr_t lists = read_ptr(manager + CM_PARAM_LISTS_OFFSET);
            const std::int32_t count =
                lists != 0 ? read_value<std::int32_t>(lists + constants::DYN_ARRAY_COUNT_OFFSET).value_or(0) : 0;
            return static_cast<std::uint32_t>(std::max(count, 0));
        }

        /**
         * @brief The name member of the manager's cached CHRPARAMS list named exactly @p name, or nullptr. Each name
         *        reads through a guarded read, so the returned member is readable.
         */
        [[nodiscard]] const char **find_param_list(std::uintptr_t manager, std::string_view name) noexcept
        {
            const std::uintptr_t lists = read_ptr(manager + CM_PARAM_LISTS_OFFSET);
            const std::uintptr_t lists_end = lists + std::size_t{cached_param_list_count(manager)} * PARAM_LIST_STRIDE;
            std::array<char, MAX_PATH * 2> text{};
            for (std::uintptr_t list = lists; list < lists_end; list += PARAM_LIST_STRIDE)
            {
                const std::size_t length = read_c_string(read_ptr(list), text);
                if (length != 0 && name == std::string_view{text.data(), length})
                {
                    return reinterpret_cast<const char **>(list);
                }
            }
            return nullptr;
        }

        /**
         * @brief Hands the path @p chrparams the cached list parsed under @p alias, when that list is the newer one.
         * @details A model the game loads again later (every model on a level change) reloads its CHRPARAMS under its
         *          own path and takes that path's cached list. That list predates the appended clips, so without the
         *          swap the model loses them. A swap of the two lists' names gives the path the new list. A name is a
         *          CryString's char pointer, so the swap moves no reference count. The cache only appends, so the later
         *          member is the newer list, and a swapped pair is never swapped back.
         * @return True when the names were swapped.
         */
        [[nodiscard]] bool
        adopt_alias_list(std::uintptr_t manager, std::string_view chrparams, std::string_view alias) noexcept
        {
            const char **own = find_param_list(manager, chrparams);
            const char **fresh = find_param_list(manager, alias);
            if (own == nullptr || fresh == nullptr || fresh <= own)
            {
                return false;
            }
            std::swap(*own, *fresh);
            return true;
        }

        // The serial of the last reload_animation_sets alias. Main thread only.
        std::uint32_t s_last_alias_serial = 0;

        /**
         * @brief Reloads every animation set's CHRPARAMS so a set built before an append lists the new clips.
         * @details The manager caches each CHRPARAMS animation list, wildcards expanded, under its exact path
         *          (Steam 0x181315D10, a case-sensitive compare), and a reload under that path reuses it. CryPak opens
         *          paths case-blind, so the same file under a path of different letter case parses afresh and expands
         *          its wildcards again, now over the appended headers. The case pattern spells a serial in binary
         *          (reload_alias) that grows with every reload: at least the header table's count plus the cached list
         *          count, which both only grow, and past the last reload's, since a reload for new blend spaces adds no
         *          header. So every reload uses a path no earlier one cached: a cached path hands back the list parsed
         *          before the new files. Once every set reloaded, each set's own path takes the new list
         *          (adopt_alias_list), so sets that share a CHRPARAMS file all parse under the alias first.
         */
        void reload_animation_sets(void *manager)
        {
            const auto address = reinterpret_cast<std::uintptr_t>(manager);
            const std::uint32_t serial =
                std::max(header_table(manager).count + cached_param_list_count(address), s_last_alias_serial + 1);
            s_last_alias_serial = serial;
            const std::uintptr_t begin = read_ptr(address + CM_SETS_BEGIN_OFFSET);
            const std::uintptr_t end = read_ptr(address + CM_SETS_END_OFFSET);
            if (begin == 0 || end < begin || (end - begin) % CM_SET_STRIDE != 0 ||
                (end - begin) / CM_SET_STRIDE > MAX_ANIMATION_SETS)
            {
                DMK::log().warning("AnimClips: the animation set list does not read as one; no set reloads");
                return;
            }
            std::array<char, MAX_PATH * 2> text{};
            std::vector<std::pair<void *, std::string>> sets;
            for (std::uintptr_t entry = begin; entry < end; entry += CM_SET_STRIDE)
            {
                const std::uintptr_t set = read_ptr(entry);
                const std::uintptr_t chrparams = set != 0 ? read_ptr(set + SET_CHRPARAMS_OFFSET) : 0;
                const std::size_t length = read_c_string(chrparams, text);
                if (length != 0)
                {
                    sets.emplace_back(reinterpret_cast<void *>(set), std::string{text.data(), length});
                }
            }
            int reloaded = 0;
            for (const auto &[set, chrparams] : sets)
            {
                if (s_chrparams_reload(set, reload_alias(chrparams, serial).c_str(), true))
                {
                    ++reloaded;
                }
            }
            int adopted = 0;
            for (const auto &[set, chrparams] : sets)
            {
                if (adopt_alias_list(address, chrparams, reload_alias(chrparams, serial)))
                {
                    ++adopted;
                }
            }
            DMK::log().info(
                "AnimClips: reloaded the animation list of {} animation set(s), {} kept for later loads",
                reloaded,
                adopted
            );
        }

        // The chunk file slots the CAF image loader (Steam 0x1806A1F18) calls: 4 Read(path)
        // (constants::CHUNK_FILE_READ_SLOT), 14 NumChunks, 15 and 16 GetChunk(index) (const and not), and 17
        // GetLastError after a failed read, the same on the Steam, GOG and Game Pass builds. GetChunk returns a
        // ChunkDesc whose type and version are the file's 16-bit values widened (0x3007/0x971 for an index entry).
        constexpr std::size_t CHUNK_FILE_COUNT_SLOT = 14;
        constexpr std::size_t CHUNK_FILE_GET_CONST_SLOT = 15;
        constexpr std::size_t CHUNK_FILE_GET_SLOT = 16;
        constexpr std::size_t CHUNK_FILE_LAST_ERROR_SLOT = 17;
        constexpr std::size_t CHUNK_FILE_SLOT_COUNT = 24;
        // A stock clip the scan must find under clip_path_hash: a miss means the engine's path hash differs from
        // clip_path_hash, and nothing appends.
        constexpr std::string_view HASH_PROBE_CLIP = "animations/animals/cat/relaxed_idle_gallop.caf";
        constexpr std::size_t MAX_LOGGED_CLIPS = 32;

        // CryEngine's IChunkFile::ChunkDesc: type, version, id and file offset, then the data pointer, the size and the
        // swap flag. The CAF image loader (Steam 0x1806A1F18) reads the type at +0, the version at +4, the data at
        // +0x10 and the swap flag at +0x1C, and nothing else.
        struct ChunkDesc
        {
            std::uint32_t type;
            std::uint32_t version;
            std::int32_t id;
            std::uint32_t file_offset;
            const void *data;
            std::uint32_t size;
            bool swap_endian;
        };
        static_assert(
            offsetof(ChunkDesc, file_offset) == 0x0C && offsetof(ChunkDesc, data) == 0x10 &&
            offsetof(ChunkDesc, size) == 0x18 && offsetof(ChunkDesc, swap_endian) == 0x1C && sizeof(ChunkDesc) == 0x20
        );

        /** @brief Built clip headers as a chunk file: the loader reads them as if from an .img on disk. */
        struct MemoryChunkFile
        {
            const void *const *vtable;
            int refs;
            const ChunkDesc *chunks;
            int count;
        };

        std::uintptr_t __fastcall memory_chunk_stub(void *) noexcept
        {
            return 0;
        }
        bool __fastcall memory_chunk_read(void *, const char *) noexcept
        {
            return true;
        }
        int __fastcall memory_chunk_count(const MemoryChunkFile *self) noexcept
        {
            return self->count;
        }
        const ChunkDesc *__fastcall memory_chunk_get(const MemoryChunkFile *self, int index) noexcept
        {
            return index >= 0 && index < self->count ? self->chunks + index : nullptr;
        }
        const char *__fastcall memory_chunk_error(void *) noexcept
        {
            return "";
        }

        [[nodiscard]] const void *const *memory_chunk_vtable() noexcept
        {
            static const std::array<const void *, CHUNK_FILE_SLOT_COUNT> table = []
            {
                std::array<const void *, CHUNK_FILE_SLOT_COUNT> slots{};
                slots.fill(reinterpret_cast<const void *>(&memory_chunk_stub));
                slots[constants::CHUNK_FILE_READ_SLOT] = reinterpret_cast<const void *>(&memory_chunk_read);
                slots[CHUNK_FILE_COUNT_SLOT] = reinterpret_cast<const void *>(&memory_chunk_count);
                slots[CHUNK_FILE_GET_CONST_SLOT] = reinterpret_cast<const void *>(&memory_chunk_get);
                slots[CHUNK_FILE_GET_SLOT] = reinterpret_cast<const void *>(&memory_chunk_get);
                slots[CHUNK_FILE_LAST_ERROR_SLOT] = reinterpret_cast<const void *>(&memory_chunk_error);
                return slots;
            }();
            return table.data();
        }

        /**
         * @brief Feeds @p headers to the engine's CAF image loader as an in-memory chunk file, from @p count on.
         * @return The loader's result. @p count holds the table's count after it.
         */
        [[nodiscard]] bool load_clip_headers(
            ClipImageLoadFunc load,
            void *manager,
            const char *path,
            std::span<const ClipHeader> headers,
            std::uint32_t *count
        )
        {
            std::vector<ChunkDesc> chunks;
            chunks.reserve(headers.size());
            for (std::size_t i = 0; i < headers.size(); ++i)
            {
                chunks.push_back(
                    ChunkDesc{
                        .type = CLIP_HEADER_CHUNK_TYPE,
                        .version = CLIP_HEADER_CHUNK_VERSION,
                        .id = static_cast<std::int32_t>(i + 1),
                        .file_offset = 0,
                        .data = headers[i].data(),
                        .size = static_cast<std::uint32_t>(CLIP_HEADER_SIZE),
                        .swap_endian = false,
                    }
                );
            }
            MemoryChunkFile file{
                .vtable = memory_chunk_vtable(),
                .refs = 1,
                .chunks = chunks.data(),
                .count = static_cast<int>(chunks.size()),
            };
            return load(manager, path, &file, count);
        }

        /**
         * @brief A clip mod paks ship, the newest pak with it (the copy CryPak serves) and whether a pak with it was
         *        written after the game started (a rebuilt or newly added pak, so its clip may have new keys).
         */
        struct ModClip
        {
            std::string path;
            std::string pak;
            bool written_this_session = false;
            // The winning pak's zip entry, the clip summary cache's key.
            std::uint32_t crc32 = 0;
            std::uint32_t compressed_size = 0;
            std::uint32_t size = 0;
        };

        /** @brief Whether @p file was last written after this process started. */
        [[nodiscard]] bool written_this_session(const std::filesystem::path &file) noexcept
        {
            static const ULONGLONG started = []
            {
                FILETIME created{};
                FILETIME unused{};
                FILETIME kernel{};
                FILETIME user{};
                if (GetProcessTimes(GetCurrentProcess(), &created, &unused, &kernel, &user) == FALSE)
                {
                    return ~0ULL;
                }
                return (static_cast<ULONGLONG>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
            }();
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data) == FALSE)
            {
                return false;
            }
            const ULONGLONG written = (static_cast<ULONGLONG>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                                      data.ftLastWriteTime.dwLowDateTime;
            return written > started;
        }

        /**
         * @brief Every `.caf` under `animations/` that the mounted mod paks (and the DEV folder's) carry, each once,
         *        in order of first appearance, read from each pak's central directory. A later pak with the same clip
         *        becomes its source, as CryPak serves the newest mount.
         */
        [[nodiscard]] std::vector<ModClip> find_mod_clips()
        {
            std::vector<ModClip> clips;
            std::unordered_map<std::string, std::size_t> slot;
            for (const ModPak &mod : mounted_mod_paks())
            {
                const auto index = zip_index_of(mod.file);
                if (!index)
                {
                    DMK::log().warning("AnimClips: could not read the directory of {}", mod.mounted);
                    continue;
                }
                const bool written = written_this_session(mod.file);
                for (const ZipEntry &entry : *index)
                {
                    if (!entry.name.starts_with("animations/") || !entry.name.ends_with(".caf"))
                    {
                        continue;
                    }
                    if (const auto it = slot.find(entry.name); it != slot.end())
                    {
                        ModClip &clip = clips[it->second];
                        clip.pak = mod.mounted;
                        clip.written_this_session = clip.written_this_session || written;
                        clip.crc32 = entry.crc32;
                        clip.compressed_size = entry.compressed_size;
                        clip.size = entry.size;
                        continue;
                    }
                    slot.emplace(entry.name, clips.size());
                    clips.push_back(
                        ModClip{
                            .path = entry.name,
                            .pak = mod.mounted,
                            .written_this_session = written,
                            .crc32 = entry.crc32,
                            .compressed_size = entry.compressed_size,
                            .size = entry.size,
                        }
                    );
                }
            }
            return clips;
        }

        // Each mod clip's zip CRC when the clips last registered, so a clip reload tells which files changed. Main
        // thread (the clip index init, then the frame tick).
        std::unordered_map<std::string, std::uint32_t> s_clip_crcs;

        /** @brief The zip CRC of each clip @p clips lists, by path. */
        [[nodiscard]] std::unordered_map<std::string, std::uint32_t> clip_crcs(const std::vector<ModClip> &clips)
        {
            std::unordered_map<std::string, std::uint32_t> crcs;
            crcs.reserve(clips.size());
            for (const ModClip &clip : clips)
            {
                crcs.emplace(clip.path, clip.crc32);
            }
            return crcs;
        }

        /** @brief A compiled .caf's motion parameters chunk, controller chunk count and the bones it keys. */
        struct CafSummary
        {
            std::array<std::byte, CAF_MOTION_CHUNK_SIZE> motion{};
            std::uint32_t controllers = 0;
            std::vector<std::uint32_t> bones;
        };

        // The controller chunk version whose descriptor starts with the bone's controller id.
        constexpr std::uint32_t CAF_CONTROLLER_CHUNK_VERSION = 0x829;

        /** @brief Reads @p path through an engine chunk file (CryPak: the winning mod's copy). */
        [[nodiscard]] std::optional<CafSummary> read_caf_summary(const std::string &path)
        {
            const ChunkFileRef chunk_file;
            if (!chunk_file || !vcall<bool>(chunk_file.get(), constants::CHUNK_FILE_READ_SLOT, path.c_str()))
            {
                return std::nullopt;
            }
            CafSummary found;
            bool has_motion = false;
            const int count = vcall<int>(chunk_file.get(), CHUNK_FILE_COUNT_SLOT);
            for (int i = 0; i < count; ++i)
            {
                const auto *chunk = vcall<const ChunkDesc *>(chunk_file.get(), CHUNK_FILE_GET_SLOT, i);
                if (chunk == nullptr)
                {
                    continue;
                }
                if (chunk->type == CAF_CONTROLLER_CHUNK_TYPE)
                {
                    ++found.controllers;
                    if (chunk->version == CAF_CONTROLLER_CHUNK_VERSION && chunk->data != nullptr &&
                        !chunk->swap_endian && chunk->size >= sizeof(std::uint32_t))
                    {
                        std::uint32_t bone = 0;
                        std::memcpy(&bone, chunk->data, sizeof(bone));
                        found.bones.push_back(bone);
                    }
                }
                else if (
                    chunk->type == CAF_MOTION_CHUNK_TYPE && chunk->version == CAF_MOTION_CHUNK_VERSION &&
                    chunk->data != nullptr && !chunk->swap_endian &&
                    static_cast<std::size_t>(chunk->size) >= CAF_MOTION_CHUNK_SIZE
                )
                {
                    std::memcpy(found.motion.data(), chunk->data, CAF_MOTION_CHUNK_SIZE);
                    has_motion = true;
                }
            }
            if (!has_motion)
            {
                return std::nullopt;
            }
            return found;
        }

        // What the scans read out of mod clips, kept across launches and hot reloads (core/clip_cache.hpp): reading a
        // clip is the engine loading the whole .caf, and a mod can ship a thousand. Loaded at the first scan of this
        // generation, saved after a scan that read anything new. Scans run on the main thread or the loading thread,
        // never both at once. The mutex keeps a stray overlap safe.
        std::mutex s_clip_cache_mutex;
        ClipCache s_clip_cache;
        bool s_clip_cache_loaded = false;

        [[nodiscard]] std::filesystem::path clip_cache_file()
        {
            return game_bin_dir() / constants::CLIP_CACHE_FILE;
        }

        [[nodiscard]] std::string clip_cache_key(const ModClip &clip)
        {
            return ClipCache::key(clip.pak, clip.path, clip.crc32, clip.compressed_size, clip.size);
        }

        // Mod clips under this folder play on Henry's skeleton (and male NPCs'), which the load-time check reads.
        constexpr std::string_view MALE_CLIP_ROOT = "animations/humans/male/";
        constexpr std::uint32_t SKELETON_BONES_CHUNK_TYPE = 0x2000;

        std::mutex s_male_skeleton_mutex;
        std::optional<std::vector<SkeletonBone>> s_male_skeleton;
        bool s_male_skeleton_read = false;

        /**
         * @brief Henry's skeleton as the game loads it (a merged one when skeleton mods ship it), read once per
         *        generation, or null when it cannot be read.
         */
        [[nodiscard]] const std::vector<SkeletonBone> *male_skeleton()
        {
            const std::lock_guard lock{s_male_skeleton_mutex};
            if (!s_male_skeleton_read)
            {
                s_male_skeleton_read = true;
                const std::string path{MALE_SKELETON_PATH};
                const ChunkFileRef chunk_file;
                const bool read =
                    chunk_file && vcall<bool>(chunk_file.get(), constants::CHUNK_FILE_READ_SLOT, path.c_str());
                std::string why = read ? "no bones chunk" : "the game cannot read it";
                if (read)
                {
                    const int count = vcall<int>(chunk_file.get(), CHUNK_FILE_COUNT_SLOT);
                    for (int i = 0; i < count && !s_male_skeleton; ++i)
                    {
                        const auto *chunk = vcall<const ChunkDesc *>(chunk_file.get(), CHUNK_FILE_GET_SLOT, i);
                        if (chunk == nullptr)
                        {
                            continue;
                        }
                        if (chunk->type == SKELETON_BONES_CHUNK_TYPE && chunk->data != nullptr && chunk->size > 0 &&
                            !chunk->swap_endian)
                        {
                            auto bones = read_skeleton_bones(
                                {static_cast<const std::byte *>(chunk->data), static_cast<std::size_t>(chunk->size)}
                            );
                            if (bones && !bones->empty())
                            {
                                s_male_skeleton = std::move(*bones);
                            }
                            else
                            {
                                why = bones ? std::string{"no bones"} : bones.error();
                            }
                        }
                    }
                }
                if (!s_male_skeleton)
                {
                    DMK::log().warning("AnimClips: could not read {} ({}); mods' clips are not checked", path, why);
                }
            }
            return s_male_skeleton ? &*s_male_skeleton : nullptr;
        }

        /** @brief Clips of one pak with the same clip check finding, logged as one line. */
        struct ClipFindingGroup
        {
            ClipFindingLevel level = ClipFindingLevel::Note;
            std::size_t count = 0;
            std::string first_clip;
        };

        // Guards s_checked_clips and s_clip_findings.
        std::mutex s_checked_clips_mutex;
        // The clip versions this generation checked (cache keys), so a reload logs only new and changed clips.
        std::unordered_set<std::string> s_checked_clips;
        // Findings since the last summary, by pak and finding text. A mod that ships many clips with the same flaw
        // (a set of re-exported cutscenes) then logs one line for it instead of one per clip.
        std::map<std::pair<std::string, std::string>, ClipFindingGroup> s_clip_findings;

        /**
         * @brief Checks what the bones @p clip keys break: bones Henry's skeleton lacks, back slots skeleton mods
         *        move. Each finding goes to DEBUG for the clip and into the summary log_clip_findings writes.
         * @details Male clips only, as they play on Henry's skeleton. Each clip version is checked once per generation.
         */
        void check_mod_clip(const ModClip &clip, std::span<const std::uint32_t> bones)
        {
            if (!clip.path.starts_with(MALE_CLIP_ROOT))
            {
                return;
            }
            {
                const std::lock_guard lock{s_checked_clips_mutex};
                if (!s_checked_clips.insert(clip_cache_key(clip)).second)
                {
                    return;
                }
            }
            const std::vector<SkeletonBone> *skeleton = male_skeleton();
            if (skeleton == nullptr)
            {
                return;
            }
            const std::vector<ClipFinding> findings = check_clip_bones(bones, *skeleton);
            for (const ClipFinding &finding : findings)
            {
                DMK::log().debug("AnimClips: {} (from {}) {}", clip.path, clip.pak, finding.text);
            }
            const std::lock_guard lock{s_checked_clips_mutex};
            for (const ClipFinding &finding : findings)
            {
                ClipFindingGroup &group = s_clip_findings[{clip.pak, finding.text}];
                if (group.count++ == 0)
                {
                    group.level = finding.level;
                    group.first_clip = clip.path;
                }
            }
        }

        /**
         * @brief Logs the findings check_mod_clip gathered since the last call, one line per pak and finding with
         *        the number of clips and the first of them. LogLevel DEBUG lists every clip.
         */
        void log_clip_findings()
        {
            std::map<std::pair<std::string, std::string>, ClipFindingGroup> groups;
            {
                const std::lock_guard lock{s_checked_clips_mutex};
                groups.swap(s_clip_findings);
            }
            DMK::Logger &logger = DMK::log();
            for (const auto &[key, group] : groups)
            {
                const auto &[pak, text] = key;
                const DMK::LogLevel level =
                    group.level == ClipFindingLevel::Warning ? DMK::LogLevel::Warning : DMK::LogLevel::Info;
                if (group.count == 1)
                {
                    logger.log(level, "AnimClips: {} (from {}) {}", group.first_clip, pak, text);
                }
                else
                {
                    logger.log(
                        level,
                        "AnimClips: {} clips from {} each {} (the first: {})",
                        group.count,
                        pak,
                        text,
                        group.first_clip
                    );
                }
            }
        }

        /**
         * @brief The index entry built from the winning pak's copy of @p clip, or nothing when it is not a compiled
         *        clip.
         */
        [[nodiscard]] std::optional<ClipHeader> build_mod_clip_header(const ModClip &clip)
        {
            const std::string key = clip_cache_key(clip);
            {
                std::unique_lock lock{s_clip_cache_mutex};
                if (!s_clip_cache_loaded)
                {
                    s_clip_cache_loaded = true;
                    (void)s_clip_cache.load(clip_cache_file());
                }
                if (const CachedClip *cached = s_clip_cache.find(key))
                {
                    const CachedClip copy = *cached;
                    lock.unlock();
                    check_mod_clip(clip, copy.bones);
                    return build_clip_header(copy.motion, copy.controllers, clip.path);
                }
            }
            const std::optional<CafSummary> caf = read_caf_summary(clip.path);
            if (!caf)
            {
                return std::nullopt;
            }
            {
                const std::lock_guard lock{s_clip_cache_mutex};
                s_clip_cache.put(
                    key,
                    CachedClip{
                        .motion = {caf->motion.begin(), caf->motion.end()},
                        .controllers = caf->controllers,
                        .bones = caf->bones,
                    }
                );
            }
            check_mod_clip(clip, caf->bones);
            return build_clip_header(caf->motion, caf->controllers, clip.path);
        }

        /** @brief Saves the clip summary cache after a scan that read new clips, keeping only @p clips' entries. */
        void save_clip_cache(const std::vector<ModClip> &clips)
        {
            const std::lock_guard lock{s_clip_cache_mutex};
            if (!s_clip_cache.dirty())
            {
                return;
            }
            std::vector<std::string> keep;
            keep.reserve(clips.size());
            for (const ModClip &clip : clips)
            {
                keep.push_back(clip_cache_key(clip));
            }
            if (s_clip_cache.save(clip_cache_file(), keep))
            {
                DMK::log().info(
                    "AnimClips: clip summary cache saved ({} clip(s), {})",
                    keep.size(),
                    clip_cache_file().filename().string()
                );
            }
            else
            {
                DMK::log().warning("AnimClips: could not write {}", clip_cache_file().string());
            }
        }

        // Runtime clip header fields the image loader (Steam 0x1806A1F18) writes from CHUNK_GAHCAF_INFO: the flags at
        // +0x18, the DBA CRC at +0x28 (0 = a loose .caf), the duration at +0x34 and the controller count (u16) at
        // +0x9E.
        constexpr std::ptrdiff_t GLOBAL_HEADER_FLAGS_OFFSET = 0x18;
        constexpr std::ptrdiff_t GLOBAL_HEADER_DBA_OFFSET = 0x28;
        constexpr std::ptrdiff_t GLOBAL_HEADER_DURATION_OFFSET = 0x34;
        constexpr std::ptrdiff_t GLOBAL_HEADER_CONTROLLERS_OFFSET = 0x9E;
        // Header flag the streamer sets while a clip's .caf is being read (CA_ASSET_REQUESTED).
        constexpr std::uint32_t GLOBAL_HEADER_FLAG_REQUESTED = 0x1000;

        /**
         * @brief The field at @p offset of the global clip header at @p header.
         * @warning @p header must lie in a table that header_table() found readable on this call path. The read is not
         *          guarded.
         */
        template <typename T> [[nodiscard]] T header_field(std::uintptr_t header, std::ptrdiff_t offset) noexcept
        {
            return DMK::memory::unchecked::read<T>(DMK::Address{header + offset});
        }

        /** @brief Whether the global clip header at @p header has a stream in flight (header_field's warning holds). */
        [[nodiscard]] bool is_streaming(std::uintptr_t header) noexcept
        {
            const auto flags = header_field<std::uint32_t>(header, GLOBAL_HEADER_FLAGS_OFFSET);
            return (flags & GLOBAL_HEADER_FLAG_REQUESTED) != 0;
        }

        // CharacterManager vtable slot 44 is ReloadCAF(path): a stub (`mov rcx, [g_pCharacterManager]`,
        // `add rcx, 1C8h`, `jmp`) into the animation manager's ReloadCAF (Steam 0x1837503D4, GOG 0x183740944, Game
        // Pass 0x1837EA2D8, all reached through slot 44). It finds the header by path hash, reads the .caf through
        // CryPak (the winning pak's copy), commits its flags, times and controllers in place, sets the DBA CRC to 0 and
        // clears every blend space's cached parameters. Returns 0 failed, 1 reloaded, 2 not indexed. The controllers
        // it drops are released the way a .dba unload releases them, and a later unload or relocation of that .dba
        // skips the clip (it matches by DBA CRC and by storage range).
        constexpr std::size_t CM_RELOAD_CAF_SLOT = 44;
        constexpr int RELOAD_CAF_OK = 1;
        // ReloadCAF zeroes the loaded controller count (u16 at +0x9C) and LoadCAF sets the flags to CA_MISSING before
        // it reads the file. Neither is undone when the read fails, which leaves the clip unplayable, so the caller
        // restores both on failure (the controllers themselves only change on success).
        constexpr std::ptrdiff_t GLOBAL_HEADER_LOADED_CONTROLLERS_OFFSET = 0x9C;
        // LoadCAF reads the path from a global record map keyed by the int at +0xA0. Loose clips get a record when
        // the index loads. Clips packed in a .dba hold -1 there, so their reload reads an empty path and fails. The
        // header's vtable slot 0, SetFilePath(const string &) (Steam 0x180B7BBC4, with GOG and Game Pass reached the
        // same way, `cmp dword ptr [rcx+0A0h], 0` first), creates the record and stores the path, as the index loader
        // does for every loose entry.
        constexpr std::ptrdiff_t GLOBAL_HEADER_PATH_RECORD_OFFSET = 0xA0;
        constexpr std::size_t GLOBAL_HEADER_SET_FILE_PATH_SLOT = 0;

        /**
         * @brief A CryString (`char *` to the text, a {refcount, length, capacity} header in the 12 bytes before it)
         *        for @p text, on the process heap with one reference that is never released.
         * @details The engine's string assignment shares a buffer by adding a reference, and frees it when the count
         *          drops to zero. Holding one reference forever means the engine can never free this buffer, which it
         *          did not allocate. Each call leaks one small buffer, only for clips that had no path record.
         */
        [[nodiscard]] char *leaked_engine_string(std::string_view text) noexcept
        {
            const auto length = static_cast<std::int32_t>(text.size());
            const std::array<std::int32_t, 3> header{
                1,
                length,
                length,
            };
            const std::size_t bytes = sizeof(header) + text.size() + 1;
            auto *block = static_cast<char *>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes));
            if (block == nullptr)
            {
                return nullptr;
            }
            std::memcpy(block, header.data(), sizeof(header));
            std::memcpy(block + sizeof(header), text.data(), text.size());
            return block + sizeof(header);
        }

        /** @brief What decides whether an index entry differs from the header the game holds for its path. */
        struct ClipFacts
        {
            std::uint32_t dba = 0;
            float duration = 0.0F;
            std::uint32_t controllers = 0;
        };

        [[nodiscard]] ClipFacts facts_of(const ClipHeader &header) noexcept
        {
            ClipFacts facts;
            std::memcpy(&facts.dba, header.data() + CLIP_HEADER_DBA_CRC_OFFSET, sizeof(facts.dba));
            std::memcpy(&facts.duration, header.data() + CLIP_HEADER_DURATION_OFFSET, sizeof(facts.duration));
            std::memcpy(&facts.controllers, header.data() + CLIP_HEADER_CONTROLLERS_OFFSET, sizeof(facts.controllers));
            return facts;
        }

        [[nodiscard]] bool differs(const ClipFacts &a, const ClipFacts &b) noexcept
        {
            return a.dba != b.dba || std::abs(a.duration - b.duration) > 1e-4F || a.controllers != b.controllers;
        }

        /** @brief Index entries alike apart from the two path CRC fields (the engine hashes the path itself). */
        [[nodiscard]] bool same_entry(const ClipHeader &a, const ClipHeader &b) noexcept
        {
            return std::memcmp(a.data(), b.data(), CLIP_HEADER_PATH_CRC_OFFSET) == 0 &&
                   std::memcmp(
                       a.data() + CLIP_HEADER_FOOT_PLANTS_OFFSET,
                       b.data() + CLIP_HEADER_FOOT_PLANTS_OFFSET,
                       CLIP_HEADER_SIZE - CLIP_HEADER_FOOT_PLANTS_OFFSET
                   ) == 0;
        }

        constexpr std::string_view CLIP_IMAGE = "animations/animations.img";

        /**
         * @brief The game's own clip index entries, in file order, from the newest `animations*` base pak. A mod's copy
         *        or clip is compared with this, not with the live table, whose entries the engine updates after it
         *        loads them.
         */
        [[nodiscard]] std::vector<ClipHeader> read_stock_clip_image()
        {
            std::vector<std::filesystem::path> bases;
            (void)mounted_mod_paks(&bases);
            const std::optional<std::string> data = read_base_file(bases, CLIP_IMAGE);
            return data ? read_clip_image(std::as_bytes(std::span{*data})) : std::vector<ClipHeader>{};
        }

        /** @brief How the game holds an entry the framework offers it. */
        enum class Holding : std::uint8_t
        {
            /// The game has no entry for the path: the framework's entry is added.
            Lacks,
            /// The game holds the path as offered.
            Same,
            /// The game holds the path with another pack, length or controller count.
            Differs,
        };

        /**
         * @brief Adds the clip index entries mods need and the game lacks, through the engine's CAF image loader fed
         *        an in-memory chunk file. Returns the headers appended.
         * @details Two sources, in this order:
         *          1. Mods that each ship a whole `animations/animations.img` (the game loads only the newest copy):
         *             every entry of every copy that differs from the game's own index and that the game lacks.
         *          2. Every `.caf` under `animations/` that a mod pak carries: a clip the game has no entry for gets
         *             one built from the file.
         *          A clip the game already holds differently (a replacement) gets no second entry: a second entry for
         *          one path confuses the characters' clip lists. At startup replacements go in through the startup
         *          index (detour_clip_image_load). On a hot reload the engine reloads them in place. The table scan
         *          checks the path hash against a stock clip first: no match means the engine's key changed, so
         *          nothing appends rather than duplicating entries.
         * @param reloads Hot reload only: receives every mod clip the game already holds differently (a replacement
         *                of a stock or packed clip) or whose pak was written this session (new keys), for
         *                reload_clips_in_place. Null at startup.
         */
        [[nodiscard]] std::uint32_t append_mod_clip_headers(void *manager, std::vector<std::string> *reloads)
        {
            DMK::Logger &logger = DMK::log();
            const auto started = std::chrono::steady_clock::now();
            const std::vector<ModClip> clips = find_mod_clips();
            std::vector<WholeCopy> images = read_mod_copies(mounted_mod_paks(), CLIP_IMAGE);
            // One copy alone is the one the game loaded.
            if (images.size() < 2)
            {
                images.clear();
            }
            const HeaderTable table = header_table(manager);
            if ((clips.empty() && images.empty()) || table.base == 0)
            {
                return 0;
            }

            // The header the game currently resolves for each path hash: the newest entry with it.
            std::unordered_map<std::uint64_t, ClipFacts> current;
            current.reserve(table.count);
            for (std::uint32_t i = 0; i < table.count; ++i)
            {
                const std::uintptr_t header = table.base + i * GLOBAL_HEADER_STRIDE;
                const ClipFacts facts{
                    .dba = header_field<std::uint32_t>(header, GLOBAL_HEADER_DBA_OFFSET),
                    .duration = header_field<float>(header, GLOBAL_HEADER_DURATION_OFFSET),
                    .controllers = header_field<std::uint16_t>(header, GLOBAL_HEADER_CONTROLLERS_OFFSET),
                };
                current[header_field<std::uint64_t>(header, GLOBAL_HEADER_HASH_OFFSET)] = facts;
            }
            if (!current.contains(clip_path_hash(HASH_PROBE_CLIP)))
            {
                logger.error(
                    "AnimClips: the stock clip {} has no header under its hash; mods' clips are not indexed",
                    HASH_PROBE_CLIP
                );
                return 0;
            }

            std::vector<ClipHeader> headers;
            const auto take = [&current, &headers](const ClipHeader &header, const std::string &path) -> Holding
            {
                const std::uint64_t hash = clip_path_hash(path);
                const ClipFacts facts = facts_of(header);
                if (const auto it = current.find(hash); it != current.end())
                {
                    return differs(it->second, facts) ? Holding::Differs : Holding::Same;
                }
                current[hash] = facts;
                headers.push_back(header);
                return Holding::Lacks;
            };

            int pending = 0;
            if (!images.empty())
            {
                std::unordered_map<std::uint64_t, ClipHeader> stock;
                for (const ClipHeader &header : read_stock_clip_image())
                {
                    stock[clip_path_hash(clip_header_path(header))] = header;
                }
                for (const WholeCopy &copy : images)
                {
                    const std::vector<ClipHeader> entries = read_clip_image(std::as_bytes(std::span{copy.text}));
                    int added = 0;
                    int held_differently = 0;
                    for (const ClipHeader &entry : entries)
                    {
                        // Only what the mod's copy changed against the game's own index: the rest is the game's.
                        const std::string path = clip_header_path(entry);
                        const auto original = stock.find(clip_path_hash(path));
                        if (original != stock.end() && same_entry(original->second, entry))
                        {
                            continue;
                        }
                        const Holding holding = take(entry, path);
                        added += holding == Holding::Lacks ? 1 : 0;
                        held_differently += holding == Holding::Differs ? 1 : 0;
                    }
                    pending += held_differently;
                    logger.info(
                        "AnimClips: {}'s whole Animations.img ({} entries): {} clip(s) the game lacked, {} it held "
                        "differently",
                        copy.modid,
                        entries.size(),
                        added,
                        held_differently
                    );
                }
            }

            int built = 0;
            std::size_t unchanged = 0;
            for (const ModClip &clip : clips)
            {
                const std::optional<ClipHeader> header = build_mod_clip_header(clip);
                if (!header)
                {
                    logger.warning("AnimClips: {} (from {}) is not a compiled clip; skipped", clip.path, clip.pak);
                    continue;
                }
                const auto it = current.find(clip_path_hash(clip.path));
                if (reloads != nullptr && it != current.end())
                {
                    if (!differs(it->second, facts_of(*header)) && !clip.written_this_session)
                    {
                        ++unchanged;
                    }
                    else if (it->second.dba != 0 && !s_packed_replacements)
                    {
                        logger.warning(
                            "AnimClips: {} replaces a packed clip, which needs the DBA/CAF mismatch check off; skipped",
                            clip.path
                        );
                    }
                    else
                    {
                        reloads->push_back(clip.path);
                    }
                    continue;
                }
                switch (take(*header, clip.path))
                {
                case Holding::Lacks:
                    ++built;
                    if (headers.size() <= MAX_LOGGED_CLIPS)
                    {
                        logger.info(
                            "AnimClips: new clip {} ({:.3f} s, {} controller(s), from {})",
                            clip.path,
                            facts_of(*header).duration,
                            facts_of(*header).controllers,
                            clip.pak
                        );
                    }
                    break;
                case Holding::Same:
                    ++unchanged;
                    break;
                case Holding::Differs:
                    ++pending;
                    break;
                }
            }
            save_clip_cache(clips);
            log_clip_findings();
            if (pending != 0 && !s_image_merged.load(std::memory_order_acquire))
            {
                logger.info(
                    "AnimClips: {} clip replacement(s) take effect on the next start (the game reads them in its "
                    "startup clip index)",
                    pending
                );
            }
            if (headers.empty())
            {
                logger.info(
                    "AnimClips: mods ship {} clip(s) and {} whole clip index(es); nothing to add ({:.1f} ms)",
                    clips.size(),
                    images.size(),
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count()
                );
                return 0;
            }

            std::uint32_t next = table.count;
            const bool loaded =
                load_clip_headers(s_clip_image_load, manager, "animations/animations__mod_clips.img", headers, &next);
            const auto elapsed =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            logger.info(
                "AnimClips: {} {} entr(ies): {} new clip(s) from .caf files ({} already as shipped), the rest from "
                "whole clip indexes ({} -> {}, {:.1f} ms)",
                loaded ? "appended" : "could not append",
                headers.size(),
                built,
                unchanged,
                table.count,
                next,
                elapsed
            );
            return next - table.count;
        }

        /**
         * @brief The clip index for the game's startup load: its own entries, every mod's changes to them from a whole
         *        `animations.img` copy (in load order), and every mod `.caf`. Empty when no mod changes anything, so
         *        the game loads as usual.
         * @details A mod `.caf` for a clip the game keeps in a .dba pack, or holds with a different length or
         *          controller count, switches that clip to the mod's loose file in its own slot. A new clip is added.
         *          A replacement in place keeps one entry per path, so the characters' clip lists resolve a name to one
         *          clip. A .dba that later lists a replaced clip logs "DBA/CAF mismatch" (a warning: Steam
         *          0x181AB5650 returns) and leaves the loose clip alone.
         */
        [[nodiscard]] std::vector<ClipHeader> build_startup_clip_image()
        {
            DMK::Logger &logger = DMK::log();
            std::vector<ClipHeader> entries = read_stock_clip_image();
            if (entries.empty())
            {
                logger.warning("AnimClips: the game's own clip index was not found; mods' clips register after it");
                return {};
            }
            const std::size_t stock_count = entries.size();
            std::unordered_map<std::uint64_t, std::size_t> slot;
            slot.reserve(entries.size());
            for (std::size_t i = 0; i < entries.size(); ++i)
            {
                slot[clip_path_hash(clip_header_path(entries[i]))] = i;
            }
            int from_copies_changed = 0;
            int from_copies_added = 0;
            // Which whole Animations.img each entry now comes from (empty: the game's own), so a mod's .caf that only
            // refines its own index entry is not reported as replacing a game clip.
            std::vector<std::string> origin(entries.size());
            const std::vector<WholeCopy> copies = read_mod_copies(mounted_mod_paks(), CLIP_IMAGE);
            // Each copy is compared with the game's own entry, so a later copy that keeps an entry as the game ships it
            // does not undo an earlier copy's change to it.
            const std::vector<ClipHeader> stock = copies.empty() ? std::vector<ClipHeader>{} : entries;
            for (const WholeCopy &copy : copies)
            {
                int changed = 0;
                int added = 0;
                for (const ClipHeader &entry : read_clip_image(std::as_bytes(std::span{copy.text})))
                {
                    const std::uint64_t hash = clip_path_hash(clip_header_path(entry));
                    const auto it = slot.find(hash);
                    if (it == slot.end())
                    {
                        slot[hash] = entries.size();
                        entries.push_back(entry);
                        origin.push_back(copy.modid);
                        ++added;
                    }
                    else if (it->second < stock.size() && !same_entry(stock[it->second], entry))
                    {
                        entries[it->second] = entry;
                        origin[it->second] = copy.modid;
                        ++changed;
                    }
                }
                logger.info(
                    "AnimClips: {}'s whole Animations.img: {} entr(ies) changed, {} added",
                    copy.modid,
                    changed,
                    added
                );
                from_copies_changed += changed;
                from_copies_added += added;
            }

            int replaced = 0;
            int refreshed = 0;
            int added = 0;
            const std::vector<ModClip> clips = find_mod_clips();
            for (const ModClip &clip : clips)
            {
                const std::optional<ClipHeader> header = build_mod_clip_header(clip);
                if (!header)
                {
                    logger.warning("AnimClips: {} (from {}) is not a compiled clip; skipped", clip.path, clip.pak);
                    continue;
                }
                const std::uint64_t hash = clip_path_hash(clip.path);
                const auto it = slot.find(hash);
                if (it == slot.end())
                {
                    slot[hash] = entries.size();
                    entries.push_back(*header);
                    origin.emplace_back();
                    ++added;
                    continue;
                }
                const ClipFacts before = facts_of(entries[it->second]);
                if (before.dba == 0 && !differs(before, facts_of(*header)))
                {
                    continue;
                }
                // Only the game's own entries can live in a .dba. An entry a mod's whole Animations.img added for a new
                // path that names a .dba (Bohemian Animator's KCD2AnimationIndex copies a template entry, "DBA home"
                // included) points the game at a pack that does not hold the clip, which plays as a silent bind pose:
                // the entry built from the .caf replaces it whatever the packed-clip setting.
                const bool game_entry = it->second < stock_count;
                if (before.dba != 0 && game_entry && !s_packed_replacements)
                {
                    logger.warning(
                        "AnimClips: {} replaces a packed clip, which needs the DBA/CAF mismatch check off; skipped",
                        clip.path
                    );
                    continue;
                }
                entries[it->second] = *header;
                const ClipFacts after = facts_of(*header);
                const std::string &owner = origin[it->second];
                if (!owner.empty() && owner == pak_modid(clip.pak))
                {
                    // The mod's own .caf and its own whole index disagree (typically the controller count): the entry
                    // built from the clip itself is the one the engine can play.
                    ++refreshed;
                    logger.debug(
                        "AnimClips: {}: {}'s Animations.img entry rebuilt from its .caf ({:.3f} s, {} -> {} "
                        "controller(s))",
                        clip.path,
                        owner,
                        after.duration,
                        before.controllers,
                        after.controllers
                    );
                    continue;
                }
                ++replaced;
                if (replaced <= static_cast<int>(MAX_LOGGED_CLIPS))
                {
                    const std::string what = !owner.empty()    ? owner + "'s clip"
                                             : before.dba != 0 ? std::string{"the game's packed clip"}
                                                               : std::string{"the game's clip"};
                    logger.info(
                        "AnimClips: {} replaces {} ({:.3f} s -> {:.3f} s, {} -> {} controller(s), from {})",
                        clip.path,
                        what,
                        before.duration,
                        after.duration,
                        before.controllers,
                        after.controllers,
                        clip.pak
                    );
                }
            }
            save_clip_cache(clips);
            log_clip_findings();
            if (refreshed != 0)
            {
                logger.info(
                    "AnimClips: {} mod clip(s) rebuilt their own mod's whole Animations.img entry from the .caf (same "
                    "clip; the entry the mod shipped disagreed with it)",
                    refreshed
                );
            }
            if (from_copies_changed + from_copies_added + replaced + refreshed + added == 0)
            {
                return {};
            }
            logger.info(
                "AnimClips: startup clip index: {} game entries, {} changed and {} added from whole copies, {} "
                "replaced and {} added from mods' .caf files",
                stock_count,
                from_copies_changed,
                from_copies_added,
                replaced,
                added
            );
            return entries;
        }

        /** @brief The game's own `animations/animations.img`, not a mod's `animations__<modid>.img`. */
        [[nodiscard]] bool is_stock_clip_image(std::string_view path) noexcept
        {
            return path_ends_with(path, CLIP_IMAGE) && path.find("__") == std::string_view::npos;
        }

        /**
         * @brief CAF image loader detour: the game's startup load of `animations/animations.img` gets the merged index
         *        (build_startup_clip_image) as an in-memory chunk file. Every other call, and a startup with nothing to
         *        merge, loads as usual.
         * @note The main thread, inside the clip index init, before any animation set or .dba exists.
         */
        bool __fastcall
        detour_clip_image_load(void *manager, const char *path, void *chunk_file, std::uint32_t *count) noexcept
        {
            const ClipImageLoadFunc original = s_clip_image_load_original.load(std::memory_order_acquire);
            if (original == nullptr)
            {
                return false;
            }
            if (path == nullptr || count == nullptr || !is_stock_clip_image(path))
            {
                return original(manager, path, chunk_file, count);
            }
            const std::uint32_t start = *count;
            try
            {
                mount_dev_paks_once();
                const std::vector<ClipHeader> image = build_startup_clip_image();
                if (!image.empty())
                {
                    const bool loaded = load_clip_headers(original, manager, path, image, count);
                    s_image_merged.store(loaded, std::memory_order_release);
                    DMK::log().info(
                        "AnimClips: the game loaded the merged clip index ({} entries, {})",
                        image.size(),
                        loaded ? "ok" : "failed"
                    );
                    if (loaded)
                    {
                        return true;
                    }
                }
            }
            catch (...)
            {
                (void)DMK::log().log_noexcept(DMK::LogLevel::Error, "AnimClips: merging the startup clip index failed");
            }
            // The game's own file then loads from the same first entry, over anything a failed merged load wrote.
            *count = start;
            return original(manager, path, chunk_file, count);
        }

        using FacialIndexLoadFunc = std::uint32_t(__fastcall *)(void *manager);
        std::atomic<FacialIndexLoadFunc> s_facial_index_load_original{nullptr};
        std::atomic<bool> s_indexes_appended{false};

        /**
         * @brief Facial index load detour: the clip index init runs it right after animations.img, before any
         *        character's animation set exists, and again after a language change rebuilds the facial part. Mods'
         *        indexes append right after it, so their clips are in place before the sets are built.
         * @note The main thread, inside the clip index init.
         */
        std::uint32_t __fastcall detour_facial_index_load(void *manager) noexcept
        {
            const FacialIndexLoadFunc original = s_facial_index_load_original.load(std::memory_order_acquire);
            const std::uint32_t result = original != nullptr ? original(manager) : 0;
            try
            {
                mount_dev_paks_once();
                (void)append_clip_indexes(manager);
                if (!s_image_merged.load(std::memory_order_acquire))
                {
                    (void)append_mod_clip_headers(manager, nullptr);
                }
                s_clip_crcs = clip_crcs(find_mod_clips());
                s_indexes_appended.store(true, std::memory_order_release);
            }
            catch (...)
            {
                (void)DMK::log().log_noexcept(DMK::LogLevel::Error, "AnimClips: appending mods' clip indexes failed");
            }
            return result;
        }

        /**
         * @brief Has the engine reload each of @p paths from the newest pak with it, in place (ReloadCAF): the
         *        header keeps its index, so the animation sets need no rebuild, and a clip packed in a .dba becomes
         *        a loose one.
         * @note The main thread, from the frame tick, where the late header append also runs.
         */
        void reload_clips_in_place(void *manager, const std::vector<std::string> &paths)
        {
            if (paths.empty())
            {
                return;
            }
            DMK::Logger &logger = DMK::log();
            const auto started = std::chrono::steady_clock::now();
            std::unordered_map<std::uint64_t, std::uintptr_t> headers;
            headers.reserve(paths.size());
            for (const std::string &path : paths)
            {
                headers.emplace(clip_path_hash(path), 0);
            }
            const HeaderTable table = header_table(manager);
            for (std::uint32_t i = 0; i < table.count; ++i)
            {
                const std::uintptr_t header = table.base + i * GLOBAL_HEADER_STRIDE;
                const auto hash = header_field<std::uint64_t>(header, GLOBAL_HEADER_HASH_OFFSET);
                // ReloadCAF reloads the first header with the hash (its search starts at index 0), so the flags and
                // path record this function reads and restores must be that header's.
                if (const auto it = headers.find(hash); it != headers.end() && it->second == 0)
                {
                    it->second = header;
                }
            }

            int reloaded = 0;
            int failed = 0;
            int busy = 0;
            for (const std::string &path : paths)
            {
                const std::uintptr_t header = headers[clip_path_hash(path)];
                if (header == 0)
                {
                    ++failed;
                    logger.warning("AnimClips: {} has no header to reload", path);
                    continue;
                }
                if (is_streaming(header))
                {
                    ++busy;
                    logger.info("AnimClips: {} is being streamed in; reload again to replace it", path);
                    continue;
                }
                const bool packed = header_field<std::uint32_t>(header, GLOBAL_HEADER_DBA_OFFSET) != 0;
                const float before = header_field<float>(header, GLOBAL_HEADER_DURATION_OFFSET);
                if (header_field<std::int32_t>(header, GLOBAL_HEADER_PATH_RECORD_OFFSET) < 0)
                {
                    char *text = leaked_engine_string(path);
                    if (text == nullptr)
                    {
                        ++failed;
                        logger.warning("AnimClips: no memory for the path of {}", path);
                        continue;
                    }
                    (void)vcall<void *>(reinterpret_cast<void *>(header), GLOBAL_HEADER_SET_FILE_PATH_SLOT, &text);
                }
                const auto flags_before = header_field<std::uint32_t>(header, GLOBAL_HEADER_FLAGS_OFFSET);
                const auto loaded_before = header_field<std::uint16_t>(header, GLOBAL_HEADER_LOADED_CONTROLLERS_OFFSET);
                const int result = vcall<int>(manager, CM_RELOAD_CAF_SLOT, path.c_str());
                const float after = header_field<float>(header, GLOBAL_HEADER_DURATION_OFFSET);
                if (result != RELOAD_CAF_OK)
                {
                    const DMK::Address flags_at{header + GLOBAL_HEADER_FLAGS_OFFSET};
                    const DMK::Address loaded_at{header + GLOBAL_HEADER_LOADED_CONTROLLERS_OFFSET};
                    const bool restored = DMK::memory::write_in_place(flags_at, flags_before).has_value() &&
                                          DMK::memory::write_in_place(loaded_at, loaded_before).has_value();
                    ++failed;
                    logger.warning(
                        "AnimClips: the engine could not reload {} (result {}); {}",
                        path,
                        result,
                        restored ? "the clip is left as it was" : "its header could not be restored"
                    );
                    continue;
                }
                ++reloaded;
                if (reloaded <= static_cast<int>(MAX_LOGGED_CLIPS))
                {
                    logger.info(
                        "AnimClips: reloaded {} in place ({}{:.3f} s -> {:.3f} s, {} controller(s))",
                        path,
                        packed ? "was packed, " : "",
                        before,
                        after,
                        header_field<std::uint16_t>(header, GLOBAL_HEADER_CONTROLLERS_OFFSET)
                    );
                }
            }
            const auto elapsed =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            logger.info(
                "AnimClips: {} clip(s) reloaded in place, {} failed, {} still streaming ({:.1f} ms)",
                reloaded,
                failed,
                busy,
                elapsed
            );
        }

        /**
         * @brief How many clip headers have a stream in flight.
         * @details Adding clips can move the whole header table (the loader reserves room past the new count), and a
         *          stream completing on a streaming thread writes into its header. Animation jobs are no hazard here:
         *          the frame tick runs at the start of the system update, and the engine syncs every animation job at
         *          the end of the frame before (CCryAction::PostUpdate -> SyncAllAnimations). New streams start only
         *          from the main thread, so none can begin between this count and the append.
         */
        [[nodiscard]] int streaming_clip_count(void *manager) noexcept
        {
            const HeaderTable table = header_table(manager);
            int streaming = 0;
            for (std::uint32_t i = 0; i < table.count; ++i)
            {
                streaming += is_streaming(table.base + i * GLOBAL_HEADER_STRIDE) ? 1 : 0;
            }
            return streaming;
        }

        /** @brief The CharacterManager once it has loaded its clip index, else nullptr. */
        [[nodiscard]] void *clip_index_manager() noexcept
        {
            void *manager = reinterpret_cast<void *>(genv_member(constants::GENV_CHARACTER_MANAGER_OFFSET));
            const std::int32_t ready =
                manager != nullptr
                    ? read_value<std::int32_t>(reinterpret_cast<std::uintptr_t>(manager) + CM_HEADERS_READY_OFFSET)
                          .value_or(0)
                    : 0;
            return ready != 0 ? manager : nullptr;
        }

        /**
         * @brief The late path, for a generation that starts after the clip index loaded (a DEV hot reload): append
         *        the mods' indexes and clips, rebuild the animation sets and reload replaced clips in place.
         * @return False while the clip index is not loaded yet or a clip is streaming in, so the frame tick tries
         *         again on a later frame.
         */
        [[nodiscard]] bool register_clips_late()
        {
            void *manager = clip_index_manager();
            if (manager == nullptr)
            {
                if (!s_clip_index_wait_logged)
                {
                    s_clip_index_wait_logged = true;
                    DMK::log().info("AnimClips: the clip index is not loaded yet; mods' indexes append when it loads");
                }
                return false;
            }
            if (streaming_clip_count(manager) != 0)
            {
                return false;
            }
            mount_dev_paks_once();
            const std::uint32_t from_images = append_clip_indexes(manager);
            std::vector<std::string> reloads;
            const std::uint32_t from_clips = append_mod_clip_headers(manager, &reloads);
            const bool new_blend_spaces = take_new_blend_spaces();
            if (from_images + from_clips != 0 || new_blend_spaces)
            {
                reload_animation_sets(manager);
            }
            reload_clips_in_place(manager, reloads);
            s_clip_crcs = clip_crcs(find_mod_clips());
            return true;
        }

        constexpr const char *RELOAD_CLIPS_HELP =
            "Animation Framework: picks up clips without a restart. Packs the loose .caf, .bspace and .comb files "
            "under KCD2_AnimationFramework_Dev\\Animations\\ (at their game paths), mounts the DEV folder's new paks, "
            "adds the clips the game lacks and reloads each changed clip in place.";
        std::atomic<bool> s_clip_reload_requested{false};
        // Frames a clip reload has waited for clips streaming in, and how many it waits at most (about 10 s at 60 fps).
        int s_reload_wait_frames = 0;
        constexpr int RELOAD_WAIT_FRAMES = 600;
        // Main thread only (the frame tick and the shutdown).
        bool s_reload_command_added = false;

        void __fastcall reload_clips_command(void * /*args*/) noexcept
        {
            s_clip_reload_requested.store(true, std::memory_order_release);
        }

        /**
         * @brief The reload animfw_reload_clips asks for: mounts the DEV folder's new paks and a new pack of its loose
         *        clips, adds the clips the game lacks (rebuilding the animation sets' lists) and reloads in place each
         *        clip whose file changed since the clips last registered.
         * @return False to try again on a later frame: a clip is streaming in (see streaming_clip_count).
         * @note The main thread, from the frame tick, once the clip index has loaded.
         */
        [[nodiscard]] bool reload_clips_now()
        {
            DMK::Logger &logger = DMK::log();
            if (!s_mod_clips_on.load(std::memory_order_acquire))
            {
                logger.info("AnimClips: mods' clips are off; nothing to reload");
                return true;
            }
            void *manager = clip_index_manager();
            if (manager == nullptr)
            {
                logger.info("AnimClips: the clip index is not loaded yet; nothing to reload");
                return true;
            }
            if (const int streaming = streaming_clip_count(manager); streaming != 0)
            {
                if (s_reload_wait_frames == 0)
                {
                    logger.info("AnimClips: {} clip(s) are streaming in; the clip reload waits for them", streaming);
                }
                return false;
            }
            const auto started = std::chrono::steady_clock::now();
            const int mounted = mount_dev_paks();
            std::vector<std::string> held;
            const std::uint32_t added = append_mod_clip_headers(manager, &held);
            std::unordered_map<std::string, std::uint32_t> crcs = clip_crcs(find_mod_clips());
            std::vector<std::string> changed;
            for (std::string &path : held)
            {
                const auto known = s_clip_crcs.find(path);
                const auto now = crcs.find(path);
                if (known == s_clip_crcs.end() || now == crcs.end() || known->second != now->second)
                {
                    changed.push_back(std::move(path));
                }
            }
            // An animation set reload is also what makes the game read its event lists again (through the patch merge).
            const bool events_changed = take_event_patch_change();
            if (events_changed)
            {
                logger.info("AnimClips: the DEV folder's event lists (.animevents) changed; the animation sets reload");
            }
            const bool new_blend_spaces = take_new_blend_spaces();
            if (added != 0 || new_blend_spaces || events_changed)
            {
                reload_animation_sets(manager);
            }
            reload_clips_in_place(manager, changed);
            s_clip_crcs = std::move(crcs);
            logger.info(
                "AnimClips: clip reload: {} pak(s) mounted, {} clip(s) added, {} changed clip(s) reloaded ({:.1f} ms)",
                mounted,
                added,
                changed.size(),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count()
            );
            return true;
        }
    } // namespace

    void run_anim_patch_frame_work() noexcept
    {
        try
        {
            report_merge_times();
        }
        catch (...)
        {
            (void)DMK::log().log_noexcept(DMK::LogLevel::Error, "AnimPatch: the load time report failed");
        }
        if (!s_reload_command_added && console_ready())
        {
            add_console_command(RELOAD_CLIPS_COMMAND, &reload_clips_command, RELOAD_CLIPS_HELP);
            s_reload_command_added = true;
        }
        // A reload waits for the clip registration of this generation, which the late path below finishes.
        if (!s_clip_work_pending.load(std::memory_order_acquire) &&
            s_clip_reload_requested.load(std::memory_order_acquire))
        {
            bool finished = true;
            try
            {
                finished = reload_clips_now();
            }
            catch (...)
            {
                (void)DMK::log().log_noexcept(DMK::LogLevel::Error, "AnimClips: the clip reload failed");
            }
            if (!finished && ++s_reload_wait_frames >= RELOAD_WAIT_FRAMES)
            {
                (void)DMK::log().log_noexcept(
                    DMK::LogLevel::Warning,
                    "AnimClips: clips kept streaming in; the clip reload gave up (run animfw_reload_clips again)"
                );
                finished = true;
            }
            if (finished)
            {
                s_clip_reload_requested.store(false, std::memory_order_release);
                s_reload_wait_frames = 0;
            }
        }
        if (!s_clip_work_pending.load(std::memory_order_acquire))
        {
            return;
        }
        // The facial index hook appended everything inside the clip index init, so the late path has nothing to do.
        if (s_indexes_appended.load(std::memory_order_acquire))
        {
            s_clip_work_pending.store(false, std::memory_order_relaxed);
            return;
        }
        try
        {
            if (register_clips_late())
            {
                s_clip_work_pending.store(false, std::memory_order_relaxed);
            }
        }
        catch (...)
        {
            s_clip_work_pending.store(false, std::memory_order_relaxed);
            (void)DMK::log().log_noexcept(DMK::LogLevel::Error, "AnimClips: clip registration failed");
        }
    }

    void install_anim_patch_hooks(const Anchors &anchors, const Settings &settings, DMK::hook::HookStack &hooks)
    {
        DMK::Logger &logger = DMK::log();
        s_genv.store(anchors.genv, std::memory_order_release);
        s_add_view_tag = settings.third_person_variants;
        if (anchors.genv == 0)
        {
            logger.warning("AnimPatch: gEnv did not resolve; animation patches and mod clips are off");
            return;
        }

        if (!settings.animation_patches)
        {
            logger.info("AnimPatch: off in the INI");
        }
        else if (anchors.xml_load_file == 0)
        {
            logger.warning("AnimPatch: the database loader was not found; animation patches are off");
        }
        else if (
            auto installed = install_inline_hook<&detour_load_xml_file>(
                hooks,
                "XmlLoadFile",
                anchors.xml_load_file,
                s_load_xml_file_original
            );
            !installed.has_value()
        )
        {
            logger.warning("AnimPatch: hook failed ({}); animation patches are off", installed.error().message());
        }
        else
        {
            logger.info(
                "AnimPatch: delta .adb files merge into the databases the game loads (XML loader at {}, DEV "
                "folder {})",
                DMK::format::format_address(anchors.xml_load_file),
                dev_patch_dir().string()
            );
        }

        if (!settings.skeleton_merge)
        {
            logger.info("SkeletonMerge: off in the INI");
        }
        else if (anchors.chunk_file_read == 0 || anchors.chunk_file_read_from_memory == 0)
        {
            logger.warning(
                "SkeletonMerge: the chunk file reads were not found; skeletons load as the last mod ships them"
            );
        }
        else
        {
            s_chunk_file_read_from_memory =
                reinterpret_cast<ChunkFileReadFromMemoryFunc>(anchors.chunk_file_read_from_memory);
            if (auto installed = install_inline_hook<&detour_chunk_file_read>(
                    hooks,
                    "ChunkFileRead",
                    anchors.chunk_file_read,
                    s_chunk_file_read_original
                );
                installed.has_value())
            {
                s_skeleton_merge_on.store(true, std::memory_order_relaxed);
                logger.info(
                    "SkeletonMerge: skeletons (.chr) two or more mods ship whole merge bone by bone (chunk file reads "
                    "at {})",
                    DMK::format::format_address(anchors.chunk_file_read)
                );
            }
            else
            {
                logger.warning(
                    "SkeletonMerge: hook failed ({}); skeletons load as the last mod ships them",
                    installed.error().message()
                );
            }
        }

        if (!settings.mod_clips)
        {
            logger.info("AnimClips: off in the INI");
            return;
        }
        if (anchors.clip_image_load == 0 || anchors.chrparams_reload == 0)
        {
            logger.warning("AnimClips: the clip index loader was not found; mods' clips are not registered");
            return;
        }
        s_clip_image_load = reinterpret_cast<ClipImageLoadFunc>(anchors.clip_image_load);
        s_chrparams_reload = reinterpret_cast<ChrParamsReloadFunc>(anchors.chrparams_reload);
        // The .dba loader ends the game when a clip it lists is indexed as a loose .caf, although the code after the
        // check already leaves such a clip alone. A mod's loose replacement of a packed clip is indexed exactly that
        // way, so the check's je over the error becomes a jmp. The patch stays at shutdown: a restore while a replaced
        // clip is indexed brings the error back the next time its .dba loads.
        if (const std::uintptr_t check = anchors.dba_mismatch_check; check != 0)
        {
            const auto current = DMK::memory::read<std::uint8_t>(DMK::Address{check});
            if (current && (*current == 0x74 || *current == 0xEB))
            {
                const std::array<std::byte, 1> jump{std::byte{0xEB}};
                const auto patched =
                    *current == 0xEB ? DMK::Result<void>{} : DMK::memory::patch_code(DMK::Address{check}, jump);
                if (patched.has_value())
                {
                    logger.info(
                        "AnimClips: the .dba loader skips clips mods replace with loose files (DBA/CAF "
                        "mismatch check at {} off)",
                        DMK::format::format_address(check)
                    );
                }
                else
                {
                    logger.warning(
                        "AnimClips: could not turn the DBA/CAF mismatch check off ({}); mods' "
                        "replacements of packed clips are off",
                        patched.error().message()
                    );
                    s_packed_replacements = false;
                }
            }
            else
            {
                logger.warning(
                    "AnimClips: the DBA/CAF mismatch check holds an unexpected byte; mods' replacements of "
                    "packed clips are off"
                );
                s_packed_replacements = false;
            }
        }
        else
        {
            logger.warning(
                "AnimClips: the DBA/CAF mismatch check was not found; mods' replacements of packed clips are "
                "off"
            );
            s_packed_replacements = false;
        }
        if (auto installed = install_inline_hook<&detour_clip_image_load>(
                hooks,
                "ClipImageLoad",
                anchors.clip_image_load,
                s_clip_image_load_original
            );
            installed.has_value())
        {
            s_clip_image_load = s_clip_image_load_original.load(std::memory_order_acquire);
        }
        else
        {
            logger.warning(
                "AnimClips: clip index hook failed ({}); mods' replacements of the game's clips are off",
                installed.error().message()
            );
        }
        if (anchors.facial_index_load == 0)
        {
            logger.warning("AnimClips: the facial index load was not found; mods' clip indexes register late only");
        }
        else if (
            auto installed = install_inline_hook<&detour_facial_index_load>(
                hooks,
                "FacialIndexLoad",
                anchors.facial_index_load,
                s_facial_index_load_original
            );
            !installed.has_value()
        )
        {
            logger.warning(
                "AnimClips: facial index hook failed ({}); mods' clip indexes register late only",
                installed.error().message()
            );
        }
        s_mod_clips_on.store(true, std::memory_order_release);
        s_clip_work_pending.store(true, std::memory_order_release);
    }

    void request_clip_reload() noexcept
    {
        s_clip_reload_requested.store(true, std::memory_order_release);
    }

    void remove_anim_patch_commands() noexcept
    {
        if (s_reload_command_added)
        {
            remove_console_command(RELOAD_CLIPS_COMMAND);
        }
        s_reload_command_added = false;
        s_clip_reload_requested.store(false, std::memory_order_relaxed);
    }

    namespace
    {
        /** @brief The chunks of @p path as the game reads it, handed to @p decode while the chunk file holds them. */
        template <typename Decode>
        [[nodiscard]] auto decode_engine_file(const std::string &path, Decode decode)
            -> decltype(decode(std::span<const ChunkView>{}))
        {
            const ChunkFileRef chunk_file;
            if (!chunk_file)
            {
                return std::unexpected{std::string{"the game's chunk file reader is not available"}};
            }
            if (!vcall<bool>(chunk_file.get(), constants::CHUNK_FILE_READ_SLOT, path.c_str()))
            {
                return std::unexpected{std::format("the game cannot read {}", path)};
            }
            std::vector<ChunkView> chunks;
            const int count = vcall<int>(chunk_file.get(), CHUNK_FILE_COUNT_SLOT);
            for (int i = 0; i < count; ++i)
            {
                const auto *chunk = vcall<const ChunkDesc *>(chunk_file.get(), CHUNK_FILE_GET_SLOT, i);
                if (chunk != nullptr && chunk->data != nullptr && !chunk->swap_endian)
                {
                    chunks.push_back(
                        ChunkView{
                            .type = chunk->type,
                            .version = chunk->version,
                            .data =
                                {static_cast<const std::byte *>(chunk->data), static_cast<std::size_t>(chunk->size)},
                        }
                    );
                }
            }
            return decode(std::span<const ChunkView>{chunks});
        }

        /** @brief The `.dba` pack whose path's CRC-32 is @p crc, from the mounted paks (the newest first). */
        [[nodiscard]] std::optional<std::string> pack_with_crc(std::uint32_t crc)
        {
            std::vector<std::filesystem::path> bases;
            const std::vector<ModPak> mods = mounted_mod_paks(&bases);
            std::vector<std::filesystem::path> paks;
            for (auto it = mods.rbegin(); it != mods.rend(); ++it)
            {
                paks.push_back(it->file);
            }
            paks.insert(paks.end(), bases.rbegin(), bases.rend());
            for (const std::filesystem::path &pak : paks)
            {
                const auto index = zip_index_of(pak);
                if (!index)
                {
                    continue;
                }
                for (const ZipEntry &entry : *index)
                {
                    if (entry.name.ends_with(".dba") &&
                        zip_crc32({reinterpret_cast<const std::uint8_t *>(entry.name.data()), entry.name.size()}) ==
                            crc)
                    {
                        return entry.name;
                    }
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] std::expected<FoundClip, std::string> read_found_clip(std::string path, std::string source)
        {
            auto clip =
                decode_engine_file(path, [](std::span<const ChunkView> chunks) { return read_caf_clip(chunks); });
            if (!clip)
            {
                return std::unexpected{std::format("{}: {}", path, clip.error())};
            }
            clip->name = path;
            return FoundClip{.clip = std::move(*clip), .path = std::move(path), .source = std::move(source)};
        }

        /**
         * @brief The clip the game plays by @p name (its file name without `.caf`, or its path): a mod's clip as the
         *        newest pak ships it, else the game's, loose or out of its `.dba` pack.
         * @note Main thread, once the game has loaded its clip index.
         */
        [[nodiscard]] std::expected<FoundClip, std::string> find_clip(std::string_view name)
        {
            std::string wanted = lowercase(name);
            std::ranges::replace(wanted, '\\', '/');
            const bool by_path = wanted.find('/') != std::string::npos;
            if (by_path && !wanted.ends_with(".caf"))
            {
                wanted += ".caf";
            }
            const auto matches = [&](std::string_view path)
            { return by_path ? path == wanted : clip_stem(path) == wanted; };

            for (const ModClip &clip : find_mod_clips())
            {
                if (matches(clip.path))
                {
                    return read_found_clip(clip.path, clip.pak);
                }
            }
            for (const ClipHeader &header : read_stock_clip_image())
            {
                std::string path = lowercase(clip_header_path(header));
                if (!matches(path))
                {
                    continue;
                }
                std::uint32_t pack = 0;
                std::memcpy(&pack, header.data() + CLIP_HEADER_DBA_CRC_OFFSET, sizeof(pack));
                if (pack == 0)
                {
                    return read_found_clip(std::move(path), "the game");
                }
                const std::optional<std::string> dba = pack_with_crc(pack);
                if (!dba)
                {
                    return std::unexpected{std::format("{} is packed in a .dba no mounted pak holds", path)};
                }
                const std::vector<std::string> stems{clip_stem(path)};
                auto clips = decode_engine_file(
                    *dba,
                    [&stems](std::span<const ChunkView> chunks) { return read_dba_chunks(chunks, stems); }
                );
                if (!clips)
                {
                    return std::unexpected{std::format("{}: {}", *dba, clips.error())};
                }
                const auto it =
                    std::ranges::find_if(*clips, [&path](const CafClip &clip) { return lowercase(clip.name) == path; });
                if (it == clips->end())
                {
                    return std::unexpected{std::format("{} does not hold {}", *dba, path)};
                }
                return FoundClip{.clip = std::move(*it), .path = std::move(path), .source = *dba};
            }
            return std::unexpected{std::format("{}{}", NO_CLIP_ERROR_PREFIX, name)};
        }
    } // namespace

    ModToolGame running_game_tools()
    {
        return ModToolGame{
            .files = animation_catalog_files(),
            .find_clip = [](std::string_view name) { return find_clip(name); },
            .skeleton = [] { return male_skeleton(); },
        };
    }

    std::filesystem::path animation_dev_folder()
    {
        return dev_patch_dir();
    }

    std::optional<std::string> game_file_folder(std::string_view file_name)
    {
        std::vector<std::filesystem::path> bases;
        // Animation patches patch files of the animation paks, and the framework's table rows Tables.pak's.
        constexpr std::array<std::string_view, 2> patched_paks{
            "animations",
            "tables",
        };
        (void)mounted_mod_paks(&bases, patched_paks);
        for (const std::filesystem::path &pak : bases)
        {
            const auto index = zip_index_of(pak);
            if (!index)
            {
                continue;
            }
            for (const ZipEntry &entry : *index)
            {
                const std::size_t slash = entry.name.rfind('/');
                if (slash != std::string::npos && std::string_view{entry.name}.substr(slash + 1) == file_name)
                {
                    return entry.name.substr(0, slash + 1);
                }
            }
        }
        return std::nullopt;
    }

    AnimCatalogFiles animation_catalog_files()
    {
        std::vector<std::filesystem::path> base_paks;
        auto mods = std::make_shared<const std::vector<ModPak>>(mounted_mod_paks(&base_paks));
        auto bases = std::make_shared<const std::vector<std::filesystem::path>>(std::move(base_paks));
        return AnimCatalogFiles{
            .read = [mods, bases](const std::string &path) -> std::optional<std::string>
            {
                for (auto mod = mods->rbegin(); mod != mods->rend(); ++mod)
                {
                    const auto index = zip_index_of(mod->file);
                    const ZipEntry *entry = index ? find_entry(*index, path) : nullptr;
                    if (entry == nullptr)
                    {
                        continue;
                    }
                    if (std::optional<std::string> text = read_zip_file(mod->file, *entry))
                    {
                        return text;
                    }
                }
                return read_base_file(*bases, path);
            },
            .patches = [mods](const std::string &path) -> std::vector<std::string>
            {
                const std::size_t dot = path.find_last_of('.');
                const std::string prefix = path.substr(0, dot) + "__";
                const std::string extension = dot == std::string::npos ? std::string{} : path.substr(dot);
                std::vector<std::string> texts;
                for (const ModPak &mod : *mods)
                {
                    const auto index = zip_index_of(mod.file);
                    if (!index)
                    {
                        continue;
                    }
                    std::vector<const ZipEntry *> entries;
                    for (const ZipEntry &entry : *index)
                    {
                        if (entry.name.starts_with(prefix) && entry.name.ends_with(extension))
                        {
                            entries.push_back(&entry);
                        }
                    }
                    std::ranges::sort(entries, {}, &ZipEntry::name);
                    for (const ZipEntry *entry : entries)
                    {
                        if (std::optional<std::string> text = read_zip_file(mod.file, *entry))
                        {
                            texts.push_back(std::move(*text));
                        }
                    }
                }
                const std::string name_prefix = prefix.substr(prefix.find_last_of('/') + 1);
                for (const std::filesystem::path &file : dev_loose_patches(name_prefix, extension))
                {
                    std::ifstream in{file, std::ios::binary};
                    texts.emplace_back(std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{});
                }
                return texts;
            },
        };
    }
} // namespace AnimationFramework
