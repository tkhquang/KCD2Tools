/**
 * @file layout_offsets.hpp
 * @brief The game's memory layout the mod reads and writes: struct offsets, vtable slots, RTTI names, engine flags
 *        and the engine's own values, with where each was found.
 *
 * @details Engine facts, not tuning: a game patch can move them, and the self-heal and the signature cascades
 *          (offset_heal.hpp, aob_resolver.hpp) guard the ones that drift. Part of constants.hpp, which includes it;
 *          the mod's own limits and timings are in tuning.hpp.
 */
#ifndef TPVCAMERA_LAYOUT_OFFSETS_HPP
#define TPVCAMERA_LAYOUT_OFFSETS_HPP

#include "tuning.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Constants
{
    // All AOB signatures are defined as multi-candidate cascades in aob_resolver.hpp
    // (k_contextCandidates, k_frustumCandidates, ...). The global-context cascade
    // resolves the storage slot whose +0x38 reaches the camera-manager root the
    // game-state detection walks (see OFFSET_MANAGER_PTR_STORAGE); the frustum-builder
    // cascade resolves CCamera::UpdateFrustumPlanes, the matrix-offset hook target that
    // every gameplay camera funnels through (gated to the game view by the embedding
    // CView vtable, camera - SVIEWPARAMS_VIEWMATRIX_OFFSET).

    // RTTI type-descriptor name of the CView class. The frustum-builder detour confirms a
    // camera belongs to a game view by matching the embedding object's vtable against this
    // name (via DMK::rtti), then caches that vtable address for a fast per-camera qword
    // compare. Anchoring on the ASLR-invariant RTTI name rather than a hardcoded vtable
    // address keeps the game-view gate working across game patches.
    constexpr const char *CVIEW_RTTI_NAME = ".?AVCView@@";

    // Player look/aim orientation chain. Used to LEVEL the aim pitch while free-look orbit is active
    // so the character's head and the eye look forward (not just the camera). Chain:
    //   g_env base (AnchorId::Genv) -> p_game (g_env + GENV_PGAME_OFFSET)
    //   -> CCryAction = p_game->IGame::GetIGameFramework() (vtable slot 16 = IGAME_GET_FRAMEWORK_VTABLE_OFFSET)
    //   -> p_action_game (CCryAction + CCRYACTION_ACTIONGAME_OFFSET)
    //   -> C_Player    (p_action_game + CACTIONGAME_LOCAL_ACTOR_OFFSET), validated by its RTTI type name
    //   -> look controller (C_Player + C_PLAYER_LOOK_CONTROLLER_OFFSET)
    //   -> scalar look PITCH (controller + LOOK_CONTROLLER_PITCH_OFFSET, radians, 0 = level), with a
    //      synchronized copy at LOOK_CONTROLLER_PITCH2_OFFSET; yaw lives alongside and is left alone.
    // The look quaternion the cameras read (controller + 0x24) is DERIVED from this scalar pitch+yaw
    // EVERY frame, so writing that quat is overwritten - the SCALAR pitch is what must be written to
    // level the aim. Both copies are written so any internal current/target smoothing also settles at level.
    // The g_env (SSystemGlobalEnvironment) base is resolved patch-resiliently at startup from
    // `lea/mov [rip+g_env]` reference sites by the AnchorId::Genv quorum (aob_resolver.hpp).
    // RTTI type-descriptor name of C_Player, used to validate the resolved actor (an RTTI match, unlike a fixed
    // vtable address, survives patches).
    constexpr const char *C_PLAYER_RTTI_NAME = ".?AVC_Player@entitymodule@wh@@";
    constexpr ptrdiff_t GENV_PGAME_OFFSET = 0x90;
    constexpr ptrdiff_t IGAME_GET_FRAMEWORK_VTABLE_OFFSET = 0x80; // IGame vtable slot 16
    constexpr ptrdiff_t CCRYACTION_ACTIONGAME_OFFSET = 0x88;
    // RTTI type-descriptor name of CActionGame, the self-heal anchor for CCRYACTION_ACTIONGAME_OFFSET.
    constexpr const char *CACTIONGAME_RTTI_NAME = ".?AVCActionGame@@";
    constexpr ptrdiff_t CACTIONGAME_LOCAL_ACTOR_OFFSET = 0xA40;

    // Cutscene gate (game_state.cpp). A TrackView sequence flagged as a cutscene renders through its own CView, which
    // the frustum detour cannot tell from the player's, so the camera stays in first person while one plays.
    // CCryAction's IGameFramework::GetIViewSystem is vtable slot 29 and CViewSystem's IViewSystem::IsPlayingCutScene
    // is slot 19. Each slot target is checked against Aob::k_frameworkGetViewSystem /
    // Aob::k_viewSystemIsPlayingCutScene (the same on Steam 1.5.6, GOG 1.5 and Game Pass 1.4), and the two member
    // offsets are read from those instructions rather than kept here.
    constexpr const char *CVIEW_SYSTEM_RTTI_NAME = ".?AVCViewSystem@@";
    constexpr std::size_t CRYACTION_VTABLE_GET_VIEW_SYSTEM_SLOT = 29;
    constexpr std::size_t VIEW_SYSTEM_VTABLE_IS_PLAYING_CUTSCENE_SLOT = 19;
    constexpr ptrdiff_t C_PLAYER_LOOK_CONTROLLER_OFFSET = 0x238;
    // The look-controller pointee is a non-polymorphic struct with no RTTI, so the self-heal layer cannot
    // match it by type directly. C_HitDeathReactions is the RTTI-typed member at the immediately adjacent
    // qword (C_Player + 0x240); pairing it with the entity pointer (C_Player + 0x38) brackets the
    // look-controller slot so its offset rides a uniform layout shift only when both straddling neighbours
    // agree on one delta, and stays nominal otherwise (fail-closed). See offset_heal.cpp.
    constexpr ptrdiff_t C_PLAYER_HIT_DEATH_REACTIONS_OFFSET = 0x240;
    constexpr const char *C_HIT_DEATH_REACTIONS_RTTI_NAME = ".?AVC_HitDeathReactions@entitymodule@wh@@";
    constexpr ptrdiff_t LOOK_CONTROLLER_PITCH_OFFSET = 0x8;   // scalar look pitch (radians, 0 = level)
    constexpr ptrdiff_t LOOK_CONTROLLER_PITCH2_OFFSET = 0x48; // synchronized pitch copy
    // Scalar look yaw (radians; horizontal forward = (-sin yaw, cos yaw)), with a synchronized copy.
    // AIM-ONLY: writing this steers camera-relative MOVEMENT (the move frame reads the derived look
    // quat) but does NOT turn the body - the body heading is owned by the animated-character layer,
    // driven separately via the BODY-turn constants below.
    constexpr ptrdiff_t LOOK_CONTROLLER_YAW_OFFSET = 0x10;
    constexpr ptrdiff_t LOOK_CONTROLLER_YAW2_OFFSET = 0x44;
    // The derived look quaternion (XYZW) at controller + 0x24, returned by GetLookQuat (C_Player vtable slot 57).
    // The engine rebuilds it every frame from the controller's Euler angles (pitch +0x8, roll +0xC, yaw +0x10).
    // It is the player's clean AIM orientation and carries NO head-bob, weapon-sway, or engine view-shake (combat /
    // hit / landing). At rest it equals the CView eye quat (SVIEWPARAMS_ROTATION_OFFSET). During an action the eye
    // quat diverges by the view-shake while this quat stays on the aim. StableAimBasis builds the third-person rig
    // basis from this aim, so a multi-meter follow distance does not amplify the view-shake into a camera swing.
    // The raw quat is not always level. A horse mount animation leaves a roll angle at +0xC that normal look input
    // never clears, so the quat stays tilted for the ride. StableAimBasis therefore reads it through
    // level_rotation(look_heading()), which keeps only the heading and the scalar pitch, as the first-person camera
    // does.
    constexpr ptrdiff_t LOOK_CONTROLLER_QUAT_OFFSET = 0x24;
    // The controller's per-frame update (1.5.6 sub_1804415E0) feeds the look angles three Ang3 deltas (pitch at +0,
    // yaw at +8) that sub_180441928 adds before the view limits clamp them: the player's look input
    // (LOOK_CONTROLLER_INPUT_DELTA_OFFSET, copied from the input each frame, mouse and stick alike), the change of a
    // decaying procedural kick (LOOK_CONTROLLER_KICK_DELTA_OFFSET) and the game's own turn requests (+0x88, cleared
    // after). The update runs before the camera each frame and leaves the first two in place until the next one, so
    // the camera reads this frame's. A mouse turn's yaw change minus the input delta stays under 1e-5 rad. The other
    // writers of the look angles are the body re-derive (sub_1806442A8) and the view limits. Steam 1.5.6, GOG 1.5 and
    // Game Pass 1.4 store both deltas at these offsets (`movsd [rcx+64h]`, `movsd [rcx+7Ch]`).
    constexpr ptrdiff_t LOOK_CONTROLLER_INPUT_DELTA_OFFSET = 0x7C;
    constexpr ptrdiff_t LOOK_CONTROLLER_KICK_DELTA_OFFSET = 0x64;
    constexpr ptrdiff_t ANG3_PITCH_OFFSET = 0x0;
    constexpr ptrdiff_t ANG3_YAW_OFFSET = 0x8;

    // Player BODY-turn: force the entity world yaw (camera-relative body facing)
    // The look controller above is aim-only, so a separate primitive turns the BODY. The engine's
    // CAnimatedCharacter::ForceOverrideRotation (main vtable slot 95) is exactly two writes: an active
    // byte and a world quat. The animated-character update copies that quat into the entity rotation
    // for the frame, REPLACING the animation-derived facing, then clears the active byte (consume-once),
    // so it is re-asserted every frame while held. We replicate it with direct member writes (same effect,
    // no cross-thread call), gated by validating the resolved CAnimatedCharacter vtable.
    // Resolution from C_Player (validated by its RTTI type name):
    //   C_AnimatedHuman    = *(C_Player + C_PLAYER_ANIMATED_HUMAN_OFFSET)   [wh::animationmodule::C_AnimatedHuman]
    //   CAnimatedCharacter = *(C_AnimatedHuman + ANIMATED_HUMAN_ANIMCHAR_OFFSET), validated by its RTTI type name
    //   then write animchar+ANIMCHAR_OVERRIDE_ROT_ACTIVE_OFFSET = 1 and the world quat (XYZW) at
    //   animchar+ANIMCHAR_OVERRIDE_ROT_QUAT_OFFSET.
    constexpr ptrdiff_t C_PLAYER_ENTITY_OFFSET = 0x38; // C_Player -> CEntity (resolve fresh each frame)
    // RTTI type-descriptor name of CEntity, the self-heal anchor for the entity pointer. CEntity is a
    // stable engine base type (per the self-heal guidance to key on a base, not a game-specific subtype).
    constexpr const char *C_ENTITY_RTTI_NAME = ".?AVCEntity@@";
    constexpr ptrdiff_t C_PLAYER_ANIMATED_HUMAN_OFFSET = 0x268;
    // RTTI type-descriptor name of C_AnimatedHuman, the self-heal anchor for the animated-human pointer.
    constexpr const char *C_ANIMATED_HUMAN_RTTI_NAME = ".?AVC_AnimatedHuman@animationmodule@wh@@";
    constexpr ptrdiff_t ANIMATED_HUMAN_ANIMCHAR_OFFSET = 0x20;
    // RTTI type-descriptor name of CAnimatedCharacter, used to validate the resolved animchar.
    constexpr const char *ANIMATED_CHARACTER_RTTI_NAME = ".?AVCAnimatedCharacter@@";
    constexpr ptrdiff_t ANIMCHAR_OVERRIDE_ROT_ACTIVE_OFFSET = 0x1D8; // BYTE, set to 1 each frame (consume-once)
    constexpr ptrdiff_t ANIMCHAR_OVERRIDE_ROT_QUAT_OFFSET = 0x1DC;   // world Quat XYZW (16 bytes)
    // Component offsets within a 16-byte quaternion (4 contiguous floats, X Y Z W). Used to write the
    // override-rotation quat above field by field; a quat is always this packed-float layout, so these are
    // sizeof(float) strides, not an engine-version struct map.
    constexpr ptrdiff_t QUAT_X_OFFSET = 0x0;
    constexpr ptrdiff_t QUAT_Y_OFFSET = 0x4;
    constexpr ptrdiff_t QUAT_Z_OFFSET = 0x8;
    constexpr ptrdiff_t QUAT_W_OFFSET = 0xC;

    // The head-visibility setter (k_headVisibilityCandidates) has signature
    // void __fastcall(this /*rcx*/, bool hide_head /*dl*/, char flags /*r8b*/). The
    // first-person rig hides the player head; forcing hide_head to false keeps it
    // visible while the third-person offset is rendering so the player is not headless
    // from behind.

    // Global action dispatcher (k_actionDispatchCandidates): the C++ source of Lua Player:OnAction. Fires
    // once per action-map action with its name and post-action-map value:
    //   _QWORD*(this /*rcx*/, const char** action_name /*rdx*/, uint activation /*r8d*/, float value /*xmm3*/)
    // The value is device-agnostic (keyboard, gamepad and rebinds all funnel through the same action
    // name), so reading the xi_movey / xi_movex axis here gives movement INTENT - nonzero while a key is
    // held even when a wall arrests the body, which the body-position speed cannot distinguish.
    // The movement action names are matched in player_onaction_hook.cpp (a small candidate list across the
    // xi_* / movement_* / move* action maps); a trace-level probe there logs each distinct action name once so
    // the on-foot movement axis vocabulary can be confirmed at runtime.

    // Physics world raycast (camera collision + the ranged crosshair ray)
    // IPhysicalWorld::RayWorldIntersection inline helper (k_rayWorldIntersectionCandidates). Casts a
    // world ray and fills a ray_hit; returns the hit count. Signature (Microsoft x64):
    //   int(this /*rcx = p_physical_world*/, const Vec3* org /*rdx*/, const Vec3* dir /*r8*/,
    //       int objtypes /*r9d*/, uint flags, ray_hit* hits, int n_max_hits, void* p_skip_ents,
    //       int n_skip_ents, void* p_foreign_data, int i_foreign_data, const char* p_name_tag)
    // dir is NOT normalized - its length is the maximum ray length, and ray_hit.dist is the
    // world-space distance to the hit.

    // p_physical_world (IPhysicalWorld*) is a member of the g_env struct: its slot address is
    // g_env + PHYSICAL_WORLD_OFFSET. It is derived from the patch-resiliently resolved g_env
    // base, so no separate static address is hardcoded. The slot is dereferenced fresh on each
    // ray: it is null until a level's physical world exists, and can change on reload.
    constexpr ptrdiff_t PHYSICAL_WORLD_OFFSET = 0x30;

    // Hardware-mouse cursor reference counter: the universal "a UI wants the OS cursor" signal.
    // Menus, inventory/map, corpse-and-container loot, trade and dialogue all route through
    // CHardwareMouse::IncrementCounter / DecrementCounter, which keep m_iReferenceCounter as the
    // count of outstanding UI cursor requests; it reads > 0 whenever a UI is up and 0 in plain
    // gameplay (including combat and bow aiming). The hardware mouse (IHardwareMouse*) is a g_env
    // member at g_env + GENV_HARDWARE_MOUSE_OFFSET, and its counter (int) sits at p_hardware_mouse +
    // HARDWARE_MOUSE_CURSOR_COUNT_OFFSET. Derived from the patch-resilient g_env base and screened on
    // each read; the orbit-freeze gate no-ops if either link cannot be resolved.
    constexpr ptrdiff_t GENV_HARDWARE_MOUSE_OFFSET = 0x118;
    constexpr ptrdiff_t HARDWARE_MOUSE_CURSOR_COUNT_OFFSET = 0x30;

    // Engine frame clock: the camera paces its per-frame integrators on the engine's own frame time so that a
    // rate-driven motion (the gamepad orbit) advances by the same step per frame as the world. The timer
    // (ITimer*, class CTimer) is a g_env member at GENV_TIMER_OFFSET, confirmed by its RTTI name before use.
    //   GetFrameStartTime = vtable slot 5 (+0x28): const CTimeValue &(this /*rcx*/, ETimer which /*edx*/). The
    //     CTimeValue is one int64; the UI clock's value is stamped once at frame start and never pauses, so it
    //     identifies the frame (both game-view frustum builds of one frame read the same stamp).
    //   GetFrameTime = vtable slot 9 (+0x48): float(this /*rcx*/, ETimer which /*edx*/), seconds. The GAME clock
    //     returns the frame time the world advanced by: clamped, time-scaled and, with t_Smoothing on (the
    //     default), averaged over the last quarter second. It returns 0 while the game timer is paused, when the
    //     UI clock's unsmoothed frame time is used instead so menus still ease.
    constexpr ptrdiff_t GENV_TIMER_OFFSET = 0x80;
    constexpr const char *CTIMER_RTTI_NAME = ".?AVCTimer@@";

    // The shared camera-view console variable (camera_view_signal.cpp), which the camera publishes for other mods.
    // gEnv's console (IConsole *, class CXConsole) is at GENV_CONSOLE_OFFSET. CXConsole slot 3 is
    // RegisterInt(name, value, flags, help, change function) and slot 23 GetCVar(name). A CXConsoleVariableInt keeps
    // its value at CVAR_INT_VALUE_OFFSET, which its slot 2 (GetIVal) and slot 3 (GetI64Val) read. The same on
    // Steam 1.5.6, GOG 1.5 and Game Pass 1.4, and each slot target is checked against Aob::k_console* before use.
    constexpr ptrdiff_t GENV_CONSOLE_OFFSET = 0xA8;
    constexpr const char *CXCONSOLE_RTTI_NAME = ".?AVCXConsole@@";
    constexpr const char *CXCONSOLE_VARIABLE_INT_RTTI_NAME = ".?AVCXConsoleVariableInt@@";
    constexpr std::size_t CONSOLE_VTABLE_REGISTER_INT_SLOT = 3;
    constexpr std::size_t CONSOLE_VTABLE_GET_CVAR_SLOT = 23;
    constexpr std::size_t CVAR_INT_VTABLE_GET_IVAL_SLOT = 2;
    constexpr std::size_t CVAR_INT_VTABLE_GET_I64VAL_SLOT = 3;
    constexpr ptrdiff_t CVAR_INT_VALUE_OFFSET = 0x48;
    constexpr const char *CAMERA_VIEW_CVAR_NAME = "mod_camera_view";
    constexpr const char *CAMERA_VIEW_CVAR_HELP =
        "Camera view the player sees, published by the active camera mod: 0 first person (game default), 1 third "
        "person. Values from 2 are reserved. Read it, never write it unless you are the camera mod.";
    constexpr ptrdiff_t ITIMER_VTABLE_GET_FRAME_START_TIME_OFFSET = 0x28;
    constexpr ptrdiff_t ITIMER_VTABLE_GET_FRAME_TIME_OFFSET = 0x48;
    // ITimer::GetRealFrameTime(): the last frame's real duration, which neither the game's time scale (a perfect
    // block's slow motion) nor its game-time scale (1/1000 while the inventory is open) touches; 0 while the timer is
    // disabled.
    constexpr ptrdiff_t ITIMER_VTABLE_GET_REAL_FRAME_TIME_OFFSET = 0x50;
    constexpr int ETIMER_GAME = 0;
    constexpr int ETIMER_UI = 1;

    // ray_hit field offsets (CryEngine physinterface.h; struct size 0x50).
    constexpr size_t RAY_HIT_SIZE = 0x50;
    constexpr ptrdiff_t RAY_HIT_OFFSET_DISTANCE = 0x00; // float, world distance along dir
    constexpr ptrdiff_t RAY_HIT_OFFSET_COLLIDER = 0x08; // IPhysicalEntity* pCollider (the entity hit)
    constexpr ptrdiff_t RAY_HIT_OFFSET_PART = 0x10;     // int ipart, the index of the entity's geometry part hit
    constexpr ptrdiff_t RAY_HIT_OFFSET_SURFACE = 0x18;  // short surface_idx, the mapped surface type of the hit
    constexpr ptrdiff_t RAY_HIT_OFFSET_POINT = 0x24;    // Vec3 world hit position
    constexpr ptrdiff_t RAY_HIT_OFFSET_NORMAL = 0x30;   // Vec3 surface normal
    constexpr ptrdiff_t RAY_HIT_OFFSET_TERRAIN = 0x3C;  // int bTerrain (non-zero == global terrain heightmap hit)

    // CPhysicalEntity / CPhysicalPlaceholder world AABB (m_BBox): min @ +0x08, max @ +0x14, each a Vec3. Static world
    // is physicalized one entity per brush (the whole CGF goes into it), so this box spans the whole object: a house
    // or barn reads 16-26m, while a window frame or a prop on it is its own collider under 1m. The occluder scan
    // matches a hit to the render node that holds this box.
    constexpr ptrdiff_t PHYS_ENTITY_BBOX_MIN_OFFSET = 0x08;
    constexpr ptrdiff_t PHYS_ENTITY_BBOX_MAX_OFFSET = 0x14;
    // CPhysicalEntity m_parts (geom part array) and one part's stride and flags (its geom_colltype bits, which decide
    // which probes stop on it), for the Trace collision line only.
    constexpr ptrdiff_t PHYS_ENTITY_PARTS_OFFSET = 0x118;
    constexpr ptrdiff_t PHYS_PART_STRIDE = 0x88;
    constexpr ptrdiff_t PHYS_PART_FLAGS_OFFSET = 0x50;

    // CPhysicalEntity foreign data: m_pForeignData @ +0x20, and at +0x28 two 16-bit fields: m_iForeignData (the
    // PHYS_FOREIGN_ID, low word; 1 == PHYS_FOREIGN_ID_STATIC = a static brush) and m_iForeignFlags (high word: the
    // PFF_* bits a brush gets from its render flags, or an entity's flags, for example 0x00200002), so only the low
    // word is compared. Brushes physicalized at runtime (furniture, baskets: +0x20 -> the CBrush) and vegetation (a
    // tree's trunk: +0x20 -> the CVegetation) carry their node here. Baked level geometry (walls, houses, sheds,
    // designer volumes) is created with pForeignData = NULL and iForeignData = 1, so for most static hits the link is
    // null.
    constexpr ptrdiff_t PHYS_ENTITY_FOREIGN_DATA_OFFSET = 0x20;
    constexpr ptrdiff_t PHYS_ENTITY_FOREIGN_TYPE_OFFSET = 0x28;
    constexpr int PHYS_FOREIGN_ID_STATIC = 1;
    // An entity's physics (a pickable sack, a crate or a cart placed as an entity, a door) has iForeignData 2 ==
    // PHYS_FOREIGN_ID_ENTITY and its CEntity at +0x20, for example the static CPhysicalEntity of the sack entity
    // sack_miller_dark006607 (sack_wearable_dark.cgf).
    constexpr int PHYS_FOREIGN_ID_ENTITY = 2;

    // entity_query_flags subset (physinterface.h; stock numbering in WHGame's 3DEngine RWI helper, 1.5.6
    // sub_1804846E4: ent_static=1, ent_sleeping_rigid=2, ent_rigid=4, ent_living=8, ent_independent=0x10,
    // ent_terrain=0x100). Camera collision blocks on static geometry and the ground only - ent_static | ent_terrain -
    // which is buildings/walls/level geometry/static props/vegetation, the invisible designer volumes (see
    // RWI_FLAGS_STOP_AT_SOLID), plus the ground. It EXCLUDES ent_independent (0x10), where the player's own body
    // physics lives: the skeleton CArticulatedEntity (sim class 4, set in its ctor; its bone parts carry ray+player
    // colltype, so it is what a camera ray would self-hit), plus ropes / ragdolls / dangling attachments. It also
    // excludes ent_living (player + NPC capsules) and ent_rigid|ent_sleeping_rigid (movable props, animal bodies).
    // This mirrors CryEngine's "robust obstruction" intent of hitting only the world, without a per-frame
    // skip-entities list. Trade-off: the camera also glides through movable rigids (crates, barrels, cart, a door
    // leaf, cows/horses).
    constexpr int RWI_OBJTYPES_CAMERA =
        0x101; // ent_static | ent_terrain (static world incl. invisible designer volumes)
    // ent_terrain alone: the lens's ground probe (camera/lens.cpp), which asks only whether the ground is near.
    constexpr int RWI_OBJTYPES_TERRAIN = 0x100;
    // rwi_stop_at_pierceable (0x0F) | rwi_colltype_any (0x400). rwi_colltype_any does not widen the ray to every
    // collision class. It only changes how the colltype bits in the high word are matched: with it a geometry part
    // needs ANY one of them, without it ALL of them (an empty high word means geom_colltype_ray alone). With the two
    // bits requested below it is what lets a part flagged only player or only ray be hit; it only adds hits.
    //
    // The high word adds an explicit geometry collide-type request: geom_colltype_ray (0x8000 << 16 = 0x80000000) |
    // geom_colltype_player (geom_colltype1 = 0x0002 << 16 = 0x00020000). Fabric tent/awning/stall ROOFS in KCD2 are
    // static collision-PROXY meshes: a render node carrying the ERF_COLLISION_PROXY render flag (0x400000) gets its
    // physics parts flagged 0x100A (brush) or 0xA (entity) when its physical materials update, which includes
    // geom_colltype_player but NOT geom_colltype_ray, so a plain colltype_any ray passes straight through them.
    // Requesting ray|player colltype makes
    // RayWorldIntersection report those proxies, so the camera stops at the cloth: a tent is a structure (objtypes stay
    // RWI_OBJTYPES_CAMERA 0x101).
    // Side effect: the player bit also admits the level's INVISIBLE designer collision volumes (Brush/designer_*.cgf,
    // surfaces mat_nodraw_designer / mat_player_only / mat_npc_only). They are scanned as brushes, so with the occluder
    // fade on one is stepped past but never faded (dissolving its no-draw material would draw a grey box); with the
    // fade off it stops the camera like a wall.
    //
    // Composed from the named physinterface.h bit values rather than a magic literal so the intent is
    // self-documenting and each contributing flag is independently auditable.
    //
    // CAMERA_GEOM_COLLTYPES is that set of part collide types, geom_colltype_ray (0x8000) | geom_colltype_player
    // (0x0002), which the fan's rays carry in their high word.
    constexpr unsigned int CAMERA_GEOM_COLLTYPES = 0x8002;
    constexpr unsigned int RWI_STOP_AT_PIERCEABLE = 0x0000000F; // stop at the first solid (non-pierceable) hit
    constexpr unsigned int RWI_COLLTYPE_ANY = 0x00000400;       // match any (not all) of the requested colltype bits
    constexpr unsigned int RWI_FLAGS_STOP_AT_SOLID =
        RWI_STOP_AT_PIERCEABLE | RWI_COLLTYPE_ANY | (CAMERA_GEOM_COLLTYPES << 16); // 0x8002040F
    static_assert(RWI_FLAGS_STOP_AT_SOLID == 0x8002040Fu);

    // Indoors, the GameState a preset binds with the `indoors` token (indoor_state.cpp). KCD2 marks building interiors
    // with the engine's clip volumes: one ClipVolume entity per enclosed interior (houses, sheds, barns, cellars),
    // placed with its building's prefab, which the renderer uses to keep outdoor light out.
    // CClipVolumeManager::GetClipVolumeByPos(this /*rcx*/, const Vec3* pos /*rdx*/, const IClipVolume* ignore /*r8*/)
    // -> CClipVolume* (1.5.6 0x18063C970) returns the first active volume whose mesh holds the point, through the
    // manager's 128x128 grid under its read lock, or null (also while the e_ClipVolumes CVar is off). The engine asks
    // it for the view camera on the main thread (sub_1805A0D24). The manager is a Cry3DEngine global
    // (AnchorId::ClipVolumeManager, RTTI below), null until a level loads. CClipVolume keeps the stock layout
    // (Cry3DEngine/ClipVolume.h): the name at +0x08 (char[64]: the entity name and its prefab path), the world AABB at
    // +0xB8 and the EClipVolumeFlags at +0xE8, read for the Trace line only.
    constexpr const char *CCLIP_VOLUME_MANAGER_RTTI_NAME = ".?AVCClipVolumeManager@@";
    constexpr ptrdiff_t CLIP_VOLUME_NAME_OFFSET = 0x08;
    constexpr std::size_t CLIP_VOLUME_NAME_CAPACITY = 64;
    constexpr ptrdiff_t CLIP_VOLUME_BBOX_OFFSET = 0xB8;
    constexpr ptrdiff_t CLIP_VOLUME_FLAGS_OFFSET = 0xE8;

    // Render-octree occluder scan (the objects the camera fades instead of pulling in front of)
    // The physics rays see collision proxies, not what is drawn, and some objects carry no ray-collidable physics at
    // all (tent / awning canopy cloth, e.g. canopy_tent_cover_a_nosticks.cgf). The renderer sees every drawn object:
    // I3DEngine::GetObjectsInBox is an octree query that returns the render nodes overlapping a box, independent of
    // physics collision type. It walks only the octree root (C3DEngine+0x698) and the VisArea lists, never the
    // always-visible list at C3DEngine+0x6B8, and RegisterEntity files a node flagged ERF_RENDER_ALWAYS (0x10) in
    // that list instead of the octree, so such a node is never returned. That covers the engine's own always-visible
    // nodes and, while HenrySenses runs with RenderAlways on, every brush it outlines (a bed, a tub, a kettle).
    //
    // p3DEngine (I3DEngine*) is a g_env member at g_env + GENV_3DENGINE_OFFSET; the query function is
    // resolved patch-resiliently (AnchorId::GetObjectsInBox). Both are screened on each use.
    constexpr ptrdiff_t GENV_3DENGINE_OFFSET = 0x08;
    // I3DEngine::GetObjectsInBox(this /*rcx*/, const AABB* bbox /*rdx; 6 floats min.xyz,max.xyz*/,
    //   IRenderNode** p_out /*r8*/) -> uint32 count. p_out == null returns the count only; otherwise it
    // memcpys the FULL list with NO size cap, so the count is read first and the buffer sized to fit it.
    // IRenderNode vtable: GetBBox = slot 4 (+0x20): AABB*(this /*rcx*/, AABB* out /*rdx*/) -> ptr to
    //   {Vec3 min @0, Vec3 max @0xC}. GetRenderNodeType = slot 7 (+0x38): int(this /*rcx*/) -> EERType
    //   (eERType_Brush == 1). These are the stock IShadowCaster slots of IRenderNode.h (0 dtor, 1 HasOcclusionmap,
    //   2 ComputeLod, 3 Render, 4 GetBBox, 5 FillBBox, 6 GetEntityCharacter, 7 GetRenderNodeType); the retail 1.5.6
    //   CBrush, COwnedBrush and CMovableBrush vtables have the same order.
    constexpr ptrdiff_t RENDERNODE_VTABLE_GETBBOX_OFFSET = 0x20;
    constexpr ptrdiff_t RENDERNODE_VTABLE_GETTYPE_OFFSET = 0x38;
    // Type 1 covers CBrush and COwnedBrush. CMovableBrush reports 0x11 but derives from CBrush with the same GetBBox
    // (+0xB4 AABB), +0x50 Matrix34 and +0x98 IStatObj; the occluder scan queries it as its own type.
    constexpr int EERTYPE_BRUSH = 1;
    // IRenderNode::m_dwRndFlags (the 64-bit ERenderNodeFlags bitmask); the low dword carries ERF_HIDDEN.
    // GetObjectsInBox returns octree nodes regardless of whether they are actually DRAWN, so a node flagged
    // ERF_HIDDEN (placed but not rendered - e.g. conditional laundry / rag decorations) must be skipped:
    // it cannot hide the character, and fading it would do nothing.
    constexpr ptrdiff_t RENDERNODE_RNDFLAGS_OFFSET = 0x28;
    constexpr unsigned int ERF_HIDDEN = 0x100; // ERenderNodeFlags BIT(8), the stock IRenderNode.h value
    // C3DEngine::GetObjectsByTypeInBox(this /*rcx*/, EERType type /*edx*/, const AABB* bbox /*r8*/,
    //   IRenderNode** p_out /*r9*/, uint64 rnd_flags_mask /*stack*/) -> uint32 count, the vtable slot right before
    // GetObjectsInBox. Same count-or-fill contract, but it descends only into octree cells whose per-type object
    // mask holds the type and walks only that type's object list, keeping the objects whose GetRenderNodeType()
    // equals it. A typed query therefore skips every node of another type, which the untyped query visits and the
    // caller would only discard, which is most of the untyped query's cost. The occluder scan runs one typed query per
    // type it keeps (1, 2, 0x11, 0x13, 0x17). rnd_flags_mask ~0 keeps every node whatever its render flags. The slot
    // is trusted only while the GetObjectsInBox slot of the same vtable holds the resolved AnchorId::GetObjectsInBox
    // entry, which pins the slot numbering; otherwise the untyped query is used and the callers' own type filter
    // applies.
    // Inside, GetObjectsByTypeInBox zeroes a local std::vector<IRenderNode*> (first, last, end of capacity), gathers
    // into it through a collector (AnchorId::OctreeCollectByType: this /*rcx, not read*/, type /*edx*/, bbox /*r8*/,
    // vector* /*r9*/, rnd_flags_mask /*stack*/; the octree root, then the VisAreas), copies it out and frees it with
    // std::_Deallocate (AnchorId::EngineDeallocate: block, capacity bytes). The count call and the fill call each walk
    // the octree, so the occluder scan calls the collector once per query instead, into one vector it keeps and frees
    // at shutdown with that deallocator (the engine allocator grew it).
    constexpr ptrdiff_t ENGINE3D_VTABLE_GET_OBJECTS_BY_TYPE_IN_BOX_OFFSET = 242 * 8;
    constexpr ptrdiff_t ENGINE3D_VTABLE_GET_OBJECTS_IN_BOX_OFFSET = 243 * 8;
    constexpr uint64_t ENGINE3D_QUERY_ANY_RNDFLAGS = ~0ull;
    // CVegetation reports EERType 2. Trees and the bushes placed as single instances are vegetation nodes; grass and
    // the small merged bushes are merged meshes (type 0x17), which the fade cannot reach (no CLodValue).
    constexpr int EERTYPE_VEGETATION = 2;
    // CMergedMeshRenderNode (1.5.6 vtable 0x18402A508) reports EERType 0x17 (slot 7 is `mov eax, 17h`). Its GetBBox
    // (slot 4) returns the visible box at +0xA4 and its GetEntityStatObj (slot 22) returns null. A patch draws through
    // AddOrCreatePersistentRenderObject with no CLodValue and then CRenderMesh::Render (sub_1804A10B0), so its items
    // reach CRenderView::AddRenderObject like a tree's and the tree see-through can swap them.
    constexpr int EERTYPE_MERGED_MESH = 0x17;
    // CStatObj's model-space bounding box: min at +0x100, max at +0x10C (Vec3 each). The statobj vtable (1.5.6
    // 0x183A2F888) slot 4 GetAABB copies +0x100..+0x114, and slots 33 and 34 (SetBBoxMin, SetBBoxMax) write +0x100 and
    // +0x10C. For example, the beam fence fence_beams_c_fancy spans z -1.53 (its posts go into the ground) to 2.51, and
    // a low stick gate reaches z 1.25.
    constexpr ptrdiff_t STATOBJ_BOX_MIN_OFFSET = 0x100;
    constexpr ptrdiff_t STATOBJ_BOX_MAX_OFFSET = 0x10C;
    // CMovableBrush reports 0x11 (see EERTYPE_BRUSH). CRenderProxy (0x13) is an entity's render node (a cart, a door,
    // a sign, an NPC); the scan tests the statobj of each of its drawn slots, and a character's slot has none.
    constexpr int EERTYPE_MOVABLE_BRUSH = 0x11;
    constexpr int EERTYPE_ENTITY_PROXY = 0x13;
    // IRenderNode::GetEntityStatObj(this /*rcx*/, uint32 sub_part /*edx*/, Matrix34A* world /*r8, may be null*/,
    // bool only_visible /*r9b*/) -> IStatObj*. Vtable slot 22 in this fork; the stock IRenderNode.h order puts it at
    // 21, so the fork has one more virtual before it. CBrush and CMovableBrush (1.5.6 0x180433F44) return the +0x98
    // statobj and copy the +0x50 matrix; CVegetation (0x1804323B4) returns its group's statobj and builds the instance
    // matrix; CRenderProxy (0x1807F2068) returns the slot's statobj and the entity matrix, and reads only_visible.
    // The matrix is written with 16-byte stores, so the buffer must be 16-byte aligned.
    constexpr ptrdiff_t RENDERNODE_VTABLE_GETENTITYSTATOBJ_OFFSET = 22 * 8;
    // IRenderNode::GetEntityCharacter uses the same four arguments as GetEntityStatObj in this fork (not the
    // stock three-argument ABI). CRenderProxy slot 6 (1.5.6 0x1807F1DBC) returns the slot's +0x80 character;
    // with a null world buffer this only reads the slot. Animated doors use this path instead of a statobj.
    constexpr ptrdiff_t RENDERNODE_VTABLE_GETENTITYCHARACTER_OFFSET = 6 * 8;
    // CCharInstance::GetFilePath(this) -> const char*, slot 31 (1.5.6 0x1811A6690), reads the +0xA38 path.
    constexpr ptrdiff_t CHAR_INSTANCE_VTABLE_GETFILEPATH_OFFSET = 31 * 8;
    // IRenderNode::GetOwnerEntity(this) -> IEntity*, vtable slot 67 in this fork. CRenderProxy (1.5.6 0x181A73360)
    // returns its CEntity at +0x80, the CEntity an entity's physics names as its foreign data; CBrush (0x18066CD10)
    // returns null.
    constexpr ptrdiff_t RENDERNODE_VTABLE_GETOWNERENTITY_OFFSET = 67 * 8;

    // Character fade (hooks/character_fade.cpp): the player's character dithers out when the third-person camera has
    // no room behind it. CRenderProxy::Render (1.5.6 sub_18049FD10, this, const SRendParams&, const
    // SRenderingPassInfo&) copies the parameters into every slot's render parameters (sub_1804A0084), and the
    // character slot hands the CLodValue dissolve byte at SRendParams + SRENDPARAMS_LOD_OFFSET +
    // CLODVALUE_DISSOLVE_OFFSET to its per-frame render objects with FOB_DISSOLVE (sub_18049E210 from the slot
    // renderer sub_18049F08C). The engine's own LOD crossfade drives the same byte: with LodA set (FOB_DISSOLVE_OUT)
    // a higher byte dithers more of the object away, and for the LOD fading in (LodA -1, LodB set) a lower one does.
    // The proxy's entity (m_pEntity) identifies the player. A static slot hands the same parameters to
    // CStatObj::Render (vtable slot 14, sub_18049E258), which passes their CLodValue to
    // AddOrCreatePersistentRenderObject (and to its non-persistent path), so a carried static item (a quiver, a sword)
    // fades through the same raise.
    constexpr ptrdiff_t SRENDPARAMS_LOD_OFFSET = 0xAC;     // CLodValue {i16 lodA, i16 lodB, u8 dissolve}
    constexpr ptrdiff_t CLODVALUE_LOD_A_OFFSET = 0x00;     // int16 current LOD (-1 = none)
    constexpr ptrdiff_t CLODVALUE_LOD_B_OFFSET = 0x02;     // int16 LOD fading in (-1 = none)
    constexpr ptrdiff_t CLODVALUE_DISSOLVE_OFFSET = 0x04;  // uint8 dissolve reference
    constexpr ptrdiff_t RENDER_PROXY_ENTITY_OFFSET = 0x80; // CRenderProxy::m_pEntity (CEntity*)
    // SRenderingPassInfo fields of the stock IsGeneralPass test: render-stack level (0 = main view), shadow-map type
    // (0 = not a shadow pass) and the aux-window flag. The fade raises the byte in the main view only, so the
    // character keeps its shadow.
    constexpr ptrdiff_t PASS_INFO_RECURSION_OFFSET = 0x01;
    constexpr ptrdiff_t PASS_INFO_SHADOW_OFFSET = 0x02;
    constexpr ptrdiff_t PASS_INFO_AUX_WINDOW_OFFSET = 0x2D;
    // The engine reads a dissolve byte of 255 with no LOD fading in as "not drawn" (AddOrCreatePersistentRenderObject
    // skips such an object), so a fade stops one short of it.
    constexpr int DISSOLVE_MAX_BYTE = 254;

    // Occluder fade: dither out the render nodes that hide the character instead of pulling the camera in.
    // CObjManager::AddOrCreatePersistentRenderObject (1.5.6 sub_1820D9410) takes (this, SRenderNodeTempData* td,
    // CRenderObject** out, CLodValue* lod, const Matrix34* world, const SRenderingPassInfo* pass, const SRendParams*
    // params, may be null) and returns true when the persistent object needs no refill. On every main-view frame it
    // copies lod->dissolve onto the node's persistent render object (+0x20) and sets FOB_DISSOLVE, plus
    // FOB_DISSOLVE_OUT when lod->lodA >= 0; the renderer then rebuilds the pipeline state once when the dissolve
    // starts and only the per-draw constants while the byte changes. The engine's own LOD crossfade drives the same
    // byte: with OUT set, a higher byte dithers more of the object away. Brushes, vegetation and the static slots of
    // entities all come through here; characters and merged meshes (grass) do not. The fade raises the byte in the
    // main view only (the PASS_INFO_* test above): a dissolve byte in a shadow pass makes AddOrCreate free the node's
    // persistent objects, and the shadow stays.
    constexpr ptrdiff_t RENDER_TEMP_DATA_OWNER_OFFSET = 0xE0; // SRenderNodeTempData: the owning IRenderNode*

    // Character attachments (character_attachments.cpp): what the player's character carries, which the close-up fade
    // dithers out with it. Every carried item (a weapon in the hand or in its holster, the quiver, a shield, a tool) is
    // an entity of its own, bound to a bone attachment of the character's CCharInstance by a CEntityAttachment that
    // keeps the item's EntityId. Such an attachment draws nothing itself (its RenderAttachment is empty), so the item
    // draws through its own CRenderProxy, the render node the CRenderProxy::Render detour raises the dissolve of. An
    // item that is a character model of its own (a bow, a crossbow) has attachments too: the nocked arrow is bound to
    // the bow's "arrow" bone and the crossbow's lever to the crossbow. Addresses are Steam 1.5.6's.
    //
    // A CEntity's EntityId, and its proxies: an MSVC std::map<uint32 type, IEntityProxy*> whose head (sentinel) node
    // pointer is at ENTITY_PROXY_MAP_OFFSET (CEntity::GetProxy, vtable slot 74, sub_1805D70C4). The render proxy has
    // type 0, the smallest key, so when it exists it is the map's leftmost node, the head node's left link. A map
    // node's uint32 key sits at MSVC_MAP_NODE_KEY_OFFSET, between the sentinel byte and the value.
    constexpr ptrdiff_t ENTITY_ID_OFFSET = 0x0C;
    // CEntity primary vtable slot 27 (1.5.6 0x18083A8F4) returns the attachment parent, and slot 91 (0x180B7258C) is
    // GetSlotWorldTM, which returns const Matrix34&. Its argument with bit 31 set (ENTITY_SLOT_ACTUAL) names an entity
    // slot by its index; without it, the argument names a character joint (sub_1807F1F50).
    constexpr ptrdiff_t ENTITY_VTABLE_GETPARENT_OFFSET = 27 * 8;
    constexpr ptrdiff_t ENTITY_VTABLE_GETSLOTWORLDTM_OFFSET = 91 * 8;
    constexpr std::uint32_t ENTITY_SLOT_ACTUAL = 0x80000000u;
    // The CActorSystem of the global context (the context OFFSET_MANAGER_PTR_STORAGE is read from). CActorSystem::
    // GetActor (slot 3, 1.5.6 0x180647E40) looks up an EntityId in the engine's actor map; a character model alone
    // does not establish actor identity.
    constexpr ptrdiff_t CONTEXT_ACTOR_SYSTEM_OFFSET = 0x180;
    constexpr ptrdiff_t ACTOR_SYSTEM_VTABLE_GET_ACTOR_OFFSET = 3 * 8;
    constexpr const char *C_ACTOR_SYSTEM_RTTI_NAME = ".?AVCActorSystem@@";
    constexpr ptrdiff_t ENTITY_PROXY_MAP_OFFSET = 0xA8;
    constexpr ptrdiff_t MSVC_MAP_NODE_KEY_OFFSET = 0x20;
    constexpr std::uint32_t ENTITY_PROXY_TYPE_RENDER = 0;
    // The map holds the render proxy's IEntityRenderProxy interface, a secondary base of CRenderProxy. The render node
    // is the primary base, which sits the interface's sub-object offset lower (the engine's own GetCharacter subtracts
    // it, 1.5.6 sub_1806411AC). That offset is read from the complete object locator in the interface vtable's -1
    // slot: an MSVC x64 COL has the signature 1 at +0 and the sub-object offset at +4.
    constexpr ptrdiff_t RTTI_COL_SIGNATURE_OFFSET = 0x00;
    constexpr ptrdiff_t RTTI_COL_SUBOBJECT_OFFSET = 0x04;
    constexpr std::uint32_t RTTI_COL_SIGNATURE_X64 = 1;
    // A render proxy's character, as CEntity::GetCharacter (slot 101) reads it through sub_182103EE0: slot 0 must
    // exist (a non-null first entry in the slot vector at RENDER_PROXY_SLOTS_BEGIN_OFFSET / _END_OFFSET), then the
    // proxy's own character at RENDER_PROXY_CHARACTER_OFFSET when it is set, else the slot's (CEntityObject +0x80).
    // A static item's slot has none.
    constexpr ptrdiff_t RENDER_PROXY_SLOTS_BEGIN_OFFSET = 0x88;
    constexpr ptrdiff_t RENDER_PROXY_SLOTS_END_OFFSET = 0x90;
    constexpr ptrdiff_t RENDER_PROXY_CHARACTER_OFFSET = 0x118;
    constexpr ptrdiff_t ENTITY_SLOT_CHARACTER_OFFSET = 0x80;
    // A slot's flags (CEntityObject +0xC0): CRenderProxy::Render (1.5.6 sub_18049FD10) draws only the slots whose
    // ENTITY_SLOT_RENDER bit is set. A proxy with more slots than OCCLUDER_ENTITY_SLOT_MAX is taken for a layout drift.
    constexpr ptrdiff_t ENTITY_SLOT_FLAGS_OFFSET = 0xC0;
    constexpr std::uint32_t ENTITY_SLOT_RENDER = 1;
    constexpr int OCCLUDER_ENTITY_SLOT_MAX = 64;
    // CCharInstance embeds its CAttachmentManager (constructor sub_180ABB0FC, the manager's sub_180ABB01C). The
    // manager keeps its owner character and its attachments, a vector of IAttachment* (GetAttachmentCount, slot 13:
    // (end - begin) >> 3).
    constexpr ptrdiff_t CHAR_INSTANCE_ATTACHMENT_MANAGER_OFFSET = 0x08;
    constexpr ptrdiff_t ATTACHMENT_MANAGER_OWNER_OFFSET = 0x70;
    constexpr ptrdiff_t ATTACHMENT_MANAGER_LIST_BEGIN_OFFSET = 0x78;
    constexpr ptrdiff_t ATTACHMENT_MANAGER_LIST_END_OFFSET = 0x80;
    // CAttachmentBONE: its socket name (const char*, GetName, slot 2) and its bound IAttachmentObject
    // (GetIAttachmentObject, slot 28; AddBinding sub_1809DFA14 stores it). Skin and cloth attachments never hold an
    // entity: their meshes draw inside the owner's own render, which the fade already dissolves.
    constexpr ptrdiff_t ATTACHMENT_BONE_NAME_OFFSET = 0x10;
    constexpr ptrdiff_t ATTACHMENT_BONE_OBJECT_OFFSET = 0x20;
    // CEntityAttachment (constructor sub_1809DFD44): the bound entity's EntityId.
    constexpr ptrdiff_t ENTITY_ATTACHMENT_ID_OFFSET = 0x08;
    // The entity system, a CEntitySystem at g_env + GENV_ENTITY_SYSTEM_OFFSET. GetEntity (slot 14, sub_1804B4F98)
    // splits an EntityId into an index (the low ENTITY_ID_SALT_SHIFT bits, 1 to ENTITY_ID_MAX_INDEX) and a salt (the
    // bits above), and reads the entry at ENTITY_SYSTEM_ARRAY_OFFSET + index * ENTITY_SYSTEM_ENTRY_STRIDE: a uint16
    // salt at +0 that must equal the id's, and the CEntity* at ENTITY_SYSTEM_ENTRY_ENTITY_OFFSET, whose own EntityId
    // must be the one asked for. The walk reads the entry itself instead of calling GetEntity, which takes a reader
    // count on the array with an interlocked add, a write to game memory.
    constexpr ptrdiff_t GENV_ENTITY_SYSTEM_OFFSET = 0xA0;
    constexpr ptrdiff_t ENTITY_SYSTEM_ARRAY_OFFSET = 0x200;
    constexpr ptrdiff_t ENTITY_SYSTEM_ENTRY_STRIDE = 0x18;
    constexpr ptrdiff_t ENTITY_SYSTEM_ENTRY_ENTITY_OFFSET = 0x10;
    constexpr std::uint32_t ENTITY_ID_INDEX_MASK = 0x3FFFF;
    constexpr int ENTITY_ID_SALT_SHIFT = 18;
    constexpr std::uint32_t ENTITY_ID_MAX_INDEX = 0x3FFFD;
    constexpr const char *C_RENDER_PROXY_RTTI_NAME = ".?AVCRenderProxy@@";
    constexpr const char *C_CHAR_INSTANCE_RTTI_NAME = ".?AVCCharInstance@@";

    // The player's skeleton (character_pose.cpp), which the camera's body anchor follows. CCharInstance slot 6
    // returns its ISkeletonPose (1.5.6 0x181AA6BC0, `lea rax, [rcx+698h]`) and slot 10 its IDefaultSkeleton
    // (0x1808A49D0, `mov rax, [rcx+0A70h]`). CSkeletonPose slot 27 (0x180641250) is GetAbsJointByID: a joint's
    // model-space QuatT from the pose data's absolute array, or a static identity for an index out of range (slot 28
    // is the relative one). CDefaultSkeleton slot 4 (0x18053F5AC) is GetJointIDByCRC32: the index of the joint whose
    // lower-case name hashes to that CRC32, or -1. A QuatT holds the rotation (x, y, z, w), then the translation.
    constexpr std::size_t CHAR_INSTANCE_VTABLE_GET_SKELETON_POSE_SLOT = 6;
    constexpr std::size_t CHAR_INSTANCE_VTABLE_GET_DEFAULT_SKELETON_SLOT = 10;
    constexpr std::size_t SKELETON_POSE_VTABLE_GET_ABS_JOINT_SLOT = 27;
    constexpr std::size_t DEFAULT_SKELETON_VTABLE_JOINT_ID_BY_CRC_SLOT = 4;
    constexpr ptrdiff_t QUATT_TRANSLATION_OFFSET = 0x10;
    constexpr const char *C_SKELETON_POSE_RTTI_NAME = ".?AVCSkeletonPose@@";
    constexpr const char *C_DEFAULT_SKELETON_RTTI_NAME = ".?AVCDefaultSkeleton@@";
    constexpr const char *C_ATTACHMENT_MANAGER_RTTI_NAME = ".?AVCAttachmentManager@@";
    constexpr const char *C_ATTACHMENT_BONE_RTTI_NAME = ".?AVCAttachmentBONE@@";
    constexpr const char *C_ENTITY_ATTACHMENT_RTTI_NAME = ".?AUCEntityAttachment@@";
    constexpr const char *C_ENTITY_SYSTEM_RTTI_NAME = ".?AVCEntitySystem@@";

    // Shader twins (hooks/shader_twins.cpp): the character's items that draw again in a forward pass that ignores the
    // dissolve (the eyes with the stock Eye shader; the eyelashes, beard, hair and a hood's hair cards with Hair; the
    // wet film over the eyes, a transparent Illum item) draw with twins of those stock shaders that apply it, so they
    // fade with him in the close-up fade. Addresses below are Steam 1.5.6's (image base 0x180000000), and a
    // value given "at runtime" is what the running 1.5.6 Steam game holds there.
    //
    // The stock shaders that get a twin, the twins' names and the stock binaries they were spliced from are the
    // generated header's k_twins (scripts/build_shader_twins.py), the trees' among them (see the tree see-through
    // below). IShader::GetName returns the name an instance was created with, in lower case at runtime, so names are
    // compared without case. mfForName hands the .ext loader the name it was given (sub_1807E9088 at 0x1807E91D7), so
    // the loader detour knows a twin by its name.
    //
    // The Eye twin's forward pixel shaders apply the dissolve (the stock ones never do, so with them the eyes stay
    // solid in a dithered face). Its items are the character's own: the head skin attachment builds one temporary
    // render object per frame (CAttachmentSKIN::RenderAttachment sub_1804A3C80) whose flags at +0x00 take the
    // SRendParams flags (0x1804A3F77), where the dissolve byte the close-up fade raises set FOB_DISSOLVE
    // (sub_18049E210), and draws every chunk of the head (the eyes included) through CRenderMesh::Render
    // (sub_1804A4B6C, CRenderMesh vtable +0x1D0) into AddRenderObject (0x1804A501A). AddRenderObject turns FOB_DISSOLVE
    // into the RT dissolve bit of the object's RT mask at +0x14 (qword_1850D9538 at 0x1804A52C3), which the compiled
    // object's PSO description carries to every non-shadow stage (sub_1804279E0 reads +0x14), the forward one included:
    // its PSO build sub_1807C5A20 strips the bit only in pass 1 (0x1807C5B50 and 0x1807C5E41), and only when the
    // description's flags at +0x14 have 0x20000. Pass 1 is a second draw in "Scene Forward Opaque" (+0x300, like
    // pass 0) with its own depth state: sub_1807C1BD4 draws lists 0x15 and 0x14 with pass 0 for the items whose object
    // flags lack 0x40000000 (batch flag 0x20 shifted in sub_1804A68F4), then with pass 1 for those that have it. The
    // recursive pass is pass 2 ("Scene Recursive Forward Opaque", +0x1BC0), which keeps the bit. The eye draws with
    // pass 0, and "Scene Forward Eye Overlays" (sub_1807C1FAC) draws with pass 1 without that flag, so the twin's EyePS
    // and EyeOverlayPS are compiled with %_RT_DISSOLVE exactly while the character dissolves.
    //
    // The dissolve's flags: sub_18049E210 (called from CRenderProxy::Render's body at 0x18049F9EF with the SRendParams
    // CLodValue) sets FOB_DISSOLVE for a non-zero dissolve byte, and FOB_DISSOLVE_OUT too when LodA (+0xAC) is not
    // negative, which it is for the close-up fade (the byte is raised on the LOD being drawn). It clears
    // FOB_UPDATED_RTMASK (0x10000) whenever FOB_DISSOLVE changes, so AddRenderObject computes the RT mask again.
    constexpr ptrdiff_t RENDER_OBJECT_FLAGS_OFFSET = 0x00;
    constexpr uint64_t RENDER_OBJECT_FLAG_DISSOLVE = 0x10000000;
    constexpr uint64_t RENDER_OBJECT_FLAG_DISSOLVE_OUT = 0x1000000;

    // CRenderView::AddRenderObject (sub_1804A50A4: view, render element, SShaderItem*, CRenderObject*, const
    // SRenderingPassInfo*, list, after water) is called directly from CRenderMesh::Render (0x1804A501A), so it is
    // hooked at its entry. The render object's node (m_pRenderNode) is at +0x30 and its persistent flag at +0x26 (the
    // not-ready path invalidates only a persistent object). A persistent object keeps the item as a packed key of the
    // resource id, the technique and the shader id (sub_1804A63E0), and a temporary object's compiled object only the
    // item's shader and resources pointers (sub_1804A6104), so a stack copy of the item with another shader is safe to
    // pass.
    constexpr ptrdiff_t RENDER_OBJECT_NODE_OFFSET = 0x30;
    constexpr ptrdiff_t RENDER_OBJECT_PERSISTENT_OFFSET = 0x26;
    // CCompiledRenderObject::Compile (sub_180429534) builds a pipeline-state description from the object's shader
    // item and hands it to CreatePipelineStates (sub_18071D8B4: pipeline, the object's PSO array, the description, the
    // resources' local PSO cache, the shadow flag; its only call is 0x180429AC0). That fills the depth, G-buffer,
    // forward and custom-stage slots of the 20-slot PSO array at +0x70; the object's CRenderObject is at +0x118. The
    // description is a 0x38-byte stack copy the call rewrites as it goes: the CShader at +0x00 (the twin, for a
    // swapped item) and the RT mask at +0x20. The PSO factory (sub_18071F508) hands back a PSO object even when a
    // shader it needs is not compiled yet; that PSO stays invalid, its byte at +0x38 zero, until the renderer creates
    // it again in place once the shader exists (sub_180C185C8, every frame), and the draw skips each pass whose PSO is
    // invalid (sub_180502790 at 0x18050283C). A PSO's reference count is the dword at +0x0C (released by
    // sub_1807C61E0). The first compile of a twin permutation can take seconds, during which the item would lose those
    // passes, so an item keeps its stock PSOs until every pass the stock draws has a valid twin PSO.
    constexpr ptrdiff_t COMPILED_OBJECT_PSO_OFFSET = 0x70;
    constexpr int COMPILED_OBJECT_PSO_SLOTS = 20;
    constexpr ptrdiff_t COMPILED_OBJECT_RENDER_OBJECT_OFFSET = 0x118;
    // The description (sub_1804279E0 fills it, sub_180427B0C copies it for the call): the CShader at +0x00, the
    // resources at +0x08, the technique at +0x10, the item's preprocess flags at +0x14, the RT mask at +0x20 (the
    // render object's 32-bit mask at +0x14 plus the tessellation and instancing bits Compile adds), the render object's
    // flags at +0x28, the stage id CreatePipelineStates writes as it goes at +0x30 (-1 on entry), and geometry bytes up
    // to +0x37. Between a build of a character item that does not dissolve and one that does, only the flags and the RT
    // mask differ: the flags gain FOB_DISSOLVE and FOB_DISSOLVE_OUT, and the RT mask the dissolve bit, which
    // AddRenderObject ORs in for FOB_DISSOLVE (the DissolveRtBit anchor's global, WHGame+0x50D9538, 0x10 at runtime).
    // Batch flags (sub_1804A68F4) and the description's other fields do not read FOB_DISSOLVE, Compile strips the bit
    // only for a shadow pass (sub_1804279D0, pass type 2), and the stages read the flags again (sub_1807C6214 ORs the
    // same dissolve bit for FOB_DISSOLVE; none reads FOB_DISSOLVE_OUT), so a description changed in those two fields
    // asks the PSO factory for exactly the permutations the fading build asks for. CreatePipelineStates keys the
    // resources' local PSO cache (sub_18071DD00) by the whole description, so the fading build finds that set there
    // too.
    constexpr std::size_t PSO_DESC_SIZE = 0x38;
    constexpr ptrdiff_t PSO_DESC_RESOURCES_OFFSET = 0x08;
    constexpr ptrdiff_t PSO_DESC_RT_MASK_OFFSET = 0x20;
    constexpr ptrdiff_t PSO_DESC_OBJECT_FLAGS_OFFSET = 0x28;
    // The int16 stage id at +0x30 names the technique a stage builder builds: -1 for the forward builder
    // (sub_18071D4FC, the CharacterForwardPsosCall anchor), 8 for the depth prepass and 0 for the G-buffer below. The
    // forward stage sits at pipeline+0x48, and a stage cache entry owns five PSOs, so forward occupies slots 10..14 of
    // the twenty-slot object array.
    constexpr ptrdiff_t PSO_DESC_TECHNIQUE_TYPE_OFFSET = 0x30;
    constexpr ptrdiff_t PIPELINE_FORWARD_STAGE_OFFSET = 0x48;
    constexpr int PSO_FORWARD_FIRST_SLOT = 10;
    constexpr int PSO_STAGE_SLOT_COUNT = 5;
    // The depth and G-buffer stages (PlantPrepassPsosCall, PlantGBufferPsosCall): the depth prepass builder
    // (sub_181070880; stage at pipeline+0x38, technique 8) fills slot 0 only. The G-buffer pass builder (sub_180F18784;
    // stage at pipeline+0x40, technique 0) fills slot 5 for pass 0 (Full: LEqual, depth write, the item's alpha test),
    // slot 7 for pass 2 (Micro, which only CMobileGraphicsPipeline draws; r_GraphicsPipelineMobile is 0) and slot 6 for
    // pass 1. Pass 1 is the same description as pass 0 unless the render object's flags have FOB_ZPREPASS; then it is
    // the Attr pass: depth func EQUAL, no depth write, alpha test and dissolve taken out of the RT mask
    // (sub_180F18784, depth state decoded by sub_180DD81F0). The scene draws pass 0 for items without the batch flag
    // FB_ZPREPASS and pass 1 for those with it (sub_1807C24B8), decals pass 1 too (sub_1807C1E44, lists 2 and 4). Only
    // Vegetation-type shaders get FB_ZPREPASS (sub_180C17074: a ZPREPASS technique, shader type 3, +0x9C bit 0 clear or
    // r_ZPrepassVegetation), and only while r_UseZPass is 2 and the item is within r_ZPrepassMaxDist (sub_1804A5908).
    // So a prepassed Vegetation item draws slots 0 and 6, every other item slot 5 (or 6 as a decal), none slot 7.
    constexpr ptrdiff_t PIPELINE_ZPREPASS_STAGE_OFFSET = 0x38;
    constexpr ptrdiff_t PIPELINE_GBUFFER_STAGE_OFFSET = 0x40;
    constexpr int16_t PSO_TECHNIQUE_ZPREPASS = 8;
    constexpr int16_t PSO_TECHNIQUE_GBUFFER = 0;
    constexpr int PSO_ZPREPASS_SLOT = 0;
    constexpr int PSO_GBUFFER_FULL_SLOT = 5; // pass 0
    constexpr int PSO_GBUFFER_ATTR_SLOT = 6; // pass 1
    constexpr uint8_t PSO_GBUFFER_FULL_PASS = 0;
    constexpr uint8_t PSO_GBUFFER_ATTR_PASS = 1;
    // FOB_ZPREPASS in the description's render object flags: AddRenderObject ORs it in from the item's FB_ZPREPASS
    // batch flag (sub_1804A68F4), for any item of the object.
    constexpr uint64_t RENDER_OBJECT_FLAG_ZPREPASS = 0x40000000;
    constexpr ptrdiff_t PSO_REF_COUNT_OFFSET = 0x0C;
    constexpr ptrdiff_t PSO_VALID_OFFSET = 0x38;
    // A PSO's state dword at +0x08: its (re)creation (sub_1807B9754) clears bit 0 and the valid byte on entry, and sets
    // bit 0 once its shaders exist and the D3D12 creation is queued (0x1807B9D8F); the creation (sub_18059E428) then
    // sets the valid byte when it succeeds. Bit 0 alone cannot distinguish queued, running or failed device work; a PSO
    // that is not valid with bit 0 clear has not reached that step in this attempt (including missing shaders or a pass
    // that never draws). Each (re)creation also adds 4 to the dword (bits 2 and up count the attempts). The factory
    // tries every PSO whose shaders are still compiling again each frame (sub_180C185C8: a creation that returns 1
    // stays on the list) and drops one whose creation fails for good (sub_181E12AD0), so a PSO that is not valid and
    // whose count still moves is waiting for a compile.
    constexpr ptrdiff_t PSO_STATE_OFFSET = 0x08;
    constexpr uint32_t PSO_STATE_QUEUED = 0x1;
    constexpr uint32_t PSO_STATE_ATTEMPTS_MASK = ~0x3u;
    // SShaderItem is 32 bytes in this build: the shader, the IRenderShaderResources, the technique (int), the
    // preprocess flags (-1 while not set up), then eight bytes the stock EF_LoadShaderItem zeroes.
    constexpr std::size_t SHADER_ITEM_SIZE = 0x20;
    // CShader m_Flags2 (uint32 at +0x98; the render list select sub_1804A6C00 reads it as v8[38]). EF2_HAIR sends an
    // item to the transparent list (0x1804A6DBE), where the forward stage draws it with the hair pass state, so a Hair
    // twin whose parse did not keep its source's bit (Hair.cfx's global script names it, and the twin keeps that
    // script token for token) would lose its hair layers; such a twin is given up.
    constexpr ptrdiff_t SHADER_FLAGS2_OFFSET = 0x98;
    constexpr uint32_t SHADER_FLAG2_HAIR = 0x800000;
    // IRenderShaderResources::GetStrengthValue(slot), CShaderResources vtable slot 7 (sub_18053FA30: a read of the
    // material's constants, no side effects), with EFTT_OPACITY. sub_1804A6C00 makes the same call on the render job
    // (0x1804A6F4E) and sends an item whose value is below 1 to the transparent list, so only such an Illum item (the
    // eye film, a spectacles' glass) swaps to the IllumFade twin; an opaque one (the clothes) keeps the stock shader.
    constexpr ptrdiff_t SHADER_RESOURCES_VTABLE_GET_STRENGTH_OFFSET = 0x38;
    constexpr int SHADER_RESOURCES_OPACITY_SLOT = 11;
    // The user shader cache's folders of compiled shaders, one per stage; an entry is "<shader name>@<entry>". The mod
    // deletes the ones of its own twins that this build does not ship (another CRC in the name) and nothing else.
    constexpr std::string_view SHADER_COMPILED_CACHE_DIRS[] = {
        "cgcshaders", "cgdshaders", "cggshaders", "cghshaders", "cgpshaders", "cgvshaders",
    };

    // CShader (vtable 0x183AA44B8, RTTI below, 0x148 bytes from sub_180D9FE44). IShader slot 2 AddRef increments the
    // reference count at +0x5C and slot 1 GetID returns +0x60, the id the persistent key packs into 11 bits
    // (sub_180D9FD98 refuses ids from 0x800 on). +0x70 is the CCryNameR resource name (for example
    // "vegetation(x2)(2404800020)"), and slot 4 GetName returns the string at +0x88, the name the instance was created
    // with ("vegetation"). Slot 6 GetFlags returns +0x94, where EF_NOTFOUND (0x10000) marks a shader whose binary did
    // not load. The techniques are a TArray at +0xA8 with its count at +0xB0 (7 or 8 on a plant instance at runtime),
    // the instance's final gen mask is at +0xC0 (sub_1807E9088), and +0x128 is the template every instance of the name
    // shares. The parse runs as a render command (sub_181390424) and sets EF_LOADED (0x100) at 0x1813904D7 once it
    // returned (a failed load sets EF_NOTFOUND first, then this too). The technique count is final before that, while
    // the parse still writes each technique's pass links and flags (sub_1807C7900) and the texture-slot links, so only
    // this bit says it is done.
    constexpr ptrdiff_t SHADER_REF_COUNT_OFFSET = 0x5C;
    // CShader slot 3 (sub_180697298) adjusts this to CBaseResource and releases the retained shader reference.
    constexpr ptrdiff_t SHADER_VTABLE_RELEASE_OFFSET = 3 * 8;
    // CShaderResources primary vtable: slot 0 (sub_18093D4F0) interlocked-increments +0x10; slot 22
    // (sub_18069730C) decrements it and schedules renderer destruction on the final reference. Temporary loading
    // candidates own both shader and resources; never retain only the shader with borrowed material resources.
    constexpr ptrdiff_t SHADER_RESOURCES_VTABLE_ADD_REF_OFFSET = 0;
    constexpr ptrdiff_t SHADER_RESOURCES_VTABLE_RELEASE_OFFSET = 22 * 8;
    constexpr ptrdiff_t SHADER_ID_OFFSET = 0x60;
    constexpr ptrdiff_t SHADER_RESOURCE_NAME_OFFSET = 0x70;
    // Retail CBaseResource name lookup (sub_1806A11C8) returns the secondary subobject, then its shader callers
    // subtract 8. CShader's registry vector contains those same subobject pointers (sub_180540BA8). The container
    // has begin/end pointers at +0/+8; element i names shader id i+1. Hold the resolved resource SRWLOCK while
    // reading the vector and taking references. Only IDs 1..SHADER_MAX_ID-1 fit the renderer's packed key.
    constexpr ptrdiff_t SHADER_BASE_RESOURCE_OFFSET = 8;
    constexpr ptrdiff_t RESOURCE_CONTAINER_BEGIN_OFFSET = 0;
    constexpr ptrdiff_t RESOURCE_CONTAINER_END_OFFSET = 8;
    // This slot takes CBaseResource*, not CShader*. Native name lookup calls it before releasing the registry
    // lock. The existing CShader primary Release slot must instead receive CShader* after subtracting 8.
    constexpr ptrdiff_t BASE_RESOURCE_VTABLE_ADD_REF_OFFSET = 2 * 8;
    // The class initializer (sub_180056240) computes standard CRC32 over lowercase "CShader". This identifies a
    // resource class, not an address. Native lookup hashes the complete name and also compares its full bytes.
    constexpr uint32_t CSHADER_RESOURCE_CLASS_CRC = 0x6903B68D;
    constexpr ptrdiff_t SHADER_NAME_OFFSET = 0x88;
    constexpr ptrdiff_t SHADER_FLAGS_OFFSET = 0x94;
    constexpr uint32_t SHADER_FLAG_LOADED = 0x100;
    constexpr uint32_t SHADER_FLAG_NOT_FOUND = 0x10000;
    // EF_RELOADED: mfForName's instance lookup (sub_1807E9088 at 0x1807E93BB) sets it on an existing instance it is
    // asked to reload (EF_RELOAD in its flags, as a shader-quality change or a shader file reload does), frees its
    // techniques and queues a new parse, which RT_ParseShader runs whatever EF_LOADED says. It is never cleared, so
    // it marks only the first reload; the technique count and the gen mask (+0xC0, which any later mfForName of the
    // name rewrites) show the others, and so does a twin's feature word (SHADER_FEATURE_FLAGS_OFFSET), which its
    // reparse rebuilds from its own name without the bits it took from its source.
    constexpr uint32_t SHADER_FLAG_RELOADED = 0x4;
    constexpr ptrdiff_t SHADER_TECHNIQUE_COUNT_OFFSET = 0xB0;
    constexpr ptrdiff_t SHADER_GEN_MASK_OFFSET = 0xC0;
    constexpr uint32_t SHADER_MAX_ID = 0x800;
    // CShader's feature word (uint16 at +0x9C, vtable slot 10 returns it). The parse (sub_180883200, inside the parse
    // command before EF_LOADED is set) fills it from the shader's own defines (sub_18091B280; sub_180919140: 0x4 for
    // FEATURE_COUNT, 0x8 for DECAL_ATLAS_SIZE) and from its name: 0x80 when it is "illum", 0x100 when "humanskin"
    // (sub_180919140 at 0x1809191AF). The forward stage's PSO build (sub_1807C5A20, from sub_18071D4FC in every
    // non-shadow CreatePipelineStates) builds no forward PSO for a shader with 0x80 unless its resources ask for
    // forward shading, so an Illum twin, whose name is not "illum", would build and compile forward permutations its
    // source never has. A twin gets the bits its source has once its parse finished, before any item draws with it.
    constexpr ptrdiff_t SHADER_FEATURE_FLAGS_OFFSET = 0x9C;
    constexpr const char *CSHADER_RTTI_NAME = ".?AVCShader@@";
    constexpr const char *CSHADER_RESOURCES_RTTI_NAME = ".?AVCShaderResources@@";
    // CShaderMan::mfForName (sub_1807E8FE4: manager, name, flags, resources, gen mask) returns the CShader with a
    // reference added and queues its parse. The material path (sub_1807E8B40 -> EF_LoadShaderItem sub_1807E8CCC) calls
    // it with flags 0, the material's CShaderResources and its gen mask, which the instance creator adjusts for the
    // textures the resources hold (sub_1807E95EC) before it stores the result at +0xC0.
    constexpr uint32_t SHADER_LOAD_FLAGS_MATERIAL = 0;
    // CD3D9Renderer m_cEF, the CShaderMan (EF_LoadShaderItem's thunk sub_1807E8C80 adds 0xF00), proven by its
    // m_ShadersPath (char *) at +0x1F8, which holds "Shaders/HWScripts/" at runtime.
    constexpr ptrdiff_t RENDERER_SHADER_MAN_OFFSET = 0xF00;
    constexpr ptrdiff_t SHADER_MAN_SHADERS_PATH_OFFSET = 0x1F8;
    // CShaderMan m_nFrameForceReload (int): the engine's shader reload count. mfReloadAllShaders (every shader-quality
    // change) and mfReloadFile raise it first thing (1.5.6 sub_18250EE9C opens with inc dword ptr [rcx+228h], rcx the
    // manager); it follows m_ShadersPath and the five fields after it as in the CryEngine source.
    constexpr ptrdiff_t SHADER_MAN_FRAME_FORCE_RELOAD_OFFSET = 0x228;
    constexpr std::string_view SHADER_MAN_SHADERS_PATH = "Shaders/HWScripts/";

    // The DEV twin clone (dev/twin_clone/twin_clone.cpp, a read-only dry-run): what it reads of a stock CShader, its
    // techniques and their CHWShader_D3D objects to plan a plant or frame twin without parsing it, and of the twin the
    // engine parsed to compare the plan with. Addresses are Steam 1.5.6's, and a value given "at runtime" is what
    // stock/twin instance pairs (Vegetation, Illum and Frame) hold in the running game. Release builds read none of it.
    //
    // CShader is 0x148 bytes: sub_180D9FD98 allocates it and the ShaderLayout anchor proves the size, the reference
    // count at +0x5C, the id at +0x60 and the name at +0x88. Its constructor sub_180D9FE44 initialises every member
    // read below, from +0x80 to +0x144 (the ShaderMembers anchor). +0x80 is its file name, a char* (for example
    // "Shaders/HWScripts/CryFX/TPVCamera_Illum_3cb7eff6.cfx"), which the parse hands the HW shader factory as each
    // pass's name source. The techniques are a TArray of SShaderTechnique pointers at +0xA8 with the count at
    // SHADER_TECHNIQUE_COUNT_OFFSET.
    constexpr std::size_t SHADER_SIZE = 0x148;
    constexpr ptrdiff_t SHADER_FILE_NAME_OFFSET = 0x80;
    constexpr ptrdiff_t SHADER_TECHNIQUES_OFFSET = 0xA8;
    // The scalars a clone takes from its stock (equal between a stock and its parsed twin at runtime): the draw type
    // (uint32, 1 at runtime), the flags and flags2 above, the feature word and the uint16 after it (0 at runtime), the
    // shader type (uint32: 3 for Vegetation, 0 for Illum), the gen mask, the generator parameters (a pointer, null on
    // an instance), and the FEATURE_COUNT and DECAL_ATLAS_SIZE defines the parse stores (sub_180919140).
    constexpr ptrdiff_t SHADER_DRAW_TYPE_OFFSET = 0x90;
    constexpr ptrdiff_t SHADER_FEATURE_HIGH_OFFSET = 0x9E;
    constexpr ptrdiff_t SHADER_TYPE_OFFSET = 0xBC;
    constexpr ptrdiff_t SHADER_GEN_PARAMS_OFFSET = 0xC8;
    constexpr ptrdiff_t SHADER_FEATURE_COUNT_OFFSET = 0x13C;
    constexpr ptrdiff_t SHADER_DECAL_ATLAS_SIZE_OFFSET = 0x140;
    // Fields a clone and its parsed twin differ in by design, never compared: the vertex format byte at +0xA0 (the
    // constructor sub_180D9FE44 sets 1, mfUpdateFXVertexFormat 0x1807C77A0 raises it over the techniques' vertex
    // shaders; a clone keeps its stock's 2), the FX parameters key at +0xB8 (a clone shares its stock's, so a twin that
    // holds its stock's key is a clone, never ground truth; mfRemoveFXParams 0x180BB9168 erases the entry with the key
    // of whichever is destroyed first, so both stay referenced for the session), and the reload stamp at +0x130 (the
    // reload routine 0x1824FD840 compares it with a global counter; a clone keeps its own).
    constexpr ptrdiff_t SHADER_FX_PARAMS_KEY_OFFSET = 0xB8;
    // The two CRCs the parse copies from the shader binary (sub_180883200 at 0x1808832CF and 0x1808832D9, the
    // ShaderBinaryCrcs anchor): the header CRC at +0x138 (k_twins' source_crc on a stock instance, the family CRC its
    // name ends with on a twin) and a second one at +0x134 the mod's twin binaries leave 0 (0x9F785C3F on stock Illum,
    // 0xF439FA7E on stock Vegetation, 0 on every twin).
    constexpr ptrdiff_t SHADER_BINARY_CRC_OFFSET = 0x138;
    constexpr ptrdiff_t SHADER_SECOND_CRC_OFFSET = 0x134;
    // A float the constructor sets to FLT_MAX that nothing in RT_ParseShader's or mfForName's call tree writes (five
    // levels deep): 0 on stock instances and an Illum twin, FLT_MAX on a Frame twin, at runtime. Runtime state, so a
    // clone copies it as it is and a difference from the parsed twin is allowed.
    constexpr ptrdiff_t SHADER_RUNTIME_FLOAT_OFFSET = 0x144;
    // SShaderTechnique is 0x88 bytes (its constructor sub_18088831C, which the TechniqueLayout anchor proves): its
    // CShader at +0x00, its name at +0x08 (an interned CCryNameR: the same pointer in a stock and its twin), six HW
    // shader slots from +0x10 (the vertex shader at +0x10, the pixel shader at +0x18), flags at +0x40 (0x400000 from
    // the constructor), the render elements (pointer and count) at +0x48 and the render targets at +0x58, empty on
    // plant techniques at runtime, the name's CRC at +0x68 (0x9A74044B, "zpass", on the Z pass) with the fields up to
    // +0x77, and the pass index table at +0x7A..+0x85: the constructor writes -1 to +0x7A..+0x83 only
    // (`mov [rcx+7Ah], r14` and `mov [rcx+82h], r14w`); +0x84 and +0x85 are set elsewhere (00 or 02, then FF, at
    // runtime), so no constructor write pins those two. The two bytes at +0x78 are never initialised (the engine ORs
    // bits in at vertex shader activation, over leftover heap bytes), so they are never compared, and neither is the
    // padding after +0x85.
    constexpr std::size_t TECHNIQUE_SIZE = 0x88;
    constexpr ptrdiff_t TECHNIQUE_OWNER_OFFSET = 0x00;
    constexpr ptrdiff_t TECHNIQUE_NAME_OFFSET = 0x08;
    constexpr ptrdiff_t TECHNIQUE_SHADERS_OFFSET = 0x10;
    constexpr int TECHNIQUE_SHADER_SLOTS = 6;
    constexpr int TECHNIQUE_PIXEL_SHADER_SLOT = 1;
    constexpr ptrdiff_t TECHNIQUE_FLAGS_OFFSET = 0x40;
    constexpr std::size_t TECHNIQUE_FLAGS_SIZE = 8;
    constexpr ptrdiff_t TECHNIQUE_ELEMENTS_OFFSET = 0x48;
    constexpr std::size_t TECHNIQUE_ELEMENTS_SIZE = 0x20; // the render elements and render targets
    constexpr ptrdiff_t TECHNIQUE_CRC_OFFSET = 0x68;
    constexpr std::size_t TECHNIQUE_CRC_SIZE = 0x10;
    constexpr ptrdiff_t TECHNIQUE_INDICES_OFFSET = 0x7A;
    constexpr std::size_t TECHNIQUE_INDICES_SIZE = 0x0C;
    // CHWShader_D3D (RTTI below) is 0x138 bytes (the HW shader factory sub_1807E8740 allocates it). Four anchors prove
    // the fields below: CBaseResource's constructor sets up the resource name at +0x68 (ResourceName), CHWShader's
    // constructor sub_1807AB558 the members from +0x78 to +0xF0 (HardwareShaderMembers), and the factory stores the
    // name source, entry, class and CRC (HardwareShaderSetup) and the cache object, type and HW mask
    // (HardwareShaderCache). The factory keys it by lower("<name>(<platform>)") plus "(<HW mask in hex>)" only when the
    // HW mask is not zero, and its cache object by the name without the mask, in the classes below; a clone's pass is
    // named "<the clone's name>@<entry>". The object: its resource name at +0x68 (char*), its class at +0x78 (a byte, 0
    // vertex, 1 pixel), the name source at +0x80 (char*; the parse passes its CShader's file name), the entry at +0x88
    // (char*, "Common_ZPassPS"), the two RT masks at +0x90 and +0x98, the HW mask at +0xA0, flags at +0xC0 (0x200,
    // WASGENERATED, is set at runtime by 0x1816896FC on a cache miss, so it is never compared), the binary CRC at
    // +0xC4, the type at +0xC8, the token map at +0xD0 (a std::string: its data, or the inline buffer while the
    // capacity is at most 15, the size at +0xE0 and the capacity at +0xE8; mfStoreCacheTokenMap's layout, see
    // dev/twin_clone/shader_token_compare.hpp) and the cache object at +0xF0. The factory writes the token map only
    // under its SRWLOCK, taken exclusively (HardwareShaderLockSite).
    constexpr std::size_t HW_SHADER_SIZE = 0x138;
    constexpr ptrdiff_t HW_SHADER_KEY_OFFSET = 0x68;
    constexpr ptrdiff_t HW_SHADER_CLASS_OFFSET = 0x78;
    constexpr ptrdiff_t HW_SHADER_NAME_SOURCE_OFFSET = 0x80;
    constexpr ptrdiff_t HW_SHADER_ENTRY_OFFSET = 0x88;
    constexpr ptrdiff_t HW_SHADER_RT_AND_OFFSET = 0x90;
    constexpr ptrdiff_t HW_SHADER_RT_OR_OFFSET = 0x98;
    constexpr ptrdiff_t HW_SHADER_MASK_OFFSET = 0xA0;
    constexpr ptrdiff_t HW_SHADER_FLAGS_OFFSET = 0xC0;
    constexpr uint32_t HW_SHADER_FLAG_WAS_GENERATED = 0x200;
    constexpr ptrdiff_t HW_SHADER_CRC_OFFSET = 0xC4;
    constexpr ptrdiff_t HW_SHADER_TYPE_OFFSET = 0xC8;
    constexpr ptrdiff_t HW_SHADER_TOKENS_OFFSET = 0xD0;
    constexpr ptrdiff_t HW_SHADER_TOKENS_SIZE_OFFSET = 0xE0;
    constexpr ptrdiff_t HW_SHADER_TOKENS_CAPACITY_OFFSET = 0xE8;
    constexpr std::size_t HW_SHADER_TOKENS_INLINE_CAPACITY = 15;
    constexpr ptrdiff_t HW_SHADER_CACHE_OFFSET = 0xF0;
    constexpr const char *CHWSHADER_RTTI_NAME = ".?AVCHWShader_D3D@@";
    // The resource classes the factory files a pixel shader and its cache object under: CRC-32 of "chwshader_ps" and of
    // "chwshader_cache_ps" (sub_1807E849C and sub_1807E81F8 for class 1).
    constexpr uint32_t HW_SHADER_PS_CLASS_CRC = 0xBF287FDF;
    constexpr uint32_t HW_SHADER_CACHE_PS_CLASS_CRC = 0x2AD08B20;
    // The pixel shader entries of the Z pre-pass and Z pass, which a clone derives (the others it keeps from its
    // stock).
    constexpr std::string_view ZPREPASS_PIXEL_ENTRY = "Common_ZPrePassPS";
    constexpr std::string_view ZPASS_PIXEL_ENTRY = "Common_ZPassPS";
    // A CryStringT text the factory reads sits behind a 12-byte header (references, length, allocated; the factory
    // reads the length at text - 8, sub_1804FCF6C); the splice builds its strings that way
    // (ShaderSplice::StringHeader).
    constexpr std::size_t CRY_STRING_HEADER_SIZE = 12;

    // Retail 1.5.6 loading boundaries. CSystem's getter (sub_180860740, slot 197) reads the global state at +0x2AE0;
    // its setter is separately AOB-resolved. States 1..9 precede LOAD_END, while engine-save deserialization can run
    // later (in state 11) or on a same-level reload. SaveGameManager::LoadCryEngineData (sub_1825BD8D8, the
    // SaveGameLoad anchor) sets the byte at this+0x190 to 1 for the deserialization and back to 0 after it.
    constexpr ptrdiff_t GENV_SYSTEM_OFFSET = 0xC0;
    constexpr ptrdiff_t SYSTEM_GLOBAL_STATE_OFFSET = 0x2AE0;
    constexpr ptrdiff_t SAVE_MANAGER_ENGINE_LOADING_OFFSET = 0x190;
    // FlushRTCommands (sub_18052FB9C) reads the render-thread object at renderer+0xF8. Command dispatch
    // sub_181DE5CB0 compares GetCurrentThreadId against uint32 at that object's +0xFC for its main-thread branches.
    // RT+0xF8 is instead the level-loading thread ID (sub_1809B16F8).
    // Slot 105 is validated against the resolved FlushRTCommands entry before invoking (true,true,true).
    constexpr ptrdiff_t RENDERER_RENDER_THREAD_OFFSET = 0xF8;
    constexpr ptrdiff_t RENDER_THREAD_MAIN_THREAD_ID_OFFSET = 0xFC;
    constexpr ptrdiff_t RENDERER_FLUSH_RT_COMMANDS_VTABLE_OFFSET = 105 * 8;
    // CMatInfo GetName (sub_18113CE50) returns +0x28; AssignShaderItem (sub_1807EA460) writes SShaderItem at +0x48.
    // Read a material only while the material-load callback owns it, or on the main thread inside the loading
    // finalizer, or in a paused frame after a DEV hot reload (service_shader_twins_paused()), for a material the level
    // tables below hold (hooks/twin_inventory.cpp); walk a sub-material array only there, never during play, when a
    // material reload can rebuild it.
    constexpr ptrdiff_t MATERIAL_NAME_OFFSET = 0x28;
    constexpr ptrdiff_t MATERIAL_SHADER_ITEM_OFFSET = 0x48;
    // CMatInfo (vtable 0x1840283F0, RTTI below): GetSubMtlCount (slot 28, sub_1804846B0) reads the int 4 bytes before
    // the sub-material array at +0x70 (a DynArray of CMatInfo pointers, Constants::DYNARRAY_COUNT_BACK), and
    // GetSubMtl (sub_1804846B8) indexes it only while m_Flags (MATINFO_FLAGS_OFFSET) has MTL_FLAG_MULTI_SUBMTL; an
    // entry can be null. A material without that flag draws its own shader item.
    constexpr ptrdiff_t MATINFO_SUBMTL_ARRAY_OFFSET = 0x70;
    constexpr const char *CMATINFO_RTTI_NAME = ".?AVCMatInfo@@";

    // The level inventory (hooks/twin_inventory.cpp): the tables a level load fills with every vegetation group, brush
    // model and brush material of the level, whatever the player's distance, which the shader twins walk at a loading
    // boundary so every plant and frame variant the level can draw gets its twin there. Addresses are Steam 1.5.6's.
    //
    // C3DEngine (g_env + GENV_3DENGINE_OFFSET, RTTI below) keeps the level statobj table at +0x20 and the brush
    // material table at +0x28: each a pointer to a heap std::vector<T*> {begin, end, capacity} that CTerrain::Load_T
    // (sub_18052C81C) allocates and stores through the out-pointers the level-load timeslicer passes
    // (`lea r9,[rbx+28h]; lea r8,[rbx+20h]` at 0x1809B3074, the LevelBrushTables anchor proves both literals), after
    // it waited for its async loaders, and that the level unload frees: null at the main menu. An entry can be null.
    constexpr ptrdiff_t C3DENGINE_LEVEL_STATOBJS_OFFSET = 0x20;
    constexpr ptrdiff_t C3DENGINE_LEVEL_MATERIALS_OFFSET = 0x28;
    constexpr const char *C3DENGINE_RTTI_NAME = ".?AVC3DEngine@@";
    // CObjManager (the VegetationGroups anchor's global, RTTI below) keeps the vegetation groups (IStatInstGroup) as a
    // std::vector at +0x10/+0x18, 0x1A0 bytes each (CVegetation::GetEntityStatObj, sub_1804323B4, indexes it by the
    // node's group id; the anchor's pattern carries both literals). Load_T resizes it for the level and fills each
    // group synchronously: the statobj at +0x00, its file name inline at +0x08 (256 chars), and the group's override
    // material at +0x188 (written from the brush material table, 0x18052CF5C; null unless the level overrides it).
    constexpr ptrdiff_t OBJMAN_GROUPS_BEGIN_OFFSET = 0x10;
    constexpr ptrdiff_t OBJMAN_GROUPS_END_OFFSET = 0x18;
    constexpr std::size_t STAT_INST_GROUP_STRIDE = 0x1A0;
    constexpr ptrdiff_t STAT_INST_GROUP_STATOBJ_OFFSET = 0x00;
    constexpr ptrdiff_t STAT_INST_GROUP_FILENAME_OFFSET = 0x08;
    constexpr ptrdiff_t STAT_INST_GROUP_MATERIAL_OFFSET = 0x188;
    constexpr const char *COBJMANAGER_RTTI_NAME = ".?AVCObjManager@@";
    // CStatObj's material (GetMaterial, sub_181A72EA0, returns +0xE8). CStatObj carries no RTTI in this build; the
    // walk checks the material it names instead, and a compound sub-object by the parent's vtable.
    constexpr ptrdiff_t STATOBJ_MATERIAL_OFFSET = 0xE8;

    // ICryPak (g_env + 0x50, CCryPak, RTTI below). AdjustFileName(source, char destination[0x800], flags) is vtable
    // slot 1 (sub_180461DFC) and resolves an alias such as %USER% to the real path. FOpen(name, mode, flags),
    // FReadRaw(data, length, count, file) and FClose(file) are read from the engine's own calls (the shader-binary
    // loader sub_1809183EC and the .ext loader sub_1806A1340); they decode to slots 36, 38 and 55 on Steam 1.5.6, GOG
    // 1.5 and Game Pass 1.4.
    constexpr ptrdiff_t GENV_CRY_PAK_OFFSET = 0x50;
    constexpr const char *CRY_PAK_RTTI_NAME = ".?AVCCryPak@@";
    constexpr ptrdiff_t CRY_PAK_VTABLE_ADJUST_FILE_NAME_OFFSET = 1 * 8;
    constexpr std::size_t CRY_PAK_MAX_PATH = 0x800;
    constexpr uint32_t CRY_PAK_FLAGS_PATH_REAL = 0x10000;
    constexpr uint32_t CRY_PAK_FLAGS_FOR_WRITING = 0x800000;
    // The tokenized shader binaries. The loader (sub_1809182A8) looks a name up in the engine copy first
    // (%ENGINE%/Shaders/Cache/D3D12/, ShadersBin.pak, FOpen flags 0) and only then in the user directory on disk,
    // where the mod writes its twin. A binary opens with SShaderBinHeader: 'FXB0', the token CRC the engine checks on
    // every load at +4, the version 3 and 4 as two words at +8, 28 bytes in all.
    constexpr const char *SHADER_ENGINE_CACHE_DIR = "%ENGINE%/Shaders/Cache/D3D12/";
    constexpr const char *SHADER_USER_CACHE_DIR = "%USER%/Shaders/Cache/D3D12/";
    constexpr std::size_t SHADER_BIN_HEADER_SIZE = 28;
    constexpr uint32_t SHADER_BIN_MAGIC = 0x30425846; // 'FXB0'
    constexpr uint16_t SHADER_BIN_VERSION_LOW = 3;
    constexpr uint16_t SHADER_BIN_VERSION_HIGH = 4;

    // Tree see-through (hooks/shader_twins.cpp): the trees between the camera and the character draw with twins of the
    // stock Vegetation and Illum shaders (Illum draws the bark, trunks and decals of some trees: a willow's bark,
    // rotten and burned trees) that discard, with the stock 4x4 dither, the pixels inside a window around the character
    // and nearer than him, and the pixels close to the lens; the bare frames near the lens draw with an Illum twin that
    // discards only those. The window and the near cut ride in four rows of the main view's per-view constant buffer.
    // Addresses are Steam 1.5.6's, as for the shader twins above.
    //

    // GeneratePerViewConstantBuffer (sub_180653940, this = the graphics pipeline in rcx, the views in rdx, the view
    // count in r8d, a smart reference to the constant buffer to fill in r9, a fifth stack argument) allocates
    // count * 0x470 bytes, memsets them, fills each view and uploads them once with sub_18042DA44(buffer rcx, data rdx,
    // stride r8d = 0x470, count at [rsp+0x20]) at 0x180654E32. The pipeline is moved to r13 once at 0x1806539D1, and
    // nothing else in the function writes r13 before that call (a sweep of Steam 1.5.6, GOG 1.5 and Game Pass 1.4), so
    // the upload's mid hook reads it there. The main view's buffer is the one the pipeline's per-view holder at +0x1F0
    // keeps at +8: the main caller sub_18057278C passes a copy of it (sub_18064E1A0), and the G-buffer stage binds it
    // to slot b6 through sub_180773BB4, which reads the same +0x1F8. Fullscreen passes that build their own per-view
    // buffer (sub_18057B174, pass+0x450) upload a different buffer. sub_18057278C is the per-view update of every
    // graphics pipeline class (the standard, minimum, character-tool, billboard and mobile ones share it), so only
    // the standard pipeline's upload, the game view's, carries the window.
    constexpr ptrdiff_t PIPELINE_PER_VIEW_BUFFER_OFFSET = 0x1F8;
    constexpr const char *C_STANDARD_PIPELINE_RTTI_NAME = ".?AVCStandardGraphicsPipeline@@";
    constexpr std::size_t PER_VIEW_BUFFER_STRIDE = 0x470;
    // The upload's view count, its fifth argument, on the stack at the call.
    constexpr ptrdiff_t PER_VIEW_UPLOAD_COUNT_STACK_OFFSET = 0x20;
    // CV_ProjMatr in CBPerViewGlobal (a float4x4, registers 63 to 66 of the row-major layout, data + 0x3F0; the
    // game's FXConstantDefs.cfi declares CV_ProjMatrUnjittered after it, which ends the 0x470 bytes). The generator's
    // last store is data + 0x3EC (rdi + 0x1EC with rdi = data + 0x200), so its four rows stay the memset zeros; only
    // the debug technique DebugPostAA reads them. The twin reads the window and the near cut from them.
    constexpr ptrdiff_t PER_VIEW_HOLE_ROWS_OFFSET = 0x3F0;
    constexpr int PER_VIEW_HOLE_ROW_FLOATS = 16;
    // More views than this in one upload is not a game view; the rows are left alone.
    constexpr uint32_t PER_VIEW_MAX_VIEWS = 4;

    // A merged grass patch's render objects leave m_pRenderNode null (its persistent object at SRenderNodeTempData +
    // 0x10 included). The patch draw (sub_1804A10B0) passes AddOrCreatePersistentRenderObject an identity matrix moved
    // to the patch origin (CMergedMeshRenderNode m_pos at +0xBC), which lands in the render object's world matrix at
    // +0x108 (3x4 row-major, so the translation is at +0x114, +0x124, +0x134), so the origin names the patch.
    constexpr ptrdiff_t RENDER_OBJECT_MATRIX_OFFSET = 0x108;
    constexpr ptrdiff_t MERGED_MESH_ORIGIN_OFFSET = 0xBC;

    // Invalidating a node's persistent render object, as AddRenderObject itself does for an item that is not ready
    // (0x1804A510E): store the temp-data dword at +0xF8 into +0xFC atomically, then -1 at node + 0x38. The next frame
    // refills the object through AddRenderObject. The temp data's owner link (+0xE0) must name the node, which proves
    // the node is still the live one before anything is written.
    constexpr ptrdiff_t RENDERNODE_TEMP_DATA_OFFSET = 0x30;
    constexpr ptrdiff_t RENDERNODE_INVALIDATE_STAMP_OFFSET = 0x38;
    constexpr ptrdiff_t TEMP_DATA_STAMP_SOURCE_OFFSET = 0xF8;
    constexpr ptrdiff_t TEMP_DATA_STAMP_TARGET_OFFSET = 0xFC;

    // Render-mesh access for the occluder scan's sightline and arm tests and the hit trace. A brush's geometry is
    // reached node -> IStatObj -> IRenderMesh, and IRenderMesh::GetPosPtr returns the engine-DECODED, tightly packed
    // float3 CPU position cache (built on first FSL_READ access; the raw vertex stream is a compressed half-float
    // format). Offsets/slots are fork-specific; every read is SEH-guarded. A prop brush whose mesh cannot be read is
    // judged by its box alone on a sightline; on the arm such a structure is left to the physics.
    constexpr ptrdiff_t CBRUSH_MATRIX_OFFSET = 0x50;      // Matrix34 world transform (rotation + scale + translation)
    constexpr ptrdiff_t CBRUSH_STATOBJ_OFFSET = 0x98;     // IStatObj*
    constexpr ptrdiff_t STATOBJ_RENDERMESH_OFFSET = 0x58; // IRenderMesh* (CRenderMesh)
    // A brush's material override (IMaterial*, a CMatInfo; null when its statobj's own material is drawn) and CMatInfo
    // m_Flags (EMaterialFlags; 1.5.6 CMatInfo vtable slot 10 GetFlags is mov eax,[rcx+40h]). A brush drawn with one
    // material that is a collision proxy (MTL_FLAG_COLLISION_PROXY, set by the engine on any material named
    // "collision_proxy...") or draws nothing (MTL_FLAG_NODRAW) is invisible, for example a level designer's blocker
    // shaped from Objects/special/primitive_cube.cgf with materials/special/collision_proxy_player_only (flags
    // 0x8A022). A multi-material brush is not: a tent's proxy sub-material only shapes its physics while its other
    // sub-materials draw the canvas.
    constexpr ptrdiff_t CBRUSH_MATERIAL_OFFSET = 0x88;
    constexpr ptrdiff_t MATINFO_FLAGS_OFFSET = 0x40;
    constexpr std::uint32_t MTL_FLAG_MULTI_SUBMTL = 0x100;
    constexpr std::uint32_t MTL_FLAG_NODRAW = 0x400;
    constexpr std::uint32_t MTL_FLAG_COLLISION_PROXY = 0x2000;
    // A brush's own sub-object hide mask (the engine's per-instance hidemask, a growable bit array in this fork): the
    // pointer to its 32-bit words at +0xD0 and the word count in the low 31 bits at +0xD8 (the top bit marks a shared
    // buffer). Sub-object i is not drawn while bit i is set. 1.5.6 CBrush::Render (sub_1834974E8, CBrush vtable slot 3)
    // copies it into the render parameters, and CStatObj's sub-object render loop (sub_18049E8E0) tests word i >> 5,
    // bit i & 31; the same loop skips a child statobj whose m_nFlags (+0x178) has STATIC_OBJECT_HIDDEN. A game event
    // that shows or hides part of a model (the canvas of a tournament tent) does it through these, or by giving the
    // brush another statobj.
    constexpr ptrdiff_t CBRUSH_HIDEMASK_WORDS_OFFSET = 0xD0;
    constexpr ptrdiff_t CBRUSH_HIDEMASK_COUNT_OFFSET = 0xD8;
    constexpr std::uint32_t HIDEMASK_COUNT_MASK = 0x7FFFFFFF;
    constexpr ptrdiff_t STATOBJ_FLAGS_OFFSET = 0x178;
    constexpr std::uint32_t STATIC_OBJECT_HIDDEN = 0x1;
    // CStatObj::m_nModificationId (uint32), incremented by sub_180611338, SetRenderMesh and the stream commit, and
    // propagated to the parent and LOD0. A change invalidates copied static geometry, not a skinned pose or an instance
    // transform.
    constexpr ptrdiff_t STATOBJ_MODIFICATION_ID_OFFSET = 0x1A8;
    // Compound IStatObj sub-objects: a multi-part .cgf (open-rail fence, gate, drying rack) has a NULL root
    // IRenderMesh and stores its geometry as a std::vector<SSubObject> of child IStatObjs. The sightline test walks
    // these, so the gaps between a fence's rails stay see-through (fence_4rod_01, for example, has 2 sub-meshes of 428
    // and 2305 verts with identity transforms). The element count is (end - begin) / stride;
    // child sub-objects share the parent CStatObj vtable (checked before use). Every read is SEH-guarded +
    // in-image-screened, so a layout drift yields "no sub-meshes", never a crash.
    constexpr ptrdiff_t STATOBJ_SUBOBJ_BEGIN_OFFSET = 0x200; // std::vector<SSubObject> begin pointer
    constexpr ptrdiff_t STATOBJ_SUBOBJ_END_OFFSET = 0x208;   // std::vector<SSubObject> end pointer
    constexpr size_t SUBOBJ_STRIDE = 0xB0;                   // sizeof(IStatObj::SSubObject)
    constexpr ptrdiff_t SUBOBJ_TM_OFFSET = 0x1C;             // SSubObject local->parent Matrix34 transform
    constexpr ptrdiff_t SUBOBJ_PSTATOBJ_OFFSET = 0x80;       // SSubObject -> child IStatObj*
    constexpr int SUBOBJ_MAX = 64;                           // cap on sub-meshes walked per compound statobj
    // SSubObject bit fields (stock IStatObj.h order): bIdentityMatrix bit 0, bHidden bit 1, bShadowProxy bit 2. A
    // hidden or shadow-proxy sub-mesh is never drawn in the view; the shadow proxy draws only into the shadow maps.
    // For example, tent_big_round_b's "shadowproxy_tent_big_round_b" has 0x05 and its drawn "tent_big_round_b_Merge"
    // 0x01.
    constexpr ptrdiff_t SUBOBJ_FLAGS_OFFSET = 0xA8;
    constexpr std::uint32_t SUBOBJ_FLAG_HIDDEN = 0x2;
    constexpr std::uint32_t SUBOBJ_FLAG_SHADOW_PROXY = 0x4;
    // CryString holding the IStatObj's .cgf path (a POINTER to the path chars, dereferenced once), used to skip HLOD
    // proxies and for logging which object the camera faded or stopped at. Read SEH-guarded and sanitized to
    // printable chars, so a wrong offset / null logs harmlessly rather than faulting.
    constexpr ptrdiff_t STATOBJ_CGF_NAME_OFFSET = 0xA0;
    constexpr ptrdiff_t RENDERMESH_NVERTS_OFFSET = 0x7C; // int vertex count (== IRenderMesh::GetVerticesCount)
    // IRenderMesh::GetPosPtr(int32& stride /*rdx*/, uint32 flags /*r8d*/, int32 offset /*r9d*/) -> uint8*:
    // vtable slot 43 in this fork. With FSL_READ it returns decoded float3 positions, stride 12.
    constexpr ptrdiff_t RENDERMESH_VTABLE_GETPOSPTR_OFFSET = 43 * 8;
    constexpr unsigned int IRENDERMESH_FSL_READ = 0x01;
    constexpr int RENDER_OCCLUSION_VERT_MAX = 200000; // sanity cap on a mesh's vertex count
    // IRenderMesh::GetIndexPtr(uint32 flags /*rdx*/, int32 offset /*r8d*/) -> vtx_idx* (uint16): vtable slot 54
    // in this fork. The triangle list (3 indices/tri) lets the sightline test hit the actual SURFACE, so a low-poly
    // wall or board with no vertex near the sightline still counts.
    constexpr ptrdiff_t RENDERMESH_NINDICES_OFFSET = 0x78; // int index count (== GetIndicesCount)
    constexpr ptrdiff_t RENDERMESH_VTABLE_GETINDEXPTR_OFFSET = 54 * 8;
    constexpr int RENDER_OCCLUSION_INDEX_MAX = 3000000; // sanity cap on a mesh's index count
    // CRenderMesh::m_Chunks, a DynArray<CRenderChunk>: the pointer to its first element, preceded by the stock CryArray
    // header {int capacity, int count}, so the element count is the int 4 bytes before it (1.5.6 GetChunks is vtable
    // slot 33, lea rax,[rcx+168h]; the engine loops over it with movsxd ...,[rax-4]). CRenderChunk keeps the stock
    // IShader.h layout: 0x30 bytes, with m_nMatFlags (uint16, the flags of the material the piece is drawn with) at
    // +0x24. A piece drawn two-sided (MTL_FLAG_2SIDED) is a thin sheet seen from both sides: every canvas, cloth and
    // leather sub-material of the tents folder is two-sided, and its poles, beams, pegs and ropes are not. For example,
    // tent_big_round_b's fabric piece has 0x2A2, and the three pieces of tent_big_rectangle_a_nocloth (beam, beam
    // decal, rope) 0x2A0.
    constexpr ptrdiff_t RENDERMESH_CHUNKS_OFFSET = 0x168;
    constexpr ptrdiff_t DYNARRAY_COUNT_BACK = 4;
    constexpr size_t RENDERCHUNK_STRIDE = 0x30;
    constexpr ptrdiff_t RENDERCHUNK_MATFLAGS_OFFSET = 0x24;
    constexpr std::uint16_t MTL_FLAG_2SIDED = 0x2;
    constexpr int RENDERMESH_CHUNKS_MAX = 256; // sanity cap on a mesh's piece count

    // Camera-space interaction (door/usable look-at ray redirect)
    // The player interactor (wh::entitymodule::C_PlayerInteractor) selects the "press to use" target by
    // casting a look ray built from the GAMEPLAY view camera (sub_18091C138 -> qword_18549B4B0), which the
    // mod does NOT offset - so in third person the screen-centre crosshair and the use-target diverge
    // (you must rotate to face a door). The ray is assembled by the query builder sub_180530584(out,
    // origin /*rdx*/, dir /*r8*/, objtypes, flags, skip_ents, ...) and dispatched downstream. The
    // interaction call comes from C_PlayerInteractor look-ray builder sub_1808333C8 (which reads the
    // gameplay camera pos@+60 / rotation@+72 and forms origin@interactor+0x3E8 / dir@+0x3F4).
    //
    // k_interactionRayBuildCandidates resolves the query builder entry (the function the mod hooks).
    // k_interactorLookRayCandidates resolves the look-ray builder entry, used only to bound the call
    // site: the builder hook acts only when its return address lies inside [entry, entry +
    // INTERACTOR_LOOKRAY_SPAN), so other (camera/AI/audio) ray queries are never touched.
    constexpr size_t INTERACTOR_LOOKRAY_SPAN = 0x500; // caller-range bound for the return-address filter
    // The dir argument (r8 / a3) points at a Vec3 (x,y,z) whose length is the ray reach; the redirect rewrites
    // x,y,z to the crosshair forward and preserves the reach. The origin (rdx / a2) Vec3 is slid forward along
    // that same ray to the player-eye projection (see redirect_interaction_ray), so the engine's interaction
    // range cap is measured from ~eye rather than from the camera standing FollowDistance behind.

    // k_interactorCandidateUpdateCandidates resolves the interactor's candidate update for one interaction mask
    // (sub_1807F55FC: a1=rcx interactor, a2=rdx pose, r8d mask). The pose is the first-person eye, {Vec3 pos; Quat
    // x,y,z,w} with a level look rotation. The update gathers the usables (shrines, beds, doors, cooking spots) in a
    // box around the player and gives each the spot on its bounds nearest the pose's look segment, from the pose
    // position along the rotation's +Y. An on-screen gate projects that spot through the view camera and keeps it
    // only inside the reticle box, and the interactor picks the kept spot nearest the reticle.

    // k_segmentBoxPointCandidates resolves the closest point of a box to a segment (sub_180A867D0: a1=rcx segment
    // {Vec3 start; Vec3 end}, a2=rdx AABB, a3=r8 out Vec3). After the pick, the interactor update (sub_1807F2DD4)
    // calls it with the eye's look segment, 40 m along the level look, and the selected object's bounds. The update
    // keeps the closest point. For a cross-centered prompt (interactor flag +0x104, set for a herb) it also keeps the
    // segment end, which it re-reads from its own segment after the call, and the HUD projects that end through the
    // view camera to place the prompt. k_promptSegmentReturnCandidates resolves the return address of that one call.

    // The UI overlay hooks (k_overlayHideCandidates / k_overlayShowCandidates) bracket
    // HideOverlays / ShowOverlays. The camera reads overlay_state().active to suppress the offset
    // while an overlay (inventory, map, dialog, codex) is up.

    // The UI menu hooks (k_menuOpenCandidates / k_menuCloseCandidates) bracket the menu-open
    // (vftable[1]) and menu-close (vftable[2]) entries.

    // Generic input-event dispatcher (k_inputDispatchCandidates): void __fastcall(controller /*rcx*/,
    // event /*rdx*/, char /*r8b*/). Every input event (movement and look, FPV and TPV) funnels through
    // here. The free-look hooks it to capture the mouse-look delta and freeze look input while orbiting.
    // The event layout matches the INPUT_EVENT_* offsets below (type at INPUT_EVENT_TYPE_OFFSET ==
    // MOUSE_INPUT_TYPE_ID for mouse, id at INPUT_EVENT_ID_OFFSET, delta float at INPUT_EVENT_VALUE_OFFSET).

    // Look-axis event ids (SInputEvent.keyId / EKeyId, KCD-renumbered) matched on the analog look channel
    // together with MOUSE_INPUT_TYPE_ID below (which is actually EInputState::eIS_Changed, NOT a device
    // type - so the same gate catches mouse AND gamepad analog axes; see INPUT_EVENT_TYPE_OFFSET).
    constexpr int INPUT_LOOK_YAW_EVENT_ID = 0x10A;   // mouse horizontal look (eKI_MouseX); value = delta
    constexpr int INPUT_LOOK_PITCH_EVENT_ID = 0x10B; // mouse vertical look (eKI_MouseY); value = delta
    // Gamepad RIGHT-STICK axes (xi_thumbrx / xi_thumbry in WHGame's XInput symbol table).
    // Same eIS_Changed channel, but value at +0x18 is the analog DEFLECTION (-1..1, post-deadzone), not a
    // delta - the orbit hook latches it and the render hook integrates it by rate (GamepadOrbitSpeed X/Y deg/s).
    constexpr int INPUT_PAD_LOOK_YAW_EVENT_ID = 0x21A;   // right-stick X (horizontal)
    constexpr int INPUT_PAD_LOOK_PITCH_EVENT_ID = 0x21B; // right-stick Y (vertical)

    // Memory Offsets
    // Global-context -> camera-manager pointer. The manager is the root the game-state detection
    // walks to read the active camera (see OFFSET_ACTIVE_CAMERA below and game_state.cpp).
    constexpr ptrdiff_t OFFSET_MANAGER_PTR_STORAGE = 0x38; // Global context to camera manager
    // RTTI type-descriptor name of the camera manager, the self-heal anchor for OFFSET_MANAGER_PTR_STORAGE.
    constexpr const char *C_CAMERA_MANAGER_RTTI_NAME = ".?AVC_CameraManager@game@wh@@";

    // wh::engine3d::C_CameraObserver: the engine observer that follows the system view camera. Its update, vtable slot
    // 30, copies GetViewCamera()'s position, forward and field of view into its out-parameters. The AI selects which
    // NPCs to update in full, and which to hide and pause, from this observer, so in third person it judges from the
    // pulled-back camera instead of the eye.
    constexpr const char *C_CAMERA_OBSERVER_RTTI_NAME = ".?AVC_CameraObserver@engine3d@wh@@";
    constexpr size_t CAMERA_OBSERVER_VTABLE_UPDATE_SLOT = 30;
    // The update reads the view camera (Aob::k_cameraObserverUpdateBody). That read among its first instructions
    // confirms the slot before it is hooked.

    // Native turn-in-place animation. The free-roam locomotion action (wh::entitymodule::C_PlayerMovementAction) plays
    // its turn fragments, and takes the LockBodyTurn reference that stops the body following the look, only when the
    // actor reports third person. IActor::IsThirdPerson, for the local player, asks the active camera, which is false
    // for the first-person camera the mod keeps active. The function is shared by every actor class and answers true
    // for any actor that is not the local player. While the player's LockBodyTurn reference count is 0 the body
    // follows the look, as in first person (mounting, pickups and scripted interactions hold references too).
    // IGameObjectExtension::HandleEvent receives the SGameObjectEvent the camera manager sends when it switches the
    // active camera, and the movement action answers it, while idle, by re-reading IsThirdPerson and taking or dropping
    // its reference. The mod sends the same event when it starts or stops reporting third person, so the body follows
    // at once instead of on the next locomotion change.
    //
    // ComputeMoveState decides turn versus idle in cl every idle frame. Past the game's 35 degrees it turns, so a turn
    // stops as soon as the gap drops under it and the body rests about 35 degrees short of the look. A mid hook on that
    // `seta cl` sets the flags it reads, which lets the mod start turns at its own angle and finish them facing the
    // look. ComputeMoveState also keeps a turn-angle output pointer in r12. UpdatePending, which runs it while the
    // action is only queued behind another one (an interaction, a stagger), passes null, so r12 == 0 at the hook means
    // the player's movement is not this action's.
    //
    // The vtable slots, the event id and target/flags word, the action's field offsets, the LockBodyTurn count and the
    // hook sites are all read from the game's code at startup (aob_resolver.hpp: AnchorId::IsThirdPersonSlot onwards
    // and resolve_turn_decision_layout).
    constexpr const char *SGAME_OBJECT_EVENT_RTTI_NAME = ".?AUSGameObjectEvent@@";

    // CAnimatedCharacter movement request type values (1 absolute, 2 impulse), read by UpdatePhysicalEntityMovement
    // from the field resolve_movement_type_offset finds. An impulse's translation is a push, not a step, and the
    // turn-in-place detour never drops it.
    constexpr int32_t ANIMATED_CHARACTER_MOVEMENT_IMPULSE = 2;

    // Crouched animations. The player crouches with CROUCHED_IDLE_PLAYER_ANIMATION and turns with
    // CROUCHED_TURN_PLAYER_BLEND_SPACE: MotionIdle, MotionTurn and MotionTurnBig with the tags stealth+player in
    // kcd_male_database.adb. The right turns of that blend space break the pose late in the clip. The skeleton then
    // lands up to a meter off the body for one to four frames. The first-person game never plays a turn, so the fault
    // never shows there. Crouched NPCs play CROUCHED_IDLE_NPC_ANIMATION and CROUCHED_TURN_NPC_BLEND_SPACE, whose turns
    // hold without that fault. The player with a decoy in hand plays that NPC turn too. The two sets hold the body at
    // different stances: the NPC set carries it about 0.3 m further forward over the feet. A player idle around an NPC
    // turn therefore slides the body forward as the turn blends in and back as the idle returns, so the native turn
    // swaps the idle and the turns together (see detour_crouched_animation). These three fragments are the only
    // crouched locomotion with a player variant, and the crouched walk is shared, so the swapped set stays consistent.
    // The game looks a clip's animation up by the 64-bit hash of its name in the character's CAnimationSet
    // (GetAnimIDByCRC). The slot and the name hash are read from the game's code (AnchorId::AnimIdByCrcSlot,
    // AnchorId::AnimNameHashCall).
    constexpr const char *ANIMATION_SET_RTTI_NAME = ".?AVCAnimationSet@@";
    constexpr std::string_view CROUCHED_IDLE_PLAYER_ANIMATION = "crouched_idle_player";
    constexpr std::string_view CROUCHED_IDLE_NPC_ANIMATION = "crouched_idle";
    constexpr std::string_view CROUCHED_TURN_PLAYER_BLEND_SPACE = "1d_stealth_idle_bigturns_nw_player";
    constexpr std::string_view CROUCHED_TURN_NPC_BLEND_SPACE = "1d_stealth_idle_bigturns_nw";

    // Game-state detection (see game_state.cpp)
    // Active-camera pointer on the wh::game::C_CameraManager (the same manager reached via
    // OFFSET_MANAGER_PTR_STORAGE). The manager stores a pointer to the currently active
    // wh::game::C_Camera* subclass here; its RTTI type name identifies the camera mode. The game
    // selects this camera BEFORE the pose smoother runs, so a state read from it does not lag.
    constexpr ptrdiff_t OFFSET_ACTIVE_CAMERA = 0x30;
    // Mount is detected from the STANCE enum (C_ACTOR_MODEL_STANCE_MOUNT == 5, see below), NOT from the player's
    // LockBodyTurn reference count: mounting, an item pickup and a scripted interaction all raise that count, so it
    // cannot tell a mount from a pickup. The stance is immune (a pickup keeps stance == 1).
    // RTTI type-descriptor names of the active-camera classes that map to a tracked game state.
    // Matched by name against the active camera's vtable (ASLR/patch-safe, like CVIEW_RTTI_NAME).
    // NOTE: there is deliberately no camera entry for minigames. Only DICE swaps the active camera to
    // C_CameraMinigame; lockpicking, reading, pickpocketing, alchemy, etc. render through
    // C_CameraFirstPerson, so the active camera class cannot tell whether a minigame is on screen.
    // The minigame state is read from the C_MinigameManager instead (see OFFSET_MINIGAME_* below).
    constexpr const char *C_CAMERA_COMBAT_RTTI_NAME = ".?AVC_CameraCombatDelegate@game@wh@@";
    constexpr const char *C_CAMERA_DIALOG_RTTI_NAME = ".?AVC_CameraDialog@game@wh@@";

    // Minigame detection (see game_state.cpp poll_active_minigame)
    // Every minigame (lockpicking, dice, reading, alchemy, ...) derives from wh::playermodule::C_Minigame
    // and is owned by a single C_MinigameManager. The manager keeps the active minigames in a map keyed
    // by the owning actor's entity id; an entry owned by the player means a minigame is on screen. The
    // chain is reached from the SAME global context the camera manager hangs off:
    //   subsystem = *(context + OFFSET_MINIGAME_SUBSYSTEM)
    //   manager   = *(subsystem + OFFSET_MINIGAME_MANAGER)
    //   head      = *(manager + OFFSET_MINIGAME_MAP_HEAD)   // circular-list sentinel; empty == self-ref
    // Each list node links to the next at OFFSET_MINIGAME_NODE_NEXT and holds the I_Minigame* at
    // OFFSET_MINIGAME_NODE_VALUE; the minigame's owning C_Human/C_Player is at OFFSET_MINIGAME_OWNER.
    // The concrete minigame's RTTI then identifies WHICH minigame (see the names below).
    constexpr ptrdiff_t OFFSET_MINIGAME_SUBSYSTEM = 0x128; // global context -> minigame subsystem pointer
    // RTTI type-descriptor name of the minigame subsystem, the self-heal anchor for OFFSET_MINIGAME_SUBSYSTEM.
    constexpr const char *C_PLAYER_MODULE_RTTI_NAME = ".?AVC_PlayerModule@playermodule@wh@@";
    constexpr ptrdiff_t OFFSET_MINIGAME_MANAGER = 0x18;    // subsystem -> C_MinigameManager (getter inlined)
    constexpr ptrdiff_t OFFSET_MINIGAME_MAP_HEAD = 0x20;   // manager -> minigame-map sentinel node pointer
    constexpr ptrdiff_t OFFSET_MINIGAME_NODE_NEXT = 0x00;  // list node -> next node (sentinel terminates)
    constexpr ptrdiff_t OFFSET_MINIGAME_NODE_VALUE = 0x28; // list node -> I_Minigame* (the active minigame)
    constexpr ptrdiff_t OFFSET_MINIGAME_OWNER = 0x18;      // I_Minigame -> owning C_Human/C_Player

    // RTTI type-descriptor names of the concrete minigames, matched against the active minigame's vtable.
    // The trailing comment is the engine's E_MenuType factory index (kept for reference only).
    constexpr const char *C_MINIGAME_SHARPENING_RTTI_NAME = ".?AVC_Sharpening@playermodule@wh@@";        // 1
    constexpr const char *C_MINIGAME_READING_RTTI_NAME = ".?AVC_Reading@playermodule@wh@@";              // 2
    constexpr const char *C_MINIGAME_ALCHEMY_RTTI_NAME = ".?AVC_Alchemy@playermodule@wh@@";              // 3
    constexpr const char *C_MINIGAME_HERB_GATHERING_RTTI_NAME = ".?AVC_HerbGathering@playermodule@wh@@"; // 4
    constexpr const char *C_MINIGAME_LOCKPICKING_RTTI_NAME = ".?AVC_LockPicking@playermodule@wh@@";      // 5
    constexpr const char *C_MINIGAME_HOLE_DIGGING_RTTI_NAME = ".?AVC_HoleDigging@playermodule@wh@@";     // 6
    constexpr const char *C_MINIGAME_DICE_RTTI_NAME = ".?AVC_Dice@playermodule@wh@@";                    // 7
    constexpr const char *C_MINIGAME_PICKPOCKETING_RTTI_NAME = ".?AVC_Pickpocketing@playermodule@wh@@";  // 8
    constexpr const char *C_MINIGAME_STONE_THROWING_RTTI_NAME = ".?AVC_StoneThrowing@playermodule@wh@@"; // 9
    constexpr const char *C_MINIGAME_BATTLE_ARCHERY_RTTI_NAME = ".?AVC_BattleArchery@playermodule@wh@@"; // 10
    constexpr const char *C_MINIGAME_DISTRACT_RTTI_NAME = ".?AVC_Distract@playermodule@wh@@";            // 11
    constexpr const char *C_MINIGAME_BLACKSMITHING_RTTI_NAME = ".?AVC_Blacksmithing@playermodule@wh@@";  // 12
    constexpr const char *C_MINIGAME_FORGE_BUILDER_RTTI_NAME = ".?AVC_ForgeBuilder@playermodule@wh@@";   // 13

    // Combat lock-on. The player's lock-on runs in the combat module, which hangs off the SAME global context as the
    // camera manager and the minigame subsystem:
    //   module     = *(context + OFFSET_COMBAT_MODULE)                // wh::combatmodule::C_CombatModule
    //   player     = *(module + COMBAT_MODULE_PLAYER_OFFSET)          // C_CombatPlayer, the player's combat actor
    //   controller = *(player + COMBAT_PLAYER_CONTROLLER_OFFSET)      // C_CombatPlayerController, the lock logic
    //   input      = *(controller + COMBAT_CONTROLLER_INPUT_OFFSET)   // C_CombatPlayerInput, mouse/pad + view steer
    //   state      = *(player + COMBAT_ACTOR_STATE_OFFSET)            // the combat actor's state block
    //   opponent   = *(state + COMBAT_STATE_OPPONENT_OFFSET)          // the opponent's combat actor, 0 for none
    //   target     = *(state + COMBAT_STATE_TARGET_OFFSET)            // C_CombatTarget, the opponent-to-be, 0 for none
    // The state block has no vtable of its own: it opens with the combat-mode model property (a C_ModelProperty<bool>),
    // whose getter (vtable slot 1) returns the byte at COMBAT_STATE_STANCE_OFFSET. That byte is the combat stance; the
    // controller's IsLocked (vtable slot 3) is that stance with an opponent selected.
    // The game's set-opponent (AnchorId::SetCombatOpponent, sub_180B21384) is the only writer of the target
    // (sub_180B214BC): a fresh C_CombatTarget holding the CEntity at COMBAT_TARGET_ENTITY_OFFSET (its vtable slot 1
    // returns it), or 0. It moves the opponent to the target at once only when no attack or finishing move runs
    // (sub_1809D32E4); otherwise the game's commit (sub_180A518F4) moves it as soon as that move ends.
    // The opponent's combat actor points at its entity-module actor (C_Human or C_Player) at COMBAT_ACTOR_HUMAN_OFFSET,
    // whose EntityId is the dword at HUMAN_ENTITY_ID_OFFSET (the local player reads 0x7777) and whose CEntity sits at
    // C_PLAYER_ENTITY_OFFSET, as on the player. The controller's unlock test compares the angle between the player
    // ENTITY's forward and the direction to the opponent with wh_cs_PlayerMaxOpponentAngleToUnlock, while its target
    // pick measures from the render camera's forward: a camera turned far off the body can pick an opponent the body
    // test then drops on the next update.
    // Death: C_Actor::IsDead (vtable slot 38) asks the actor's C_Soul at HUMAN_SOUL_OFFSET when there is one (C_Soul's
    // own IsDead, slot 6, is its health at SOUL_HEALTH_OFFSET <= 0) and the actor's health at HUMAN_HEALTH_OFFSET
    // otherwise. The killing blow sets it, before the death or finishing animation plays out.
    constexpr ptrdiff_t OFFSET_COMBAT_MODULE = 0xF8;
    // C_CombatPlayer's per-frame update: it runs the lock-on controller (the combat-player update the kill retarget
    // hooks, so the game's own set-opponent is called on the thread that calls it).
    constexpr size_t COMBAT_PLAYER_VTABLE_UPDATE_SLOT = 101;
    constexpr const char *C_COMBAT_MODULE_RTTI_NAME = ".?AVC_CombatModule@combatmodule@wh@@";
    constexpr ptrdiff_t COMBAT_MODULE_PLAYER_OFFSET = 0x28;
    constexpr const char *C_COMBAT_PLAYER_RTTI_NAME = ".?AVC_CombatPlayer@combatmodule@wh@@";
    constexpr ptrdiff_t COMBAT_PLAYER_CONTROLLER_OFFSET = 0x450;
    constexpr const char *C_COMBAT_PLAYER_CONTROLLER_RTTI_NAME = ".?AVC_CombatPlayerController@combatmodule@wh@@";
    constexpr ptrdiff_t COMBAT_CONTROLLER_INPUT_OFFSET = 0x28;
    constexpr const char *C_COMBAT_PLAYER_INPUT_RTTI_NAME = ".?AVC_CombatPlayerInput@combatmodule@wh@@";
    constexpr ptrdiff_t COMBAT_ACTOR_STATE_OFFSET = 0x210;
    constexpr ptrdiff_t COMBAT_STATE_STANCE_OFFSET = 0x8;
    constexpr ptrdiff_t COMBAT_STATE_OPPONENT_OFFSET = 0xCF8;
    constexpr ptrdiff_t COMBAT_STATE_TARGET_OFFSET = 0xCF0;
    constexpr const char *C_COMBAT_TARGET_RTTI_NAME = ".?AVC_CombatTarget@combatmodule@wh@@";
    constexpr ptrdiff_t COMBAT_TARGET_ENTITY_OFFSET = 0x8;
    constexpr ptrdiff_t COMBAT_ACTOR_HUMAN_OFFSET = 0x1F8;
    constexpr ptrdiff_t HUMAN_ENTITY_ID_OFFSET = 0x30;
    constexpr ptrdiff_t HUMAN_SOUL_OFFSET = 0x668;
    constexpr ptrdiff_t HUMAN_HEALTH_OFFSET = 0x164;
    constexpr ptrdiff_t SOUL_HEALTH_OFFSET = 0x708;
    constexpr const char *C_SOUL_RTTI_NAME = ".?AVC_Soul@rpgmodule@wh@@";
    // The controller's timers. Each is an int64 deadline on the game clock (ITimer::GetFrameStartTime(ETIMER_GAME), in
    // 1/100000 s), set by the controller as now + duration, and parked at or below COMBAT_TIMER_UNARMED when unarmed.
    //   DELAYED_UNLOCK: armed when the player changes target. While it runs, the controller's pick takes the hostile
    //     the camera points at even when he stands farther off than the opponent (sub_1809D411C); when it runs out
    //     the controller re-picks an opponent and drops the lock if it finds none (sub_1809D4244).
    //   DELAYED_END: armed by the kill event (wh_cs_PlayerInputCombatDelayAfterKill, or ...WithOtherOpponent when a
    //     threat is near). The controller reads it unarmed as run out (sub_180731070 returns true at or below
    //     COMBAT_TIMER_UNARMED). While it reads run out, the stance ends on the update the opponent fails the threat
    //     test below (sub_1809D30C0, with further gates on the player's own state), and the timer is parked unarmed.
    //     So outside the delay after a kill the stance lasts only while the opponent is a threat.
    // The threat test (AnchorId::CombatThreat, sub_18082A334): bool(source, C_CombatActor *other), with source the
    // object at the player's combat actor + COMBAT_PLAYER_THREAT_SOURCE_OFFSET. It compares the two actors' souls
    // through the soul relations. The stance-end rule asks it of the opponent (sub_181418724), and the controller's
    // pick asks it of every listed candidate and skips those that fail (sub_180686A50), so a listed hostile who is not
    // fighting the player is never picked. A C_Human names its combat actor at HUMAN_COMBAT_ACTOR_OFFSET, the link the
    // selector list is built through (sub_1808315BC).
    constexpr ptrdiff_t COMBAT_PLAYER_THREAT_SOURCE_OFFSET = 0x388;
    constexpr ptrdiff_t HUMAN_COMBAT_ACTOR_OFFSET = 0x278;
    // The controller's opponent selector (no RTTI of its own): it starts with the player's combat actor and holds the
    // combat actors the pick chooses from as a vector<C_CombatActor *> (begin, end). sub_180686A50 walks it.
    constexpr ptrdiff_t COMBAT_CONTROLLER_SELECTOR_OFFSET = 0x20;
    constexpr ptrdiff_t COMBAT_SELECTOR_CANDIDATES_BEGIN_OFFSET = 0x8;
    constexpr ptrdiff_t COMBAT_SELECTOR_CANDIDATES_END_OFFSET = 0x10;
    // The game's hard lock, a byte: the opponent_lock action (Tab) sets it and cycles the opponent (sub_1827C3640), and
    // the unlock action (middle mouse) clears it (sub_180D4A768). While set, the view never leaves the opponent for the
    // mouse (sub_1808BAE70), a mouse flick moves the lock to the next opponent that way (sub_1808BA62C), and the
    // camera's pick does not move the lock (sub_1809D411C).
    constexpr ptrdiff_t COMBAT_CONTROLLER_HARD_LOCK_OFFSET = 0xC0;
    constexpr ptrdiff_t COMBAT_CONTROLLER_DELAYED_UNLOCK_OFFSET = 0xA8;
    constexpr ptrdiff_t COMBAT_CONTROLLER_DELAYED_END_OFFSET = 0xB8;
    constexpr std::int64_t COMBAT_TIMER_UNARMED = -100000;
    constexpr double COMBAT_TIMER_TICKS_PER_SECOND = 100000.0;
    // The input's free-look bytes. A fast mouse (or right-stick) sweep sets the byte and the view steer toward the
    // opponent stops; the byte clears once the input has rested for wh_cs_PlayerInputMouseUnlockReturnTime (or the
    // gamepad twin), and the steer swings the view back. Both bytes also read 1 whenever the steer is not running at
    // all (outside the stance).
    constexpr ptrdiff_t COMBAT_INPUT_FREE_MOUSE_OFFSET = 0xF4;
    constexpr ptrdiff_t COMBAT_INPUT_FREE_PAD_OFFSET = 0x120;
    // The input's own view while the lock-on runs (its view update, sub_180830BCC). It
    // copies the player's look into these while the lock is off and stops copying once it is on: the steer then slerps
    // VIEW_BASE (Quat, x y z w) toward the opponent into VIEW (Quat), and VIEW_ANGLES (Ang3 x = pitch, y = roll,
    // z = yaw, radians) is taken from VIEW; mouse or right-stick free look adds the input to VIEW_ANGLES and turns the
    // player's body to its yaw and the look to it. VIEW_ANGLES_RESET is the copy taken with VIEW_ANGLES on that same
    // hand-over. A look or body turned by anyone else is therefore undone by the next steer or free-look frame unless
    // these four agree with it. VIEW_BASE and VIEW are rebuilt from VIEW_ANGLES with Quat::SetRotationXYZ
    // (sub_18053F6C0).
    constexpr ptrdiff_t COMBAT_INPUT_VIEW_BASE_OFFSET = 0x4C;
    constexpr ptrdiff_t COMBAT_INPUT_VIEW_OFFSET = 0x5C;
    constexpr ptrdiff_t COMBAT_INPUT_VIEW_ANGLES_OFFSET = 0x6C;
    constexpr ptrdiff_t COMBAT_INPUT_VIEW_ANGLES_RESET_OFFSET = 0x78;

    // Aiming a missile weapon (bow/crossbow). The player's C_MissileWeaponPlayerController is an
    // input action-map listener constructed inside the C_Player constructor, so it is EMBEDDED in
    // C_Player at a fixed offset (always present, and player-only); it is reached by adding the
    // offset, not dereferencing a pointer. The AIM flag is the single BYTE at
    // MISSILE_CONTROLLER_AIM_FLAG_OFFSET: 1 while the player is aiming/drawing, 0 otherwise. It MUST
    // be read as a byte, not a dword: the surrounding bytes pack a separate "weapon in hand" flag at
    // +0x22 (set whenever the weapon is drawn), so a dword read would also fire when merely holding
    // the weapon. In hand not aiming = 0, raised/aiming = 1.
    constexpr ptrdiff_t C_PLAYER_MISSILE_CONTROLLER_OFFSET = 0xD70;
    constexpr ptrdiff_t MISSILE_CONTROLLER_AIM_FLAG_OFFSET = 0x20;
    // RTTI type-descriptor name of the embedded controller, used to validate the layout before the
    // flag read (a drift in the offset then yields "not aiming" rather than a garbage read).
    constexpr const char *C_MISSILE_CONTROLLER_RTTI_NAME = ".?AVC_MissileWeaponPlayerController@entitymodule@wh@@";
    // The "weapon in hand" byte next to the aim flag: 1 whenever a missile weapon is drawn, aiming or not.
    constexpr ptrdiff_t MISSILE_CONTROLLER_IN_HAND_FLAG_OFFSET = 0x22;

    // The distraction minigame (crouch, then hold the distract key): C_Distract holds a stone ready in the hand as a
    // C_Decoy projectile and launches it on release from where it sits, along the look turned about the vertical by
    // atan(DISTRACT_HAND_OFFSET / range), so a stone thrown from beside the eye comes down on the look line at the
    // range the throw covers back down to the feet. DISTRACT_HAND_OFFSET is the game's wh_pl_DistractHandOffset
    // default.
    constexpr ptrdiff_t C_DISTRACT_STATE_OFFSET = 0x78; // E_State: 2 taking the stone, 3 held ready, 6 putting it back
    constexpr int DISTRACT_STATE_HOLDING = 3;
    constexpr ptrdiff_t C_DISTRACT_DECOY_OFFSET = 0xA0; // C_Decoy* in the hand, 0 when none
    constexpr float DISTRACT_HAND_OFFSET = 0.25f;
    constexpr const char *C_DECOY_RTTI_NAME = ".?AVC_Decoy@entitymodule@wh@@";

    // The player's C_PlayerInput, a pointer on C_Player, holds the game's own record of the movement the keys ask for:
    // a body-relative vector, x strafe (left negative) and y forward, 1 while a key is held and 0 once it is released.
    constexpr ptrdiff_t C_PLAYER_INPUT_OFFSET = 0xB88;
    constexpr ptrdiff_t PLAYER_INPUT_MOVE_X_OFFSET = 0x58;
    constexpr ptrdiff_t PLAYER_INPUT_MOVE_Y_OFFSET = 0x5C;
    constexpr const char *C_PLAYER_INPUT_RTTI_NAME = ".?AVC_PlayerInput@entitymodule@wh@@";

    // The focused look (the focus key turns the view onto what the player looks at, with a slight zoom). C_Player holds
    // its controller through a pointer whose offset, and the offset of the controller's flags word (uint32), come from
    // game code (AnchorId::FocusController, AnchorId::FocusFlags). The focus start sets FOCUSED_LOOK_FLAG_ACTIVE (its
    // `or eax, 2`), and the focus stop clears it. While it is set the game turns the look every frame by part of the
    // angle between the rendered camera's forward and the target, measured from the rendered camera, so the focus
    // settles only where that camera follows the look.
    constexpr std::uint32_t FOCUSED_LOOK_FLAG_ACTIVE = 0x2;

    // The aim flag is the shoot request: crossbow prepare raises it at once, so it is also set through the whole
    // reload (spanning a bolt) before the weapon comes up. The running shooting sequence tells the phases apart:
    // C_Player -> C_ActionActor (C_PLAYER_ACTION_ACTOR_OFFSET) -> its component map (ACTION_ACTOR_COMPONENT_MAP_OFFSET,
    // an MSVC std::map<uint8, component*>) -> C_ActorShootingExpansion -> the running C_ActorActionShootingMain
    // (SHOOTING_EXPANSION_MAIN_OFFSET, null when idle) -> its current sub-action (SHOOTING_MAIN_SUBACTION_OFFSET),
    // which is C_ActorActionShootingReloading or ...Unloading while a bolt goes in or out. The game's own shooting code
    // reads the same offsets. The shooting expansion, the running main action and its sub-action are each checked by
    // RTTI; the C_ActionActor and the map header are plausibility-checked pointers.
    constexpr ptrdiff_t C_PLAYER_ACTION_ACTOR_OFFSET = 0x280;
    constexpr ptrdiff_t ACTION_ACTOR_COMPONENT_MAP_OFFSET = 0x38; // std::map header: the head (sentinel) node pointer
    constexpr ptrdiff_t SHOOTING_EXPANSION_MAIN_OFFSET = 0x50;
    // The weapon and the ammo the shooting expansion holds ready (the nocked arrow or the loaded bolt), C_Items.
    constexpr ptrdiff_t SHOOTING_EXPANSION_WEAPON_OFFSET = 0x40;
    constexpr ptrdiff_t SHOOTING_EXPANSION_AMMO_OFFSET = 0x48;
    constexpr ptrdiff_t SHOOTING_MAIN_SUBACTION_OFFSET = 0x98;
    // MSVC std::map node: left, parent and right links, the sentinel byte, then the stored pair (a one-byte key, so a
    // pointer value lands at +0x28).
    constexpr ptrdiff_t MSVC_MAP_NODE_LEFT_OFFSET = 0x00;
    constexpr ptrdiff_t MSVC_MAP_NODE_PARENT_OFFSET = 0x08;
    constexpr ptrdiff_t MSVC_MAP_NODE_RIGHT_OFFSET = 0x10;
    constexpr ptrdiff_t MSVC_MAP_NODE_ISNIL_OFFSET = 0x19;
    constexpr ptrdiff_t MSVC_MAP_NODE_POINTER_VALUE_OFFSET = 0x28;
    constexpr const char *C_SHOOTING_EXPANSION_RTTI_NAME = ".?AVC_ActorShootingExpansion@entitymodule@wh@@";
    constexpr const char *C_SHOOTING_MAIN_RTTI_NAME = ".?AVC_ActorActionShootingMain@entitymodule@wh@@";
    constexpr const char *C_SHOOTING_RELOADING_RTTI_NAME = ".?AVC_ActorActionShootingReloading@entitymodule@wh@@";
    constexpr const char *C_SHOOTING_UNLOADING_RTTI_NAME = ".?AVC_ActorActionShootingUnloading@entitymodule@wh@@";
    // The shot's phase while the bow is held fully drawn or the crossbow raised, ready to loose.
    constexpr const char *C_SHOOTING_AIMING_RTTI_NAME = ".?AVC_ActorActionShootingAiming@entitymodule@wh@@";

    // Ranged shots. The player's shot launches from the bow's "arrow" joint along that joint's axis, never along the
    // camera, so in third person it misses the screen center by the shoulder parallax. The ranged hook re-aims the
    // shot as the first-person shot at the point the crosshair ray hits. CProjectile::Launch(this, pos, dir,
    // velocity, speedScale) orients the entity from dir but flies it on velocity * speedScale (a pe_params_particle
    // with gravity and no drag for arrows), so both are rewritten. Field offsets match stock GameSDK CProjectile.
    // An actor's (and a projectile's) own EntityId, the game object extension's entity id. FireProjectile reads the
    // shooter's here for the projectile owner; the local player's is the stock LOCAL_PLAYER_ENTITY_ID 0x7777.
    constexpr ptrdiff_t ACTOR_ENTITY_ID_OFFSET = 0x30;
    constexpr ptrdiff_t PROJECTILE_ENTITY_OFFSET = 0x38;      // IEntity* (a CEntity)
    constexpr ptrdiff_t PROJECTILE_AMMO_PARAMS_OFFSET = 0x68; // const SAmmoParams*
    constexpr ptrdiff_t PROJECTILE_FLAGS_OFFSET = 0xA0;       // uint32 ePFlag_* bits
    constexpr ptrdiff_t PROJECTILE_POSITION_OFFSET = 0xA4;    // Vec3, where the projectile sits now
    constexpr ptrdiff_t PROJECTILE_INITIAL_POS_OFFSET = 0xB0; // Vec3 m_initial_pos, written by Launch
    constexpr ptrdiff_t PROJECTILE_FLIGHT_TIME_OFFSET = 0xE0; // float seconds since Launch
    constexpr std::uint32_t PROJECTILE_FLAG_COLLIDED = 0x1;   // ePFlag_collided: the flight has ended
    constexpr std::uint32_t PROJECTILE_FLAG_DESTROYING = 0x8; // ePFlag_destroying
    constexpr std::uint32_t PROJECTILE_FLAG_HIDDEN = 0x400;   // ePFlag_needDestruction: hidden, awaiting the pool
    // SAmmoParams launch speed (m/s), the speed a thrown distraction stone leaves the hand at.
    constexpr ptrdiff_t AMMO_SPEED_OFFSET = 0x58;
    // SAmmoParams -> pe_params_particle template (physinterface.h layout) the arrow's physics is created from.
    constexpr ptrdiff_t AMMO_PARTICLE_PARAMS_OFFSET = 0x88;
    constexpr ptrdiff_t PARTICLE_PARAMS_AIR_RESISTANCE_OFFSET = 0x24; // float kAirResistance
    constexpr ptrdiff_t PARTICLE_PARAMS_GRAVITY_OFFSET = 0x44;        // Vec3 gravity
    constexpr ptrdiff_t PARTICLE_PARAMS_PIERCEABILITY_OFFSET = 0x98;  // int iPierceability
    // Used when the ammo template leaves a field unset (the physics "unused" NaN marker) or reads implausibly: a
    // particle then keeps the world gravity, and 13 is the pierceability the arrow's collision handler starts from.
    constexpr float PARTICLE_DEFAULT_GRAVITY = 9.81f;
    constexpr int ARROW_DEFAULT_PIERCEABILITY = 13;
    // The projectile's own physics, the CParticleEntity whose velocity Launch sets. Its gravity is changed the way
    // Launch changes the velocity: IPhysicalEntity::SetParams(pe_params_particle*, bThreadSafe) with every other field
    // left at the physics "unused" marker (a float's bit pattern, an int's value), so only the gravity is written.
    constexpr ptrdiff_t PROJECTILE_PHYSICS_OFFSET = 0x70;
    constexpr ptrdiff_t PHYS_ENTITY_VTABLE_SET_PARAMS_OFFSET = 4 * 8;
    constexpr int PE_PARAMS_PARTICLE_TYPE = 3; // ePE_params_particle
    constexpr std::uint32_t PHYS_UNUSED_FLOAT_BITS = 0xFFBFFFFFu;
    constexpr std::uint32_t PHYS_UNUSED_INT = 0x80000000u;
    constexpr const char *C_PARTICLE_ENTITY_RTTI_NAME = ".?AVCParticleEntity@@";
    // What a shot is: FireProjectile's ammo item (a C_Item) -> its class data (the game's own item class lookup) -> the
    // S_AmmoItemClass ammo class, the item table's Ammo Type. Every bow takes arrows, every crossbow bolts, and the
    // handgonne a ball or scatter shot.
    constexpr ptrdiff_t ITEM_CLASS_OFFSET = 0x48;
    constexpr ptrdiff_t AMMO_ITEM_CLASS_TYPE_OFFSET = 0xF0;
    constexpr std::uint32_t AMMO_CLASS_ARROW = 1;
    constexpr std::uint32_t AMMO_CLASS_BOLT = 2;
    constexpr std::uint32_t AMMO_CLASS_BALL = 3;
    constexpr std::uint32_t AMMO_CLASS_SCATTER_SHOT = 4;
    constexpr const char *S_AMMO_ITEM_CLASS_RTTI_NAME = ".?AUS_AmmoItemClass@entitymodule@wh@@";
    // EventPhysCollision fields the impact hook reads: the stock CryEngine layout, the one the arrow's own collision
    // handler reads.
    constexpr ptrdiff_t PHYS_COLLISION_FOREIGN_DATA_OFFSET = 0x20; // void* pForeignData[2] (an IEntity* for an entity)
    constexpr ptrdiff_t PHYS_COLLISION_FOREIGN_ID_OFFSET = 0x30;   // int iForeignData[2]
    constexpr ptrdiff_t PHYS_COLLISION_POINT_OFFSET = 0x3C;        // Vec3 pt
    // CEntity's name (const char*), logged for what an arrow hit.
    constexpr ptrdiff_t ENTITY_NAME_OFFSET = 0xE0;
    // The crosshair target ray mirrors the arrow particle's own collision ray (CParticleEntity::Step): every entity
    // class but water, the arrow's pierceability (a surface MORE pierceable than it is flown through), and the ray and
    // foliage-proxy collide types, with non-colliding geometry and solid back faces passed through.
    constexpr int RWI_OBJTYPES_ARROW = 0x11F;                               // ent_all (static|rigid|living|terrain...)
    constexpr unsigned int RWI_ARROW_COLLTYPES = (0x8000u | 0x2000u) << 16; // geom_colltype_ray | geom_colltype13
    constexpr unsigned int RWI_IGNORE_SOLID_BACK_FACES = 0x100;
    constexpr unsigned int RWI_FORCE_PIERCEABLE_NONCOLL = 0x1000;
    constexpr unsigned int RWI_FLAGS_ARROW_BASE =
        RWI_ARROW_COLLTYPES | RWI_COLLTYPE_ANY | RWI_FORCE_PIERCEABLE_NONCOLL | RWI_IGNORE_SOLID_BACK_FACES;
    constexpr int RWI_OBJTYPE_LIVING = 0x8; // ent_living (actor and animal capsules)
    // How far the crosshair target ray and the landing trace reach. On a miss the point this far along the crosshair
    // ray stands in for the crosshair point.
    constexpr float RANGED_TARGET_RANGE = 300.0f;
    // While mounted, a living body the crosshair ray hits within this many meters of the rider's eye, with the rider
    // inside its world box, is the rider's own horse: the ray skips it and looks on behind it.
    constexpr float RANGED_MOUNT_CLEARANCE = 3.5f;

    // Aux geometry (IRenderAuxGeom), used to draw the arrow flight trail. g_env->pRenderer is at GENV_RENDERER_OFFSET;
    // its GetIRenderAuxGeom slot returns the calling thread's CAuxGeomCB. Each slot is validated against the function
    // its AOB anchor resolved before it is called. The trail flags are 3D, alpha blended, depth write off and depth
    // test off, so the trail shows through walls (the vendored IRenderAuxGeom.h bit layout).
    constexpr ptrdiff_t GENV_RENDERER_OFFSET = 0x108;
    constexpr ptrdiff_t RENDERER_VTABLE_GET_AUX_GEOM_OFFSET = 199 * 8;
    constexpr ptrdiff_t AUX_VTABLE_SET_RENDER_FLAGS_OFFSET = 1 * 8; // returns the flags it replaced
    constexpr ptrdiff_t AUX_VTABLE_DRAW_LINES_OFFSET = 13 * 8;
    constexpr std::uint32_t AUX_TRAIL_RENDER_FLAGS = 0x20600000;
    // The aim preview's flags: the same with the depth test on, so the scene hides it behind walls like real geometry.
    constexpr std::uint32_t AUX_PREVIEW_RENDER_FLAGS = 0x20400000;
    constexpr const char *C_RENDERER_RTTI_NAME = ".?AVCD3D9Renderer@@";
    constexpr const char *C_AUX_GEOM_RTTI_NAME = ".?AVCAuxGeomCB@@";

    // Crouch/sneak AND mount BOTH come from the player's STANCE enum. C_ActorModel is a POINTER on
    // C_Player, dereferenced then validated by its RTTI. The 4-byte CURRENT STANCE enum lives at +0x80 -
    // NOT +0x8, which is a 64-bit pointer whose low dword is never 1 (reading the stance there would make
    // crouch read false). Written by the virtual SetStance (C_ActorModel vtable +0x220): STAND = 1,
    // MOUNTED = 5, CROUCH/sneak = 6. (A transient 5 also appears one
    // frame mid stand<->crouch switch; the GameState debounce filters that blip, so a HELD 5 = mounted.)
    constexpr ptrdiff_t C_PLAYER_ACTOR_MODEL_OFFSET = 0x990;
    constexpr ptrdiff_t C_ACTOR_MODEL_STANCE_OFFSET = 0x80;
    // wh::entitymodule::E_StanceCategory::Type (RTTR reg sub_1800ED010; 8 contiguous
    // values). 0 = undefined and 1 = standing carry no GameState bit (standing is the DEFAULT preset), so
    // only the stances below map to bits.
    constexpr unsigned int C_ACTOR_MODEL_STANCE_LYING = 2u;   // lying down (sleeping in bed)
    constexpr unsigned int C_ACTOR_MODEL_STANCE_SITTING = 3u; // sitting (bench / chair)
    constexpr unsigned int C_ACTOR_MODEL_STANCE_KNEEL = 4u;   // kneeling
    constexpr unsigned int C_ACTOR_MODEL_STANCE_MOUNT = 5u;   // horse (mounted)
    constexpr unsigned int C_ACTOR_MODEL_STANCE_CROUCH = 6u;  // crouch / sneak / stealth
    constexpr unsigned int C_ACTOR_MODEL_STANCE_CART = 7u;    // riding / driving a cart
    constexpr const char *C_ACTOR_MODEL_RTTI_NAME = ".?AVC_ActorModel@entitymodule@wh@@";

    // CEntity world transform member (relative to the CEntity* base): Matrix34, translation in column 3.
    constexpr ptrdiff_t OFFSET_ENTITY_WORLD_MATRIX_MEMBER = 0x58;

    // Fields inside the CView object that the third-person camera reads.
    // SVIEWPARAMS_POSITION_OFFSET is the eye-frame world position (untouched, used as the
    // offset anchor so a second same-frame frustum rebuild cannot double-offset).
    // SVIEWPARAMS_ROTATION_OFFSET is the eye orientation Quat (XYZW): CView::Update reads and
    // normalizes it, then BUILDS the render matrix from it before the frustum builder runs, so
    // it is the canonical untouched rotation input (deriving the eye basis from this quat rather
    // than from the matrix columns we overwrite keeps the offset idempotent across same-frame
    // rebuilds; matrix col0 == quat*+X, col1 == quat*+Y, col2 == quat*+Z).
    // SVIEWPARAMS_VIEWMATRIX_OFFSET is the start of the embedded render CCamera: a 3x4 Matrix34
    // (rotation basis in columns, translation in column 3). The frustum builder is called with a
    // pointer to this embedded camera, so CView = camera - SVIEWPARAMS_VIEWMATRIX_OFFSET.
    constexpr ptrdiff_t SVIEWPARAMS_POSITION_OFFSET = 0x14;   // Vec3 camera world eye position
    constexpr ptrdiff_t SVIEWPARAMS_ROTATION_OFFSET = 0x20;   // Quat (x,y,z,w) eye orientation
    constexpr ptrdiff_t SVIEWPARAMS_VIEWMATRIX_OFFSET = 0xE8; // embedded render CCamera (3x4 matrix at +0)

    // Render CCamera internals that SetFrustum (sub_1805392FC) writes right before the frustum builder
    // runs, read and rewritten by the per-preset FOV override and the lens near plane (see camera_hook
    // offset_game_view_camera). The projection FOV scalar (radians) is at +0x30. Then the frustum's corner vectors in
    // camera space (CryEngine m_edge_nlt, m_edge_plt, m_edge_flt; x right, y forward, z up): the near plane's corner
    // {-near*tan_h, near, near*tan_v} at +0x50, the projection plane's {-w/2, w/2/tan_h, h/2} in pixels at +0x5C,
    // and the far plane's corner at +0x68 (in 1.5.6 gameplay, near 0.05 m and far 8000 m). The builder rebuilds the
    // cull planes and the render projection from them, so a new FOV rescales the x and z of the near and far corners
    // by tan(new/2)/tan(game/2) and the projection plane's distance by the inverse, and a new near plane scales the
    // near corner as a whole.
    constexpr ptrdiff_t CCAMERA_PROJECTION_FOV_OFFSET = 0x30; // float, projection FOV (radians)
    constexpr ptrdiff_t CCAMERA_CULL_EDGE_0_OFFSET = 0x50;    // float, near corner x (proportional to tan)
    constexpr ptrdiff_t CCAMERA_CULL_EDGE_1_OFFSET = 0x58;    // float, near corner z (proportional to tan)
    constexpr ptrdiff_t CCAMERA_CULL_TAN_OFFSET = 0x60;       // float, projection plane distance (1/tan)
    constexpr ptrdiff_t CCAMERA_CULL_EDGE_3_OFFSET = 0x68;    // float, far corner x (proportional to tan)
    constexpr ptrdiff_t CCAMERA_CULL_EDGE_4_OFFSET = 0x70;    // float, far corner z (proportional to tan)
    constexpr ptrdiff_t CCAMERA_NEAR_CORNER_OFFSET = 0x50;    // Vec3, near corner {x, near distance, z}

    // Hide-head flag mirrored on the player entity (relative to the entity passed to the
    // head-visibility setter); read to re-assert the head while the offset is active.
    constexpr ptrdiff_t OFFSET_ENTITY_HIDE_HEAD_FLAG = 0xA38;
    // The setter's third argument, stored next to the flag (+0xA39); the game passes it back unchanged on re-applies.
    constexpr ptrdiff_t OFFSET_ENTITY_HIDE_HEAD_ARG = 0xA39;

    // Input Event Offsets
    // SInputEvent layout (CryEngine IInput.h: deviceType@+0x00, state@+0x04, keyName@+0x08, keyId@+0x10,
    // modifiers@+0x14, value@+0x18, pSymbol@+0x20). TYPE_OFFSET is the STATE field, not the device type.
    constexpr ptrdiff_t INPUT_EVENT_TYPE_OFFSET = 0x04;  // SInputEvent.state (EInputState)
    constexpr ptrdiff_t INPUT_EVENT_ID_OFFSET = 0x10;    // SInputEvent.keyId (EKeyId)
    constexpr ptrdiff_t INPUT_EVENT_VALUE_OFFSET = 0x18; // SInputEvent.value (mouse: delta; pad: deflection)
    // EInputState::eIS_Changed (1 << 3). Every analog axis move - mouse OR gamepad stick - posts with this
    // state, so it is the device-AGNOSTIC gate for the look channel. Despite the name, the value is the input STATE,
    // not a mouse/device type (the keyId distinguishes the axis and the device).
    constexpr int MOUSE_INPUT_TYPE_ID = 8;

    /** @brief Name of the target game module. */
    constexpr const char *MODULE_NAME = "WHGame.dll";
} // namespace Constants

#endif // TPVCAMERA_LAYOUT_OFFSETS_HPP
