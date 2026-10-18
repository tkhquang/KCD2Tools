/**
 * @file hooks/native_turn_hooks.cpp
 * @brief The native turn-in-place animation hooks (see native_turn_hooks.hpp).
 */

#include "hooks/native_turn_hooks.hpp"
#include "aob_resolver.hpp"
#include "config.hpp"
#include "constants.hpp"
#include "game_state.hpp"
#include "game_structures.hpp"
#include "global_state.hpp"
#include "hooks/vtable_slots.hpp"
#include "math_utils.hpp"
#include "offset_heal.hpp"
#include "rtti_types.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <optional>

namespace TPVCamera
{
    namespace
    {
        // IActor::IsThirdPerson (C_Player vtable, the slot AnchorId::IsThirdPersonSlot reads from its call sites):
        // bool __fastcall(actor /*rcx*/). One function shared by every actor class; it answers true for any actor that
        // is not the local player.
        using IsThirdPersonFunc = bool(__fastcall *)(uintptr_t actor);

        // IGameObjectExtension::HandleEvent (C_Player vtable, the slot AnchorId::HandleEventSlot reads from the
        // camera-changed event's sender): void __fastcall(extension /*rcx*/, const SGameObjectEvent *event /*rdx*/).
        using HandleEventFunc = void(__fastcall *)(uintptr_t extension, const void *event);

        // CAnimatedCharacter::UpdatePhysicalEntityMovement: char __fastcall(animchar /*rcx*/, QuatT *movement /*rdx*/).
        // The frame's movement is a rotation (x, y, z, w) followed by a translation (x, y, z). The translation is
        // requested from physics as a velocity and the rotation is composed into the body.
        using UpdatePhysEntMovementFunc = char(__fastcall *)(uintptr_t animchar, float *movement);
        std::atomic<IsThirdPersonFunc> s_is_third_person_original{nullptr};
        std::atomic<UpdatePhysEntMovementFunc> s_phys_ent_movement_original{nullptr};

        // Native turn-in-place animation. The game is kept in first person, where the body follows the look and
        // C_PlayerMovementAction (the free-roam locomotion action) never plays its turn fragments. The body follows the
        // look while the player's LockBodyTurn count (C_Player vtable slot 135) is 0, and the action holds one
        // LockBodyTurn reference in third person. The IsThirdPerson detour answers "third person" for
        // s_native_turn_actor (0 = off) at exactly two of that action's call sites:
        //   - the idle LockBodyTurn sync (s_lock_sync_return), which then takes the action's reference so the body stops
        //     following the look;
        //   - the turn trigger (s_turn_trigger_return, only while s_native_turn_trigger is set), after which the game turns
        //     once the look leads the body by more than 35 degrees. The turn-decision hook below replaces that decision
        //     with its own start angle and with turns that finish facing the look.
        // Both are required: the trigger alone never frees the body, and the sync alone freezes it with no turn to
        // follow. Every other caller keeps the first-person answer, among them the first-person view effects (bow-draw
        // zoom and aim sensitivity, the horse camera), the pose aligner and the animation-event audio. The return
        // addresses are written once at install, before the hook arms. The actor and the trigger flag are published by
        // the frustum detour each game-view frame. s_native_turn_armed is set only once the hook is armed. The
        // trampoline pointer itself is never cleared, because an armed (or pinned) hook still routes calls through it.
        uintptr_t s_turn_trigger_return = 0;
        uintptr_t s_lock_sync_return = 0;
        std::atomic<bool> s_native_turn_armed{false};
        std::atomic<uintptr_t> s_native_turn_actor{0};
        std::atomic<bool> s_native_turn_trigger{false};
        // C_Player::HandleEvent, the SGameObjectEvent vtable and the camera-changed event's id and target/flags word,
        // all resolved and validated at install. The feature is not armed without them: a switch-off cannot reach an
        // idle action, which then keeps its reference (body not following the look).
        uintptr_t s_player_handle_event = 0;
        uintptr_t s_game_object_event_vtable = 0;
        uint32_t s_camera_changed_event_id = 0;
        uint32_t s_camera_changed_event_flags = 0;
        // C_Player's LockBodyTurn reference-count offset (AnchorId::LockBodyTurnCount), read only for the log. 0 when
        // it did not resolve.
        ptrdiff_t s_lock_body_turn_count_offset = 0;
        // Set by release_native_turn_animation() during shutdown: the frustum detour stops reporting third person and
        // has the action drop its reference, on the game thread.
        std::atomic<bool> s_native_turn_release_requested{false};
        // update_native_turn calls under way, counted on entry before the release flag is read and uncounted on every
        // return. Once shutdown has set the flag and then sees none under way, no call that read the flag unset is
        // left.
        std::atomic<int> s_native_turn_updating{0};
        // A change being published and its camera-changed event, counted from BEFORE the actor store until the event
        // has returned. Shutdown reads the actor and then this count, so it cannot remove the hooks under an event in
        // flight.
        std::atomic<int> s_native_turn_changes_in_flight{0};

