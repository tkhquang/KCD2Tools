/**
 * @file constants.hpp
 * @brief File names, engine layout constants and RTTI names for the HotPak dev tool.
 *
 * Every engine offset below belongs to WHGame.dll 1.5.x. Each one is only dereferenced through guarded reads, after the
 * object that holds it passed a vtable check: an RTTI identity, or for a table database (a template type per table) a
 * vtable inside the game image. A layout change in a game patch therefore fails the feature closed instead of reading
 * through a shifted structure.
 */
#ifndef HOTPAK_CONSTANTS_HPP
#define HOTPAK_CONSTANTS_HPP

#include "version.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace HotPak::constants
{
    /// Internal mod name; also the stem of the INI, log and ASI file names.
    inline constexpr const char *MOD_NAME = HotPak::version::MOD_NAME;

    /// INI file extension.
    inline constexpr const char *INI_FILE_EXTENSION = ".ini";

    /** @brief Returns the INI file name beside the ASI. */
    [[nodiscard]] inline std::string get_config_filename()
    {
        return std::string(MOD_NAME) + INI_FILE_EXTENSION;
    }

    /// Log file name beside the ASI.
    inline constexpr const char *LOG_FILE_NAME = "KCD2_HotPak.log";

    /// Prefix of the single-instance mutex.
    inline constexpr const char *INSTANCE_MUTEX_PREFIX = "KCD2_HotPak_";

    /// Default log level when the INI has none.
    inline constexpr const char *DEFAULT_LOG_LEVEL = "INFO";

    /// The module that holds every engine system this tool touches.
    inline constexpr const char *GAME_MODULE = "WHGame.dll";

    /// The C++ runtime both the game and this tool link. Its exported mutex functions guard the skin cache.
    inline constexpr const char *CPP_RUNTIME_MODULE = "msvcp140.dll";

    /// Folder under %LOCALAPPDATA% that holds the shadow copies CryPak mounts; deleting it is a clean uninstall.
    inline constexpr const wchar_t *SHADOW_FOLDER = L"KCD2_HotPak";

    /**
     * @brief How long one hook retirement waits for a thread inside a detour.
     * @details A PostUpdate normally returns within a frame, but a loading screen can hold one for seconds. A Busy
     *          retirement changes nothing, and the dev loader retries it.
     */
    inline constexpr std::chrono::milliseconds HOOK_RETIREMENT_DEADLINE{250};

    // SSystemGlobalEnvironment members (gEnv).
    inline constexpr std::ptrdiff_t GENV_SCRIPT_SYSTEM_OFFSET = 0x28;
    inline constexpr std::ptrdiff_t GENV_CRYPAK_OFFSET = 0x50;
    inline constexpr std::ptrdiff_t GENV_CHARACTER_MANAGER_OFFSET = 0xD0;
    inline constexpr std::ptrdiff_t GENV_RENDERER_OFFSET = 0x108;

    // CCryPak. The vtable keeps the SDK declaration order with two Warhorse slots inserted ahead of the file API, and
    // MSVC emits each pair of adjacent overloads in reverse order, so OpenPack(bindRoot, name, ...) comes before
    // OpenPack(name, ...).
    inline constexpr std::size_t CRYPAK_OPEN_PACK_BOUND_SLOT = 6;
    /// m_arrZips: std::vector<PackDesc> begin / end.
    inline constexpr std::ptrdiff_t CRYPAK_ZIPS_BEGIN_OFFSET = 0x120;
    inline constexpr std::ptrdiff_t CRYPAK_ZIPS_END_OFFSET = 0x128;
    /// sizeof(PackDesc): {CryStringT strBindRoot, CryStringT strFileName, ..., pArchive @0x28, pZip @0x30}.
    inline constexpr std::size_t PACK_DESC_STRIDE = 0x38;
    inline constexpr std::ptrdiff_t PACK_DESC_BIND_ROOT_OFFSET = 0x00;
    inline constexpr std::ptrdiff_t PACK_DESC_ZIP_OFFSET = 0x30;
    /// ZipDir::Cache stores its file path at zip + *(zip + 0x70) + 0x9A8 (the same expression ClosePack compares).
    inline constexpr std::ptrdiff_t ZIP_PATH_INDIRECT_OFFSET = 0x70;
    inline constexpr std::ptrdiff_t ZIP_PATH_BASE_OFFSET = 0x9A8;
    /**
     * @brief Flags the Warhorse mod manager passes when it mounts a mod's Data paks: ICryPak::FLAGS_PATH_REAL
     *        (0x10000) | ICryArchive::FLAGS_OVERRIDE_PAK (0x400).
     * @details An override pak is appended after every other pak, and CryPak searches its pak list newest first, so
     *          an overlay mounted with these flags wins over the installed copy of the same mod.
     */
    inline constexpr std::uint32_t OVERLAY_PAK_FLAGS = 0x10400;
    /// The bind root the mod manager uses for a mod's Data paks.
    inline constexpr const char *DEFAULT_BIND_ROOT = "data\\";

    // CScriptSystem (IScriptSystem). Slots 1-4 are Update, SetGCFrequency, SetEnvironment and GetEnvironment.
    /**
     * @brief IScriptSystem::ExecuteFile(name, raiseError, forceReload, env).
     * @details The file is read through CryPak. A file the script system already ran is skipped unless forceReload is
     *          set. A forced call also raises the system's force count while the file runs, so every nested
     *          ExecuteFile (Script.ReloadScript, Script.LoadScript) inside it runs its file again too.
     *          Script.ReloadScript itself never forces outside the Editor, which is why a mod script cannot reload
     *          itself from Lua that way.
     */
    inline constexpr std::size_t SCRIPT_EXECUTE_FILE_SLOT = 5;
    /// IScriptSystem::ExecuteBuffer(buffer, size, description, env); the description is the Lua chunk name.
    inline constexpr std::size_t SCRIPT_EXECUTE_BUFFER_SLOT = 6;
    /**
     * @brief Folder of mod init scripts. CryAction runs "scripts/mods/<modid>.lua" once at startup for every enabled
     *        mod, through ExecuteFile without a forced reload.
     */
    inline constexpr const char *MOD_SCRIPTS_PREFIX = "scripts/mods/";
    /// Method a mod's global table (named after its mod id) may define to undo its patches before its script reruns.
    inline constexpr const char *SCRIPT_HOT_RELOAD_METHOD = "OnHotReload";

    // CD3D9Renderer (IRenderer).
    /// IRenderer::EF_ReloadFile(const char*): synchronous; a .cgf goes through IStatObj::Refresh. Main thread only.
    inline constexpr std::size_t RENDERER_RELOAD_FILE_SLOT = 136;
    /// IRenderer::EF_ReloadFile_Request(const char*): queues a texture reload that CTexture::Update drains each frame.
    inline constexpr std::size_t RENDERER_RELOAD_FILE_REQUEST_SLOT = 137;

    // CharacterManager skin cache: a std::unordered_multimap<string, CSkin*> keyed by the lowercase file path.
    inline constexpr std::ptrdiff_t CM_SKIN_CACHE_HEAD_OFFSET = 0x140;
    inline constexpr std::ptrdiff_t CM_SKIN_CACHE_SIZE_OFFSET = 0x148;
    /**
     * @brief The std::mutex every cache lookup and insert holds.
     * @details Its _Mtx_t is opaque to everything but the C++ runtime DLL the game links, so only that DLL's own
     *          _Mtx_lock and _Mtx_unlock exports may take and release it.
     */
    inline constexpr std::ptrdiff_t CM_SKIN_CACHE_MUTEX_OFFSET = 0x178;
    inline constexpr const char *MUTEX_LOCK_EXPORT = "_Mtx_lock";
    inline constexpr const char *MUTEX_UNLOCK_EXPORT = "_Mtx_unlock";
    /// List node: {next, prev, std::string key @0x10, CSkin* @0x30}.
    inline constexpr std::ptrdiff_t SKIN_NODE_KEY_OFFSET = 0x10;
    inline constexpr std::ptrdiff_t SKIN_NODE_VALUE_OFFSET = 0x30;
    /// Upper bound on the cache walk; the live cache holds a few hundred skins.
    inline constexpr std::size_t SKIN_CACHE_WALK_LIMIT = 100000;

    // CSkin.
    /// Loading flags the skin was created with (CA_* bits).
    inline constexpr std::ptrdiff_t SKIN_FLAGS_OFFSET = 0x18;
    /// Intrusive reference count.
    inline constexpr std::ptrdiff_t SKIN_REFS_OFFSET = 0x20;
    /**
     * @brief CA_ImmediateMode (0x80). The cache lookup never hands out a skin that carries it: the caller gets a miss,
     *        loads the file again and registers the new skin beside the old one.
     */
    inline constexpr std::uint8_t SKIN_FLAG_UNSHARED = 0x80;

    // CAttachmentSKIN.
    /// The CSkin the attachment shows.
    inline constexpr std::ptrdiff_t ATTACHMENT_SKIN_OFFSET = 0x78;
    /// Attachment name (C string pointer), for example "hair_dog_fur".
    inline constexpr std::ptrdiff_t ATTACHMENT_NAME_OFFSET = 0x18;
    inline constexpr std::ptrdiff_t ATTACHMENT_MANAGER_OFFSET = 0x30;
    /// Stream state; 0 and 1 mean the skin is not there yet, and the character cannot swap in anything until it is.
    inline constexpr std::ptrdiff_t ATTACHMENT_STATE_OFFSET = 0x50;
    /// CAttachmentManager -> its CCharInstance.
    inline constexpr std::ptrdiff_t ATTACHMENT_MANAGER_INSTANCE_OFFSET = 0x70;
    /**
     * @brief CCharInstance's attachment list {begin, end}. CharacterManager checks every SKIN attachment in it before
     *        it swaps any pending one in; a SKIN attachment stuck before its skin arrives blocks them all, and the
     *        character is then dropped from the check until something queues it again.
     */
    inline constexpr std::ptrdiff_t INSTANCE_ATTACHMENTS_BEGIN_OFFSET = 0x80;
    inline constexpr std::ptrdiff_t INSTANCE_ATTACHMENTS_END_OFFSET = 0x88;

    // CMatInfo (IMaterial) and CMatMan (IMaterialManager). A material's CMatMan key is its lowercase path without
    // ".mtl".
    /**
     * @brief IMaterial::GetMaterialManager. Its body is `mov rax, [rip+disp32]; ret` reading the engine-wide CMatMan
     *        pointer, so decoding it finds the manager without a signature of its own.
     */
    inline constexpr std::size_t MATERIAL_GET_MANAGER_SLOT = 6;
    /**
     * @brief IMaterialManager::RenameMaterial(IMaterial*, const char*): erases the old key, renames, then stores the
     *        material under the new key.
     * @details The store assigns through the map's operator[], so a new key that is already in use replaces the
     *          material filed under it. A later release of either material then erases the key by name alone.
     */
    inline constexpr std::size_t MATMAN_RENAME_SLOT = 4;
    /// IMaterialManager::FindMaterial(const char*).
    inline constexpr std::size_t MATMAN_FIND_SLOT = 6;
    /// Inserted into the name of a material taken out of CMatMan's map ("<key>~hotpak<N>", N unique per process).
    inline constexpr const char *RETIRED_MATERIAL_MARKER = "~hotpak";
    /// CMatInfo's name (IMaterial::GetName reads this pointer): its CMatMan key, or a retired name.
    inline constexpr std::ptrdiff_t MATERIAL_NAME_OFFSET = 0x28;

    // wh::engine3d::C_MaterialInstance: the per-attachment material built over a CMatMan material.
    /// The CMatInfo the instance is built on.
    inline constexpr std::ptrdiff_t MATERIAL_INSTANCE_BASE_OFFSET = 0x10;

    // wh::databasemodule. Every table under Libs/Tables is one database object registered with the manager. It reads
    // "Libs/Tables/<subfolder>/<name>.xml" and every "<name>__*.xml" patch a mod ships.
    /// C_ObjectDatabaseManager: std::vector of database pointers.
    inline constexpr std::ptrdiff_t DB_MANAGER_LIST_BEGIN_OFFSET = 0x30;
    inline constexpr std::ptrdiff_t DB_MANAGER_LIST_END_OFFSET = 0x38;
    /**
     * @brief Set while the dependency graph (each database's depth and dependents) needs a rebuild. The manager's own
     *        reload rebuilds it first and clears the byte.
     */
    inline constexpr std::ptrdiff_t DB_MANAGER_GRAPH_DIRTY_OFFSET = 0xA0;
    /// Set by the manager around each database's reload call.
    inline constexpr std::ptrdiff_t DB_MANAGER_RELOADING_OFFSET = 0xA1;
    /// Dependency depth: 0 for a database that references no other, else one more than its deepest dependency.
    inline constexpr std::ptrdiff_t DB_DEPTH_OFFSET = 0x10;
    /**
     * @brief std::vector of the databases whose rows point into this one's rows (for example CharacterUberlod's
     *        sources into CharacterComponent).
     */
    inline constexpr std::ptrdiff_t DB_DEPENDENTS_BEGIN_OFFSET = 0x18;
    inline constexpr std::ptrdiff_t DB_DEPENDENTS_END_OFFSET = 0x20;
    inline constexpr std::ptrdiff_t DB_NAME_OFFSET = 0x50;
    inline constexpr std::ptrdiff_t DB_SUBFOLDER_OFFSET = 0x58;
    /// int Reload(): clears the rows and reads the table and its patches again through CryPak. Returns 0 on success.
    inline constexpr std::size_t DB_RELOAD_SLOT = 9;
    /// bool Link(): resolves the rows' references into other databases. Returns true on success.
    inline constexpr std::size_t DB_LINK_SLOT = 10;

    // wh::entitymodule::clothing::C_ClothingAttachmentProxy.
    /// The wh::entitymodule::clothing::C_SkinElement row the proxy streams: what it asks for, in every state.
    inline constexpr std::ptrdiff_t PROXY_ELEMENT_OFFSET = 0x28;
    /// The CAttachmentSKIN the proxy shows; Reset() releases it.
    inline constexpr std::ptrdiff_t PROXY_ATTACHMENT_OFFSET = 0x50;
    /// The C_MaterialInstance the attachment renders with; Reset() releases it with the attachment.
    inline constexpr std::ptrdiff_t PROXY_MATERIAL_INSTANCE_OFFSET = 0x60;
    /// E_StreamingState: Init, Loading, Streaming, WaitingForSwapToStreamed, Error.
    inline constexpr std::ptrdiff_t PROXY_STREAMING_STATE_OFFSET = 0x58;
    inline constexpr std::uint8_t PROXY_STATE_STREAMING = 2;
    /**
     * @brief WaitingForSwapToStreamed: the request built a new attachment, added to the character, and waits for the
     *        character to swap all of its pending attachments in together.
     */
    inline constexpr std::uint8_t PROXY_STATE_WAITING_FOR_SWAP = 3;
    /// The pending request of a proxy outside Streaming; the new CAttachmentSKIN it built is at request + 0x40.
    inline constexpr std::ptrdiff_t PROXY_REQUEST_OFFSET = 0x40;
    inline constexpr std::ptrdiff_t REQUEST_ATTACHMENT_OFFSET = 0x40;
    /**
     * @brief The manager swaps its proxies in together, so one proxy in this state also holds back every other
     *        reloaded proxy of the character (they wait in WaitingForSwapToStreamed with nothing attached).
     */
    inline constexpr std::uint8_t PROXY_STATE_ERROR = 4;

    // wh::entitymodule::clothing::C_SkinElement: CharacterComponent's <SkinElement Model= Material=>.
    /// Model file name as the table gives it (for example "shiba_fur.skin"), as a C string pointer.
    inline constexpr std::ptrdiff_t SKIN_ELEMENT_MODEL_OFFSET = 0x18;
    /// Material file name (for example "shiba_fur.mtl"), as a C string pointer.
    inline constexpr std::ptrdiff_t SKIN_ELEMENT_MATERIAL_OFFSET = 0x28;
    /// Reset(): detaches and releases the attachment and drops the proxy back to Init.
    inline constexpr std::size_t PROXY_RESET_SLOT = 3;

    // wh::entitymodule::clothing::C_ComponentAttachmentManager.
    inline constexpr std::ptrdiff_t MANAGER_PROXIES_OFFSET = 0x20;
    inline constexpr std::ptrdiff_t MANAGER_PROXY_COUNT_OFFSET = 0x28;
    /// Sanity bound on one manager's proxy array.
    inline constexpr std::size_t MANAGER_PROXY_LIMIT = 4096;

    // MSVC RTTI type-descriptor names of the objects above.
    inline constexpr const char *RTTI_SCRIPT_SYSTEM = ".?AVCScriptSystem@@";
    inline constexpr const char *RTTI_CRYPAK = ".?AVCCryPak@@";
    inline constexpr const char *RTTI_RENDERER = ".?AVCD3D9Renderer@@";
    inline constexpr const char *RTTI_CHARACTER_MANAGER = ".?AVCharacterManager@@";
    inline constexpr const char *RTTI_SKIN = ".?AVCSkin@@";
    inline constexpr const char *RTTI_ATTACHMENT_SKIN = ".?AVCAttachmentSKIN@@";
    inline constexpr const char *RTTI_CLOTHING_PROXY = ".?AVC_ClothingAttachmentProxy@clothing@entitymodule@wh@@";
    inline constexpr const char *RTTI_SKIN_ELEMENT = ".?AVC_SkinElement@clothing@entitymodule@wh@@";
    inline constexpr const char *RTTI_MATERIAL_INFO = ".?AVCMatInfo@@";
    inline constexpr const char *RTTI_MATERIAL_MANAGER = ".?AVCMatMan@@";
    inline constexpr const char *RTTI_MATERIAL_INSTANCE = ".?AVC_MaterialInstance@engine3d@wh@@";
    inline constexpr const char *RTTI_DATABASE_MANAGER = ".?AVC_ObjectDatabaseManager@databasemodule@wh@@";
} // namespace HotPak::constants

#endif // HOTPAK_CONSTANTS_HPP
