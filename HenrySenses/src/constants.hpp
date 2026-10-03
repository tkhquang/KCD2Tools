/**
 * @file constants.hpp
 * @brief File names, engine struct offsets, vtable slots and RTTI names shared across the mod.
 *
 * Code and data locations inside WHGame.dll are never listed here as addresses: they are resolved by the AOB
 * cascades in aob_resolver.hpp. What remains here are struct member offsets and vtable slots, which the consumers
 * read through guarded memory and validate by RTTI where the pointee carries a type name.
 */
#ifndef HENRYSENSES_CONSTANTS_HPP
#define HENRYSENSES_CONSTANTS_HPP

#include "version.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

/**
 * @namespace HenrySenses::constants
 * @brief Global constants and configuration defaults.
 */
namespace HenrySenses::constants
{
    /// Mod name from version.hpp; the stem of the INI and log file names and the instance mutex.
    inline constexpr const char *MOD_NAME = HenrySenses::version::MOD_NAME;

    /// INI configuration file extension.
    inline constexpr const char *INI_FILE_EXTENSION = ".ini";

    /** @brief Returns the INI config file name ("KCD2_HenrySenses.ini"). */
    [[nodiscard]] inline std::string get_config_filename()
    {
        return std::string(MOD_NAME) + INI_FILE_EXTENSION;
    }

    /// Log file name passed to the DMK Session.
    inline constexpr const char *LOG_FILE_NAME = "KCD2_HenrySenses.log";
    /// Per-process instance-mutex prefix so a duplicate ASI load bails cleanly.
    inline constexpr const char *INSTANCE_MUTEX_PREFIX = "KCD2_HenrySenses_";
    /// Default logging level.
    inline constexpr const char *DEFAULT_LOG_LEVEL = "INFO";

    /// Name of the game module every anchor and offset lives in.
    inline constexpr const char *MODULE_NAME = "WHGame.dll";

    // The signature file beside the ASI: optional repairs of the built-in signatures, merged over them by label.
    inline constexpr const char *SIGNATURE_FILE_SUFFIX = ".signatures.ini";
    // The [Settings] ExportSignatures output: every built-in signature with its captured baselines.
    inline constexpr const char *SIGNATURE_EXPORT_SUFFIX = ".signatures.captured.ini";
    // The signature-contract epoch a signature file must declare. Bump it only when an in-code change makes older
    // signature files incompatible (a renamed label, a dropped signature); a file for another epoch is ignored.
    inline constexpr std::uint32_t SIGNATURE_REVISION = 1;

    // SSystemGlobalEnvironment (gEnv) members. gEnv is an inline struct inside WHGame.dll (AnchorId::Genv), so each
    // interface pointer is read fresh at gEnv + offset: several are null until the engine has created them.
    inline constexpr std::ptrdiff_t GENV_3DENGINE_OFFSET = 0x08;
    inline constexpr std::ptrdiff_t GENV_CRY_PAK_OFFSET = 0x50;
    // The IParticleManager secondary base of CParticleManager (the object's primary base is 8 bytes before it).
    inline constexpr std::ptrdiff_t GENV_PARTICLE_MANAGER_OFFSET = 0x68;
    inline constexpr std::ptrdiff_t GENV_PGAME_OFFSET = 0x90;
    inline constexpr std::ptrdiff_t GENV_ENTITY_SYSTEM_OFFSET = 0xA0;
    inline constexpr std::ptrdiff_t GENV_SYSTEM_OFFSET = 0xC0; // CSystem
    inline constexpr std::ptrdiff_t GENV_RENDERER_OFFSET = 0x108;

    // Local player chain: gEnv -> pGame -> IGame::GetIGameFramework (vtable slot 16) = CCryAction -> CActionGame ->
    // C_Player (validated by RTTI) -> CEntity*.
    inline constexpr std::ptrdiff_t IGAME_GET_FRAMEWORK_VTABLE_OFFSET = 0x80;
    inline constexpr std::ptrdiff_t CCRYACTION_ACTIONGAME_OFFSET = 0x88;
    inline constexpr std::ptrdiff_t CACTIONGAME_LOCAL_ACTOR_OFFSET = 0xA40;
    inline constexpr std::ptrdiff_t C_PLAYER_ENTITY_OFFSET = 0x38;
    // C_HitDeathReactions pointer; paired with the entity pointer as the corroborating anchor that lets the
    // self-heal recover a shifted entity offset without latching onto an unrelated CEntity neighbour.
    inline constexpr std::ptrdiff_t C_PLAYER_HIT_DEATH_REACTIONS_OFFSET = 0x240;

    // CEntity members.
    inline constexpr std::ptrdiff_t ENTITY_WORLD_MATRIX_OFFSET = 0x58; // Matrix34, translation in column 3
    inline constexpr std::ptrdiff_t ENTITY_NAME_OFFSET = 0xE0;         // const char *
    // CEntity::GetProxy(this, type) -> IEntityProxy*, vtable slot 74.
    inline constexpr std::ptrdiff_t ENTITY_VTABLE_GET_PROXY_OFFSET = 74 * 8;
    // Proxy map key of the render proxy.
    inline constexpr std::uint32_t ENTITY_PROXY_RENDER = 0;
    // Proxy map key of the script proxy; its vtable slot 18 returns the entity's IScriptTable.
    inline constexpr std::uint32_t ENTITY_PROXY_SCRIPT = 2;
    inline constexpr std::ptrdiff_t SCRIPT_PROXY_VTABLE_GET_TABLE_OFFSET = 18 * 8;
    // IScriptTable::GetValueAny(this, key, ScriptAnyValue *value, bool ignoreMeta), vtable slot 8. The value's type
    // (int at +0) asks for a conversion and reports what was read; the payload is at +8.
    inline constexpr std::ptrdiff_t SCRIPT_TABLE_VTABLE_GET_VALUE_ANY_OFFSET = 8 * 8;
    inline constexpr std::int32_t SCRIPT_ANY_BOOLEAN = 2;
    // ANY_THANDLE: a light userdata such as a WUID; the handle is the 64-bit payload.
    inline constexpr std::int32_t SCRIPT_ANY_HANDLE = 3;
    inline constexpr std::int32_t SCRIPT_ANY_NUMBER = 4;
    inline constexpr std::size_t SCRIPT_ANY_VALUE_SIZE = 0x20;

    // CRenderProxy layout. The primary base is the IRenderNode; the entity's proxy map stores the IEntityRenderProxy
    // secondary base, so the render node is the map value minus RENDER_PROXY_SECONDARY_OFFSET.
    inline constexpr std::ptrdiff_t RENDER_PROXY_SECONDARY_OFFSET = 0x50;
    inline constexpr std::ptrdiff_t RENDER_PROXY_ENTITY_OFFSET = 0x80;      // CEntity *m_pEntity
    inline constexpr std::ptrdiff_t RENDER_PROXY_SLOTS_BEGIN_OFFSET = 0x88; // CEntitySlot ** vector begin
    inline constexpr std::ptrdiff_t RENDER_PROXY_SLOTS_END_OFFSET = 0x90;   // CEntitySlot ** vector end
    // CEntitySlot: the static mesh (CStatObj *) and the character (ICharacterInstance *) it shows.
    inline constexpr std::ptrdiff_t ENTITY_SLOT_STAT_OBJ_OFFSET = 0x78;
    inline constexpr std::ptrdiff_t ENTITY_SLOT_CHARACTER_OFFSET = 0x80;
    // CEntitySlot child render node (IRenderNode *): the slot's particle emitter, light or geometry-cache node. The
    // slot's GetParticleEmitter (0x1807F1CB8) returns it when its GetRenderNodeType is 8.
    inline constexpr std::ptrdiff_t ENTITY_SLOT_CHILD_RENDER_NODE_OFFSET = 0x90;

    // IParticleManager::FindEffect(this, name, source, bLoadResources) -> IParticleEffect *, vtable slot 6 of the
    // IParticleManager base. It loads the effect's library (Libs/Particles/<library>.xml) on first use and returns
    // null for an unknown or disabled effect.
    inline constexpr std::ptrdiff_t PARTICLE_MANAGER_VTABLE_FIND_EFFECT_OFFSET = 6 * 8;
    // CParticleManager's primary base, 8 bytes before the IParticleManager base gEnv holds.
    inline constexpr std::ptrdiff_t PARTICLE_MANAGER_PRIMARY_DELTA = 8;
    // The live emitter list of CParticleManager (from its primary base): head, tail, then the u32 count at +0x10.
    // Each CParticleEmitter links itself: next at +0x250, previous at +0x258 (every build since 1.4). Level unloads
    // destroy the listed emitters, so a free emitter is touched only after it is found here again.
    inline constexpr std::ptrdiff_t PARTICLE_MANAGER_EMITTERS_OFFSET = 0x1B8;
    inline constexpr std::ptrdiff_t PARTICLE_EMITTER_LIST_COUNT_OFFSET = 0x10;
    inline constexpr std::ptrdiff_t PARTICLE_EMITTER_NEXT_OFFSET = 0x250;
    // CParticleEmitter location (QuatTS: quaternion x, y, z, w, position, uniform scale).
    inline constexpr std::ptrdiff_t PARTICLE_EMITTER_LOCATION_OFFSET = 0x70;
    // IParticleEmitter::Kill, vtable slot 76 (what IParticleManager::DeleteEmitter calls): ends the emitter, which the
    // manager then erases once it holds the only reference.
    inline constexpr std::ptrdiff_t PARTICLE_EMITTER_VTABLE_KILL_OFFSET = 76 * 8;

    // IParticleManager::LoadLibrary(this, name, XmlNodeRef &library, bLoadResources) -> bool, vtable slot 10 (MSVC
    // puts the overload after LoadLibrary(name, file, bLoadResources), slot 9). Each <Particles> child becomes the
    // effect "<name>.<its Name>" through LoadEffect (slot 7), which returns an effect that already exists untouched.
    // A level unload clears the loaded libraries and effects (ClearRenderResources).
    inline constexpr std::ptrdiff_t PARTICLE_MANAGER_VTABLE_LOAD_LIBRARY_OFFSET = 10 * 8;

    // CSystem's CXmlUtils (the ISystem XML calls forward to it). CXmlUtils::LoadXmlFromBuffer(this, XmlNodeRef *out,
    // buffer, size, bReuseStrings, bSuppressWarnings), vtable slot 2; bReuseStrings = false builds CXmlNode trees.
    inline constexpr std::ptrdiff_t SYSTEM_XML_UTILS_OFFSET = 0xA20;
    inline constexpr std::ptrdiff_t XML_UTILS_VTABLE_LOAD_FROM_BUFFER_OFFSET = 2 * 8;
    // IXmlNode reference count: AddRef vtable slot 3, Release slot 4 (what XmlNodeRef's copy and destructor call).
    inline constexpr std::ptrdiff_t XML_NODE_VTABLE_RELEASE_OFFSET = 4 * 8;

