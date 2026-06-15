/**
 * @file constants.hpp
 * @brief Central definitions for constants used throughout the mod.
 *
 * Includes version info, filenames, memory offsets, RTTI type names, and engine
 * flag values. Code/data locations are resolved by the multi-candidate AOB
 * cascades in aob_resolver.hpp so they survive game updates; the few static
 * vtable/code addresses kept here are translated to runtime via
 * module_base + (static - IMAGE_BASE) and serve only as last-resort fallbacks.
 */
#ifndef TPVCAMERA_CONSTANTS_HPP
#define TPVCAMERA_CONSTANTS_HPP

#include <cstddef>
#include <cstdint>
#include <string>

#include "version.hpp"

/**
 * @namespace Constants
 * @brief Encapsulates global constants and config defaults.
 */
namespace Constants
{
    // Mod name derived from version.hpp; used for the INI/log file names and the
    // per-process instance mutex. The version string and repository URL are read
    // directly from TPVCamera::Version where they are needed.
    constexpr const char *MOD_NAME = TPVCamera::Version::MOD_NAME;

    // File extensions
    constexpr const char *INI_FILE_EXTENSION = ".ini";
    constexpr const char *PRESETS_FILE_SUFFIX = "_presets.json";

    /** @brief Gets the INI config filename (e.g., "KCD2_TPVCamera.ini"). */
    [[nodiscard]] inline std::string get_config_filename()
    {
        return std::string(MOD_NAME) + INI_FILE_EXTENSION;
    }

    /** @brief Gets the camera-presets JSON filename (e.g., "KCD2_TPVCamera_presets.json"). */
    [[nodiscard]] inline std::string get_presets_filename()
    {
        return std::string(MOD_NAME) + PRESETS_FILE_SUFFIX;
    }

    /** @brief Log file name passed to DMK::Bootstrap (string-view-safe literal). */
    constexpr const char *LOG_FILE_NAME = "KCD2_TPVCamera.log";
    /** @brief Per-PID instance-mutex prefix so duplicate ASI loads bail cleanly. */
    constexpr const char *INSTANCE_MUTEX_PREFIX = "KCD2_TPVCamera_";

    // --- Default Configuration Values ---
    /** @brief Default logging level ("INFO"). */
    constexpr const char *DEFAULT_LOG_LEVEL = "INFO";

    // All AOB signatures are defined as multi-candidate cascades in aob_resolver.hpp
    // (k_contextCandidates, k_frustumCandidates, ...). The global-context cascade
    // resolves the storage slot whose +0x38 reaches the camera-manager root the
    // game-state detection walks (see OFFSET_MANAGER_PTR_STORAGE); the frustum-builder
    // cascade resolves CCamera::UpdateFrustumPlanes, the matrix-offset hook target that
    // every gameplay camera funnels through (gated to the game view by the embedding
    // CView vtable, camera - SVIEWPARAMS_VIEWMATRIX_OFFSET).

    // Static image base of WHGame.DLL; used to translate static vtable/code addresses
    // into runtime addresses (runtime = module_base + (static - IMAGE_BASE)).
    constexpr uintptr_t IMAGE_BASE = 0x180000000;

    // RTTI type-descriptor name of the CView class. The frustum-builder detour confirms a
    // camera belongs to a game view by matching the embedding object's vtable against this
    // name (via DMK::Rtti), then caches that vtable address for a fast per-camera qword
    // compare. Anchoring on the ASLR-invariant RTTI name rather than a hardcoded vtable
    // address keeps the game-view gate working across game patches.
    constexpr const char *CVIEW_RTTI_NAME = ".?AVCView@@";