        // Turn decision: the mid hook on ComputeMoveState's `seta cl` (see resolve_turn_decision_layout). On its own
        // the game starts a turn past 35 degrees and stops it as soon as the gap is back under 35, so the body always
        // rests about 35 degrees short of the look. The hook starts a turn past s_turn_start_angle (NativeTurnAngle,
        // radians), or past k_turn_settle_angle once the look has rested for s_turn_settle_delay
        // (NativeTurnSettleDelay). It then keeps the turn on until the body is within k_turn_finish_angle of the look,
        // or until the gap changes sign (he faces it). s_turn_continuing, s_turn_last_sign and the rest state carry the
        // decision from frame to frame. Only the hook updates them, on whichever job worker updates the player that
        // frame, one frame after another; a feature switch in update_native_turn only resets them. The hook runs on
        // every idle frame with a gap, so a player frame without it (s_turn_decision_missed, raised by the movement
        // detour) means the action left its idle decision (he walked, an interaction or a script took over, turns were
        // off) and the decision starts over. Breaks are counted in frames, so a long frame is not a break. Without the
        // movement detour the break falls back to wall-clock time: more than k_turn_decision_break_ms since
        // s_turn_decision_tick. The look counts as resting while the gap stays within k_turn_rest_drift of its value
        // when the rest began. While his body holds still only the look changes the gap, and a cumulative bound (not a
        // per-frame rate) means a slow pan never counts as rest. A turn that sets the game's spin latch is refused for
        // that frame (see would_set_spin_latch). One frame later the game holds the new sign, and the turn can start.
        // The action's field offsets the hook reads are resolved from the game's code at install and written before the
        // hook arms.
        constexpr float k_turn_finish_angle = 0.07f; // rad (4 deg)
        constexpr float k_turn_settle_angle = 0.21f; // rad (12 deg)
        constexpr float k_turn_rest_drift = 0.0175f; // rad (1 deg)
        constexpr uint64_t k_turn_decision_break_ms = 150;
        std::atomic<float> s_turn_start_angle{0.61086524f};
        std::atomic<float> s_turn_settle_delay{0.8f}; // seconds, 0 = no settle turns
        std::atomic<bool> s_turn_continuing{false};
        std::atomic<float> s_turn_last_sign{0.0f};
        std::atomic<float> s_turn_rest_gap{0.0f};
        std::atomic<uint64_t> s_turn_rest_since{0};
        std::atomic<uint64_t> s_turn_decision_tick{0};
        std::atomic<bool> s_turn_decision_missed{false};
        std::atomic<bool> s_turn_steps_hooked{false}; // the movement detour is armed: breaks are counted in frames
        ptrdiff_t s_spin_latch_offset = 0;
        ptrdiff_t s_installed_state_offset = 0;
        ptrdiff_t s_last_sign_offset = 0;