    // The optional particle library beside the ASI ("KCD2_HenrySenses.particles.xml"), registered under this library
    // name, so its effects are "HenrySenses.<Particles Name>".
    inline constexpr const char *PARTICLE_LIBRARY_FILE_SUFFIX = ".particles.xml";
    inline constexpr const char *PARTICLE_LIBRARY_NAME = "HenrySenses";
    // CStatObj model path (const char *; CBrush::GetName returns it). CCharInstance model path (const char *),
    // what ICharacterInstance::GetFilePath (vtable slot 31) returns.
    inline constexpr std::ptrdiff_t STAT_OBJ_FILE_PATH_OFFSET = 0xA0;
    inline constexpr std::ptrdiff_t CHAR_INSTANCE_FILE_PATH_OFFSET = 0xA38;

    // IRenderNode members.
    inline constexpr std::ptrdiff_t RENDERNODE_RNDFLAGS_OFFSET = 0x28; // u64 m_dwRndFlags
    // IRenderNode virtual slots (CBrush vtable 0x18401FE70): 3 Render, 4 GetBBox(this, AABB *out), 7
    // GetRenderNodeType, 9 GetName (a brush's statobj path).
    inline constexpr std::ptrdiff_t RENDERNODE_VTABLE_GET_BBOX_OFFSET = 0x20;
    inline constexpr std::ptrdiff_t RENDERNODE_VTABLE_GET_TYPE_OFFSET = 0x38;
    inline constexpr std::ptrdiff_t RENDERNODE_VTABLE_GET_NAME_OFFSET = 0x48;
    // GetRenderNodeType values: CBrush and COwnedBrush 1, CVegetation 2, CMovableBrush 0x11, CRenderProxy 0x13,
    // CMergedMeshRenderNode 0x17.
    inline constexpr std::uint32_t RENDERNODE_TYPE_BRUSH = 1;
    inline constexpr std::uint32_t RENDERNODE_TYPE_VEGETATION = 2;
    inline constexpr std::uint32_t RENDERNODE_TYPE_MOVABLE_BRUSH = 0x11;
    inline constexpr std::uint32_t RENDERNODE_TYPE_RENDER_PROXY = 0x13;
    inline constexpr std::uint32_t RENDERNODE_TYPE_MERGED_MESH = 0x17;
    inline constexpr std::ptrdiff_t RENDERNODE_TEMP_DATA_OFFSET = 0x30; // SRenderNodeTempData *
    // u32 HUD silhouette word 0xRRGGBBAA (A = 0 flat fill, A > 0 outline, 0 = off). Exactly 4 bytes may be written:
    // the qword at +0x40 is an atomic the engine owns.
    inline constexpr std::ptrdiff_t RENDERNODE_HUD_SILHOUETTE_OFFSET = 0x3C;
    // Invalidating a persistent render object: atomically store the temp-data dword at +0xF8 into +0xFC, then store
    // -1 at node + RENDERNODE_INVALIDATE_STAMP_OFFSET, so the next frame refills the object from SRendParams instead
    // of taking the cached fast path. The engine uses the same sequence when a node's render state changes.
    inline constexpr std::ptrdiff_t RENDERNODE_INVALIDATE_STAMP_OFFSET = 0x38;
    inline constexpr std::ptrdiff_t TEMP_DATA_STAMP_SOURCE_OFFSET = 0xF8;
    inline constexpr std::ptrdiff_t TEMP_DATA_STAMP_TARGET_OFFSET = 0xFC;

    // ERenderNodeFlags bits read or toggled by the mod.
    inline constexpr std::uint64_t ERF_RENDER_ALWAYS = 0x10;
    inline constexpr std::uint64_t ERF_HIDDEN = 0x100;
    // Set on the render nodes of a pickable vegetation group. A picked CVegetation instance (a mushroom) keeps it and
    // gains ERF_HIDDEN until it respawns.
    inline constexpr std::uint64_t ERF_PICKABLE = 0x8000;

    // SRendParams member carrying the HUD silhouette word into the render object.
    inline constexpr std::ptrdiff_t SRENDPARAMS_HUD_SILHOUETTE_OFFSET = 0x78;
    // Other SRendParams fields CStatObj::Render (0x18049E258) reads for a temporary render object: pMatrix, fAlpha,
    // the sub-object hide mask (+0x88 pointer, +0x90 count; zero = none) and the CLodValue (int16 lodA, int16 lodB,
    // u8 dissolveRef). Every field fits in the buffer size.
    inline constexpr std::ptrdiff_t SRENDPARAMS_MATRIX_OFFSET = 0x00;
    inline constexpr std::ptrdiff_t SRENDPARAMS_ALPHA_OFFSET = 0x60;
    // float fDistance, the object's camera distance: CStatObj::Render copies it into CRenderObject m_fDistance
    // (0x18049E6E8), the key the silhouette pass sorts far to near.
    inline constexpr std::ptrdiff_t SRENDPARAMS_DISTANCE_OFFSET = 0x64;
    inline constexpr std::ptrdiff_t SRENDPARAMS_LOD_OFFSET = 0xAC;
    inline constexpr std::size_t SRENDPARAMS_BUFFER_SIZE = 0x100;
    // CRenderObject: m_ObjFlags at +0, m_fAlpha at +8 (CStatObj::Render copies SRendParams::fAlpha into it,
    // 0x18049E67D) and the HUD silhouette word the octree's brush render copies from the brush (+0x3C) just before
    // CStatObj::RenderInternal.
    inline constexpr std::ptrdiff_t RENDER_OBJECT_FLAGS_OFFSET = 0x00;
    inline constexpr std::ptrdiff_t RENDER_OBJECT_ALPHA_OFFSET = 0x08;
    inline constexpr std::ptrdiff_t RENDER_OBJECT_HUD_SILHOUETTE_OFFSET = 0xA4;
    // SRendParams::dwFObjFlags (u64): CRenderProxy::Render ORs FOB_IN_DOORS (0x100) into it, and its copy constructor
    // carries it into every slot, where the geometry ORs it into CRenderObject::m_ObjFlags.
    inline constexpr std::ptrdiff_t SRENDPARAMS_OBJ_FLAGS_OFFSET = 0x68;
    // SGraphicsPipelineStateDescription::objectFlags (u64). CSceneCustomStage::CreatePipelineState reads bit 33
    // (FOB_HUD_REQUIRE_DEPTHTEST) from it for the silhouette pass. The per-material PSO cache hashes the whole
    // description, so objects that differ only in objectFlags get separate cache entries.
    inline constexpr std::ptrdiff_t PSO_DESC_OBJECT_FLAGS_OFFSET = 0x28;
    // Render-object flag the mod adds to the objects it highlights, so only they get a silhouette PSO. Bit 55 is
    // never a render-object flag in 1.5.6: its only uses in the image are the render-node flag setter at
    // 0x180400550 (IRenderNode+0x28) and a bit table; no code tests it in m_ObjFlags or in a PSO description.
    inline constexpr std::uint64_t FOB_HENRYSENSES_MARKER = 1ull << 55;
    // FOB_HUD_REQUIRE_DEPTHTEST. With it, CSceneCustomStage::CreatePipelineState gives the silhouette PSO a LEQUAL
    // depth test (reverse-depth converted by sub_1807C4878) instead of GS_NODEPTHTEST (0x800000). The mask pass depth
    // target is the view's scene depth (CSceneCustomStage::Update, vtable slot 4, binds CRenderView::GetDepthTarget
    // with $SceneNormalsMap every frame), so only the unoccluded part of the object reaches the mask. CBrush sets it
    // from ERF_HUD_REQUIRE_DEPTHTEST (render-node bit 37, 0x1806BAF9E); the mod sets it with the marker for
    // [Render] SeeThrough = false.
    inline constexpr std::uint64_t FOB_HUD_REQUIRE_DEPTHTEST = 1ull << 33;

    // CPrimitiveRenderPass::SetRenderTarget is a thunk (add rcx, 58h; jmp) to its implementation, which takes the pass
    // pointer plus this offset. The anchor names the implementation, whose body a signature can tell apart.
    inline constexpr std::ptrdiff_t PRIMITIVE_PASS_RENDER_TARGET_THIS_OFFSET = 0x58;

    // Global context (AnchorId::Context) members used by the dialogue / combat / minigame gates.
    inline constexpr std::ptrdiff_t OFFSET_MANAGER_PTR_STORAGE = 0x38; // context -> wh::game::C_CameraManager
    inline constexpr std::ptrdiff_t OFFSET_ACTIVE_CAMERA = 0x30;       // camera manager -> active wh::game camera
    inline constexpr std::ptrdiff_t OFFSET_MINIGAME_SUBSYSTEM = 0x128; // context -> C_PlayerModule
    inline constexpr std::ptrdiff_t OFFSET_MINIGAME_MANAGER = 0x18;    // C_PlayerModule -> C_MinigameManager
    inline constexpr std::ptrdiff_t OFFSET_MINIGAME_MAP_HEAD = 0x20;   // manager -> active-minigame list sentinel
    inline constexpr std::ptrdiff_t OFFSET_MINIGAME_NODE_NEXT = 0x00;  // list node -> next node
    inline constexpr std::ptrdiff_t OFFSET_MINIGAME_NODE_VALUE = 0x28; // list node -> I_Minigame *
    inline constexpr std::ptrdiff_t OFFSET_MINIGAME_OWNER = 0x18;      // I_Minigame -> owning actor

