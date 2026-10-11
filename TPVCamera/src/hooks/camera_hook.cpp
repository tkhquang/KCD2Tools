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
#include "../rtti_types.hpp"
#include "aob_resolver.hpp"
#include "constants.hpp"
#include "config.hpp"
#include "global_state.hpp"
#include "game_state.hpp"
#include "game_structures.hpp"
#include "offset_heal.hpp"
#include "math_utils.hpp"
#include "physics_raycast.hpp"
#include "render_occlusion.hpp"
#include "hooks/archery_hook.hpp"
#include "hooks/character_fade.hpp"
#include "hooks/ui_menu_hooks.hpp"
#include "hooks/player_onaction_hook.hpp"
#include "presets/preset_runtime.hpp"

#include <DetourModKit.hpp>

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <array>

namespace TPVCamera
{

    // CCamera::UpdateFrustumPlanes(camera /*rcx*/): camera object starts with the 3x4 matrix,
    // followed by the projection params; this builds the world cull planes. Offsetting the matrix
    // here, before the cull planes are computed, moves the rendered view AND its culling together.
    using FrustumBuildFunc = uintptr_t(__fastcall *)(uintptr_t camera);

    // The least gap kept between the camera and the surface that stopped it while the camera pulls in (meters): the
    // game's own near clip distance. A pull-in that lags its target never puts the lens on or past that surface.
    constexpr float k_lens_near_margin = 0.05f;

    // Eases the camera collision distance toward @p target. Pulling in follows at k_pull_in_speed (1/s), close to
    // instant so a wall is never clipped, and never trails the target by more than @p pull_slack, so the lens never
    // comes nearer to the surface that stopped it than k_lens_near_margin. Easing back out is critically damped at
    // twice @p return_speed and starts from rest, so a release begins gently and settles on the target without a
    // jolt; a return_speed of 0 snaps out. Snaps to the target on the first valid frame. Shared by the per-frame
    // easing and the static-world throttle bypass so the two cannot drift; with a delta_time of 0 (the frame's second
    // build) only the pull-in limit can move the camera.
    static void ease_collision_toward(CameraState &cam, float target, float delta_time, float return_speed,
                                      float pull_slack)
    {
        if (!cam.collision_valid)
        {
            cam.collision_distance = target;
            cam.collision_speed = 0.0f;
            cam.collision_valid = true;
            return;
        }
        constexpr float k_pull_in_speed = 25.0f; // near-instant but still smooth
        const float from = cam.collision_distance;
        if (target < from)
        {
            const float eased = from + (target - from) * (1.0f - std::exp(-k_pull_in_speed * delta_time));
            cam.collision_distance = std::min(eased, target + pull_slack);
            cam.collision_speed = 0.0f;
            return;
        }
        if (return_speed <= 0.0f)
        {
            cam.collision_distance = target;
            cam.collision_speed = 0.0f;
            return;
        }
        // Critically damped toward the target: behind is how far short of it the camera is (<= 0), the speed its
        // rate outward. A target nearer than where the camera's speed would carry it (a farther surface met on the
        // way out) lowers the speed first, so the camera slows into it instead of stopping dead at it.
        const float rate = 2.0f * return_speed;
        const float behind = from - target;
        const float speed = std::min(cam.collision_speed, -rate * behind);
        const float c = speed + rate * behind;
        const float decay = std::exp(-rate * delta_time);
        float behind_next = (behind + c * delta_time) * decay;
        float speed_next = (speed - rate * c * delta_time) * decay;
        if (behind_next > 0.0f)
        {
            behind_next = 0.0f;
            speed_next = 0.0f;
        }
        cam.collision_distance = target + behind_next;
        cam.collision_speed = speed_next;
    }

    // The close-up fade: the band (meters of arm above HeadClearance) over which the character fades out.
    constexpr float k_close_fade_band = 0.4f;
    // The stock eye, hair and eye-film shaders ignore the dissolve, so the eyes, eyelashes, hair, beard and the wet
    // film over the eyes would stay solid in the dithered face. Once the character is this far faded the head is hidden
    // like in first person (the game's FirstPersonView hiding group, which keeps its shadow), and it is shown again
    // below the lower mark.
    constexpr float k_close_fade_head_hide_on = 0.6f;
    constexpr float k_close_fade_head_hide_off = 0.45f;

    // Fraction (0..1) of the character that the hit collider hides, with a per-collider cache so a solid the
    // camera moves along is not re-measured every frame. Pipeline: a cheap footprint pre-check (a building-scale
    // collider always occludes, so collide without rasterizing its often-compound mesh), else the visible-mesh
    // raster (render_coverage_of_brush / render_coverage_at), else a physics-ray fallback for a compound the
    // render mesh cannot see through. Returns < 0 (collide) for an unmeasurable solid, 0..1 otherwise. A COLLIDE
    // verdict (< 0 or >= cov_thresh) is cached and reused FREELY on later frames: a wall / shed stays solid as the
    // camera slides along it, colliding is the safe direction, and re-running the octree + raster every frame is
    // the dominant moving cost in dense scenes. A SKIP verdict (a thin prop the body is visible past) is reused
    // only within proximity of the cached hit, so a prop's low coverage never leaks across a big shared collider
    // onto a bare wall (which would clip) and a moving camera re-measures it. Single render-thread caller, so the
    // static cache is race-free.
    static float measure_collider_coverage(uintptr_t collider, const Vector3 &hit_point, const Vector3 &pivot,
                                           const Vector3 &desired_cam, const Vector3 &to_camera, float cov_thresh)
    {
        constexpr int k_cov_cache = 8;
        constexpr float k_cov_reuse_dist2 = 0.5f * 0.5f;
        static uintptr_t s_cov_col[k_cov_cache] = {};
        static float s_cov_val[k_cov_cache] = {};
        static float s_cov_hx[k_cov_cache] = {};
        static float s_cov_hy[k_cov_cache] = {};
        static float s_cov_hz[k_cov_cache] = {};

        int slot = -1, free_slot = -1;
        for (int k = 0; k < k_cov_cache; ++k)
        {
            if (collider != 0 && s_cov_col[k] == collider)
            {
                slot = k;
                break;
            }
            if (s_cov_col[k] == 0)
            {
                free_slot = k;
            }
        }
        if (slot >= 0)
        {
            const float cached = s_cov_val[slot];
            if (cached < 0.0f || cached >= cov_thresh)
            {
                return cached; // cached COLLIDE verdict: reuse freely (solid stays solid as the camera moves)
            }
            const float dx = hit_point.x - s_cov_hx[slot], dy = hit_point.y - s_cov_hy[slot],
                        dz = hit_point.z - s_cov_hz[slot];
            if (dx * dx + dy * dy + dz * dz < k_cov_reuse_dist2)
            {
                return cached; // cached SKIP verdict: reuse only near the cached hit (no leak across a collider)
            }
        }

        const float footprint = collider_horizontal_footprint(collider);
        const bool building_scale = footprint > Constants::COLLIDER_WALL_FOOTPRINT_MIN;
        void *node = reinterpret_cast<void *>(static_brush_render_node(collider));
        float cov = building_scale ? -1.0f : render_coverage_of_brush(node, pivot, desired_cam);
        if (cov < 0.0f && !building_scale)
        {
            // Foreign-null collider (merged / proxy: a columbarium, a tent guy-stick, an ENTITY / HLOD-baked
            // post). Measure the visible brush at the hit - a thin DIAGONAL stick has a huge axis-aligned bbox
            // but a sliver mesh, so only the raster coverage can tell it is thin (the bbox footprint cannot).
            cov = render_coverage_at(hit_point, pivot, to_camera);
            if (cov < 0.0f && footprint >= 0.0f && footprint < Constants::COLLIDER_POST_FOOTPRINT_MAX)
            {
                cov = 0.0f; // unmeasurable thin post -> body visible past it -> skip
            }
            else if (cov < 0.0f)
            {
                // A COMPOUND structure the render mesh cannot raster (rmesh null), neither a tiny post nor a
                // building: an OPEN wooden frame (body visible THROUGH the beams) vs a SOLID compound (a shed).
                // The PHYSICS ray occlusion tells them apart: open -> low -> skip; solid -> high -> collide.
                cov = character_occluded_fraction(desired_cam, pivot, Constants::RWI_OBJTYPES_CAMERA,
                                                  Constants::RWI_FLAGS_STOP_AT_SOLID);
            }
        }

        if (collider != 0)
        {
            if (slot < 0)
            {
                slot = (free_slot >= 0) ? free_slot : static_cast<int>(collider % k_cov_cache);
            }
            s_cov_col[slot] = collider;
            s_cov_val[slot] = cov;
            s_cov_hx[slot] = hit_point.x;
            s_cov_hy[slot] = hit_point.y;
            s_cov_hz[slot] = hit_point.z;
        }
        return cov;
    }

    // Head-visibility setter: SetHeadHidden(this /*rcx*/, bool hide_head /*dl*/, char flags /*r8b*/).
    // hide_head == false keeps the head rendered.
    using SetHeadVisibilityFunc = void(__fastcall *)(uintptr_t entity, bool hide_head, char flags);

    // Generic input-event dispatcher: void __fastcall(controller /*rcx*/, event /*rdx*/,
    // char /*r8b*/). Every input event funnels through here; hooking it lets free-look
    // capture mouse-look deltas and freeze the player by not dispatching while orbiting.
    using InputDispatchFunc = void(__fastcall *)(uintptr_t controller, uintptr_t input_event, char flag);

    // wh::engine3d::C_CameraObserver update (vtable slot CAMERA_OBSERVER_VTABLE_UPDATE_SLOT): bool __fastcall(observer
    // /*rcx*/, Vec3 *position /*rdx*/, bool *position_valid /*r8*/, Vec3 *direction /*r9*/, bool *direction_valid,
    // float *fov). It copies the system view camera's translation, forward column and field of view.
    using CameraObserverUpdateFunc = bool(__fastcall *)(uintptr_t observer, float *position, bool *position_valid,
                                                        float *direction, bool *direction_valid, float *fov);

    // IActor::IsThirdPerson (C_Player vtable, the slot AnchorId::IsThirdPersonSlot reads from its call sites):
    // bool __fastcall(actor /*rcx*/). One function shared by every actor class; it answers true for any actor that is
    // not the local player.
    using IsThirdPersonFunc = bool(__fastcall *)(uintptr_t actor);

    // IGameObjectExtension::HandleEvent (C_Player vtable, the slot AnchorId::HandleEventSlot reads from the
    // camera-changed event's sender): void __fastcall(extension /*rcx*/, const SGameObjectEvent *event /*rdx*/).
    using HandleEventFunc = void(__fastcall *)(uintptr_t extension, const void *event);

    // CAnimatedCharacter::UpdatePhysicalEntityMovement: char __fastcall(animchar /*rcx*/, QuatT *movement /*rdx*/).
    // The frame's movement is a rotation (x, y, z, w) followed by a translation (x, y, z). The translation is requested
    // from physics as a velocity and the rotation is composed into the body.
    using UpdatePhysEntMovementFunc = char(__fastcall *)(uintptr_t animchar, float *movement);

    // Trampoline slots. Each install stores its slot with release order before the hook arms. Each detour reads its
    // slot with acquire order, so a detour never chains through a slot it cannot see.
    static std::atomic<FrustumBuildFunc> s_frustum_build_original{nullptr};
    static std::atomic<SetHeadVisibilityFunc> s_set_head_visibility_original{nullptr};
    static std::atomic<InputDispatchFunc> s_input_dispatch_original{nullptr};
    static std::atomic<CameraObserverUpdateFunc> s_camera_observer_update_original{nullptr};
    static std::atomic<IsThirdPersonFunc> s_is_third_person_original{nullptr};
    static std::atomic<UpdatePhysEntMovementFunc> s_phys_ent_movement_original{nullptr};

    // Native turn-in-place animation. The game is kept in first person, where the body follows the look and
    // C_PlayerMovementAction (the free-roam locomotion action) never plays its turn fragments. The body follows the
    // look while the player's LockBodyTurn count (C_Player vtable slot 135) is 0, and the action holds one LockBodyTurn
    // reference in third person. The IsThirdPerson detour answers "third person" for s_native_turn_actor (0 = off) at
    // exactly two of that action's call sites:
    //   - the idle LockBodyTurn sync (s_lock_sync_return), which then takes the action's reference so the body stops
    //     following the look;
    //   - the turn trigger (s_turn_trigger_return, only while s_native_turn_trigger is set), after which the game turns
    //     once the look leads the body by more than 35 degrees. The turn-decision hook below replaces that decision
    //     with its own start angle and with turns that finish facing the look.
    // Both are required: the trigger alone never frees the body, and the sync alone freezes it with no turn to follow.
    // Every other caller keeps the first-person answer, among them the first-person view effects (bow-draw zoom and aim
    // sensitivity, the horse camera), the pose aligner and the animation-event audio. The return addresses are written
    // once at install, before the hook arms. The actor and the trigger flag are published by the frustum detour each
    // game-view frame. s_native_turn_armed is set only once the hook is armed. The trampoline pointer itself is never
    // cleared, because an armed (or pinned) hook still routes calls through it.
    static uintptr_t s_turn_trigger_return = 0;
    static uintptr_t s_lock_sync_return = 0;
    static std::atomic<bool> s_native_turn_armed{false};
    static std::atomic<uintptr_t> s_native_turn_actor{0};
    static std::atomic<bool> s_native_turn_trigger{false};
    // C_Player::HandleEvent, the SGameObjectEvent vtable and the camera-changed event's id and target/flags word, all
    // resolved and validated at install. The feature is not armed without them: a switch-off cannot reach an idle
    // action, which then keeps its reference (body not following the look).
    static uintptr_t s_player_handle_event = 0;
    static uintptr_t s_game_object_event_vtable = 0;
    static uint32_t s_camera_changed_event_id = 0;
    static uint32_t s_camera_changed_event_flags = 0;
    // C_Player's LockBodyTurn reference-count offset (AnchorId::LockBodyTurnCount), read only for the log. 0 when it
    // did not resolve.
    static ptrdiff_t s_lock_body_turn_count_offset = 0;
    // Set by release_native_turn_animation() during shutdown: the frustum detour stops reporting third person and has
    // the action drop its reference, on the game thread.
    static std::atomic<bool> s_native_turn_release_requested{false};
    // update_native_turn calls under way, counted on entry before the release flag is read and uncounted on every
    // return. Once shutdown has set the flag and then sees none under way, no call that read the flag unset is left.
    static std::atomic<int> s_native_turn_updating{0};
    // A change being published and its camera-changed event, counted from BEFORE the actor store until the event has
    // returned. Shutdown reads the actor and then this count, so it cannot remove the hooks under an event in flight.
    static std::atomic<int> s_native_turn_changes_in_flight{0};

    // Turn decision: the mid hook on ComputeMoveState's `seta cl` (see resolve_turn_decision_layout). On its own the
    // game starts a turn past 35 degrees and stops it as soon as the gap is back under 35, so the body always rests
    // about 35 degrees short of the look. The hook starts a turn past s_turn_start_angle (NativeTurnAngle, radians), or
    // past k_turn_settle_angle once the look has rested for s_turn_settle_delay (NativeTurnSettleDelay). It then keeps
    // the turn on until the body is within k_turn_finish_angle of the look, or until the gap changes sign (he faces
    // it). s_turn_continuing, s_turn_last_sign and the rest state carry the decision from frame to frame. Only the hook
    // updates them, on whichever job worker updates the player that frame, one frame after another; a feature switch
    // in update_native_turn only resets them. The hook runs on every idle frame with a gap, so a player frame without
    // it (s_turn_decision_missed, raised by the movement detour) means the action left its idle decision (he walked, an
    // interaction or a script took over, turns were off) and the decision starts over. Breaks are counted in frames,
    // so a long frame is not a break. Without the movement detour the break falls back to wall-clock time: more than
    // k_turn_decision_break_ms since s_turn_decision_tick.
    // The look counts as resting while the gap stays within k_turn_rest_drift of its value when the rest began. While
    // his body holds still only the look changes the gap, and a cumulative bound (not a per-frame rate) means a slow
    // pan never counts as rest. A turn that sets the game's spin latch is refused for that frame (see
    // would_set_spin_latch). One frame later the game holds the new sign, and the turn can start. The action's field
    // offsets the hook reads are resolved from the game's code at install and written before the hook arms.
    constexpr float k_turn_finish_angle = 0.07f; // rad (4 deg)
    constexpr float k_turn_settle_angle = 0.21f; // rad (12 deg)
    constexpr float k_turn_rest_drift = 0.0175f; // rad (1 deg)
    constexpr uint64_t k_turn_decision_break_ms = 150;
    static std::atomic<float> s_turn_start_angle{0.61086524f};
    static std::atomic<float> s_turn_settle_delay{0.8f}; // seconds, 0 = no settle turns
    static std::atomic<bool> s_turn_continuing{false};
    static std::atomic<float> s_turn_last_sign{0.0f};
    static std::atomic<float> s_turn_rest_gap{0.0f};
    static std::atomic<uint64_t> s_turn_rest_since{0};
    static std::atomic<uint64_t> s_turn_decision_tick{0};
    static std::atomic<bool> s_turn_decision_missed{false};
    static std::atomic<bool> s_turn_steps_hooked{false}; // the movement detour is armed: breaks are counted in frames
    static ptrdiff_t s_spin_latch_offset = 0;
    static ptrdiff_t s_installed_state_offset = 0;
    static ptrdiff_t s_last_sign_offset = 0;

    // Turn steps in place. A turn fragment's root motion walks the body 5-20 cm, which leaves a still camera off centre
    // (and shifts a camera that follows it). The movement detour drops the horizontal part of the player's movement on
    // the frames the movement action is in its idle/turn decision, from the first turn frame through a tail after the
    // last one: a turn cut short at the look still sends a step, for at most one frame, while the game blends to idle.
    // The tail lasts k_turn_step_tail_frames decision frames or k_turn_step_tail_ms, whichever is longer, so neither a
    // long frame nor a high frame rate cuts it short, and it ends on the first frame without a decision.
    // s_turn_decision_seen is set by the decision hook and taken by the movement detour, which CAnimatedCharacter's
    // update calls right after the action's update on the same thread, so the flag is exact per frame: the frame he
    // starts walking, or anything else takes the action over, his movement goes through untouched. s_turn_decided_tick
    // is when the hook last chose to turn (GetTickCount64, 0 = tail ended). s_player_animchar is the player's
    // CAnimatedCharacter, published by the frustum detour while the native turn is on (0 otherwise).
    // s_movement_type_offset is CAnimatedCharacter's movement request type, resolved at install before the detour
    // arms.
    constexpr int k_turn_step_tail_frames = 3;
    constexpr uint64_t k_turn_step_tail_ms = 300;
    static std::atomic<bool> s_turn_decision_seen{false};
    static std::atomic<int> s_turn_tail_frames{0};
    static std::atomic<uint64_t> s_turn_decided_tick{0};
    static std::atomic<uintptr_t> s_player_animchar{0};
    static ptrdiff_t s_movement_type_offset = 0;

    // Crouched animations (see Constants::CROUCHED_IDLE_PLAYER_ANIMATION). The game looks a clip's animation up by the
    // 64-bit hash of its name. The hook on that lookup (detour_crouched_animation) swaps the hash of each player clip
    // for its NPC twin while the native turn is on. The hashes come from the game's own name hash at install, and the
    // install writes them before the hook arms.
    struct AnimationSwap
    {
        std::uint64_t player = 0;
        std::uint64_t npc = 0;
    };
    static std::array<AnimationSwap, 2> s_crouched_swaps{};

    // The first-person eye position of the last game-view frame that rendered the third-person offset, published by the
    // frustum detour for the camera-observer detour. The components are separate atomics: a reader racing a writer can
    // mix two consecutive frames' eyes, which differ by a frame of movement and do not matter to the AI. s_ai_eye_valid
    // is released after the components and cleared when the offset disengages.
    static std::atomic<float> s_ai_eye_x{0.0f};
    static std::atomic<float> s_ai_eye_y{0.0f};
    static std::atomic<float> s_ai_eye_z{0.0f};
    static std::atomic<bool> s_ai_eye_valid{false};

    /**
     * @brief The zoom hold bindings resolved to BindingTokens for the per-frame query.
     * @details Acquired on the control plane (refresh_zoom_binding_tokens: after the input engine starts and after
     *          every INI reload, the only places their bindings reshape) and published whole, so the render thread
     *          never resolves a binding name itself. A token left stale by a reshape reads "not held" (fail closed)
     *          until the reload callback republishes, a window of a few milliseconds.
     */
    struct ZoomBindingTokens
    {
        DMK::input::BindingToken zoom_in;
        DMK::input::BindingToken zoom_out;
    };
    static std::atomic<std::shared_ptr<const ZoomBindingTokens>> s_zoom_tokens;

    // Resolved SSystemGlobalEnvironment (g_env) base, set once at init by the AOB cascade (0 when it missed).
    // Reused by the player/animchar walks. p_physical_world is g_env + PHYSICAL_WORLD_OFFSET.
    static uintptr_t s_genv_runtime = 0;

    // CView vtable address, cached lazily on the first frustum-builder camera whose embedding object
    // passes the CView RTTI check. The steady-state game-view gate is then a single qword compare;
    // using the RTTI type name (not a hardcoded address) keeps the gate working across game patches.
    static uintptr_t s_cview_vtable_runtime = 0;

    // Latched from the head-visibility setter (or adopted from the player's stored setter
    // arguments, see adopt_head_state) so the per-frame re-assert can drive the head without
    // waiting for the game to call the setter again (toggling the offset does not make the
    // game call it, so without the re-assert the head would stay hidden after an FPV/TPV
    // switch). The setter only ever runs for the player (it toggles the FirstPersonView
    // rig), so the entity is the player. s_head_was_active tracks the offset's active state
    // across frames so the head is restored to the game's intended value exactly once when
    // the offset turns off.
    static std::atomic<uintptr_t> s_head_entity{0};
    static std::atomic<uint8_t> s_head_flags{0};
    static std::atomic<bool> s_game_intended_hide_head{false};
    static std::atomic<bool> s_head_was_active{false};

    // The offset's effective active state (effective_tpv && should_apply_view), published by the
    // frustum detour each frame. The head-visibility detour and the free-look input gate read it so
    // they mirror the live offset state without recomputing the whole forced-view policy themselves.
    static std::atomic<bool> s_offset_active{false};

    // First-person fallback, published by the offset each frame: true while the camera has no room behind the player
    // (the arm is shorter than HeadClearance) and sits at the eye, so the head must be hidden like in first person or
    // the view would be inside it. Read by the head re-assert and the head-visibility detour.
    static std::atomic<bool> s_head_fallback{false};
    // Set while the close-up fade has the character mostly dithered out (see k_close_fade_head_hide_on): the head is
    // hidden like in first person. Read with s_head_fallback by the head re-assert and the head-visibility detour;
    // unlike it, it leaves the camera where it is.
    static std::atomic<bool> s_head_close_fade{false};

    // Published by the frustum detour each game-view frame: true while the game is showing the OS cursor
    // (a UI is up). The free-look input gate reads it to FREEZE the orbit - hold its angles and ignore
    // mouse-look - while any cursor UI is open (menu, inventory, loot/trade, dialogue), so the camera
    // does not turn from cursor motion and resumes from the same angle when the cursor hides. The orbit
    // hook captures raw mouse UPSTREAM of the engine's own input freeze, so it needs this explicit gate.
    static std::atomic<bool> s_cursor_shown{false};

    // Camera-relative movement (toggle orbit). A move starts when the device-agnostic movement-input magnitude
    // (~0..1.4) rises above MOVE_INPUT_START from idle, which aligns the heading to the camera once, and ends only
    // below the lower k_orbit_move_input_stop (hysteresis, so a momentary dip cannot re-trigger the align). The input
    // stays nonzero while a key is held against a wall, so the heading is not falsely released on a collision stop.
    // k_orbit_aim_level_speed eases the look pitch toward level per second while orbiting.
    constexpr float k_orbit_move_input_stop = 0.05f;
    // A move armed by the game's own move input (see the re-arm guard) also ends once that input has stayed idle this
    // long, which covers the game's few frames of lag behind the keys.
    constexpr float k_orbit_move_game_stop_delay = 0.1f; // seconds
    constexpr float k_orbit_aim_level_speed = 8.0f;

    // Orbit angle low-pass. The engine smooths native look DOWNSTREAM of the input dispatch the orbit
    // hook captures and blocks, so free-look applies the raw per-frame mouse deltas and reads as jittery.
    // orbit_smoothing (0..1) maps to a frame-rate-independent catch-up speed between these bounds: higher
    // smoothing -> lower speed -> more lag (smoother); 0 disables the filter (snap to the raw target).
    constexpr float k_orbit_smooth_min_speed = 6.0f;  // strength 1.0: heavy smoothing
    constexpr float k_orbit_smooth_max_speed = 40.0f; // strength near 0: barely-there smoothing

    // Aim hand-off: the least turn or rise off the plain rig worth a hand-off, and the heading gap at which the look
    // counts as turned; how long it waits for the look; the body step between two frames that counts as a load or a
    // teleport rather than a run or a hitch; and the bound on the look pitch it writes.
    constexpr float k_aim_handoff_min_angle = 0.0175f; // rad (1 deg)
    constexpr float k_aim_handoff_timeout = 0.5f;      // seconds
    constexpr float k_aim_handoff_jump = 3.0f;         // meters per frame
    constexpr float k_aim_handoff_max_pitch = 1.4f;    // rad (80 deg)
    // Situations in which the game sets which way the character faces, so the aim hand-off leaves the facing alone:
    // riding a horse or a cart, sitting, lying or kneeling, a conversation, and the minigames that put the character at
    // a lock, a table, an anvil and the like. The throwing and shooting minigames are aimed by the player.
    constexpr uint32_t k_game_held_facing_states =
        state_bit(GameState::Mount) | state_bit(GameState::Cart) | state_bit(GameState::Lying) |
        state_bit(GameState::Sitting) | state_bit(GameState::Kneel) | state_bit(GameState::Dialogue) |
        state_bit(GameState::MinigameSharpening) | state_bit(GameState::MinigameReading) |
        state_bit(GameState::MinigameAlchemy) | state_bit(GameState::MinigameHerbGathering) |
        state_bit(GameState::MinigameLockpicking) | state_bit(GameState::MinigameHoleDigging) |
        state_bit(GameState::MinigameDice) | state_bit(GameState::MinigamePickpocketing) |
        state_bit(GameState::MinigameBlacksmithing) | state_bit(GameState::MinigameForgeBuilder);

