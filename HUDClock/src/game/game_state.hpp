/**
 * @file game/game_state.hpp
 * @brief The game state the clock follows: the HUD parts that show, the world time, the player's minigames and photo
 *        mode.
 */
#ifndef HUDCLOCK_GAME_GAME_STATE_HPP
#define HUDCLOCK_GAME_GAME_STATE_HPP

#include <cstdint>
#include <optional>

namespace HUDClock::game
{
    /**
     * @brief The game's HUD mask: bit N is set while HUD part N (core::HUD_PARTS) shows.
     * @return The bits, or std::nullopt while no HUD exists (the main menu, a loading screen).
     * @note Main thread, where the game sets the mask.
     */
    [[nodiscard]] std::optional<std::uint32_t> hud_part_bits() noexcept;

    /**
     * @brief The calendar's world time, in milliseconds since its start.
     * @details The time the quests and NPC schedules run on. A faked time of day (Calendar.SetFakeTimeOfDay) changes
     *          only the light, so it does not show here.
     * @return The time, or std::nullopt while no game is loaded.
     * @note Main thread, where the calendar advances.
     */
    [[nodiscard]] std::optional<std::int64_t> world_time_ms() noexcept;

    /**
     * @brief The minigames the player is in: bit N is set while the player plays core::MINIGAMES[N].
     * @details Read from the game's minigame manager, which holds a minigame from its start to its end, including while
     *          a menu is open over it. Each object is checked against its RTTI vtable, so a layout this build does not
     *          share reads as no minigame.
     * @return The bits, or std::nullopt while no game is loaded or when the minigame anchors did not resolve.
     * @note Main thread, where minigames start and end.
     */
    [[nodiscard]] std::optional<std::uint32_t> player_minigames() noexcept;

    /**
     * @brief Whether photo mode runs, by the photo mode manager's own flag.
     * @return False while no game is loaded or when the photo mode anchors did not resolve.
     * @note Main thread, where photo mode starts and ends.
     */
    [[nodiscard]] bool photo_mode_active() noexcept;
} // namespace HUDClock::game

#endif // HUDCLOCK_GAME_GAME_STATE_HPP