    // Gameplay natives (engine/game_natives.hpp). The global context is a module registry.
    inline constexpr std::ptrdiff_t CONTEXT_GAME_OFFSET = 0x18;          // wh::game::C_Game
    inline constexpr std::ptrdiff_t CONTEXT_ENTITY_MODULE_OFFSET = 0xE0; // wh::entitymodule::C_EntityModule
    inline constexpr std::ptrdiff_t CONTEXT_SHOP_MODULE_OFFSET = 0x110;  // wh::shopmodule::C_ShopModule
    inline constexpr std::ptrdiff_t CONTEXT_RPG_MODULE_OFFSET = 0x130;   // wh::rpgmodule::C_RPGModule
    inline constexpr std::ptrdiff_t CONTEXT_ACTOR_SYSTEM_OFFSET = 0x180; // CActorSystem
    // CCryAction: the actor system (IGameFramework::GetIActorSystem returns it) and the item system, whose
    // IItemSystem interface sits at +8 (IGameFramework::GetIItemSystem).
    inline constexpr std::ptrdiff_t CRYACTION_ACTOR_SYSTEM_OFFSET = 0x518;
    inline constexpr std::ptrdiff_t CRYACTION_ITEM_SYSTEM_OFFSET = 0x520;
    inline constexpr std::ptrdiff_t ITEM_SYSTEM_INTERFACE_OFFSET = 0x08;
    // CActorSystem: EntityId -> IActor* hash map. Its list sentinel is at +0x40; a list node is { next, prev,
    // EntityId key at +0x10, IActor * at +0x18 }. IActorSystem::GetActor(this, id) is vtable slot 3.
    inline constexpr std::ptrdiff_t ACTOR_SYSTEM_LIST_HEAD_OFFSET = 0x40;
    inline constexpr std::ptrdiff_t ACTOR_NODE_KEY_OFFSET = 0x10;
    inline constexpr std::ptrdiff_t ACTOR_NODE_VALUE_OFFSET = 0x18;
    inline constexpr std::ptrdiff_t ACTOR_SYSTEM_VTABLE_GET_ACTOR_OFFSET = 3 * 8;
    // IItemSystem (interface): std::map<EntityId, IItem *> head node at +0x70; a tree node is { left, parent, right,
    // colour u8 at +0x18, is-nil u8 at +0x19, EntityId key at +0x20, IItem * at +0x28 }. GetItem(this, id) is
    // vtable slot 21.
    inline constexpr std::ptrdiff_t ITEM_MAP_HEAD_OFFSET = 0x70;
    // The map's element count follows its head (MSVC _Tree_val { _Myhead, _Mysize }): AddItem (slot 19, the emplace
    // sub_180707460) checks it against max_size, RemoveItem (slot 20, the erase sub_1808E037C) decrements it.
    inline constexpr std::ptrdiff_t ITEM_MAP_SIZE_OFFSET = ITEM_MAP_HEAD_OFFSET + 0x08;
    inline constexpr std::ptrdiff_t ITEM_NODE_IS_NIL_OFFSET = 0x19;
    inline constexpr std::ptrdiff_t ITEM_NODE_KEY_OFFSET = 0x20;
    inline constexpr std::ptrdiff_t ITEM_NODE_VALUE_OFFSET = 0x28;
    inline constexpr std::ptrdiff_t ITEM_SYSTEM_VTABLE_GET_ITEM_OFFSET = 21 * 8;
    // Actor (C_Actor): CEntity * at +0x38 and C_Soul * at +0x668; IsDead is vtable slot 38, GetHealth (float) 30.
    // For player.actor:CanLoot the looter's carried corpse (vtable slot 14 = its EntityId) is at +0x188 and its
    // looter state (vtable slot 59 = looting blocked) at +0x278; the victim's state object (vtable slot 83 =
    // IsInState(u8)) is at +0x990.
    inline constexpr std::ptrdiff_t ACTOR_ENTITY_OFFSET = 0x38;
    inline constexpr std::ptrdiff_t ACTOR_SOUL_OFFSET = 0x668;
    inline constexpr std::ptrdiff_t ACTOR_VTABLE_IS_DEAD_OFFSET = 38 * 8;
    inline constexpr std::ptrdiff_t ACTOR_VTABLE_GET_HEALTH_OFFSET = 30 * 8;
    inline constexpr std::ptrdiff_t ACTOR_CARRIED_CORPSE_OFFSET = 0x188;
    inline constexpr std::ptrdiff_t ACTOR_LOOTER_STATE_OFFSET = 0x278;
    inline constexpr std::ptrdiff_t ACTOR_STATE_OFFSET = 0x990;
    inline constexpr std::ptrdiff_t CARRIED_CORPSE_VTABLE_ID_OFFSET = 14 * 8;
    inline constexpr std::ptrdiff_t LOOTER_STATE_VTABLE_BLOCKED_OFFSET = 59 * 8;
    inline constexpr std::ptrdiff_t ACTOR_STATE_VTABLE_IS_IN_OFFSET = 83 * 8;
    // Victim states CanLoot asks for: one of the two must hold, and the third must not.
    inline constexpr std::uint8_t ACTOR_STATE_DEAD = 22;
    inline constexpr std::uint8_t ACTOR_STATE_UNCONSCIOUS = 9;
    inline constexpr std::uint8_t ACTOR_STATE_CARRIED = 12;
    // An actor's AI, wh::xgenaimodule::C_NPC (1.5.6). Its constructor (sub_180BDC5F8) stores its member
    // C_MovementControllerAdapter (C_NPC + 0x780) in the actor at +0x9A8. The C_NPC's actor link (built at +0x5C0 by
    // sub_180901FD4) keeps the bound actor's EntityId at C_NPC + 0x600: sub_180903B48 writes it when the AI binds to
    // its actor and writes 0 when the AI lets go (a despawn: sub_180873348, vtable slot 108 sub_1816EE578; a level
    // reset). Nothing tied to view or distance lets go: an NPC asleep out of view keeps the id, a disposed body has 0.
    inline constexpr std::ptrdiff_t ACTOR_MOVEMENT_CONTROLLER_OFFSET = 0x9A8;
    inline constexpr std::ptrdiff_t AI_NPC_MOVEMENT_ADAPTER_OFFSET = 0x780;
    inline constexpr std::ptrdiff_t AI_NPC_ACTOR_ID_OFFSET = 0x600;
    // C_Soul: WUID at +0x30; vtable slots 7 IsUnconscious, 27 IsLootable, 50 reputation component, 60 inventory
    // holder (C_InventorySoul, whose slot 0 returns the C_Inventory), 144 CanBeButchered.
    inline constexpr std::ptrdiff_t SOUL_WUID_OFFSET = 0x30;
    inline constexpr std::ptrdiff_t SOUL_VTABLE_IS_UNCONSCIOUS_OFFSET = 7 * 8;
    inline constexpr std::ptrdiff_t SOUL_VTABLE_IS_LOOTABLE_OFFSET = 27 * 8;
    inline constexpr std::ptrdiff_t SOUL_VTABLE_REPUTATION_OFFSET = 50 * 8;
    inline constexpr std::ptrdiff_t SOUL_VTABLE_INVENTORY_HOLDER_OFFSET = 60 * 8;
    inline constexpr std::ptrdiff_t SOUL_VTABLE_CAN_BE_BUTCHERED_OFFSET = 144 * 8;
    inline constexpr std::ptrdiff_t INVENTORY_HOLDER_VTABLE_GET_OFFSET = 0;
    // Reputation component: vtable slot 14 = HasFactionTag(tag); with AnchorId::PublicEnemyTag it is IsPublicEnemy.
    inline constexpr std::ptrdiff_t REPUTATION_VTABLE_HAS_TAG_OFFSET = 14 * 8;
    // Script context 84 makes a soul legal to loot (soul:IsLegalToLoot).
    inline constexpr std::uint8_t SCRIPT_CONTEXT_LEGAL_TO_LOOT = 84;
    // C_Game vtable slot 35 = C_GameModel, whose slot 7 = C_ScriptContextManager: slot 7 HasContext(soulWuid,
    // context entry), slot 16 HasContextById(u8 id, soulWuid).
    inline constexpr std::ptrdiff_t GAME_VTABLE_MODEL_OFFSET = 35 * 8;
    inline constexpr std::ptrdiff_t GAME_MODEL_VTABLE_SCRIPT_CONTEXTS_OFFSET = 7 * 8;
    inline constexpr std::ptrdiff_t SCRIPT_CONTEXTS_VTABLE_HAS_OFFSET = 7 * 8;
    inline constexpr std::ptrdiff_t SCRIPT_CONTEXTS_VTABLE_HAS_ID_OFFSET = 16 * 8;
    // AnchorId::ScriptContextMap: the context name table, a vector of 16-byte entries { const char *name, ... } from
    // +0x30 to +0x38; a context is identified by the address of its entry.
    inline constexpr std::ptrdiff_t SCRIPT_CONTEXT_MAP_BEGIN_OFFSET = 0x30;
    inline constexpr std::ptrdiff_t SCRIPT_CONTEXT_MAP_END_OFFSET = 0x38;
    inline constexpr std::size_t SCRIPT_CONTEXT_ENTRY_SIZE = 0x10;
    // C_Inventory: C_Item * vector at +0x08..+0x10; the interface at +0xE8 has slot 0 = WUID *, slot 5 = locked,
    // slot 6 = no player-visible item left; +0x179 = the lock applies.
    inline constexpr std::ptrdiff_t INVENTORY_ITEMS_BEGIN_OFFSET = 0x08;
    inline constexpr std::ptrdiff_t INVENTORY_ITEMS_END_OFFSET = 0x10;
    inline constexpr std::ptrdiff_t INVENTORY_INTERFACE_OFFSET = 0xE8;
    inline constexpr std::ptrdiff_t INVENTORY_LOCKABLE_OFFSET = 0x179;
    inline constexpr std::ptrdiff_t INVENTORY_VTABLE_WUID_OFFSET = 0;
    inline constexpr std::ptrdiff_t INVENTORY_VTABLE_LOCKED_OFFSET = 5 * 8;
    inline constexpr std::ptrdiff_t INVENTORY_VTABLE_EMPTY_FOR_PLAYER_OFFSET = 6 * 8;
    // WUID handle tables (inventory manager at C_EntityModule + 0xE8, table at +0x28; RPG module soul table at
    // *(module + 0x80) + 0x38): entry i has its generation (u16) at table + 16 * i + 0x28 and the object at
    // table + 16 * i + 0x30. A WUID is { u16 index, u16 generation, ..., u8 type in the top byte }.
    inline constexpr std::ptrdiff_t ENTITY_MODULE_INVENTORY_MANAGER_OFFSET = 0xE8;
    inline constexpr std::ptrdiff_t INVENTORY_MANAGER_TABLE_OFFSET = 0x28;
    inline constexpr std::ptrdiff_t RPG_MODULE_SOULS_OFFSET = 0x80;
    inline constexpr std::ptrdiff_t SOUL_MANAGER_TABLE_OFFSET = 0x38;
    inline constexpr std::ptrdiff_t WUID_TABLE_GENERATION_OFFSET = 0x28;
    inline constexpr std::ptrdiff_t WUID_TABLE_OBJECT_OFFSET = 0x30;
    inline constexpr std::size_t WUID_TABLE_STRIDE = 0x10;
    inline constexpr std::uint8_t WUID_TYPE_INVENTORY = 3;
    inline constexpr std::uint8_t WUID_TYPE_SOUL = 5;
    // C_PickableItem: C_Item * at +0x58, in-use bit 0 of the byte at +0x60; vtable slot 91 = CanSteal(user).
    // C_Item: class at +0x48 (class vtable slot 3 = IsA(id)), flags u32 at +0x60 (0x20000 = NPC only), owner at
    // +0x98, else +0x90.
    inline constexpr std::ptrdiff_t PICKABLE_ITEM_ENTITY_OFFSET = 0x38;
    inline constexpr std::ptrdiff_t PICKABLE_ITEM_DATA_OFFSET = 0x58;
    inline constexpr std::ptrdiff_t PICKABLE_ITEM_STATE_OFFSET = 0x60;
    inline constexpr std::uint8_t PICKABLE_ITEM_IN_USE = 0x01;
    inline constexpr std::ptrdiff_t PICKABLE_ITEM_VTABLE_CAN_STEAL_OFFSET = 91 * 8;
    inline constexpr std::ptrdiff_t ITEM_CLASS_OFFSET = 0x48;
    inline constexpr std::ptrdiff_t ITEM_FLAGS_OFFSET = 0x60;
    inline constexpr std::uint32_t ITEM_FLAG_NPC_ONLY = 0x20000;
    inline constexpr std::ptrdiff_t ITEM_OWNER_OFFSET = 0x98;
    inline constexpr std::ptrdiff_t ITEM_OWNER_FALLBACK_OFFSET = 0x90;
    inline constexpr std::ptrdiff_t ITEM_CLASS_VTABLE_IS_A_OFFSET = 3 * 8;
    // item:CanUse accepts only items whose class IsA this id (PlayerItem).
    inline constexpr std::uint32_t ITEM_CLASS_PLAYER_ITEM = 25;
    // C_Stash: its own C_Inventory at +0xB8 (the script field inventoryId mirrors its WUID).
    inline constexpr std::ptrdiff_t STASH_INVENTORY_OFFSET = 0xB8;
    // C_ShopModule: shop manager at +0x10, whose shop list sentinel is at +0x08; a list node is { next, prev, ...,
    // C_Shop * at +0x18 }. C_Shop: goods interface at +0x08 (slot 3 = holds C_Item), stash links object at +0xA8
    // (slot 117 = first link; a link is { ..., EntityId at +0x08, ..., next at +0x18 }).
    inline constexpr std::ptrdiff_t SHOP_MODULE_MANAGER_OFFSET = 0x10;
    inline constexpr std::ptrdiff_t SHOP_MANAGER_LIST_OFFSET = 0x08;
    inline constexpr std::ptrdiff_t SHOP_NODE_SHOP_OFFSET = 0x18;
    inline constexpr std::ptrdiff_t SHOP_GOODS_OFFSET = 0x08;
    inline constexpr std::ptrdiff_t SHOP_GOODS_VTABLE_HOLDS_OFFSET = 3 * 8;

