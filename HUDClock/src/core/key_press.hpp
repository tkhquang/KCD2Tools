/**
 * @file core/key_press.hpp
 * @brief When a key press acts, given the key's hold time.
 *
 * Plain C++ with no game, Windows or DetourModKit dependency, so its tests run without the game.
 */
#ifndef HUDCLOCK_CORE_KEY_PRESS_HPP
#define HUDCLOCK_CORE_KEY_PRESS_HPP

#include <cstdint>

namespace HUDClock::core
{
    /** @brief What a frame does with a new press of a key. */
    enum class PressStep : std::uint8_t
    {
        /// The key is still down and short of its hold time. A later frame decides.
        Wait,
        /// The press acts now.
        Act,
        /// The key came up before its hold time, so the press does nothing.
        Spend,
    };

    /**
     * @brief Decides a new press of a key that must stay down for @p hold_ticks before it acts.
     * @param down_since The press time, or 0 when the key is up again.
     * @param now The current time, in the unit of @p down_since.
     * @param hold_ticks The hold time in that unit. 0 or less acts on the press.
     */
    [[nodiscard]] constexpr PressStep
    step_press(std::int64_t down_since, std::int64_t now, std::int64_t hold_ticks) noexcept
    {
        if (hold_ticks <= 0)
        {
            return PressStep::Act;
        }
        if (down_since == 0)
        {
            return PressStep::Spend;
        }
        return now - down_since >= hold_ticks ? PressStep::Act : PressStep::Wait;
    }
} // namespace HUDClock::core

#endif // HUDCLOCK_CORE_KEY_PRESS_HPP