    // Player look/aim orientation chain. Used to LEVEL the aim pitch while free-look orbit is active
    // so the character's head and the eye look forward (not just the camera). Chain:
    //   g_env base (k_genvCandidates cascade) -> p_game (g_env + GENV_PGAME_OFFSET)
    //   -> CCryAction = p_game->IGame::GetIGameFramework() (vtable slot 16 = IGAME_GET_FRAMEWORK_VTABLE_OFFSET)
    //   -> p_action_game (CCryAction + CCRYACTION_ACTIONGAME_OFFSET)
    //   -> C_Player    (p_action_game + CACTIONGAME_LOCAL_ACTOR_OFFSET), validated by its RTTI type name
    //   -> look controller (C_Player + C_PLAYER_LOOK_CONTROLLER_OFFSET)
    //   -> scalar look PITCH (controller + LOOK_CONTROLLER_PITCH_OFFSET, radians, 0 = level), with a
    //      synchronized copy at LOOK_CONTROLLER_PITCH2_OFFSET; yaw lives alongside and is left alone.
    // The look quaternion the cameras read (controller + 0x24) is DERIVED from this scalar pitch+yaw
    // EVERY frame, so writing that quat is overwritten -- the SCALAR pitch is what must be written to
    // level the aim. Both copies are written so any internal current/target smoothing also settles at level.
    // g_env (SSystemGlobalEnvironment) base. Resolved patch-resiliently at startup from
    // `lea/mov [rip+g_env]` reference sites via the k_genvCandidates cascade
    // (aob_resolver.hpp); this static RVA is only the fallback used if every candidate
    // ever drifts.
    constexpr uintptr_t GENV_STATIC = 0x18492B800;
    // RTTI type-descriptor name of C_Player, used to validate the resolved actor (replaces a
    // hardcoded vtable address so the check survives patches).
    constexpr const char *C_PLAYER_RTTI_NAME = ".?AVC_Player@entitymodule@wh@@";
    constexpr ptrdiff_t GENV_PGAME_OFFSET = 0x90;
    constexpr ptrdiff_t IGAME_GET_FRAMEWORK_VTABLE_OFFSET = 0x80; // IGame vtable slot 16
    constexpr ptrdiff_t CCRYACTION_ACTIONGAME_OFFSET = 0x88;
    constexpr ptrdiff_t CACTIONGAME_LOCAL_ACTOR_OFFSET = 0xA40;
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
    // quat) but does NOT turn the body -- the body heading is owned by the animated-character layer,
    // driven separately via the BODY-turn constants below.
    constexpr ptrdiff_t LOOK_CONTROLLER_YAW_OFFSET = 0x10;
    constexpr ptrdiff_t LOOK_CONTROLLER_YAW2_OFFSET = 0x44;

    // --- Player BODY-turn: force the entity world yaw (camera-relative body facing) ----------------
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

    // The head-visibility setter (k_headVisibilityCandidates) has signature
    // void __fastcall(this /*rcx*/, bool hide_head /*dl*/, char flags /*r8b*/). The
    // first-person rig hides the player head; forcing hide_head to false keeps it
    // visible while the third-person offset is rendering so the player is not headless
    // from behind.

    // Global action dispatcher (k_actionDispatchCandidates): the C++ source of Lua Player:OnAction. Fires
    // once per action-map action with its name and post-action-map value:
    //   _QWORD*(this /*rcx*/, const char** action_name /*rdx*/, uint activation /*r8d*/, float value /*xmm3*/)
    // The value is device-agnostic (keyboard, gamepad and rebinds all funnel through the same action
    // name), so reading the xi_movey / xi_movex axis here gives movement INTENT -- nonzero while a key is
    // held even when a wall arrests the body, which the body-position speed cannot distinguish.
    // The movement action names are matched in player_onaction_hook.cpp (a small candidate list across the
    // xi_* / movement_* / move* action maps); a trace-level probe there logs each distinct action name once so
    // the on-foot movement axis vocabulary can be confirmed at runtime.

    // --- Physics world raycast (camera collision + aim convergence) ---
    // IPhysicalWorld::RayWorldIntersection inline helper (k_rayWorldIntersectionCandidates). Casts a
    // world ray and fills a ray_hit; returns the hit count. Signature (Microsoft x64):
    //   int(this /*rcx = p_physical_world*/, const Vec3* org /*rdx*/, const Vec3* dir /*r8*/,
    //       int objtypes /*r9d*/, uint flags, ray_hit* hits, int n_max_hits, void* p_skip_ents,
    //       int n_skip_ents, void* p_foreign_data, int i_foreign_data, const char* p_name_tag)
    // dir is NOT normalized -- its length is the maximum ray length, and ray_hit.dist is the
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