    // CEntity identity and class.
    inline constexpr std::ptrdiff_t ENTITY_ID_OFFSET = 0x0C;         // u32 EntityId
    inline constexpr std::ptrdiff_t ENTITY_CLASS_OFFSET = 0x20;      // IEntityClass *
    inline constexpr std::ptrdiff_t ENTITY_CLASS_NAME_OFFSET = 0x10; // IEntityClass -> const char *name
    // CEntity flag word; ScriptBind IsHidden (handler sub_1837D9A60) answers (flags & 0x20) != 0.
    // CEntity entity links (ScriptBind GetLink, 0x1804D22AC): singly linked list at +0xD0 of { const char *name;
    // EntityId target; ...; next at +0x18 }.
    inline constexpr std::ptrdiff_t ENTITY_LINKS_OFFSET = 0xD0;
    inline constexpr std::ptrdiff_t ENTITY_LINK_NAME_OFFSET = 0x00;
    inline constexpr std::ptrdiff_t ENTITY_LINK_TARGET_OFFSET = 0x08;
    inline constexpr std::ptrdiff_t ENTITY_LINK_NEXT_OFFSET = 0x18;
    inline constexpr std::ptrdiff_t ENTITY_FLAGS_OFFSET = 0x08;
    inline constexpr std::uint32_t ENTITY_FLAG_HIDDEN = 0x20;
    // Invisible (IEntity slot 63 Invisible(bool) sub_18087963C sets it, slot 64 IsInvisible sub_1804B5260 reads it):
    // the game sets it, apart from Hidden, on the NPCs and items the camera cannot see (inside a building while the
    // player stands outside), and the renderer then skips the entity whatever its render flags.
    inline constexpr std::uint32_t ENTITY_INVISIBLE_FLAG = 0x10;
    // Active (IEntity slot 53 IsActive): the game deactivates the entities it makes invisible that way.
    inline constexpr std::uint32_t ENTITY_ACTIVE_FLAG = 0x01;
    // CEntity::GetWorldBounds(this, AABB *out), vtable slot 34. An empty entity returns min > max.
    inline constexpr std::ptrdiff_t ENTITY_VTABLE_GET_WORLD_BOUNDS_OFFSET = 34 * 8;

    // Places: the prefab template library (wh::framework::C_PrefabTemplateLibrary) the game fills from every
    // Prefabs/*.xml at startup and keeps for the session. The global context's first member is the library's owner,
    // which holds the library at +0xD0 (stored by sub_1807D0AE0). The library is an MSVC unordered_map from template
    // GUID to entry (built by sub_1807D0E84): max load factor (float 1.0) at +0, list sentinel at +0x08, element count
    // at +0x10; a list node is { next, prev, 16-byte GUID key at +0x10, entry * at +0x20 }.
    inline constexpr std::ptrdiff_t CONTEXT_PREFAB_OWNER_OFFSET = 0x00;
    inline constexpr std::ptrdiff_t PREFAB_OWNER_LIBRARY_OFFSET = 0xD0;
    inline constexpr std::ptrdiff_t PREFAB_LIBRARY_LOAD_FACTOR_OFFSET = 0x00;
    inline constexpr std::ptrdiff_t PREFAB_LIBRARY_LIST_OFFSET = 0x08;
    inline constexpr std::ptrdiff_t PREFAB_LIBRARY_COUNT_OFFSET = 0x10;
    inline constexpr std::ptrdiff_t PREFAB_NODE_NEXT_OFFSET = 0x00;
    inline constexpr std::ptrdiff_t PREFAB_NODE_ENTRY_OFFSET = 0x20;
    // Template entry, 40 bytes (sub_1807EAA64, sub_1807EAC40): GUID at +0, the name (CryString char *: the file path
    // without "Prefabs/" and ".xml", lower case, '/' separators) at +0x10, the parsed <Objects> node at +0x18.
    inline constexpr std::ptrdiff_t PREFAB_ENTRY_NAME_OFFSET = 0x10;
    inline constexpr std::ptrdiff_t PREFAB_ENTRY_OBJECTS_OFFSET = 0x18;
    // CXmlNode (vtable slots 6 getTag, 9 getNumAttributes, 10 getAttributeByIndex, 44 getChildCount, 45 getChild): the
    // tag char * at +0x18; +0x30 points at the std::vector<XmlNodeRef> of children (8-byte node pointers) and +0x38 at
    // the std::vector of 16-byte { const char *key; const char *value } attributes; either pointer is null when empty.
    inline constexpr std::ptrdiff_t XML_NODE_TAG_OFFSET = 0x18;
    inline constexpr std::ptrdiff_t XML_NODE_CHILDREN_OFFSET = 0x30;
    inline constexpr std::ptrdiff_t XML_NODE_ATTRIBUTES_OFFSET = 0x38;
    inline constexpr std::size_t XML_CHILD_SIZE = 0x08;
    inline constexpr std::size_t XML_ATTRIBUTE_SIZE = 0x10;
    // CXMLReadOnlyNode (same vtable slots): the same tag char * at +0x18, but its lists are inline arrays with u32
    // counts: the 16-byte { key, value } attributes at +0x20 (count at +0x28) and the 8-byte child node pointers at
    // +0x30 (count at +0x38).
    inline constexpr std::ptrdiff_t XML_READ_ONLY_NODE_ATTRIBUTES_OFFSET = 0x20;
    inline constexpr std::ptrdiff_t XML_READ_ONLY_NODE_ATTRIBUTE_COUNT_OFFSET = 0x28;
    inline constexpr std::ptrdiff_t XML_READ_ONLY_NODE_CHILDREN_OFFSET = 0x30;
    inline constexpr std::ptrdiff_t XML_READ_ONLY_NODE_CHILD_COUNT_OFFSET = 0x38;
    // CBrush, COwnedBrush and CMovableBrush world Matrix34 m_Matrix at +0x50 (CBrush::PhysicalizeOnHeap sub_180530A3C
    // reads its translation at +0x5C / +0x6C / +0x7C). COwnedBrush keeps its owner CEntity * at +0x100 (vtable slots
    // 66 / 67 SetOwnerEntity / GetOwnerEntity); a runtime prefab's brushes are owned by the RuntimePrefab entity.
    inline constexpr std::ptrdiff_t BRUSH_MATRIX_OFFSET = 0x50;
    inline constexpr std::ptrdiff_t OWNED_BRUSH_OWNER_OFFSET = 0x100;
    // ScriptAnyValue type of a table. Reading one into an empty value creates an IScriptTable holding one reference,
    // which IScriptTable::Release (vtable slot 4) drops.
    inline constexpr std::int32_t SCRIPT_ANY_TABLE = 6;
    inline constexpr std::ptrdiff_t SCRIPT_TABLE_VTABLE_RELEASE_OFFSET = 4 * 8;

    // IEntitySystem virtual slots. GetEntity(this, EntityId) -> IEntity*; GetEntityIterator(this) -> IEntityIt*.
    inline constexpr std::ptrdiff_t ENTITY_SYSTEM_VTABLE_GET_ENTITY_OFFSET = 14 * 8;
    inline constexpr std::ptrdiff_t ENTITY_SYSTEM_VTABLE_GET_ITERATOR_OFFSET = 22 * 8;
    // IEntityIt virtual slots: AddRef, Release (deletes at zero), IsEnd, Next (returns the entity and advances).
    inline constexpr std::ptrdiff_t ENTITY_IT_VTABLE_ADD_REF_OFFSET = 1 * 8;
    inline constexpr std::ptrdiff_t ENTITY_IT_VTABLE_RELEASE_OFFSET = 2 * 8;
    inline constexpr std::ptrdiff_t ENTITY_IT_VTABLE_IS_END_OFFSET = 3 * 8;
    inline constexpr std::ptrdiff_t ENTITY_IT_VTABLE_NEXT_OFFSET = 4 * 8;
    // CEntitySystem entity salt buffer, the list CEntityItMap walks (MoveFirst sub_180AE2CC4, Next sub_1804B4E80;
    // 1.5.6). Slot i of 24 bytes at +0x200 + 24 * i holds { u16 salt, ..., u32 next used slot at +0x08, CEntity * at
    // +0x10 }. The used slots form a singly linked list whose head is the next field of the sentinel slot 0x3FFFD; a
    // next outside [1, 0x3FFFC] ends it, and a slot with no entity is skipped. CEntitySystem::DeleteEntity
    // (sub_1820FA1E0) clears the slot's entity before it frees the slot, so the direct walk reads the slots in order
    // and keeps those holding an entity; following the list costs one dependent cache miss per entity.
    inline constexpr std::ptrdiff_t ENTITY_SYSTEM_SALT_BUFFER_OFFSET = 0x200;
    inline constexpr std::size_t SALT_SLOT_STRIDE = 24;
    inline constexpr std::ptrdiff_t SALT_SLOT_SALT_OFFSET = 0x00;
    inline constexpr std::ptrdiff_t SALT_SLOT_NEXT_OFFSET = 0x08;
    inline constexpr std::ptrdiff_t SALT_SLOT_ENTITY_OFFSET = 0x10;
    inline constexpr std::uint32_t SALT_LAST_SLOT = 0x3FFFC;
    inline constexpr std::uint32_t SALT_SENTINEL_SLOT = 0x3FFFD;
    // An EntityId is its slot | salt << 18 (the id allocator sub_1810B02BC; DeleteEntity splits it the same way).
    inline constexpr std::uint32_t SALT_ID_SLOT_MASK = 0x3FFFF;
    inline constexpr int SALT_ID_SALT_SHIFT = 18;
    inline constexpr std::ptrdiff_t ENTITY_SYSTEM_SALT_HEAD_OFFSET =
        ENTITY_SYSTEM_SALT_BUFFER_OFFSET + SALT_SENTINEL_SLOT * SALT_SLOT_STRIDE + SALT_SLOT_NEXT_OFFSET;
    static_assert(ENTITY_SYSTEM_SALT_HEAD_OFFSET == 0x6001C0, "MoveFirst reads the list head at +0x6001C0.");
    // The salt buffer's bookkeeping counters, right after the list head (1.5.6). Every push onto the used list, always
    // at its head, increments one of three u64 counters: the two id allocators (sub_1810B0224 +0x6001C8,
    // sub_1810B0354 +0x6001D0) and the insert of a known handle (sub_1803D1DA4 +0x6001D8). Every free (sub_18121DB2C,
    // called by DeleteEntity) adds to +0x6001E0. Only sub_1823D5148 writes them otherwise (it zeroes all four). So the
    // entities added since the counters read N inserts ago are among the first N nodes of the used list.
    inline constexpr std::ptrdiff_t ENTITY_SYSTEM_SALT_COUNTERS_OFFSET = ENTITY_SYSTEM_SALT_HEAD_OFFSET + 0x08;
    inline constexpr std::size_t SALT_INSERT_COUNTERS = 3;
    static_assert(ENTITY_SYSTEM_SALT_COUNTERS_OFFSET == 0x6001C8, "The insert counters start at +0x6001C8.");