    // Aim-basis low-pass (AimBasisSmoothing). Maps the 0..1 strength to a frame-rate-independent slerp
    // catch-up speed: higher strength -> lower speed -> more damping (and slightly laggier aim). The window
    // is faster than the orbit low-pass so even at full strength the camera still tracks genuine aim turns
    // closely while shedding the high-frequency view-shake.
    constexpr float k_basis_smooth_min_speed = 8.0f;  // strength 1.0: heaviest damping
    constexpr float k_basis_smooth_max_speed = 60.0f; // strength near 0: barely-there damping

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
    // The states read from the live UI signals rather than the debounced mask: the suppression gate and the
    // orbit-exclude policy both react on the frame a menu or an overlay opens or closes.
    constexpr uint32_t k_live_ui_states = state_bit(GameState::Menu) | state_bit(GameState::Overlay);

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
        if ((suppress & state_bit(GameState::Overlay)) != 0 && overlay_state().active.load(std::memory_order_relaxed))
        {
            return false;
        }
        // Any other listed state is matched against the debounced mask published each game-view frame.
        const uint32_t gameplay = suppress & ~k_live_ui_states;
        if (gameplay != 0 && (gameplay & game_state_mask().load(std::memory_order_relaxed)) != 0)
        {
            return false;
        }
        return true;
    }

    // ITimer::GetFrameStartTime and ITimer::GetFrameTime (see constants.hpp): the engine frame clock.
    using TimerGetFrameStartTimeFunc = const std::int64_t *(__fastcall *)(uintptr_t timer, int which);
    using TimerGetFrameTimeFunc = float(__fastcall *)(uintptr_t timer, int which);

    /**
     * @brief One engine frame as the frame clock reports it.
     */
    struct EngineFrame
    {
        /// UI-clock frame-start stamp, identical for every read within one frame.
        std::int64_t start_stamp;
        /// Seconds the world advanced this frame.
        float seconds;
        /// The game clock is stopped (the pause menu) and seconds is the UI clock's.
        bool paused;
    };

    /**
     * @brief Reads the engine frame clock through the g_env timer.
     * @details The timer is confirmed by its RTTI name and both vtable slots must point into the game image
     *          before either is called; the frame time must come back finite and non-negative. The game clock
     *          reads 0 while the game timer is paused (the pause menu), and the UI clock's frame time stands in
     *          then: nothing in the world moves, so its lack of smoothing cannot show, and the view still eases
     *          into and out of the menu. Runs under the frustum detour's SEH frame, which contains a fault in
     *          either engine call.
     * @return The frame, or std::nullopt when the clock cannot be trusted.
     */
    [[nodiscard]] static std::optional<EngineFrame> read_engine_frame()
    {
        const ModuleInfo &mod = module_info();
        if (s_genv_runtime == 0 || mod.base == 0)
        {
            return std::nullopt;
        }
        const auto timer = DMK::memory::read<uintptr_t>(DMK::Address{s_genv_runtime + Constants::GENV_TIMER_OFFSET});
        if (!timer || !DMK::memory::is_plausible_ptr(DMK::Address{*timer}))
        {
            return std::nullopt;
        }
        const auto vtable = DMK::memory::read<uintptr_t>(DMK::Address{*timer});
        if (!vtable || !vtable_is(GameClass::Timer, *vtable))
        {
            return std::nullopt;
        }
        const auto start_fn =
            DMK::memory::read<uintptr_t>(DMK::Address{*vtable + Constants::ITIMER_VTABLE_GET_FRAME_START_TIME_OFFSET});
        const auto time_fn =
            DMK::memory::read<uintptr_t>(DMK::Address{*vtable + Constants::ITIMER_VTABLE_GET_FRAME_TIME_OFFSET});
        const auto in_image = [&mod](const auto &fn) { return fn && *fn >= mod.base && *fn < mod.base + mod.size; };
        if (!in_image(start_fn) || !in_image(time_fn))
        {
            return std::nullopt;
        }

        const std::int64_t *stamp_slot =
            reinterpret_cast<TimerGetFrameStartTimeFunc>(*start_fn)(*timer, Constants::ETIMER_UI);
        const auto stamp = DMK::memory::read<std::int64_t>(DMK::Address{reinterpret_cast<uintptr_t>(stamp_slot)});
        if (!stamp)
        {
            return std::nullopt;
        }
        const auto get_frame_time = reinterpret_cast<TimerGetFrameTimeFunc>(*time_fn);
        float seconds = get_frame_time(*timer, Constants::ETIMER_GAME);
        const bool paused = !(seconds > 0.0f);
        if (paused)
        {
            seconds = get_frame_time(*timer, Constants::ETIMER_UI);
        }
        if (!std::isfinite(seconds) || seconds < 0.0f)
        {
            return std::nullopt;
        }
        return EngineFrame{*stamp, seconds, paused};
    }

    // The heading a movement press captures (radians), when it was published (GetTickCount64, 0 = none) and
    // the thread the camera ran on, for orbit_move_pressed. A heading older than k_move_press_max_age_ms is not used.
    static std::atomic<float> s_move_press_heading{0.0f};
    static std::atomic<std::uint64_t> s_move_press_stamp_ms{0};
    static std::atomic<DWORD> s_move_press_thread{0};
    static std::atomic<float> s_move_press_body_yaw{0.0f};
    // Set by a press that turned the body by more than k_move_press_min_turn: the next movement request of that
    // animated character (detour_phys_ent_movement) was built along the old facing. Stamped (GetTickCount64) so a
    // stale one is ignored; the stamp is stored before the character.
    static std::atomic<std::uint64_t> s_move_press_turned_ms{0};
    static std::atomic<std::uintptr_t> s_move_press_animchar{0};
    constexpr float k_move_press_min_turn = 0.087f; // rad (5 deg)
    constexpr std::uint64_t k_move_press_max_age_ms = 100;

    /** @brief What one game-view frustum build advances by, as frame_delta reports it. */
    struct FrameStep
    {
        /// Seconds the camera's per-frame integrators and easings advance on this build, clamped.
        float seconds{0.0f};
        /**
         * @brief The frame's second build.
         * @details Work that must run exactly once per engine frame (the archery step) keys on this flag, not on a
         *          zero seconds value: the wall-clock fallback returns the small gap for the second build, not 0.
         */
        bool repeat_build{false};
        /// The game clock stands still (the pause menu); false when the engine clock cannot be read.
        bool paused{false};
    };

    /**
     * @brief Measures one game-view frustum build: the seconds it advances, whether it repeats the frame, and whether
     *        the game clock is paused.
     * @details The game view's frustum is built twice per frame (the projection setup, then the final rebuild).
     *          The frame's time is spent on the first build and the second advances by 0; the engine's
     *          frame-start stamp tells the two apart.
     *
     *          The duration is the engine's own game frame time, not the wall-clock gap between two builds. The
     *          engine averages its frame time over the last quarter second and moves the whole world by that
     *          average, so everything it animates advances by a near-constant step per frame. The raw gap between
     *          two builds carries the full frame-to-frame variance instead (about a tenth of a frame on average,
     *          up to half a frame). A rate-driven motion paced on it, the gamepad orbit most of all, then turns by
     *          an uneven step each frame against an evenly moving world, which reads as micro-stutter in a slow
     *          pan. Pacing on the engine clock keeps the camera in step with the world. The wall-clock gap is the
     *          fallback whenever the engine clock cannot be read, and it keeps running every call so that
     *          fallback always measures from the previous build.
     * @return This build's step.
     */
    [[nodiscard]] static FrameStep frame_delta()
    {
        static std::chrono::steady_clock::time_point s_last_time = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        const float wall_delta = std::chrono::duration<float>(now - s_last_time).count();
        s_last_time = now;
        FrameStep step{};
        // Without the engine clock, a build within a couple of milliseconds of the previous one is the same frame's
        // second build.
        step.repeat_build = wall_delta < 0.002f;

        const std::optional<EngineFrame> frame = read_engine_frame();
        step.paused = frame.has_value() && frame->paused;

        // Report which clock paces the camera once per switch, so a log shows whether the engine clock resolved.
        enum class ClockSource
        {
            None,
            Engine,
            Wall,
        };
        static ClockSource s_clock_source = ClockSource::None;
        const ClockSource clock_source = frame.has_value() ? ClockSource::Engine : ClockSource::Wall;
        if (clock_source != s_clock_source)
        {
            s_clock_source = clock_source;
            const char *const clock_name =
                frame.has_value() ? "engine frame clock" : "wall clock (engine clock unavailable)";
            (void)DMK::log().try_log(DMK::LogLevel::Debug, "Camera: pacing on the {}", clock_name);
        }

        if (frame.has_value())
        {
            static std::int64_t s_last_stamp = 0;
            step.repeat_build = frame->start_stamp == s_last_stamp;
            if (step.repeat_build)
            {
                return step; // second build of the same frame: the frame's time was already spent
            }
            s_last_stamp = frame->start_stamp;
            step.seconds = std::clamp(frame->seconds, 0.0f, 0.1f);
            return step;
        }
        step.seconds = std::clamp(wall_delta, 0.0f, 0.1f);
        return step;
    }

    /**
     * @brief Resolves the live C_Player via g_env each frame (validated by its vtable); 0 on failure.
     * @details Walks g_env -> p_game -> CCryAction (cached, resolved once via a virtual GetIGameFramework
     *          call) -> p_action_game -> C_Player, then confirms C_Player by its main vtable. Resolving
     *          FRESH every frame - rather than trusting a mirrored pointer that goes null/stale across
     *          view transitions and reloads - is what keeps the move-detection and body-turn locked onto
     *          the CURRENT player. Always called from within an SEH frame (the frustum detour, the
     *          body-turn wrapper, or the action dispatcher's guarded capture through orbit_move_pressed), so a fault
     *          during the walk is contained.
     */
    static uintptr_t resolve_c_player()
    {
        const ModuleInfo &mod = module_info();
        if (s_genv_runtime == 0 || mod.base == 0)
        {
            return 0;
        }
        const uintptr_t g_env_addr = s_genv_runtime;
        const auto p_game = DMK::memory::read<uintptr_t>(DMK::Address{g_env_addr + Constants::GENV_PGAME_OFFSET});
        if (!p_game || !DMK::memory::is_plausible_ptr(DMK::Address{*p_game}))
        {
            return 0;
        }

        static uintptr_t s_cry_action = 0;
        if (!DMK::memory::is_plausible_ptr(DMK::Address{s_cry_action}))
        {
            const auto vtable = DMK::memory::read<uintptr_t>(DMK::Address{*p_game});
            if (!vtable || !DMK::memory::is_plausible_ptr(DMK::Address{*vtable}))
            {
                return 0;
            }
            const auto fn =
                DMK::memory::read<uintptr_t>(DMK::Address{*vtable + Constants::IGAME_GET_FRAMEWORK_VTABLE_OFFSET});
            if (!fn || !DMK::memory::is_plausible_ptr(DMK::Address{*fn}))
            {
                return 0;
            }
            using GetFrameworkFn = uintptr_t(__fastcall *)(uintptr_t);
            s_cry_action = reinterpret_cast<GetFrameworkFn>(*fn)(*p_game);
            if (!DMK::memory::is_plausible_ptr(DMK::Address{s_cry_action}))
            {
                s_cry_action = 0;
                return 0;
            }
        }

        // Publish the chain root for the CCryAction -> CActionGame heal group. The scheduler owns the cadence
        // and the latch: its gate stays shut (silently) until the slot holds a constructed CActionGame, so the
        // offset is nominal until then and fixed afterwards. A layout shift here would silently break every
        // walk below, which is why it is the first group registered.
        note_framework_base(s_cry_action);

        const auto p_action_game = DMK::memory::read<uintptr_t>(
            DMK::Address{s_cry_action + runtime_offsets().ccryaction_actiongame.load().value});
        if (!p_action_game || !DMK::memory::is_plausible_ptr(DMK::Address{*p_action_game}))
        {
            return 0;
        }
        note_action_game_base(*p_action_game);

        // Read the local actor through the self-healed CActionGame offset, then confirm it is a real C_Player
        // by its vtable. C_Player is found THROUGH this offset, so the offset cannot be healed from a resolved
        // C_Player; instead, when the cached slot holds a populated object that is NOT a C_Player (the
        // signature of a CActionGame layout drift), attempt a bounded recovery scan and re-read. The nominal
        // slot is probed first inside the heal, so an undrifted build never scans.
        RuntimeOffsets &offsets = runtime_offsets();
        auto player =
            DMK::memory::read<uintptr_t>(DMK::Address{*p_action_game + offsets.cactiongame_local_actor.load().value});
        const auto is_c_player = [](const DMK::Result<uintptr_t> &candidate)
        {
            if (!candidate || !DMK::memory::is_plausible_ptr(DMK::Address{*candidate}))
            {
                return false;
            }
            const auto vt = DMK::memory::read<uintptr_t>(DMK::Address{*candidate});
            return vt.has_value() && vtable_is(GameClass::Player, *vt);
        };
        bool player_valid = is_c_player(player);
        if (!player_valid && player && DMK::memory::is_plausible_ptr(DMK::Address{*player}))
        {
            // The cached slot holds a populated object that is NOT a C_Player: the signature of a CActionGame
            // layout drift. Open the local-actor heal group's gate and let the scheduler recover the offset on
            // its own cadence; it scans CActionGame for the C_Player slot and latches once it resolves. The read
            // above then succeeds on a later frame and this branch stops being entered (the natural stop). The
            // request is idempotent, so a frame that re-enters here simply re-asserts it: the scheduler, not
            // this call site, owns the "do not scan every frame" rate limit and the never-give-up retry.
            request_local_actor_recovery();
        }
        if (!player_valid)
        {
            return 0;
        }

        // A real C_Player is in hand (vtable validated above): publish it for the player-rooted heal groups, so
        // the per-frame aim and body-turn walks read the recovered offsets on a drifted build.
        note_player_base(*player);

        // Log the resolved C_Player and cached CCryAction only when the player pointer CHANGES (once on first
        // resolve, and again after a reload / new player) so the address is available for external tooling
        // without flooding the log.
        {
            static uintptr_t s_logged_player{0};
            if (*player != s_logged_player)
            {
                s_logged_player = *player;
                (void)DMK::log().try_log(DMK::LogLevel::Debug, "C_Player resolved={} (CCryAction={})",
                                         DMK::format::format_address(*player),
                                         DMK::format::format_address(s_cry_action));
            }
        }

        // The player resolves once the game is in-world (window at its final resolution); let the overlay
        // wait on this so it never sizes itself to the transient loading window.
        game_world_ready().store(true, std::memory_order_relaxed);

        return *player;
    }

    /**
     * @brief Resolves the player look controller and drives the real aim: eases the PITCH toward a target (level
     *        while orbiting) and/or sets the YAW (heading).
     * @details Walks the player look chain (g_env -> p_game -> CCryAction -> p_action_game -> C_Player ->
     *          look controller; see constants.hpp) and validates C_Player by its vtable. The look
     *          quaternion the cameras read is RE-DERIVED from the controller's scalar pitch+yaw every
     *          frame, so writing those scalars (not the derived quat, which is overwritten) is what
     *          actually moves the eye, the character head AND the movement heading. While orbiting the mod
     *          redirects the look input, so the writes stick. Outside orbit (the aim hand-off) the game re-derives
     *          the look yaw from the body heading while aiming, so a yaw write sticks only together with a body
     *          turn (apply_orbit_body_turn). When the controller is unreachable nothing is written (the
     *          camera-side held base is unaffected); a false return can also follow a partial write.
     *          CCryAction is process-lifetime and resolved once via a virtual GetIGameFramework call, then
     *          cached; the rest is a guarded walk.
     * @param pitch_ease Per-frame fraction to move the look pitch toward pitch_target, in [0, 1] (0 = leave it,
     *        1 = set it).
     * @param pitch_target Look pitch to ease toward, in radians (0 = level, negative = down).
     * @param set_yaw When true, the look yaw is set to yaw_value to align the heading to the camera.
     * @param yaw_value Target look yaw in radians (engine convention: forward = (-sin yaw, cos yaw)).
     * @return True when every requested write landed; false when the controller is unreachable, a value
     *         failed its sanity check or a write was refused.
     */
    [[nodiscard]] static bool apply_orbit_aim_control(float pitch_ease, float pitch_target, bool set_yaw,
                                                      float yaw_value)
    {
        const ModuleInfo &mod = module_info();
        if (s_genv_runtime == 0 || mod.base == 0)
        {
            return false;
        }
        const uintptr_t g_env_addr = s_genv_runtime;
        const auto p_game = DMK::memory::read<uintptr_t>(DMK::Address{g_env_addr + Constants::GENV_PGAME_OFFSET});
        if (!p_game || !DMK::memory::is_plausible_ptr(DMK::Address{*p_game}))
        {
            return false;
        }

        // CCryAction is process-lifetime; resolve it once via p_game->IGame::GetIGameFramework() (a
        // trivial member getter) and cache, so the per-frame path is a pure guarded pointer walk.
        static uintptr_t s_cry_action = 0;
        if (!DMK::memory::is_plausible_ptr(DMK::Address{s_cry_action}))
        {
            const auto vtable = DMK::memory::read<uintptr_t>(DMK::Address{*p_game});
            if (!vtable || !DMK::memory::is_plausible_ptr(DMK::Address{*vtable}))
            {
                return false;
            }
            const auto fn =
                DMK::memory::read<uintptr_t>(DMK::Address{*vtable + Constants::IGAME_GET_FRAMEWORK_VTABLE_OFFSET});
            if (!fn || !DMK::memory::is_plausible_ptr(DMK::Address{*fn}))
            {
                return false;
            }
            using GetFrameworkFn = uintptr_t(__fastcall *)(uintptr_t);
            s_cry_action = reinterpret_cast<GetFrameworkFn>(*fn)(*p_game);
            if (!DMK::memory::is_plausible_ptr(DMK::Address{s_cry_action}))
            {
                s_cry_action = 0;
                return false;
            }
        }

        // Publish the chain root for the heal group, as the resolver does.
        note_framework_base(s_cry_action);

        // CCryAction -> CActionGame -> local actor in one guarded walk. walk() issues one guarded read per hop
        // and screens each result against that hop's validity floor, which is exactly the per-hop read plus
        // plausibility check this used to spell out, and it reports the failing hop index in Error::detail.
        const RuntimeOffsets &offsets = runtime_offsets();
        const std::array<std::ptrdiff_t, 3> actor_chain{offsets.ccryaction_actiongame.load().value,
                                                        offsets.cactiongame_local_actor.load().value, 0};
        const auto c_player = DMK::memory::walk(DMK::Address{s_cry_action}, actor_chain);
        if (!c_player || !DMK::memory::is_plausible_ptr(*c_player))
        {
            return false;
        }
        // Confirm this is really C_Player (its main vtable) before trusting the controller offset.
        const auto vt = DMK::memory::read<uintptr_t>(*c_player);
        if (!vt || !vtable_is(GameClass::Player, *vt))
        {
            return false;
        }
        // This offset decides where a WRITE lands, so demand a Confirmed heal rather than accepting the
        // retained nominal: on a genuinely drifted C_Player the store would land in whatever member now
        // occupies the slot. A miss simply skips the aim control for the frame (fail closed).
        const auto look_controller_offset = offsets.c_player_look_controller.authorized();
        if (!look_controller_offset)
        {
            return false;
        }
        const auto controller = DMK::memory::read<uintptr_t>(c_player->offset(*look_controller_offset));
        if (!controller || !DMK::memory::is_plausible_ptr(DMK::Address{*controller}))
        {
            return false;
        }
        // Ease the SCALAR pitch toward pitch_target. Both synchronized copies are written so any internal
        // current/target smoothing also settles there. A sane pitch is within about +/- 1.6 rad; a
        // wild or non-finite value means the layout drifted, so that write is skipped.
        //
        // Every store below goes through memory::write_in_place, the library's per-frame data write: a guarded
        // copy that changes NO page protection and fails closed with WriteFaulted when the target is not
        // already writable. That is the right primitive for a value written every frame through a RESOLVED
        // address (a scanned base plus a pointer chain that can go stale between frames), and it is why this
        // function needs no SEH wrapper of its own: a stale chain reports a fault instead of raising one, and
        // a chain that drifted onto a read-only page is rejected rather than silently unprotected. It is also
        // an out-of-line library call, so the consume-once store the engine reads on its own thread cannot be
        // elided - the intent the old volatile qualifier carried.
        bool wrote = true;
        if (pitch_ease > 0.0f)
        {
            const DMK::Address pitch_addr = DMK::Address{*controller + Constants::LOOK_CONTROLLER_PITCH_OFFSET};
            const auto pitch_value = DMK::memory::read<float>(pitch_addr);
            if (pitch_value && *pitch_value > -3.2f && *pitch_value < 3.2f && std::isfinite(pitch_target))
            {
                const float eased = *pitch_value + (pitch_target - *pitch_value) * pitch_ease;
                wrote = DMK::memory::write_in_place<float>(pitch_addr, eased) &&
                        DMK::memory::write_in_place<float>(
                            DMK::Address{*controller + Constants::LOOK_CONTROLLER_PITCH2_OFFSET}, eased);
            }
            else
            {
                wrote = false;
            }
        }

        // Set the look YAW (heading) to face the camera direction. The character moves along this heading,
        // so this is what turns the body and makes movement camera-relative on the idle -> moving edge.
        // Both synchronized copies are written. yaw_value is finite-checked first: a non-finite heading (a
        // layout drift feeding NaN through the derivation) must never be written into the engine's look state.
        if (set_yaw)
        {
            const bool yaw_wrote = std::isfinite(yaw_value) &&
                                   DMK::memory::write_in_place<float>(
                                       DMK::Address{*controller + Constants::LOOK_CONTROLLER_YAW_OFFSET}, yaw_value) &&
                                   DMK::memory::write_in_place<float>(
                                       DMK::Address{*controller + Constants::LOOK_CONTROLLER_YAW2_OFFSET}, yaw_value);
            wrote = wrote && yaw_wrote;
        }
        return wrote;
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
     *          Resolution mirrors apply_orbit_aim_control: g_env -> p_game -> CCryAction (cached) ->
     *          p_action_game -> C_Player (validated vtable) -> C_AnimatedHuman -> CAnimatedCharacter,
     *          validated by the animchar vtable before any write. The quat is written before the active
     *          byte so the game never observes active==1 with a torn/stale quat.
     *          See constants.hpp (ANIMCHAR_*, C_PLAYER_ANIMATED_HUMAN_OFFSET) for the offsets.
     * @return The player's animated character the turn was written to, or 0 when nothing was written.
     */
    static uintptr_t apply_orbit_body_turn(float target_yaw)
    {
        const ModuleInfo &mod = module_info();
        const uintptr_t c_player = resolve_c_player();
        if (mod.base == 0 || c_player == 0)
        {
            return 0;
        }

        // C_Player -> C_AnimatedHuman (+0x268) -> CAnimatedCharacter (+0x20). Both offsets decide where a
        // WRITE lands, so each must be a Confirmed heal rather than a retained nominal; on a drifted layout
        // the quat would otherwise be stored over whatever member now occupies the slot. A miss skips the
        // body turn for the frame (fail closed).
        const RuntimeOffsets &offsets = runtime_offsets();
        const auto animated_human_offset = offsets.c_player_animated_human.authorized();
        const auto animchar_offset = offsets.animated_human_animchar.authorized();
        if (!animated_human_offset || !animchar_offset)
        {
            return 0;
        }

        // One guarded walk instead of a read-and-screen per hop: walk() reads each hop under the same fault
        // guard, screens it against that hop's validity floor, and reports the failing hop index on a miss.
        const std::array<std::ptrdiff_t, 3> animchar_chain{*animated_human_offset, *animchar_offset, 0};
        const auto anim_char = DMK::memory::walk(DMK::Address{c_player}, animchar_chain);
        if (!anim_char || !DMK::memory::is_plausible_ptr(*anim_char))
        {
            return 0;
        }
        // Validate the animchar vtable before touching its override fields; a mismatch means the layout
        // drifted, so skip this frame.
        const auto avt = DMK::memory::read<uintptr_t>(*anim_char);
        if (!avt || !vtable_is(GameClass::AnimatedCharacter, *avt))
        {
            return 0;
        }

        // Yaw-only world quat (XYZW): rotation about +Z by target_yaw == {0, 0, sin(y/2), cos(y/2)}. The four
        // components are contiguous and written as ONE guarded store, so the quat cannot straddle a protection
        // boundary mid-write and there is no per-component call. memory::write_in_place is the per-frame data
        // write: guarded, no protection change, and it fails closed instead of faulting the host when the
        // resolved chain has gone stale - which is why this function needs no SEH wrapper of its own.
        const float half = target_yaw * 0.5f;
        const std::array<float, 4> quat{0.0f, 0.0f, std::sin(half), std::cos(half)};
        static_assert(Constants::QUAT_X_OFFSET == 0 && Constants::QUAT_Y_OFFSET == 4 &&
                          Constants::QUAT_Z_OFFSET == 8 && Constants::QUAT_W_OFFSET == 12,
                      "The quat components must be contiguous XYZW floats for the single-store write.");
        if (!DMK::memory::write_in_place<std::array<float, 4>>(
                anim_char->offset(Constants::ANIMCHAR_OVERRIDE_ROT_QUAT_OFFSET), quat))
        {
            return 0;
        }
        // Set the active byte LAST so the animated-character update (a separate consumer) never reads
        // active==1 with a half-written quat. The release fence orders the quat store before the active
        // store explicitly rather than relying on the target's store-store ordering; on x86 it lowers to a
        // compiler barrier with no runtime cost.
        std::atomic_thread_fence(std::memory_order_release);
        if (!DMK::memory::write_in_place<unsigned char>(
                anim_char->offset(Constants::ANIMCHAR_OVERRIDE_ROT_ACTIVE_OFFSET), 1))
        {
            return 0;
        }
        return anim_char->raw();
    }

    // Turn-in-place pivot hold tuning. A native turn step is the body rotating TOWARD the look (faster than
    // k_turn_hold_yaw_rate) while it trails the look by more than k_turn_hold_min_gap, moving no faster than
    // k_turn_hold_max_speed and with no movement input. Its displacement, and that of the step's foot settle for
    // k_turn_hold_settle after the rotation stops, is held off the pivot. A body that moves faster than
    // k_turn_hold_follow_speed without being held is really moving, and the hold eases back onto it at
    // k_turn_hold_release_rate. A per-frame jump beyond k_turn_hold_teleport is a teleport or a load, not a step, and
    // drops the hold. The hold never exceeds k_turn_hold_max. With the turn steps kept in place
    // (detour_phys_ent_movement) there is normally nothing to hold; this keeps the camera still when that hook is
    // unavailable.
    constexpr float k_turn_hold_yaw_rate = 0.26f;    // rad/s (15 deg/s)
    constexpr float k_turn_hold_min_gap = 0.17f;     // rad (10 deg)
    constexpr float k_turn_hold_max_speed = 1.2f;    // m/s
    constexpr float k_turn_hold_follow_speed = 0.3f; // m/s
    constexpr float k_turn_hold_settle = 0.35f;      // seconds
    constexpr float k_turn_hold_teleport = 0.25f;    // meters per frame
    constexpr float k_turn_hold_max = 0.5f;          // meters
    constexpr float k_turn_hold_release_rate = 8.0f; // 1/s

    /**
     * @brief Keeps the pivot still while the body turns in place, returning the horizontal offset to hold off it.
     * @details A native turn-in-place animation carries root motion: the body steps 5-20 cm while it turns, and the
     *          rig anchors to the body, so the camera shifts with every turn unless the step is held off. The
     *          displacement of a turn step is absorbed into a hold the caller subtracts from its anchor, so the camera
     *          stays where it was during and after the turn. A turn step is recognised by its signature: the body
     *          rotates toward the look it trails, at step speed, with no movement input. Running to a stop, a
     *          scripted move or a push does not match it, so the camera follows them, and any such movement also
     *          eases the hold back onto the body. Standing still keeps the hold. Without the look the rotation alone
     *          is used. Normally the steps are kept in place (detour_phys_ent_movement) and the hold stays near zero;
     *          it is the fallback for when that hook is unavailable.
     * @param cam Camera state holding the tracker and the hold.
     * @param body_origin The entity world origin this frame.
     * @param body_yaw The entity world yaw this frame (radians).
     * @param look_yaw The look yaw this frame (radians), used only when @p look_valid.
     * @param look_valid Whether @p look_yaw was read.
     * @param delta_time Frame time in seconds.
     * @return The horizontal offset (z = 0) to subtract from the anchor.
     */
    static Vector3 hold_turn_in_place_pivot(CameraState &cam, const Vector3 &body_origin, float body_yaw,
                                            float look_yaw, bool look_valid, float delta_time)
    {
        if (!cam.turn_track_valid)
        {
            cam.turn_track_x = body_origin.x;
            cam.turn_track_y = body_origin.y;
            cam.turn_track_yaw = body_yaw;
            cam.turn_track_valid = true;
            cam.turn_track_step = 0.0f;
            cam.turn_hold_x = 0.0f;
            cam.turn_hold_y = 0.0f;
            cam.turn_hold_timer = 0.0f;
            return Vector3{0.0f, 0.0f, 0.0f};
        }
        const float step_x = body_origin.x - cam.turn_track_x;
        const float step_y = body_origin.y - cam.turn_track_y;
        const float step = std::sqrt(step_x * step_x + step_y * step_y);
        cam.turn_track_step = step;
        const float turn = std::remainder(body_yaw - cam.turn_track_yaw, 2.0f * DirectX::XM_PI);
        cam.turn_track_x = body_origin.x;
        cam.turn_track_y = body_origin.y;
        cam.turn_track_yaw = body_yaw;
        if (step > k_turn_hold_teleport)
        {
            cam.turn_hold_x = 0.0f;
            cam.turn_hold_y = 0.0f;
            cam.turn_hold_timer = 0.0f;
            return Vector3{0.0f, 0.0f, 0.0f};
        }

        const float speed = delta_time > 1e-4f ? step / delta_time : 0.0f;
        const bool native_turn = s_native_turn_actor.load(std::memory_order_relaxed) != 0;
        const bool move_input =
            !player_onaction_available() || player_onaction_move_magnitude() > k_orbit_move_input_stop;
        const bool in_place = native_turn && !move_input && speed < k_turn_hold_max_speed;
        bool toward_look = true;
        if (look_valid)
        {
            const float gap = std::remainder(look_yaw - body_yaw, 2.0f * DirectX::XM_PI);
            toward_look = std::fabs(gap) > k_turn_hold_min_gap && turn * gap > 0.0f;
        }
        if (in_place && toward_look && std::fabs(turn) > k_turn_hold_yaw_rate * delta_time)
        {
            cam.turn_hold_timer = k_turn_hold_settle;
        }
        else
        {
            cam.turn_hold_timer = std::max(0.0f, cam.turn_hold_timer - delta_time);
        }

        if (in_place && cam.turn_hold_timer > 0.0f)
        {
            cam.turn_hold_x += step_x;
            cam.turn_hold_y += step_y;
        }
        else if (!native_turn || speed > k_turn_hold_follow_speed)
        {
            // Real movement (or the feature off). Keyed on the body's speed rather than the movement-input latch, which
            // a combat action-map swap can strand above zero with the keys up.
            const float keep = std::exp(-k_turn_hold_release_rate * delta_time);
            cam.turn_hold_x *= keep;
            cam.turn_hold_y *= keep;
        }
        const float held = std::sqrt(cam.turn_hold_x * cam.turn_hold_x + cam.turn_hold_y * cam.turn_hold_y);
        if (held > k_turn_hold_max)
        {
            const float scale = k_turn_hold_max / held;
            cam.turn_hold_x *= scale;
            cam.turn_hold_y *= scale;
        }
        return Vector3{cam.turn_hold_x, cam.turn_hold_y, 0.0f};
    }

    /**
     * @brief Rebuilds the look orientation from its heading and the scalar pitch, without the stale roll.
     * @details The raw look quat can carry a stale roll angle (see LOOK_CONTROLLER_QUAT_OFFSET).
     *          C_CameraFirstPerson::Update never renders that quat directly. It takes only the heading of the look
     *          forward vector and combines it with the scalar pitch that C_Player vtable slot 59 returns
     *          (controller + LOOK_CONTROLLER_PITCH2_OFFSET), so the roll never reaches the first-person view. This
     *          helper builds RotZ(heading) * RotX(pitch) from the same two inputs, which gives a level aim with no
     *          view shake. The game's own third-person camera renders the raw quat instead, which is why it tilts on
     *          horseback.
     * @param look Look quat read from the controller.
     * @param pitch Scalar look pitch in radians, 0 = level.
     * @return Roll-free look orientation.
     */
    [[nodiscard]] static Quaternion level_look_rotation(const Quaternion &look, float pitch) noexcept
    {
        // Heading of forward = look * +Y, the yaw the engine extracts. The engine writes forward.y as
        // 2 * (y * y + w * w) - 1, which holds only at unit length. The homogeneous form below keeps the heading
        // exact for the slightly non-unit quats the caller's sanity check still accepts.
        const float forward_x = 2.0f * (look.x * look.y - look.w * look.z);
        const float forward_y = look.w * look.w - look.x * look.x + look.y * look.y - look.z * look.z;
        const float heading = std::atan2(-forward_x, forward_y);
        const float sin_heading = std::sin(heading * 0.5f);
        const float cos_heading = std::cos(heading * 0.5f);
        const float sin_pitch = std::sin(pitch * 0.5f);
        const float cos_pitch = std::cos(pitch * 0.5f);
        // Expanded product RotZ(heading) * RotX(pitch) in XYZW order.
        return Quaternion{cos_heading * sin_pitch, sin_heading * sin_pitch, sin_heading * cos_pitch,
                          cos_heading * cos_pitch};
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
     * @param step This build's step (shared with the state poll).
     * @param view_blend Smoothstepped first-person(0) -> third-person(1) blend for the view-switch ease.
     */
    static void offset_game_view_camera(uintptr_t camera, uintptr_t cview, uintptr_t c_player, const FrameStep &step,
                                        float view_blend)
    {
        const float delta_time = step.seconds;
        LiveSettings &cfg = settings();
        CameraState &cam = camera_state();

        // Follow distance = configured base (hot-reloadable INI FollowDistance, re-read every
        // frame so an edit applies live) plus the accumulated zoom offset from the hold
        // keys, clamped to the configured window. The holds are queried through the BindingTokens the control
        // plane publishes (refresh_zoom_binding_tokens), the library's per-frame form of the query. Before the
        // first publish the name-based query stands in.
        const DMK::input::Input &input = DMK::input::Input::instance();
        const std::shared_ptr<const ZoomBindingTokens> zoom_tokens = s_zoom_tokens.load(std::memory_order_acquire);
        const bool zoom_in_held =
            zoom_tokens ? input.is_active(zoom_tokens->zoom_in) : input.is_active(k_zoom_in_binding);
        const bool zoom_out_held =
            zoom_tokens ? input.is_active(zoom_tokens->zoom_out) : input.is_active(k_zoom_out_binding);
        float zoom_offset = cam.zoom_offset.load(std::memory_order_relaxed);
        const float zoom_step = cfg.zoom_step.load(std::memory_order_relaxed);
        if (zoom_in_held)
        {
            zoom_offset -= zoom_step * delta_time; // zoom in pulls the camera closer
        }
        if (zoom_out_held)
        {
            zoom_offset += zoom_step * delta_time;
        }
        const float base_distance = cfg.follow_distance.load(std::memory_order_relaxed);
        const float distance =
            std::clamp(base_distance + zoom_offset, cfg.follow_distance_min.load(std::memory_order_relaxed),
                       cfg.follow_distance_max.load(std::memory_order_relaxed));
        cam.zoom_offset.store(distance - base_distance, std::memory_order_relaxed);

        // Read the eye anchor and the orientation quat from the untouched CView pose. The basis is
        // derived from the quat (CryEngine column-vector convention: right = quat * +X, forward =
        // quat * +Y, up = quat * +Z). Reading the quat rather
        // than the matrix columns keeps the basis idempotent across the frustum builder's same-frame
        // rebuilds (we overwrite the matrix, never the pose, so the pose stays the clean eye input).
        // The matrix to offset is the camera's 3x4 at offset 0 (cview + SVIEWPARAMS_VIEWMATRIX_OFFSET).
        // The eye pose is read through memory::read (memcpy semantics into a trivially-copyable Vector3 /
        // Quaternion): it screens the source against the low-address floor and a wrap guard, swallows an
        // access fault via its own SEH frame, and avoids the type-pun UB of a placement reinterpret_cast
        // read, so a stale/torn cview fails closed here rather than depending solely on the outer detour
        // SEH frame.
        const auto eye_position_read =
            DMK::memory::read<Vector3>(DMK::Address{cview + Constants::SVIEWPARAMS_POSITION_OFFSET});
        const auto eye_rotation_read =
            DMK::memory::read<Quaternion>(DMK::Address{cview + Constants::SVIEWPARAMS_ROTATION_OFFSET});
        if (!eye_position_read || !eye_rotation_read)
        {
            return; // CView pose read faulted; leave the view untouched this frame
        }
        const Vector3 eye_position = *eye_position_read;
        const Quaternion eye_rotation = *eye_rotation_read;
        // FPV-side basis (the engine eye orientation). Kept as the view-blend SOURCE so view_blend 0 leaves
        // the untouched first-person view; the third-person rig basis below is built from a STABLE source.
        const Vector3 eye_forward = eye_rotation.rotate(Vector3{0.0f, 1.0f, 0.0f});
        const Vector3 eye_up = eye_rotation.rotate(Vector3{0.0f, 0.0f, 1.0f});

        // Stable rig basis. The third-person rig (camera = pivot - forward * distance) amplifies any rotation of
        // the basis into a position swing. The EyeHeight body anchor removes the POSITIONAL bob, and this removes
        // the ROTATIONAL component the engine bakes into the eye quat during animations (head-bob and weapon-sway
        // rotation, combat / hit / landing shake). StableAimBasis follows the player's clean look-controller AIM
        // instead, leveled by level_look_rotation. That aim carries none of the engine view-shake and equals the eye
        // quat at rest. AimBasisSmoothing low-passes it. basis_overridden records whether the basis was shaped at
        // all. When neither feature is active the basis stays the raw eye quat, and the orientation path below is
        // byte-identical to the convergence/orbit-only behavior (a fail-safe default).
        Quaternion basis_rotation = eye_rotation;
        bool basis_overridden = false;
        Quaternion look_rotation = eye_rotation;
        bool look_valid = false;
        if (cfg.stable_aim_basis.load(std::memory_order_relaxed) && c_player != 0)
        {
            // Read the look controller through the self-healed offset (seeded from the constant, recovered if
            // the C_Player layout drifts) so the basis tracks the same controller the orbit aim control writes.
            // Read-only here (the basis is derived from the controller's quat and pitch), so the retained nominal is
            // acceptable: worst case on a drifted layout it reads neighboring fields and the sanity checks below
            // reject them. The WRITE path (apply_orbit_aim_control) demands a Confirmed heal.
            const std::ptrdiff_t c_player_look_controller_offset =
                runtime_offsets().c_player_look_controller.load().value;
            const auto controller =
                DMK::memory::read<uintptr_t>(DMK::Address{c_player + c_player_look_controller_offset});
            if (controller && DMK::memory::is_plausible_ptr(DMK::Address{*controller}))
            {
                const auto look_quat =
                    DMK::memory::read<Quaternion>(DMK::Address{*controller + Constants::LOOK_CONTROLLER_QUAT_OFFSET});
                const auto look_pitch =
                    DMK::memory::read<float>(DMK::Address{*controller + Constants::LOOK_CONTROLLER_PITCH2_OFFSET});
                if (look_quat && look_pitch)
                {
                    const Quaternion q = *look_quat;
                    const float n2 = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
                    // Accept only a finite, ~unit quaternion and a finite pitch within the engine's +/-1.5608 rad
                    // look clamp, plus a small margin. A layout drift that feeds garbage falls back to the eye quat
                    // and never rotates the rig by a denormal.
                    if (std::isfinite(n2) && n2 > 0.9f && n2 < 1.1f && std::isfinite(*look_pitch) &&
                        std::fabs(*look_pitch) <= 1.6f)
                    {
                        look_rotation = level_look_rotation(q, *look_pitch);
                        look_valid = true;
                    }
                }
            }
        }
        // Low-pass the basis quaternion toward the target: the leveled look orientation when StableAimBasis resolved
        // it, else the raw eye quat. Snaps on the first engaged frame (basis_quat_valid false) so the camera does not
        // swing in from a stale orientation across a first-person gap. AimBasisSmoothing 0 disables the filter
        // (the target is used directly).
        const float basis_smoothing = std::clamp(cfg.aim_basis_smoothing.load(std::memory_order_relaxed), 0.0f, 1.0f);
        if (look_valid || basis_smoothing > 1e-4f)
        {
            basis_overridden = true;
            const DirectX::XMVECTOR target = (look_valid ? look_rotation : eye_rotation).to_xm_vector();
            DirectX::XMVECTOR result;
            if (!cam.basis_quat_valid)
            {
                result = target; // snap on the first engaged frame so the camera does not swing in from stale
                cam.basis_quat_valid = true;
            }
            else if (basis_smoothing > 1e-4f)
            {
                const DirectX::XMVECTOR current =
                    DirectX::XMVectorSet(cam.basis_quat_x, cam.basis_quat_y, cam.basis_quat_z, cam.basis_quat_w);
                const float speed =
                    k_basis_smooth_max_speed - basis_smoothing * (k_basis_smooth_max_speed - k_basis_smooth_min_speed);
                const float t = 1.0f - std::exp(-speed * delta_time);
                // XMQuaternionSlerp takes the shortest arc, so a quaternion double-cover sign flip never spins the
                // basis the long way.
                result = DirectX::XMQuaternionNormalize(DirectX::XMQuaternionSlerp(current, target, t));
            }
            else
            {
                result = target; // StableAimBasis on, smoothing off: use the look target directly
            }
            cam.basis_quat_x = DirectX::XMVectorGetX(result);
            cam.basis_quat_y = DirectX::XMVectorGetY(result);
            cam.basis_quat_z = DirectX::XMVectorGetZ(result);
            cam.basis_quat_w = DirectX::XMVectorGetW(result);
            basis_rotation = Quaternion{cam.basis_quat_x, cam.basis_quat_y, cam.basis_quat_z, cam.basis_quat_w};
        }
        else
        {
            cam.basis_quat_valid = false; // basis not shaped: the next engage snaps the low-pass
        }

        const Vector3 right = basis_rotation.rotate(Vector3{1.0f, 0.0f, 0.0f});
        const Vector3 forward = basis_rotation.rotate(Vector3{0.0f, 1.0f, 0.0f});
        // The look pitch and heading the rig is built from (engine convention: forward = (-sin yaw, cos yaw)).
        const float forward_pitch = std::asin(std::clamp(forward.z, -1.0f, 1.0f));
        const float forward_yaw = std::atan2(-forward.x, forward.y);
        const Vector3 up = basis_rotation.rotate(Vector3{0.0f, 0.0f, 1.0f});
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
        const float game_fov = *reinterpret_cast<const float *>(
            camera + Constants::CCAMERA_PROJECTION_FOV_OFFSET); // radians, freshly set by SetFrustum
        if (view_blend > 0.0f && game_fov > 0.05f && game_fov < 3.0f)
        {
            const float fov_target_degrees = cfg.fov.load(std::memory_order_relaxed);
            const float desired_fov =
                (fov_target_degrees > 0.0f) ? DMK::math::degrees_to_radians(fov_target_degrees) : game_fov;
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
                cam.fov_ease_stage1 +=
                    (desired_fov - cam.fov_ease_stage1) * alpha; // 2-stage critically-damped, like the preset blend
                cam.fov_ease_applied += (cam.fov_ease_stage1 - cam.fov_ease_applied) * alpha;
            }
            // Override while a preset wants a FOV or while still easing back toward the game FOV; otherwise pass
            // through (preset off + settled) so the game keeps its own FOV (including any dynamic FOV).
            if ((fov_target_degrees > 0.0f || std::fabs(cam.fov_ease_applied - game_fov) > 0.005f) &&
                cam.fov_ease_applied > 0.05f && cam.fov_ease_applied < 3.0f)
            {
                const float ratio = std::tan(cam.fov_ease_applied * 0.5f) / std::tan(game_fov * 0.5f);
                *reinterpret_cast<float *>(camera + Constants::CCAMERA_PROJECTION_FOV_OFFSET) =
                    cam.fov_ease_applied; // projection FOV (radians)
                *reinterpret_cast<float *>(camera + Constants::CCAMERA_CULL_EDGE_0_OFFSET) *=
                    ratio; // cull edges (proportional to 1/tan)
                *reinterpret_cast<float *>(camera + Constants::CCAMERA_CULL_EDGE_1_OFFSET) *= ratio;
                *reinterpret_cast<float *>(camera + Constants::CCAMERA_CULL_TAN_OFFSET) /=
                    ratio; // = tan term (inverse)
                *reinterpret_cast<float *>(camera + Constants::CCAMERA_CULL_EDGE_3_OFFSET) *= ratio;
                *reinterpret_cast<float *>(camera + Constants::CCAMERA_CULL_EDGE_4_OFFSET) *= ratio;
            }
        }
        // First-person camera position - the blend source for the view-switch ease. Sourced from the
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
        // shakes the whole view. Instead anchor to the player BODY origin - column 3 of the entity
        // world matrix at OFFSET_ENTITY_WORLD_MATRIX_MEMBER (Matrix34, translation at m[*][3]) - which
        // does not bob - lifted by EyeHeight to roughly eye level along world up. Falls back to the eye
        // anchor when the feature is off (EyeHeight <= 0) or the entity/world matrix is unavailable, so
        // a missing player or a layout drift degrades to the previous behaviour rather than misplacing
        // the camera.
        const Vector3 world_up{0.0f, 0.0f, 1.0f};
        const float eye_height = cfg.eye_height.load(std::memory_order_relaxed);
        // Read the player body world origin once. It feeds the stable anchor (below) AND the
        // device-agnostic movement speed used by the camera-relative move alignment; body_valid gates both.
        Vector3 body_origin{0.0f, 0.0f, 0.0f};
        float body_yaw = 0.0f;
        bool body_valid = false;
        uintptr_t entity_addr = 0;
        {
            // Resolve the entity FRESH from the live C_Player every frame (rather than trusting a mirrored
            // pointer that goes null/stale across view transitions and reloads), which is what keeps the
            // move-detection (hence the camera-relative body-turn and the body anchor) locked onto the
            // CURRENT player. On a failed walk body_valid stays false and the camera degrades to the eye anchor.
            if (c_player != 0)
            {
                const std::ptrdiff_t c_player_entity_offset = runtime_offsets().c_player_entity.load().value;
                const auto ent = DMK::memory::read<uintptr_t>(DMK::Address{c_player + c_player_entity_offset});
                if (ent && DMK::memory::is_plausible_ptr(DMK::Address{*ent}))
                {
                    entity_addr = *ent;
                }
            }
            if (entity_addr != 0 && DMK::memory::is_plausible_ptr(DMK::Address{entity_addr}))
            {
                const DMK::Address matrix_addr =
                    DMK::Address{entity_addr}.offset(Constants::OFFSET_ENTITY_WORLD_MATRIX_MEMBER);
                const auto world_matrix = DMK::memory::read<GameStructures::Matrix34f>(matrix_addr);
                if (world_matrix)
                {
                    body_origin = Vector3{world_matrix->m[0][3], world_matrix->m[1][3], world_matrix->m[2][3]};
                    // Forward is column 1; yaw in the engine convention forward = (-sin yaw, cos yaw).
                    body_yaw = std::atan2(-world_matrix->m[0][1], world_matrix->m[1][1]);
                    body_valid = true;
                }
            }
        }
        // Held off the anchor while the body steps through a native turn-in-place animation, so the camera does not
        // shift with the turn. The look yaw comes from the look quat, or from the eye quat, whose yaw is the look yaw
        // in first person.
        Vector3 turn_hold{0.0f, 0.0f, 0.0f};
        if (body_valid)
        {
            const Vector3 look_fwd = (look_valid ? look_rotation : eye_rotation).rotate(Vector3{0.0f, 1.0f, 0.0f});
            const bool look_yaw_valid = look_fwd.x * look_fwd.x + look_fwd.y * look_fwd.y > 1e-6f;
            const float look_yaw = look_yaw_valid ? std::atan2(-look_fwd.x, look_fwd.y) : 0.0f;
            turn_hold = hold_turn_in_place_pivot(cam, body_origin, body_yaw, look_yaw, look_yaw_valid, delta_time);
        }
        const Vector3 anchor_origin = body_origin - turn_hold;
        // Publish the live REAL eye height (FP eye above the body root) for the overlay read-out and as the
        // dynamic-eye-sync source. eye_position carries head bob; body_origin is the bob-free feet origin.
        if (body_valid)
        {
            cam.real_eye_height.store(eye_position.z - body_origin.z, std::memory_order_relaxed);
        }
        // The eye anchor (EyeHeight 0, or no body) takes the turn hold too, so switching anchor mode with a hold in
        // place does not jump; the hold then eases out as usual.
        Vector3 anchor_base = eye_position - turn_hold;
        if (body_valid && eye_height > 0.0f)
        {
            float effective_eye_height = eye_height;
            bool sync_engaged = false; // true while actively re-anchoring (vs. just using the configured height)
            if (cfg.dynamic_eye_sync.load(std::memory_order_relaxed))
            {
                // Re-anchor the eye HEIGHT to the real FP eye when a low pose (kneel / pray, and other poses
                // EyeHeight does not model) drops it OUT OF RANGE of the configured height - where a fixed
                // height floats the camera too high. Within the threshold (head bob / a minor pose change) keep
                // the steady bob-free height; in the re-anchored low poses we accept the bob (they are mostly
                // stationary). Ease so the swap slides; eye_sync_valid resets on suppression so re-engaging snaps.
                constexpr float k_oor_threshold = 0.25f; // meters: a genuine pose drop, not head bob
                constexpr float k_sync_rate = 9.0f;      // ease rate, 1/sec (frame-rate independent below)
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
            anchor_base = anchor_origin + world_up * effective_eye_height;
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
        // negated below (stick-up raises the camera, like the mouse). GamepadOrbitSpeed X/Y (deg/sec at full stick,
        // one rate per axis so each can be inverted with a negative value) sets the rate - a SEPARATE knob from the
        // mouse's OrbitSensitivity X/Y, because a relative mouse (delta) and an absolute stick (rate) cannot share
        // one linear scale; the pitch is clamped to the same limits.
        // Gated on the cursor-shown freeze (a UI up holds
        // still, mirroring the mouse path); when not orbiting or frozen the latch is cleared so re-engaging with
        // the stick centred does not jump.
        if (orbit_held && !s_cursor_shown.load(std::memory_order_relaxed))
        {
            const float pad_yaw = cam.orbit_pad_yaw.load(std::memory_order_relaxed);
            const float pad_pitch = cam.orbit_pad_pitch.load(std::memory_order_relaxed);
            if (pad_yaw != 0.0f || pad_pitch != 0.0f)
            {
                const float step_x = cfg.gamepad_orbit_speed_x.load(std::memory_order_relaxed) * delta_time;
                const float step_y = cfg.gamepad_orbit_speed_y.load(std::memory_order_relaxed) * delta_time;
                cam.orbit_yaw.store(cam.orbit_yaw.load(std::memory_order_relaxed) - pad_yaw * step_x,
                                    std::memory_order_relaxed);
                const float old_pitch = cam.orbit_pitch.load(std::memory_order_relaxed);
                cam.orbit_pitch.store(
                    std::clamp(old_pitch - pad_pitch * step_y,
                               std::min(cam.orbit_pitch_lo.load(std::memory_order_relaxed), old_pitch),
                               std::max(cam.orbit_pitch_hi.load(std::memory_order_relaxed), old_pitch)),
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
        // forward is this frame's rig basis forward (the stable-aim or smoothed basis when active, else the raw
        // eye look); char_forward_yaw below derives from the SAME forward that positions the camera, so the two
        // cancel and the held world yaw is independent of which source forward tracks. cam.orbit_yaw stays the
        // raw input accumulator throughout; the rendered angle is baked back only on release/stop so free-look resumes
        // from exactly where the moving camera was. orbit_moving implies the key was held, so this also runs on
        // the release frame (orbit_moving is cleared at end of frame), giving the bake-on-release below.
        const bool move_orbit = cam.orbit_moving && cam.orbit_target_valid;
        if (move_orbit)
        {
            const float char_forward_yaw = forward_yaw;
            const float raw_user_since_deg =
                cam.orbit_yaw.load(std::memory_order_relaxed) - cam.orbit_yaw_at_capture_deg;
            // In continuous-align (GTA) mode the user's orbit-since-capture STEERS the run: it drives
            // body_target_yaw (the look + body heading) below, and the camera follows that heading (the
            // derived rig angle cancels to ~0, keeping the camera behind). Smooth that steering input when
            // OrbitSmoothing is on so the turn is fluid instead of tracking the raw per-frame mouse deltas;
            // the SAME smoothed value is reused for body_target_yaw below so the camera stays behind the body.
            // Hold mode keeps the raw value (its orbit-around rides the snapped rig offset, left instant) -
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
                const float speed =
                    k_orbit_smooth_max_speed - orbit_smoothing * (k_orbit_smooth_max_speed - k_orbit_smooth_min_speed);
                const float ease = 1.0f - std::exp(-speed * delta_time);
                cam.orbit_steer_smooth += std::remainder(raw_user_since_deg - cam.orbit_steer_smooth, 360.0f) * ease;
            }
            const float user_orbit_since_deg = steer_smooth ? cam.orbit_steer_smooth : raw_user_since_deg;
            orbit_yaw_deg = std::remainder(
                DMK::math::radians_to_degrees(cam.orbit_target_yaw - char_forward_yaw) + user_orbit_since_deg, 360.0f);
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
        // turn to keep the camera planted, so it must track the body 1:1 - smoothing it would lag the
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
        const bool orbiting = orbit_yaw_deg < -0.05f || orbit_yaw_deg > 0.05f || orbit_pitch_deg < -0.05f ||
                              orbit_pitch_deg > 0.05f || render_yaw_deg < -0.05f || render_yaw_deg > 0.05f ||
                              render_pitch_deg < -0.05f || render_pitch_deg > 0.05f;

        // While free-look is on, the orbit is built from a held base: the follow rig for the look pitch it had when
        // free-look took over, so the camera stays put whatever the look pitch does meanwhile. Until the base is held
        // its pitch follows the look, so on the frame free-look takes over the held base is the pose on screen and is
        // taken at once; easing it in lets the live look (leveled by OrbitLevelAim meanwhile) drag the camera.
        // When free-look turns off, the held pitch eases back onto the look by angle, riding on the look at a
        // decaying offset, so the rig is always a real follow rig: free-look turned back on during the ease holds it
        // where it is, and the live rig takes over once the two meet. The hold keys on orbit_held, not on
        // "orbiting": with OrbitReturnSpeed 0 the angles stay and "orbiting" latches true, which pins the camera
        // to the held base and lock out the look pitch. Easing back on release lets a retained orbit angle ride as a
        // rigid offset on top of the real look, so vertical control is kept in every preset.
        {
            constexpr float k_orbit_base_speed = 8.0f;
            // The ease back ends once the held pitch is this close to the look's.
            constexpr float k_orbit_base_settled = 1e-3f; // rad
            if (!cam.orbit_base_held)
            {
                cam.orbit_base_pitch = forward_pitch;
                cam.orbit_base_offset = 0.0f;
                cam.orbit_base_held = orbit_held;
            }
            else if (orbit_held)
            {
                cam.orbit_base_offset = cam.orbit_base_pitch - forward_pitch;
            }
            else
            {
                cam.orbit_base_offset *= std::exp(-k_orbit_base_speed * delta_time);
                cam.orbit_base_pitch = forward_pitch + cam.orbit_base_offset;
                cam.orbit_base_held = std::fabs(cam.orbit_base_offset) >= k_orbit_base_settled;
            }
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
        // under the crosshair - the "magnet" on hover, the shake on a swing, the orbit jitter. The toe-in
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
        // circles you. The free-look orbit then rotates further on top of this resting angle, and its held base
        // is turned the same way.
        const float follow_yaw = cfg.follow_yaw.load(std::memory_order_relaxed);
        const float follow_pitch = cfg.follow_pitch.load(std::memory_order_relaxed);
        const bool follow_turned =
            follow_yaw < -0.05f || follow_yaw > 0.05f || follow_pitch < -0.05f || follow_pitch > 0.05f;
        auto turn_by_follow_angle = [follow_yaw, follow_pitch](Vector3 &off, Vector3 &look) -> void
        {
            const float yaw = DMK::math::degrees_to_radians(follow_yaw);
            const float cy = std::cos(yaw);
            const float sy = std::sin(yaw);
            auto yaw_about_z = [cy, sy](const Vector3 &v)
            { return Vector3{v.x * cy - v.y * sy, v.x * sy + v.y * cy, v.z}; };
            off = yaw_about_z(off);
            look = yaw_about_z(look);

            const float pitch = DMK::math::degrees_to_radians(follow_pitch);
            if (pitch < -1e-5f || pitch > 1e-5f)
            {
                const float azimuth = std::atan2(off.y, off.x);
                const Vector3 axis{std::sin(azimuth), -std::cos(azimuth), 0.0f};
                const float cp = std::cos(pitch);
                const float sp = std::sin(pitch);
                auto rotate_about_axis = [&axis, cp, sp](const Vector3 &v)
                {
                    const Vector3 cross = axis.cross(v);
                    const float dot = axis.x * v.x + axis.y * v.y + axis.z * v.z;
                    return v * cp + cross * sp + axis * (dot * (1.0f - cp));
                };
                off = rotate_about_axis(off);
                look = rotate_about_axis(look);
            }
            if (look.magnitude_squared() > 1e-6f)
            {
                look = look.normalized();
            }
        };
        if (follow_turned)
        {
            Vector3 off = camera_position - pivot;
            turn_by_follow_angle(off, look_forward);
            camera_position = pivot + off;
            rebuild_basis = true;
        }

        // Aim hand-off. When a situation in OrbitExcludeState (aiming, a distraction throw, a drawn weapon, a menu)
        // turns free-look off while the camera is turned away from the look, the camera swings back behind the old
        // look, and what starts there (a shot, a throw) starts where the character faced instead of where the
        // camera points. So the look and the body are turned to the camera's direction (the follow-up below the orbit
        // rig), and the orbit rig holds the camera where it was until the look shows it. The direction comes from the
        // previous frame, the one on screen, measured against that frame's own plain rig.
        if (cam.aim_handoff_requested && !orbit_held && !cam.aim_handoff_active && cam.rig_last_orbited &&
            cam.orbit_suspended.load(std::memory_order_relaxed) &&
            (std::fabs(cam.rig_last_turn) > k_aim_handoff_min_angle ||
             std::fabs(cam.rig_last_rise) > k_aim_handoff_min_angle))
        {
            cam.aim_handoff_active = true;
            cam.aim_handoff_azimuth = cam.rig_last_azimuth;
            cam.aim_handoff_elevation = cam.rig_last_elevation;
            cam.aim_handoff_yaw = cam.rig_last_yaw;
            cam.aim_handoff_pitch = cam.rig_last_pitch;
            cam.aim_handoff_seconds = 0.0f;
            cam.aim_handoff_written = false;
            cam.aim_handoff_player = c_player;
            // The plain rig takes over at once; the hold keeps the camera in place across the switch.
            cam.orbit_base_held = false;
            (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                     "Orbit: free-look turned off by a situation; the look takes the camera "
                                     "direction, turning {:.1f} deg, pitch {:.1f} to {:.1f} deg",
                                     DMK::math::radians_to_degrees(cam.rig_last_turn),
                                     DMK::math::radians_to_degrees(forward_pitch),
                                     DMK::math::radians_to_degrees(cam.rig_last_pitch));
        }
        const Vector3 plain_offset = camera_position - pivot;

        // Free-look orbit: rigidly rotate the WHOLE non-orbit rig (the camera's offset from the pivot
        // AND the converged look direction) around the pivot - yaw about world up, then pitch about the
        // heading's horizontal right axis. Because the offset (shoulder + height + distance) and the
        // aim-convergence toe-in rotate TOGETHER, the over-the-shoulder framing and the crosshair keep
        // their screen positions while the camera circles the player, and there is no far focus anchor
        // that blows up near the front. Yaw is unbounded so full spins work; the target elevation is
        // clamped away from the pole and the same clamped delta rotates the look so the two agree. At
        // zero angle the rotation is identity (continuous engage); looking at the ground still circles
        // the player (the offset's elevation is preserved) and mouse up/down raises/lowers the camera.
        const bool orbit_rig = orbiting || cam.orbit_base_held || cam.aim_handoff_active;
        if (orbit_rig)
        {
            // The base the orbit turns: the live follow pose (the player's look pitch included), or the held base. The
            // held base is the follow rig built the same way as above for orbit_base_pitch, with this frame's heading,
            // distance, shoulder, focus and follow angle, so a zoom or a preset blend still reshapes it. The height
            // stays in the pivot, and the look converges on the held aim line so the crosshair stays compensated.
            Vector3 offset0 = camera_position - pivot;
            Vector3 base_look = look_forward;
            if (cam.orbit_base_held)
            {
                Vector3 heading{forward.x, forward.y, 0.0f};
                if (heading.magnitude_squared() > 1e-6f)
                {
                    heading = heading.normalized();
                }
                else
                {
                    heading = world_up.cross(right).normalized(); // straight down/up: recover heading from right
                }
                const Vector3 held_fwd =
                    heading * std::cos(cam.orbit_base_pitch) + world_up * std::sin(cam.orbit_base_pitch);
                const Vector3 held_right = heading.cross(world_up).normalized();
                offset0 = held_fwd * (-distance) + held_right * cfg.offset_right.load(std::memory_order_relaxed);
                base_look = held_fwd;
                if (have_focus)
                {
                    const Vector3 to_focus = (anchor_base + held_fwd * focus_distance) - (pivot + offset0);
                    if (to_focus.magnitude() > 1e-4f)
                    {
                        base_look = to_focus.normalized();
                    }
                }
                if (follow_turned)
                {
                    turn_by_follow_angle(offset0, base_look);
                }
            }

            float radius = offset0.magnitude();
            if (radius < 1e-3f)
            {
                radius = distance; // degenerate (camera at the pivot): nothing to rotate about
            }
            const float base_elevation = std::asin(std::clamp(offset0.z / radius, -1.0f, 1.0f));
            // The elevation stays off the overhead pole; a held base already past that limit (a steep look) is kept
            // but cannot go further.
            const float elevation_min = std::min(-1.45f, base_elevation);
            const float elevation_max = std::max(1.45f, base_elevation);
            float elevation = std::clamp(base_elevation + DMK::math::degrees_to_radians(render_pitch_deg),
                                         elevation_min, elevation_max);
            // The pitch range for the next input, from level: OrbitPitchMin / OrbitPitchMax less the held base's
            // elevation, widened to include the base itself (reachable even when it starts outside the range) and
            // kept within the elevation the rig can show.
            if (orbit_held && !cam.aim_handoff_active)
            {
                const float base_deg = DMK::math::radians_to_degrees(base_elevation);
                const float lo =
                    std::max(std::min(cfg.orbit_pitch_min.load(std::memory_order_relaxed) - base_deg, 0.0f),
                             DMK::math::radians_to_degrees(elevation_min) - base_deg);
                const float hi =
                    std::min(std::max(cfg.orbit_pitch_max.load(std::memory_order_relaxed) - base_deg, 0.0f),
                             DMK::math::radians_to_degrees(elevation_max) - base_deg);
                cam.orbit_pitch_lo.store(lo, std::memory_order_relaxed);
                cam.orbit_pitch_hi.store(hi, std::memory_order_relaxed);
            }
            float yaw_delta = DMK::math::degrees_to_radians(render_yaw_deg);
            if (cam.aim_handoff_active)
            {
                // Aim hand-off: keep the camera in the direction it had when the situation took over, whatever the look
                // and the basis do meanwhile, and leave the matching orbit angles behind so the ease back, or free-look
                // turned back on before the look has turned (a paused menu closed), resumes here.
                yaw_delta = std::remainder(cam.aim_handoff_azimuth - std::atan2(offset0.y, offset0.x), DirectX::XM_2PI);
                elevation = std::clamp(cam.aim_handoff_elevation, elevation_min, elevation_max);
                const float held_yaw_deg = DMK::math::radians_to_degrees(yaw_delta);
                const float held_pitch_deg = DMK::math::radians_to_degrees(elevation - base_elevation);
                cam.orbit_yaw.store(held_yaw_deg, std::memory_order_relaxed);
                cam.orbit_pitch.store(held_pitch_deg, std::memory_order_relaxed);
                cam.orbit_yaw_render = held_yaw_deg;
                cam.orbit_pitch_render = held_pitch_deg;
                cam.orbit_render_valid = true;
            }
            const float pitch_delta = elevation - base_elevation;

            // Yaw about world up (Z): applied to both the offset and the look so they swing together.
            const float cy = std::cos(yaw_delta);
            const float sy = std::sin(yaw_delta);
            auto yaw_about_z = [cy, sy](const Vector3 &v)
            { return Vector3{v.x * cy - v.y * sy, v.x * sy + v.y * cy, v.z}; };
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
                auto rotate_about_axis = [&pitch_axis, cp, sp](const Vector3 &v)
                {
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

        // While the orbit rig shapes the camera, its direction from the pivot and the look that puts this frame's
        // plain rig there, for the next frame's aim hand-off. Measured within one frame, so a preset blend or the
        // basis moving between frames is not taken for orbit. The plain rig turns rigidly with the look heading, and
        // its azimuth off that heading is atan2(-distance * cos(pitch), shoulder), so the heading turns by the
        // camera's azimuth off the plain rig, corrected for the pitch change. Its elevation follows the look pitch by
        // sin(elevation) = -distance * sin(pitch) / |offset| (a look that points down puts the camera above), so the
        // pitch moves by the difference of that inverse.
        cam.rig_last_orbited = false;
        if (orbit_rig)
        {
            const Vector3 rig_offset = camera_position - pivot;
            const float rig_reach = rig_offset.magnitude();
            const float plain_reach = plain_offset.magnitude();
            if (rig_reach > 1e-3f && plain_reach > 1e-3f && distance > 1e-3f)
            {
                const float plain_elevation = std::asin(std::clamp(plain_offset.z / plain_reach, -1.0f, 1.0f));
                cam.rig_last_azimuth = std::atan2(rig_offset.y, rig_offset.x);
                cam.rig_last_elevation = std::asin(std::clamp(rig_offset.z / rig_reach, -1.0f, 1.0f));
                cam.rig_last_turn =
                    std::remainder(cam.rig_last_azimuth - std::atan2(plain_offset.y, plain_offset.x), DirectX::XM_2PI);
                cam.rig_last_rise = cam.rig_last_elevation - plain_elevation;
                const float reach = plain_reach / distance;
                auto pitch_for = [reach](float elevation)
                { return std::asin(std::clamp(-reach * std::sin(elevation), -1.0f, 1.0f)); };
                const float shoulder = cfg.offset_right.load(std::memory_order_relaxed);
                auto side_at = [distance, shoulder](float look_pitch)
                { return std::atan2(-distance * std::cos(look_pitch), shoulder); };
                cam.rig_last_pitch =
                    std::clamp(forward_pitch + pitch_for(cam.rig_last_elevation) - pitch_for(plain_elevation),
                               -k_aim_handoff_max_pitch, k_aim_handoff_max_pitch);
                cam.rig_last_yaw = std::remainder(forward_yaw + cam.rig_last_turn + side_at(forward_pitch) -
                                                      side_at(cam.rig_last_pitch),
                                                  DirectX::XM_2PI);
                cam.rig_last_orbited = true;
            }
        }

        // Aim hand-off follow-up. While aiming the game re-derives the look heading from the body every frame (the body
        // follows the look), so the look alone cannot be turned: the body is turned with it, as camera-relative
        // movement does, and both are written every frame until the look shows the new heading. They are written at
        // least once, so a hand-off that only raises or lowers the camera still sets the pitch, which sticks, and it
        // can end only on a later frame than its first write: on the same frame's second build the eye pose does not
        // show the write yet. Until then the next frame's basis snaps to the look, so the rig lands on the new
        // direction at once instead of easing across the turn under AimBasisSmoothing.
        if (cam.aim_handoff_active)
        {
            const bool lost = cam.aim_handoff_cancel || c_player == 0 || c_player != cam.aim_handoff_player;
            const bool jumped = body_valid && cam.turn_track_step > k_aim_handoff_jump;
            const Vector3 look_now = (look_valid ? look_rotation : eye_rotation).rotate(Vector3{0.0f, 1.0f, 0.0f});
            const float yaw_gap =
                std::remainder(std::atan2(-look_now.x, look_now.y) - cam.aim_handoff_yaw, DirectX::XM_2PI);
            if (lost)
            {
                cam.aim_handoff_active = false;
                cam.aim_handoff_cancel = false;
                (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                         "Orbit: the hand-off ended without turning the look (the game sets the "
                                         "facing here, or the player changed)");
            }
            else if (cam.aim_handoff_written && !step.repeat_build && std::fabs(yaw_gap) < k_aim_handoff_min_angle)
            {
                cam.aim_handoff_active = false;
                (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                         "Orbit: the look took the camera direction after {:.2f} s",
                                         cam.aim_handoff_seconds);
            }
            else if (jumped)
            {
                cam.aim_handoff_active = false;
                (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                         "Orbit: the hand-off ended without turning the look (the player was moved)");
            }
            else if (cam.aim_handoff_seconds >= k_aim_handoff_timeout)
            {
                cam.aim_handoff_active = false;
                (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                         "Orbit: the look did not take the camera direction ({:.1f} deg left after "
                                         "{:.2f} s); the camera eases back",
                                         DMK::math::radians_to_degrees(yaw_gap), cam.aim_handoff_seconds);
            }
            else if (!apply_orbit_aim_control(1.0f, cam.aim_handoff_pitch, true, cam.aim_handoff_yaw))
            {
                cam.aim_handoff_active = false;
                (void)DMK::log().try_log(DMK::LogLevel::Debug, "Orbit: the look write failed; the camera eases back");
            }
            else
            {
                apply_orbit_body_turn(cam.aim_handoff_yaw);
                cam.aim_handoff_written = true;
                cam.basis_quat_valid = false;
                // The pause menu stops the player update, so the look shows the new heading only once play resumes;
                // the wait does not count paused time.
                if (!step.paused)
                {
                    cam.aim_handoff_seconds += delta_time;
                }
            }
        }

        // Camera-relative movement (toggle orbit): on the idle -> moving edge CAPTURE the camera heading, then
        // HOLD it while moving. The body turn (apply_orbit_body_turn) pins the body to that heading, which is
        // what makes locomotion camera-relative in EVERY direction (KCD2 moves relative to the body rotation):
        // W runs away from the camera, A/D strafe to the sides, S backs toward it. The look-yaw write
        // (apply_orbit_aim_control) faces the head/aim the same way and feeds the camera-position derivation.
        // Both are re-applied every frame because the engine reverts/consumes a single write - like the pitch
        // leveling. Released when movement stops.
        bool do_align = false;
        // Camera-relative movement, keyed on the device-agnostic action-input intent
        // (player_onaction_move_magnitude, from the action dispatcher). The input is nonzero the instant a movement
        // key is pressed - BEFORE the body accelerates - so the heading captures immediately and the
        // character turns to the camera direction without first moving the old way; and it stays nonzero while
        // a key is held against a wall, so the heading is not falsely released on a collision arrest. The
        // body-position speed is deliberately NOT used (it lags the intent and reads zero when arrested), so
        // the feature requires the action hook - if it did not resolve, camera-relative movement stays off
        // (free-look still works).
        if (orbit_held && body_valid && player_onaction_available())
        {
            bool moving = cam.orbit_moving;
            const float move_magnitude = player_onaction_move_magnitude();
            // Re-arm guard: a genuine release (magnitude below the stop threshold) must be observed since orbit
            // engaged before a move-start is honored. A stranded latch (a held-move release swallowed on a
            // combat action-map swap, see player_onaction_reset) reads > 0 with the keys up; without this guard
            // it re-trips orbit_moving the instant orbit restores and drives the body-turn with no input (the
            // post-combat self-rotation). Arming on a sub-stop reading means a fresh, observed press engages it.
            // A key held while orbit restores (running on right after leaving crouch) is no stranded latch: the
            // game's own move input shows it too, so that also arms the move. A stranded latch has the keys up and
            // the game's input at 0, so the guard still holds it. A move armed that way also ends when the game's
            // input goes idle, as another key's slot can still be stranded and hold the latch up.
            static bool s_stale_suppress_logged = false;
            const std::optional<float> game_move = player_move_input(c_player);
            if (move_magnitude < k_orbit_move_input_stop)
            {
                cam.orbit_move_armed = true;
                cam.orbit_move_armed_by_game = false;
                s_stale_suppress_logged = false;
            }
            else if (!cam.orbit_move_armed && game_move && *game_move > MOVE_INPUT_START)
            {
                cam.orbit_move_armed = true;
                cam.orbit_move_armed_by_game = true;
                cam.orbit_move_game_idle = 0.0f;
                s_stale_suppress_logged = false;
                (void)DMK::log().try_log(DMK::LogLevel::Trace,
                                         "Orbit: move held as orbit engaged (magnitude {:.2f}, the game's own move "
                                         "input agrees); armed",
                                         move_magnitude);
            }
            // A hand-off still turning the look and the body (free-look back on before it ends) is not interrupted.
            if (!moving && !cam.aim_handoff_active && move_magnitude > MOVE_INPUT_START)
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
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Trace,
                        "Orbit: move-orbit START (body-turn engaging) - move_magnitude={:.2f}, continuous_align={}, "
                        "state_mask=0x{:X}",
                        move_magnitude, cfg.orbit_continuous_align.load(std::memory_order_relaxed),
                        game_state_mask().load(std::memory_order_relaxed));
                }
                else if (!s_stale_suppress_logged)
                {
                    // Unarmed (no release seen since orbit engaged) yet reading as movement: a stranded latch.
                    // Suppress the body-turn re-trip and log it once - the diagnostic for the post-combat case.
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Trace,
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
                (void)DMK::log().try_log(DMK::LogLevel::Trace,
                                         "Orbit: move-orbit STOP (body-turn releasing) - move_magnitude={:.2f}",
                                         move_magnitude);
            }
            else if (moving && cam.orbit_move_armed_by_game)
            {
                // Armed by the game's own input: it ends once that input has stayed idle, even while another latch
                // slot is stranded. Disarmed rather than reset, so a stranded slot stays held off by the guard and a
                // key still held re-arms through the game's input on the next frame.
                cam.orbit_move_game_idle =
                    (game_move && *game_move < k_orbit_move_input_stop) ? cam.orbit_move_game_idle + delta_time : 0.0f;
                if (cam.orbit_move_game_idle >= k_orbit_move_game_stop_delay)
                {
                    moving = false;
                    cam.orbit_move_armed = false;
                    cam.orbit_move_armed_by_game = false;
                    cam.orbit_yaw.store(orbit_yaw_deg, std::memory_order_relaxed);
                    (void)DMK::log().try_log(DMK::LogLevel::Trace,
                                             "Orbit: move-orbit STOP (the game's own move input is idle; magnitude "
                                             "{:.2f} is a stranded latch)",
                                             move_magnitude);
                }
            }
            cam.orbit_moving = moving;
        }
        else
        {
            cam.orbit_moving = false;
            cam.orbit_move_armed = false; // re-armed by a release, or by a held move the game's input shows
            cam.orbit_move_armed_by_game = false;
        }

        // Capture the heading on move-start. Use the camera's POSITIONAL world yaw - the eye-look yaw plus
        // the orbit applied this frame - NOT atan2(look_forward). look_forward is the CONVERGED look: with
        // aim-convergence on, the over-the-shoulder camera toes its look INWARD toward the aim point by a
        // shoulder-dependent angle. Capturing that toed-in yaw would rotate the rig by the toe-in on the
        // move transition, shifting the camera sideways toward the shoulder every time you press W. The
        // positional yaw is exactly what the world-stable orbit derivation above renders, so the camera
        // stays put as the body turns. It is also the more intuitive move direction (straight away from the
        // camera rather than toed-in toward the crosshair).
        if (do_align)
        {
            cam.orbit_target_yaw = forward_yaw + DMK::math::degrees_to_radians(cam.orbit_yaw_render);
            cam.orbit_target_valid = true;
            // Snapshot the orbit input so further orbiting while moving is measured from here. We do NOT
            // reset the orbit to 0: the world-stable derivation above holds the camera in place while the
            // body turns to this heading, so there is no snap-behind pop.
            cam.orbit_yaw_at_capture_deg = cam.orbit_yaw.load(std::memory_order_relaxed);
            // The look is turned to this heading below, so the next frame's basis snaps to it instead of easing
            // across the turn (see orbit_move_pressed).
            cam.basis_quat_valid = false;
        }
        if (!cam.orbit_moving)
        {
            cam.orbit_target_valid = false;
        }
        // The heading a move press captures, for orbit_move_pressed: the camera's yaw on screen (with the
        // orbit as rendered, so a smoothed angle still catching up does not snap), published only while a press
        // starts a camera-relative move with the body turn, and stamped so a stale one is never used.
        if (orbit_held && body_valid && player_onaction_available() && !cam.orbit_moving && cam.orbit_move_armed &&
            !cam.aim_handoff_active && cfg.orbit_body_turn.load(std::memory_order_relaxed))
        {
            s_move_press_heading.store(forward_yaw + DMK::math::degrees_to_radians(cam.orbit_yaw_render),
                                       std::memory_order_relaxed);
            s_move_press_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
            s_move_press_body_yaw.store(body_yaw, std::memory_order_relaxed);
            s_move_press_stamp_ms.store(GetTickCount64(), std::memory_order_release);
        }
        else
        {
            s_move_press_stamp_ms.store(0, std::memory_order_release);
        }

        const float pitch_ease = (orbit_held && cfg.orbit_level_aim.load(std::memory_order_relaxed))
                                     ? (1.0f - std::exp(-k_orbit_aim_level_speed * delta_time))
                                     : 0.0f;
        const bool hold_yaw = orbit_held && cam.orbit_moving && cam.orbit_target_valid;
        // Heading fed to the body + movement while moving. CONTINUOUS-ALIGN (GTA style): follow the CURRENT
        // camera world yaw every frame (captured heading + the orbit added since), so orbiting STEERS the run
        // and the rig stays behind the character. Otherwise HOLD the heading captured on move-start (orbit
        // looks around freely while the character keeps its line). The world-stable camera derivation above is
        // the same either way - only this target differs - so in continuous mode the derived orbit angle
        // cancels to ~0 (camera behind the character) while in hold mode it carries the orbit-around offset.
        float body_target_yaw = cam.orbit_target_yaw;
        if (hold_yaw && cfg.orbit_continuous_align.load(std::memory_order_relaxed))
        {
            // Reuse the smoothed steer angle computed in the move-orbit derivation above (when OrbitSmoothing
            // is on) so the body/look heading and the rig agree - the camera follows the smoothed steering
            // and stays behind. With smoothing off this is the raw orbit-since-capture. The smoothed value is
            // only current once the move-orbit block above has run for it (cam.orbit_steer_valid); on the
            // idle->moving capture frame that block did NOT run (move_orbit read the pre-update orbit_moving,
            // which was still false), so fall back to the raw orbit-since-capture - ~0 on the capture frame
            // because orbit_yaw_at_capture_deg was just snapshotted - instead of a stale smoothed value from a
            // prior move. orbit_smoothing was read for the orbit-angle low-pass above and is constant across
            // the frame, so it is reused here.
            const float user_orbit_since_deg =
                (orbit_smoothing > 1e-4f && cam.orbit_steer_valid)
                    ? cam.orbit_steer_smooth
                    : (cam.orbit_yaw.load(std::memory_order_relaxed) - cam.orbit_yaw_at_capture_deg);
            body_target_yaw = cam.orbit_target_yaw + DMK::math::degrees_to_radians(user_orbit_since_deg);
        }
        // The hand-off writes the look itself while it runs.
        if (orbit_held && !cam.aim_handoff_active && (pitch_ease > 0.0f || hold_yaw))
        {
            (void)apply_orbit_aim_control(pitch_ease, 0.0f, hold_yaw, body_target_yaw);
        }
        // Pin the BODY to the camera heading while moving so the character runs camera-relative in EVERY
        // direction. KCD2 moves the player relative to the ENTITY (body) rotation, NOT the look, so this body
        // override - not the look-yaw write above - is what redirects locomotion: with the body faced at the
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
                (void)DMK::log().log_noexcept(DMK::LogLevel::Trace,
                                              "Camera: orbit body-turn ENGAGED (moving; heading locked to camera)");
            }
            else
            {
                (void)DMK::log().log_noexcept(DMK::LogLevel::Trace, "Camera: orbit body-turn released");
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

                // Static-world throttle: camera collision only ever queries STATIC / terrain geometry (movable
                // rigids, the player and NPCs are all excluded), which cannot move while the camera holds still and
                // changes only SLOWLY as the camera moves. So the full result (walk, sphere, render occlusion,
                // lateral probe) is recomputed only once the pivot or the desired camera position has moved more
                // than k_collision_recompute_dist; in between it is reused and only the easing runs - which
                // collapses the cost to ~zero while standing AND skips most frames while walking (the dominant cost
                // in dense scenes such as a doorway). The threshold is a MOVEMENT distance, so fast motion still
                // recomputes every frame (responsive) while slow motion reuses for a few frames; the reused target
                // is at most one threshold of camera travel stale, which the easing and the collision standoff
                // (CollisionSkin) absorb, so the camera never visibly clips.
                constexpr float k_collision_recompute_dist = 0.06f;
                const float recompute_d2 = k_collision_recompute_dist * k_collision_recompute_dist;
                const Vector3 throttle_cam = camera_position; // desired (pre-collision); the block overwrites it
                static bool s_collision_throttle_valid = false;
                static Vector3 s_throttle_pivot{};
                static Vector3 s_throttle_cam{};
                static float s_cached_allowed = 0.0f;
                const bool recompute = !s_collision_throttle_valid ||
                                       (pivot - s_throttle_pivot).magnitude_squared() > recompute_d2 ||
                                       (throttle_cam - s_throttle_cam).magnitude_squared() > recompute_d2;
                // CollisionSkin is the gap the camera keeps from whatever surface the probes found. A pull-in may lag
                // its target by the skin less the lens margin.
                const float skin = cfg.collision_skin.load(std::memory_order_relaxed);
                const float pull_slack = std::max(0.0f, skin - k_lens_near_margin);
                if (!recompute && cam.collision_valid)
                {
                    // Reuse last frame's allowed distance (the shared easing), then skip the whole walk / sphere /
                    // render-occlusion / lateral-probe computation below.
                    const float return_speed = cfg.collision_return_speed.load(std::memory_order_relaxed);
                    ease_collision_toward(cam, s_cached_allowed, delta_time, return_speed, pull_slack);
                    camera_position = pivot + ray_dir * cam.collision_distance;
                    // The hold drains here too; when it runs out the next frame recomputes, so the release starts.
                    if (cam.collision_hold_timer > 0.0f)
                    {
                        cam.collision_hold_timer -= delta_time;
                        if (cam.collision_hold_timer <= 0.0f)
                        {
                            s_collision_throttle_valid = false;
                        }
                    }
                    goto collision_done;
                }

                // Prefer the swept SPHERE (PrimitiveWorldIntersection): its contact distance is continuous
                // as the sweep grazes edges, so the camera does not pump in dense geometry the way a single
                // thin ray does (the root cause of the orbit position-jump). Whichever probe finds the surface,
                // the camera keeps the same CollisionSkin from it. Fall back to the ray fan alone when the sphere
                // is disabled, unavailable, or faults, so collision always works and never regresses.
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
                // UseCoverageCollision is the master switch for the coverage-based heuristics (the coverage gate
                // and the lateral probe). When OFF the camera collides plainly on the nearest solid: the coverage
                // threshold is forced to 0 (no walk) and the lateral probe is skipped. Render occlusion is
                // INDEPENDENT (its own UseRenderOcclusion toggle below).
                const bool use_coverage = cfg.use_coverage_collision.load(std::memory_order_relaxed);
                const float cov_thresh =
                    use_coverage ? cfg.collision_coverage_threshold.load(std::memory_order_relaxed) : 0.0f;
                // Find the nearest occluder that actually HIDES the body. With the coverage gate OFF (cov_thresh
                // <= 0) this is just the nearest solid world surface. With it ON, WALK the arm: step through
                // readable THIN props - each measured by ITS OWN visible mesh (via the brush the ray actually
                // hit, resolved through the collider's foreign data), so a foreground basket / pole the body is
                // plainly visible past is skipped - and stop at the first thing that genuinely covers the body: a
                // solid we cannot rasterize (a building's compound mesh, a pure-physics proxy), terrain, or a
                // readable mesh hiding >= CoverageThreshold of the body. If only thin props lie between the camera
                // and the body with open space behind them, nothing covers and the camera stays out (no jolt).
                // This judges each occluder on its own merit, where the prior single-shot gate saw only the
                // nearest brush and let the body sit buried behind everything past it.
                std::optional<RayHit> fan;
                // Measured coverage (0..1) of the occluder the walk decides to block on; -9 = not measured (coverage
                // gate off, terrain, or an unmeasurable solid). Logged so an oscillating thin prop whose coverage
                // straddles CoverageThreshold frame to frame is visible without live debugging.
                float blocked_cov = -9.0f;
                if (cov_thresh <= 0.0f)
                {
                    fan = ray_fan_sweep(pivot, to_camera, collision_radius, Constants::RWI_OBJTYPES_CAMERA,
                                        Constants::RWI_FLAGS_STOP_AT_SOLID);
                }
                else
                {
                    const Vector3 desired_cam = pivot + to_camera;
                    uintptr_t cov_skip[Constants::COVERAGE_SKIP_MAX];
                    int n_cov_skip = 0;
                    for (int iter = 0; iter < Constants::COVERAGE_SKIP_MAX; ++iter)
                    {
                        const std::optional<RayHit> h =
                            ray_fan_sweep(pivot, to_camera, collision_radius, Constants::RWI_OBJTYPES_CAMERA,
                                          Constants::RWI_FLAGS_STOP_AT_SOLID, cov_skip, n_cov_skip);
                        if (!h.has_value() || h->m_distance >= desired_distance)
                        {
                            break; // open past here -> nothing between camera and body hides it
                        }
                        if (h->m_terrain != 0)
                        {
                            fan = h; // terrain always covers (never see under the world)
                            break;
                        }
                        // How much of the character does this collider hide? Cheap footprint pre-check, then
                        // visible-mesh raster, then a physics-ray fallback - cached per collider so a solid the
                        // camera slides along is not re-rasterized every frame. See measure_collider_coverage.
                        const float cov = measure_collider_coverage(h->m_collider, h->m_point, pivot, desired_cam,
                                                                    to_camera, cov_thresh);
                        if (cov < 0.0f || cov >= cov_thresh)
                        {
                            // Unmeasurable solid (building / pure-physics) OR a readable mesh that hides enough of
                            // the body -> this is the collision.
                            blocked_cov = cov;
                            fan = h;
                            break;
                        }
                        if (h->m_collider == 0)
                        {
                            fan = h; // cannot skip an unknown collider -> stop here (safe: collide)
                            break;
                        }
                        cov_skip[n_cov_skip++] = h->m_collider; // a thin prop the body is visible past -> look behind
                    }
                }
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
                    const int n_skip = resolve_player_physics_skip(pivot, to_camera, collision_radius,
                                                                   0x11F /*ent_all*/, Constants::RWI_OBJTYPES_CAMERA,
                                                                   Constants::RWI_FLAGS_STOP_AT_SOLID, skip_ents, 4);
                    const std::optional<RayHit> sphere = sphere_world_sweep(
                        pivot, collision_radius, to_camera, Constants::RWI_OBJTYPES_CAMERA, skip_ents, n_skip);
                    // The FAN (0x101 = static|terrain) is the AUTHORITY for collision: a plain objtypes arg that
                    // provably excludes ALL actors (player body/gear, NPCs = ent_living/independent/rigid). The PWI
                    // sphere only SMOOTHS the fan's world hit - it queries ent_all (the struct entTypes is dead in
                    // this fork, so it cannot be type-filtered and returns no collider to test), so it is trusted
                    // ONLY when it AGREES with the fan's world surface (within the radius inset). Crucially, when
                    // the fan finds NO world (open space), there is NO collision - any actor the sphere saw (an
                    // NPC at the camera, your own shield) is ignored = transparent, like non-physicalized grass.
                    // This drops actor collisions UNIFORMLY with no skip-list / probe and no per-entity guessing.
                    // The sphere's surface (its contact plus the radius) must also lie nearer than the fan's hit: a
                    // farther one would put the camera past a wall the fan's rays met.
                    if (fan.has_value() && sphere.has_value() &&
                        sphere->m_distance >= fan->m_distance - collision_radius - 0.10f &&
                        sphere->m_distance <= fan->m_distance + 0.05f &&
                        sphere->m_distance + collision_radius < fan->m_distance)
                    {
                        hit = sphere; // sphere agrees with the fan's world surface -> use it (continuous, no pump)
                        from_sphere = true;
                    }
                }

                // After a recompute met a surface the camera does not ease back out at all for this long (seconds,
                // drained on every frame): a probe that grazes an edge and misses it the next frame, or an arm swung
                // fast through clutter, keeps the camera in instead of letting it ease out and snap back in.
                constexpr float k_collision_hold_seconds = 0.3f;

                // Where the nearest solid surface is along the arm: the fan's ray hit, or the sphere's contact plus its
                // radius (the sphere's centre stops CollisionRadius short of the surface). The coverage walk above
                // already decided this hit COVERS the body (or the gate is off and the nearest solid blocks). The
                // camera keeps the same CollisionSkin from it whichever probe found it, so a frame that switches
                // between the fan and the sphere does not move the camera.
                float surface = desired_distance;
                if (hit.has_value())
                {
                    surface = hit->m_distance + (from_sphere ? collision_radius : 0.0f);
                }
                float blocking_distance = desired_distance;
                bool blocked = false;
                if (surface < desired_distance)
                {
                    blocking_distance = std::max(0.0f, surface - skin);
                    blocked = true;
                }

                // Trace the PHYSICS collision hit (the solid-world fan/sphere result) so it can be compared
                // against RenderOcclusion HIT: physics fires on walls / terrain / solid props (objtypes
                // ent_static|ent_terrain), render occlusion on non-physical cloth. Where physics is silent but
                // render fires (or vice versa) is the proof they are complementary, not redundant. Rate-limited
                // to a meaningful change (block-state toggle or distance move > 0.1m) so it never spams a frame.
                {
                    static bool s_phys_blocked = false;
                    static float s_phys_dist = -1.0f;
                    const float dd = blocked ? (surface - s_phys_dist) : 0.0f;
                    const bool moved = (dd > 0.1f) || (dd < -0.1f);
                    // The trace below identifies the hit object via a render-octree query (render_hit_info); gate it
                    // on trace logging being ENABLED so that diagnostic octree + per-node vertex scan never runs on
                    // a normal-play frame - it is purely for the PhysicsCollision HIT log. (A foreign-null hit, the
                    // common case once the coverage walk skips thin scenery, takes render_hit_info's expensive
                    // octree path, which was running every frame the camera moved while blocked.)
                    const bool trace_on = DMK::log().is_enabled(DMK::LogLevel::Trace);
                    if (trace_on && blocked && (blocked != s_phys_blocked || moved))
                    {
                        // The FAN is the collision AUTHORITY: it carries the real collider + surface normal. The
                        // sphere only SMOOTHS the distance and reports a synthetic normal / null collider, so identify
                        // the object the camera blocked on from the fan (point + collider), not the sphere's `hit`.
                        // node = the collider's foreign render-node link (0 for a merged / proxy collider, which
                        // render_hit_info then resolves by querying the render octree at the hit point).
                        const RayHit &id_hit = fan.has_value() ? *fan : *hit;
                        const Vector3 &hp = id_hit.m_point;
                        const Vector3 &hn = id_hit.m_normal;
                        const int hit_terrain = id_hit.m_terrain;
                        const uintptr_t collider = id_hit.m_collider;
                        void *node = reinterpret_cast<void *>(static_brush_render_node(collider));
                        char obj_name[160] = {};
                        float obj_ext[3] = {};
                        int obj_kind = 0;
                        (void)render_hit_info(hp, node, obj_name, static_cast<int>(sizeof(obj_name)), obj_ext,
                                              &obj_kind);
                        static const char *const k_obj_kinds[] = {"none", "foreign", "prop", "solid", "hlod"};
                        const char *kind_str = (obj_kind >= 0 && obj_kind <= 4) ? k_obj_kinds[obj_kind] : "?";
                        (void)DMK::log().try_log(
                            DMK::LogLevel::Trace,
                            "PhysicsCollision HIT: src={} bTerrain={} dist={} of {} cov={} kind={} obj=\"{}\" "
                            "ext=({}, {}, {}) collider={:#x} node={:#x} point=({}, {}, {}) normal=({}, {}, {}) "
                            "cam=({}, {}, {}) pivot=({}, {}, {})",
                            from_sphere ? "sphere" : "fan", hit_terrain, surface, desired_distance, blocked_cov,
                            kind_str, obj_name, obj_ext[0], obj_ext[1], obj_ext[2], collider,
                            reinterpret_cast<uintptr_t>(node), hp.x, hp.y, hp.z, hn.x, hn.y, hn.z, camera_position.x,
                            camera_position.y, camera_position.z, pivot.x, pivot.y, pivot.z);
                    }
                    s_phys_blocked = blocked;
                    if (blocked)
                    {
                        s_phys_dist = surface;
                    }
                }

                // Render-only overhead roofs (tent / awning canopy cloth) carry no ray-collidable physics, so the
                // fan and sphere glide through them and the cloth buries the camera on a look-down. Query the
                // render octree along the same arm and clamp below an overhead brush. The clamp keeps its own standoff,
                // the sightline tube radius (RENDER_OCCLUSION_COLUMN_RADIUS), so no skin is subtracted. Gated by
                // UseRenderOcclusion alone - it is INDEPENDENT of UseCoverageCollision (it handles non-physical cloth,
                // not a coverage heuristic). A thin overhead beam is rejected inside render_occlusion_limit by the
                // sightline vertex-count test, not a body-coverage gate (an overhead canopy covers ~0 of the body
                // silhouette).
                if (cfg.use_render_occlusion.load(std::memory_order_relaxed))
                {
                    // Query the render octree only out to where the physics collision already stops the camera: a
                    // roof beyond that point can never be reached, so indoors / near walls (where physics blocks
                    // close) the query box shrinks to the short arm and skips the room's geometry, the bulk of the
                    // render-occlusion cost in dense scenes.
                    const Vector3 occ_arm = ray_dir * blocking_distance;
                    const std::optional<float> roof = render_occlusion_limit(pivot, occ_arm, collision_radius);
                    if (roof.has_value() && roof.value() < blocking_distance)
                    {
                        blocking_distance = std::max(0.0f, roof.value());
                        blocked = true;
                    }
                }

                // The target: the blocking distance, or the desired one when the arm is clear. For
                // k_collision_hold_seconds after a recompute met a surface (drained on every frame, the throttled ones
                // too) the camera does not ease back out at all; a pull-in is never held back. A hold that runs out on
                // this frame releases at once: the throttled frames after it reuse this target and drain nothing.
                float allowed_distance = blocked ? blocking_distance : desired_distance;
                if (blocked)
                {
                    cam.collision_hold_timer = k_collision_hold_seconds;
                }
                else if (cam.collision_valid && cam.collision_hold_timer > 0.0f)
                {
                    cam.collision_hold_timer -= delta_time;
                    if (cam.collision_hold_timer > 0.0f)
                    {
                        allowed_distance = std::min(allowed_distance, cam.collision_distance);
                    }
                }

                // Lateral / frustum clearance. The pivot->camera probe only sees obstacles ALONG the arm, so a
                // wall BESIDE the camera (a corner, a doorway jamb, a narrow gap) is invisible to it and intrudes
                // into the view. Probe the camera's lateral surroundings (the frustum cross-section: +-right /
                // +-up around the look axis) and pull the camera further in along the arm until any CONVERGING
                // side wall is at least CameraProbeSize away. This is geometric penetration safety, NOT occlusion,
                // so it bypasses the coverage gate (a side wall does not hide the character) and queries static /
                // terrain world only (RWI_OBJTYPES_CAMERA never returns the player or NPCs). A wall the arm runs
                // PARALLEL to (a corridor) cannot be escaped by pulling in, so it is left alone rather than yanking
                // the camera to first person. Bounded passes; only does work when something is within reach.
                const float probe = use_coverage ? cfg.camera_probe_size.load(std::memory_order_relaxed) : 0.0f;
                if (probe > 0.0f && allowed_distance > collision_radius)
                {
                    const Vector3 view = ray_dir * -1.0f; // the camera looks back along the arm toward the pivot
                    Vector3 probe_right = view.cross(Vector3{0.0f, 0.0f, 1.0f});
                    const float rl = probe_right.magnitude();
                    if (rl > 1e-4f)
                    {
                        probe_right = probe_right / rl;
                        const Vector3 probe_up = probe_right.cross(view); // orthonormal (right & view perpendicular)
                        const Vector3 lateral[4] = {probe_right, probe_right * -1.0f, probe_up, probe_up * -1.0f};
                        // Floor the pull-in at the closest USEFUL third-person distance (follow_distance_min). A side
                        // wall grazing the frustum is a SOFTER concern than a direct occluder, so never drag the
                        // camera to near first person (losing the whole view + jamming into the character) just to
                        // hold one off - accept a little intrusion instead. Capped to the incoming allowed_distance
                        // so a tight pull from a real along-arm occluder is never pushed back OUT into it.
                        const float lateral_floor =
                            std::min(allowed_distance, cfg.follow_distance_min.load(std::memory_order_relaxed));
                        for (int pass = 0; pass < 3 && allowed_distance > lateral_floor; ++pass)
                        {
                            const Vector3 cam_test = pivot + ray_dir * allowed_distance;
                            float capped = allowed_distance;
                            for (const Vector3 &side : lateral)
                            {
                                const std::optional<RayHit> h =
                                    ray_world_intersection(cam_test, side * probe, Constants::RWI_OBJTYPES_CAMERA,
                                                           Constants::RWI_FLAGS_STOP_AT_SOLID);
                                if (!h.has_value() || h->m_distance >= probe)
                                {
                                    continue; // this side is clear within the probe
                                }
                                // Wall plane: point P, normal N (toward the camera). Lateral clearance along the
                                // arm is clear(t) = dot(pivot - P, N) + t * dot(ray_dir, N). Pulling IN clears the
                                // wall only when the arm runs INTO it (dot(ray_dir, N) < 0); a parallel / diverging
                                // wall is left alone. Solve for the largest t that keeps the camera `probe` off it.
                                const Vector3 &P = h->m_point;
                                const Vector3 &N = h->m_normal;
                                const float a = ray_dir.x * N.x + ray_dir.y * N.y + ray_dir.z * N.z;
                                if (a >= -0.05f)
                                {
                                    continue; // parallel / diverging side wall: pulling in cannot clear it
                                }
                                const float b = (pivot.x - P.x) * N.x + (pivot.y - P.y) * N.y + (pivot.z - P.z) * N.z;
                                const float t = (probe - b) / a;
                                if (t < capped)
                                {
                                    capped = t;
                                }
                            }
                            if (capped >= allowed_distance - 1e-3f)
                            {
                                break; // every side clear -> done
                            }
                            allowed_distance = std::max(lateral_floor, capped);
                            blocked = true;
                            cam.collision_hold_timer = k_collision_hold_seconds; // latch like a normal blocking hit
                        }
                    }
                }

                // Ease toward the allowed distance (fast pull-IN so a wall is never clipped, a critically damped
                // return-OUT), via the shared helper the throttle bypass also uses.
                const float return_speed = cfg.collision_return_speed.load(std::memory_order_relaxed);
                ease_collision_toward(cam, allowed_distance, delta_time, return_speed, pull_slack);

                camera_position = pivot + ray_dir * cam.collision_distance;

                // Cache the freshly computed target so a held-still camera reuses it next frame (throttle above).
                s_collision_throttle_valid = true;
                s_throttle_pivot = pivot;
                s_throttle_cam = throttle_cam;
                s_cached_allowed = allowed_distance;
            }
        collision_done:;
        }
        else
        {
            // No collision in this preset: the camera goes wherever the rig puts it. The collision state is dropped,
            // so turning collision back on starts afresh.
            cam.collision_valid = false;
            cam.collision_speed = 0.0f;
            cam.collision_hold_timer = 0.0f;
        }

        // Too close to the character. With no room behind the player - a low lintel in a doorway (the roof clamp holds
        // the camera under it), a wall right behind - collision pulls the camera onto the pivot, inside the head, and
        // the view shows the head's inside until the player moves. With CloseUpFade the camera stays where the
        // collision put it and the character dithers out instead, as far as CloseUpFadeMinOpacity lets him from
        // HeadClearance of arm and starting k_close_fade_band before it, so the view never jumps; once he is mostly
        // faded his head is hidden like in first person (s_head_close_fade). Otherwise (or without the character fade
        // hook), below HeadClearance the camera eases onto the real eye and the head is hidden by the game's own
        // first-person rig (s_head_fallback, read by the head re-assert and the head-visibility detour), so the view
        // is plain first person until there is room again. The arm is measured after collision, so a close zoom
        // counts too. The margin keeps a doorway from switching the first-person view on and off as the arm hovers
        // around the threshold.
        {
            constexpr float k_head_fallback_margin = 0.15f; // meters of extra room before third person resumes
            constexpr float k_head_fallback_rate = 12.0f;   // ease rate, 1/sec (frame-rate independent below)
            const float clearance = cfg.head_clearance.load(std::memory_order_relaxed);
            // Measured from the head where it really is: the pivot plus the turn hold, which keeps the pivot off the
            // body while it steps through a turn.
            const float arm = (camera_position - (pivot + turn_hold)).magnitude();
            const bool close_fade = cfg.close_up_fade.load(std::memory_order_relaxed) && character_fade_available();
            if (!step.repeat_build)
            {
                const float close_most =
                    1.0f - std::clamp(cfg.close_up_fade_min_opacity.load(std::memory_order_relaxed), 0.0f, 1.0f);
                const float close_amount =
                    (close_fade && clearance > 0.0f)
                        ? std::clamp((clearance + k_close_fade_band - arm) / k_close_fade_band, 0.0f, close_most)
                        : 0.0f;
                set_character_fade(entity_addr, close_amount);
                const bool head_was_hidden = s_head_close_fade.load(std::memory_order_relaxed);
                s_head_close_fade.store(close_amount >= k_close_fade_head_hide_on ||
                                            (head_was_hidden && close_amount >= k_close_fade_head_hide_off),
                                        std::memory_order_relaxed);
            }
            if (clearance <= 0.0f || close_fade)
            {
                cam.head_fallback = false;
            }
            else if (!cam.head_fallback && arm < clearance)
            {
                cam.head_fallback = true;
                (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                         "Camera: first-person fallback ON (arm {:.2f} m < HeadClearance {:.2f} m)",
                                         arm, clearance);
            }
            else if (cam.head_fallback && arm > clearance + k_head_fallback_margin)
            {
                cam.head_fallback = false;
                (void)DMK::log().try_log(DMK::LogLevel::Debug, "Camera: first-person fallback OFF (arm {:.2f} m)", arm);
            }
            const float target = cam.head_fallback ? 1.0f : 0.0f;
            const float k = 1.0f - std::exp(-k_head_fallback_rate * delta_time);
            cam.head_fallback_blend += (target - cam.head_fallback_blend) * k;
            if (std::fabs(target - cam.head_fallback_blend) < 1e-3f)
            {
                cam.head_fallback_blend = target;
            }
            // The head hides as soon as the fallback starts (the camera is already close to it) and shows again only
            // once the camera is back out past halfway.
            s_head_fallback.store(cam.head_fallback || cam.head_fallback_blend > 0.5f, std::memory_order_relaxed);
            if (cam.head_fallback_blend > 0.0f)
            {
                camera_position = camera_position + (fpv_position - camera_position) * cam.head_fallback_blend;
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
        // When the stable / smoothed basis is active, the rendered ORIENTATION must be rebuilt from it too:
        // otherwise the engine eye orientation (which carries the view-shake) would still drive the view
        // rotation even though the position is now stable, leaving the view tilting during an action. Forcing
        // the rebuild here routes the orientation through the same eye->stable blend below. When the basis was
        // NOT overridden, eye_forward == forward and eye_up == up, so this is a no-op and the path stays
        // byte-identical to the convergence/orbit-only behaviour.
        if (basis_overridden)
        {
            rebuild_basis = true;
        }
        Vector3 screen_forward = forward;
        if (rebuild_basis)
        {
            // Blend the orientation from the engine EYE basis (FPV side, so view_blend 0 leaves the untouched
            // first-person view) to the stable look (TPV side). At view_blend 0 this reconstructs the engine
            // eye basis exactly (eye_forward x eye_up == eye_right); at view_blend 1 it is the stable look.
            Vector3 blended_forward = eye_forward + (look_forward - eye_forward) * view_blend;
            blended_forward =
                (blended_forward.magnitude_squared() > 1e-6f) ? blended_forward.normalized() : look_forward;
            screen_forward = blended_forward;
            const Vector3 ref_up = eye_up + (up - eye_up) * view_blend;
            const Vector3 right_axis = blended_forward.cross(ref_up);
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
        // (worst at close range - you cannot loot/use what the crosshair appears to be on). The hook
        // re-origins the cone onto this pose when InteractFromCamera is set. valid is true only here (while
        // the offset is engaged); the suppression path below clears it, so first person leaves the game alone.
        interaction_aim_pose().store(final_position.x, final_position.y, final_position.z, screen_forward.x,
                                     screen_forward.y, screen_forward.z);
    }

    /**
     * @brief Calls the head-visibility setter on the player entity.
     * @details Invokes the original (the trampoline, so this does not re-enter our detour).
     *          Faults are caught by the frustum detour's SEH wrapper this runs under. Runs on
     *          the view-update thread, the same thread the engine drives the setter on.
     * @param entity Player entity (C_Player).
     * @param hide_head Desired hide state (false = head shown).
     * @param flags The setter's third argument.
     */
    static void call_head_visibility(uintptr_t entity, bool hide_head, uint8_t flags)
    {
        if (const SetHeadVisibilityFunc original = s_set_head_visibility_original.load(std::memory_order_acquire))
        {
            original(entity, hide_head, static_cast<char>(flags));
        }
    }

    /**
     * @brief Latches @p player as if the game had just called the head-visibility setter on it.
     * @details The setter stores its hide value at OFFSET_ENTITY_HIDE_HEAD_FLAG and its third
     *          argument at OFFSET_ENTITY_HIDE_HEAD_ARG, and the game re-applies the head from
     *          exactly those two fields. A player the detour has not seen a call on (a new level,
     *          or a freshly loaded generation) is adopted from them before the re-assert first
     *          changes the head. Turning the offset off then restores what the game applied, not
     *          the default of a never-latched state, which would show the head in first person.
     * @param player The live C_Player.
     * @return False when either field cannot be read; nothing is latched then.
     */
    static bool adopt_head_state(uintptr_t player)
    {
        const auto hide_head =
            DMK::memory::read<uint8_t>(DMK::Address{player + Constants::OFFSET_ENTITY_HIDE_HEAD_FLAG});
        const auto flags = DMK::memory::read<uint8_t>(DMK::Address{player + Constants::OFFSET_ENTITY_HIDE_HEAD_ARG});
        if (!hide_head || !flags)
        {
            return false;
        }
        s_head_entity.store(player, std::memory_order_relaxed);
        s_head_flags.store(*flags, std::memory_order_relaxed);
        s_game_intended_hide_head.store(*hide_head != 0, std::memory_order_relaxed);
        return true;
    }

    /**
     * @brief Keeps the player head in sync with the offset state once per frame.
     * @details While the offset is active the head must stay shown (hidden only during the
     *          first-person fallback, when the camera sits at the eye, and once the close-up fade
     *          has the character mostly dithered out, since the stock eyes, hair and eye film
     *          ignore the dither), but the engine only sets head visibility on its own
     *          transitions, so re-call the setter whenever the live hide flag disagrees. When the
     *          offset has just turned off, restore the game's intended hide value exactly once so
     *          the first-person rig hides the head again. The hide flag is re-read rather than cached so this
     *          self-heals regardless of who last changed it, and a call is issued only on a
     *          mismatch so the engine is not driven every frame.
     *          The player is the live C_Player resolved this frame when there is one, else the
     *          one the setter was last called on. A live player that differs from the latched
     *          one is adopted first (adopt_head_state), because a freshly loaded generation or a
     *          new level has no call latched until the game next switches the view itself.
     * @param active Whether the offset is currently being applied to the game view.
     * @param c_player The live C_Player (vtable-validated), or 0.
     */
    static void reassert_head_visibility(bool active, uintptr_t c_player)
    {
        const uintptr_t latched = s_head_entity.load(std::memory_order_relaxed);
        const uintptr_t entity = c_player != 0 ? c_player : latched;
        if (entity != 0 && DMK::memory::is_plausible_ptr(DMK::Address{entity}) &&
            (entity == latched || adopt_head_state(entity)))
        {
            const uint8_t flags = s_head_flags.load(std::memory_order_relaxed);
            if (active)
            {
                const bool want_hidden = s_head_fallback.load(std::memory_order_relaxed) ||
                                         s_head_close_fade.load(std::memory_order_relaxed);
                const auto hide_flag =
                    DMK::memory::read<uint8_t>(DMK::Address{entity + Constants::OFFSET_ENTITY_HIDE_HEAD_FLAG});
                if (hide_flag && (*hide_flag != 0) != want_hidden)
                {
                    call_head_visibility(entity, want_hidden, flags);
                }
            }
            else if (s_head_was_active.load(std::memory_order_relaxed))
            {
                call_head_visibility(entity, s_game_intended_hide_head.load(std::memory_order_relaxed), flags);
            }
        }
        s_head_was_active.store(active, std::memory_order_relaxed);
    }

    /**
     * @brief Applies the forced-view policy on STATE-CHANGE EDGES, not every frame.
     * @details A continuous per-frame override would fight the player: they could never toggle the view
     *          while a forced state was active, because the next frame would re-force it. Instead this
     *          forces the view ONCE when a forced state begins, lets the player toggle freely while it
     *          lasts, and restores the pre-force view when the state ends - unless the player changed
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
     *
     *          orbit_active is also written by the input thread (the toggle and OrbitHoldKey). The policy turns it off
     *          and on only by compare-exchange, and an OrbitHoldKey release posts orbit_hold_released before it clears
     *          orbit_active. The exit branch restores free-look, then consumes the flag and turns free-look back off
     *          when a release was posted, so every interleaving with a release ends with free-look off. These steps
     *          stay sequentially consistent because the protocol spans two variables.
     * @param cam Camera state whose orbit_active flag is suspended and restored.
     * @param state Current GameState mask: debounced, except Menu and Overlay, which come from the live UI signals.
     * @param orbit_exclude_mask States in which free-look is disabled.
     * @return True on the frame it turns free-look off.
     */
    [[nodiscard]] static bool apply_orbit_exclude_policy(CameraState &cam, uint32_t state, uint32_t orbit_exclude_mask)
    {
        const bool excluded = (state & orbit_exclude_mask) != 0;
        static bool s_prev_excluded = false;

        if (excluded == s_prev_excluded)
        {
            // Still in the same state. If the policy suspended free-look on entry but the player has since
            // turned it back on by hand, release the suspension so the exit branch does not later re-assert
            // it against a manual choice (symmetric to apply_forced_view_policy's ownership release).
            if (excluded && cam.orbit_suspended.load(std::memory_order_relaxed) &&
                cam.orbit_active.load(std::memory_order_relaxed))
            {
                cam.orbit_suspended.store(false, std::memory_order_relaxed);
            }
            return false;
        }
        bool suspended_now = false;
        if (excluded)
        {
            // Entering an excluded state (combat, dialogue, minigame) swaps the action map, which can swallow a
            // held-move release and strand the movement-input latch > 0. Drop it now so when free-look restores on
            // exit it cannot re-trip the body-turn with the keys released (the post-combat self-rotation). Logged
            // with the cleared magnitude so the trace shows whether the latch WAS actually stranded.
            const float stranded = player_onaction_reset();
            (void)DMK::log().try_log(DMK::LogLevel::Trace,
                                     "Orbit: exclude-state entered; cleared move-latch (had magnitude {:.2f}{})",
                                     stranded, stranded > k_orbit_move_input_stop ? ", WAS STRANDED" : "");
            // If free-look was on, turn it off and remember to restore it.
            bool on = true;
            const bool suspend = cam.orbit_active.compare_exchange_strong(on, false);
            cam.orbit_suspended.store(suspend, std::memory_order_relaxed);
            suspended_now = suspend;
        }
        else if (cam.orbit_suspended.load(std::memory_order_relaxed))
        {
            // Leaving the excluded state: re-enable free-look. Like a toggle-on, it takes over from the pose on
            // screen with the orbit angles it has, so the camera does not move. An OrbitHoldKey let go of meanwhile
            // keeps it off.
            cam.orbit_suspended.store(false, std::memory_order_relaxed);
            bool off = false;
            const bool restored = cam.orbit_active.compare_exchange_strong(off, true);
            if (cam.orbit_hold_released.exchange(false) && restored)
            {
                cam.orbit_active.store(false);
            }
        }
        s_prev_excluded = excluded;
        return suspended_now;
    }

    /**
     * @brief SGameObjectEvent as the camera manager builds it when it switches the active camera.
     */
    struct alignas(16) GameObjectEvent
    {
        uintptr_t vtable;
        uint32_t event;
        uint32_t flags;
        uint64_t param[2];
    };
    static_assert(sizeof(GameObjectEvent) == 0x20, "SGameObjectEvent is 0x20 bytes");

    /**
     * @brief Sends the player the camera-changed event, so its locomotion action re-reads IsThirdPerson now.
     * @details The camera manager sends this event through C_Player::HandleEvent whenever it switches the active
     *          camera, and the movement action answers it, while idle, by taking or dropping its LockBodyTurn
     *          reference to match IsThirdPerson. Thread: the actor updates, the locomotion action included, run on
     *          job-system workers early in the frame and finish before C_Player::Update (vtable slot 22, where the
     *          game sends this same event) runs on the main thread. C_Player::UpdateView follows on the main thread,
     *          and the frustum detour runs inside it, so this is sent from the same thread and frame phase as the
     *          game's own. Guarded: a layout change in the handler must never crash the game.
     * @param c_player The live C_Player (vtable-validated), or 0.
     * @return True when the event was delivered.
     */
    static bool send_camera_changed_event(uintptr_t c_player) noexcept
    {
        if (c_player == 0 || s_player_handle_event == 0 || s_game_object_event_vtable == 0)
        {
            return false;
        }
        const GameObjectEvent event{
            .vtable = s_game_object_event_vtable,
            .event = s_camera_changed_event_id,
            .flags = s_camera_changed_event_flags,
            .param = {0, 0},
        };
        __try
        {
            reinterpret_cast<HandleEventFunc>(s_player_handle_event)(c_player, &event);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return true;
    }

    /**
     * @brief Reads the player's LockBodyTurn reference count, or -1 when it cannot be read or its offset did not
     *        resolve. Used only for the log.
     */
    static int32_t read_lock_body_turn_count(uintptr_t c_player)
    {
        if (c_player == 0 || s_lock_body_turn_count_offset == 0)
        {
            return -1;
        }
        const auto count = DMK::memory::read<int32_t>(DMK::Address{c_player + s_lock_body_turn_count_offset});
        return count ? *count : -1;
    }

    /**
     * @brief Reads the player's world body yaw and look yaw, in radians (engine convention: forward = (-sin, cos)).
     * @return False when either could not be read. Used only for the log.
     */
    static bool read_body_and_look_yaw(uintptr_t c_player, float &body_yaw, float &look_yaw)
    {
        const auto entity =
            DMK::memory::read<uintptr_t>(DMK::Address{c_player + runtime_offsets().c_player_entity.load().value});
        const auto controller = DMK::memory::read<uintptr_t>(
            DMK::Address{c_player + runtime_offsets().c_player_look_controller.load().value});
        if (!entity || !controller || !DMK::memory::is_plausible_ptr(DMK::Address{*entity}) ||
            !DMK::memory::is_plausible_ptr(DMK::Address{*controller}))
        {
            return false;
        }
        const auto world = DMK::memory::read<GameStructures::Matrix34f>(
            DMK::Address{*entity}.offset(Constants::OFFSET_ENTITY_WORLD_MATRIX_MEMBER));
        const auto yaw = DMK::memory::read<float>(DMK::Address{*controller + Constants::LOOK_CONTROLLER_YAW_OFFSET});
        if (!world || !yaw || !std::isfinite(*yaw))
        {
            return false;
        }
        body_yaw = std::atan2(-world->m[0][1], world->m[1][1]);
        look_yaw = *yaw;
        return true;
    }

    /**
     * @brief Publishes the native turn-animation state for the IsThirdPerson detour and re-syncs the locomotion
     *        action's LockBodyTurn reference on every change.
     * @details The detour reports third person to the locomotion action while all of these hold:
     *          - the third-person view is engaged, including its ease-out, so the body follows the look again only
     *            once the view is back at the eye;
     *          - the setting is on and no NativeTurnExcludeState state is active;
     *          - the view is not on the first-person eye (@p eye_anchored: the head fallback, or an Eye Height 0
     *            preset). On the eye the body follows the look as in first person, because the eye sits on the head
     *            bone and swings with a turn, and a body free of the look with no turns stays behind while the player
     *            looks around.
     *          Turns are held off (the body stays free of the look but starts no turn) while @p turns_allowed is
     *          false, which is free-look. Its look input is captured, so a look change is the mod's own heading write
     *          on a move start and must not start a turn fragment under the orbit body override.
     *          On a change the action is told at once through the camera-changed event, so an idle body is freed or
     *          follows the look again now rather than on its next locomotion change. Following again snaps the body
     *          to the look, as the game does when it leaves its own third-person camera. A player that went away (a
     *          load) took its action with it, so only the live player is ever sent the event. Game-view frustum
     *          frames only.
     * @param c_player The live C_Player (vtable-validated), or 0.
     * @param engaged Whether the third-person offset is rendering this frame.
     * @param state The debounced GameState mask.
     * @param eye_anchored Whether the view rides the first-person eye (see above).
     * @param turns_allowed Whether a turn fragment may start (see above).
     * @param delta_time Frame time in seconds, for the trace cadence.
     */
    static void update_native_turn(uintptr_t c_player, bool engaged, uint32_t state, bool eye_anchored,
                                   bool turns_allowed, float delta_time)
    {
        // Counted before the release flag is read and uncounted on every return (both sequentially consistent, like
        // the shutdown side), so a shutdown that sees no call under way knows any later call sees the flag.
        s_native_turn_updating.fetch_add(1, std::memory_order_seq_cst);
        struct UpdateScope
        {
            ~UpdateScope() { s_native_turn_updating.fetch_sub(1, std::memory_order_seq_cst); }
        } const update_scope;
        const LiveSettings &cfg = settings();
        const bool enabled = cfg.native_turn_animation.load(std::memory_order_relaxed);
        const bool excluded = (state & cfg.native_turn_exclude_mask.load(std::memory_order_relaxed)) != 0;
        const bool releasing = s_native_turn_release_requested.load(std::memory_order_seq_cst);

        // Without a resolved player there is no one to send the event to, so switching off would leave the action's
        // reference taken until its next idle start. Keep the published state through a brief miss; a longer one is a
        // load or a menu, whose player (and action) is going away anyway.
        constexpr float k_player_miss_grace = 0.5f; // seconds
        static float s_player_missing = 0.0f;
        if (c_player == 0 && !releasing)
        {
            s_player_missing += delta_time;
            if (s_player_missing < k_player_miss_grace)
            {
                return;
            }
        }
        else
        {
            s_player_missing = 0.0f;
        }
        const bool want = s_native_turn_armed.load(std::memory_order_acquire) && c_player != 0 && engaged && enabled &&
                          !excluded && !eye_anchored && !releasing;
        s_native_turn_trigger.store(want && turns_allowed, std::memory_order_relaxed);

        const uintptr_t actor = want ? c_player : 0;
        const uintptr_t previous = s_native_turn_actor.load(std::memory_order_relaxed);
        if (actor != previous)
        {
            // Counted before the store, so a shutdown that sees the new actor also sees the change in flight.
            s_native_turn_changes_in_flight.fetch_add(1, std::memory_order_seq_cst);
            s_turn_continuing.store(false, std::memory_order_relaxed);     // a fresh start for the turn decision
            s_turn_decision_missed.store(true, std::memory_order_relaxed); // and for the look's rest
            s_native_turn_actor.store(actor, std::memory_order_seq_cst);
            const uintptr_t target = actor != 0 ? actor : (previous == c_player ? previous : 0);
            const int32_t count_before = read_lock_body_turn_count(target);
            const bool sent = send_camera_changed_event(target);
            const int32_t count_after = read_lock_body_turn_count(target);
            s_native_turn_changes_in_flight.fetch_sub(1, std::memory_order_seq_cst);
            (void)DMK::log().try_log(
                DMK::LogLevel::Debug,
                "Native turn: {} (view engaged {}, setting {}, excluded state {}, view on the eye {}, shutting down "
                "{}); camera-changed event {}, LockBodyTurn count {} -> {} (0 = body follows the look)",
                actor != 0 ? "ON" : "OFF", engaged, enabled, excluded, eye_anchored, releasing,
                sent ? "sent" : "not sent", count_before, count_after);
        }

        // The player's animated character, for the movement detour that keeps turn steps in place (0 while off).
        uintptr_t player_animchar = 0;
        if (actor != 0)
        {
            const RuntimeOffsets &offsets = runtime_offsets();
            const std::array<std::ptrdiff_t, 3> animchar_chain{offsets.c_player_animated_human.load().value,
                                                               offsets.animated_human_animchar.load().value, 0};
            const auto anim_char = DMK::memory::walk(DMK::Address{c_player}, animchar_chain);
            if (anim_char && DMK::memory::is_plausible_ptr(*anim_char))
            {
                const auto vt = DMK::memory::read<uintptr_t>(*anim_char);
                if (vt && vtable_is(GameClass::AnimatedCharacter, *vt))
                {
                    player_animchar = anim_char->raw();
                }
            }
        }
        s_player_animchar.store(player_animchar, std::memory_order_relaxed);

        // The turn decision's settings, read by the decision hook in the next frame's actor update.
        const float start_angle_deg = std::clamp(cfg.native_turn_angle.load(std::memory_order_relaxed), 10.0f, 90.0f);
        s_turn_start_angle.store(DMK::math::degrees_to_radians(start_angle_deg), std::memory_order_relaxed);
        s_turn_settle_delay.store(std::max(0.0f, cfg.native_turn_settle_delay.load(std::memory_order_relaxed)),
                                  std::memory_order_relaxed);

        if (actor == 0 || !DMK::log().is_enabled(DMK::LogLevel::Trace))
        {
            return;
        }

        // Trace the gap between the look and the body a few times a second while it changes, so a log shows each
        // turn's start, progress and finish against the LockBodyTurn count and the decision state.
        static float s_trace_timer = 0.0f;
        static float s_last_gap_deg = 0.0f;
        s_trace_timer += delta_time;
        if (s_trace_timer >= 0.1f)
        {
            s_trace_timer = 0.0f;
            float body_yaw = 0.0f;
            float look_yaw = 0.0f;
            if (read_body_and_look_yaw(c_player, body_yaw, look_yaw))
            {
                const float gap = std::remainder(look_yaw - body_yaw, 2.0f * DirectX::XM_PI);
                const float gap_deg = DMK::math::radians_to_degrees(gap);
                if (std::fabs(gap_deg - s_last_gap_deg) > 1.0f)
                {
                    (void)DMK::log().try_log(DMK::LogLevel::Trace,
                                             "Native turn: look leads body by {:.1f} deg (LockBodyTurn count {}, turn "
                                             "trigger {}, turning {}, look still for {} ms)",
                                             gap_deg, read_lock_body_turn_count(c_player),
                                             s_native_turn_trigger.load(std::memory_order_relaxed) ? "on" : "off",
                                             s_turn_continuing.load(std::memory_order_relaxed),
                                             GetTickCount64() - s_turn_rest_since.load(std::memory_order_relaxed));
                }
                s_last_gap_deg = gap_deg;
            }
        }
    }

    /**
     * @brief Runs the archery step once per engine frame (the first of the frame's two builds).
     * @param cview The game view, whose pose holds the first-person eye the crosshair target ray starts beside.
     * @param c_player The live C_Player, or 0.
     * @param repeat_build True on the frame's second build, which skips the step.
     */
    static void run_archery_frame(uintptr_t cview, uintptr_t c_player, bool repeat_build)
    {
        if (repeat_build)
        {
            return;
        }
        const auto eye = DMK::memory::read<Vector3>(DMK::Address{cview + Constants::SVIEWPARAMS_POSITION_OFFSET});
        archery_frame(c_player, eye ? *eye : Vector3{}, eye.has_value());
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

        // Named timing scope for the whole per-frame camera body. Compiles to nothing unless the build sets
        // DMK_ENABLE_PROFILING, and the collision/occlusion queries below carry their own nested scopes, so a
        // profiling build attributes a frame spike to a specific query instead of "the camera hook".
        DMK_PROFILE_SCOPE("camera.frustum_build");

        // Advance the self-heal scheduler once per frame, before anything reads a healed offset. This is the
        // mod's render-thread heartbeat: every un-latched group whose gate passes and whose retry interval is
        // due runs its scan here, and a group that has resolved is not scanned again. It runs even on the idle
        // fast path below, because the bases a group waits on come up while the offset is still disengaged.
        offset_heal_tick();

        CameraState &cam = camera_state();
        LiveSettings &cfg = settings();

        const bool state_policy = cfg.enable_state_behavior.load(std::memory_order_relaxed);
        const uint32_t forced_fpv = state_policy ? cfg.forced_fpv_mask.load(std::memory_order_relaxed) : 0u;
        const uint32_t forced_tpv = state_policy ? cfg.forced_tpv_mask.load(std::memory_order_relaxed) : 0u;

        // The camera is embedded in its CView at SVIEWPARAMS_VIEWMATRIX_OFFSET, so the CView is that
        // far below the camera. Confirm it by checking the CView vtable: shadow/reflection/portal
        // cameras handed to the same builder are not embedded in a CView and fail this guard. Checked before the
        // fast path, so the about fifty other frustum builds of a frame pay only for this compare.
        if (!DMK::memory::is_plausible_ptr(DMK::Address{camera}))
        {
            return;
        }
        const uintptr_t cview = camera - Constants::SVIEWPARAMS_VIEWMATRIX_OFFSET;
        const auto vtable = DMK::memory::read<uintptr_t>(DMK::Address{cview});
        if (!vtable || !DMK::memory::is_plausible_ptr(DMK::Address{*vtable}))
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
        else if (vtable_is(GameClass::View, *vtable))
        {
            s_cview_vtable_runtime = *vtable;
        }
        else
        {
            return;
        }

        // Fast path: nothing can drive the offset, so there is nothing to do. When any forced state is
        // configured the policy must run every game-view frame to catch the state-change EDGES that
        // trigger a one-time forced switch, so only skip when the player is in manual first person with
        // no forced states configured and the head has already been restored (the head-restore branch
        // keeps running for one frame after a toggle-off, while s_head_was_active is still set).
        // The native turn animation must also be off (it lets the body follow the look again on the frame the view
        // disengages).
        // A pending arrow keeps the full path too, so a first-person shot is still followed and drawn, and so does a
        // distraction stone in hand while the aim preview is on, so its landing is still shown.
        if (!cam.applying.load(std::memory_order_relaxed) && (forced_fpv | forced_tpv) == 0 &&
            !s_head_was_active.load(std::memory_order_relaxed) && cam.view_blend <= 1e-3f &&
            s_native_turn_actor.load(std::memory_order_relaxed) == 0 &&
            !archery_wants_frame(cfg.archery_show_aim_preview.load(std::memory_order_relaxed) ? resolve_c_player() : 0))
        {
            // Even while idle (first person, no forced state), probe for the player until the world is
            // first seen so game_world_ready becomes true in-world REGARDLESS of the third-person view
            // being on - the overlay waits on it. resolve_c_player sets the flag on success and returns
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

        // Game-view camera: take the single per-frame delta and resolve the player once here, then
        // reuse both in the matrix offset below so neither is computed twice per frame. The game state
        // is derived from the discrete engine signals and published for the input-dispatch thread.
        const FrameStep step = frame_delta();
        const float delta_time = step.seconds;
        const bool repeat_build = step.repeat_build;
        const uintptr_t c_player = resolve_c_player();
        // Presets are always active, so the debounced game state is always needed here (to select the
        // active preset and, when state behaviour is on, to drive the forced-view / orbit-exclude policies).
        const uint32_t raw_state = poll_game_state(c_player);
        const uint32_t state =
            debounce_game_state(raw_state, delta_time, cfg.state_switch_hold_seconds.load(std::memory_order_relaxed));
        // The frame the orbit-exclude policy turns free-look off raises the aim hand-off (see offset_game_view_camera),
        // unless the game sets the character's facing in that situation. The policy reads the menu and the overlays
        // live, as should_apply_view does, so free-look comes back on the frame third person does. The raw state
        // counts for the facing too, since such a situation can still be inside its hold delay while the policy
        // already reacts to a screen. A same-frame rebuild reads the same state, so it is raised once.
        const uint32_t policy_state = (state & ~k_live_ui_states) | (raw_state & k_live_ui_states);
        const bool facing_held = ((state | raw_state) & k_game_held_facing_states) != 0u;
        cam.aim_handoff_requested = false;
        if (state_policy)
        {
            // Both policies are edge-triggered (they act on state-change edges, not every frame) so the
            // player keeps manual control while a state lasts. The forced-view policy writes cam.applying
            // on entry and restores it on exit; the orbit-exclude policy suspends free-look on entry and
            // restores it on exit.
            apply_forced_view_policy(cam, state, forced_fpv, forced_tpv);
            cam.aim_handoff_requested =
                apply_orbit_exclude_policy(cam, policy_state, cfg.orbit_exclude_mask.load(std::memory_order_relaxed)) &&
                !facing_held;
        }
        else
        {
            // No policy restores free-look while state behavior is off, so nothing stays suspended.
            cam.orbit_suspended.store(false, std::memory_order_relaxed);
        }
        // One that shows up while a hand-off runs ends it after this frame's rig, and the camera eases back.
        if (facing_held && cam.aim_handoff_active)
        {
            cam.aim_handoff_cancel = true;
        }
        game_state_mask().store(state, std::memory_order_relaxed);

        // Publish whether the game is showing the OS cursor (a UI is up) so the free-look input gate can
        // freeze the orbit while menus / loot / trade / dialogue are open. The hardware-mouse reference
        // counter (g_env -> p_hardware_mouse -> + count) reads > 0 whenever any UI requests the cursor.
        // Every link is screened (memory::walk + memory::read), and any failed link - or the feature
        // being disabled - publishes false, so a layout miss or a load degrades to normal orbit rather
        // than a stuck-frozen one. Read here on the render thread; the input thread only reads the
        // published flag (same producer/consumer split as game_state_mask, so no new cross-thread race).
        // Non-game-view frustum frames (shadow/reflection) skip this publish, but cannot strand the orbit
        // frozen: s_offset_active is co-published on this same game-view path and the input gate tests it
        // first, so a stale true here is moot whenever the offset is not actively rendering the view.
        bool cursor_shown = false;
        if (cfg.freeze_orbit_on_cursor.load(std::memory_order_relaxed) && s_genv_runtime != 0)
        {
            const std::array<std::ptrdiff_t, 2> cursor_chain{Constants::GENV_HARDWARE_MOUSE_OFFSET,
                                                             Constants::HARDWARE_MOUSE_CURSOR_COUNT_OFFSET};
            const auto count_addr = DMK::memory::walk(DMK::Address{s_genv_runtime}, cursor_chain);
            if (count_addr)
            {
                const auto count = DMK::memory::read<int32_t>(*count_addr);
                cursor_shown = count.has_value() && *count > 0;
            }
        }
        s_cursor_shown.store(cursor_shown, std::memory_order_relaxed);

        // Drive the player head from the offset state every frame (the engine only sets it on its own
        // transitions, so toggling the offset would otherwise leave the head stuck), then offset the
        // matrix while active. The offset follows cam.applying directly - both the manual toggle and
        // the edge-triggered policy write it - and should_apply_view() applies the SuppressTPVState hard
        // gate.
        // Ease the first-person <-> third-person blend toward the desired view so toggling (and UI
        // suppression) slides instead of snapping. ViewTransitionDuration 0 makes the switch instant.
        const bool want_tpv = cam.applying.load(std::memory_order_relaxed) && should_apply_view();
        // A situation that also leaves third person (an overlay under SuppressTPVState) does not hand the look off:
        // the camera eases to the eye, whatever ViewTransitionDuration is.
        if (!want_tpv)
        {
            cam.aim_handoff_requested = false;
        }
        const float view_dur = cfg.view_transition_duration.load(std::memory_order_relaxed);
        const float view_target = want_tpv ? 1.0f : 0.0f;
        if (view_dur > 1e-4f)
        {
            const float view_step = delta_time / view_dur;
            cam.view_blend = (cam.view_blend < view_target) ? std::min(view_target, cam.view_blend + view_step)
                                                            : std::max(view_target, cam.view_blend - view_step);
        }
        else
        {
            cam.view_blend = view_target;
        }

        // The offset is rendered while heading TO or holding third person, so the ease-OUT renders too.
        const bool offset_engaged = want_tpv || cam.view_blend > 1e-3f;
        s_offset_active.store(offset_engaged, std::memory_order_relaxed);
        reassert_head_visibility(offset_engaged, c_player);
        // The body's native turn-in-place animation follows the offset too (it follows the look again once the view is
        // back at the eye), and is off while the view rides the first-person eye (the head fallback, or an Eye Height 0
        // preset anchored to it). No turn starts under free-look.
        const bool eye_anchored =
            s_head_fallback.load(std::memory_order_relaxed) || cfg.eye_height.load(std::memory_order_relaxed) <= 0.0f;
        update_native_turn(c_player, offset_engaged, state, eye_anchored,
                           !cam.orbit_active.load(std::memory_order_relaxed), delta_time);

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
                cam.orbit_move_armed_by_game = false;
                const float stranded = player_onaction_reset();
                if (stranded > k_orbit_move_input_stop)
                {
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Trace,
                        "Orbit: TPV disengaged; cleared a stranded move-latch (magnitude {:.2f})", stranded);
                }
                s_orbit_was_engaged = false;
            }
            cam.collision_valid = false;
            cam.collision_speed = 0.0f;
            cam.collision_hold_timer = 0.0f;
            cam.head_fallback = false;
            cam.head_fallback_blend = 0.0f;
            s_head_fallback.store(false, std::memory_order_relaxed);
            s_head_close_fade.store(false, std::memory_order_relaxed);
            // The character draws solid again at once: in first person its body is in view.
            set_character_fade(0, 0.0f);
            // First person (or suppressed): the camera is the eye, so the interaction hook must NOT redirect.
            interaction_aim_pose().invalidate();
            // Nor does the AI's camera observer need the eye substituted.
            s_ai_eye_valid.store(false, std::memory_order_release);
            // The held base outlives a first-person gap while a situation holds free-look off, so free-look comes
            // back on the base it had.
            if (!cam.orbit_suspended.load(std::memory_order_relaxed))
            {
                cam.orbit_base_held = false;
            }
            cam.orbit_render_valid =
                false; // next engaged frame snaps the orbit low-pass instead of easing across the gap
            cam.orbit_steer_valid = false; // and the continuous-align steer low-pass
            cam.eye_sync_valid = false;    // and the dynamic eye-height low-pass
            cam.fov_ease_valid = false;    // and the per-preset FOV override ease
            cam.basis_quat_valid = false;  // and the aim-basis low-pass
            cam.turn_track_valid = false;  // and the turn-in-place pivot hold
            // A press in first person does not start a free-look move.
            s_move_press_stamp_ms.store(0, std::memory_order_release);
            s_move_press_animchar.store(0, std::memory_order_relaxed);
            s_move_press_turned_ms.store(0, std::memory_order_relaxed);
            // A move under way keeps the angle the camera last showed, as stopping does.
            if (cam.orbit_moving && cam.orbit_target_valid)
            {
                cam.orbit_yaw.store(cam.orbit_yaw_render, std::memory_order_relaxed);
            }
            cam.orbit_moving = false;
            cam.orbit_target_valid = false;
            // A hand-off that turned the look left angles measured against the old look; the look takes the camera
            // direction once play resumes, so they are centered.
            if (cam.aim_handoff_active && cam.aim_handoff_written)
            {
                cam.orbit_yaw.store(0.0f, std::memory_order_relaxed);
                cam.orbit_pitch.store(0.0f, std::memory_order_relaxed);
                cam.orbit_yaw_render = 0.0f;
                cam.orbit_pitch_render = 0.0f;
            }
            cam.aim_handoff_active = false; // and the aim hand-off, which has no camera to hold
            cam.aim_handoff_cancel = false;
            cam.rig_last_orbited = false;
            // Back to first person: the next engaged frame should snap the preset, not ease across the gap.
            Presets::reset_transition();
            // Arrows loosed in first person are still followed and drawn (the pose is invalid, so none is re-aimed).
            run_archery_frame(cview, c_player, repeat_build);
            return;
        }
        s_orbit_was_engaged = true;

        // Resolve the active preset (by debounced state, or the overlay's editing pin) and ease the
        // live framing toward it BEFORE the matrix offset reads those settings this frame.
        Presets::resolve_and_apply(state, delta_time);

        // Publish the untouched first-person eye for the camera-observer detour before the matrix is offset: the AI
        // judges which NPCs to update from the observer, and in first person that observer stands at the eye.
        if (const auto eye = DMK::memory::read<Vector3>(DMK::Address{cview + Constants::SVIEWPARAMS_POSITION_OFFSET});
            eye)
        {
            s_ai_eye_x.store(eye->x, std::memory_order_relaxed);
            s_ai_eye_y.store(eye->y, std::memory_order_relaxed);
            s_ai_eye_z.store(eye->z, std::memory_order_relaxed);
            s_ai_eye_valid.store(true, std::memory_order_release);
        }

        // Smoothstep the linear view blend for an ease-in/out feel, then offset with it.
        const float vb = cam.view_blend;
        const float view_s = vb * vb * (3.0f - 2.0f * vb);
        offset_game_view_camera(camera, cview, c_player, step, view_s);
        // After the offset, so the crosshair target is cast from the pose this frame renders.
        run_archery_frame(cview, c_player, repeat_build);
    }

    /**
     * @brief Runs the per-frame camera offset under SEH, so an outdated offset or layout never crashes the game.
     * @details Apart from the detour, because the detour's DetourScope needs C++ object unwinding, which a __try
     *          frame cannot hold.
     */
    static void frustum_build_offset_guarded(uintptr_t camera) noexcept
    {
        __try
        {
            detour_frustum_build_impl(camera);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    /**
     * @brief Per-frame frustum-builder detour.
     * @details The matrix offset runs BEFORE the original, which then computes the cull planes from
     *          the offset matrix, so geometry culling matches the rendered third-person view. A
     *          layout drift degrades the offset to a no-op. The original always runs and its return
     *          value is forwarded so the frustum is still built whether or not the offset applied.
     */
    static uintptr_t __fastcall detour_frustum_build(uintptr_t camera) noexcept
    {
        const DetourScope in_flight;
        frustum_build_offset_guarded(camera);
        const FrustumBuildFunc original = s_frustum_build_original.load(std::memory_order_acquire);
        return original ? original(camera) : 0;
    }

    /**
     * @brief Runs the head-visibility latch and the original setter under SEH.
     * @details Apart from the detour, because the detour's DetourScope needs C++ object unwinding, which a __try
     *          frame cannot hold.
     */
    static void set_head_visibility_guarded(SetHeadVisibilityFunc original, uintptr_t entity, bool hide_head,
                                            char flags) noexcept
    {
        __try
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
            // to match the view the player actually sees. The first-person fallback (camera at the
            // eye, no room behind) hides it like first person does, and so does a close-up fade far
            // enough along that the eyes and hair would stay solid in the dithered face.
            const bool active = s_offset_active.load(std::memory_order_relaxed);
            const bool final_hide_head = active ? (s_head_fallback.load(std::memory_order_relaxed) ||
                                                   s_head_close_fade.load(std::memory_order_relaxed))
                                                : hide_head;
            original(entity, final_hide_head, flags);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            // Swallow: a layout change in the head setter must never crash the game.
        }
    }

    /**
     * @brief Head-visibility detour: keep the player head while the offset is rendering.
     * @details The first-person rig hides the head so it does not clip the eye camera.
     *          While the third-person view is active, force hide_head to false
     *          so the player is not headless from behind. Otherwise pass the game's
     *          intended value through unchanged.
     */
    static void __fastcall detour_set_head_visibility(uintptr_t entity, bool hide_head, char flags) noexcept
    {
        const DetourScope in_flight;
        if (const SetHeadVisibilityFunc original = s_set_head_visibility_original.load(std::memory_order_acquire))
        {
            set_head_visibility_guarded(original, entity, hide_head, flags);
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
        if (!s_offset_active.load(std::memory_order_relaxed) || !cam.orbit_active.load(std::memory_order_relaxed) ||
            s_cursor_shown.load(std::memory_order_relaxed) || !should_apply_view())
        {
            return false;
        }
        if (input_event == 0 || !DMK::memory::is_plausible_ptr(DMK::Address{input_event}))
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
                if (id == Constants::INPUT_LOOK_YAW_EVENT_ID)
                {
                    // Negated so mouse-left orbits the camera left and mouse-right orbits right; a negative X
                    // sensitivity inverts that.
                    const float sensitivity_x = settings().orbit_sensitivity_x.load(std::memory_order_relaxed);
                    cam.orbit_yaw.store(cam.orbit_yaw.load(std::memory_order_relaxed) - value * sensitivity_x,
                                        std::memory_order_relaxed);
                }
                else
                {
                    // Mouse-up raises the camera, mouse-down lowers it; a negative Y sensitivity inverts that.
                    const float sensitivity_y = settings().orbit_sensitivity_y.load(std::memory_order_relaxed);
                    // Within the range the render thread published (see CameraState::orbit_pitch_lo); a value
                    // already outside it is kept but cannot grow.
                    const float old_pitch = cam.orbit_pitch.load(std::memory_order_relaxed);
                    cam.orbit_pitch.store(
                        std::clamp(old_pitch + value * sensitivity_y,
                                   std::min(cam.orbit_pitch_lo.load(std::memory_order_relaxed), old_pitch),
                                   std::max(cam.orbit_pitch_hi.load(std::memory_order_relaxed), old_pitch)),
                        std::memory_order_relaxed);
                }
                return true; // block ONLY the look so the player look stays put while free-looking
            }

            // Gamepad RIGHT STICK: latch the held DEFLECTION (-1..1) for the orbit rate integration, then ZERO
            // the event value IN PLACE and let it PASS (do NOT block). A held analog stick drives an engine
            // look-RATE that the engine only zeroes on a release event. BLOCKING swallows that release: if orbit
            // engages while the stick is already deflected (e.g. you hold the right stick then toggle orbit, or an
            // exclude state suspends/restores orbit mid-deflection), the engine keeps its last rate and SPINS the
            // player look forever - surviving a switch to first person and curable only by a hard input flush
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
     * @brief Runs orbit_capture_and_decide under SEH. A fault falls back to normal dispatch.
     * @details Apart from the detour, because the detour's DetourScope needs C++ object unwinding, which a __try
     *          frame cannot hold.
     * @return true to block (swallow) the event, false to dispatch it normally.
     */
    [[nodiscard]] static bool orbit_capture_guarded(uintptr_t input_event) noexcept
    {
        __try
        {
            return orbit_capture_and_decide(input_event);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    /**
     * @brief Input-dispatcher detour. Swallows events while free-looking, else passes through.
     */
    static void __fastcall detour_input_dispatch(uintptr_t controller, uintptr_t input_event, char flag) noexcept
    {
        const DetourScope in_flight;
        if (orbit_capture_guarded(input_event))
        {
            return;
        }
        if (const InputDispatchFunc original = s_input_dispatch_original.load(std::memory_order_acquire))
        {
            original(controller, input_event, flag);
        }
    }

    /**
     * @brief Resolves the SSystemGlobalEnvironment (g_env) base, patch-resiliently.
     * @details Reads the g_env base through the Engine gate. The Genv anchor is a 2-of-5 quorum of
     *          RIP-relative lea/mov [rip+g_env] reference sites, so the address survives game
     *          patches that shift the RVA. The result is screened as a plausible user-space pointer
     *          before it is accepted. There is no fixed-address fallback: the Steam, GOG and Game Pass
     *          builds each keep g_env at a different address, so a fixed one names the wrong slot
     *          on every build but one, and the engine calls made through it run arbitrary code.
     * @return The g_env base address, or 0 when the gate failed. Every consumer then stays off.
     */
    static uintptr_t resolve_genv()
    {
        DMK::Logger &logger = DMK::log();

        // The Genv quorum resolved up front in resolve_all_anchors(), when at least two independent
        // lea/mov [rip+g_env] reference sites agreed. The result is screened as a plausible pointer.
        const uintptr_t resolved = gated_anchor_address(Feature::Engine, AnchorId::Genv);
        if (resolved != 0 && DMK::memory::is_plausible_ptr(DMK::Address{resolved}))
        {
            logger.info("Camera: g_env resolved at {}", DMK::format::format_address(resolved));
            return resolved;
        }

        logger.warning("Camera: g_env did not resolve; collision, aim convergence, render occlusion and the "
                       "player look features are unavailable");
        return 0;
    }

    /**
     * @brief Camera-observer update detour: keeps the AI's camera observer at the first-person eye in third person.
     * @details The engine's camera observer copies the system view camera, which carries the third-person offset.
     *          The AI selects which NPCs to update in full, and which to hide and pause, from that observer, so an
     *          NPC a few metres beyond its reach from the pulled-back camera (a sleeper in a house, someone inside a
     *          shop) vanishes in third person while first person, whose camera is the eye, shows it. The original
     *          runs first. While the offset renders, its position is replaced with the eye the frustum detour last
     *          published, so every consumer of the observer sees what it sees in first person. The direction and
     *          field of view stay as the original set them: the offset camera looks the same way as the eye.
     */
    static bool __fastcall detour_camera_observer_update(uintptr_t observer, float *position, bool *position_valid,
                                                         float *direction, bool *direction_valid, float *fov) noexcept
    {
        const DetourScope in_flight;
        const CameraObserverUpdateFunc original = s_camera_observer_update_original.load(std::memory_order_acquire);
        const bool updated =
            original != nullptr && original(observer, position, position_valid, direction, direction_valid, fov);
        // The original has just written through this pointer, so writing it again is as safe as its own store.
        if (updated && position != nullptr && s_offset_active.load(std::memory_order_relaxed) &&
            s_ai_eye_valid.load(std::memory_order_acquire))
        {
            position[0] = s_ai_eye_x.load(std::memory_order_relaxed);
            position[1] = s_ai_eye_y.load(std::memory_order_relaxed);
            position[2] = s_ai_eye_z.load(std::memory_order_relaxed);
        }
        return updated;
    }

    /**
     * @brief Reads a vtable slot and returns its target when it lies in the game image, else 0.
     */
    static uintptr_t read_image_vtable_slot(uintptr_t vtable, size_t slot, uintptr_t module_base, size_t module_size)
    {
        const DMK::Region image{DMK::Address{module_base}, module_size};
        const auto slot_value = DMK::memory::read<uintptr_t>(DMK::Address{vtable + slot * sizeof(uintptr_t)});
        return slot_value.has_value() && image.contains(DMK::Address{*slot_value}) ? *slot_value : 0;
    }

    /**
     * @brief True when @p signature matches within the first @p window bytes of the code at @p target, all inside
     *        @p image.
     * @details A window of the pattern's own length requires the match to start exactly at @p target. Proves the
     *          instructions a call or hook depends on at an address read from live data, before it is trusted.
     */
    static bool code_window_matches(uintptr_t target, const DMK::scan::Pattern &signature, size_t window,
                                    const DMK::Region &image)
    {
        if (target == 0 || window == 0)
        {
            return false;
        }
        const DMK::Region head{DMK::Address{target}, window};
        return image.contains(head.base) && image.contains(head.base.offset(static_cast<std::ptrdiff_t>(window) - 1)) &&
               DMK::scan::scan(signature, head, 1, DMK::scan::Pages::Executable).has_value();
    }

    /**
     * @brief Reads a vtable slot and returns its target when it lies in the game image and @p signature matches
     *        within its first @p window bytes, else 0, so a slot that no longer holds the expected function is
     *        refused rather than hooked or called.
     */
    static uintptr_t read_checked_vtable_slot(uintptr_t vtable, size_t slot, const DMK::scan::Pattern &signature,
                                              size_t window, uintptr_t module_base, size_t module_size)
    {
        const DMK::Region image{DMK::Address{module_base}, module_size};
        const uintptr_t target = read_image_vtable_slot(vtable, slot, module_base, module_size);
        return code_window_matches(target, signature, window, image) ? target : 0;
    }

    /**
     * @brief Hooks the camera observer's update, found through its RTTI vtable (no signature scan).
     * @details Best-effort: without it the camera still works, and NPCs just beyond the pulled-back camera's reach stay
     *          hidden in third person, as they would for the unmodded game with its view camera there. The slot's
     *          target must lie in the game image and still read the view camera (Aob::k_cameraObserverUpdateBody
     *          among its first instructions), so a patch that moves the slot leaves the hook uninstalled rather than
     *          hooking another function.
     */
    static void install_camera_observer_hook(uintptr_t module_base, size_t module_size, HookSet &hooks)
    {
        DMK::Logger &logger = DMK::log();
        const std::optional<std::uintptr_t> vtable = class_vtable(GameClass::CameraObserver);
        if (!vtable.has_value())
        {
            logger.warning("Camera: C_CameraObserver did not resolve; in third person the AI keeps judging NPCs from "
                           "the pulled-back camera");
            return;
        }
        const uintptr_t target = read_checked_vtable_slot(
            *vtable, Constants::CAMERA_OBSERVER_VTABLE_UPDATE_SLOT, Aob::k_cameraObserverUpdateBody,
            Aob::k_cameraObserverUpdateBodyWindow, module_base, module_size);
        if (target == 0)
        {
            logger.warning("Camera: C_CameraObserver slot {} is not an in-image update that reads the view camera; "
                           "camera-observer hook skipped",
                           Constants::CAMERA_OBSERVER_VTABLE_UPDATE_SLOT);
            return;
        }

        auto result = DMK::hook::inline_at(
            DMK::hook::InlineRequest{.name = "CameraObserverUpdate", .target = DMK::Address{target}},
            detour_camera_observer_update);
        if (!result.has_value())
        {
            logger.warning("Camera: camera-observer hook failed ({}); in third person the AI keeps judging NPCs from "
                           "the pulled-back camera",
                           result.error().message());
            return;
        }
        // Publish the trampoline before enable() arms the patch, so the detour never sees a null original.
        s_camera_observer_update_original.store(result->original<CameraObserverUpdateFunc>(),
                                                std::memory_order_release);
        const auto armed = hooks.push(std::move(*result)).enable();
        if (!armed.has_value())
        {
            logger.warning("Camera: camera-observer hook could not be armed ({}); in third person the AI keeps judging "
                           "NPCs from the pulled-back camera",
                           armed.error().message());
            return;
        }
        logger.info("Camera: the AI camera observer follows the first-person eye in third person (C_CameraObserver "
                    "slot {} at {})",
                    Constants::CAMERA_OBSERVER_VTABLE_UPDATE_SLOT, DMK::format::format_address(target));
    }

    /**
     * @brief IsThirdPerson detour: answers "third person" to the free-roam locomotion action for the native turn.
     * @details Hot (every actor, several calls a frame each), so it only compares: the published player first, then
     *          the caller's return address against the two resolved call sites. The inline hook enters this detour
     *          with a jump, so _ReturnAddress() is the game caller's return address. Every other call, every other
     *          actor's included, goes to the original unchanged. Reads nothing through a pointer, so it needs no SEH
     *          frame.
     */
    static bool __fastcall detour_is_third_person(uintptr_t actor) noexcept
    {
        const DetourScope in_flight;
        if (actor != 0 && actor == s_native_turn_actor.load(std::memory_order_relaxed))
        {
            const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
            if (ret == s_lock_sync_return)
            {
                return true;
            }
            if (ret == s_turn_trigger_return && s_native_turn_trigger.load(std::memory_order_relaxed))
            {
                return true;
            }
        }
        const IsThirdPersonFunc original = s_is_third_person_original.load(std::memory_order_acquire);
        return original != nullptr && original(actor);
    }

    /**
     * @brief True when deciding to turn this frame would set the game's spin latch: the latch is clear, a turn
     *        fragment is still installed (state 1 or 2) and the gap's @p sign differs from the previous evaluation's
     *        non-zero sign. The latch then spins the body the OLD way, the long way round. @p action is the
     *        C_PlayerMovementAction the hook runs in. The field offsets come from resolve_turn_decision_layout. An
     *        unreadable field counts as a latch, so the turn waits.
     */
    static bool would_set_spin_latch(uintptr_t action, float sign) noexcept
    {
        const auto latched = DMK::memory::read<uint8_t>(DMK::Address{action + s_spin_latch_offset});
        const auto state = DMK::memory::read<int32_t>(DMK::Address{action + s_installed_state_offset});
        const auto last_sign = DMK::memory::read<float>(DMK::Address{action + s_last_sign_offset});
        if (!latched.has_value() || !state.has_value() || !last_sign.has_value())
        {
            return true;
        }
        return *latched == 0 && (*state == 1 || *state == 2) && *last_sign != 0.0f && *last_sign != sign;
    }

    /**
     * @brief Turn-decision mid hook: decides turn versus idle for the player through the flags `seta cl` reads.
     * @details Runs at the `seta cl` right after the game compared |angle| (xmm7) with its 35 degrees. The game
     *          reaches it only after IsThirdPerson answered true, so for the player only while the native turn is on
     *          with turns allowed. Leaves any other actor to the game. Sets the flags so that cl comes out as the
     *          mod's decision: CF and ZF clear gives cl = 1 (turn), ZF set gives cl = 0 (idle). xmm7 is left alone,
     *          so the animation's turn angle and playback speed, which the game derives from it next, are the game's
     *          own. See s_turn_start_angle.
     */
    static void detour_turn_decision(DMK::hook::MidContext &ctx) noexcept
    {
        const uintptr_t actor = DMK::hook::gpr(ctx, DMK::hook::Gpr::Rbx);
        if (actor == 0 || actor != s_native_turn_actor.load(std::memory_order_relaxed))
        {
            return;
        }
        if (DMK::hook::gpr(ctx, DMK::hook::Gpr::R12) == 0)
        {
            // UpdatePending: the action is queued behind another one that moves him, and runs ComputeMoveState without
            // the turn-angle output pointer it keeps in r12. Left to the game, and not an idle decision.
            return;
        }
        const float gap = DMK::hook::xmm(ctx, 13).lane<float>(0); // signed look-minus-body angle, radians
        if (!std::isfinite(gap))
        {
            return;
        }
        const float gap_abs = std::fabs(gap);
        const float sign = gap < 0.0f ? -1.0f : 1.0f;
        const uint64_t now = GetTickCount64();
        const uint64_t last_decision = s_turn_decision_tick.exchange(now, std::memory_order_relaxed);
        const bool broke = s_turn_steps_hooked.load(std::memory_order_relaxed)
                               ? s_turn_decision_missed.exchange(false, std::memory_order_relaxed)
                               : now - last_decision > k_turn_decision_break_ms;
        if (broke)
        {
            // Back in the idle decision after a break: what came before no longer describes this turn or this rest.
            s_turn_continuing.store(false, std::memory_order_relaxed);
            s_turn_rest_gap.store(gap, std::memory_order_relaxed);
            s_turn_rest_since.store(now, std::memory_order_relaxed);
        }
        s_turn_decision_seen.store(true, std::memory_order_relaxed);

        // The look's rest, judged on the gap itself (see k_turn_rest_drift).
        if (std::fabs(gap - s_turn_rest_gap.load(std::memory_order_relaxed)) > k_turn_rest_drift)
        {
            s_turn_rest_gap.store(gap, std::memory_order_relaxed);
            s_turn_rest_since.store(now, std::memory_order_relaxed);
        }
        const float settle_delay = s_turn_settle_delay.load(std::memory_order_relaxed);
        const uint64_t rest_ms = now - s_turn_rest_since.load(std::memory_order_relaxed);
        const bool rested = settle_delay > 0.0f && static_cast<float>(rest_ms) >= settle_delay * 1000.0f;

        // A started turn continues until he faces the look. Otherwise a turn starts past the start angle, or past the
        // settle angle after the look rested.
        const bool wants_turn =
            s_turn_continuing.load(std::memory_order_relaxed)
                ? gap_abs > k_turn_finish_angle && sign == s_turn_last_sign.load(std::memory_order_relaxed)
                : gap_abs > s_turn_start_angle.load(std::memory_order_relaxed) ||
                      (gap_abs > k_turn_settle_angle && rested);
        const bool turn = wants_turn && !would_set_spin_latch(DMK::hook::gpr(ctx, DMK::hook::Gpr::Rdi), sign);
        s_turn_continuing.store(turn, std::memory_order_relaxed);
        s_turn_last_sign.store(sign, std::memory_order_relaxed);
        if (turn)
        {
            s_turn_decided_tick.store(now, std::memory_order_relaxed);
            s_turn_tail_frames.store(k_turn_step_tail_frames, std::memory_order_relaxed);
        }
        else if (const int tail = s_turn_tail_frames.load(std::memory_order_relaxed); tail > 0)
        {
            s_turn_tail_frames.store(tail - 1, std::memory_order_relaxed);
        }

        constexpr uintptr_t k_carry_flag = 0x1;
        constexpr uintptr_t k_zero_flag = 0x40;
        uintptr_t &cpu_flags = DMK::hook::flags(ctx);
        cpu_flags = turn ? (cpu_flags & ~(k_carry_flag | k_zero_flag)) : (cpu_flags | k_zero_flag);
    }

    /**
     * @brief Installs the turn-decision mid hook on the site resolve_turn_decision_layout proves around the trigger.
     * @details Best-effort: without it the native turn still works, with the game's own 35 degrees and the body
     *          resting about 35 degrees short of the look. The layout fixes the registers the hook reads (rbx, xmm13,
     *          rdi, r12), the instruction it sits on, and the action's spin-latch field offsets, which are written
     *          before the hook arms.
     */
    static void install_turn_decision_hook(uintptr_t trigger_return, HookSet &hooks)
    {
        DMK::Logger &logger = DMK::log();
        const std::optional<TurnDecisionLayout> layout = resolve_turn_decision_layout(trigger_return);
        if (!layout.has_value())
        {
            logger.warning("Camera: turn decision hook skipped; turns rest about 35 degrees short of the look");
            return;
        }
        const uintptr_t site = layout->site;
        s_spin_latch_offset = layout->spin_latch_offset;
        s_installed_state_offset = layout->installed_state_offset;
        s_last_sign_offset = layout->last_sign_offset;
        auto result = DMK::hook::mid_at(DMK::hook::MidRequest{.name = "TurnDecision", .target = DMK::Address{site}},
                                        detour_turn_decision);
        if (!result.has_value())
        {
            logger.warning("Camera: turn decision hook failed ({}); turns rest about 35 degrees short of the look",
                           result.error().message());
            return;
        }
        const auto armed = hooks.push(std::move(*result)).enable();
        if (!armed.has_value())
        {
            logger.warning("Camera: turn decision hook could not be armed ({}); turns rest about 35 degrees short "
                           "of the look",
                           armed.error().message());
            return;
        }
        logger.info("Camera: turn decision hooked at {} (turns finish facing the look)",
                    DMK::format::format_address(site));
    }

    // The movement request is a QuatT: the rotation (x, y, z, w), then the world translation (x, y, z).
    constexpr std::size_t k_movement_translation_x = 4;
    constexpr std::size_t k_movement_translation_y = 5;

    /**
     * @brief Drops the horizontal translation of a movement request, unless the request is an impulse.
     * @return True when the translation was dropped.
     */
    static bool drop_horizontal_step(uintptr_t animchar, float *movement) noexcept
    {
        const auto type = DMK::memory::read<int32_t>(DMK::Address{animchar + s_movement_type_offset});
        if (!type.has_value() || *type == Constants::ANIMATED_CHARACTER_MOVEMENT_IMPULSE)
        {
            return false;
        }
        movement[k_movement_translation_x] = 0.0f;
        movement[k_movement_translation_y] = 0.0f;
        return true;
    }

    /**
     * @brief Movement detour: keeps a native turn step on the spot, and drops the stale first step of a free-look move
     *        started at a key press.
     * @details Drops the horizontal translation of the player's movement on a frame his movement action ran its
     *          idle/turn decision, from the first turn frame through the step tail after the last one (see
     *          s_turn_decision_seen). A frame without a decision ends the tail and marks a break for the decision hook.
     *          The rotation and the vertical part are left to the game, and so is an impulse.
     *
     *          A free-look press that turned the body (orbit_move_pressed) arms one more drop. The game builds the
     *          frame's travel from the entity rotation in the player's pre-physics update and applies the body
     *          override only at the end of the animated-character update, so the first request after the press still
     *          points along the old facing (toward the camera when the character faced it). That one request loses its
     *          horizontal part; the next one follows the new facing.
     *
     *          Every other character, and the player on any frame neither case applies (walking, interactions,
     *          scripted animations), goes through untouched. Hot (every animated character each frame), so it only
     *          compares before calling the original.
     */
    static char __fastcall detour_phys_ent_movement(uintptr_t animchar, float *movement) noexcept
    {
        const DetourScope in_flight;
        // A compare first, so the many other characters cost no locked instruction; the exchange then takes the press
        // once even when the dispatcher arms a new one meanwhile.
        if (std::uintptr_t pressed = animchar;
            animchar != 0 && s_move_press_animchar.load(std::memory_order_relaxed) == animchar &&
            s_move_press_animchar.compare_exchange_strong(pressed, 0, std::memory_order_acquire,
                                                          std::memory_order_relaxed))
        {
            const std::uint64_t turned = s_move_press_turned_ms.exchange(0, std::memory_order_relaxed);
            if (turned != 0 && movement != nullptr && GetTickCount64() - turned <= k_move_press_max_age_ms)
            {
                const float step_x = movement[k_movement_translation_x];
                const float step_y = movement[k_movement_translation_y];
                if (drop_horizontal_step(animchar, movement))
                {
                    (void)DMK::log().try_log(DMK::LogLevel::Trace,
                                             "Orbit: dropped the first step after the press ({:.3f}, {:.3f}), built "
                                             "along the old facing",
                                             step_x, step_y);
                }
            }
        }
        if (animchar != 0 && animchar == s_player_animchar.load(std::memory_order_relaxed))
        {
            if (!s_turn_decision_seen.exchange(false, std::memory_order_relaxed))
            {
                // Something else drove this frame: a break for the decision, and the end of any step tail.
                s_turn_decision_missed.store(true, std::memory_order_relaxed);
                s_turn_tail_frames.store(0, std::memory_order_relaxed);
                s_turn_decided_tick.store(0, std::memory_order_relaxed);
            }
            else if (movement != nullptr &&
                     (s_turn_tail_frames.load(std::memory_order_relaxed) > 0 ||
                      GetTickCount64() - s_turn_decided_tick.load(std::memory_order_relaxed) < k_turn_step_tail_ms))
            {
                (void)drop_horizontal_step(animchar, movement);
            }
        }
        const UpdatePhysEntMovementFunc original = s_phys_ent_movement_original.load(std::memory_order_acquire);
        return original != nullptr ? original(animchar, movement) : 0;
    }

    /**
     * @brief Hooks CAnimatedCharacter::UpdatePhysicalEntityMovement to keep turn steps in place and drop the stale
     *        first step of a free-look move started at a press. Best-effort: without it a turn step moves the body
     *        5-20 cm, which the still camera leaves slightly off center, and a free-look move that turns the body
     *        starts with a short step along the old facing.
     * @details Installed on its own: the turn-step part acts only on frames the decision hook flags, so without that
     *          hook it never drops a step, while the press drop needs only this hook.
     */
    static void install_turn_in_place_hook(HookSet &hooks)
    {
        DMK::Logger &logger = DMK::log();
        s_move_press_stamp_ms.store(0, std::memory_order_relaxed);
        s_move_press_animchar.store(0, std::memory_order_relaxed);
        s_move_press_turned_ms.store(0, std::memory_order_relaxed);
        const uintptr_t target = gated_anchor_address(Feature::TurnSteps, AnchorId::PhysEntMovement);
        if (target == 0)
        {
            logger.warning("Camera: UpdatePhysicalEntityMovement did not resolve; turn steps move the body a little");
            return;
        }
        // The movement request type the detour reads to leave impulses alone, written before the hook arms.
        const std::optional<std::ptrdiff_t> type_offset = resolve_movement_type_offset(target);
        if (!type_offset.has_value())
        {
            logger.warning("Camera: UpdatePhysicalEntityMovement at {} is not the expected one; turn steps move the "
                           "body a little",
                           DMK::format::format_address(target));
            return;
        }
        s_movement_type_offset = *type_offset;
        auto result = DMK::hook::inline_at(
            DMK::hook::InlineRequest{.name = "UpdatePhysicalEntityMovement", .target = DMK::Address{target}},
            detour_phys_ent_movement);
        if (!result.has_value())
        {
            logger.warning("Camera: UpdatePhysicalEntityMovement hook failed ({}); turn steps move the body a little",
                           result.error().message());
            return;
        }
        // Published before enable() arms the patch, and never cleared: an armed or pinned hook still routes through it.
        s_phys_ent_movement_original.store(result->original<UpdatePhysEntMovementFunc>(), std::memory_order_release);
        const auto armed = hooks.push(std::move(*result)).enable();
        if (!armed.has_value())
        {
            logger.warning("Camera: UpdatePhysicalEntityMovement hook could not be armed ({}); turn steps move "
                           "the body a little",
                           armed.error().message());
            return;
        }
        s_turn_steps_hooked.store(true, std::memory_order_relaxed);
        logger.info("Camera: turn steps kept in place (UpdatePhysicalEntityMovement at {})",
                    DMK::format::format_address(target));
    }

    /**
     * @brief Crouched animations mid hook on CAnimationSet::GetAnimIDByCRC (see
     *        Constants::CROUCHED_IDLE_PLAYER_ANIMATION).
     * @details While the native turn is on, a lookup of the player's crouched idle or crouched turn blend space gets
     *          its NPC twin, so the idle and the turns share one stance. The hook runs at the entry of the vtable
     *          slot's thunk, with the name hash in rdx. Every character's lookups pass through it, so it only compares.
     *          Any other hash goes to the game unchanged, and so does any lookup while the native turn is off (first
     *          person, or the feature switched off). A clip looks its animation up when it starts, so a crouched clip
     *          started in third person keeps that animation.
     */
    static void detour_crouched_animation(DMK::hook::MidContext &ctx) noexcept
    {
        if (s_native_turn_actor.load(std::memory_order_relaxed) == 0)
        {
            return;
        }
        uintptr_t &hash = DMK::hook::gpr(ctx, DMK::hook::Gpr::Rdx);
        for (const AnimationSwap &swap : s_crouched_swaps)
        {
            if (hash == swap.player)
            {
                hash = swap.npc;
                return;
            }
        }
    }

    /**
     * @brief Hooks CAnimationSet::GetAnimIDByCRC for the crouched animation swap (see detour_crouched_animation).
     *        Best-effort: without it a crouched right turn can throw the game's broken pose late in its clip.
     * @details CActionScope::InstallAnimation calls the slot with a clip's name hash, and AnchorId::AnimIdByCrcSlot
     *          reads the slot from that call. The slot comes from CAnimationSet's RTTI vtable. Its target must lie in
     *          the game image and still be the thunk to the name map lookup (Aob::k_animIdByCrcThunk). The hashes come
     *          from the name hash that CAnimationSet::GetAnimIDByName calls (AnchorId::AnimNameHashCall). That function
     *          must lie in the game image and still open as Aob::k_animNameHashBody before the mod calls it. All four
     *          hashes must be non-zero and distinct, and the idle and the turns swap together or not at all, because
     *          one without the other slides the body between the two stances.
     */
    static void install_crouched_animation_hook(uintptr_t module_base, size_t module_size, HookSet &hooks)
    {
        DMK::Logger &logger = DMK::log();
        const DMK::Region image{DMK::Address{module_base}, module_size};
        const std::optional<std::int64_t> slot_offset =
            gated_anchor_value(Feature::CrouchedAnimations, AnchorId::AnimIdByCrcSlot);
        const std::optional<std::uintptr_t> vtable = class_vtable(GameClass::AnimationSet);
        const uintptr_t lookup =
            slot_offset.has_value() && vtable.has_value()
                ? read_checked_vtable_slot(*vtable, static_cast<size_t>(*slot_offset) / sizeof(uintptr_t),
                                           Aob::k_animIdByCrcThunk, Aob::k_animIdByCrcThunk.max_match_length(),
                                           module_base, module_size)
                : 0;
        // The anchor proved that the call's target is a function entry in the image before this decode.
        uintptr_t hash_function = 0;
        if (const uintptr_t hash_call = gated_anchor_address(Feature::CrouchedAnimations, AnchorId::AnimNameHashCall);
            hash_call != 0)
        {
            if (const auto callee = DMK::scan::resolve_rip_relative(DMK::Address{hash_call}, 1, 5); callee)
            {
                hash_function = callee->raw();
            }
        }
        if (lookup == 0 || !code_window_matches(hash_function, Aob::k_animNameHashBody,
                                                Aob::k_animNameHashBody.max_match_length(), image))
        {
            logger.warning("Camera: crouched animation lookup not found; crouched right turns can show the game's "
                           "broken pose");
            return;
        }
        using NameHashFunc = std::uint64_t(__fastcall *)(const char *name, std::uint32_t length);
        const auto name_hash = reinterpret_cast<NameHashFunc>(hash_function);
        const auto hash_of = [name_hash](std::string_view name) -> std::uint64_t
        { return name_hash(name.data(), static_cast<std::uint32_t>(name.size())); };
        const std::array<AnimationSwap, 2> swaps{{
            {hash_of(Constants::CROUCHED_IDLE_PLAYER_ANIMATION), hash_of(Constants::CROUCHED_IDLE_NPC_ANIMATION)},
            {hash_of(Constants::CROUCHED_TURN_PLAYER_BLEND_SPACE), hash_of(Constants::CROUCHED_TURN_NPC_BLEND_SPACE)},
        }};
        const std::array<std::uint64_t, 4> hashes{swaps[0].player, swaps[0].npc, swaps[1].player, swaps[1].npc};
        const bool usable =
            std::ranges::none_of(hashes, [](std::uint64_t h) { return h == 0; }) &&
            std::ranges::all_of(hashes, [&hashes](std::uint64_t h) { return std::ranges::count(hashes, h) == 1; });
        if (!usable)
        {
            logger.warning("Camera: crouched animation hashes are not usable; crouched right turns can show the "
                           "game's broken pose");
            return;
        }
        s_crouched_swaps = swaps;
        auto result =
            DMK::hook::mid_at(DMK::hook::MidRequest{.name = "CrouchedAnimation", .target = DMK::Address{lookup}},
                              detour_crouched_animation);
        if (!result.has_value())
        {
            logger.warning("Camera: crouched animation hook failed ({}); crouched right turns can show the game's "
                           "broken pose",
                           result.error().message());
            return;
        }
        const auto armed = hooks.push(std::move(*result)).enable();
        if (!armed.has_value())
        {
            logger.warning("Camera: crouched animation hook could not be armed ({}); crouched right turns can show the "
                           "game's broken pose",
                           armed.error().message());
            return;
        }
        logger.debug("Camera: crouched idle hash {:#x} plays as {:#x}, crouched turn hash {:#x} plays as {:#x}",
                     swaps[0].player, swaps[0].npc, swaps[1].player, swaps[1].npc);
        logger.info("Camera: crouched animations hooked at {} (the crouched idle and turns play the NPC set)",
                    DMK::format::format_address(lookup));
    }

    /**
     * @brief Hooks IsThirdPerson for the native turn-in-place animation, found through the C_Player vtable.
     * @details Best-effort: without it the camera still works and the body follows the look, as in first person.
     *          The install requires all of these:
     *          - both locomotion call sites resolve (answering at only one of them either never turns or freezes the
     *            body).
     *          - the IsThirdPerson slot both call sites use resolves, and its target lies in the game image and still
     *            asks the active camera.
     *          - the camera-changed event (id, target/flags word, HandleEvent slot) resolves, and that slot's target
     *            dispatches the id. Without it a switch-off cannot reach an idle action, whose body then stays free of
     *            the look in first person until the player's next step.
     *          The trampoline pointer is only ever overwritten by a new hook, never cleared: an armed hook, or one a
     *          failed teardown left pinned in a reused image, still routes calls through it. s_native_turn_armed is
     *          what switches the feature on, and only once enable() succeeded.
     */
    static void install_native_turn_hook(uintptr_t module_base, size_t module_size, HookSet &hooks)
    {
        DMK::Logger &logger = DMK::log();
        // A reloaded generation can be handed the previous image with its statics intact; start from off.
        s_native_turn_armed.store(false, std::memory_order_release);
        s_native_turn_actor.store(0, std::memory_order_relaxed);
        s_native_turn_trigger.store(false, std::memory_order_relaxed);
        s_native_turn_release_requested.store(false, std::memory_order_relaxed);
        s_native_turn_changes_in_flight.store(0, std::memory_order_relaxed);
        s_turn_continuing.store(false, std::memory_order_relaxed);
        s_turn_rest_since.store(GetTickCount64(), std::memory_order_relaxed);
        s_turn_decision_tick.store(0, std::memory_order_relaxed);
        s_turn_decision_missed.store(false, std::memory_order_relaxed);
        s_turn_steps_hooked.store(false, std::memory_order_relaxed);
        s_turn_decision_seen.store(false, std::memory_order_relaxed);
        s_turn_tail_frames.store(0, std::memory_order_relaxed);
        s_turn_decided_tick.store(0, std::memory_order_relaxed);
        s_player_animchar.store(0, std::memory_order_relaxed);

        const uintptr_t trigger_return = gated_anchor_address(Feature::NativeTurn, AnchorId::TurnTriggerReturn);
        const uintptr_t lock_sync_return = gated_anchor_address(Feature::NativeTurn, AnchorId::LockSyncReturn);
        if (trigger_return == 0 || lock_sync_return == 0)
        {
            logger.warning("Camera: native turn animation unavailable (locomotion call sites did not resolve: turn "
                           "trigger {}, LockBodyTurn sync {})",
                           DMK::format::format_address(trigger_return), DMK::format::format_address(lock_sync_return));
            return;
        }
        const std::optional<std::uintptr_t> player_vtable = class_vtable(GameClass::Player);
        if (!player_vtable.has_value())
        {
            logger.warning("Camera: native turn animation unavailable (C_Player did not resolve)");
            return;
        }
        // The vtable slots and the event are read from the game's code (see AnchorId::IsThirdPersonSlot onwards).
        const std::optional<std::int64_t> is_third_person_offset =
            gated_anchor_value(Feature::NativeTurn, AnchorId::IsThirdPersonSlot);
        const std::optional<std::int64_t> handle_event_offset =
            gated_anchor_value(Feature::NativeTurn, AnchorId::HandleEventSlot);
        const std::optional<std::int64_t> event_id = gated_anchor_value(Feature::NativeTurn, AnchorId::CameraEventId);
        const std::optional<std::int64_t> event_flags =
            gated_anchor_value(Feature::NativeTurn, AnchorId::CameraEventFlags);
        if (!is_third_person_offset || !handle_event_offset || !event_id || !event_flags)
        {
            logger.warning("Camera: native turn animation unavailable (C_Player's IsThirdPerson or HandleEvent slot, "
                           "or the camera-changed event, did not resolve from the game code)");
            return;
        }
        const size_t is_third_person_slot = static_cast<size_t>(*is_third_person_offset) / sizeof(uintptr_t);
        const size_t handle_event_slot = static_cast<size_t>(*handle_event_offset) / sizeof(uintptr_t);

        const uintptr_t target =
            read_checked_vtable_slot(*player_vtable, is_third_person_slot, Aob::k_isThirdPersonBody,
                                     Aob::k_isThirdPersonBodyWindow, module_base, module_size);
        if (target == 0)
        {
            logger.warning("Camera: native turn animation unavailable (C_Player slot {} is not IsThirdPerson)",
                           is_third_person_slot);
            return;
        }

        // HandleEvent dispatches the event id somewhere in its body. That dispatch, found with the resolved id, ties
        // the slot the sender calls to the function the id was read from.
        const uintptr_t handle_event =
            read_image_vtable_slot(*player_vtable, handle_event_slot, module_base, module_size);
        const bool dispatches =
            handle_event != 0 && dispatches_camera_event(handle_event, static_cast<uint8_t>(*event_id));
        s_player_handle_event = dispatches ? handle_event : 0;
        s_game_object_event_vtable = class_vtable(GameClass::GameObjectEvent).value_or(0);
        if (s_player_handle_event == 0 || s_game_object_event_vtable == 0)
        {
            logger.warning("Camera: native turn animation unavailable (camera-changed event did not resolve: "
                           "HandleEvent slot {} at {}, SGameObjectEvent {})",
                           handle_event_slot, DMK::format::format_address(s_player_handle_event),
                           DMK::format::format_address(s_game_object_event_vtable));
            return;
        }
        s_camera_changed_event_id = static_cast<uint32_t>(*event_id);
        s_camera_changed_event_flags = static_cast<uint32_t>(*event_flags);
        s_lock_body_turn_count_offset = static_cast<ptrdiff_t>(anchor_value(AnchorId::LockBodyTurnCount).value_or(0));

        // Written before the hook arms, so the detour never compares against a half-set pair.
        s_turn_trigger_return = trigger_return;
        s_lock_sync_return = lock_sync_return;

        auto result = DMK::hook::inline_at(
            DMK::hook::InlineRequest{.name = "IsThirdPerson", .target = DMK::Address{target}}, detour_is_third_person);
        if (!result.has_value())
        {
            logger.warning("Camera: IsThirdPerson hook failed ({}); native turn animation unavailable",
                           result.error().message());
            return;
        }
        // Publish the trampoline before enable() arms the patch, so the detour never sees a null original.
        s_is_third_person_original.store(result->original<IsThirdPersonFunc>(), std::memory_order_release);
        const auto armed = hooks.push(std::move(*result)).enable();
        if (!armed.has_value())
        {
            // The detour may still be reachable (a backend error after the patch committed leaves the hook active), so
            // the trampoline stays; the feature simply stays off.
            logger.warning("Camera: IsThirdPerson hook could not be armed ({}); native turn animation unavailable",
                           armed.error().message());
            return;
        }
        s_native_turn_armed.store(true, std::memory_order_release);
        logger.info("Camera: native turn animation ready (IsThirdPerson slot {} at {}; turn trigger {}, LockBodyTurn "
                    "sync {}; camera-changed event {} through HandleEvent slot {})",
                    is_third_person_slot, DMK::format::format_address(target),
                    DMK::format::format_address(trigger_return), DMK::format::format_address(lock_sync_return),
                    s_camera_changed_event_id, handle_event_slot);

        // Lets turns finish facing the look instead of resting 35 degrees short. Best-effort.
        install_turn_decision_hook(trigger_return, hooks);
        install_crouched_animation_hook(module_base, module_size, hooks);
    }

    void orbit_move_pressed() noexcept
    {
        // Taken once: the camera publishes again on its next frame only while no move is under way. The move state
        // below belongs to the camera's thread, so the press acts only when it arrives there. Acquire pairs with the
        // publish's release, so the heading and the thread read below are that frame's.
        const std::uint64_t stamp = s_move_press_stamp_ms.exchange(0, std::memory_order_acquire);
        if (stamp == 0 || GetTickCount64() - stamp > k_move_press_max_age_ms ||
            GetCurrentThreadId() != s_move_press_thread.load(std::memory_order_relaxed))
        {
            return;
        }
        // Third person and free-look must still be on: a view switch or a released hold key since the publish
        // leaves the heading unused.
        CameraState &cam = camera_state();
        if (!cam.applying.load(std::memory_order_relaxed) || !cam.orbit_active.load(std::memory_order_relaxed) ||
            !s_offset_active.load(std::memory_order_relaxed))
        {
            return;
        }
        const float heading = s_move_press_heading.load(std::memory_order_relaxed);
        // The move starts here as the camera starts it, so this frame's rig already holds the camera's world yaw
        // while the look turns under it. The look and the body both turn at the press: the body override sets the
        // facing at the end of the animated-character update, and the look-driven release of the walk start
        // (LockBodyTurn) copies the look into the body one frame later, so an old look turns the body back.
        cam.orbit_target_yaw = heading;
        cam.orbit_target_valid = true;
        cam.orbit_yaw_at_capture_deg = cam.orbit_yaw.load(std::memory_order_relaxed);
        cam.orbit_moving = true;
        // The look jumps to the new heading, so the basis snaps to it rather than easing across the turn. A short
        // press then ends with the rig already on the new heading, and the angle baked on release stays put.
        cam.basis_quat_valid = false;
        (void)apply_orbit_aim_control(0.0f, 0.0f, true, heading);
        const std::uintptr_t anim_char = apply_orbit_body_turn(heading);
        const float turn =
            std::remainder(heading - s_move_press_body_yaw.load(std::memory_order_relaxed), DirectX::XM_2PI);
        if (anim_char != 0 && std::fabs(turn) > k_move_press_min_turn)
        {
            s_move_press_turned_ms.store(GetTickCount64(), std::memory_order_relaxed);
            s_move_press_animchar.store(anim_char, std::memory_order_release);
        }
        (void)DMK::log().try_log(DMK::LogLevel::Trace,
                                 "Orbit: move-orbit START at the press (the body and the look turn to the camera "
                                 "heading {:.1f} deg)",
                                 DMK::math::radians_to_degrees(heading));
    }

    void release_native_turn_animation() noexcept
    {
        // Sequentially consistent with update_native_turn, which counts itself under way before it reads this flag.
        s_native_turn_release_requested.store(true, std::memory_order_seq_cst);
        if (!s_native_turn_armed.load(std::memory_order_seq_cst))
        {
            return; // never armed: nothing was or can be published
        }
        const bool was_active = s_native_turn_actor.load(std::memory_order_seq_cst) != 0;
        // The frustum detour has the action drop its reference on the game thread, because the locomotion action
        // must not be driven from this one. The switch-off is complete when all three of these hold:
        //   - no update_native_turn call is under way: a later call reads the flag set and cannot switch on, and a
        //     call that read it unset has returned;
        //   - no actor is published;
        //   - no change is in flight. The actor is read before the count, and a change is counted before its actor
        //     store, so a switch-off still delivering its event is always seen.
        // The wait is bounded: a minimized or loading game renders no game-view frame. Polls every k_release_poll_ms,
        // first for a game-view frame and then, after a forced switch-off, for an event still in flight.
        constexpr DWORD k_release_poll_ms = 10;
        constexpr int k_release_frame_polls = 40;
        constexpr int k_release_event_polls = 10;
        const auto settled = []() noexcept -> bool
        {
            const bool idle = s_native_turn_updating.load(std::memory_order_seq_cst) == 0;
            const bool no_actor = s_native_turn_actor.load(std::memory_order_seq_cst) == 0;
            return idle && no_actor && s_native_turn_changes_in_flight.load(std::memory_order_seq_cst) == 0;
        };
        for (int i = 0; i < k_release_frame_polls && !settled(); ++i)
        {
            Sleep(k_release_poll_ms);
        }
        if (settled())
        {
            if (was_active)
            {
                (void)DMK::log().try_log(DMK::LogLevel::Info,
                                         "Shutdown: native turn animation switched off (the body follows the look)");
            }
            return;
        }
        // No game-view frame came: stop answering "third person" without the event. Then give a change that is
        // mid-event a moment to return.
        s_native_turn_actor.store(0, std::memory_order_seq_cst);
        for (int i = 0;
             i < k_release_event_polls && s_native_turn_changes_in_flight.load(std::memory_order_seq_cst) != 0; ++i)
        {
            Sleep(k_release_poll_ms);
        }
        (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                 "Shutdown: no game frame switched the native turn animation off; the body follows the "
                                 "look again on the player's next step");
    }

    DMK::Result<void> initialize_camera(uintptr_t module_base, size_t module_size, HookSet &hooks)
    {
        DMK::Logger &logger = DMK::log();

        // Start in first-person (offset off) so the game looks normal on load; the view
        // hotkeys toggle/force the third-person offset on. The hooks below fast-path out
        // while the offset is off, so installing them unconditionally costs almost nothing.
        CameraState &cam = camera_state();
        cam.applying.store(false);
        cam.zoom_offset.store(0.0f);

        // Resolve g_env once (patch-resilient AOB). The CView vtable is identified lazily by RTTI on
        // the first game-view camera, so nothing is resolved here.
        s_genv_runtime = resolve_genv();

        // Resolve the engine ray helper for collision + aim convergence. Best-effort: on a miss
        // those features no-op (the camera still renders), so the result is intentionally discarded.
        (void)initialize_physics_raycast(module_base, module_size, s_genv_runtime);

        // Resolve the 3DEngine render-octree query so the camera can also collide with render-only roofs
        // (tent / awning canopy cloth) that carry no ray-collidable physics. Best-effort: a miss no-ops the
        // roof render clamp, so the result is intentionally discarded.
        (void)initialize_render_occlusion(module_base, module_size, s_genv_runtime);

        // The default hook::Options prologue policy is Fail, which is what this mod wants: refuse the
        // install when the resolved entry leads with a call or breakpoint byte, the shape a cascade
        // mis-resolution or a foreign int3 stub produces. A sibling mod's E9 jump hook decodes as a
        // relocatable branch rather than a refusal, so layering still works.
        //
        // The frustum hook is mandatory and returns the library Error verbatim on a refusal, so the caller
        // sees the real ErrorCode. The head-visibility and input-dispatcher hooks are best-effort WITHIN
        // this unit (each costs one cosmetic/optional feature, not the camera), so they warn in place.

        // Hook the camera frustum builder - the matrix-offset point; without it the feature
        // does nothing. The module-scoped cascade resolves to the function entry inside the game
        // image or returns 0, so no separate bounds check is needed here.
        const uintptr_t frustum_addr = gated_anchor_address(Feature::Camera, AnchorId::Frustum);
        if (frustum_addr == 0)
        {
            return std::unexpected(DMK::Error{DMK::ErrorCode::NoMatch, "camera_hook/frustum_anchor"});
        }
        auto frustum_result = DMK::hook::inline_at(
            DMK::hook::InlineRequest{.name = "CameraFrustumBuild", .target = DMK::Address{frustum_addr}},
            detour_frustum_build);
        if (!frustum_result.has_value())
        {
            return std::unexpected(frustum_result.error());
        }
        // Publish the trampoline and store the handle BEFORE enable() arms the patch. The detour then cannot observe a
        // null original on its first frame, and the set owns a hook whose arm fails with the patch live. The backend
        // allocates the trampoline at install, so it is readable off the still-disabled handle.
        s_frustum_build_original.store(frustum_result->original<FrustumBuildFunc>(), std::memory_order_release);
        if (auto armed = hooks.push(std::move(*frustum_result)).enable(); !armed.has_value())
        {
            return std::unexpected(armed.error());
        }

        // The head-visibility hook is best-effort: if it fails the camera still works,
        // the player just appears headless from behind, so a miss is a warning.
        const uintptr_t head_addr = gated_anchor_address(Feature::HeadVisibility, AnchorId::HeadVisibility);
        if (head_addr == 0)
        {
            logger.warning("Camera: Head visibility cascade unresolved; player may appear headless from behind");
        }
        else
        {
            auto head_result = DMK::hook::inline_at(
                DMK::hook::InlineRequest{.name = "SetHeadVisibility", .target = DMK::Address{head_addr}},
                detour_set_head_visibility);

            if (!head_result.has_value())
            {
                logger.warning("Camera: Head visibility hook failed ({}); player may appear headless from behind",
                               head_result.error().message());
            }
            else
            {
                s_set_head_visibility_original.store(head_result->original<SetHeadVisibilityFunc>(),
                                                     std::memory_order_release);
                if (auto armed = hooks.push(std::move(*head_result)).enable(); !armed.has_value())
                {
                    logger.warning("Camera: Head visibility hook could not be armed ({}); player may appear "
                                   "headless from behind",
                                   armed.error().message());
                }
            }
        }

        // The input-dispatcher hook powers free-look orbit. Best-effort: a miss only
        // disables orbit, the offset camera still works. The detour is inert until the
        // orbit key is held, so it is harmless when free-look is unused.
        const uintptr_t input_addr = gated_anchor_address(Feature::Orbit, AnchorId::InputDispatch);
        if (input_addr == 0)
        {
            logger.warning("Camera: Input dispatcher cascade unresolved; free-look orbit unavailable");
        }
        else
        {
            auto input_result = DMK::hook::inline_at(
                DMK::hook::InlineRequest{.name = "CameraInputDispatch", .target = DMK::Address{input_addr}},
                detour_input_dispatch);

            if (!input_result.has_value())
            {
                logger.warning("Camera: Input dispatcher hook failed ({}); free-look orbit unavailable",
                               input_result.error().message());
            }
            else
            {
                s_input_dispatch_original.store(input_result->original<InputDispatchFunc>(), std::memory_order_release);
                if (auto armed = hooks.push(std::move(*input_result)).enable(); !armed.has_value())
                {
                    logger.warning("Camera: Input dispatcher hook could not be armed ({}); free-look orbit "
                                   "unavailable",
                                   armed.error().message());
                }
            }
        }

        // Keep the AI judging which NPCs to update (and which to hide and pause) from the first-person eye while the
        // third-person offset renders. Best-effort, like the two hooks above.
        install_camera_observer_hook(module_base, module_size, hooks);

        // Let the body play the game's own turn-in-place animations while the third-person view is engaged.
        // Best-effort, like the hooks above.
        install_native_turn_hook(module_base, module_size, hooks);
        // Keeps native turn steps on the spot (with the decision hook above) and drops the stale first step of a
        // free-look move started at the press. After the native turn hook, which resets the state this one sets.
        // Best-effort.
        install_turn_in_place_hook(hooks);

        // Fade the character instead of switching to first person when the camera has no room behind it. Best-effort:
        // without it the first-person switch stays.
        if (auto character = initialize_character_fade(hooks); !character.has_value())
        {
            logger.warning(
                "Camera: character fade unavailable ({}); the camera switches to first person when too close",
                character.error().message());
        }

        logger.info("Camera: Third-person camera hooks installed");
        return {};
    }

    void refresh_zoom_binding_tokens() noexcept
    {
        try
        {
            const DMK::input::Input &input = DMK::input::Input::instance();
            s_zoom_tokens.store(std::make_shared<const ZoomBindingTokens>(ZoomBindingTokens{
                                    .zoom_in = input.acquire_token(k_zoom_in_binding),
                                    .zoom_out = input.acquire_token(k_zoom_out_binding),
                                }),
                                std::memory_order_release);
        }
        catch (...)
        {
            // An allocation failure leaves no tokens published, so the render thread falls back to the name-based
            // query and never reads a stale pair.
            s_zoom_tokens.store(nullptr, std::memory_order_release);
        }
    }

    void release_zoom_binding_tokens() noexcept
    {
        s_zoom_tokens.store(nullptr, std::memory_order_release);
    }

} // namespace TPVCamera
