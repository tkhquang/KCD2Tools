/**
 * @file core/input/move_input.hpp
 * @brief The movement-input thresholds of camera-relative movement (toggle orbit), and the game's own move input
 *        standing in for the movement-input latch.
 *
 * @details Engine-free (no Windows or DetourModKit header), so the local tests compile it as is.
 */
#ifndef TPVCAMERA_CORE_INPUT_MOVE_INPUT_HPP
#define TPVCAMERA_CORE_INPUT_MOVE_INPUT_HPP

#include <algorithm>
#include <optional>

namespace TPVCamera
{
    // A move starts when the device-agnostic movement-input magnitude (~0..1.4, the action dispatcher's) rises above
    // k_move_input_start from idle, which aligns the heading to the camera once, and ends only below the lower
    // k_move_input_stop (hysteresis, so a momentary dip cannot re-trigger the align). The input stays nonzero while a
    // key is held against a wall, so the heading is not falsely released on a collision stop.
    constexpr float k_move_input_start = 0.15f;
    constexpr float k_move_input_stop = 0.05f;
    // A move carried by the game's own move input ends once that input has stayed below k_move_input_stop this long
    // (seconds), which covers the game's few frames of lag behind the keys.
    constexpr float k_game_move_stop_delay = 0.1f;

    /** @brief The game's own move input standing in for the movement-input latch (step_game_move_stand_in()). */
    struct GameMoveStandIn
    {
        bool active = false;
        /// The seconds the game's move input has stayed below k_move_input_stop while the stand-in is active.
        float idle_seconds = 0.0f;
    };

    /** @brief One step of step_game_move_stand_in(). */
    struct GameMoveStandInStep
    {
        /// The movement-input magnitude the move logic reads.
        float magnitude = 0.0f;
        /// The stand-in started on this step.
        bool started = false;
    };

    /**
     * @brief Reads the movement-input latch, with the game's own move input standing in for a key or stick held
     *        through a reset of the latch.
     * @details player_onaction_reset() clears the latch where the game can swallow a held key's release. A key or a
     *          stick held through that reset sends no new press, so the latch reads 0 until the key is let go or the
     *          stick moves. A stick's other axis can refill the latch with a slight deflection that starts no move, or
     *          with one that falls back to 0 while the stick stays pushed. The game's own move input still shows the
     *          held move. So while no release has been seen since the move logic engaged, a game input above
     *          k_move_input_start starts the stand-in, and the stand-in lasts until that input has stayed below
     *          k_move_input_stop for k_game_move_stop_delay. An unreadable game input counts as idle. A stranded latch is
     *          the reverse (the latch up with the keys released, the game's input idle) and starts no stand-in.
     * @param stand_in The carry-over between steps.
     * @param armed Whether a release (a latch reading below k_move_input_stop) was seen since the move logic engaged.
     * @param latch The latch's magnitude.
     * @param game_move The game's own move input, or std::nullopt when it cannot be read.
     * @param delta_time The step's length in seconds.
     * @return The larger of @p latch and @p game_move while the stand-in is active, else @p latch, and whether the
     *         stand-in started on this step.
     */
    [[nodiscard]] inline GameMoveStandInStep step_game_move_stand_in(GameMoveStandIn &stand_in, bool armed, float latch,
                                                                     std::optional<float> game_move,
                                                                     float delta_time) noexcept
    {
        bool started = false;
        if (!armed && !stand_in.active && game_move && *game_move > k_move_input_start)
        {
            stand_in.active = true;
            stand_in.idle_seconds = 0.0f;
            started = true;
        }
        else if (stand_in.active)
        {
            const bool idle = !game_move || *game_move < k_move_input_stop;
            stand_in.idle_seconds = idle ? stand_in.idle_seconds + delta_time : 0.0f;
            if (stand_in.idle_seconds >= k_game_move_stop_delay)
            {
                stand_in = {};
            }
        }
        return {
            .magnitude = stand_in.active ? std::max(latch, game_move.value_or(0.0f)) : latch,
            .started = started,
        };
    }
} // namespace TPVCamera

#endif // TPVCAMERA_CORE_INPUT_MOVE_INPUT_HPP