    // I3DEngine virtual slots used by the ERF_RENDER_ALWAYS recipe. RegisterEntity takes the fast octree path only
    // while the node still owns an octree node, so the node is unregistered first; with the bit set, the full path
    // then files it in the always-visible list, which skips the per-object occlusion test.
    inline constexpr std::ptrdiff_t ENGINE_3D_VTABLE_REGISTER_ENTITY_OFFSET = 38 * 8;
    inline constexpr std::ptrdiff_t ENGINE_3D_VTABLE_UNREGISTER_ENTITY_DIRECT_OFFSET = 46 * 8;
    // I3DEngine post-effect parameters, the calls behind System.SetScreenFx / GetPostProcessFxParam (handlers
    // sub_1839ABD08 / sub_1839A88D8): SetPostEffectParamVec4(name, const Vec4 &, bool force), SetPostEffectParamString
    // (name, const char *), GetPostEffectParamVec4(name, Vec4 &). A forced value holds until the next forced set.
    inline constexpr std::ptrdiff_t ENGINE_3D_VTABLE_SET_POST_EFFECT_VEC4_OFFSET = 0x560;
    inline constexpr std::ptrdiff_t ENGINE_3D_VTABLE_SET_POST_EFFECT_STRING_OFFSET = 0x568;
    inline constexpr std::ptrdiff_t ENGINE_3D_VTABLE_GET_POST_EFFECT_VEC4_OFFSET = 0x578;
    // The game's VisualArtifacts effect (UberGamePostProcess, sub_182560BCC; its bane and berserker potions use it):
    // screen * ColorTint, lerped with the screen by the mask texture's red channel, runs while the tint is not white.
    inline constexpr const char *POST_EFFECT_VISUAL_ARTIFACTS_MASK = "tex_VisualArtifacts_Mask";
    inline constexpr const char *POST_EFFECT_VISUAL_ARTIFACTS_TINT = "clr_VisualArtifacts_ColorTint";
    // The engine's white texture: as the mask it blends the effect everywhere, as with no mask.
    inline constexpr const char *ENGINE_WHITE_TEXTURE = "%ENGINE%/EngineAssets/Textures/White.dds";
    // IRenderNode octree node pointer (null while the node is not registered in the octree).
    inline constexpr std::ptrdiff_t RENDERNODE_OCTREE_NODE_OFFSET = 0x20;
    // A node carrying this flag is never registered by the 3D engine, so unregistering it would lose it.
    inline constexpr std::uint64_t ERF_NO_3DENGINE_REGISTRATION = 1ull << 39;

    // Herbs. A pickable herb is not an entity: it is an instance of a merged-mesh vegetation group (grass-like patches
    // batched per 16 m cell) whose vegetation group carries the pickable flag. The PickableArea entity is only a proxy
    // the interaction probe activates on the herb it looks at.
    // I3DEngine::GetObjectsInBox(engine, const AABB* box /*6 floats*/, IRenderNode** out) -> uint32 count; a null out
    // returns the count only, otherwise the whole list is copied with no size cap.
    // C3DEngine vtable slots (1.5.6 vtable 0x1840205E8): I3DEngine declares GetObjectsByTypeInBox(engine, EERType,
    // const AABB*, IRenderNode** out, u64 rndFlagsMask) right before GetObjectsInBox, so while slot 243 still holds the
    // GetObjectsInBox anchor, slot 242 is the typed query. It skips every octree branch and object list that holds no
    // node of the type (the brushes and ~230k vegetation instances for a merged-mesh query); a mask of ~0 keeps nodes
    // whatever their flags.
    inline constexpr std::ptrdiff_t ENGINE3D_VTABLE_GET_OBJECTS_BY_TYPE_IN_BOX_OFFSET = 242 * 8;
    inline constexpr std::ptrdiff_t ENGINE3D_VTABLE_GET_OBJECTS_IN_BOX_OFFSET = 243 * 8;
    inline constexpr std::uint64_t ENGINE3D_QUERY_ANY_RNDFLAGS = ~0ull;
    // CObjManager vegetation group table: std::vector at +0x10 (begin) / +0x18 (end) of 0x1A0-byte groups; the group's
    // CStatObj* is at +0x0. The pickable flag is ERF_PICKABLE (bit 15) of the group's u64 render-node flags at +0x198,
    // i.e. bit 0x80 of the byte at +0x199: StatInstGroup::Update (0x180B1859C) sets it from the group's bPickable bool
    // at +0x10A, which the level's vegetation table sets per group (the game's own pick loop tests +0x10A). Every
    // level flags 29 groups: 18 merged-mesh plants and 11 mushroom groups drawn as CVegetation nodes.
    inline constexpr std::ptrdiff_t OBJMAN_VEG_GROUPS_BEGIN_OFFSET = 0x10;
    inline constexpr std::ptrdiff_t OBJMAN_VEG_GROUPS_END_OFFSET = 0x18;
    inline constexpr std::size_t VEG_GROUP_STRIDE = 0x1A0;
    inline constexpr std::ptrdiff_t VEG_GROUP_FLAGS_OFFSET = 0x199;
    inline constexpr std::uint8_t VEG_GROUP_FLAG_PICKABLE = 0x80;
    // u8 bAutoMerged: 1 for a group the engine batches into merged-mesh cells, 0 for one it places as CVegetation
    // nodes (the mushrooms).
    inline constexpr std::ptrdiff_t VEG_GROUP_AUTO_MERGED_OFFSET = 0x13A;
    // CStatObj model path (char *), for the log line naming each pickable group.
    inline constexpr std::ptrdiff_t STATOBJ_PATH_OFFSET = 0xA0;
    // CMergedMeshRenderNode (primary base IRenderNode). Instance positions are 16-bit fractions of the cell:
    // world = internalAABB.min + u16 / 65535 * (internalAABB.max.x - internalAABB.min.x), rotated about m_pos by
    // m_zRotation. KCD2 cells are 16 m wide.
    inline constexpr std::ptrdiff_t MERGED_MESH_AABB_MIN_OFFSET = 0x8C;
    inline constexpr std::ptrdiff_t MERGED_MESH_AABB_MAX_OFFSET = 0x98;
    // m_visibleAABB, what IRenderNode::GetBBox returns (0x1806597A4) and the octree's box test compares.
    inline constexpr std::ptrdiff_t MERGED_MESH_VISIBLE_AABB_MIN_OFFSET = 0xA4;
    inline constexpr std::ptrdiff_t MERGED_MESH_VISIBLE_AABB_MAX_OFFSET = 0xB0;
    inline constexpr std::ptrdiff_t MERGED_MESH_POS_OFFSET = 0xBC;
    inline constexpr std::ptrdiff_t MERGED_MESH_ZROTATION_OFFSET = 0xD4;
    inline constexpr std::ptrdiff_t MERGED_MESH_GROUPS_OFFSET = 0xF0;      // SMMRMGroupHeader *
    inline constexpr std::ptrdiff_t MERGED_MESH_GROUP_COUNT_OFFSET = 0xF8; // u32
    // u32 m_State. The game picks herbs only from a cell in STREAMED_IN (the pick iterator 0x18043C258 bails
    // otherwise); in any other state the cell is being built or streamed and its instances may not be drawn.
    inline constexpr std::ptrdiff_t MERGED_MESH_STATE_OFFSET = 0x6C;
    inline constexpr std::uint32_t MERGED_MESH_STATE_STREAMED_IN = 5;
    // SMMRMGroupHeader, 0x80 bytes: instance array, vegetation group index, sample count.
    inline constexpr std::size_t MERGED_MESH_GROUP_STRIDE = 0x80;
    inline constexpr std::ptrdiff_t MERGED_MESH_GROUP_INSTANCES_OFFSET = 0x00;
    inline constexpr std::ptrdiff_t MERGED_MESH_GROUP_VEG_INDEX_OFFSET = 0x4C;
    inline constexpr std::ptrdiff_t MERGED_MESH_GROUP_SAMPLE_COUNT_OFFSET = 0x54;
    // SMMRMInstance, 16 bytes: u16 x, y, z at 0/2/4 and the scale byte at 12. A pick zeroes the scale of every plant of
    // the same pickable kind within its radius, max(0.05, 0.4 + 0.04 * Survival) m; a respawn restores it from byte 14
    // (the original scale). So scale 0 means picked.
    inline constexpr std::size_t MERGED_MESH_INSTANCE_SIZE = 16;
    inline constexpr std::size_t MERGED_MESH_INSTANCE_SCALE_OFFSET = 12;
    // int8 quaternion x, y, z, w (DecompressQuat: each / 128, then normalized); the scale byte is / 64
    // (VEGETATION_CONV_FACTOR).
    inline constexpr std::size_t MERGED_MESH_INSTANCE_QUAT_OFFSET = 6;
    inline constexpr float VEGETATION_CONV_FACTOR = 64.0f;
    // Vegetation group entry: CStatObj * at +0.
    inline constexpr std::ptrdiff_t VEG_GROUP_STAT_OBJ_OFFSET = 0x00;
    // CMergedMeshesManager: the engine's own index of every merged-mesh cell, m_Nodes[32][32][2] of 24-byte
    // std::vector<CMergedMeshRenderNode *> ({begin, end, capacity}) at +0x08. FindNode (vtable slot 6, 0x1834ECE00)
    // files a cell under ((int)(|z| / 16) & 1) + 2 * (32 * ((int)(|x| / 16) & 31) + ((int)(|y| / 16) & 31)), so the
    // buckets wrap every 512 m and hold the cells of the whole streamed area. The manager is a 3D-engine static
    // stored next to the CObjManager slot by the same init function (0x180A66278); it is found by RTTI in a window
    // of MERGED_MESHES_MANAGER_SLOT_WINDOW bytes on each side of that slot.
    inline constexpr std::ptrdiff_t MERGED_MESHES_MANAGER_BUCKETS_OFFSET = 0x08;
    inline constexpr std::size_t MERGED_MESHES_MANAGER_BUCKET_STRIDE = 24;
    inline constexpr std::uint32_t MERGED_MESHES_HASH_DIM_XY = 32;
    inline constexpr std::uint32_t MERGED_MESHES_HASH_DIM_Z = 2;
    inline constexpr float MERGED_MESHES_HASH_CELL_SIZE = 16.0f;
    inline constexpr std::ptrdiff_t MERGED_MESHES_MANAGER_SLOT_WINDOW = 0x200;
    // CVegetation (a vegetation instance the engine does not merge: the pickable mushrooms). Vec3 m_vPos at +0x50,
    // the vegetation group index (int) at +0x78. Vtable slot 22 is GetEntityStatObj(this, u32 part, Matrix34 *out)
    // (0x1804323B4): it writes the instance's world matrix (unaligned stores) and returns the group's CStatObj.
    inline constexpr std::ptrdiff_t VEGETATION_POSITION_OFFSET = 0x50;
    inline constexpr std::ptrdiff_t VEGETATION_GROUP_INDEX_OFFSET = 0x78;
    inline constexpr std::ptrdiff_t VEGETATION_VTABLE_GET_ENTITY_STAT_OBJ_OFFSET = 22 * 8;
    // Plants of one species closer than this to a cluster's first plant are one pick, so they share one marker.
    inline constexpr float HERB_CLUSTER_RADIUS = 1.0f;
    // Height of the marker box above the plants' base, meters.
    inline constexpr float HERB_MARKER_HEIGHT = 0.5f;
    // Horizontal padding of the marker box around the cluster's plants, meters.
    inline constexpr float HERB_MARKER_PADDING = 0.2f;
    // [Render] SeeThrough = false: how far a herb's silhouette draw is pulled toward the camera, metres. The draw is
    // rebuilt from quantized instance data without the wind bending, so at its true depth the herb's own scene copy
    // would hide it in patches.
    inline constexpr float HERB_DEPTH_PULL = 0.25f;
    // Lower bound of the pull's scale about the camera, for a herb right in front of it.
    inline constexpr float HERB_DEPTH_PULL_MIN_SCALE = 0.5f;
    // The alpha of a herb's silhouette draw: above 0, at which the engine drops the draw before it reaches any render
    // list, silhouette included. Scene passes ignore object alpha, so the draw also lands in the depth pre-pass; an
    // item whose render object carries this alpha and a silhouette word is taken back out of it before it is drawn.
    inline constexpr float HERB_PROXY_ALPHA = 0.01f;