        // Turn steps in place. A turn fragment's root motion walks the body 5-20 cm, which leaves a still camera off
        // centre (and shifts a camera that follows it). The movement detour drops the horizontal part of the player's
        // movement on the frames the movement action is in its idle/turn decision, from the first turn frame through a
        // tail after the last one: a turn cut short at the look still sends a step, for at most one frame, while the
        // game blends to idle. The tail lasts k_turn_step_tail_frames decision frames or k_turn_step_tail_ms, whichever
        // is longer, so neither a long frame nor a high frame rate cuts it short, and it ends on the first frame
        // without a decision. s_turn_decision_seen is set by the decision hook and taken by the movement detour, which
        // CAnimatedCharacter's update calls right after the action's update on the same thread, so the flag is exact
        // per frame: the frame he starts walking, or anything else takes the action over, his movement goes through
        // untouched. s_turn_decided_tick is when the hook last chose to turn (GetTickCount64, 0 = tail ended).
        // s_player_animchar is the player's CAnimatedCharacter, published by the frustum detour while the native turn
        // is on (0 otherwise). s_movement_type_offset is CAnimatedCharacter's movement request type, resolved at
        // install before the detour arms.
        constexpr int k_turn_step_tail_frames = 3;
        constexpr uint64_t k_turn_step_tail_ms = 300;
        std::atomic<bool> s_turn_decision_seen{false};
        std::atomic<int> s_turn_tail_frames{0};
        std::atomic<uint64_t> s_turn_decided_tick{0};
        std::atomic<uintptr_t> s_player_animchar{0};
        ptrdiff_t s_movement_type_offset = 0;

        // Crouched animations (see Constants::CROUCHED_IDLE_PLAYER_ANIMATION). The game looks a clip's animation up by
        // the 64-bit hash of its name. The hook on that lookup (detour_crouched_animation) swaps the hash of each
        // player clip for its NPC twin while the native turn is on. The hashes come from the game's own name hash at
        // install, and the install writes them before the hook arms.
        struct AnimationSwap
        {
            std::uint64_t player = 0;
            std::uint64_t npc = 0;
        };
        std::array<AnimationSwap, 2> s_crouched_swaps{};

        // Set by a press that turned the body by more than k_move_press_min_turn: the next movement request of that
        // animated character (detour_phys_ent_movement) was built along the old facing. Stamped (GetTickCount64) so a
        // stale one is ignored; the stamp is stored before the character.
        std::atomic<std::uint64_t> s_move_press_turned_ms{0};
        std::atomic<std::uintptr_t> s_move_press_animchar{0};

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
        bool send_camera_changed_event(uintptr_t c_player) noexcept
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
        int32_t read_lock_body_turn_count(uintptr_t c_player)
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
        bool read_body_and_look_yaw(uintptr_t c_player, float &body_yaw, float &look_yaw)
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
            const auto yaw =
                DMK::memory::read<float>(DMK::Address{*controller + Constants::LOOK_CONTROLLER_YAW_OFFSET});
            if (!world || !yaw || !std::isfinite(*yaw))
            {
                return false;
            }
            body_yaw = std::atan2(-world->m[0][1], world->m[1][1]);
            look_yaw = *yaw;
            return true;
        }

