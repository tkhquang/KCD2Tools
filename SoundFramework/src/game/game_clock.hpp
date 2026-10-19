/**
 * @file game/game_clock.hpp
 * @brief The clock voice lines play on: real time while the game runs, stopped while it is paused.
 *
 * The game's speech is on a bus that pauses with the game, and its own lip-sync animations follow the game clock, which
 * stops in the pause menu and slows to a near stop (1/1000) in the inventory. Framework faces and subtitles advance on
 * this clock so they stay with the voice across those screens. Main-thread state.
 */
#ifndef SNDFW_GAME_GAME_CLOCK_HPP
#define SNDFW_GAME_GAME_CLOCK_HPP

#include <cstdint>

namespace SoundFramework::game::game_clock
{
    /**
     * @brief Reads the engine timer at gEnv (@p genv) + 0x80, checked against @p timer_vtable; without both the clock
     *        falls back to the wall clock.
     * @note Setup/control-plane only.
     */
    void configure(std::uintptr_t genv, std::uintptr_t timer_vtable) noexcept;

    /** @brief Advances the clock by one engine frame. Once per frame on the main thread, before its readers. */
    void tick() noexcept;

    /**
     * @brief How far the clock moved this frame, in milliseconds: the frame's real duration while the game clock
     *        runs (slow motion included, as the voice does not slow down), 0 while it is paused or nearly frozen.
     */
    [[nodiscard]] double frame_ms() noexcept;
} // namespace SoundFramework::game::game_clock

#endif // SNDFW_GAME_GAME_CLOCK_HPP
