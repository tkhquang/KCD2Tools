/**
 * @file hooks/twin_inventory.cpp
 * @brief Implementation of the level inventory of plant and frame materials (see twin_inventory.hpp).
 *
 * @details Every engine read sits in a small POD reader under SEH, so a layout drift or a table the engine is freeing
 *          reads as a fault that skips the entry, never a crash. Class checks go through RTTI once per walk: the
 *          engine and the object manager by their own vtables, each material against the first vtable that proved to
 *          be CMatInfo's (DMK::rtti::vtable_is_type queries the loader, so it is not run per material).
 */

#include "twin_inventory.hpp"
#include "camera_rules_files.hpp"
#include "constants.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace TPVCamera
{

    namespace
    {
        // The scopes the inventory hands back: the character's twins come from his own material loads.
        constexpr std::uintptr_t k_listed_scopes = Rules::TWIN_PLANTS | Rules::TWIN_FRAMES;

        static_assert(Constants::OBJMAN_GROUPS_END_OFFSET - Constants::OBJMAN_GROUPS_BEGIN_OFFSET ==
                          static_cast<std::ptrdiff_t>(sizeof(std::uintptr_t)),
                      "The vegetation groups are a std::vector: its end pointer follows its begin pointer.");
        static_assert((Constants::TWIN_INVENTORY_MATERIAL_SET & (Constants::TWIN_INVENTORY_MATERIAL_SET - 1)) == 0,
                      "The dedupe set's size must be a power of two.");

        [[nodiscard]] constexpr char lower(char c) noexcept
        {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
        }

        /**
         * @brief Canonicalizes an engine string while its owner keeps it alive: lower case, forward slashes.
         * @param backslash Set when the string held a backslash.
         */
        [[nodiscard]] bool copy_asset_path_guarded(const char *path, char *out, std::size_t capacity,
                                                   bool &backslash) noexcept
        {
            __try
            {
                if (path == nullptr || capacity == 0)
                {
                    return false;
                }
                for (std::size_t i = 0; i < capacity; ++i)
                {
                    const char c = path[i];
                    backslash = backslash || c == '\\';
                    out[i] = c == '\\' ? '/' : lower(c);
                    if (c == '\0')
                    {
                        return true;
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
            if (capacity != 0)
            {
                out[0] = '\0';
            }
            return false;
        }

        /** @brief The scopes @p rules (null: none) give the asset path @p path; see twin_asset_scopes(). */
        [[nodiscard]] std::uintptr_t scopes_of(const Rules::RuleSet *rules, const char *path) noexcept
        {
            std::array<char, 512> normalized;
            bool backslash = false;
            if (!copy_asset_path_guarded(path, normalized.data(), normalized.size(), backslash))
            {
                return 0;
            }
            const std::string_view name{normalized.data()};
            if (backslash)
            {
                Rules::log_name_form(Rules::NameSite::Twin, Rules::NameForm::Backslash, name,
                                     Rules::count_name_form(Rules::NameSite::Twin, Rules::NameForm::Backslash));
            }
            if (name.starts_with('/'))
            {
                Rules::log_name_form(Rules::NameSite::Twin, Rules::NameForm::LeadingSlash, name,
                                     Rules::count_name_form(Rules::NameSite::Twin, Rules::NameForm::LeadingSlash));
            }
            return rules != nullptr ? rules->twin_scopes(name) : 0;
        }

        /** @brief The pointer stored at @p address, or 0 on a fault. SEH-guarded, POD body. */
        [[nodiscard]] std::uintptr_t read_pointer_guarded(std::uintptr_t address) noexcept
        {
            __try
            {
                return *reinterpret_cast<const std::uintptr_t *>(address);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }

        /** @brief A table's elements: the first one's address and how many there are. */
        struct TableSpan
        {
            std::uintptr_t begin = 0;
            std::size_t count = 0;
        };

        enum class TableRead : std::uint8_t
        {
            Read,
            Empty,
            Implausible, // an end before its begin, a partial element, or more elements than the limit
            Fault,
        };

        /**
         * @brief Reads the {begin, end} pointers at @p header of a std::vector whose elements are @p stride bytes, and
         *        checks them: an end at or past its begin, a whole number of elements, at most @p max of them.
         * @note SEH-guarded, POD body.
         */
        [[nodiscard]] TableRead read_table_guarded(std::uintptr_t header, std::size_t stride, std::size_t max,
                                                   TableSpan &out) noexcept
        {
            __try
            {
                const auto begin = *reinterpret_cast<const std::uintptr_t *>(header);
                const auto end = *reinterpret_cast<const std::uintptr_t *>(header + sizeof(std::uintptr_t));
                if (begin == end)
                {
                    return TableRead::Empty;
                }
                if (begin == 0 || end < begin || (end - begin) % stride != 0 || (end - begin) / stride > max)
                {
                    return TableRead::Implausible;
                }
                out.begin = begin;
                out.count = (end - begin) / stride;
                return TableRead::Read;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return TableRead::Fault;
            }
        }

        /** @brief What the walk reads of a material (see Constants::MATINFO_SUBMTL_ARRAY_OFFSET). */
        struct MaterialView
        {
            std::uintptr_t vtable = 0;
            std::uint32_t flags = 0;
            std::uintptr_t subs = 0; // its sub-material array, only for a multi-material
            int sub_count = 0;
        };

        /** @brief Reads @p material's vtable, flags and, for a multi-material, its sub-material array. SEH-guarded. */
        [[nodiscard]] bool read_material_guarded(std::uintptr_t material, MaterialView &out) noexcept
        {
            __try
            {
                out.vtable = *reinterpret_cast<const std::uintptr_t *>(material);
                out.flags = *reinterpret_cast<const std::uint32_t *>(material + Constants::MATINFO_FLAGS_OFFSET);
                out.subs = 0;
                out.sub_count = 0;
                if ((out.flags & Constants::MTL_FLAG_MULTI_SUBMTL) != 0)
                {
                    out.subs =
                        *reinterpret_cast<const std::uintptr_t *>(material + Constants::MATINFO_SUBMTL_ARRAY_OFFSET);
                    out.sub_count =
                        out.subs != 0
                            ? *reinterpret_cast<const std::int32_t *>(out.subs - Constants::DYNARRAY_COUNT_BACK)
                            : 0;
                }
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        /** @brief What the walk reads of a statobj: its vtable, .cgf path, material and compound sub-objects. */
        struct StatObjView
        {
            std::uintptr_t vtable = 0;
            const char *path = nullptr;
            std::uintptr_t material = 0;
            std::uintptr_t children = 0; // the first SSubObject, or 0
            int child_count = 0;         // at most Constants::SUBOBJ_MAX
        };

        /** @brief Reads @p statobj's fields the walk needs. SEH-guarded, POD body. */
        [[nodiscard]] bool read_statobj_guarded(std::uintptr_t statobj, StatObjView &out) noexcept
        {
            __try
            {
                out.vtable = *reinterpret_cast<const std::uintptr_t *>(statobj);
                out.path = *reinterpret_cast<const char *const *>(statobj + Constants::STATOBJ_CGF_NAME_OFFSET);
                out.material = *reinterpret_cast<const std::uintptr_t *>(statobj + Constants::STATOBJ_MATERIAL_OFFSET);
                const auto begin =
                    *reinterpret_cast<const std::uintptr_t *>(statobj + Constants::STATOBJ_SUBOBJ_BEGIN_OFFSET);
                const auto end =
                    *reinterpret_cast<const std::uintptr_t *>(statobj + Constants::STATOBJ_SUBOBJ_END_OFFSET);
                out.children = 0;
                out.child_count = 0;
                if (begin != 0 && end > begin && (end - begin) % Constants::SUBOBJ_STRIDE == 0)
                {
                    out.children = begin;
                    out.child_count = static_cast<int>(
                        std::min<std::size_t>((end - begin) / Constants::SUBOBJ_STRIDE, Constants::SUBOBJ_MAX));
                }
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        /** @brief True when @p object's vtable carries the RTTI name @p mangled (one loader query; setup-style). */
        [[nodiscard]] bool object_has_type(std::uintptr_t object, std::string_view mangled) noexcept
        {
            const std::uintptr_t vtable = object != 0 ? read_pointer_guarded(object) : 0;
            return vtable != 0 && DMK::rtti::vtable_is_type(DMK::Address{vtable}, mangled);
        }

        /**
         * @class Walk
         * @brief One walk of the level tables: the materials expanded so far and the output.
         */
        class Walk
        {
        public:
            Walk(std::vector<TwinInventoryMaterial> &out, TwinInventoryStats &stats, const Rules::RuleSet *rules)
                : m_out(out), m_stats(stats), m_seen(Constants::TWIN_INVENTORY_MATERIAL_SET), m_rules(rules)
            {
            }

            /** @brief The scopes of the asset path @p path by the walk's rule set. */
            [[nodiscard]] std::uintptr_t scopes(const char *path) const noexcept { return scopes_of(m_rules, path); }

            /** @brief The scopes of @p material (a CMatInfo, or 0) by its name, by the walk's rule set. */
            [[nodiscard]] std::uintptr_t material_scopes(std::uintptr_t material) const noexcept
            {
                return material == 0 ? 0
                                     : scopes(reinterpret_cast<const char *>(
                                           read_pointer_guarded(material + Constants::MATERIAL_NAME_OFFSET)));
            }

            /**
             * @brief Hands back @p object's drawn materials for the plant and frame bits of @p scopes it was not
             *        expanded for yet: the material itself, or each sub-material of a multi-material.
             */
            void material(std::uintptr_t object, std::uintptr_t scopes)
            {
                scopes &= k_listed_scopes;
                if (object == 0 || scopes == 0)
                {
                    return;
                }
                const std::uintptr_t fresh = mark(object, scopes);
                if (fresh == 0)
                {
                    return;
                }
                MaterialView view{};
                if (!read_material_guarded(object, view))
                {
                    ++m_stats.faults;
                    return;
                }
                if (!is_material(view.vtable))
                {
                    ++m_stats.not_materials;
                    return;
                }
                ++m_stats.distinct_materials;
                if ((view.flags & Constants::MTL_FLAG_MULTI_SUBMTL) == 0)
                {
                    emit(object, fresh);
                    return;
                }
                const int count = std::clamp(view.sub_count, 0, Constants::TWIN_INVENTORY_MAX_SUBMATERIALS);
                m_stats.skipped_submaterials += std::max(view.sub_count - count, 0);
                for (int i = 0; i < count; ++i)
                {
                    const std::uintptr_t sub =
                        read_pointer_guarded(view.subs + static_cast<std::uintptr_t>(i) * sizeof(std::uintptr_t));
                    if (sub == 0)
                    {
                        continue;
                    }
                    MaterialView sub_view{};
                    if (!read_material_guarded(sub, sub_view))
                    {
                        ++m_stats.faults;
                        continue;
                    }
                    if (!is_material(sub_view.vtable))
                    {
                        ++m_stats.not_materials;
                        continue;
                    }
                    emit(sub, fresh);
                }
            }

            /**
             * @brief Hands back the drawn materials of the statobj @p object and of its compound sub-objects, each
             *        scoped by its own .cgf path together with @p inherited (a group's file name, a parent's scopes).
             * @return The scopes @p object itself got (0 when it is not a plant or frame asset or did not read).
             */
            std::uintptr_t statobj(std::uintptr_t object, std::uintptr_t inherited)
            {
                if (object == 0)
                {
                    return 0;
                }
                StatObjView view{};
                if (!read_statobj_guarded(object, view))
                {
                    ++m_stats.faults;
                    return 0;
                }
                const std::uintptr_t scopes = (this->scopes(view.path) | inherited) & k_listed_scopes;
                if (scopes != 0)
                {
                    ++m_stats.scoped_assets;
                    material(view.material, scopes);
                }
                for (int i = 0; i < view.child_count; ++i)
                {
                    // A sub-object's statobj shares its parent's vtable; anything else is not one.
                    const std::uintptr_t child =
                        read_pointer_guarded(view.children + static_cast<std::uintptr_t>(i) * Constants::SUBOBJ_STRIDE +
                                             Constants::SUBOBJ_PSTATOBJ_OFFSET);
                    StatObjView child_view{};
                    if (child == 0 || child == object)
                    {
                        continue;
                    }
                    ++m_stats.children;
                    if (!read_statobj_guarded(child, child_view) || child_view.vtable != view.vtable)
                    {
                        ++m_stats.faults;
                        continue;
                    }
                    const std::uintptr_t child_scopes = (this->scopes(child_view.path) | scopes) & k_listed_scopes;
                    if (child_scopes != 0)
                    {
                        ++m_stats.scoped_assets;
                        material(child_view.material, child_scopes);
                    }
                }
                return scopes;
            }

        private:
            struct Seen
            {
                std::uintptr_t object = 0; // a material, or 0 for a free slot
                std::uintptr_t scopes = 0;
            };

            /**
             * @brief Records that @p object is expanded for @p scopes; returns the bits it was not expanded for
             *        before. Past three quarters full, the set stops deduplicating and every bit is new (the caller
             *        deduplicates the shader items it turns into twins anyway).
             */
            std::uintptr_t mark(std::uintptr_t object, std::uintptr_t scopes) noexcept
            {
                if (m_seen_count * 4 >= m_seen.size() * 3)
                {
                    m_stats.dedupe_full = true;
                    return scopes;
                }
                const std::size_t mask = m_seen.size() - 1;
                std::size_t at = static_cast<std::size_t>((object >> 4) * 0x9E3779B97F4A7C15ull) & mask;
                for (;;)
                {
                    Seen &entry = m_seen[at];
                    if (entry.object == 0)
                    {
                        entry = Seen{object, scopes};
                        ++m_seen_count;
                        return scopes;
                    }
                    if (entry.object == object)
                    {
                        const std::uintptr_t fresh = scopes & ~entry.scopes;
                        entry.scopes |= scopes;
                        return fresh;
                    }
                    at = (at + 1) & mask;
                }
            }

            /** @brief True when @p vtable is CMatInfo's: the first one proved by RTTI, then by comparison. */
            [[nodiscard]] bool is_material(std::uintptr_t vtable) noexcept
            {
                if (vtable == 0 || vtable == m_rejected_vtable)
                {
                    return false;
                }
                if (vtable == m_material_vtable)
                {
                    return true;
                }
                if (!DMK::rtti::vtable_is_type(DMK::Address{vtable}, Constants::CMATINFO_RTTI_NAME))
                {
                    m_rejected_vtable = vtable;
                    return false;
                }
                if (m_material_vtable == 0)
                {
                    m_material_vtable = vtable;
                }
                return true;
            }

            void emit(std::uintptr_t drawn, std::uintptr_t scopes)
            {
                m_out.push_back(TwinInventoryMaterial{.material = drawn, .scopes = scopes});
                ++m_stats.drawn_materials;
            }

            std::vector<TwinInventoryMaterial> &m_out;
            TwinInventoryStats &m_stats;
            std::vector<Seen> m_seen;
            std::size_t m_seen_count = 0;
            std::uintptr_t m_material_vtable = 0;
            std::uintptr_t m_rejected_vtable = 0;
            const Rules::RuleSet *m_rules = nullptr;
        };

        /** @brief The first problem a walk met, kept for the log (a later one does not replace it). */
        void note(TwinInventoryStats &stats, const char *reason) noexcept
        {
            if (stats.reason[0] == '\0')
            {
                stats.reason = reason;
            }
        }

        /** @brief Walks the vegetation groups of the object manager the global @p objman_global holds. */
        void walk_groups(Walk &walk, TwinInventoryStats &stats, std::uintptr_t objman_global)
        {
            const std::uintptr_t objman = read_pointer_guarded(objman_global);
            if (objman == 0)
            {
                return;
            }
            if (!object_has_type(objman, Constants::COBJMANAGER_RTTI_NAME))
            {
                ++stats.faults;
                note(stats, "the vegetation groups' owner is not a CObjManager");
                return;
            }
            TableSpan groups{};
            const TableRead read =
                read_table_guarded(objman + Constants::OBJMAN_GROUPS_BEGIN_OFFSET, Constants::STAT_INST_GROUP_STRIDE,
                                   Constants::TWIN_INVENTORY_MAX_GROUPS, groups);
            if (read == TableRead::Empty)
            {
                return;
            }
            if (read != TableRead::Read)
            {
                ++stats.faults;
                note(stats, "the vegetation group table did not check out");
                return;
            }
            for (std::size_t i = 0; i < groups.count; ++i)
            {
                const std::uintptr_t group = groups.begin + i * Constants::STAT_INST_GROUP_STRIDE;
                ++stats.groups;
                // The group's file name is its statobj's; it scopes a group whose statobj carries no path.
                const std::uintptr_t named =
                    walk.scopes(reinterpret_cast<const char *>(group + Constants::STAT_INST_GROUP_FILENAME_OFFSET));
                const std::uintptr_t scopes =
                    walk.statobj(read_pointer_guarded(group + Constants::STAT_INST_GROUP_STATOBJ_OFFSET), named) |
                    (named & k_listed_scopes);
                if (scopes != 0)
                {
                    walk.material(read_pointer_guarded(group + Constants::STAT_INST_GROUP_MATERIAL_OFFSET), scopes);
                }
            }
        }

    } // namespace

    std::uintptr_t twin_asset_scopes(const char *path) noexcept
    {
        const std::shared_ptr<const Rules::RuleSet> rules = Rules::current();
        return scopes_of(rules.get(), path);
    }

    std::uintptr_t twin_material_scopes(std::uintptr_t material) noexcept
    {
        if (material == 0)
        {
            return 0;
        }
        return twin_asset_scopes(
            reinterpret_cast<const char *>(read_pointer_guarded(material + Constants::MATERIAL_NAME_OFFSET)));
    }

    TwinInventoryStats enumerate_level_twin_materials(std::uintptr_t genv, std::uintptr_t objman_global,
                                                      std::vector<TwinInventoryMaterial> &out)
    {
        const auto started = std::chrono::steady_clock::now();
        TwinInventoryStats stats{};
        const auto finish = [&stats, started]() -> TwinInventoryStats
        {
            stats.micros = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started)
                    .count());
            return stats;
        };
        if (genv == 0 || objman_global == 0)
        {
            stats.reason = "the inventory anchors did not resolve";
            return finish();
        }
        const std::uintptr_t engine = read_pointer_guarded(genv + Constants::GENV_3DENGINE_OFFSET);
        if (engine == 0)
        {
            stats.reason = "the 3D engine is not up";
            return finish();
        }
        if (!object_has_type(engine, Constants::C3DENGINE_RTTI_NAME))
        {
            ++stats.faults;
            stats.reason = "g_env's 3D engine is not a C3DEngine";
            return finish();
        }
        // Each table is a std::vector the level load allocates and the level unload frees: null without a level.
        const std::uintptr_t statobj_table = read_pointer_guarded(engine + Constants::C3DENGINE_LEVEL_STATOBJS_OFFSET);
        const std::uintptr_t material_table =
            read_pointer_guarded(engine + Constants::C3DENGINE_LEVEL_MATERIALS_OFFSET);
        if (statobj_table == 0 || material_table == 0)
        {
            stats.reason = "no level is loaded";
            return finish();
        }
        stats.level = true;
        const std::shared_ptr<const Rules::RuleSet> rules = Rules::current();
        Walk walk(out, stats, rules.get());
        walk_groups(walk, stats, objman_global);

        TableSpan statobjs{};
        const TableRead statobjs_read =
            read_table_guarded(statobj_table, sizeof(std::uintptr_t), Constants::TWIN_INVENTORY_MAX_STATOBJS, statobjs);
        if (statobjs_read == TableRead::Read)
        {
            for (std::size_t i = 0; i < statobjs.count; ++i)
            {
                ++stats.statobjs;
                (void)walk.statobj(read_pointer_guarded(statobjs.begin + i * sizeof(std::uintptr_t)), 0);
            }
        }
        else if (statobjs_read != TableRead::Empty)
        {
            ++stats.faults;
            note(stats, "the level statobj table did not check out");
        }

        TableSpan materials{};
        const TableRead materials_read = read_table_guarded(material_table, sizeof(std::uintptr_t),
                                                            Constants::TWIN_INVENTORY_MAX_MATERIALS, materials);
        if (materials_read == TableRead::Read)
        {
            for (std::size_t i = 0; i < materials.count; ++i)
            {
                ++stats.brush_materials;
                const std::uintptr_t material = read_pointer_guarded(materials.begin + i * sizeof(std::uintptr_t));
                const std::uintptr_t scopes = walk.material_scopes(material) & k_listed_scopes;
                if (scopes != 0)
                {
                    ++stats.scoped_assets;
                    walk.material(material, scopes);
                }
            }
        }
        else if (materials_read != TableRead::Empty)
        {
            ++stats.faults;
            note(stats, "the level brush material table did not check out");
        }
        return finish();
    }

} // namespace TPVCamera
