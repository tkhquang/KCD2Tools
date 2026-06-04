/**
 * @file camera_hook.cpp
 * @brief Third-person camera implemented at the camera frustum builder.
 *
 * Hooks CCamera::UpdateFrustumPlanes, the function that turns a camera's 3x4 matrix
 * into world-space cull planes. By offsetting the game view camera's matrix HERE,
 * before the cull planes are computed from it, the camera renders behind the player
 * AND the frustum culls from the same offset position, so nearby geometry is not
 * wrongly hidden. Offsetting only the rendered matrix afterwards (at the view
 * convergence point on return) leaves the frustum culling from the eye, which was the
 * source of the "items not shown up close" artifact.
 *
 * Every gameplay camera (first person, combat through its private inner camera, mount)
 * funnels into this builder for the active CView, so one hook covers them all. The
 * builder is shared by shadow, reflection, and portal cameras too, so the detour gates
 * to the game view by checking that the CView embedding the camera carries the CView
 * vtable.
 *
 * Gameplay still reads the player look-dir/eye channel, not this render camera, so aim
 * and interaction keep working off the native frame; only the rendered sink moves. A
 * companion hook on the head-visibility setter keeps the player head rendered from
 * behind. The offset is suppressed while the game is in any state listed in SuppressTPVState (by
 * default the pause menu and blocking overlays), so that context renders from the game's own camera.
 */

#include "camera_hook.hpp"
#include "constants.hpp"
#include "config.hpp"
#include "global_state.hpp"
#include "game_state.hpp"
#include "game_structures.hpp"
#include "math_utils.hpp"
#include "physics_raycast.hpp"
#include "hooks/ui_menu_hooks.hpp"
#include "hooks/player_onaction_hook.hpp"
#include "presets/preset_runtime.hpp"

