/**
 * @file hooks/native_turn_hooks.hpp
 * @brief The native turn-in-place animation in third person: the IsThirdPerson, turn-decision, turn-step and crouched
 *        animation hooks, and the per-frame switch the camera drives.
 */
#ifndef TPVCAMERA_HOOKS_NATIVE_TURN_HOOKS_HPP
#define TPVCAMERA_HOOKS_NATIVE_TURN_HOOKS_HPP

#include <DetourModKit/hook.hpp>

#include <cstddef>
#include <cstdint>

namespace TPVCamera
{
    // A free-look move press older than this (GetTickCount64 milliseconds) is not used: the camera's own press and the
    // turn-step detour's dropped first step both expire after it.
    constexpr std::uint64_t k_move_press_max_age_ms = 100;

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
    void update_native_turn(uintptr_t c_player, bool engaged, uint32_t state, bool eye_anchored, bool turns_allowed,
                            float delta_time);

    /**
     * @brief Hooks CAnimatedCharacter::UpdatePhysicalEntityMovement to keep turn steps in place and drop the stale
     *        first step of a free-look move started at a press. Best-effort: without it a turn step moves the body
     *        5-20 cm, which the still camera leaves slightly off center, and a free-look move that turns the body
     *        starts with a short step along the old facing.
     * @details Installed on its own: the turn-step part acts only on frames the decision hook flags, so without that
     *          hook it never drops a step, while the press drop needs only this hook.
     */
    void install_turn_in_place_hook(DMK::hook::HookStack &hooks);

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
    void install_native_turn_hook(uintptr_t module_base, size_t module_size, DMK::hook::HookStack &hooks);

    /**
     * @brief Stops the native turn animation, so the body follows the look again, before the hooks are removed.
     * @details The locomotion action keeps the LockBodyTurn reference it took while the mod reported third person, so
     *          unloading without this would leave the body free of the look (not following it) until the player's next
     *          step. The switch-off runs on the game thread, in the next game-view frames; this waits for it, bounded
     *          to about half a second for a game that renders no frame (minimized, loading). Idempotent.
     * @note Call from shutdown while the camera hooks are still installed.
     */
    void release_native_turn_animation() noexcept;

    /** @brief Whether the IsThirdPerson detour currently reports third person for the player (the turn is engaged). */
    [[nodiscard]] bool native_turn_engaged() noexcept;

    /**
     * @brief Notes that a free-look move press turned @p animchar's body, so the turn-step detour drops the stale
     *        first step of its next movement request (built along the old facing).
     * @note Any thread; the stamp is stored before the character, which is released.
     */
    void note_move_press_turn(std::uintptr_t animchar) noexcept;

    /** @brief Drops a noted press turn, when the view leaves third person. */
    void clear_move_press_turn() noexcept;
} // namespace TPVCamera

#endif // TPVCAMERA_HOOKS_NATIVE_TURN_HOOKS_HPP
