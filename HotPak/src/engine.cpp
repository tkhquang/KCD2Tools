/**
 * @file engine.cpp
 * @brief CryPak, renderer, script system, skin cache, CMatMan and table database access through vtable-checked
 *        objects and guarded reads.
 */

#include "engine.hpp"
#include "constants.hpp"
#include "game_memory.hpp"
#include "table_links.hpp"
#include "text_util.hpp"

#include <DetourModKit.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace HotPak
{
    namespace
    {
        std::uintptr_t s_genv = 0;
        DMK::Region s_game{};
        EngineAnchors s_anchors{};

        // The skin cache's std::mutex belongs to the C++ runtime DLL the game links. Its _Mtx_t is that DLL's opaque
        // type, so only that DLL's own lock and unlock exports may touch it.
        using MutexFn = int(__cdecl *)(void *mutex);

        using OpenPackFn = bool(__fastcall *)(
            void *self,
            const char *bind_root,
            const char *path,
            std::uint32_t flags,
            void *memory_block,
            void *full_path_out
        );
        using ExecuteFileFn =
            bool(__fastcall *)(void *self, const char *name, bool raise_error, bool force_reload, void *env);
        using ExecuteBufferFn =
            bool(__fastcall *)(void *self, const char *buffer, std::size_t size, const char *description, void *env);
        using RendererFileFn = bool(__fastcall *)(void *self, const char *path);
        using FindMaterialFn = void *(__fastcall *)(void *self, const char *name);
        using RenameMaterialFn = void(__fastcall *)(void *self, void *material, const char *new_name);
        using DatabaseReloadFn = std::int32_t(__fastcall *)(void *self);
        using DatabaseLinkFn = bool(__fastcall *)(void *self);

        std::uintptr_t s_database_manager = 0;

        // Bounds on game-owned containers, so a torn or foreign structure cannot drive a long walk.
        constexpr std::size_t MAX_MOUNTED_PAKS = 8192;
        constexpr std::size_t MAX_DATABASES = 4096;
        constexpr std::size_t MAX_STD_STRING = 4096;
        // The manager is one static object. More candidate copies of its vtable pointer than this mean a wrong match.
        constexpr std::size_t MAX_MANAGER_CANDIDATES = 16;

        // The loader's persistent slots: slot 0 holds the layout tag, slot 1 the last retired-material number.
        constexpr std::uint64_t PERSISTENT_LAYOUT_TAG = UINT64_C(0x48504D4154534551); // ASCII "HPMATSEQ"
        constexpr std::size_t PERSISTENT_TAG_SLOT = 0;
        constexpr std::size_t PERSISTENT_MATERIAL_SEQUENCE_SLOT = 1;
        // The retired-material number when no persistent area is bound (the production ASI has one image per process).
        std::uint64_t s_local_material_sequence = 0;
        std::uint64_t *s_material_sequence = &s_local_material_sequence;

        /** @brief Reads a NUL-terminated string of at most 1023 characters, or empty when it faults or does not fit. */
        [[nodiscard]] std::string read_string(std::uintptr_t address)
        {
            std::array<char, 1024> buffer{};
            const std::size_t length = read_c_string(address, buffer);
            return std::string(buffer.data(), length);
        }

        /**
         * @brief Reads an MSVC std::string: {16-byte buffer or heap pointer, size @0x10, capacity @0x18}.
         * @details A capacity above 15 means the characters live on the heap behind the first qword.
         */
        [[nodiscard]] std::string read_std_string(std::uintptr_t address)
        {
            const std::size_t size = read_value<std::size_t>(address + 0x10).value_or(0);
            const std::size_t capacity = read_value<std::size_t>(address + 0x18).value_or(0);
            if (size == 0 || size > MAX_STD_STRING || capacity < size)
            {
                return {};
            }
            const std::uintptr_t data = capacity > 15 ? read_ptr(address) : address;
            std::string out(size, '\0');
            const std::span<std::byte> bytes(reinterpret_cast<std::byte *>(out.data()), out.size());
            if (!DMK::memory::read_into(DMK::Address{data}, bytes).has_value())
            {
                return {};
            }
            return out;
        }

        [[nodiscard]] bool in_game_image(std::uintptr_t address) noexcept
        {
            return address != 0 && s_game.contains(DMK::Address{address});
        }

        /** @brief Reads gEnv + @p offset and returns it only when the object carries @p vtable. */
        [[nodiscard]] std::uintptr_t typed_interface(std::ptrdiff_t offset, std::uintptr_t vtable) noexcept
        {
            if (s_genv == 0 || vtable == 0)
            {
                return 0;
            }
            const std::uintptr_t object = read_ptr(s_genv + static_cast<std::uintptr_t>(offset));
            if (object == 0 || read_ptr(object) != vtable)
            {
                return 0;
            }
            return object;
        }

        [[nodiscard]] const char *availability(std::ptrdiff_t offset, std::uintptr_t vtable) noexcept
        {
            if (vtable == 0)
            {
                return "off";
            }
            return typed_interface(offset, vtable) != 0 ? "live" : "not yet created";
        }

        /** @brief Holds the skin cache's mutex for one scope. */
        class SkinCacheLock
        {
        public:
            explicit SkinCacheLock(void *mutex) noexcept
                : m_mutex(mutex), m_unlock(reinterpret_cast<MutexFn>(s_anchors.mutex_unlock))
            {
                const auto lock = reinterpret_cast<MutexFn>(s_anchors.mutex_lock);
                m_locked = m_mutex != nullptr && lock != nullptr && m_unlock != nullptr && lock(m_mutex) == 0;
            }

            ~SkinCacheLock() noexcept
            {
                if (m_locked)
                {
                    (void)m_unlock(m_mutex);
                }
            }

            SkinCacheLock(const SkinCacheLock &) = delete;
            SkinCacheLock &operator=(const SkinCacheLock &) = delete;
            SkinCacheLock(SkinCacheLock &&) = delete;
            SkinCacheLock &operator=(SkinCacheLock &&) = delete;

            [[nodiscard]] bool locked() const noexcept { return m_locked; }

        private:
            void *m_mutex = nullptr;
            MutexFn m_unlock = nullptr;
            bool m_locked = false;
        };

        [[nodiscard]] bool path_listed(std::string_view key, std::span<const std::string> paths) noexcept
        {
            for (const std::string &path : paths)
            {
                if (key == path)
                {
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief Walks the skin cache list under its mutex and collects (optionally marks) the listed keys.
         * @details The multimap's element list is a circular doubly linked list whose sentinel is the head pointer at
         *          CharacterManager + 0x140; its size lives at + 0x148. The walk is bounded by that size, so a torn
         *          list cannot loop.
         */
        [[nodiscard]] std::vector<SkinCacheEntry> walk_skin_cache(std::span<const std::string> paths, bool mark)
        {
            std::vector<SkinCacheEntry> found;
            const std::uintptr_t manager =
                typed_interface(constants::GENV_CHARACTER_MANAGER_OFFSET, s_anchors.character_manager);
            if (manager == 0 || s_anchors.skin == 0)
            {
                DMK::log().warning("SkinCache: CharacterManager unavailable");
                return found;
            }

            const SkinCacheLock lock(reinterpret_cast<void *>(manager + constants::CM_SKIN_CACHE_MUTEX_OFFSET));
            if (!lock.locked())
            {
                DMK::log().warning("SkinCache: the cache mutex could not be taken");
                return found;
            }

            const std::uintptr_t head = read_ptr(manager + constants::CM_SKIN_CACHE_HEAD_OFFSET);
            const std::size_t size =
                read_value<std::size_t>(manager + constants::CM_SKIN_CACHE_SIZE_OFFSET).value_or(0);
            if (head == 0 || size > constants::SKIN_CACHE_WALK_LIMIT)
            {
                DMK::log().warning("SkinCache: implausible cache header (head {:#x}, size {})", head, size);
                return found;
            }

            std::uintptr_t node = read_ptr(head);
            for (std::size_t visited = 0; node != 0 && node != head && visited <= size; ++visited)
            {
                const std::string key = normalize_path(read_std_string(node + constants::SKIN_NODE_KEY_OFFSET));
                const std::uintptr_t skin = read_ptr(node + constants::SKIN_NODE_VALUE_OFFSET);
                if (!key.empty() && path_listed(key, paths) && skin != 0 && read_ptr(skin) == s_anchors.skin)
                {
                    const DMK::Address flags_address{skin + constants::SKIN_FLAGS_OFFSET};
                    std::uint32_t flags = read_value<std::uint32_t>(flags_address.raw()).value_or(0);
                    const std::uint32_t unshared = flags | constants::SKIN_FLAG_UNSHARED;
                    // The cache reads the flags only under this mutex, so the store is ordered with every lookup that
                    // follows the unlock.
                    if (mark && unshared != flags && DMK::memory::write_in_place(flags_address, unshared).has_value())
                    {
                        flags = unshared;
                    }
                    found.push_back(
                        SkinCacheEntry{
                            .key = key,
                            .skin = skin,
                            .flags = flags,
                            .refs = read_value<std::int32_t>(skin + constants::SKIN_REFS_OFFSET).value_or(-1),
                        }
                    );
                }
                node = read_ptr(node);
            }
            return found;
        }

        /**
         * @brief Returns CMatMan by decoding IMaterial::GetMaterialManager, `mov rax, [rip+disp32]; ret`.
         * @details The global it reads is the engine-wide material manager pointer; the result must carry the CMatMan
         *          vtable, so a getter of another shape or a moved global fails closed.
         */
        [[nodiscard]] std::uintptr_t material_manager() noexcept
        {
            const std::uintptr_t getter = vtable_slot(s_anchors.material_info, constants::MATERIAL_GET_MANAGER_SLOT);
            if (getter == 0)
            {
                return 0;
            }
            std::array<std::byte, 8> code{};
            if (!DMK::memory::read_into(DMK::Address{getter}, code).has_value() || code[0] != std::byte{0x48} ||
                code[1] != std::byte{0x8B} || code[2] != std::byte{0x05} || code[7] != std::byte{0xC3})
            {
                return 0;
            }
            std::int32_t displacement = 0;
            std::memcpy(&displacement, code.data() + 3, sizeof(displacement));
            const std::uintptr_t global = getter + 7 + static_cast<std::intptr_t>(displacement);
            const std::uintptr_t manager = read_ptr(global);
            return manager != 0 && read_ptr(manager) == s_anchors.material_manager ? manager : 0;
        }

        /** @brief True when @p candidate holds a plausible database list whose first entry has an in-image vtable. */
        [[nodiscard]] bool plausible_database_manager(std::uintptr_t candidate) noexcept
        {
            if (candidate % alignof(std::uintptr_t) != 0)
            {
                return false;
            }
            const std::uintptr_t begin = read_ptr(candidate + constants::DB_MANAGER_LIST_BEGIN_OFFSET);
            const std::uintptr_t end = read_ptr(candidate + constants::DB_MANAGER_LIST_END_OFFSET);
            return begin != 0 && end > begin && (end - begin) % sizeof(std::uintptr_t) == 0 &&
                   (end - begin) / sizeof(std::uintptr_t) <= MAX_DATABASES && in_game_image(read_ptr(read_ptr(begin)));
        }

        /**
         * @brief Finds the static C_ObjectDatabaseManager by its vtable pointer inside the WHGame.dll image.
         * @details The manager is a function-local static of the game module, so its first qword (the vtable) sits in
         *          the image's data once the game constructed it. Every copy of that pointer in the image is checked,
         *          and exactly one must hold a plausible database list. An incomplete scan finds nothing.
         */
        [[nodiscard]] std::uintptr_t find_database_manager()
        {
            if (s_database_manager != 0 || s_anchors.database_manager == 0)
            {
                return s_database_manager;
            }
            std::string dsl;
            for (std::size_t i = 0; i < sizeof(std::uintptr_t); ++i)
            {
                dsl += std::format("{:02X} ", (s_anchors.database_manager >> (8 * i)) & 0xFF);
            }
            const auto pattern = DMK::scan::Pattern::compile(dsl);
            if (!pattern.has_value())
            {
                return 0;
            }
            std::uintptr_t manager = 0;
            std::size_t plausible = 0;
            for (std::size_t occurrence = 1;; ++occurrence)
            {
                const auto hit = DMK::scan::scan(*pattern, s_game, occurrence, DMK::scan::Pages::Readable);
                if (!hit.has_value())
                {
                    if (hit.error().code != DMK::ErrorCode::NoMatch)
                    {
                        DMK::log().warning("Engine: database manager scan incomplete ({})", hit.error().message());
                        return 0;
                    }
                    break;
                }
                if (occurrence > MAX_MANAGER_CANDIDATES)
                {
                    DMK::log().warning("Engine: database manager vtable referenced too often; table reload disabled");
                    return 0;
                }
                if (plausible_database_manager(hit->raw()))
                {
                    manager = hit->raw();
                    ++plausible;
                }
            }
            if (plausible != 1)
            {
                DMK::log().warning(
                    "Engine: {} database manager candidate(s); table reload needs exactly one",
                    plausible
                );
                return 0;
            }
            s_database_manager = manager;
            DMK::log().info("Engine: database manager at {}", DMK::format::format_address(manager));
            return manager;
        }

        /** @brief The direct dependents of a table database, read from its dependents vector. */
        [[nodiscard]] std::vector<std::uintptr_t> database_dependents(std::uintptr_t database)
        {
            std::vector<std::uintptr_t> out;
            const std::uintptr_t begin = read_ptr(database + constants::DB_DEPENDENTS_BEGIN_OFFSET);
            const std::uintptr_t end = read_ptr(database + constants::DB_DEPENDENTS_END_OFFSET);
            if (begin == 0 || end <= begin || (end - begin) / sizeof(std::uintptr_t) > MAX_DATABASES)
            {
                return out;
            }
            for (std::uintptr_t slot = begin; slot < end; slot += sizeof(std::uintptr_t))
            {
                const std::uintptr_t dependent = read_ptr(slot);
                if (dependent != 0 && in_game_image(read_ptr(dependent)))
                {
                    out.push_back(dependent);
                }
            }
            return out;
        }

        /**
         * @brief Points the retired-material sequence at the loader's persistent slots, or at a local counter.
         * @details A slot area that carries another layout tag is claimed and restarted at zero.
         */
        void bind_material_sequence(std::span<std::uint64_t> slots) noexcept
        {
            if (slots.size() <= PERSISTENT_MATERIAL_SEQUENCE_SLOT)
            {
                s_material_sequence = &s_local_material_sequence;
                return;
            }
            if (slots[PERSISTENT_TAG_SLOT] != PERSISTENT_LAYOUT_TAG)
            {
                slots[PERSISTENT_TAG_SLOT] = PERSISTENT_LAYOUT_TAG;
                slots[PERSISTENT_MATERIAL_SEQUENCE_SLOT] = 0;
            }
            s_material_sequence = &slots[PERSISTENT_MATERIAL_SEQUENCE_SLOT];
        }
    } // namespace

    void initialize_engine(
        std::uintptr_t genv,
        DMK::Region game,
        const EngineAnchors &anchors,
        std::span<std::uint64_t> persistent_slots
    )
    {
        s_genv = genv;
        s_game = game;
        s_anchors = anchors;
        s_database_manager = 0;
        bind_material_sequence(persistent_slots);

        DMK::log().info(
            "Engine: gEnv {} - script system {}, CryPak {}, renderer {}, character manager {}; retired materials "
            "continue after #{}",
            DMK::format::format_address(genv),
            availability(constants::GENV_SCRIPT_SYSTEM_OFFSET, anchors.script_system),
            availability(constants::GENV_CRYPAK_OFFSET, anchors.cry_pak),
            availability(constants::GENV_RENDERER_OFFSET, anchors.renderer),
            availability(constants::GENV_CHARACTER_MANAGER_OFFSET, anchors.character_manager),
            *s_material_sequence
        );
    }

    void shutdown_engine() noexcept
    {
        s_genv = 0;
        s_anchors = EngineAnchors{};
        s_database_manager = 0;
        s_material_sequence = &s_local_material_sequence;
    }

    std::uintptr_t vtable_slot(std::uintptr_t vtable, std::size_t slot) noexcept
    {
        if (vtable == 0)
        {
            return 0;
        }
        const std::uintptr_t target = read_ptr(vtable + slot * sizeof(std::uintptr_t));
        return in_game_image(target) ? target : 0;
    }

    std::vector<MountedPak> mounted_paks()
    {
        std::vector<MountedPak> paks;
        const std::uintptr_t pak = typed_interface(constants::GENV_CRYPAK_OFFSET, s_anchors.cry_pak);
        if (pak == 0)
        {
            return paks;
        }
        const std::uintptr_t begin = read_ptr(pak + constants::CRYPAK_ZIPS_BEGIN_OFFSET);
        const std::uintptr_t end = read_ptr(pak + constants::CRYPAK_ZIPS_END_OFFSET);
        if (begin == 0 || end < begin || (end - begin) % constants::PACK_DESC_STRIDE != 0 ||
            (end - begin) / constants::PACK_DESC_STRIDE > MAX_MOUNTED_PAKS)
        {
            DMK::log().warning("CryPak: implausible pak list [{:#x}, {:#x})", begin, end);
            return paks;
        }
        for (std::uintptr_t desc = begin; desc < end; desc += constants::PACK_DESC_STRIDE)
        {
            const std::uintptr_t zip = read_ptr(desc + constants::PACK_DESC_ZIP_OFFSET);
            const std::uintptr_t path_offset = read_ptr(zip + constants::ZIP_PATH_INDIRECT_OFFSET);
            paks.push_back(
                MountedPak{
                    .path = read_string(zip + path_offset + constants::ZIP_PATH_BASE_OFFSET),
                    .bind_root = read_string(read_ptr(desc + constants::PACK_DESC_BIND_ROOT_OFFSET)),
                }
            );
        }
        return paks;
    }

    bool open_overlay_pak(const std::string &bind_root, const std::string &path)
    {
        const std::uintptr_t pak = typed_interface(constants::GENV_CRYPAK_OFFSET, s_anchors.cry_pak);
        const std::uintptr_t target = vtable_slot(s_anchors.cry_pak, constants::CRYPAK_OPEN_PACK_BOUND_SLOT);
        if (pak == 0 || target == 0)
        {
            DMK::log().error("CryPak: OpenPack unavailable");
            return false;
        }
        return reinterpret_cast<OpenPackFn>(target)(
            reinterpret_cast<void *>(pak),
            bind_root.c_str(),
            path.c_str(),
            constants::OVERLAY_PAK_FLAGS,
            nullptr,
            nullptr
        );
    }

    bool request_texture_reload(const std::string &path)
    {
        const std::uintptr_t renderer = typed_interface(constants::GENV_RENDERER_OFFSET, s_anchors.renderer);
        const std::uintptr_t target = vtable_slot(s_anchors.renderer, constants::RENDERER_RELOAD_FILE_REQUEST_SLOT);
        if (renderer == 0 || target == 0)
        {
            return false;
        }
        return reinterpret_cast<RendererFileFn>(target)(reinterpret_cast<void *>(renderer), path.c_str());
    }

    bool reload_file_now(const std::string &path)
    {
        const std::uintptr_t renderer = typed_interface(constants::GENV_RENDERER_OFFSET, s_anchors.renderer);
        const std::uintptr_t target = vtable_slot(s_anchors.renderer, constants::RENDERER_RELOAD_FILE_SLOT);
        if (renderer == 0 || target == 0)
        {
            return false;
        }
        return reinterpret_cast<RendererFileFn>(target)(reinterpret_cast<void *>(renderer), path.c_str());
    }

    std::vector<SkinCacheEntry> mark_skins_unshared(std::span<const std::string> paths)
    {
        return walk_skin_cache(paths, true);
    }

    std::vector<SkinCacheEntry> find_cached_skins(std::span<const std::string> paths)
    {
        return walk_skin_cache(paths, false);
    }

    std::uintptr_t retire_material(const std::string &name)
    {
        const std::uintptr_t manager = material_manager();
        const std::uintptr_t find = vtable_slot(s_anchors.material_manager, constants::MATMAN_FIND_SLOT);
        const std::uintptr_t rename = vtable_slot(s_anchors.material_manager, constants::MATMAN_RENAME_SLOT);
        if (manager == 0 || find == 0 || rename == 0)
        {
            DMK::log().warning("Material: CMatMan unavailable");
            return 0;
        }
        const auto material = reinterpret_cast<std::uintptr_t>(
            reinterpret_cast<FindMaterialFn>(find)(reinterpret_cast<void *>(manager), name.c_str())
        );
        if (material == 0 || read_ptr(material) != s_anchors.material_info)
        {
            return 0;
        }
        // RenameMaterial overwrites whatever the map holds under the new key, so every retirement in the process,
        // across logic generations, takes a number of its own.
        const std::string retired = name + constants::RETIRED_MATERIAL_MARKER + std::to_string(++*s_material_sequence);
        reinterpret_cast<RenameMaterialFn>(rename)(
            reinterpret_cast<void *>(manager),
            reinterpret_cast<void *>(material),
            retired.c_str()
        );
        return material;
    }

    std::uintptr_t find_material(const std::string &name)
    {
        const std::uintptr_t manager = material_manager();
        const std::uintptr_t find = vtable_slot(s_anchors.material_manager, constants::MATMAN_FIND_SLOT);
        if (manager == 0 || find == 0)
        {
            return 0;
        }
        const auto material = reinterpret_cast<std::uintptr_t>(
            reinterpret_cast<FindMaterialFn>(find)(reinterpret_cast<void *>(manager), name.c_str())
        );
        return material != 0 && read_ptr(material) == s_anchors.material_info ? material : 0;
    }

    bool rerun_script_file(const std::string &path)
    {
        const std::uintptr_t scripts = typed_interface(constants::GENV_SCRIPT_SYSTEM_OFFSET, s_anchors.script_system);
        const std::uintptr_t target = vtable_slot(s_anchors.script_system, constants::SCRIPT_EXECUTE_FILE_SLOT);
        if (scripts == 0 || target == 0)
        {
            DMK::log().error("Script: IScriptSystem::ExecuteFile unavailable");
            return false;
        }
        // raiseError routes a load or runtime error through the script system's error handler, which logs the message
        // and the Lua call stack to the game log.
        return reinterpret_cast<ExecuteFileFn>(target)(
            reinterpret_cast<void *>(scripts),
            path.c_str(),
            true,
            true,
            nullptr
        );
    }

    bool run_script_buffer(std::string_view code, const std::string &chunk_name)
    {
        const std::uintptr_t scripts = typed_interface(constants::GENV_SCRIPT_SYSTEM_OFFSET, s_anchors.script_system);
        const std::uintptr_t target = vtable_slot(s_anchors.script_system, constants::SCRIPT_EXECUTE_BUFFER_SLOT);
        if (scripts == 0 || target == 0)
        {
            DMK::log().error("Script: IScriptSystem::ExecuteBuffer unavailable");
            return false;
        }
        return reinterpret_cast<ExecuteBufferFn>(target)(
            reinterpret_cast<void *>(scripts),
            code.data(),
            code.size(),
            chunk_name.c_str(),
            nullptr
        );
    }

    std::vector<DatabaseInfo> table_databases()
    {
        std::vector<DatabaseInfo> out;
        const std::uintptr_t manager = find_database_manager();
        if (manager == 0)
        {
            return out;
        }
        const std::uintptr_t begin = read_ptr(manager + constants::DB_MANAGER_LIST_BEGIN_OFFSET);
        const std::uintptr_t end = read_ptr(manager + constants::DB_MANAGER_LIST_END_OFFSET);
        if (begin == 0 || end <= begin || (end - begin) / sizeof(std::uintptr_t) > MAX_DATABASES)
        {
            return out;
        }
        for (std::uintptr_t slot = begin; slot < end; slot += sizeof(std::uintptr_t))
        {
            const std::uintptr_t database = read_ptr(slot);
            if (database == 0 || !in_game_image(read_ptr(database)))
            {
                continue;
            }
            out.push_back(
                DatabaseInfo{
                    .object = database,
                    .name = read_string(read_ptr(database + constants::DB_NAME_OFFSET)),
                    .subfolder = read_string(read_ptr(database + constants::DB_SUBFOLDER_OFFSET)),
                }
            );
        }
        return out;
    }

    std::optional<TableReloadResult> reload_table_database(std::uintptr_t database)
    {
        const std::uintptr_t manager = find_database_manager();
        const std::uintptr_t vtable = read_ptr(database);
        const std::uintptr_t reload = vtable_slot(vtable, constants::DB_RELOAD_SLOT);
        const std::uintptr_t link = vtable_slot(vtable, constants::DB_LINK_SLOT);
        if (manager == 0 || reload == 0 || link == 0)
        {
            return std::nullopt;
        }
        // The dependents vectors are only current while this byte is clear. The manager's own reload rebuilds the
        // graph first. Without that rebuild a dependent could be missed and keep pointing at freed rows.
        const std::optional<std::uint8_t> graph_dirty =
            read_value<std::uint8_t>(manager + constants::DB_MANAGER_GRAPH_DIRTY_OFFSET);
        if (graph_dirty.value_or(1) != 0)
        {
            DMK::log().warning("Table: the database dependency graph awaits a rebuild; restart the game instead");
            return std::nullopt;
        }

        const DMK::Address reloading{manager + constants::DB_MANAGER_RELOADING_OFFSET};
        if (!DMK::memory::write_in_place<std::uint8_t>(reloading, 1).has_value())
        {
            return std::nullopt;
        }
        const std::int32_t reload_status =
            reinterpret_cast<DatabaseReloadFn>(reload)(reinterpret_cast<void *>(database));
        (void)DMK::memory::write_in_place<std::uint8_t>(reloading, 0);

        TableReloadResult result;
        result.ok = reload_status == 0 && reinterpret_cast<DatabaseLinkFn>(link)(reinterpret_cast<void *>(database));

        // The reload freed the old rows, and every database that resolved references into them still points there,
        // whether or not the reload succeeded.
        const std::vector<std::uintptr_t> dependents = relink_order(
            database,
            [](std::uintptr_t object) { return database_dependents(object); },
            [](std::uintptr_t object)
            { return read_value<std::uint32_t>(object + constants::DB_DEPTH_OFFSET).value_or(0); },
            MAX_DATABASES
        );
        for (const std::uintptr_t dependent : dependents)
        {
            const std::uintptr_t dependent_link = vtable_slot(read_ptr(dependent), constants::DB_LINK_SLOT);
            if (dependent_link != 0 &&
                reinterpret_cast<DatabaseLinkFn>(dependent_link)(reinterpret_cast<void *>(dependent)))
            {
                ++result.relinked;
            }
            else
            {
                ++result.relink_failed;
            }
        }
        return result;
    }

} // namespace HotPak