    // ray_hit field offsets (CryEngine physinterface.h; struct size 0x50).
    constexpr size_t RAY_HIT_SIZE = 0x50;
    constexpr ptrdiff_t RAY_HIT_OFFSET_DISTANCE = 0x00; // float, world distance along dir
    constexpr ptrdiff_t RAY_HIT_OFFSET_COLLIDER = 0x08; // IPhysicalEntity* pCollider (the entity hit)
    constexpr ptrdiff_t RAY_HIT_OFFSET_POINT = 0x24;    // Vec3 world hit position
    constexpr ptrdiff_t RAY_HIT_OFFSET_NORMAL = 0x30;   // Vec3 surface normal

    // CPhysicalEntity / CPhysicalPlaceholder world AABB (m_BBox): min @ +0x08, max @ +0x14, each a Vec3.
    // Across many CPhysicalEntity colliders (buildings ~2-4m, a post ~0.72m, fence enclosures) each reads a
    // sane world-space min/max. Used to size a hit collider so the
    // camera can skip standalone THIN scenery (a ~0.02-0.1m stick/pole) while still blocking on real walls.
    constexpr ptrdiff_t PHYS_ENTITY_BBOX_MIN_OFFSET = 0x08;
    constexpr ptrdiff_t PHYS_ENTITY_BBOX_MAX_OFFSET = 0x14;

    // entity_query_flags subset (physinterface.h; stock numbering in WHGame's 3DEngine
    // RWI helper sub_180484A10: ent_static=1, ent_sleeping_rigid=2, ent_rigid=4, ent_living=8,
    // ent_independent=0x10, ent_terrain=0x100). Camera collision blocks on the SOLID WORLD ONLY --
    // ent_static | ent_terrain -- which is buildings/walls/level geometry/static props/vegetation plus
    // the ground. It deliberately EXCLUDES ent_rigid|ent_sleeping_rigid (movable props AND the player's
    // own physicalized worn gear, e.g. a shield on the back -- the gear is a rigid that owning-actor
    // exclusion alone cannot drop), ent_living (player + NPC capsules) and ent_independent (ropes /
    // ragdolls / dangling attachments). This mirrors CryEngine's "robust obstruction" intent of hitting
    // only the world, without needing a per-frame skip-entities list. Trade-off: the camera also glides
    // through movable rigids (crates, barrels, cart, an open/closed door leaf); add a player skip-ents
    // list and restore ent_rigid only if those must block the view.
    constexpr int RWI_OBJTYPES_CAMERA = 0x101; // ent_static | ent_terrain (solid world only)
    // rwi_stop_at_pierceable (0x0F) | rwi_colltype_any (0x400). Every engine camera/aim caller
    // sets rwi_colltype_any so the ray reports hits whatever the surface collision class; without
    // it a surface that does not match the default collision filter is skipped (a phantom miss
    // that let the camera/crosshair punch through). It only adds valid hits, never removes them.
    //
    // The high word adds an explicit geometry collide-type request: geom_colltype_ray (0x00020000) |
    // geom_colltype_player (0x80000000). Fabric tent/awning/stall ROOFS in KCD2 are static collision-PROXY
    // meshes flagged geom_colltype_player but NOT geom_colltype_ray (MTL_FLAG_COLLISION_PROXY), so a plain
    // colltype_any ray slid straight through them and the third-person camera buried itself in the cloth.
    // Requesting ray|player colltype makes RayWorldIntersection report those proxies, so the camera collides
    // with fabric roofs while still hitting normal world geometry (objtypes stay RWI_OBJTYPES_CAMERA 0x101).
    // Verified live on retail 1.5.5. (See docs/analysis/foliage_occlusion_findings.md.)
    //
    // Composed from the named physinterface.h bit values rather than a magic literal so the intent is
    // self-documenting and each contributing flag is independently auditable.
    constexpr unsigned int RWI_STOP_AT_PIERCEABLE = 0x0000000F; // stop at the first solid (non-pierceable) hit
    constexpr unsigned int RWI_COLLTYPE_ANY = 0x00000400;       // report a hit whatever the surface collision class
    constexpr unsigned int GEOM_COLLTYPE_RAY = 0x00020000;      // request geometry flagged as ray-collidable
    constexpr unsigned int GEOM_COLLTYPE_PLAYER = 0x80000000;   // also request player-collidable proxies (fabric roofs)
    constexpr unsigned int RWI_FLAGS_STOP_AT_SOLID =
        RWI_STOP_AT_PIERCEABLE | RWI_COLLTYPE_ANY | GEOM_COLLTYPE_RAY | GEOM_COLLTYPE_PLAYER; // 0x8002040F

