/**
 * @file clock.hpp
 * @brief The clock's frame: follow the HUD, read the time, and keep the movie's text, style and place current.
 */
#ifndef HUDCLOCK_CLOCK_HPP
#define HUDCLOCK_CLOCK_HPP

#include <cstdint>

namespace HUDClock::clock
{
    /**
     * @brief One frame of the clock.
     * @note Main thread, once per frame, from the frame hook. No-throw.
     */
    void run_frame() noexcept;

    /// The clock's keys.
    enum class Key : std::uint8_t
    {
        /// ToggleKey flips the clock between staying up and hidden. It starts as AlwaysShow says.
        Toggle,
        /// ShowKey shows the clock for ShowSeconds.
        Show,
    };

    /**
     * @brief Records a key edge. The next frame acts once the key was held for its HoldTimeInSeconds.
     * @param down True on the press edge, false on the release edge.
     * @note Any thread.
     */
    void key_changed(Key key, bool down) noexcept;

    /**
     * @brief Asks the next frame to hide the clock and to stop drawing it, for a teardown.
     * @note Control plane.
     */
    void request_stop() noexcept;

    /**
     * @brief True once a frame after request_stop() has hidden the clock.
     * @note Control plane.
     */
    [[nodiscard]] bool stopped() noexcept;

    /**
     * @brief Forgets the frame state, so a later start finds the element and sends the movie everything again.
     * @note Control plane, after the frame hook retired.
     */
    void reset() noexcept;
} // namespace HUDClock::clock

#endif // HUDCLOCK_CLOCK_HPP