    // The mod's composite shader (shaders/HenrySensesSilhouette.cfx, embedded as a tokenized .cfxb). The game loads
    // tokenized shaders only, from ShadersBin.pak and then from the user shader cache on disk (0x1809182A8), where the
    // mod writes it; the game compiles its permutation at run time.
    // Its techniques: the composite, and the focus coverage (the highlight's pixels, outline ring included).
    inline constexpr const char *SILHOUETTE_TECHNIQUE = "HenrySensesSilhouette";
    inline constexpr const char *SILHOUETTE_COVERAGE_TECHNIQUE = "HenrySensesSilhouetteCoverage";
    // The user shader cache the binary is written to (<shader name>.cfxb), and the file-name prefix of its versions.
    inline constexpr const char *SILHOUETTE_SHADER_CACHE_DIR = "%USER%/Shaders/Cache/D3D12/";
    inline constexpr std::string_view SILHOUETTE_SHADER_FILE_PREFIX = "henrysensessilhouette";
    // CD3D9Renderer m_cEF, the CShaderMan (IRenderer::EF_LoadShader, vtable slot 148, adds this before it calls
    // mfForName), whose m_ShadersPath (char *) at +0x1F8 reads SHADER_MAN_SHADERS_PATH.
    inline constexpr std::ptrdiff_t RENDERER_SHADER_MAN_OFFSET = 0xF00;
    inline constexpr std::ptrdiff_t SHADER_MAN_SHADERS_PATH_OFFSET = 0x1F8;
    inline constexpr std::string_view SHADER_MAN_SHADERS_PATH = "Shaders/HWScripts/";
    // mfForName flags: EF_SYSTEM, as the engine loads its system shaders (0x180A5C648).
    inline constexpr std::uint32_t SHADER_LOAD_FLAG_SYSTEM = 0x20000000;
    // CShader m_Flags (+148): EF_NOTFOUND once the parse found no binary.
    inline constexpr std::ptrdiff_t SHADER_FLAGS_OFFSET = 0x94;
    inline constexpr std::uint32_t SHADER_FLAG_NOT_FOUND = 0x10000;
    // ICryPak::AdjustFileName(src, char dst[CRY_PAK_MAX_PATH], flags) -> dst, CCryPak vtable slot 1: resolves an
    // alias such as %USER% to the real path.
    inline constexpr std::ptrdiff_t CRY_PAK_VTABLE_ADJUST_FILE_NAME_OFFSET = 1 * 8;
    inline constexpr std::size_t CRY_PAK_MAX_PATH = 0x800;
    inline constexpr std::uint32_t CRY_PAK_FLAGS_PATH_REAL = 0x10000;
    inline constexpr std::uint32_t CRY_PAK_FLAGS_FOR_WRITING = 0x800000;
    // Corruption guards on one herb query: octree nodes returned for the box, groups per cell, samples per group, cells
    // per manager bucket, pickable vegetation nodes. Each sits far above any real count, since a real count past one
    // would drop herbs inside the radius (a cell cannot hold more groups than the level's ~600).
    inline constexpr std::uint32_t HERB_MAX_OCTREE_NODES = 65536;
    inline constexpr std::uint32_t HERB_MAX_GROUPS_PER_CELL = 1024;
    inline constexpr std::uint32_t HERB_MAX_SAMPLES_PER_GROUP = 65536;
    inline constexpr std::uint32_t HERB_MAX_CELLS_PER_BUCKET = 1024;
    inline constexpr std::uint32_t HERB_MAX_VEGETATION_NODES = 65536;

    // Renderer and aux geometry. GetIRenderAuxGeom returns the calling thread's own command buffer.
    inline constexpr std::ptrdiff_t RENDERER_VTABLE_GET_AUX_GEOM_OFFSET = 199 * 8;
    inline constexpr std::ptrdiff_t AUX_VTABLE_SET_RENDER_FLAGS_OFFSET = 1 * 8;
    inline constexpr std::ptrdiff_t AUX_VTABLE_GET_RENDER_FLAGS_OFFSET = 2 * 8;
    inline constexpr std::ptrdiff_t AUX_VTABLE_DRAW_LINES_OFFSET = 13 * 8;
    // SAuxGeomRenderFlags for markers: 3D mode, alpha blended, no culling, no depth write, no depth test.
    inline constexpr std::uint32_t AUX_MARKER_RENDER_FLAGS = 0x20600000;
    // e_DepthTestOff (bit 21) of those flags; cleared for [Render] SeeThrough = false.
    inline constexpr std::uint32_t AUX_DEPTH_TEST_OFF = 0x00200000;

    // SRenderingPassInfo bytes: a non-zero recursion level is a mirror / recursive pass, a non-zero shadow byte a
    // shadow-map pass. Only the general pass receives the silhouette word.
    inline constexpr std::ptrdiff_t PASS_INFO_RECURSION_OFFSET = 0x01;
    inline constexpr std::ptrdiff_t PASS_INFO_SHADOW_OFFSET = 0x02;
    // i32 frame id (CStatObj::RenderInternal stores it as the statobj's last rendered frame).
    inline constexpr std::ptrdiff_t PASS_INFO_FRAME_ID_OFFSET = 0x0C;
    // const CCamera * of the pass. The camera's world position is the translation column of its Matrix34 at +0
    // (+0x0C, +0x1C, +0x2C), as the octree brush render reads it (0x1806BA94C).
    inline constexpr std::ptrdiff_t PASS_INFO_CAMERA_OFFSET = 0x18;
    inline constexpr std::ptrdiff_t CAMERA_POSITION_X_OFFSET = 0x0C;
    inline constexpr std::ptrdiff_t CAMERA_POSITION_Y_OFFSET = 0x1C;
    inline constexpr std::ptrdiff_t CAMERA_POSITION_Z_OFFSET = 0x2C;