        /**
         * @brief IsThirdPerson detour: answers "third person" to the free-roam locomotion action for the native turn.
         * @details Hot (every actor, several calls a frame each), so it only compares: the published player first, then
         *          the caller's return address against the two resolved call sites. DMK's counted thunk calls this
         *          detour, so _ReturnAddress() points into the thunk, and @p frame carries the game caller's return address.
         *          Every other call, every other actor's included, goes to the original unchanged. Reads nothing
         *          through a pointer, so it needs no SEH frame.
         */
        bool __fastcall detour_is_third_person(DMK::hook::CallerFrame frame, uintptr_t actor) noexcept
        {
            if (actor != 0 && actor == s_native_turn_actor.load(std::memory_order_relaxed))
            {
                const uintptr_t ret = reinterpret_cast<uintptr_t>(frame.return_address());
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
        bool would_set_spin_latch(uintptr_t action, float sign) noexcept
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
        void detour_turn_decision(DMK::hook::MidContext &ctx) noexcept
        {
            const uintptr_t actor = DMK::hook::gpr(ctx, DMK::hook::Gpr::Rbx);
            if (actor == 0 || actor != s_native_turn_actor.load(std::memory_order_relaxed))
            {
                return;
            }
            if (DMK::hook::gpr(ctx, DMK::hook::Gpr::R12) == 0)
            {
                // UpdatePending: the action is queued behind another one that moves him, and runs ComputeMoveState
                // without the turn-angle output pointer it keeps in r12. Left to the game, and not an idle decision.
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

            // A started turn continues until he faces the look. Otherwise a turn starts past the start angle, or past
            // the settle angle after the look rested.
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
         * @brief Installs the turn-decision mid hook on the site resolve_turn_decision_layout proves around the
         * trigger.
         * @details Best-effort: without it the native turn still works, with the game's own 35 degrees and the body
         *          resting about 35 degrees short of the look. The layout fixes the registers the hook reads (rbx, xmm13,
         *          rdi, r12), the instruction it sits on, and the action's spin-latch field offsets, which are written
         *          before the hook arms.
         */
        void install_turn_decision_hook(uintptr_t trigger_return, DMK::hook::HookStack &hooks)
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
        bool drop_horizontal_step(uintptr_t animchar, float *movement) noexcept
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
         * @brief Movement detour: keeps a native turn step on the spot, and drops the stale first step of a free-look
         * move
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
        char __fastcall detour_phys_ent_movement(uintptr_t animchar, float *movement) noexcept
        {
            // A compare first, so the many other characters cost no locked instruction; the exchange then takes the
            // press once even when the dispatcher arms a new one meanwhile.
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
                        (void)DMK::log().try_log(
                            DMK::LogLevel::Trace,
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
         * @brief Crouched animations mid hook on CAnimationSet::GetAnimIDByCRC (see
         *        Constants::CROUCHED_IDLE_PLAYER_ANIMATION).
         * @details While the native turn is on, a lookup of the player's crouched idle or crouched turn blend space
         * gets
         *          its NPC twin, so the idle and the turns share one stance. The hook runs at the entry of the vtable
         *          slot's thunk, with the name hash in rdx. Every character's lookups pass through it, so it only compares.
         *          Any other hash goes to the game unchanged, and so does any lookup while the native turn is off (first
         *          person, or the feature switched off). A clip looks its animation up when it starts, so a crouched clip
         *          started in third person keeps that animation.
         */
        void detour_crouched_animation(DMK::hook::MidContext &ctx) noexcept
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
        void install_crouched_animation_hook(uintptr_t module_base, size_t module_size, DMK::hook::HookStack &hooks)
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
            if (const uintptr_t hash_call =
                    gated_anchor_address(Feature::CrouchedAnimations, AnchorId::AnimNameHashCall);
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
                {hash_of(Constants::CROUCHED_TURN_PLAYER_BLEND_SPACE),
                 hash_of(Constants::CROUCHED_TURN_NPC_BLEND_SPACE)},
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
                logger.warning(
                    "Camera: crouched animation hook could not be armed ({}); crouched right turns can show the "
                    "game's broken pose",
                    armed.error().message());
                return;
            }
            logger.debug("Camera: crouched idle hash {:#x} plays as {:#x}, crouched turn hash {:#x} plays as {:#x}",
                         swaps[0].player, swaps[0].npc, swaps[1].player, swaps[1].npc);
            logger.info("Camera: crouched animations hooked at {} (the crouched idle and turns play the NPC set)",
                        DMK::format::format_address(lookup));
        }
    } // namespace

    void update_native_turn(uintptr_t c_player, bool engaged, uint32_t state, bool eye_anchored, bool turns_allowed,
                            float delta_time)
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
        const bool excluded = (state & state_policy().native_turn_exclude()) != 0;
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

    void install_turn_in_place_hook(DMK::hook::HookStack &hooks)
    {
        DMK::Logger &logger = DMK::log();
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
        auto result = DMK::hook::inline_at<&detour_phys_ent_movement>(DMK::hook::InlineRequest{
            .name = "UpdatePhysicalEntityMovement",
            .target = DMK::Address{target},
        });
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

    void install_native_turn_hook(uintptr_t module_base, size_t module_size, DMK::hook::HookStack &hooks)
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

        auto result = DMK::hook::inline_at<&detour_is_third_person>(DMK::hook::InlineRequest{
            .name = "IsThirdPerson",
            .target = DMK::Address{target},
        });
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

    bool native_turn_engaged() noexcept
    {
        return s_native_turn_actor.load(std::memory_order_relaxed) != 0;
    }

    void note_move_press_turn(std::uintptr_t animchar) noexcept
    {
        s_move_press_turned_ms.store(GetTickCount64(), std::memory_order_relaxed);
        s_move_press_animchar.store(animchar, std::memory_order_release);
    }

    void clear_move_press_turn() noexcept
    {
        s_move_press_animchar.store(0, std::memory_order_relaxed);
        s_move_press_turned_ms.store(0, std::memory_order_relaxed);
    }
} // namespace TPVCamera