    // --- Swept-sphere camera collision (IPhysicalWorld::PrimitiveWorldIntersection) ---
    // A single thin RayWorldIntersection ray to a sweeping endpoint (the camera rotates+moves
    // every frame) catches different objects on consecutive frames as it grazes edges, so the
    // nearest-hit distance is a step function -> the camera position pumps in dense geometry.
    // A swept SPHERE replaces the line with a tube of radius r, so the contact distance varies
    // continuously across edges and the pump is gone at the source. PWI returns the distance to
    // first contact (its float return value), and the sphere radius is the standoff (so no skin
    // is subtracted on the sphere path).
    //
    // PWI is reached through the LIVE physical-world vtable, NOT a static address: the static
    // pPhysicalWorld global did not hold a valid pointer in the running process, and the vtable
    // slot is the most patch-stable anchor. world = *(g_env + PHYSICAL_WORLD_OFFSET);
    // pwi = (*(void***)world)[PHYS_WORLD_VTABLE_PWI_OFFSET/8]. That slot is a lock wrapper
    // (sub_1808181DC: takes the world mutex, forwards to the real impl sub_1808182A0), so calling
    // it is the safe public entry. Signature (Microsoft x64), consolidated SPWIParams form:
    //   float(this /*rcx=world*/, SPWIParams* pp /*rdx*/, void* pLockContacts /*r8=0*/,
    //         const char* pNameTag /*r9*/)  -> xmm0 = distance to first hit (> 0 == hit).
    constexpr ptrdiff_t PHYS_WORLD_VTABLE_PWI_OFFSET = 0x1C8; // world vtable slot

    // primitives::sphere { Vec3 center; float r; } -- 16 bytes, type id 4. center is WORLD space.
    constexpr int PRIMITIVE_TYPE_SPHERE = 4;
    constexpr size_t PRIMITIVE_SPHERE_SIZE = 0x10;

    // Fork SPWIParams field offsets, reversed from the impl (sub_1808182A0) reading its 2nd arg and
    // the constructing caller (sub_180817E4C). The struct is heavily reorganized vs the generic
    // CryEngine header, so these are build-specific. Confidence noted per field; the OUTPUT/lock
    // fields (engine writes through them) are the only memory-safety-critical ones and are CONFIRMED.
    constexpr size_t SPWI_PARAMS_SIZE = 0x100;    // ctor memsets 0xE8; pad to 0x100
    constexpr ptrdiff_t SPWI_OFF_ITYPE = 0x18;    // int primitive type   (CONFIRMED, matches header)
    constexpr ptrdiff_t SPWI_OFF_PPRIM = 0x20;    // const primitive*     (CONFIRMED, matches header)
    constexpr ptrdiff_t SPWI_OFF_SWEEPDIR = 0x8C; // Vec3 sweep vector    (CONFIRMED: impl |dir|^2>0 -> sweep)
    constexpr ptrdiff_t SPWI_OFF_FLAGS =
        0x98; // int rwi-style flags  (CONFIRMED: impl tests &0x800 rwi_queue) -- NOT entTypes
    constexpr ptrdiff_t SPWI_OFF_ENTTYPES = 0x9C; // entity_query_flags slot per header decl order, but DEAD
                                                  //                       in this fork: the impl (sub_1808182A0) has
                                                  //                       ZERO reads of +0x9C, so the sphere CANNOT be
                                                  //                       type-filtered (it always queries ent_all).
                                                  //                       Actors are excluded by the fan-authority gate
                                                  //                       in camera_hook instead, NOT by this field.
    constexpr ptrdiff_t SPWI_OFF_PPCONTACT = 0xA0;    // geom_contact** OUT   (CONFIRMED, engine writes *ppcontact)
    constexpr ptrdiff_t SPWI_OFF_GEOMFLAGSALL = 0xA8; // int                  (tentative: filtering only)
    constexpr ptrdiff_t SPWI_OFF_GEOMFLAGSANY = 0xAC; // int                  (tentative: filtering only)
    constexpr ptrdiff_t SPWI_OFF_NSKIPENTS = 0xB8;    // int                  (CONFIRMED: impl clamps to <=4)
    constexpr ptrdiff_t SPWI_OFF_PSKIPENTS = 0xC0;    // IPhysicalEntity**    (CONFIRMED: impl indexes pSkipEnts[i])
    constexpr ptrdiff_t SPWI_OFF_LOCK_IACTIVE = 0xD8; // int  WriteLockCond.iActive (CONFIRMED)
    constexpr ptrdiff_t SPWI_OFF_LOCK_PRW =
        0xE0; // int* WriteLockCond.prw     (CONFIRMED; self-ptr = thread-safe, no global lock)