    // CStandardGraphicsPipeline members.
    inline constexpr std::ptrdiff_t PIPELINE_FORWARD_STAGE_OFFSET = 0x48;  // CSceneForwardStage *
    inline constexpr std::ptrdiff_t PIPELINE_CUSTOM_STAGE_OFFSET = 0x50;   // CSceneCustomStage * (null as shipped)
    inline constexpr std::ptrdiff_t PIPELINE_RESOURCES_OFFSET = 0x840;     // CGraphicsPipelineResources (inline)
    inline constexpr std::ptrdiff_t PIPELINE_CURRENT_VIEW_OFFSET = 0x1978; // CRenderView * being executed
    inline constexpr std::ptrdiff_t PIPELINE_FLAGS_OFFSET = 0x1980; // u64 execution flags, restored around the mask
    // Base CGraphicsStage members.
    inline constexpr std::ptrdiff_t STAGE_PIPELINE_OFFSET = 0x08;    // CGraphicsPipeline *
    inline constexpr std::ptrdiff_t STAGE_RESOURCES_OFFSET = 0x10;   // CGraphicsPipelineResources *
    inline constexpr std::ptrdiff_t STAGE_RENDER_VIEW_OFFSET = 0x28; // CRenderView *, set by the pipeline every view
    // Stage vtable slot 1 is Init.
    inline constexpr std::ptrdiff_t STAGE_VTABLE_INIT_OFFSET = 0x08;
    // CSceneCustomStage members.
    inline constexpr std::ptrdiff_t CUSTOM_STAGE_RESOURCE_LAYOUT_OFFSET = 0x110; // set by Init
    inline constexpr std::ptrdiff_t CUSTOM_STAGE_MASK_PASS_OFFSET = 0x540;       // CSceneRenderPass (silhouette mask)
    inline constexpr std::ptrdiff_t CUSTOM_STAGE_HIGHLIGHT_PASS_OFFSET = 0x750;  // CFullscreenPass (composite)
    // CFullscreenPass "Scene Custom Resolve" (stage constructor sub_18194E024), used only by the debug overdraw view;
    // the focus coverage pass draws with it.
    inline constexpr std::ptrdiff_t CUSTOM_STAGE_RESOLVE_PASS_OFFSET = 0xBB0;
    inline constexpr std::ptrdiff_t CUSTOM_STAGE_MASK_CLEAR_PASS_OFFSET = 0x1190; // CClearSurfacePass
    // CSceneRenderPass members.
    inline constexpr std::ptrdiff_t SCENE_PASS_DEVICE_PASS_OFFSET = 0x170; // CDeviceRenderPass *, valid after Init
    // D3D12_VIEWPORT m_viewPort[2] (regular, nearest; 6 floats each: TopLeftX, TopLeftY, Width, Height, MinDepth,
    // MaxDepth), set by the stage's Update every view and read by BeginRenderPass in each recording job.
    inline constexpr std::ptrdiff_t SCENE_PASS_VIEWPORTS_OFFSET = 0x180;
    inline constexpr std::size_t SCENE_PASS_VIEWPORT_COUNT = 2;
    inline constexpr std::size_t VIEWPORT_FLOATS = 6;
    // D3D12_RECT m_scissorRect (i32 left, top, right, bottom), which SetViewport derives from the first viewport and
    // BeginRenderPass binds with it.
    inline constexpr std::ptrdiff_t SCENE_PASS_SCISSOR_OFFSET = 0x1B0;
    // CDeviceRenderPassDesc m_renderPassDesc (+0x50): the CTexture * of colour target 0 (+0x40 in it) and of the depth
    // target (+0xB0), each followed by its view handle.
    inline constexpr std::ptrdiff_t SCENE_PASS_COLOR_TARGET_OFFSET = 0x90;
    inline constexpr std::ptrdiff_t SCENE_PASS_DEPTH_TARGET_OFFSET = 0x100;
    inline constexpr std::ptrdiff_t SCENE_PASS_TECHNIQUE_OFFSET = 0x1D8;   // u16 technique
    inline constexpr std::ptrdiff_t SCENE_PASS_STAGE_PASS_OFFSET = 0x1DC;  // u16 stage id, u16 pass id
    inline constexpr std::ptrdiff_t SCENE_PASS_FILTERS_OFFSET = 0x1E0;     // u32 include, u32 exclude batch filters
    inline constexpr std::ptrdiff_t SCENE_PASS_GROUP_COUNT_OFFSET = 0x1EC; // u32 render-item group count
    // CGraphicsPipelineResources: the silhouette mask is $SceneNormalsMap (RGBA8, render resolution); it is free
    // after HDR post-processing and the custom stage binds it as the mask pass target every frame.
    inline constexpr std::ptrdiff_t RESOURCES_SCENE_NORMALS_OFFSET = 0x08;
    // $ZTarget, the view's linear depth (R32F at render resolution). The composite reads it to keep the nearer of
    // two touching objects' outlines.
    inline constexpr std::ptrdiff_t RESOURCES_LINEAR_DEPTH_OFFSET = 0x28;
    // CTexture members.
    inline constexpr std::ptrdiff_t TEXTURE_ID_OFFSET = 0x60; // u32
    // const char *: the lower-case name the engine finds the texture by.
    inline constexpr std::ptrdiff_t TEXTURE_NAME_OFFSET = 0x70;
    inline constexpr std::ptrdiff_t TEXTURE_DEVICE_OFFSET = 0x80; // device texture, null until created
    // $SceneDiffuseTmp: a render-resolution RGBA8 scratch target of the deferred shading, free once post-processing
    // ran; the mask blur's intermediate.
    inline constexpr std::ptrdiff_t RESOURCES_SCENE_DIFFUSE_TMP_OFFSET = 0xC0;
    // CGaussianBlurPass: 0xBD8 bytes, vtable slot 0 the scalar deleting destructor (flag 0: destroy, do not free).
    inline constexpr std::size_t BLUR_PASS_SIZE = 0xBD8;
    inline constexpr std::ptrdiff_t TEXTURE_WIDTH_OFFSET = 0x90;  // u16
    inline constexpr std::ptrdiff_t TEXTURE_HEIGHT_OFFSET = 0x92; // u16
    // u32 creation flags and u8 ETEX_Format. The supersampled mask's targets copy both from the normal mask targets,
    // so every silhouette PSO (compiled against those formats) draws into them unchanged.
    inline constexpr std::ptrdiff_t TEXTURE_FLAGS_OFFSET = 0x98;
    inline constexpr std::ptrdiff_t TEXTURE_FORMAT_OFFSET = 0xA4;
    // FT_DONT_RELEASE: Release ignores a texture with it, so the mod's own targets are created without it.
    inline constexpr std::uint32_t TEXTURE_FLAG_DONT_RELEASE = 0x10000;
    // CTexture primary vtable: slot 1 AddRef, slot 2 Release (0x180771960).
    inline constexpr std::ptrdiff_t TEXTURE_VTABLE_RELEASE_OFFSET = 2 * 8;
    // The supersampled mask: the silhouettes are rasterized into the mod's own target at up to
    // SUPERSAMPLED_MASK_SCALE times the output resolution on each axis, capped at SUPERSAMPLED_MASK_MAX_PIXELS (one 4K
    // frame), so the outline composited after tone-mapping is anti-aliased without the upscaler.
    inline constexpr float SUPERSAMPLED_MASK_SCALE = 1.5f;
    inline constexpr double SUPERSAMPLED_MASK_MAX_PIXELS = 3840.0 * 2160.0;
    inline constexpr std::uint32_t SUPERSAMPLED_MASK_MAX_SIDE = 16384;
    // The depth the supersampled mask's depth target is cleared to: the far plane of the reverse depth buffer (the mask
    // pass carries ePassFlags_ReverseDepth, live flags 0x2), which every depth-tested silhouette PSO passes.
    inline constexpr float SUPERSAMPLED_MASK_FAR_DEPTH = 0.0f;
    // CRenderView members.
    inline constexpr std::ptrdiff_t RENDER_VIEW_LISTS_OFFSET = 0x210; // render-item lists, one per EFSLIST
    inline constexpr std::ptrdiff_t RENDER_VIEW_LIST_STRIDE = 0x58;
    inline constexpr std::ptrdiff_t RENDER_VIEW_BATCH_FLAGS_OFFSET = 0xC08; // u32 combined batch flags per list
    inline constexpr std::ptrdiff_t RENDER_VIEW_DRAWER_OFFSET = 0xA728;     // CRenderItemDrawer
    // m_vProjMatrixSubPixoffset: the view's camera jitter in clip space (2 * jx / width, 2 * jy / height), written per
    // view by CPostAAStage::CalculateJitterOffsets (0x1807C1A7C, cold part 0x18223A7EE). The view's mask is drawn
    // with it; CSuperResolutionStage +0x78 holds the same jitter in pixels, but for the view prepared last.
    inline constexpr std::ptrdiff_t RENDER_VIEW_SUBPIXEL_OFFSET = 0xA720;
    inline constexpr std::ptrdiff_t DRAWER_PASSES_BEGIN_OFFSET = 0x18;
    inline constexpr std::ptrdiff_t DRAWER_PASSES_END_OFFSET = 0x20;
    // Render-item list: begin / end of 32-byte items, and a pending worker-chunk pointer (non-null while unmerged).
    // The pending chunk is another list of the same layout; the item count sums the chain (0x18045EF58).
    inline constexpr std::ptrdiff_t RENDER_LIST_BEGIN_OFFSET = 0x00;
    inline constexpr std::ptrdiff_t RENDER_LIST_END_OFFSET = 0x08;
    inline constexpr std::ptrdiff_t RENDER_LIST_PENDING_OFFSET = 0x50;
    inline constexpr std::uint32_t RENDER_LIST_ITEM_SHIFT = 5;
    // SRendItem (32 bytes): sort value, batch flags, the distance sort key at +0x08, the CCompiledRenderObject at
    // +0x10. EFSLIST_CUSTOM is sorted far to near by that key, but EncodeCustomDistanceSortingValue gives a node with
    // ERF_RENDER_ALWAYS lowest() + distance, which is lowest() in float: every highlighted object (all render-always)
    // gets the same key, so they draw in any order and, with the mask pass writing without a depth test, a farther
    // object can paint over a nearer one.
    inline constexpr std::ptrdiff_t RENDER_ITEM_COMPILED_OBJECT_OFFSET = 0x10;
    // CCompiledRenderObject: the render object of the current frame; CRenderObject m_fDistance is the object's
    // camera distance in metres, shared by all of its render items.
    inline constexpr std::ptrdiff_t COMPILED_OBJECT_RENDER_OBJECT_OFFSET = 0x118;
    inline constexpr std::ptrdiff_t RENDER_OBJECT_DISTANCE_OFFSET = 0x0C;
    // CCompiledRenderObject PSO array (+0x70): the per-material PSO builder (0x18071D8B4, called by
    // CCompiledRenderObject::Compile 0x180429534) fills the custom stage's passes from +0xE8 only while pipeline + 0x50
    // holds the stage, so the silhouette pass (4) is +0x108. Without the stage it still completes the object, and the
    // object keeps a null silhouette PSO until the engine rebuilds it.
    inline constexpr std::ptrdiff_t COMPILED_OBJECT_SILHOUETTE_PSO_OFFSET = 0x108;
    // CCompiledRenderObject dirty bytes, one per view kind (+1 for a shadow view): Compile rebuilds the pipeline states
    // when bit 0 is set and clears it once they are built. A character's parts compile every frame.
    inline constexpr std::ptrdiff_t COMPILED_OBJECT_DIRTY_OFFSET = 0x140;
    inline constexpr std::uint8_t COMPILED_OBJECT_DIRTY_PSO = 0x01;
    // CRenderView view type (u32); 2 is a shadow view, whose objects use the second dirty byte (0x180429B0C).
    inline constexpr std::ptrdiff_t RENDER_VIEW_TYPE_OFFSET = 0x14;
    inline constexpr std::uint32_t RENDER_VIEW_TYPE_SHADOW = 2;
    // CFullscreenPass members.
    inline constexpr std::ptrdiff_t FULLSCREEN_PRIMITIVE_OFFSET = 0x2D0; // CRenderPrimitive (SetTechnique target)
    // u8 m_bRequirePerViewCB. The stage's constructor leaves it 0, and without the per-view constant buffer the
    // DeferredSilhouettesOptimised PSO never becomes valid, so Execute draws nothing.
    inline constexpr std::ptrdiff_t FULLSCREEN_REQUIRE_PER_VIEW_CB_OFFSET = 0x2C0;
    // CPrimitiveRenderPass::Execute (0x180501240) returns 1 whenever the pass holds compiled primitives, and draws
    // only when its device render pass exists and is valid; it then either records into the core command list or,
    // while the pipeline's render-pass scheduler is active, queues the pass there.
    inline constexpr std::ptrdiff_t PRIMITIVE_PASS_DEVICE_PASS_OFFSET = 0x178;    // CDeviceRenderPass *
    inline constexpr std::ptrdiff_t DEVICE_RENDER_PASS_VALID_OFFSET = 0x28;       // u8 IsValid
    inline constexpr std::ptrdiff_t PRIMITIVE_PASS_COMPILED_BEGIN_OFFSET = 0x298; // compiled primitives vector
    inline constexpr std::ptrdiff_t PRIMITIVE_PASS_COMPILED_END_OFFSET = 0x2A0;
    inline constexpr std::ptrdiff_t PRIMITIVE_PASS_PIPELINE_OFFSET = 0x2B0;    // CGraphicsPipeline *
    inline constexpr std::ptrdiff_t PIPELINE_SCHEDULER_ENABLED_OFFSET = 0x1A0; // u8, CRenderPassScheduler
    inline constexpr std::ptrdiff_t PIPELINE_SCHEDULER_OWNER_OFFSET = 0x1A8;   // pointer read with the flag below
    inline constexpr std::ptrdiff_t SCHEDULER_OWNER_ACTIVE_OFFSET = 0x18F4;    // u32
    inline constexpr std::ptrdiff_t FULLSCREEN_DIRTY_OFFSET = 0x344;           // u32 dirty mask
    inline constexpr std::ptrdiff_t FULLSCREEN_RENDER_STATE_OFFSET = 0x348;    // u64 render state
    inline constexpr std::ptrdiff_t FULLSCREEN_PRIMITIVE_TYPE_OFFSET = 0x374;  // u32 primitive type
    inline constexpr std::ptrdiff_t FULLSCREEN_RESOURCE_DESC_OFFSET = 0x3B0;   // CDeviceResourceSetDesc
    inline constexpr std::uint32_t FULLSCREEN_DIRTY_RENDER_STATE = 0x08;
    inline constexpr std::uint32_t FULLSCREEN_DIRTY_PRIMITIVE_TYPE = 0x10;
    // Engine values the silhouette pass uses.
    inline constexpr std::uint32_t FB_CUSTOM_RENDER = 0x800; // batch flag of objects with a silhouette word
    inline constexpr std::uint8_t RENDER_LIST_CUSTOM = 27;   // EFSLIST_CUSTOM
    // EFSLIST_ZPREPASS: drawn depth-only before the G-buffer (CSceneGBufferStage::ExecuteDepthPrepass 0x1807C1B44
    // draws EFSLIST_ZPREPASS_NEAREST, then this, through CSceneRenderPass::DrawRenderItems).
    inline constexpr std::uint8_t RENDER_LIST_ZPREPASS = 25;
    inline constexpr std::uint16_t TECHNIQUE_CUSTOM_RENDER_PASS = 4;
    // SGraphicsPipelinePassContext stage id (u16 at SCENE_PASS_STAGE_PASS_OFFSET) and pass id (u16 after it).
    // CCompiledRenderObject::DrawToCommandList (0x180502790) draws m_pso[(max(stage, 1) - 1) * 5 + pass], m_pso at
    // +0x70. CreatePipelineStates stores the custom stage's PSOs at array slot 3 (+0xE8..+0x108), so the custom stage
    // id is 4; the forward stage (slot 2) is 3, which the engine's forward passes stamp (0x1807C2429, 0x18115A9AC).
    // The silhouette mask pass is pass 4.
    inline constexpr std::uint32_t CUSTOM_STAGE_PASS_SILHOUETTE = 0x00040004; // stage 4, pass 4
    inline constexpr std::uint8_t CUSTOM_PASS_SILHOUETTE = 4;
    inline constexpr std::uint64_t RENDER_STATE_ADDITIVE_NO_DEPTH = 0x802222;
    // Blend factors are D3D12_BLEND values, source colour and alpha in the low byte and destination in the next (the
    // uber pass lerps with 0x806655: SRC_ALPHA / INV_SRC_ALPHA). The focus coverage pass multiplies a white target by
    // one minus the composite's alpha: ZERO / INV_SRC_ALPHA, no depth test.
    inline constexpr std::uint64_t RENDER_STATE_COVERAGE_NO_DEPTH = 0x806611;
    inline constexpr std::uint32_t PRIMITIVE_TYPE_PROCEDURAL_TRIANGLE = 1;
    inline constexpr std::uint32_t DISPLAY_TARGET_STAGE = 29;
    inline constexpr std::uint8_t RESOURCE_VIEW_RENDER_TARGET = 2;
    inline constexpr std::uint16_t SHADER_STAGE_PIXEL = 2;
    inline constexpr std::uint16_t SAMPLER_LINEAR_CLAMP = 5;
    // Offset of the reference count in an engine device object (the PSO intrusive pointer).
    inline constexpr std::ptrdiff_t DEVICE_OBJECT_REFCOUNT_OFFSET = 0x0C;

