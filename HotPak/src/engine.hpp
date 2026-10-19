/**
 * @file engine.hpp
 * @brief The engine systems the tool drives: CryPak, the script system, the renderer's reload entry points, the skin
 *        cache, CMatMan and the table databases.
 *
 * Every interface pointer is read fresh from gEnv and must carry the vtable its RTTI name resolved to, and every
 * virtual call reads its slot from that vtable and requires a target inside WHGame.dll. A mismatch on another build
 * fails the call instead of jumping through a shifted table.
 */
#ifndef HOTPAK_ENGINE_HPP
#define HOTPAK_ENGINE_HPP

#include <DetourModKit.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace HotPak
{
    /**
     * @brief The primary vtables resolved from RTTI type names inside WHGame.dll, and the C++ runtime's mutex
     *        functions. A member a feature gate turned off stays 0, and every call that needs it fails closed.
     */
    struct EngineAnchors
    {
        std::uintptr_t script_system = 0;
        std::uintptr_t cry_pak = 0;
        std::uintptr_t renderer = 0;
        std::uintptr_t character_manager = 0;
        std::uintptr_t skin = 0;
        std::uintptr_t attachment_skin = 0;
        std::uintptr_t clothing_proxy = 0;
        std::uintptr_t skin_element = 0;
        std::uintptr_t material_info = 0;
        std::uintptr_t material_manager = 0;
        std::uintptr_t material_instance = 0;
        std::uintptr_t database_manager = 0;
        /// msvcp140 _Mtx_lock.
        std::uintptr_t mutex_lock = 0;
        /// msvcp140 _Mtx_unlock.
        std::uintptr_t mutex_unlock = 0;
    };

    /** @brief One registered table database. */
    struct DatabaseInfo
    {
        std::uintptr_t object = 0;
        /// Table name, for example "ClothingMaterial".
        std::string name;
        /// Folder under Libs/Tables, for example "Character" (empty for a top-level table).
        std::string subfolder;
    };

    /** @brief One pak in CryPak's search list. */
    struct MountedPak
    {
        /// The path CryPak opened, as it stores it (game-root relative or absolute, backslashes).
        std::string path;
        /// The bind root its entries resolve under (for example "data\").
        std::string bind_root;
    };

    /** @brief One skin cache entry. */
    struct SkinCacheEntry
    {
        std::string key;
        std::uintptr_t skin = 0;
        std::uint32_t flags = 0;
        std::int32_t refs = 0;
    };

    /**
     * @brief Binds the engine layer to gEnv, the game image and the resolved anchors.
     * @param genv The resolved gEnv base.
     * @param game The WHGame.dll image.
     * @param anchors The gated vtables and runtime functions.
     * @param persistent_slots The dev loader's process-lifetime slots, or an empty span in the production ASI. The
     *        retired-material sequence lives there, so a reloaded logic generation never reuses a name an earlier one
     *        filed in CMatMan.
     * @note Setup/control-plane only. The interfaces themselves are read on each use: an ASI loaded at game start
     *       runs this before the engine fills gEnv.
     */
    void initialize_engine(
        std::uintptr_t genv,
        DMK::Region game,
        const EngineAnchors &anchors,
        std::span<std::uint64_t> persistent_slots
    );

    /** @brief Forgets the resolved pointers. */
    void shutdown_engine() noexcept;

    /** @brief Reads slot @p slot of @p vtable, or 0 unless it points into WHGame.dll. */
    [[nodiscard]] std::uintptr_t vtable_slot(std::uintptr_t vtable, std::size_t slot) noexcept;

    /**
     * @brief Snapshots CryPak's pak list in search order (the last entry wins a lookup).
     * @note Main thread. The list is read through guarded loads without CryPak's own lock, which only a mount or an
     *       unmount on another thread contends for.
     */
    [[nodiscard]] std::vector<MountedPak> mounted_paks();

    /**
     * @brief Mounts @p path the way the mod manager mounts a mod's Data paks, after every installed pak.
     * @param bind_root The root its entries resolve under.
     * @param path The pak file, absolute.
     * @return True when CryPak reports the pak open.
     * @note Main thread.
     */
    [[nodiscard]] bool open_overlay_pak(const std::string &bind_root, const std::string &path);

    /**
     * @brief Queues a texture reload through IRenderer::EF_ReloadFile_Request.
     * @param path Game-relative texture path; a .tif name is mapped to its .dds by the renderer.
     * @return True when the request was accepted.
     */
    [[nodiscard]] bool request_texture_reload(const std::string &path);

    /**
     * @brief Reloads a file synchronously through IRenderer::EF_ReloadFile (a .cgf refreshes its IStatObj).
     * @return The renderer's result.
     * @note Main thread.
     */
    [[nodiscard]] bool reload_file_now(const std::string &path);

    /**
     * @brief Marks every cached skin whose key is in @p paths unshareable, so the next request loads the file again.
     * @details Runs under the cache's own std::mutex, taken through the C++ runtime export WHGame.dll itself imports,
     *          so no lookup or insert on a loader thread can interleave with the walk. A skin that is already unshared
     *          is reported but left as is.
     * @param paths Normalized skin paths.
     * @return The matching entries, after marking.
     * @note Main thread.
     */
    [[nodiscard]] std::vector<SkinCacheEntry> mark_skins_unshared(std::span<const std::string> paths);

    /**
     * @brief Lists the cached skins whose key is in @p paths, under the cache lock, without changing them.
     * @note Main thread.
     */
    [[nodiscard]] std::vector<SkinCacheEntry> find_cached_skins(std::span<const std::string> paths);

    /**
     * @brief Takes a loaded material out of CMatMan's name map, so the next load of @p name reads the file again.
     * @details IMaterialManager::RenameMaterial erases the old key and files the same object under a new name that is
     *          unique for the process, under CMatMan's own lock. Every current user keeps the old object until it lets
     *          go of it.
     * @param name The material key: lowercase path without ".mtl".
     * @return The retired material, or 0 when @p name is not loaded or the manager is unavailable.
     * @note Main thread.
     */
    [[nodiscard]] std::uintptr_t retire_material(const std::string &name);

    /**
     * @brief Looks a material up in CMatMan's name map without loading it.
     * @return The material, or 0.
     * @note Main thread.
     */
    [[nodiscard]] std::uintptr_t find_material(const std::string &name);

    /**
     * @brief Runs a script file again through IScriptSystem::ExecuteFile with a forced reload.
     * @details The file is read through CryPak, so the newest overlay wins, and it runs even though the script system
     *          ran it before. Every Script.ReloadScript or Script.LoadScript call the file makes while it runs reruns
     *          that file as well. A Lua error goes through the engine's error handler into the game log.
     * @param path Game-relative script path, for example "scripts/mods/mymod.lua".
     * @return False when the file is missing, raised a Lua error, or the script system is unavailable.
     * @note Main thread.
     */
    [[nodiscard]] bool rerun_script_file(const std::string &path);

    /**
     * @brief Runs Lua source through IScriptSystem::ExecuteBuffer in the global environment.
     * @param code The source.
     * @param chunk_name The Lua chunk name error messages carry; a leading '=' makes Lua print it as is.
     * @return False when the code raised a Lua error (logged by the engine) or the script system is unavailable.
     * @note Main thread.
     */
    [[nodiscard]] bool run_script_buffer(std::string_view code, const std::string &chunk_name);

    /**
     * @brief Lists the table databases registered with C_ObjectDatabaseManager.
     * @note Main thread. The manager is found on first use (a static object inside WHGame.dll).
     */
    [[nodiscard]] std::vector<DatabaseInfo> table_databases();

    /// Outcome of reload_table_database.
    struct TableReloadResult
    {
        /// The reload read the table and its patches, and the database linked its new rows.
        bool ok = false;
        /// Dependent databases linked again (direct and indirect).
        std::size_t relinked = 0;
        /// Dependent databases whose link was missing or reported failure.
        std::size_t relink_failed = 0;
    };

    /**
     * @brief Reloads one table database the way the manager's own reload (wh_db_ReloadObjectDatabase) does.
     * @details The manager raises its reloading flag around the database's Reload, which clears the rows and reads the
     *          table and its "<name>__*.xml" patches again through CryPak. A database whose Reload failed is not
     *          linked. Every database whose rows point into it, directly or through another, is then linked again in
     *          ascending dependency depth, so none keeps a pointer to a freed row.
     * @return The outcome, or std::nullopt when nothing ran: the manager is unavailable, or its dependency graph
     *         awaits a rebuild that only the manager's own reload performs (logged).
     * @note Main thread.
     */
    [[nodiscard]] std::optional<TableReloadResult> reload_table_database(std::uintptr_t database);

} // namespace HotPak

#endif // HOTPAK_ENGINE_HPP