    // --- Camera-space interaction (door/usable look-at ray redirect) ---
    // The player interactor (wh::entitymodule::C_PlayerInteractor) selects the "press to use" target by
    // casting a look ray built from the GAMEPLAY view camera (sub_18091C138 -> qword_18549B4B0), which the
    // mod does NOT offset -- so in third person the screen-centre crosshair and the use-target diverge
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

    // k_interactionOnScreenCandidates resolves the on-screen reticle gate: it projects a candidate's
    // world interaction point through the gameplay camera and rejects it when it falls outside the
    // on-screen reticle range. This is the single gate that drops the shrine when the body is not
    // turned (it projects off-reticle in the gameplay camera even though centered in the offset render
    // view). The hook force-passes ONLY candidates whose world point lies along the published crosshair
    // ray (perp distance below the max below); all others fall through to the original. Args: a1=rcx
    // a2=rdx(&worldPoint Vec3) a3=r8(&out 2D screen coords, written) a4=r9(config).
    constexpr float INTERACTION_ONSCREEN_RAY_PERP_MAX = 0.85f; // m; max perp distance to the crosshair ray
    constexpr float INTERACTION_ONSCREEN_CENTER = 50.0f;       // screen-center coord (priority = dist from it)

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
    // type -- so the same gate catches mouse AND gamepad analog axes; see INPUT_EVENT_TYPE_OFFSET).
    constexpr int INPUT_LOOK_YAW_EVENT_ID = 0x10A;   // mouse horizontal look (eKI_MouseX); value = delta
    constexpr int INPUT_LOOK_PITCH_EVENT_ID = 0x10B; // mouse vertical look (eKI_MouseY); value = delta
    // Gamepad RIGHT-STICK axes (xi_thumbrx / xi_thumbry, verified via the XInput symbol table in WHGame).
    // Same eIS_Changed channel, but value at +0x18 is the analog DEFLECTION (-1..1, post-deadzone), not a
    // delta -- the orbit hook latches it and the render hook integrates it by rate (GamepadOrbitSpeed deg/s).
    constexpr int INPUT_PAD_LOOK_YAW_EVENT_ID = 0x21A;   // right-stick X (horizontal)
    constexpr int INPUT_PAD_LOOK_PITCH_EVENT_ID = 0x21B; // right-stick Y (vertical)

    // --- Memory Offsets ---
    // Global-context -> camera-manager pointer. The manager is the root the game-state detection
    // walks to read the active camera (see OFFSET_ACTIVE_CAMERA below and game_state.cpp).
    constexpr ptrdiff_t OFFSET_MANAGER_PTR_STORAGE = 0x38; // Global context to camera manager
    // RTTI type-descriptor name of the camera manager, the self-heal anchor for OFFSET_MANAGER_PTR_STORAGE.
    constexpr const char *C_CAMERA_MANAGER_RTTI_NAME = ".?AVC_CameraManager@game@wh@@";