    // RTTI type-descriptor names used to validate resolved objects and to key the self-heal landmarks.
    inline constexpr const char *C_PLAYER_RTTI_NAME = ".?AVC_Player@entitymodule@wh@@";
    inline constexpr const char *CACTIONGAME_RTTI_NAME = ".?AVCActionGame@@";
    inline constexpr const char *C_ENTITY_RTTI_NAME = ".?AVCEntity@@";
    inline constexpr const char *C_HIT_DEATH_REACTIONS_RTTI_NAME = ".?AVC_HitDeathReactions@entitymodule@wh@@";
    inline constexpr const char *C_RENDER_PROXY_RTTI_NAME = ".?AVCRenderProxy@@";
    inline constexpr const char *C_ENTITY_SYSTEM_RTTI_NAME = ".?AVCEntitySystem@@";
    inline constexpr const char *C_AUX_GEOM_RTTI_NAME = ".?AVCAuxGeomCB@@";
    inline constexpr const char *C_CAMERA_MANAGER_RTTI_NAME = ".?AVC_CameraManager@game@wh@@";
    inline constexpr const char *C_CAMERA_COMBAT_RTTI_NAME = ".?AVC_CameraCombatDelegate@game@wh@@";
    inline constexpr const char *C_CAMERA_DIALOG_RTTI_NAME = ".?AVC_CameraDialog@game@wh@@";
    inline constexpr const char *C_PLAYER_MODULE_RTTI_NAME = ".?AVC_PlayerModule@playermodule@wh@@";
    inline constexpr const char *C_3DENGINE_RTTI_NAME = ".?AVC3DEngine@@";
    inline constexpr const char *C_RENDERER_RTTI_NAME = ".?AVCD3D9Renderer@@";
    inline constexpr const char *C_STD_PIPELINE_RTTI_NAME = ".?AVCStandardGraphicsPipeline@@";
    inline constexpr const char *C_CUSTOM_STAGE_RTTI_NAME = ".?AVCSceneCustomStage@@";
    inline constexpr const char *C_FORWARD_STAGE_RTTI_NAME = ".?AVCSceneForwardStage@@";
    inline constexpr const char *C_MERGED_MESH_NODE_RTTI_NAME = ".?AVCMergedMeshRenderNode@@";
    inline constexpr const char *C_BRUSH_RTTI_NAME = ".?AVCBrush@@";
    inline constexpr const char *C_CHAR_INSTANCE_RTTI_NAME = ".?AVCCharInstance@@";
    // Brush subclasses: an entity-owned brush (runtime prefabs, procedural objects) and a movable one.
    inline constexpr const char *C_OWNED_BRUSH_RTTI_NAME = ".?AVCOwnedBrush@@";
    inline constexpr const char *C_MOVABLE_BRUSH_RTTI_NAME = ".?AVCMovableBrush@@";
    // Actors and the gameplay objects engine/game_natives.hpp reads.
    inline constexpr const char *C_NPC_ACTOR_RTTI_NAME = ".?AVC_NPCActor@entitymodule@wh@@";
    inline constexpr const char *C_HORSE_RTTI_NAME = ".?AVC_Horse@entitymodule@wh@@";
    inline constexpr const char *C_DOG_RTTI_NAME = ".?AVC_Dog@entitymodule@wh@@";
    inline constexpr const char *C_ANIMAL_RTTI_NAME = ".?AVC_Animal@entitymodule@wh@@";
    inline constexpr const char *C_AI_NPC_RTTI_NAME = ".?AVC_NPC@xgenaimodule@wh@@";
    inline constexpr const char *C_SOUL_RTTI_NAME = ".?AVC_Soul@rpgmodule@wh@@";
    inline constexpr const char *C_INVENTORY_RTTI_NAME = ".?AVC_Inventory@entitymodule@wh@@";
    inline constexpr const char *C_ITEM_SLOT_RTTI_NAME = ".?AVC_ItemSlot@entitymodule@wh@@";
    inline constexpr const char *C_ITEM_SLOT_PILE_RTTI_NAME = ".?AVC_ItemSlotPile@entitymodule@wh@@";
    inline constexpr const char *C_ITEM_VECTOR_BORROWER_RTTI_NAME = ".?AVC_ItemVectorBorrower@entitymodule@wh@@";
    inline constexpr const char *C_ITEM_WRAPPER_RTTI_NAME = ".?AVC_ItemWrapper@entitymodule@wh@@";
    inline constexpr const char *C_WORLD_INVENTORY_RTTI_NAME = ".?AVC_WorldInventory@entitymodule@wh@@";
    inline constexpr const char *C_PICKABLE_ITEM_RTTI_NAME = ".?AVC_PickableItem@entitymodule@wh@@";
    inline constexpr const char *C_ITEM_RTTI_NAME = ".?AVC_Item@entitymodule@wh@@";
    inline constexpr const char *C_ACTOR_SYSTEM_RTTI_NAME = ".?AVCActorSystem@@";
    inline constexpr const char *C_ITEM_SYSTEM_RTTI_NAME = ".?AVCItemSystem@@";
    inline constexpr const char *C_GAME_RTTI_NAME = ".?AVC_Game@game@wh@@";
    inline constexpr const char *C_GAME_MODEL_RTTI_NAME = ".?AVC_GameModel@game@wh@@";
    inline constexpr const char *C_SCRIPT_CONTEXT_MANAGER_RTTI_NAME = ".?AVC_ScriptContextManager@game@wh@@";
    inline constexpr const char *C_ENTITY_MODULE_RTTI_NAME = ".?AVC_EntityModule@entitymodule@wh@@";
    inline constexpr const char *C_INVENTORY_MANAGER_RTTI_NAME = ".?AVC_InventoryManager@entitymodule@wh@@";
    inline constexpr const char *C_SHOP_MODULE_RTTI_NAME = ".?AVC_ShopModule@shopmodule@wh@@";
    inline constexpr const char *C_SHOP_RTTI_NAME = ".?AVC_Shop@shopmodule@wh@@";
    inline constexpr const char *C_RPG_MODULE_RTTI_NAME = ".?AVC_RPGModule@rpgmodule@wh@@";
    inline constexpr const char *C_STASH_RTTI_NAME = ".?AVC_Stash@entitymodule@wh@@";
    // The engine's parsed XML nodes: the editable tree node, and the compact read-only node the prefab template library
    // keeps every template as.
    inline constexpr const char *C_XML_NODE_RTTI_NAME = ".?AVCXmlNode@@";
    inline constexpr const char *C_XML_READ_ONLY_NODE_RTTI_NAME = ".?AVCXMLReadOnlyNode@@";
    // Herbs: the merged-mesh cell index and an unmerged vegetation instance (the pickable mushrooms).
    inline constexpr const char *C_MERGED_MESHES_MANAGER_RTTI_NAME = ".?AVCMergedMeshesManager@@";
    inline constexpr const char *C_VEGETATION_RTTI_NAME = ".?AVCVegetation@@";
    inline constexpr const char *C_CRY_PAK_RTTI_NAME = ".?AVCCryPak@@";
    // Loot effects: the legacy (pfx1) emitter an entity slot holds.
    inline constexpr const char *C_PARTICLE_EMITTER_RTTI_NAME = ".?AVCParticleEmitter@@";
    inline constexpr const char *C_PARTICLE_MANAGER_RTTI_NAME = ".?AVCParticleManager@@";
    // The mod's particle library: the XML utilities that parse it and the node tree they build.
    inline constexpr const char *C_XML_UTILS_RTTI_NAME = ".?AVCXmlUtils@@";
} // namespace HenrySenses::constants

#endif // HENRYSENSES_CONSTANTS_HPP