#include <DetourModKit.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace TPVCamera
{

// CCamera::UpdateFrustumPlanes(camera /*rcx*/): camera object starts with the 3x4 matrix,
// followed by the projection params; this builds the world cull planes. Offsetting the matrix
// here, before the cull planes are computed, moves the rendered view AND its culling together.
using FrustumBuildFunc = uintptr_t(__fastcall *)(uintptr_t camera);

// Head-visibility setter: SetHeadHidden(this /*rcx*/, bool hide_head /*dl*/, char flags /*r8b*/).
// hide_head == false keeps the head rendered.
using SetHeadVisibilityFunc = void(__fastcall *)(uintptr_t entity, bool hide_head, char flags);

// Generic input-event dispatcher: void __fastcall(controller /*rcx*/, event /*rdx*/,
// char /*r8b*/). Every input event funnels through here; hooking it lets free-look
// capture mouse-look deltas and freeze the player by not dispatching while orbiting.
using InputDispatchFunc = void(__fastcall *)(uintptr_t controller, uintptr_t input_event, char flag);

static FrustumBuildFunc s_frustum_build_original = nullptr;
static SetHeadVisibilityFunc s_set_head_visibility_original = nullptr;
static InputDispatchFunc s_input_dispatch_original = nullptr;

// Resolved SSystemGlobalEnvironment (g_env) base, set once at init (AOB, with the static RVA as
// fallback). Reused by the player/animchar walks; p_physical_world is g_env + PHYSICAL_WORLD_OFFSET.
static uintptr_t s_genv_runtime = 0;

// CView vtable address, cached lazily on the first frustum-builder camera whose embedding object
// passes the CView RTTI check. The steady-state game-view gate is then a single qword compare;
// using the RTTI type name (not a hardcoded address) keeps the gate working across game patches.
static uintptr_t s_cview_vtable_runtime = 0;

// Latched from the head-visibility setter so the per-frame re-assert can drive the head
// without waiting for the game to call the setter again (toggling the offset does not
// make the game call it, so without the re-assert the head would stay hidden after an
// FPV/TPV switch). The setter only ever runs for the player (it toggles the
// FirstPersonView rig), so the entity is the player. s_head_was_active tracks the offset's
// active state across frames so the head is restored to the game's intended value
// exactly once when the offset turns off.
static std::atomic<uintptr_t> s_head_entity{0};
static std::atomic<uint8_t> s_head_flags{0};
static std::atomic<bool> s_game_intended_hide_head{false};
static std::atomic<bool> s_head_was_active{false};

// The offset's effective active state (effective_tpv && should_apply_view), published by the
// frustum detour each frame. The head-visibility detour and the free-look input gate read it so
// they mirror the live offset state without recomputing the whole forced-view policy themselves.
static std::atomic<bool> s_offset_active{false};

// Published by the frustum detour each game-view frame: true while the game is showing the OS cursor
// (a UI is up). The free-look input gate reads it to FREEZE the orbit -- hold its angles and ignore
// mouse-look -- while any cursor UI is open (menu, inventory, loot/trade, dialogue), so the camera
// does not turn from cursor motion and resumes from the same angle when the cursor hides. The orbit
// hook captures raw mouse UPSTREAM of the engine's own input freeze, so it needs this explicit gate.
static std::atomic<bool> s_cursor_shown{false};

// Camera-relative movement (toggle orbit). The character's horizontal speed is derived from its body
// world position each frame (device-agnostic: no hardcoded movement keys). Crossing the START speed
// from idle aligns the heading to the camera once; it returns to idle only below the lower STOP speed
// (hysteresis, so a momentary dip cannot re-trigger the align). k_orbit_aim_level_speed eases the look
// pitch toward level per second while orbiting.
// Movement-INPUT thresholds (device-agnostic xi_move axis magnitude, ~0..1.4), used in place of the
// body-position speed when the action-dispatch hook resolved. The input stays nonzero while a movement
// key is held even if a wall arrests the body, so the heading is not falsely released on a collision stop.
constexpr float k_orbit_move_input_start = 0.15f;
constexpr float k_orbit_move_input_stop = 0.05f;
constexpr float k_orbit_aim_level_speed = 8.0f;

// Orbit angle low-pass. The engine smooths native look DOWNSTREAM of the input dispatch the orbit
// hook captures and blocks, so free-look applies the raw per-frame mouse deltas and reads as jittery.
// orbit_smoothing (0..1) maps to a frame-rate-independent catch-up speed between these bounds: higher
// smoothing -> lower speed -> more lag (smoother); 0 disables the filter (snap to the raw target).
constexpr float k_orbit_smooth_min_speed = 6.0f;  // strength 1.0: heavy smoothing
constexpr float k_orbit_smooth_max_speed = 40.0f; // strength near 0: barely-there smoothing

// Maximum aim-convergence toe-in. The off-axis camera toes in toward the aim focus point by
// atan(|OffsetRight| / (focus_distance + follow_distance)); that angle is ALSO the residual crosshair
// error for any target past the focus. Bounding it stops a small follow distance combined with a
// shoulder offset from swinging the rendered look far off the aim line (the crosshair drifts left, so
// the real aim lands to the right). 0.14054 = tan(8 deg); 8 deg leaves typical over-the-shoulder
// framing unchanged (FollowDistance 5, OffsetRight 1 toes in only ~5.7 deg) and turns the pathological
// FollowDistance 0.5 / OffsetRight 1 case from ~45 deg down to 8 deg.
constexpr float k_tan_max_convergence = 0.14054f;

/**
 * @brief Whether the third-person offset should be applied right now.
 * @details Suppressed (forced first-person) while the game is in any state listed in SuppressTPVState,
 *          so that context renders from the game's own untouched camera. This is a HARD gate: it
 *          overrides cam.applying, so the player cannot toggle third-person back on while a listed state
 *          lasts (unlike the edge-triggered ForcedFPVState policy), and the prior view resumes when the
 *          state ends. Menu and Overlay are read from the LIVE UI signals, so they suppress the same
 *          frame the screen opens and release the same frame it closes; every other state (Combat, Mount,
 *          Dialogue, Minigame, Aiming, Crouch) is matched against the debounced game-state mask, so a
 *          brief flicker does not pop the view. Read directly from the live config, so it is independent
 *          of EnableStateBehavior. Called from the render thread (the frustum detour) and the
 *          input-dispatch thread (the free-look gate); every read is atomic, so the cross-thread call is
 *          safe.
 */
[[nodiscard]] static bool should_apply_view()
{
    const uint32_t suppress = settings().suppress_tpv_mask.load(std::memory_order_relaxed);
    if (suppress == 0)
    {
        return true;
    }
    // Menu / Overlay use the live UI signals so suppression is instant (no debounce frame of TPV in a UI).
    if ((suppress & state_bit(GameState::Menu)) != 0 && is_game_menu_open())
    {
        return false;
    }
    if ((suppress & state_bit(GameState::Overlay)) != 0 &&
        overlay_state().active.load(std::memory_order_relaxed))
    {
        return false;
    }
    // Any other listed state is matched against the debounced mask published each game-view frame.
    constexpr uint32_t k_ui_bits = state_bit(GameState::Menu) | state_bit(GameState::Overlay);
    const uint32_t gameplay = suppress & ~k_ui_bits;
    if (gameplay != 0 && (gameplay & game_state_mask().load(std::memory_order_relaxed)) != 0)
    {
        return false;
    }
    return true;
}

/**
 * @brief Wall-clock seconds since the previous call, clamped.
 * @details The frustum builder runs once or twice per frame for the game view (projection
 *          setup plus the final rebuild). A second call within the same frame sees a near-zero
 *          delta, so the integrators do not double-advance; the lower clamp is therefore 0.
 */
[[nodiscard]] static float frame_delta()
{
    static std::chrono::steady_clock::time_point s_last_time = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const float delta_time = std::chrono::duration<float>(now - s_last_time).count();
    s_last_time = now;
    return std::clamp(delta_time, 0.0f, 0.1f);
}

/**
 * @brief Resolves the live C_Player via g_env each frame (validated by its vtable); 0 on failure.
 * @details Walks g_env -> p_game -> CCryAction (cached, resolved once via a virtual GetIGameFramework
 *          call) -> p_action_game -> C_Player, then confirms C_Player by its main vtable. Resolving
 *          FRESH every frame -- rather than trusting a mirrored pointer that goes null/stale across
 *          view transitions and reloads -- is what keeps the move-detection and body-turn locked onto
 *          the CURRENT player. Always called from within an SEH frame (the frustum detour / the
 *          body-turn wrapper), so a fault during the walk is contained.
 */
static uintptr_t resolve_c_player()
{
    const ModuleInfo &mod = module_info();
    if (mod.base == 0)
    {
        return 0;
    }
    const uintptr_t g_env_addr = s_genv_runtime;
    const auto p_game = DMK::Memory::seh_read<uintptr_t>(g_env_addr + Constants::GENV_PGAME_OFFSET);
    if (!p_game || !DMK::Memory::plausible_userspace_ptr(*p_game))
    {
        return 0;
    }

    static uintptr_t s_cry_action = 0;
    if (!DMK::Memory::plausible_userspace_ptr(s_cry_action))
    {
        const auto vtable = DMK::Memory::seh_read<uintptr_t>(*p_game);
        if (!vtable || !DMK::Memory::plausible_userspace_ptr(*vtable))
        {
            return 0;
        }
        const auto fn = DMK::Memory::seh_read<uintptr_t>(*vtable + Constants::IGAME_GET_FRAMEWORK_VTABLE_OFFSET);
        if (!fn || !DMK::Memory::plausible_userspace_ptr(*fn))
        {
            return 0;
        }
        using GetFrameworkFn = uintptr_t(__fastcall *)(uintptr_t);
        s_cry_action = reinterpret_cast<GetFrameworkFn>(*fn)(*p_game);
        if (!DMK::Memory::plausible_userspace_ptr(s_cry_action))
        {
            s_cry_action = 0;
            return 0;
        }
    }

    const auto p_action_game =
        DMK::Memory::seh_read<uintptr_t>(s_cry_action + Constants::CCRYACTION_ACTIONGAME_OFFSET);
    if (!p_action_game || !DMK::Memory::plausible_userspace_ptr(*p_action_game))
    {
        return 0;
    }
    const auto c_player =
        DMK::Memory::seh_read<uintptr_t>(*p_action_game + Constants::CACTIONGAME_LOCAL_ACTOR_OFFSET);
    if (!c_player || !DMK::Memory::plausible_userspace_ptr(*c_player))
    {
        return 0;
    }
    const auto vt = DMK::Memory::seh_read<uintptr_t>(*c_player);
    if (!vt || !DMK::Rtti::vtable_is_type(*vt, Constants::C_PLAYER_RTTI_NAME))
    {
        return 0;
    }

    // Log the resolved C_Player and cached CCryAction only when the player pointer CHANGES (once on first
    // resolve, and again after a reload / new player) so the address is available for external tooling
    // without flooding the log.
    {
        static uintptr_t s_logged_player{0};
        if (*c_player != s_logged_player)
        {
            s_logged_player = *c_player;
            DMK::Logger::get_instance().debug("C_Player resolved={} (CCryAction={})",
                                              DMK::Format::format_address(*c_player),
                                              DMK::Format::format_address(s_cry_action));
        }
    }

    // The player resolves once the game is in-world (window at its final resolution); let the overlay
    // wait on this so it never sizes itself to the transient loading window.
    game_world_ready().store(true, std::memory_order_relaxed);

    return *c_player;
}

/**
 * @brief Resolves the player look controller and drives the real aim while orbiting: eases the PITCH
 *        toward level and/or sets the YAW (heading). Separated from the SEH wrapper so this frame
 *        holds no unwinding objects.
 * @details Walks the player look chain (g_env -> p_game -> CCryAction -> p_action_game -> C_Player ->
 *          look controller; see constants.hpp) and validates C_Player by its vtable. The look
 *          quaternion the cameras read is RE-DERIVED from the controller's scalar pitch+yaw every
 *          frame, so writing those scalars (not the derived quat, which is overwritten) is what
 *          actually moves the eye, the character head AND the movement heading. The mod redirects the
 *          look input while orbiting, so the writes stick; on any failure it returns without writing
 *          and the camera-side level blend still levels the view. CCryAction is process-lifetime and
 *          resolved once via a virtual GetIGameFramework call, then cached; the rest is a guarded walk.
 * @param pitch_ease Per-frame fraction to move the look pitch toward level, in [0, 1] (0 = leave it).
 * @param set_yaw When true, the look yaw is set to yaw_value to align the heading to the camera.
 * @param yaw_value Target look yaw in radians (engine convention: forward = (-sin yaw, cos yaw)).
 */
static void apply_orbit_aim_control_impl(float pitch_ease, bool set_yaw, float yaw_value)
{
    const ModuleInfo &mod = module_info();
    if (mod.base == 0)
    {
        return;
    }
    const uintptr_t g_env_addr = s_genv_runtime;
    const auto p_game = DMK::Memory::seh_read<uintptr_t>(g_env_addr + Constants::GENV_PGAME_OFFSET);
    if (!p_game || !DMK::Memory::plausible_userspace_ptr(*p_game))
    {
        return;
    }

    // CCryAction is process-lifetime; resolve it once via p_game->IGame::GetIGameFramework() (a
    // trivial member getter) and cache, so the per-frame path is a pure guarded pointer walk.
    static uintptr_t s_cry_action = 0;
    if (!DMK::Memory::plausible_userspace_ptr(s_cry_action))
    {
        const auto vtable = DMK::Memory::seh_read<uintptr_t>(*p_game);
        if (!vtable || !DMK::Memory::plausible_userspace_ptr(*vtable))
        {
            return;
        }
        const auto fn = DMK::Memory::seh_read<uintptr_t>(*vtable + Constants::IGAME_GET_FRAMEWORK_VTABLE_OFFSET);
        if (!fn || !DMK::Memory::plausible_userspace_ptr(*fn))
        {
            return;
        }
        using GetFrameworkFn = uintptr_t(__fastcall *)(uintptr_t);
        s_cry_action = reinterpret_cast<GetFrameworkFn>(*fn)(*p_game);
        if (!DMK::Memory::plausible_userspace_ptr(s_cry_action))
        {
            s_cry_action = 0;
            return;
        }
    }

    const auto p_action_game =
        DMK::Memory::seh_read<uintptr_t>(s_cry_action + Constants::CCRYACTION_ACTIONGAME_OFFSET);
    if (!p_action_game || !DMK::Memory::plausible_userspace_ptr(*p_action_game))
    {
        return;
    }
    const auto c_player =
        DMK::Memory::seh_read<uintptr_t>(*p_action_game + Constants::CACTIONGAME_LOCAL_ACTOR_OFFSET);
    if (!c_player || !DMK::Memory::plausible_userspace_ptr(*c_player))
    {
        return;
    }
    // Confirm this is really C_Player (its main vtable) before trusting the controller offset.
    const auto vt = DMK::Memory::seh_read<uintptr_t>(*c_player);
    if (!vt || !DMK::Rtti::vtable_is_type(*vt, Constants::C_PLAYER_RTTI_NAME))
    {
        return;
    }
    const auto controller =
        DMK::Memory::seh_read<uintptr_t>(*c_player + Constants::C_PLAYER_LOOK_CONTROLLER_OFFSET);
    if (!controller || !DMK::Memory::plausible_userspace_ptr(*controller))
    {
        return;
    }
    // Ease the SCALAR pitch toward 0 (level). Both synchronized copies are written so any internal
    // current/target smoothing also settles at level. A sane pitch is within about +/- 1.6 rad; a
    // wild or non-finite value means the layout drifted, so that write is skipped.
    if (pitch_ease > 0.0f)
    {
        const uintptr_t pitch_addr = *controller + Constants::LOOK_CONTROLLER_PITCH_OFFSET;
        const auto pitch_value = DMK::Memory::seh_read<float>(pitch_addr);
        if (pitch_value && *pitch_value > -3.2f && *pitch_value < 3.2f)
        {
            const float levelled = *pitch_value * (1.0f - pitch_ease);
            *reinterpret_cast<float *>(pitch_addr) = levelled;
            *reinterpret_cast<float *>(*controller + Constants::LOOK_CONTROLLER_PITCH2_OFFSET) = levelled;
        }
    }

    // Set the look YAW (heading) to face the camera direction. The character moves along this heading,
    // so this is what turns the body and makes movement camera-relative on the idle -> moving edge.
    // Both synchronized copies are written.
    if (set_yaw)
    {
        *reinterpret_cast<float *>(*controller + Constants::LOOK_CONTROLLER_YAW_OFFSET) = yaw_value;
        *reinterpret_cast<float *>(*controller + Constants::LOOK_CONTROLLER_YAW2_OFFSET) = yaw_value;
    }
}

/**
 * @brief SEH wrapper for the player-aim control: a stale pointer or layout drift in the look chain
 *        must never crash the game. On a fault the aim is simply left unchanged for the frame.
 */
static void apply_orbit_aim_control(float pitch_ease, bool set_yaw, float yaw_value)
{
    __try
    {
        apply_orbit_aim_control_impl(pitch_ease, set_yaw, yaw_value);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

/**
 * @brief Forces the player BODY to face a world yaw, making movement+facing camera-relative.
 * @details The look controller (apply_orbit_aim_control) is aim-only: writing its yaw steers the
 *          movement frame but does not turn the body. The body heading is owned by the engine's
 *          animated-character layer. This replicates CAnimatedCharacter::ForceOverrideRotation
 *          (main vtable slot 95): set the override-active byte and store a yaw-only world quat
 *          (XYZW, rotation about +Z); the animated-character update copies it into the entity
 *          rotation for the frame, replacing the animation-derived facing, then clears the active
 *          byte. It is consume-once, so this is called every frame while the heading is held.
 *
 *          Resolution mirrors apply_orbit_aim_control_impl: g_env -> p_game -> CCryAction (cached) ->
 *          p_action_game -> C_Player (validated vtable) -> C_AnimatedHuman -> CAnimatedCharacter,
 *          validated by the animchar vtable before any write. The quat is written before the active
 *          byte so the game never observes active==1 with a torn/stale quat.
 *          See constants.hpp (ANIMCHAR_*, C_PLAYER_ANIMATED_HUMAN_OFFSET) for the offsets.
 */
static void apply_orbit_body_turn_impl(float target_yaw)
{
    const ModuleInfo &mod = module_info();
    const uintptr_t c_player = resolve_c_player();
    if (mod.base == 0 || c_player == 0)
    {
        return;
    }

    // C_Player -> C_AnimatedHuman (+0x268) -> CAnimatedCharacter (+0x20). Validate the animchar vtable
    // before touching its override fields; a mismatch means the layout drifted, so skip this frame.
    const auto animated_human =
        DMK::Memory::seh_read<uintptr_t>(c_player + Constants::C_PLAYER_ANIMATED_HUMAN_OFFSET);
    if (!animated_human || !DMK::Memory::plausible_userspace_ptr(*animated_human))
    {
        return;
    }
    const auto anim_char =
        DMK::Memory::seh_read<uintptr_t>(*animated_human + Constants::ANIMATED_HUMAN_ANIMCHAR_OFFSET);
    if (!anim_char || !DMK::Memory::plausible_userspace_ptr(*anim_char))
    {
        return;
    }
    const auto avt = DMK::Memory::seh_read<uintptr_t>(*anim_char);
    if (!avt || !DMK::Rtti::vtable_is_type(*avt, Constants::ANIMATED_CHARACTER_RTTI_NAME))
    {
        return;
    }

    // Yaw-only world quat (XYZW): rotation about +Z by target_yaw == {0, 0, sin(y/2), cos(y/2)}.
    const float half = target_yaw * 0.5f;
    const uintptr_t quat_addr = *anim_char + Constants::ANIMCHAR_OVERRIDE_ROT_QUAT_OFFSET;
    *reinterpret_cast<float *>(quat_addr + 0x0) = 0.0f;
    *reinterpret_cast<float *>(quat_addr + 0x4) = 0.0f;
    *reinterpret_cast<float *>(quat_addr + 0x8) = std::sin(half);
    *reinterpret_cast<float *>(quat_addr + 0xC) = std::cos(half);
    // Set the active byte LAST so the animated-character update (a separate consumer) never reads
    // active==1 with a half-written quat. The release fence orders the quat stores before the active
    // store explicitly rather than relying on the target's store-store ordering; on x86 it lowers to a
    // compiler barrier with no runtime cost.
    std::atomic_thread_fence(std::memory_order_release);
    *reinterpret_cast<volatile unsigned char *>(*anim_char + Constants::ANIMCHAR_OVERRIDE_ROT_ACTIVE_OFFSET) = 1;
}

/**
 * @brief SEH wrapper for the body-turn: a stale animated-character pointer or layout drift must
 *        never crash the game. On a fault the body is simply left unchanged for the frame.
 */
static void apply_orbit_body_turn(float target_yaw)
{
    __try
    {
        apply_orbit_body_turn_impl(target_yaw);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

/**
 * @brief Offsets the game view camera matrix behind the player and advances zoom/smoothing.
 * @details Reads the eye anchor and orientation from the untouched CView pose (position at
 *          SVIEWPARAMS_POSITION_OFFSET, quat at SVIEWPARAMS_ROTATION_OFFSET), derives the look
 *          basis from the quat (idempotent across same-frame rebuilds), computes the
 *          third-person pose, then writes the camera matrix translation and (on convergence/
 *          orbit) basis. The caller is about to compute the cull planes from this matrix, so
 *          culling matches the rendered view.
 * @param camera The game view's embedded render camera (matrix at offset 0).
 * @param cview The CView embedding the camera (camera - SVIEWPARAMS_VIEWMATRIX_OFFSET).
 * @param c_player Live C_Player address for the body anchor, or 0 to fall back to the eye anchor.
 * @param delta_time Seconds since the previous game-view frame (shared with the state poll).
 * @param view_blend Smoothstepped first-person(0) -> third-person(1) blend for the view-switch ease.
 */
static void offset_game_view_camera(uintptr_t camera, uintptr_t cview, uintptr_t c_player, float delta_time,
                                    float view_blend)
{
    LiveSettings &cfg = settings();
    CameraState &cam = camera_state();

    // Follow distance = configured base (hot-reloadable INI FollowDistance, re-read every
    // frame so an edit applies live) plus the accumulated zoom offset from the hold
    // keys (queried lock-free, render thread safe), clamped to the configured window.
    const DMK::InputManager &input = DMK::InputManager::get_instance();
    float zoom_offset = cam.zoom_offset.load(std::memory_order_relaxed);
    const float zoom_step = cfg.zoom_step.load(std::memory_order_relaxed);
    if (input.is_binding_active(k_zoom_in_binding))
    {
        zoom_offset -= zoom_step * delta_time; // zoom in pulls the camera closer
    }
    if (input.is_binding_active(k_zoom_out_binding))
    {
        zoom_offset += zoom_step * delta_time;
    }
    const float base_distance = cfg.follow_distance.load(std::memory_order_relaxed);
    const float distance = std::clamp(base_distance + zoom_offset,
                                      cfg.follow_distance_min.load(std::memory_order_relaxed),
                                      cfg.follow_distance_max.load(std::memory_order_relaxed));
    cam.zoom_offset.store(distance - base_distance, std::memory_order_relaxed);

    // Read the eye anchor and the orientation quat from the untouched CView pose. The basis is
    // derived from the quat (CryEngine column-vector convention: right = quat * +X, forward =
    // quat * +Y, up = quat * +Z). Reading the quat rather
    // than the matrix columns keeps the basis idempotent across the frustum builder's same-frame
    // rebuilds (we overwrite the matrix, never the pose, so the pose stays the clean eye input).
    // The matrix to offset is the camera's 3x4 at offset 0 (cview + SVIEWPARAMS_VIEWMATRIX_OFFSET).
    const Vector3 eye_position = *reinterpret_cast<const Vector3 *>(
        cview + Constants::SVIEWPARAMS_POSITION_OFFSET);
    const Quaternion eye_rotation = *reinterpret_cast<const Quaternion *>(
        cview + Constants::SVIEWPARAMS_ROTATION_OFFSET);
    const Vector3 right = eye_rotation.rotate(Vector3{1.0f, 0.0f, 0.0f});
    const Vector3 forward = eye_rotation.rotate(Vector3{0.0f, 1.0f, 0.0f});
    const Vector3 up = eye_rotation.rotate(Vector3{0.0f, 0.0f, 1.0f});
    GameStructures::Matrix34f *matrix = reinterpret_cast<GameStructures::Matrix34f *>(camera);
    // Per-preset FOV, smoothly crossing the "off" boundary. SetFrustum (sub_1805392FC) writes the render
    // CCamera's FOV scalar at camera+0x30 right before this builder, plus the cull-frustum edge vectors at
    // camera+0x50 / +0x58 / +0x60 / +0x68 / +0x70 (all proportional to 1/tan(fov/2), with +0x60 the tan term
    // itself). So camera+0x30 read here is the live GAME FOV (radians). cfg.fov is the per-preset TARGET in
    // degrees (0 = use the game FOV). We ease a rendered FOV toward (target>0 ? target : game FOV) with a
    // 2-stage critically-damped ease at the preset-blend rate, so turning a preset's FOV on/off GLIDES
    // through the game FOV instead of snapping across 0. We write it (rescaling the edges by
    // tan(new/2)/tan(game/2) so culling matches) only while a preset wants a FOV or is still easing back;
    // once settled at the game FOV we pass through, so the game's own dynamic FOV keeps working. TPV only.
    const float game_fov = *reinterpret_cast<const float *>(camera + Constants::CCAMERA_PROJECTION_FOV_OFFSET); // radians, freshly set by SetFrustum
    if (view_blend > 0.0f && game_fov > 0.05f && game_fov < 3.0f)
    {
        const float fov_target_degrees = cfg.fov.load(std::memory_order_relaxed);
        const float desired_fov =
            (fov_target_degrees > 0.0f) ? DMK::Math::degrees_to_radians(fov_target_degrees) : game_fov;
        const float speed = cfg.preset_blend_speed.load(std::memory_order_relaxed);
        if (!cam.fov_ease_valid) // first engaged frame after a first-person gap: snap, no ease across the gap
        {
            cam.fov_ease_stage1 = desired_fov;
            cam.fov_ease_applied = desired_fov;
            cam.fov_ease_valid = true;
        }
        else
        {
            const float alpha = (speed > 0.0f) ? (1.0f - std::exp(-speed * 1.6f * delta_time)) : 1.0f;
            cam.fov_ease_stage1 += (desired_fov - cam.fov_ease_stage1) * alpha; // 2-stage critically-damped, like the preset blend
            cam.fov_ease_applied += (cam.fov_ease_stage1 - cam.fov_ease_applied) * alpha;
        }
        // Override while a preset wants a FOV or while still easing back toward the game FOV; otherwise pass
        // through (preset off + settled) so the game keeps its own FOV (including any dynamic FOV).
        if ((fov_target_degrees > 0.0f || std::fabs(cam.fov_ease_applied - game_fov) > 0.005f) &&
            cam.fov_ease_applied > 0.05f && cam.fov_ease_applied < 3.0f)
        {
            const float ratio = std::tan(cam.fov_ease_applied * 0.5f) / std::tan(game_fov * 0.5f);
            *reinterpret_cast<float *>(camera + Constants::CCAMERA_PROJECTION_FOV_OFFSET) = cam.fov_ease_applied; // projection FOV (radians)
            *reinterpret_cast<float *>(camera + Constants::CCAMERA_CULL_EDGE_0_OFFSET) *= ratio;                 // cull edges (proportional to 1/tan)
            *reinterpret_cast<float *>(camera + Constants::CCAMERA_CULL_EDGE_1_OFFSET) *= ratio;
            *reinterpret_cast<float *>(camera + Constants::CCAMERA_CULL_TAN_OFFSET) /= ratio;                    // = tan term (inverse)
            *reinterpret_cast<float *>(camera + Constants::CCAMERA_CULL_EDGE_3_OFFSET) *= ratio;
            *reinterpret_cast<float *>(camera + Constants::CCAMERA_CULL_EDGE_4_OFFSET) *= ratio;
        }
    }
    // First-person camera position -- the blend source for the view-switch ease. Sourced from the
    // untouched eye pose (cview + SVIEWPARAMS_POSITION_OFFSET, same as eye_position above), NOT the
    // matrix translation this function overwrites, so a second same-frame frustum rebuild does not
    // compound the FPV->TPV lerp (the same idempotency the rotation basis and the TPV anchor rely on).
    // The engine builds the matrix translation from this eye pose, so at view_blend 0 the matrix is
    // left exactly first person.
    const Vector3 fpv_position = eye_position;

    // Over-the-shoulder lateral offset along the eye-right axis, applied to the non-orbit follow
    // pose. During free-look it is folded into the orbit start offset (below) so it revolves with
    // the ring, which keeps engaging free-look continuous.
    const Vector3 lateral_offset = right * cfg.offset_right.load(std::memory_order_relaxed);

    // Anchor the rig to a STABLE point. The first-person eye (eye_position, read from CView+0x14)
    // carries head-bob, breathing and weapon-sway, so anchoring the third-person camera to it
    // shakes the whole view. Instead anchor to the player BODY origin -- column 3 of the entity
    // world matrix at OFFSET_ENTITY_WORLD_MATRIX_MEMBER (Matrix34, translation at m[*][3]) -- which
    // does not bob -- lifted by EyeHeight to roughly eye level along world up. Falls back to the eye
    // anchor when the feature is off (EyeHeight <= 0) or the entity/world matrix is unavailable, so
    // a missing player or a layout drift degrades to the previous behaviour rather than misplacing
    // the camera.
    const Vector3 world_up{0.0f, 0.0f, 1.0f};
    const float eye_height = cfg.eye_height.load(std::memory_order_relaxed);
    // Read the player body world origin once. It feeds the stable anchor (below) AND the
    // device-agnostic movement speed used by the camera-relative move alignment; body_valid gates both.
    Vector3 body_origin{0.0f, 0.0f, 0.0f};
    bool body_valid = false;
    {
        // Resolve the entity FRESH from the live C_Player every frame (rather than trusting a mirrored
        // pointer that goes null/stale across view transitions and reloads), which is what keeps the
        // move-detection (hence the camera-relative body-turn and the body anchor) locked onto the
        // CURRENT player. On a failed walk body_valid stays false and the camera degrades to the eye anchor.
        uintptr_t entity_addr = 0;
        if (c_player != 0)
        {
            const auto ent = DMK::Memory::seh_read<uintptr_t>(c_player + Constants::C_PLAYER_ENTITY_OFFSET);
            if (ent && DMK::Memory::plausible_userspace_ptr(*ent))
            {
                entity_addr = *ent;
            }
        }
        if (entity_addr != 0 && DMK::Memory::plausible_userspace_ptr(entity_addr))
        {
            const auto world_matrix = DMK::Memory::seh_read<GameStructures::Matrix34f>(
                entity_addr + Constants::OFFSET_ENTITY_WORLD_MATRIX_MEMBER);
            if (world_matrix)
            {
                body_origin = Vector3{world_matrix->m[0][3], world_matrix->m[1][3], world_matrix->m[2][3]};
                body_valid = true;
            }
        }
    }
    // Publish the live REAL eye height (FP eye above the body root) for the overlay read-out and as the
    // dynamic-eye-sync source. eye_position carries head bob; body_origin is the bob-free feet origin.
    if (body_valid)
    {
        cam.real_eye_height.store(eye_position.z - body_origin.z, std::memory_order_relaxed);
    }
    Vector3 anchor_base = eye_position;
    if (body_valid && eye_height > 0.0f)
    {
        float effective_eye_height = eye_height;
        bool sync_engaged = false; // true while actively re-anchoring (vs. just using the configured height)
        if (cfg.dynamic_eye_sync.load(std::memory_order_relaxed))
        {
            // Re-anchor the eye HEIGHT to the real FP eye when a low pose (kneel / pray, and other poses
            // EyeHeight does not model) drops it OUT OF RANGE of the configured height -- where a fixed
            // height floats the camera too high. Within the threshold (head bob / a minor pose change) keep
            // the steady bob-free height; in the re-anchored low poses we accept the bob (they are mostly
            // stationary). Ease so the swap slides; eye_sync_valid resets on suppression so re-engaging snaps.
            constexpr float k_oor_threshold = 0.25f; // meters: a genuine pose drop, not head bob
            constexpr float k_sync_rate = 9.0f;       // ease rate, 1/sec (frame-rate independent below)
            const float real_eye_height = eye_position.z - body_origin.z;
            sync_engaged = std::fabs(real_eye_height - eye_height) > k_oor_threshold;
            const float target_eye_height = sync_engaged ? real_eye_height : eye_height;
            if (!cam.eye_sync_valid)
            {
                cam.eye_sync_applied = target_eye_height;
                cam.eye_sync_valid = true;
            }
            else
            {
                const float k = 1.0f - std::exp(-k_sync_rate * delta_time);
                cam.eye_sync_applied += (target_eye_height - cam.eye_sync_applied) * k;
            }
            effective_eye_height = cam.eye_sync_applied;
        }
        else
        {
            cam.eye_sync_valid = false; // feature off -> the next engage snaps to the current pose
        }
        // Publish for the overlay status line (actively re-anchoring vs. using the configured height).
        cam.eye_sync_engaged.store(sync_engaged, std::memory_order_relaxed);
        cam.eye_sync_effective.store(effective_eye_height, std::memory_order_relaxed);
        anchor_base = body_origin + world_up * effective_eye_height;
    }
    else
    {
        cam.eye_sync_engaged.store(false, std::memory_order_relaxed);
    }
    const Vector3 pivot = anchor_base + world_up * cfg.offset_up.load(std::memory_order_relaxed);

    // Free-look orbit angles (degrees), accumulated by the input hook while the key is
    // held. On release they ease back to center. Read here so the render reflects them.
    const bool orbit_held = cam.orbit_active.load(std::memory_order_relaxed);

    // Gamepad right-stick orbit: integrate the latched stick DEFLECTION by RATE into the SAME orbit
    // accumulator the mouse writes, so the stick orbits the camera (yaw) and raises/lowers it (pitch) like
    // the mouse. Done here (not in the input hook) because the stick reports a HELD position, so it needs
    // delta_time to be a frame-rate-independent turn rate and to keep moving while held steady. Yaw matches
    // the mouse (stick-right orbits right); the right-stick Y reads OPPOSITE the mouse look delta, so it is
    // negated below (stick-up raises the camera, like the mouse). GamepadOrbitSpeed (deg/sec at full stick)
    // sets the rate -- a SEPARATE knob from the mouse's OrbitSensitivity, because a relative mouse (delta)
    // and an absolute stick (rate) cannot share one linear scale; the pitch is clamped to the same limits.
    // Gated on the cursor-shown freeze (a UI up holds
    // still, mirroring the mouse path); when not orbiting or frozen the latch is cleared so re-engaging with
    // the stick centred does not jump.
    if (orbit_held && !s_cursor_shown.load(std::memory_order_relaxed))
    {
        const float pad_yaw = cam.orbit_pad_yaw.load(std::memory_order_relaxed);
        const float pad_pitch = cam.orbit_pad_pitch.load(std::memory_order_relaxed);
        if (pad_yaw != 0.0f || pad_pitch != 0.0f)
        {
            const float step = cfg.gamepad_orbit_speed.load(std::memory_order_relaxed) * delta_time;
            cam.orbit_yaw.store(cam.orbit_yaw.load(std::memory_order_relaxed) - pad_yaw * step, std::memory_order_relaxed);
            const float pitch = cam.orbit_pitch.load(std::memory_order_relaxed) - pad_pitch * step;
            cam.orbit_pitch.store(std::clamp(pitch,
                                            cfg.orbit_pitch_min.load(std::memory_order_relaxed),
                                            cfg.orbit_pitch_max.load(std::memory_order_relaxed)),
                                 std::memory_order_relaxed);
        }
    }
    else
    {
        cam.orbit_pad_yaw.store(0.0f, std::memory_order_relaxed);
        cam.orbit_pad_pitch.store(0.0f, std::memory_order_relaxed);
    }

    // Wrap the accumulated yaw into [-180, 180] for the orbit math. The orbit is periodic in yaw,
    // so this is invisible while orbiting, but on release it lets the camera ease back to centre
    // along the SHORTEST path (<= 180 deg) in one smooth pass instead of un-spinning every full
    // revolution the player made. The re-based value is only stored back on release (below), so the wrap
    // here stays render-thread-local while held. The raw accumulator itself is written by the mouse path
    // on the input thread and by the gamepad integration above on the render thread; only one input device
    // drives orbit at a time in practice, so the relaxed load here does not race a meaningful write (and a
    // dropped sub-degree delta in the unlikely simultaneous case is harmless, never torn).
    float orbit_yaw_deg = std::remainder(cam.orbit_yaw.load(std::memory_order_relaxed), 360.0f);
    float orbit_pitch_deg = cam.orbit_pitch.load(std::memory_order_relaxed);

    // While moving with a captured heading, the camera holds its WORLD yaw and the body turns UNDER it.
    // Derive the rig's orbit angle as (camera_world_yaw - current_body_forward_yaw), where camera_world_yaw =
    // the captured heading plus any orbit the player has added since capture. On the capture frame the body
    // has not turned yet, so this equals the pre-move orbit (no snap); as the eye+body rotate to the heading
    // the angle eases to the residual orbit, so the camera POSITION is identical across the turn (no pop).
    // forward is the untouched eye look (read at frame start), so it tracks the body one frame behind --
    // exactly in step with the body-turn's own one-frame apply latency. cam.orbit_yaw stays the raw input
    // accumulator throughout; the rendered angle is baked back only on release/stop so free-look resumes
    // from exactly where the moving camera was. orbit_moving implies the key was held, so this also runs on
    // the release frame (orbit_moving is cleared at end of frame), giving the bake-on-release below.
    const bool move_orbit = cam.orbit_moving && cam.orbit_target_valid;
    if (move_orbit)
    {
        const float char_forward_yaw = std::atan2(-forward.x, forward.y);
        const float raw_user_since_deg =
            cam.orbit_yaw.load(std::memory_order_relaxed) - cam.orbit_yaw_at_capture_deg;
        // In continuous-align (GTA) mode the user's orbit-since-capture STEERS the run: it drives
        // body_target_yaw (the look + body heading) below, and the camera follows that heading (the
        // derived rig angle cancels to ~0, keeping the camera behind). Smooth that steering input when
        // OrbitSmoothing is on so the turn is fluid instead of tracking the raw per-frame mouse deltas;
        // the SAME smoothed value is reused for body_target_yaw below so the camera stays behind the body.
        // Hold mode keeps the raw value (its orbit-around rides the snapped rig offset, left instant) --
        // only continuous-align steering is smoothed.
        const float orbit_smoothing = std::clamp(cfg.orbit_smoothing.load(std::memory_order_relaxed), 0.0f, 1.0f);
        const bool steer_smooth =
            cfg.orbit_continuous_align.load(std::memory_order_relaxed) && orbit_smoothing > 1e-4f;
        if (!steer_smooth || !cam.orbit_steer_valid)
        {
            cam.orbit_steer_smooth = raw_user_since_deg; // snap: smoothing off / hold mode, or first move frame
            cam.orbit_steer_valid = true;
        }
        else
        {
            const float speed = k_orbit_smooth_max_speed -
                                orbit_smoothing * (k_orbit_smooth_max_speed - k_orbit_smooth_min_speed);
            const float ease = 1.0f - std::exp(-speed * delta_time);
            cam.orbit_steer_smooth += std::remainder(raw_user_since_deg - cam.orbit_steer_smooth, 360.0f) * ease;
        }
        const float user_orbit_since_deg = steer_smooth ? cam.orbit_steer_smooth : raw_user_since_deg;
        orbit_yaw_deg = std::remainder(
            DMK::Math::radians_to_degrees(cam.orbit_target_yaw - char_forward_yaw) + user_orbit_since_deg, 360.0f);
    }
    else
    {
        cam.orbit_steer_valid = false; // not move-orbiting: the next engage snaps the steer low-pass
    }

    if (!orbit_held)
    {
        // Orbit toggle released. When a move-orbit was active, orbit_yaw_deg already holds the rendered
        // (world-stable) angle derived above, not the raw input accumulator, so the ease-back below starts
        // from where the camera actually is rather than jumping by the orbit held at capture. The single
        // store at the end of this block persists the re-based (and, with a return speed, eased) angle.
        const float return_speed = cfg.orbit_return_speed.load(std::memory_order_relaxed);
        if (return_speed > 0.0f)
        {
            // Ease back to the centre: directly behind and level (hardcoded 0, 0).
            const float init_yaw = 0.0f;
            const float init_pitch = 0.0f;
            const float ease = 1.0f - std::exp(-return_speed * delta_time);
            orbit_yaw_deg -= std::remainder(orbit_yaw_deg - init_yaw, 360.0f) * ease; // shortest path to centre
            orbit_pitch_deg -= (orbit_pitch_deg - init_pitch) * ease;
        }
        // Store the re-based (and, with a return speed, eased) angles so the next frame resumes from the
        // short-path value; also runs with return_speed == 0 so the held-spin accumulator is bounded the
        // moment the key is released.
        cam.orbit_yaw.store(orbit_yaw_deg, std::memory_order_relaxed);
        cam.orbit_pitch.store(orbit_pitch_deg, std::memory_order_relaxed);
    }

    // Temporal low-pass on the orbit angle. orbit_yaw_deg / orbit_pitch_deg above are the raw target
    // (the accumulated mouse input, or the eased return / world-stable move derivation); the rig is
    // built from a smoothed copy that eases toward it each frame so free-look is not as jittery as the
    // unfiltered per-frame deltas (the engine's own look smoothing runs past the dispatch we block).
    // orbit_smoothing 0 = off (snap, raw). Snap on the first engaged frame (orbit_render_valid false) so
    // the camera does not slide in from centre. SNAP ALSO while move-orbiting (a movement key is driving
    // the camera-relative turn): there orbit_yaw_deg is the WORLD-STABLE derivation that cancels the body
    // turn to keep the camera planted, so it must track the body 1:1 -- smoothing it would lag the
    // cancellation and swing the camera on the move-start, when it should be instant. The target writes
    // above always use the unsmoothed value, so the accumulator and the return/move logic are unaffected;
    // only the idle (standing free-look) rendered angle is low-passed.
    const float orbit_smoothing = std::clamp(cfg.orbit_smoothing.load(std::memory_order_relaxed), 0.0f, 1.0f);
    if (orbit_smoothing <= 1e-4f || !cam.orbit_render_valid || move_orbit)
    {
        cam.orbit_yaw_render = orbit_yaw_deg;
        cam.orbit_pitch_render = orbit_pitch_deg;
        cam.orbit_render_valid = true;
    }
    else
    {
        const float smooth_speed =
            k_orbit_smooth_max_speed - orbit_smoothing * (k_orbit_smooth_max_speed - k_orbit_smooth_min_speed);
        const float smooth_ease = 1.0f - std::exp(-smooth_speed * delta_time);
        // Shortest-path ease on yaw so a wrap (e.g. -179 -> +179) eases the short way, not all the way round.
        cam.orbit_yaw_render += std::remainder(orbit_yaw_deg - cam.orbit_yaw_render, 360.0f) * smooth_ease;
        cam.orbit_pitch_render += (orbit_pitch_deg - cam.orbit_pitch_render) * smooth_ease;
    }
    const float render_yaw_deg = cam.orbit_yaw_render;
    const float render_pitch_deg = cam.orbit_pitch_render;

    // "Orbiting" while EITHER the raw target or the still-easing rendered angle is off-centre, so the
    // orbit block keeps running until the smoothed angle has fully settled back (otherwise a release
    // would cut the rig straight to centre before the low-pass finishes).
    const bool orbiting = orbit_yaw_deg < -0.05f || orbit_yaw_deg > 0.05f ||
                          orbit_pitch_deg < -0.05f || orbit_pitch_deg > 0.05f ||
                          render_yaw_deg < -0.05f || render_yaw_deg > 0.05f ||
                          render_pitch_deg < -0.05f || render_pitch_deg > 0.05f;

    // Ease the orbit "level" blend toward 1 ONLY while the orbit key is held, and back to 0 on
    // release. While held the orbit is built from a LEVEL reference (so a steep look does not tip it
    // near the overhead pole); easing in/out keeps engaging and releasing smooth. Keying this on
    // orbit_held alone (not on "orbiting") is deliberate: with OrbitReturnSpeed 0 the angles "stay"
    // and "orbiting" latches true forever, so keying off it would pin the camera to the level
    // reference and lock out look pitch (only the head would move, the view would not). De-leveling
    // on release instead lets any retained orbit angle ride as a rigid offset on top of the real
    // look (the level_blend 0 path follows pitch), so vertical control is preserved in every preset.
    {
        constexpr float k_orbit_level_speed = 8.0f;
        const float level_target = orbit_held ? 1.0f : 0.0f;
        const float level_ease = 1.0f - std::exp(-k_orbit_level_speed * delta_time);
        cam.orbit_level_blend += (level_target - cam.orbit_level_blend) * level_ease;
    }

    // The real aim is driven AFTER the orbit look is finalized (below), so the heading can be aligned
    // to the camera's forward direction on the idle -> moving edge.

    // Centered base (no shoulder), straight back from the pivot. The shoulder is re-applied
    // below. The orbit rotation is applied on top, not folded into this base, so the orbit
    // stays instantly responsive and never accumulates into the follow position. The follow
    // is instant: the anchor is the de-bobbed body origin, so there is no shake to smooth out.
    const Vector3 centered_position = pivot - forward * distance;

    // Final camera position with the fixed shoulder applied (the normal, non-orbit pose).
    Vector3 camera_position = centered_position + lateral_offset;

    // Aim focus point. The off-axis (shoulder/height) camera looks at a point on the aim line so the
    // screen-centre crosshair lands on what the player points at: focus_point = anchor + forward *
    // focus_distance, and the camera toes in toward it by ~ shoulder / focus_distance.
    //
    // focus_distance auto-tracks the live follow distance, deliberately NOT the per-frame scene depth.
    // Driving the toe-in from a raycast hit distance is what made the camera rotate toward whatever sat
    // under the crosshair -- the "magnet" on hover, the shake on a swing, the orbit jitter. The toe-in
    // is ~ shoulder / distance, so any change in the hovered depth rotates the whole view, and no filter
    // can remove a rotation the mechanism is built to produce. Tying the focus to the follow distance
    // (which changes only, and slowly, when the player zooms) keeps it decoupled from the scene, so it
    // is stable by construction: the convergence scales with the zoom (closer follow -> nearer focus)
    // and the player never has to set a second distance by hand. The focus is built from anchor_base
    // (the de-bobbed body anchor), not the bobbing first-person eye, so head-bob and weapon-sway never
    // reach it. The convergence moves only the RENDERED look; the game's own interaction ray is never
    // touched.
    // AimFocusDistance pins the convergence depth (0 = auto: track the live follow distance, as the
    // comment above describes). Then bound the toe-in: a small follow distance with a shoulder offset
    // would otherwise converge so steeply that the crosshair misses everything past the near focus
    // (the ~45 deg case at FollowDistance 0.5, OffsetRight 1.0). Raise focus_distance so the toe-in
    // atan(|offset_right| / (focus_distance + distance)) stays within k_tan_max_convergence.
    float focus_distance = cfg.aim_focus_distance.load(std::memory_order_relaxed);
    if (focus_distance <= 0.0f)
    {
        focus_distance = distance;
    }
    const float shoulder_abs = std::abs(cfg.offset_right.load(std::memory_order_relaxed));
    if (shoulder_abs > 1e-4f)
    {
        const float min_reach = shoulder_abs / k_tan_max_convergence; // lower bound on focus_distance + distance
        focus_distance = std::max(focus_distance, min_reach - distance);
    }
    const bool have_focus = focus_distance > 0.0f;
    const Vector3 focus_point = anchor_base + forward * focus_distance;

    // Converge the look on the focus point so the off-axis (shoulder/height) camera keeps the
    // screen-centre crosshair on the aim target: the camera looks straight at the focus point, so
    // the shoulder/height offset no longer makes the crosshair miss. The orbit below carries this
    // converged look RIGIDLY (it rotates with the rig), so the crosshair holds its screen position
    // while free-looking instead of re-aiming at the far focus point each frame.
    Vector3 look_forward = forward;
    bool rebuild_basis = false;
    if (have_focus)
    {
        const Vector3 to_focus = focus_point - camera_position;
        if (to_focus.magnitude() > 1e-4f)
        {
            look_forward = to_focus.normalized();
            rebuild_basis = true;
        }
    }

    // Static follow angle: orbit the RESTING camera around the pivot by FollowYaw / FollowPitch
    // (degrees; 0,0 = directly behind and level). Positive yaw swings the camera to one side, positive
    // pitch raises it. The offset AND the converged look rotate TOGETHER (same convention as the
    // free-look orbit below), so the over-the-shoulder framing and crosshair hold while the camera
    // circles you. The free-look orbit then rotates further on top of this resting angle.
    {
        const float follow_yaw = cfg.follow_yaw.load(std::memory_order_relaxed);
        const float follow_pitch = cfg.follow_pitch.load(std::memory_order_relaxed);
        if (follow_yaw < -0.05f || follow_yaw > 0.05f || follow_pitch < -0.05f || follow_pitch > 0.05f)
        {
            Vector3 off = camera_position - pivot;
            const float yaw = DMK::Math::degrees_to_radians(follow_yaw);
            const float cy = std::cos(yaw);
            const float sy = std::sin(yaw);
            auto yaw_about_z = [cy, sy](const Vector3 &v) {
                return Vector3{v.x * cy - v.y * sy, v.x * sy + v.y * cy, v.z};
            };
            off = yaw_about_z(off);
            look_forward = yaw_about_z(look_forward);

            const float pitch = DMK::Math::degrees_to_radians(follow_pitch);
            if (pitch < -1e-5f || pitch > 1e-5f)
            {
                const float azimuth = std::atan2(off.y, off.x);
                const Vector3 axis{std::sin(azimuth), -std::cos(azimuth), 0.0f};
                const float cp = std::cos(pitch);
                const float sp = std::sin(pitch);
                auto rotate_about_axis = [&axis, cp, sp](const Vector3 &v) {
                    const Vector3 cross = axis.cross(v);
                    const float dot = axis.x * v.x + axis.y * v.y + axis.z * v.z;
                    return v * cp + cross * sp + axis * (dot * (1.0f - cp));
                };
                off = rotate_about_axis(off);
                look_forward = rotate_about_axis(look_forward);
            }

            camera_position = pivot + off;
            if (look_forward.magnitude_squared() > 1e-6f)
            {
                look_forward = look_forward.normalized();
            }
            rebuild_basis = true;
        }
    }

    // Free-look orbit: rigidly rotate the WHOLE non-orbit rig (the camera's offset from the pivot
    // AND the converged look direction) around the pivot -- yaw about world up, then pitch about the
    // heading's horizontal right axis. Because the offset (shoulder + height + distance) and the
    // aim-convergence toe-in rotate TOGETHER, the over-the-shoulder framing and the crosshair keep
    // their screen positions while the camera circles the player, and there is no far focus anchor
    // that blows up near the front. Yaw is unbounded so full spins work; the target elevation is
    // clamped away from the pole and the same clamped delta rotates the look so the two agree. At
    // zero angle the rotation is identity (continuous engage); looking at the ground still circles
    // the player (the offset's elevation is preserved) and mouse up/down raises/lowers the camera.
    if (orbiting || cam.orbit_level_blend > 0.001f)
    {
        const float level_blend = cam.orbit_level_blend;

        // ACTUAL base: the live follow offset and converged look -- encodes the player's look pitch.
        const Vector3 actual_offset = camera_position - pivot;
        const Vector3 actual_look = look_forward;

        // LEVEL base: as if the player looked straight ahead. A steep up/down look otherwise puts the
        // orbit near the overhead pole where the rotation gets messy, so while orbiting the rig is
        // blended to this level reference. The height stays in the pivot, so the level camera sits at
        // eye level straight behind the heading with the shoulder, and the look converges on the
        // level aim line so the crosshair stays compensated.
        Vector3 level_fwd{forward.x, forward.y, 0.0f};
        if (level_fwd.magnitude_squared() > 1e-6f)
        {
            level_fwd = level_fwd.normalized();
        }
        else
        {
            level_fwd = world_up.cross(right).normalized(); // straight down/up: recover heading from right
        }
        const Vector3 level_right = level_fwd.cross(world_up).normalized();
        const Vector3 level_offset = level_fwd * (-distance) +
                                    level_right * cfg.offset_right.load(std::memory_order_relaxed);
        Vector3 level_look = have_focus ? (pivot + level_fwd * focus_distance) - (pivot + level_offset)
                                      : level_offset * -1.0f; // no focus: look at the pivot
        level_look = (level_look.magnitude_squared() > 1e-6f) ? level_look.normalized() : level_fwd;

        // Blend actual -> level by the eased level_blend (0 = follow pose, 1 = leveled). At 0 this is
        // exactly the non-orbit pose, so engaging/leaving free-look is continuous; the blend carries
        // the transition smoothly instead of snapping between the steep and level poses.
        const Vector3 offset0 = actual_offset + (level_offset - actual_offset) * level_blend;
        Vector3 base_look = actual_look + (level_look - actual_look) * level_blend;
        base_look = (base_look.magnitude_squared() > 1e-6f) ? base_look.normalized() : actual_look;

        float radius = offset0.magnitude();
        if (radius < 1e-3f)
        {
            radius = distance; // degenerate (camera at the pivot): nothing to rotate about
        }
        const float base_elevation = std::asin(std::clamp(offset0.z / radius, -1.0f, 1.0f));
        const float elevation =
            std::clamp(base_elevation + DMK::Math::degrees_to_radians(render_pitch_deg), -1.45f, 1.45f);
        const float yaw_delta = DMK::Math::degrees_to_radians(render_yaw_deg);
        const float pitch_delta = elevation - base_elevation;

        // Yaw about world up (Z): applied to both the offset and the look so they swing together.
        const float cy = std::cos(yaw_delta);
        const float sy = std::sin(yaw_delta);
        auto yaw_about_z = [cy, sy](const Vector3 &v) {
            return Vector3{v.x * cy - v.y * sy, v.x * sy + v.y * cy, v.z};
        };
        Vector3 new_offset = yaw_about_z(offset0);
        Vector3 new_look = yaw_about_z(base_look);

        // Pitch about the horizontal right axis of the yawed heading (Rodrigues). The axis
        // (sin az, -cos az, 0) is the one about which a positive pitch_delta raises the camera.
        if (pitch_delta < -1e-5f || pitch_delta > 1e-5f)
        {
            const float azimuth = std::atan2(new_offset.y, new_offset.x);
            const Vector3 pitch_axis{std::sin(azimuth), -std::cos(azimuth), 0.0f};
            const float cp = std::cos(pitch_delta);
            const float sp = std::sin(pitch_delta);
            auto rotate_about_axis = [&pitch_axis, cp, sp](const Vector3 &v) {
                const Vector3 cross = pitch_axis.cross(v);
                const float dot = pitch_axis.x * v.x + pitch_axis.y * v.y + pitch_axis.z * v.z;
                return v * cp + cross * sp + pitch_axis * (dot * (1.0f - cp));
            };
            new_offset = rotate_about_axis(new_offset);
            new_look = rotate_about_axis(new_look);
        }

        camera_position = pivot + new_offset;
        if (new_look.magnitude_squared() > 1e-6f)
        {
            look_forward = new_look.normalized();
        }
        rebuild_basis = true;
    }

    // Camera-relative movement (toggle orbit): on the idle -> moving edge CAPTURE the camera heading, then
    // HOLD it while moving. The body turn (apply_orbit_body_turn) pins the body to that heading, which is
    // what makes locomotion camera-relative in EVERY direction (KCD2 moves relative to the body rotation):
    // W runs away from the camera, A/D strafe to the sides, S backs toward it. The look-yaw write
    // (apply_orbit_aim_control) faces the head/aim the same way and feeds the camera-position derivation.
    // Both are re-applied every frame because the engine reverts/consumes a single write -- like the pitch
    // leveling. Released when movement stops.
    bool do_align = false;
    // Camera-relative movement, keyed on the device-agnostic action-input intent
    // (player_onaction_move_magnitude, from the action dispatcher). The input is nonzero the instant a movement
    // key is pressed -- BEFORE the body accelerates -- so the heading captures immediately and the
    // character turns to the camera direction without first moving the old way; and it stays nonzero while
    // a key is held against a wall, so the heading is not falsely released on a collision arrest. The
    // body-position speed is deliberately NOT used (it lags the intent and reads zero when arrested), so
    // the feature requires the action hook -- if it did not resolve, camera-relative movement stays off
    // (free-look still works).
    if (orbit_held && body_valid && player_onaction_available())
    {
        bool moving = cam.orbit_moving;
        const float move_magnitude = player_onaction_move_magnitude();
        // Re-arm guard: a genuine release (magnitude below the stop threshold) must be observed since orbit
        // engaged before a move-start is honoured. A stranded latch -- a held-move release swallowed on a
        // combat action-map swap (see player_onaction_reset) -- reads > 0 with the keys up; without this guard
        // it would re-trip orbit_moving the instant orbit restores and drive the body-turn with no input (the
        // post-combat self-rotation). Arming only on a sub-stop reading means a fresh, observed press engages it.
        static bool s_stale_suppress_logged = false;
        if (move_magnitude < k_orbit_move_input_stop)
        {
            cam.orbit_move_armed = true;
            s_stale_suppress_logged = false;
        }
        if (!moving && move_magnitude > k_orbit_move_input_start)
        {
            if (cam.orbit_move_armed)
            {
                moving = true;
                do_align = true; // idle -> moving edge: capture the camera heading
                // Diagnostic: what tripped the body-turn. move_magnitude near 1.0 with a movement key/stick =
                // genuine locomotion; a smaller or unexpected value while the player is only free-looking
                // points at game-driven movement (finishing-move / combat footwork) being mistaken for intent.
                // state is the debounced GameState mask (see game_state.hpp) so we can tell whether a combat /
                // aiming / cinematic state was active when the body-turn engaged.
                DMK::Logger::get_instance().trace(
                    "Orbit: move-orbit START (body-turn engaging) -- move_magnitude={:.2f}, continuous_align={}, "
                    "state_mask=0x{:X}",
                    move_magnitude, cfg.orbit_continuous_align.load(std::memory_order_relaxed),
                    game_state_mask().load(std::memory_order_relaxed));
            }
            else if (!s_stale_suppress_logged)
            {
                // Unarmed (no release seen since orbit engaged) yet reading as movement: a stranded latch.
                // Suppress the body-turn re-trip and log it once -- the diagnostic for the post-combat case.
                DMK::Logger::get_instance().trace(
                    "Orbit: suppressed a stale move re-trip (magnitude {:.2f}, no release observed since orbit "
                    "engaged; likely a movement latch stranded by a combat action-map swap)",
                    move_magnitude);
                s_stale_suppress_logged = true;
            }
        }
        else if (moving && move_magnitude < k_orbit_move_input_stop)
        {
            // Release the latch the instant the movement keys are let go (no debounce tail), so stopping
            // and then rotating is immediately pure free-look and the character does not keep turning to
            // the camera. No bridge is needed the way body-speed required: the action input stays nonzero
            // while any key is held (diagonals, overlapping key-switches), so a drop to zero is a genuine
            // stop; a full release-then-repress simply re-captures the heading on the new press, which is
            // the correct behaviour.
            moving = false;
            // Bake the world-stable orbit angle back into the accumulator so free-look resumes from
            // exactly where the moving camera was, with no jump the instant movement stops.
            cam.orbit_yaw.store(orbit_yaw_deg, std::memory_order_relaxed);
            DMK::Logger::get_instance().trace("Orbit: move-orbit STOP (body-turn releasing) -- move_magnitude={:.2f}",
                                              move_magnitude);
        }
        cam.orbit_moving = moving;
    }
    else
    {
        cam.orbit_moving = false;
        cam.orbit_move_armed = false; // require a fresh observed release after re-engaging orbit
    }

    // Capture the heading on move-start. Use the camera's POSITIONAL world yaw -- the eye-look yaw plus
    // the orbit applied this frame -- NOT atan2(look_forward). look_forward is the CONVERGED look: with
    // aim-convergence on, the over-the-shoulder camera toes its look INWARD toward the aim point by a
    // shoulder-dependent angle. Capturing that toed-in yaw would rotate the rig by the toe-in on the
    // move transition, shifting the camera sideways toward the shoulder every time you press W. The
    // positional yaw is exactly what the world-stable orbit derivation above renders, so the camera
    // stays put as the body turns. It is also the more intuitive move direction (straight away from the
    // camera rather than toed-in toward the crosshair).
    if (do_align)
    {
        const float eye_forward_yaw = std::atan2(-forward.x, forward.y);
        cam.orbit_target_yaw = eye_forward_yaw + DMK::Math::degrees_to_radians(orbit_yaw_deg);
        cam.orbit_target_valid = true;
        // Snapshot the orbit input so further orbiting while moving is measured from here. We do NOT
        // reset the orbit to 0: the world-stable derivation above holds the camera in place while the
        // body turns to this heading, so there is no snap-behind pop.
        cam.orbit_yaw_at_capture_deg = cam.orbit_yaw.load(std::memory_order_relaxed);
    }
    if (!cam.orbit_moving)
    {
        cam.orbit_target_valid = false;
    }

    const float pitch_ease = (orbit_held && cfg.orbit_level_aim.load(std::memory_order_relaxed))
                                ? (1.0f - std::exp(-k_orbit_aim_level_speed * delta_time))
                                : 0.0f;
    const bool hold_yaw = orbit_held && cam.orbit_moving && cam.orbit_target_valid;
    // Heading fed to the body + movement while moving. CONTINUOUS-ALIGN (GTA style): follow the CURRENT
    // camera world yaw every frame (captured heading + the orbit added since), so orbiting STEERS the run
    // and the rig stays behind the character. Otherwise HOLD the heading captured on move-start (orbit
    // looks around freely while the character keeps its line). The world-stable camera derivation above is
    // the same either way -- only this target differs -- so in continuous mode the derived orbit angle
    // cancels to ~0 (camera behind the character) while in hold mode it carries the orbit-around offset.
    float body_target_yaw = cam.orbit_target_yaw;
    if (hold_yaw && cfg.orbit_continuous_align.load(std::memory_order_relaxed))
    {
        // Reuse the smoothed steer angle computed in the move-orbit derivation above (when OrbitSmoothing
        // is on) so the body/look heading and the rig agree -- the camera follows the smoothed steering
        // and stays behind. With smoothing off this is the raw orbit-since-capture. The smoothed value is
        // only current once the move-orbit block above has run for it (cam.orbit_steer_valid); on the
        // idle->moving capture frame that block did NOT run (move_orbit read the pre-update orbit_moving,
        // which was still false), so fall back to the raw orbit-since-capture -- ~0 on the capture frame
        // because orbit_yaw_at_capture_deg was just snapshotted -- instead of a stale smoothed value from a
        // prior move. orbit_smoothing was read for the orbit-angle low-pass above and is constant across
        // the frame, so it is reused here.
        const float user_orbit_since_deg =
            (orbit_smoothing > 1e-4f && cam.orbit_steer_valid)
                ? cam.orbit_steer_smooth
                : (cam.orbit_yaw.load(std::memory_order_relaxed) - cam.orbit_yaw_at_capture_deg);
        body_target_yaw = cam.orbit_target_yaw + DMK::Math::degrees_to_radians(user_orbit_since_deg);
    }
    if (orbit_held && (pitch_ease > 0.0f || hold_yaw))
    {
        apply_orbit_aim_control(pitch_ease, hold_yaw, body_target_yaw);
    }
    // Pin the BODY to the camera heading while moving so the character runs camera-relative in EVERY
    // direction. KCD2 moves the player relative to the ENTITY (body) rotation, NOT the look, so this body
    // override -- not the look-yaw write above -- is what redirects locomotion: with the body faced at the
    // camera heading, W runs away from the camera, A/D strafe to the sides and S backs toward it. The body
    // must hold the CAMERA heading, not the input direction: facing it at the movement direction would
    // rotate the move frame and re-apply the input on top of it (world_move = body + input), sending every
    // key the wrong way (A/D -> backward, S -> forward). Held every frame while moving (the override is
    // consume-once); released when movement stops.
    static bool s_body_turn_engaged = false;
    const bool body_turn_active = hold_yaw && cfg.orbit_body_turn.load(std::memory_order_relaxed);
    if (body_turn_active != s_body_turn_engaged)
    {
        s_body_turn_engaged = body_turn_active;
        if (body_turn_active)
        {
            DMK::Logger::get_instance().trace("Camera: orbit body-turn ENGAGED (moving; heading locked to camera)");
        }
        else
        {
            DMK::Logger::get_instance().trace("Camera: orbit body-turn released");
        }
    }
    if (body_turn_active)
    {
        apply_orbit_body_turn(body_target_yaw);
    }

    // Camera collision: keep the view out of world geometry. Cast from the pivot (a safe
    // point inside the player) to the computed camera position; on a hit, pull the camera in
    // to just short of the surface. RWI_OBJTYPES_CAMERA excludes living entities so the ray
    // ignores the player and NPCs and only solid world geometry blocks the view. The camera
    // pulls in instantly (so it never ends up behind a wall) and eases back out once the
    // obstruction clears. The follow distance is carried in collision_distance.
    if (cfg.enable_collision.load(std::memory_order_relaxed))
    {
        const Vector3 to_camera = camera_position - pivot;
        const float desired_distance = to_camera.magnitude();
        if (desired_distance > 1e-3f)
        {
            const Vector3 ray_dir = to_camera / desired_distance;

            // Prefer the swept SPHERE (PrimitiveWorldIntersection): its contact distance is continuous
            // as the sweep grazes edges, so the camera does not pump in dense geometry the way a single
            // thin ray does (the root cause of the orbit position-jump). The sphere radius is the
            // standoff, so no skin is subtracted on this path. Fall back to the thin ray (with skin)
            // when the sphere is disabled, unavailable, or faults, so collision always works and never
            // regresses.
            // Camera collision = swept SPHERE (smooth) cross-checked against an RWI multi-ray FAN (correct).
            // The PrimitiveWorldIntersection sphere may not honour our object types: the fork SPWIParams keeps
            // a FLAGS field at +0x98 and the real entTypes at +0x9C, and +0x9C is a static-RE inference (the
            // sim-class filter runs in a dispatched processor), so the sphere cannot be trusted to type-filter
            // on its own. With entTypes effectively ent_all the sweep can hit the player's OWN
            // CArticulatedEntity (skeleton + worn-gear physics) at point-blank (the "shield on the back" case).
            // So the sphere is CROSS-CHECKED against the FAN (centre + 4 rays offset by the radius; RWI takes
            // objtypes as a plain function ARG so it is provably correct), which gives the true nearest WORLD
            // distance. The sphere is trusted ONLY when its contact is not closer than that world distance
            // (allowing the sphere-radius inset); otherwise the fan distance is used. So the sphere can only
            // make the result SMOOTHER, never closer than the world: no regression.
            const float collision_radius = cfg.collision_radius.load(std::memory_order_relaxed);
            const float thin_skip_max = cfg.collision_thin_skip_max.load(std::memory_order_relaxed);
            const std::optional<RayHit> fan = ray_fan_sweep_skipping_thin(
                pivot, to_camera, collision_radius, Constants::RWI_OBJTYPES_CAMERA,
                Constants::RWI_FLAGS_STOP_AT_SOLID, thin_skip_max);
            std::optional<RayHit> hit = fan;
            bool from_sphere = false;
            // UseSphereCollision selects the probe: when set, the swept SPHERE smooths the fan's world hit
            // (continuous contact distance, no pump in dense geometry); when clear, the multi-ray FAN result
            // is used alone (with the configured skin) for a cheaper, ray-only probe that skips the per-frame
            // PWI sweep. The FAN is always the collision AUTHORITY either way; the sphere can only make the
            // result smoother, never closer.
            if (cfg.use_sphere_collision.load(std::memory_order_relaxed))
            {
                // Resolve the player's / actors' physics entities (body / skeleton / worn shield / NPCs in the
                // way) to SKIP on the sphere sweep, so it reports a clean WORLD distance instead of slamming
                // into the player at point-blank. The PWI struct entTypes field is DEAD in this fork (0 reads
                // in the impl sub_1808182A0), so pSkipEnts is the ONLY way to exclude the player; the
                // probe casts in REVERSE (camera -> pivot) because the pivot is inside the body and a forward
                // ray exits through a back-face and misses it. Buffer capped at 4 (SPWIParams nSkipEnts clamps).
                uintptr_t skip_ents[4];
                const int n_skip = resolve_player_physics_skip(
                    pivot, to_camera, collision_radius, 0x11F /*ent_all*/, Constants::RWI_OBJTYPES_CAMERA,
                    Constants::RWI_FLAGS_STOP_AT_SOLID, skip_ents, 4);
                const std::optional<RayHit> sphere = sphere_world_sweep(
                    pivot, collision_radius, to_camera, Constants::RWI_OBJTYPES_CAMERA, skip_ents, n_skip);
                // The FAN (0x101 = static|terrain) is the AUTHORITY for collision: a plain objtypes arg that
                // provably excludes ALL actors (player body/gear, NPCs = ent_living/independent/rigid). The PWI
                // sphere only SMOOTHS the fan's world hit -- it queries ent_all (the struct entTypes is dead in
                // this fork, so it cannot be type-filtered and returns no collider to test), so it is trusted
                // ONLY when it AGREES with the fan's world surface (within the radius inset). Crucially, when
                // the fan finds NO world (open space), there is NO collision -- any actor the sphere saw (an
                // NPC at the camera, your own shield) is ignored = transparent, like non-physicalized grass.
                // This drops actor collisions UNIFORMLY with no skip-list / probe and no per-entity guessing.
                if (fan.has_value() && sphere.has_value() &&
                    sphere->m_distance >= fan->m_distance - collision_radius - 0.10f &&
                    sphere->m_distance <= fan->m_distance + 0.05f)
                {
                    hit = sphere; // sphere agrees with the fan's world surface -> use it (continuous, no pump)
                    from_sphere = true;
                }
            }

            constexpr float k_collision_hold_seconds = 0.3f; // hold the pull-in across edge hit/miss gaps
            float allowed_distance;
            if (hit.has_value() && hit->m_distance < desired_distance)
            {
                // Sphere path: the radius already insets from the surface, so do NOT subtract the skin
                // again (that would double-inset and seat the camera too close). Thin-ray path: subtract
                // the configured skin as before.
                const float skin = from_sphere ? 0.0f : cfg.collision_skin.load(std::memory_order_relaxed);
                allowed_distance = std::max(0.0f, hit->m_distance - skin);
                cam.collision_hold_timer = k_collision_hold_seconds; // latch on a blocking hit
            }
            else if (cam.collision_valid && cam.collision_hold_timer > 0.0f)
            {
                // Recently blocked but clear this frame: HOLD the pulled-in distance through the
                // gap so an edge-grazing hit/miss alternation cannot pump the camera (sawtooth).
                cam.collision_hold_timer -= delta_time;
                allowed_distance = cam.collision_distance;
            }
            else
            {
                allowed_distance = desired_distance; // truly clear: ease back out
            }

            // Ease toward the allowed distance: fast when pulling IN (so a wall is never
            // clipped) and slower when returning out. Easing the pull-in rather than snapping
            // stops the camera pumping when the ray grazes an edge and the hit alternates
            // near/far frame to frame.
            if (!cam.collision_valid)
            {
                cam.collision_distance = allowed_distance;
                cam.collision_valid = true;
            }
            else
            {
                constexpr float k_collision_pull_in_speed = 25.0f; // near-instant but still smooth
                const float return_speed = cfg.collision_return_speed.load(std::memory_order_relaxed);
                const float speed = (allowed_distance < cam.collision_distance) ? k_collision_pull_in_speed : return_speed;
                const float blend = (speed > 0.0f) ? (1.0f - std::exp(-speed * delta_time)) : 1.0f;
                cam.collision_distance += (allowed_distance - cam.collision_distance) * blend;
            }

            camera_position = pivot + ray_dir * cam.collision_distance;
        }
    }

    // Write the offset position into the camera matrix translation column. The basis columns are
    // left as the engine built them (from the eye quat) unless convergence/orbit changed the look,
    // so the camera otherwise stays parallel to the player's view. The caller computes the cull
    // planes from this matrix next, so culling follows the rendered view.
    // Blend the final pose between first person (the engine's eye matrix) and the third-person pose
    // by view_blend so toggling the view eases instead of snapping. At view_blend 1 this is the full
    // third-person pose; at 0 it is the untouched first-person camera.
    const Vector3 final_position = fpv_position + (camera_position - fpv_position) * view_blend;
    matrix->m[0][3] = final_position.x;
    matrix->m[1][3] = final_position.y;
    matrix->m[2][3] = final_position.z;

    // Orientation: only when convergence/orbit changed the look (otherwise the engine-built eye basis
    // is already correct at every blend value, so the camera just sits behind the player looking where
    // they look). Ease the look from the eye forward to the converged look by view_blend, then rebuild
    // the column basis (right = forward x up, up = right x forward; col0 = right, col1 = forward,
    // col2 = up) so the orientation transitions smoothly too.
    // Screen-centre forward (the crosshair direction): the converged/orbited look when we rebuilt the
    // basis, else the engine eye forward. Published below for the camera-space interaction hook.
    Vector3 screen_forward = forward;
    if (rebuild_basis)
    {
        Vector3 blended_forward = forward + (look_forward - forward) * view_blend;
        blended_forward = (blended_forward.magnitude_squared() > 1e-6f) ? blended_forward.normalized()
                                                                        : look_forward;
        screen_forward = blended_forward;
        const Vector3 right_axis = blended_forward.cross(up);
        if (right_axis.magnitude_squared() > 1e-6f)
        {
            const Vector3 new_right = right_axis.normalized();
            const Vector3 new_up = new_right.cross(blended_forward);
            matrix->m[0][0] = new_right.x;
            matrix->m[1][0] = new_right.y;
            matrix->m[2][0] = new_right.z;
            matrix->m[0][1] = blended_forward.x;
            matrix->m[1][1] = blended_forward.y;
            matrix->m[2][1] = blended_forward.z;
            matrix->m[0][2] = new_up.x;
            matrix->m[1][2] = new_up.y;
            matrix->m[2][2] = new_up.z;
        }
    }

    // Publish the rendered camera pose + crosshair direction for the camera-space interaction hook
    // (interaction_hook.cpp). The player use-cone is eye-anchored and never reads the render camera, so
    // in third person the screen-centre crosshair and the use-target diverge by the shoulder offset
    // (worst at close range -- you cannot loot/use what the crosshair appears to be on). The hook
    // re-origins the cone onto this pose when InteractFromCamera is set. valid is true only here (while
    // the offset is engaged); the suppression path below clears it, so first person leaves the game alone.
    {
        InteractionAimPose &aim = interaction_aim_pose();
        aim.pos_x.store(final_position.x, std::memory_order_relaxed);
        aim.pos_y.store(final_position.y, std::memory_order_relaxed);
        aim.pos_z.store(final_position.z, std::memory_order_relaxed);
        aim.dir_x.store(screen_forward.x, std::memory_order_relaxed);
        aim.dir_y.store(screen_forward.y, std::memory_order_relaxed);
        aim.dir_z.store(screen_forward.z, std::memory_order_relaxed);
        aim.valid.store(true, std::memory_order_relaxed);
    }
}

/**
 * @brief Calls the head-visibility setter on the latched player entity.
 * @details Invokes the original (the trampoline, so this does not re-enter our detour)
 *          with the latched flags. Faults are caught by the CView::Update SEH wrapper this
 *          runs under. Runs on the view-update thread, the same thread the engine drives the
 *          setter on.
 * @param entity Latched player entity.
 * @param hide_head Desired hide state (false = head shown).
 */
static void call_head_visibility(uintptr_t entity, bool hide_head)
{
    if (s_set_head_visibility_original)
    {
        s_set_head_visibility_original(entity, hide_head, static_cast<char>(s_head_flags.load(std::memory_order_relaxed)));
    }
}

/**
 * @brief Keeps the player head in sync with the offset state once per frame.
 * @details While the offset is active the head must stay shown, but the engine only
 *          sets head visibility on its own transitions, so re-call the setter whenever
 *          the live hide flag says the head is hidden. When the offset has just turned
 *          off, restore the game's intended hide value exactly once so the first-person
 *          rig hides the head again. The hide flag is re-read rather than cached so this
 *          self-heals regardless of who last changed it, and a call is issued only on a
 *          mismatch so the engine is not driven every frame.
 * @param active Whether the offset is currently being applied to the game view.
 */
static void reassert_head_visibility(bool active)
{
    const uintptr_t entity = s_head_entity.load(std::memory_order_relaxed);
    if (entity != 0 && DMK::Memory::plausible_userspace_ptr(entity))
    {
        if (active)
        {
            const auto hide_flag = DMK::Memory::seh_read<uint8_t>(entity + Constants::OFFSET_ENTITY_HIDE_HEAD_FLAG);
            if (hide_flag && *hide_flag != 0)
            {
                call_head_visibility(entity, false);
            }
        }
        else if (s_head_was_active.load(std::memory_order_relaxed))
        {
            call_head_visibility(entity, s_game_intended_hide_head.load(std::memory_order_relaxed));
        }
    }
    s_head_was_active.store(active, std::memory_order_relaxed);
}

/**
 * @brief Applies the forced-view policy on STATE-CHANGE EDGES, not every frame.
 * @details A continuous per-frame override would fight the player: they could never toggle the view
 *          while a forced state was active, because the next frame would re-force it. Instead this
 *          forces the view ONCE when a forced state begins, lets the player toggle freely while it
 *          lasts, and restores the pre-force view when the state ends -- unless the player changed
 *          the view themselves meanwhile, in which case their choice stands. Forced-FPV wins over
 *          forced-TPV on overlap. Runs on the render thread only (the detour), so the latch state is
 *          plain file-scope static; cam.applying is atomic because the hotkeys write it too.
 * @param cam Camera state whose applying flag (the live view) is forced and restored.
 * @param state Current debounced GameState mask.
 * @param forced_fpv_mask States that switch to first person on entry.
 * @param forced_tpv_mask States that switch to third person on entry.
 */
static void apply_forced_view_policy(CameraState &cam, uint32_t state, uint32_t forced_fpv_mask,
                                     uint32_t forced_tpv_mask)
{
    // The view the policy wants this frame: -1 none, 0 force FPV, 1 force TPV (forced-FPV wins).
    int forced_view = -1;
    if ((state & forced_fpv_mask) != 0)
    {
        forced_view = 0;
    }
    else if ((state & forced_tpv_mask) != 0)
    {
        forced_view = 1;
    }

    // s_prev_forced_view detects the edge; s_saved_applying is the view to restore on exit;
    // s_policy_owns is cleared once the player toggles the view while a forced state is active, so
    // the policy then stops re-forcing and skips the restore on exit (their manual choice stands).
    static int s_prev_forced_view = -1;
    static bool s_saved_applying = false;
    static bool s_policy_owns = false;

    if (forced_view != s_prev_forced_view)
    {
        if (forced_view == -1)
        {
            // Left every forced state: restore the pre-force view, but only if the player did not
            // switch the view themselves during the state (otherwise their choice stands).
            if (s_policy_owns)
            {
                cam.applying.store(s_saved_applying, std::memory_order_relaxed);
                s_policy_owns = false;
            }
        }
        else
        {
            // Entered a forced state (or switched directly from one forced view to another). Save
            // the pre-force view on the first entry from an unforced state so it can be restored on
            // exit, then apply the forced view exactly once.
            if (s_prev_forced_view == -1)
            {
                s_saved_applying = cam.applying.load(std::memory_order_relaxed);
            }
            cam.applying.store(forced_view == 1, std::memory_order_relaxed);
            s_policy_owns = true;
        }
        s_prev_forced_view = forced_view;
    }
    else if (forced_view != -1 && s_policy_owns &&
             cam.applying.load(std::memory_order_relaxed) != (forced_view == 1))
    {
        // Same forced state as last frame, but the player toggled the view away from the forced
        // value: release ownership so the policy stops forcing and does not restore on exit.
        s_policy_owns = false;
    }
}

/**
 * @brief Suspends free-look on entering an OrbitExcludeState and RESTORES it on exit.
 * @details Edge-triggered, like the forced-view policy: free-look is turned off once when a listed
 *          state begins and re-enabled when it ends, so a state that interrupts free-look (a
 *          dialogue, a minigame) does not permanently cancel it. It only re-enables if the policy
 *          was the one that turned it off (free-look was on at entry). Render-thread only.
 * @param cam Camera state whose orbit_active flag is suspended and restored.
 * @param state Current debounced GameState mask.
 * @param orbit_exclude_mask States in which free-look is disabled.
 */
static void apply_orbit_exclude_policy(CameraState &cam, uint32_t state, uint32_t orbit_exclude_mask)
{
    const bool excluded = (state & orbit_exclude_mask) != 0;
    static bool s_prev_excluded = false;
    static bool s_suspended_orbit = false; // the policy turned free-look off and will restore it

    if (excluded == s_prev_excluded)
    {
        // Still in the same state. If the policy suspended free-look on entry but the player has since
        // turned it back on by hand, release the suspension so the exit branch does not later re-assert
        // it against a manual choice (symmetric to apply_forced_view_policy's ownership release).
        if (excluded && s_suspended_orbit && cam.orbit_active.load(std::memory_order_relaxed))
        {
            s_suspended_orbit = false;
        }
        return;
    }
    if (excluded)
    {
        // Entering an excluded state (combat, dialogue, minigame) swaps the action map, which can swallow a
        // held-move release and strand the movement-input latch > 0. Drop it now so when free-look restores on
        // exit it cannot re-trip the body-turn with the keys released (the post-combat self-rotation). Logged
        // with the cleared magnitude so the trace shows whether the latch WAS actually stranded.
        const float stranded = player_onaction_reset();
        DMK::Logger::get_instance().trace("Orbit: exclude-state entered; cleared move-latch (had magnitude "
                                          "{:.2f}{})",
                                          stranded, stranded > k_orbit_move_input_stop ? ", WAS STRANDED" : "");
        // If free-look was on, turn it off and remember to restore it.
        s_suspended_orbit = cam.orbit_active.load(std::memory_order_relaxed);
        if (s_suspended_orbit)
        {
            cam.orbit_active.store(false, std::memory_order_relaxed);
        }
    }
    else if (s_suspended_orbit)
    {
        // Leaving the excluded state: re-enable free-look and re-seed its angles to the configured
        // centre, matching a fresh toggle-on (the camera moved during the excluded state, so
        // resuming the old orbit angle would be meaningless).
        cam.orbit_yaw.store(0.0f, std::memory_order_relaxed);
        cam.orbit_pitch.store(0.0f, std::memory_order_relaxed);
        cam.orbit_active.store(true, std::memory_order_relaxed);
        s_suspended_orbit = false;
    }
    s_prev_excluded = excluded;
}

/**
 * @brief Gate + matrix-offset body for the frustum-builder detour. Separated from the SEH wrapper
 *        so that frame can hold C++ objects that need unwinding.
 * @details Cheapest exits first: the runtime toggle, then the CView vtable identity (a single
 *          guarded read), so the inactive feature and any non-game camera (shadow, reflection,
 *          portal) pay almost nothing.
 * @param camera The camera handed to the frustum builder (matrix at offset 0).
 */
static void detour_frustum_build_impl(uintptr_t camera)
{
    CameraState &cam = camera_state();
    LiveSettings &cfg = settings();

    const bool state_policy = cfg.enable_state_behavior.load(std::memory_order_relaxed);
    const uint32_t forced_fpv = state_policy ? cfg.forced_fpv_mask.load(std::memory_order_relaxed) : 0u;
    const uint32_t forced_tpv = state_policy ? cfg.forced_tpv_mask.load(std::memory_order_relaxed) : 0u;

    // Fast path: nothing can drive the offset, so there is nothing to do. When any forced state is
    // configured the policy must run every game-view frame to catch the state-change EDGES that
    // trigger a one-time forced switch, so only skip when the player is in manual first person with
    // no forced states configured and the head has already been restored (the head-restore branch
    // keeps running for one frame after a toggle-off, while s_head_was_active is still set).
    if (!cam.applying.load(std::memory_order_relaxed) && (forced_fpv | forced_tpv) == 0 &&
        !s_head_was_active.load(std::memory_order_relaxed) && cam.view_blend <= 1e-3f)
    {
        // Even while idle (first person, no forced state), probe for the player until the world is
        // first seen so game_world_ready becomes true in-world REGARDLESS of the third-person view
        // being on -- the overlay waits on it. resolve_c_player sets the flag on success and returns
        // 0 at the menu, so this costs a few guarded reads per frame only until load-in, then never.
        if (!game_world_ready().load(std::memory_order_relaxed))
        {
            resolve_c_player();
        }
        // Offset disengaged: the orbit cannot be capturing, and the cursor flag is only refreshed on
        // the game-view path below, so clear it here to keep it from latching true across the gap.
        s_cursor_shown.store(false, std::memory_order_relaxed);
        return;
    }

    // The camera is embedded in its CView at SVIEWPARAMS_VIEWMATRIX_OFFSET, so the CView is that
    // far below the camera. Confirm it by checking the CView vtable: shadow/reflection/portal
    // cameras handed to the same builder are not embedded in a CView and fail this guard.
    if (!DMK::Memory::plausible_userspace_ptr(camera))
    {
        return;
    }
    const uintptr_t cview = camera - Constants::SVIEWPARAMS_VIEWMATRIX_OFFSET;
    const auto vtable = DMK::Memory::seh_read<uintptr_t>(cview);
    if (!vtable || !DMK::Memory::plausible_userspace_ptr(*vtable))
    {
        return;
    }
    // Identify the CView by RTTI on the first hit and cache its vtable address, so the
    // steady-state gate is a single qword compare. Using the RTTI type name rather than a
    // hardcoded vtable address keeps this resilient across game patches.
    if (s_cview_vtable_runtime != 0)
    {
        if (*vtable != s_cview_vtable_runtime)
        {
            return;
        }
    }
    else if (DMK::Rtti::vtable_is_type(*vtable, Constants::CVIEW_RTTI_NAME))
    {
        s_cview_vtable_runtime = *vtable;
    }
    else
    {
        return;
    }

    // Game-view camera: take the single per-frame delta and resolve the player once here, then
    // reuse both in the matrix offset below so neither is computed twice per frame. The game state
    // is derived from the discrete engine signals and published for the input-dispatch thread.
    const float delta_time = frame_delta();
    const uintptr_t c_player = resolve_c_player();
    // Presets are always active, so the debounced game state is always needed here (to select the
    // active preset and, when state behaviour is on, to drive the forced-view / orbit-exclude policies).
    const uint32_t raw_state = poll_game_state(c_player);
    const uint32_t state = debounce_game_state(raw_state, delta_time,
                                               cfg.state_switch_hold_seconds.load(std::memory_order_relaxed));
    if (state_policy)
    {
        // Both policies are edge-triggered (they act on state-change edges, not every frame) so the
        // player keeps manual control while a state lasts. The forced-view policy writes cam.applying
        // on entry and restores it on exit; the orbit-exclude policy suspends free-look on entry and
        // restores it on exit.
        apply_forced_view_policy(cam, state, forced_fpv, forced_tpv);
        apply_orbit_exclude_policy(cam, state, cfg.orbit_exclude_mask.load(std::memory_order_relaxed));
    }
    game_state_mask().store(state, std::memory_order_relaxed);

    // Publish whether the game is showing the OS cursor (a UI is up) so the free-look input gate can
    // freeze the orbit while menus / loot / trade / dialogue are open. The hardware-mouse reference
    // counter (g_env -> p_hardware_mouse -> + count) reads > 0 whenever any UI requests the cursor.
    // Every link is screened (seh_resolve_chain + seh_read), and any failed link -- or the feature
    // being disabled -- publishes false, so a layout miss or a load degrades to normal orbit rather
    // than a stuck-frozen one. Read here on the render thread; the input thread only reads the
    // published flag (same producer/consumer split as game_state_mask, so no new cross-thread race).
    // Non-game-view frustum frames (shadow/reflection) skip this publish, but cannot strand the orbit
    // frozen: s_offset_active is co-published on this same game-view path and the input gate tests it
    // first, so a stale true here is moot whenever the offset is not actively rendering the view.
    bool cursor_shown = false;
    if (cfg.freeze_orbit_on_cursor.load(std::memory_order_relaxed) && s_genv_runtime != 0)
    {
        const auto count_addr = DMK::Memory::seh_resolve_chain(
            s_genv_runtime,
            {Constants::GENV_HARDWARE_MOUSE_OFFSET, Constants::HARDWARE_MOUSE_CURSOR_COUNT_OFFSET});
        if (count_addr)
        {
            const auto count = DMK::Memory::seh_read<int32_t>(*count_addr);
            cursor_shown = count.has_value() && *count > 0;
        }
    }
    s_cursor_shown.store(cursor_shown, std::memory_order_relaxed);

    // Drive the player head from the offset state every frame (the engine only sets it on its own
    // transitions, so toggling the offset would otherwise leave the head stuck), then offset the
    // matrix while active. The offset follows cam.applying directly -- both the manual toggle and
    // the edge-triggered policy write it -- and should_apply_view() applies the SuppressTPVState hard
    // gate.
    // Ease the first-person <-> third-person blend toward the desired view so toggling (and UI
    // suppression) slides instead of snapping. ViewTransitionDuration 0 makes the switch instant.
    const bool want_tpv = cam.applying.load(std::memory_order_relaxed) && should_apply_view();
    const float view_dur = cfg.view_transition_duration.load(std::memory_order_relaxed);
    const float view_target = want_tpv ? 1.0f : 0.0f;
    if (view_dur > 1e-4f)
    {
        const float view_step = delta_time / view_dur;
        cam.view_blend = (cam.view_blend < view_target)
                             ? std::min(view_target, cam.view_blend + view_step)
                             : std::max(view_target, cam.view_blend - view_step);
    }
    else
    {
        cam.view_blend = view_target;
    }

    // The offset is rendered while heading TO or holding third person, so the ease-OUT renders too.
    const bool offset_engaged = want_tpv || cam.view_blend > 1e-3f;
    s_offset_active.store(offset_engaged, std::memory_order_relaxed);
    reassert_head_visibility(offset_engaged);

    // Edge tracker for the disengage cleanup below: true while the offset is rendering, so the cleanup runs
    // ONCE on the third-person -> first-person transition, not every first-person frame.
    static bool s_orbit_was_engaged = false;
    if (!offset_engaged)
    {
        if (s_orbit_was_engaged)
        {
            // Just left third person (toggled to FPV, or a state suppressed the offset). Drop the orbit
            // run-state so re-engaging is fresh: clear the move re-arm latch and force-clear any
            // movement-input latch the game may have stranded on an action-map swap. Edge-gated so a key
            // held in first person does not spam the reset/log every frame.
            cam.orbit_move_armed = false;
            const float stranded = player_onaction_reset();
            if (stranded > k_orbit_move_input_stop)
            {
                DMK::Logger::get_instance().trace(
                    "Orbit: TPV disengaged; cleared a stranded move-latch (magnitude {:.2f})", stranded);
            }
            s_orbit_was_engaged = false;
        }
        cam.collision_valid = false;
        cam.collision_hold_timer = 0.0f;
        // First person (or suppressed): the camera is the eye, so the interaction hook must NOT redirect.
        interaction_aim_pose().valid.store(false, std::memory_order_relaxed);
        cam.orbit_level_blend = 0.0f;
        cam.orbit_render_valid = false; // next engaged frame snaps the orbit low-pass instead of easing across the gap
        cam.orbit_steer_valid = false;  // and the continuous-align steer low-pass
        cam.eye_sync_valid = false;     // and the dynamic eye-height low-pass
        cam.fov_ease_valid = false;     // and the per-preset FOV override ease
        cam.orbit_moving = false;
        cam.orbit_target_valid = false;
        // Back to first person: the next engaged frame should snap the preset, not ease across the gap.
        Presets::reset_transition();
        return;
    }
    s_orbit_was_engaged = true;

    // Resolve the active preset (by debounced state, or the overlay's editing pin) and ease the
    // live framing toward it BEFORE the matrix offset reads those settings this frame.
    Presets::resolve_and_apply(state, delta_time);

    // Smoothstep the linear view blend for an ease-in/out feel, then offset with it.
    const float vb = cam.view_blend;
    const float view_s = vb * vb * (3.0f - 2.0f * vb);
    offset_game_view_camera(camera, cview, c_player, delta_time, view_s);
}

/**
 * @brief SEH wrapper for the per-frame frustum-builder detour.
 * @details The matrix offset runs BEFORE the original, which then computes the cull planes from
 *          the offset matrix, so geometry culling matches the rendered third-person view. A
 *          layout drift degrades the offset to a no-op; the original always runs and its return
 *          value is forwarded so the frustum is still built whether or not the offset applied.
 */
static uintptr_t __fastcall detour_frustum_build(uintptr_t camera)
{
    __try
    {
        detour_frustum_build_impl(camera);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // Swallow: an outdated offset or layout must never crash the game.
    }
    return s_frustum_build_original ? s_frustum_build_original(camera) : 0;
}

/**
 * @brief Head-visibility detour: keep the player head while the offset is rendering.
 * @details The first-person rig hides the head so it does not clip the eye camera.
 *          While the third-person view is active, force hide_head to false
 *          so the player is not headless from behind; otherwise pass the game's
 *          intended value through unchanged.
 */
static void __fastcall detour_set_head_visibility(uintptr_t entity, bool hide_head, char flags)
{
    __try
    {
        if (s_set_head_visibility_original)
        {
            // Latch the player entity, the flags, and the game's intended hide value so
            // the per-frame re-assert can keep the head shown while the offset is active
            // and restore the game's value when it turns off. This is the player because
            // the setter only toggles the FirstPersonView rig.
            s_head_entity.store(entity, std::memory_order_relaxed);
            s_head_flags.store(static_cast<uint8_t>(flags), std::memory_order_relaxed);
            s_game_intended_hide_head.store(hide_head, std::memory_order_relaxed);

            // Mirror the head to the offset's effective active state (published by the frustum
            // detour) rather than the raw toggle, so a forced-FPV/TPV state shows or hides the head
            // to match the view the player actually sees.
            const bool active = s_offset_active.load(std::memory_order_relaxed);
            const bool final_hide_head = active ? false : hide_head;
            s_set_head_visibility_original(entity, final_hide_head, flags);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // Swallow: a layout change in the head setter must never crash the game.
    }
}

/**
 * @brief Free-look capture/block decision for one input event (toggle orbit camera).
 * @details While the offset is active and orbit is TOGGLED on, mouse-look deltas are accumulated
 *          into the orbit angles and ONLY those look events are blocked, so the player's look stays
 *          put while free-looking. Every other input (movement keys, interaction, clicks) passes
 *          through, so the toggle orbit is a usable camera mode the character can move and act in.
 *          Lives apart from the SEH wrapper so that frame stays free of C++ object unwinding; the
 *          wrapper guards the engine-owned reads.
 * @param input_event Engine input event (GameStructures::InputEvent layout).
 * @return true to block (swallow) the event, false to dispatch it normally.
 */
[[nodiscard]] static bool orbit_capture_and_decide(uintptr_t input_event)
{
    CameraState &cam = camera_state();
    // Gate on the offset's effective active state (so free-look also works when a forced-TPV state
    // turned the offset on without the manual toggle), the orbit toggle, the cursor-shown flag (so a
    // UI being up holds the orbit instead of letting cursor motion turn it), and the shared UI gate
    // so the cursor and look work normally while a suppressed UI is up. Returning false here leaves
    // orbit_yaw/orbit_pitch untouched, so the camera resumes from the same angle when the UI closes.
    // orbit_active is the single source of truth for whether free-look captures input. The
    // OrbitExcludeState policy turns it off when ENTERING a listed state (edge-triggered, see
    // apply_orbit_exclude_policy), so capture stops there without forbidding a manual re-enable: a
    // player who toggles free-look back on in an excluded state (e.g. for a photo on a mount) still
    // gets mouse-look here. The cursor-shown gate freezes the orbit while any UI cursor is up.
    if (!s_offset_active.load(std::memory_order_relaxed) ||
        !cam.orbit_active.load(std::memory_order_relaxed) ||
        s_cursor_shown.load(std::memory_order_relaxed) ||
        !should_apply_view())
    {
        return false;
    }
    if (input_event == 0 || !DMK::Memory::plausible_userspace_ptr(input_event))
    {
        return false;
    }

    const int32_t type = *reinterpret_cast<const int32_t *>(input_event + Constants::INPUT_EVENT_TYPE_OFFSET);
    const int32_t id = *reinterpret_cast<const int32_t *>(input_event + Constants::INPUT_EVENT_ID_OFFSET);
    // The look channel is the eIS_Changed (analog-axis-moved) state. Mouse AND gamepad both post here; the
    // keyId picks the axis and the device. Everything else (movement axes, buttons, clicks) falls through.
    if (type == Constants::MOUSE_INPUT_TYPE_ID)
    {
        const float value = *reinterpret_cast<const float *>(input_event + Constants::INPUT_EVENT_VALUE_OFFSET);

        // Mouse look: relative DELTA accumulated straight into the orbit angle (one event = one nudge).
        if (id == Constants::INPUT_LOOK_YAW_EVENT_ID || id == Constants::INPUT_LOOK_PITCH_EVENT_ID)
        {
            const float sensitivity = settings().orbit_sensitivity.load(std::memory_order_relaxed);
            if (id == Constants::INPUT_LOOK_YAW_EVENT_ID)
            {
                // Negated so mouse-left orbits the camera left and mouse-right orbits right.
                cam.orbit_yaw.store(cam.orbit_yaw.load(std::memory_order_relaxed) - value * sensitivity, std::memory_order_relaxed);
            }
            else
            {
                // Mouse-up raises the camera, mouse-down lowers it.
                const float pitch = cam.orbit_pitch.load(std::memory_order_relaxed) + value * sensitivity;
                cam.orbit_pitch.store(std::clamp(pitch,
                                                settings().orbit_pitch_min.load(std::memory_order_relaxed),
                                                settings().orbit_pitch_max.load(std::memory_order_relaxed)),
                                     std::memory_order_relaxed);
            }
            return true; // block ONLY the look so the player look stays put while free-looking
        }

        // Gamepad RIGHT STICK: latch the held DEFLECTION (-1..1) for the orbit rate integration, then ZERO
        // the event value IN PLACE and let it PASS (do NOT block). A held analog stick drives an engine
        // look-RATE that the engine only zeroes on a release event. BLOCKING swallows that release: if orbit
        // engages while the stick is already deflected (e.g. you hold the right stick then toggle orbit, or an
        // exclude state suspends/restores orbit mid-deflection), the engine keeps its last rate and SPINS the
        // player look forever -- surviving a switch to first person and curable only by a hard input flush
        // (menu / alt-tab). It is the same swallowed-release defect as the move latch, but stranding the
        // ENGINE's own gamepad look. Zeroing the value instead means the engine continuously sees no
        // deflection (so it never turns and never strands) while we keep the real value for the orbit camera;
        // the release reaches the engine too. The mouse path above can still hard-block: a mouse delta is a
        // one-shot nudge with no held rate, so there is nothing to strand. Left stick (movement) keeps its own
        // ids and passes untouched.
        if (id == Constants::INPUT_PAD_LOOK_YAW_EVENT_ID)
        {
            cam.orbit_pad_yaw.store(value, std::memory_order_relaxed);
            *reinterpret_cast<float *>(input_event + Constants::INPUT_EVENT_VALUE_OFFSET) = 0.0f;
            return false;
        }
        if (id == Constants::INPUT_PAD_LOOK_PITCH_EVENT_ID)
        {
            cam.orbit_pad_pitch.store(value, std::memory_order_relaxed);
            *reinterpret_cast<float *>(input_event + Constants::INPUT_EVENT_VALUE_OFFSET) = 0.0f;
            return false;
        }
    }

    return false; // pass movement, interaction and everything else through
}

/**
 * @brief Input-dispatcher detour. Swallows events while free-looking, else passes through.
 */
static void __fastcall detour_input_dispatch(uintptr_t controller, uintptr_t input_event, char flag)
{
    bool block = false;
    __try
    {
        block = orbit_capture_and_decide(input_event);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        block = false; // on any fault, fall back to normal dispatch
    }

    if (!block && s_input_dispatch_original)
    {
        s_input_dispatch_original(controller, input_event, flag);
    }
}

/**
 * @brief Resolves the SSystemGlobalEnvironment (g_env) base, patch-resiliently.
 * @details Anchors on a unique `lea rdx, [g_env]` instruction (GENV_LOAD_AOB_PATTERN) and
 *          resolves its RIP-relative target, so the address survives game patches that shift
 *          the RVA. Falls back to the known static RVA if the AOB ever drifts, so the camera
 *          still functions on the build it was authored against. The result is screened as a
 *          plausible user-space pointer before it is accepted.
 * @return The g_env base address (AOB result, or the static fallback).
 */
static uintptr_t resolve_genv(uintptr_t module_base, size_t module_size)
{
    DMK::Logger &logger = DMK::Logger::get_instance();

    auto pattern = DMK::Scanner::parse_aob(Constants::GENV_LOAD_AOB_PATTERN);
    if (pattern.has_value())
    {
        const std::byte *match = DMK::Scanner::find_pattern(
            reinterpret_cast<const std::byte *>(module_base), module_size, *pattern);
        if (match)
        {
            // The lea is GENV_LOAD_AOB_LEA_OFFSET bytes into the match; resolve its disp32.
            const auto resolved = DMK::Scanner::resolve_rip_relative(
                match + Constants::GENV_LOAD_AOB_LEA_OFFSET,
                Constants::GENV_LOAD_LEA_DISP_OFFSET,
                Constants::GENV_LOAD_LEA_LENGTH);
            if (resolved.has_value() && DMK::Memory::plausible_userspace_ptr(*resolved))
            {
                logger.info("Camera: g_env resolved via AOB at {}", DMK::Format::format_address(*resolved));
                return *resolved;
            }
        }
    }

    const uintptr_t fallback = module_base + (Constants::GENV_STATIC - Constants::IMAGE_BASE);
    logger.warning("Camera: g_env AOB unavailable; using static fallback at {}",
                   DMK::Format::format_address(fallback));
    return fallback;
}

bool initialize_camera(uintptr_t module_base, size_t module_size)
{
    DMK::Logger &logger = DMK::Logger::get_instance();

    // Start in first-person (offset off) so the game looks normal on load; the view
    // hotkeys toggle/force the third-person offset on. The hooks below fast-path out
    // while the offset is off, so installing them unconditionally costs almost nothing.
    CameraState &cam = camera_state();
    cam.applying.store(false);
    cam.zoom_offset.store(0.0f);

    // Resolve g_env once (patch-resilient AOB, static RVA fallback). The CView vtable is
    // identified lazily by RTTI on the first game-view camera, so nothing is resolved here.
    s_genv_runtime = resolve_genv(module_base, module_size);

    // Resolve the engine ray helper for collision + aim convergence. Best-effort: on a miss
    // those features no-op (the camera still renders), so the result is intentionally discarded.
    (void)initialize_physics_raycast(module_base, module_size, s_genv_runtime);

    try
    {
        DMK::HookManager &hook_manager = DMK::HookManager::get_instance();

        // Hook the camera frustum builder -- the matrix-offset point; without it the feature
        // does nothing. Located by AOB at the function prologue (offset 0 into the match).
        auto frustum_result = hook_manager.create_inline_hook_aob(
            "CameraFrustumBuild",
            module_base,
            module_size,
            Constants::FRUSTUM_BUILD_AOB_PATTERN,
            0,
            reinterpret_cast<void *>(detour_frustum_build),
            reinterpret_cast<void **>(&s_frustum_build_original));

        if (!frustum_result.has_value())
        {
            throw std::runtime_error("Failed to create frustum builder hook: " +
                                     std::string(DMK::Hook::error_to_string(frustum_result.error())));
        }

        // The head-visibility hook is best-effort: if it fails the camera still works,
        // the player just appears headless from behind, so a miss is a warning.
        auto head_result = hook_manager.create_inline_hook_aob(
            "SetHeadVisibility",
            module_base,
            module_size,
            Constants::SET_HEAD_VISIBILITY_AOB_PATTERN,
            0,
            reinterpret_cast<void *>(detour_set_head_visibility),
            reinterpret_cast<void **>(&s_set_head_visibility_original));

        if (!head_result.has_value())
        {
            logger.warning("Camera: Head visibility hook failed ({}); player may appear headless from behind",
                           DMK::Hook::error_to_string(head_result.error()));
        }

        // The input-dispatcher hook powers free-look orbit. Best-effort: a miss only
        // disables orbit, the offset camera still works. The detour is inert until the
        // orbit key is held, so it is harmless when free-look is unused.
        auto input_result = hook_manager.create_inline_hook_aob(
            "CameraInputDispatch",
            module_base,
            module_size,
            Constants::INPUT_DISPATCHER_AOB_PATTERN,
            0,
            reinterpret_cast<void *>(detour_input_dispatch),
            reinterpret_cast<void **>(&s_input_dispatch_original));

        if (!input_result.has_value())
        {
            logger.warning("Camera: Input dispatcher hook failed ({}); free-look orbit unavailable",
                           DMK::Hook::error_to_string(input_result.error()));
        }

        logger.info("Camera: Third-person camera hooks installed");
        return true;
    }
    catch (const std::exception &e)
    {
        logger.error("Camera: Initialization failed: {}", e.what());
        return false;
    }
}

} // namespace TPVCamera