    // --- Game-state detection (see game_state.cpp) ---
    // Active-camera pointer on the wh::game::C_CameraManager (the same manager reached via
    // OFFSET_MANAGER_PTR_STORAGE). The manager stores a pointer to the currently active
    // wh::game::C_Camera* subclass here; its RTTI type name identifies the camera mode. The game
    // selects this camera BEFORE the pose smoother runs, so a state read from it does not lag.
    constexpr ptrdiff_t OFFSET_ACTIVE_CAMERA = 0x30;
    // Mount is detected from the STANCE enum (C_ACTOR_MODEL_STANCE_MOUNT == 5, see below), NOT a per-player
    // flag. The old C_PLAYER_MOUNT_FLAG_OFFSET (C_Player + 0x174) was a CONTROL-OVERRIDE REFCOUNT: it is
    // incremented/decremented for ANY control state (mounting OR an item pickup OR a scripted interaction),
    // so == 1 was true during pickups too -- that flashed the MOUNT camera preset on item pickups. The
    // stance is immune (a pickup keeps stance == 1).
    // RTTI type-descriptor names of the active-camera classes that map to a tracked game state.
    // Matched by name against the active camera's vtable (ASLR/patch-safe, like CVIEW_RTTI_NAME).
    // NOTE: there is deliberately no camera entry for minigames. Only DICE swaps the active camera to
    // C_CameraMinigame; lockpicking, reading, pickpocketing, alchemy, etc. render through
    // C_CameraFirstPerson, so the active camera class cannot tell whether a minigame is on screen.
    // The minigame state is read from the C_MinigameManager instead (see OFFSET_MINIGAME_* below).
    constexpr const char *C_CAMERA_COMBAT_RTTI_NAME = ".?AVC_CameraCombatDelegate@game@wh@@";
    constexpr const char *C_CAMERA_DIALOG_RTTI_NAME = ".?AVC_CameraDialog@game@wh@@";

    // --- Minigame detection (see game_state.cpp poll_active_minigame) ---
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

    // Crouch/sneak AND mount BOTH come from the player's STANCE enum. C_ActorModel is a POINTER on
    // C_Player, dereferenced then validated by its RTTI. The 4-byte CURRENT STANCE enum lives at +0x80 --
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
    // runs, read and rewritten by the per-preset FOV override (see camera_hook offset_game_view_camera).
    // The projection FOV scalar (radians) is at +0x30; the cull-frustum edge vectors at +0x50/+0x58/+0x60/
    // +0x68/+0x70 are all proportional to 1/tan(fov/2) (with +0x60 the tan term itself), so rescaling them
    // by tan(new/2)/tan(game/2) keeps culling matched to the overridden FOV.
    constexpr ptrdiff_t CCAMERA_PROJECTION_FOV_OFFSET = 0x30; // float, projection FOV (radians)
    constexpr ptrdiff_t CCAMERA_CULL_EDGE_0_OFFSET = 0x50;    // float, cull edge (proportional to 1/tan)
    constexpr ptrdiff_t CCAMERA_CULL_EDGE_1_OFFSET = 0x58;
    constexpr ptrdiff_t CCAMERA_CULL_TAN_OFFSET = 0x60; // float, tan(fov/2) term (rescaled by the inverse ratio)
    constexpr ptrdiff_t CCAMERA_CULL_EDGE_3_OFFSET = 0x68;
    constexpr ptrdiff_t CCAMERA_CULL_EDGE_4_OFFSET = 0x70;

    // Hide-head flag mirrored on the player entity (relative to the entity passed to the
    // head-visibility setter); read to re-assert the head while the offset is active.
    constexpr ptrdiff_t OFFSET_ENTITY_HIDE_HEAD_FLAG = 0xA38;

    // --- Input Event Offsets ---
    // SInputEvent layout (CryEngine IInput.h: deviceType@+0x00, state@+0x04, keyName@+0x08, keyId@+0x10,
    // modifiers@+0x14, value@+0x18, pSymbol@+0x20). TYPE_OFFSET is the STATE field, not the device type.
    constexpr ptrdiff_t INPUT_EVENT_TYPE_OFFSET = 0x04;  // SInputEvent.state (EInputState)
    constexpr ptrdiff_t INPUT_EVENT_ID_OFFSET = 0x10;    // SInputEvent.keyId (EKeyId)
    constexpr ptrdiff_t INPUT_EVENT_VALUE_OFFSET = 0x18; // SInputEvent.value (mouse: delta; pad: deflection)
    // EInputState::eIS_Changed (1 << 3). Every analog axis move -- mouse OR gamepad stick -- posts with this
    // state, so it is the device-AGNOSTIC gate for the look channel. Name kept for compatibility; it is the
    // input STATE, not a mouse/device type (the keyId distinguishes the axis and the device).
    constexpr int MOUSE_INPUT_TYPE_ID = 8;

    /** @brief Name of the target game module. */
    constexpr const char *MODULE_NAME = "WHGame.dll";
} // namespace Constants
#endif // TPVCAMERA_CONSTANTS_HPP
