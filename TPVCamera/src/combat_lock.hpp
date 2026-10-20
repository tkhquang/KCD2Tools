/**
 * @file combat_lock.hpp
 * @brief The player's combat lock-on as the game's combat module holds it: stance, opponent, free look and timers.
 *
 * The game's lock-on (wh::combatmodule::C_CombatPlayerController) picks an opponent, holds the combat stance and steers
 * the player's look toward the opponent every frame. The camera reads it so that it does not fight that steer, and
 * logs each change so the lock-on's behavior can be followed in a fight. Every link is read under DetourModKit's
 * fault guard and every class is checked by its RTTI, so a layout drift or a level unload reads as "not resolved".
 * The chain and the fields are documented with their constants in layout_offsets.hpp.
 */
#ifndef TPVCAMERA_COMBAT_LOCK_HPP
#define TPVCAMERA_COMBAT_LOCK_HPP

#include "constants.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace TPVCamera
{
    /**
     * @struct CombatLock
     * @brief One frame's reading of the player's lock-on.
     * @details Headings are world yaws in the engine convention (forward = (-sin yaw, cos yaw)), in radians.
     */
    struct CombatLock
    {
        /// The combat chain resolved and every class matched. Otherwise the other fields hold their defaults.
        bool resolved{false};
        /// The combat stance (the combat-mode model property of the player's combat actor).
        bool stance{false};
        /// The selected opponent's combat actor, 0 for none. An identity for change detection, never dereferenced.
        std::uintptr_t opponent{0};
        /// The opponent's EntityId, 0 when there is no opponent or it did not read.
        std::uint32_t opponent_id{0};
        /// The combat target's EntityId (the opponent-to-be the game's pick, a block or the retarget set, which its
        /// commit moves into the lock), 0 for none or when it did not read.
        std::uint32_t target_id{0};
        /// The opponent is dead (C_Actor::IsDead), set by the killing blow.
        bool opponent_dead{false};
        /// The opponent's health (its soul's, or the actor's own without a soul), NaN when it did not read.
        float opponent_health{std::numeric_limits<float>::quiet_NaN()};
        /// The player's and the opponent's entity positions read, so the four fields below hold.
        bool geometry{false};
        /// The opponent's entity position (his feet), world meters.
        std::array<float, 3> opponent_position{};
        /// Heading from the player to the opponent.
        float opponent_heading{0.0f};
        /// Ground distance from the player to the opponent, meters.
        float opponent_distance{0.0f};
        /// The player's body heading (the entity's forward), the heading the game's unlock test measures from.
        float body_heading{0.0f};
        /// Mouse free look: the steer toward the opponent is paused while the mouse turns the view.
        bool free_mouse{false};
        /// Gamepad free look, the right-stick twin of free_mouse.
        bool free_pad{false};
        /// The game's hard lock (Tab, see COMBAT_CONTROLLER_HARD_LOCK_OFFSET).
        bool hard_lock{false};
        /// Game-clock deadline of the controller's delayed-unlock timer, COMBAT_TIMER_UNARMED when unarmed.
        std::int64_t unlock_deadline{Constants::COMBAT_TIMER_UNARMED};
        /// Game-clock deadline of the controller's end-of-stance-after-a-kill timer, COMBAT_TIMER_UNARMED when unarmed.
        std::int64_t end_deadline{Constants::COMBAT_TIMER_UNARMED};

        /**
         * @brief True when the lock-on has an opponent selected.
         * @details Outside the stance the controller re-selects every frame whichever hostile the view points at and
         *          locks on to it, which raises the stance. Inside the stance it steers the look toward the opponent.
         *          Either way the game is about to turn, or is turning, the player toward that opponent.
         */
        [[nodiscard]] constexpr bool has_opponent() const noexcept { return resolved && opponent != 0; }
    };

    /**
     * @brief Reads the player's lock-on from the combat module.
     * @param c_player The local C_Player, or 0 when not resolved. When set, the combat actor must belong to it.
     * @return The reading. resolved is false when any link or class check fails.
     * @note Main thread only.
     */
    [[nodiscard]] CombatLock read_combat_lock(std::uintptr_t c_player) noexcept;

    /**
     * @brief Turns the lock-on's own view to @p heading and keeps its pitch.
     * @details Once the lock is on, the combat input stops following the player's look and drives the look and the body
     *          from a view of its own (see COMBAT_INPUT_VIEW_* in layout_offsets.hpp). Its steer turns that view toward
     *          the opponent, and mouse free look adds to it. The next steer or free-look frame therefore turns back a
     *          look and body that the camera turned, unless that view turns with them.
     * @param c_player The local C_Player, or 0 when not resolved.
     * @param heading World yaw in radians (forward = (-sin, cos)).
     * @return True when every field was written. False, with nothing or part written, when a link or check failed.
     * @note Main thread only.
     */
    [[nodiscard]] bool set_combat_view_heading(std::uintptr_t c_player, float heading) noexcept;

    /**
     * @brief Clears the controller's end-of-stance-after-a-kill timer.
     * @details The kill arms it, and the controller clears it only when it ends the stance. When the lock moves to a
     *          living opponent before that (request_combat_retarget), the expired timer stays behind and ends the stance
     *          on the living opponent the next time the controller's other end condition holds.
     * @param c_player The local C_Player, or 0 when not resolved.
     * @return True when the timer was armed and is now cleared.
     * @note Main thread only.
     */
    bool clear_kill_end_timer(std::uintptr_t c_player) noexcept;

    /**
     * @struct CombatCandidate
     * @brief One combat actor the lock-on can pick, as the player sees it.
     */
    struct CombatCandidate
    {
        /// The candidate's EntityId, 0 when it did not read.
        std::uint32_t id{0};
        /// The candidate's C_Human, to read him again by (read_combat_actor).
        std::uintptr_t human{0};
        /// The candidate is dead (C_Actor::IsDead).
        bool dead{false};
        /// Ground distance from the player, meters.
        float distance{0.0f};
        /// Heading from the player to the candidate (world yaw, forward = (-sin, cos)).
        float heading{0.0f};
    };

    /**
     * @brief Reads the combat actors the lock-on picks from (the controller's selector list).
     * @param c_player The local C_Player, or 0 when not resolved.
     * @param out Receives up to out.size() candidates whose positions read.
     * @return The number written.
     * @note Main thread only.
     */
    [[nodiscard]] std::size_t read_combat_candidates(std::uintptr_t c_player, std::span<CombatCandidate> out) noexcept;

    /**
     * @brief Reads a combat actor found earlier (read_combat_candidates) again, whether the lock-on lists him or not.
     * @details The C_Human must still carry @p id and a CEntity, so one freed or reused since does not read as him.
     * @param c_player The local C_Player, or 0 when not resolved.
     * @param human The actor's C_Human, as read before.
     * @param id The actor's EntityId, as read before.
     * @return The actor as he is now, or std::nullopt when he does not read as that actor any more.
     * @note Main thread only.
     */
    [[nodiscard]] std::optional<CombatCandidate> read_combat_actor(std::uintptr_t c_player, std::uintptr_t human,
                                                                   std::uint32_t id) noexcept;

    /**
     * @struct CombatRetarget
     * @brief The lock-on while the stance holds a dead opponent: what the game's set-opponent takes to move it, and the
     *        combat target it holds now. Every entity is a CEntity, 0 for none.
     */
    struct CombatRetarget
    {
        /// The player's combat actor, set-opponent's first argument.
        std::uintptr_t combat_player{0};
        /// The dead opponent the stance holds.
        std::uintptr_t held{0};
        /// The combat target's entity: the opponent the game moves the lock-on to once the attack under way ends.
        std::uintptr_t target{0};
        /// The requested actor, 0 when he no longer carries the EntityId asked for or is dead.
        std::uintptr_t next{0};
    };

    /**
     * @brief Reads what a retarget needs while the stance holds a dead opponent.
     * @details Every link is re-read and every class re-checked, so a request from an earlier frame cannot point the
     *          call at a freed object.
     * @param human The requested actor's C_Human, as read from the lock-on's list, or 0 for none.
     * @param id The requested actor's EntityId, or 0 for none.
     * @return The reading, or std::nullopt when the stance does not hold a dead opponent or a read failed.
     * @note Called on the thread that runs the combat player's update, right after it.
     */
    [[nodiscard]] std::optional<CombatRetarget> read_combat_retarget(std::uintptr_t human, std::uint32_t id) noexcept;

    /**
     * @brief Keeps the end-of-stance-after-a-kill timer from running out before @p until.
     * @details The timer and the game's move of the lock-on to the new target both wait for the attack under way to
     *          end. When the timer has run out by then, the update that moves the lock-on also ends the stance on the
     *          new opponent. Only an armed timer due earlier is moved. An unarmed one is left alone.
     * @param until The game clock (1/100000 s) the timer must not run out before.
     * @return True when the timer was moved.
     * @note Called on the thread that runs the combat player's update, right after it.
     */
    bool defer_kill_end_timer(std::int64_t until) noexcept;

    /**
     * @brief Logs at Debug the living candidates other than the opponent, with how far the camera is turned off each.
     * @param lock This frame's reading.
     * @param candidates The candidates read this frame.
     * @param camera_heading The heading of the view the camera rendered last.
     * @note Main thread only.
     */
    void log_combat_candidates(const CombatLock &lock, std::span<const CombatCandidate> candidates,
                               float camera_heading) noexcept;

    /**
     * @brief Logs each change of the lock-on at Debug, one line with every field.
     * @details A timer is reported as the seconds it has left on the game clock when it reads, and only its deadline
     *          counts as a change, so a running timer does not log every frame. The geometry (distance, how far the body
     *          and the camera are turned off the opponent) is reported on each line but never counts as a change.
     * @param lock This frame's reading.
     * @param game_clock The game clock's frame-start stamp in 1/100000 s, 0 when it did not read.
     * @param camera_heading The heading of the view the camera rendered last, when known.
     * @param pick_heading The heading the game's target pick went along last frame instead of the rendered camera's
     *        (hooks/combat_pick.hpp), when it did.
     * @note Main thread only, once per frame.
     */
    void log_combat_lock(const CombatLock &lock, std::int64_t game_clock, std::optional<float> camera_heading,
                         std::optional<float> pick_heading) noexcept;
} // namespace TPVCamera

#endif // TPVCAMERA_COMBAT_LOCK_HPP
