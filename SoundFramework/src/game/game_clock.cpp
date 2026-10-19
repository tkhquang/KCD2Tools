/**
 * @file game/game_clock.cpp
 * @brief The voice clock: the engine timer's real frame time, held while the game clock is stopped.
 */

#include "game/game_clock.hpp"
#include "constants.hpp"

#include <windows.h>

#include <cmath>

namespace SoundFramework::game::game_clock
{
    namespace
    {
        using FrameTimeFn = float(__fastcall *)(void *timer, int which);
        using RealFrameTimeFn = float(__fastcall *)(void *timer);

        /// The game clock counts as stopped below this share of real time (the inventory runs it at 1/1000).
        constexpr float FROZEN_SHARE = 0.1f;

        std::uintptr_t s_genv = 0;
        std::uintptr_t s_timer_vtable = 0;
        std::uint64_t s_last_wall_ms = 0;
        double s_frame_ms = 0.0;
    } // namespace

    void configure(std::uintptr_t genv, std::uintptr_t timer_vtable) noexcept
    {
        s_genv = genv;
        s_timer_vtable = timer_vtable;
        s_last_wall_ms = 0;
        s_frame_ms = 0.0;
    }

    void tick() noexcept
    {
        const std::uint64_t wall = GetTickCount64();
        s_frame_ms = s_last_wall_ms == 0 ? 0.0 : static_cast<double>(wall - s_last_wall_ms);
        s_last_wall_ms = wall;
        if (s_genv == 0 || s_timer_vtable == 0)
        {
            return;
        }
        void *const timer = *reinterpret_cast<void *const *>(s_genv + constants::GENV_TIMER);
        if (timer == nullptr || *static_cast<const std::uintptr_t *>(timer) != s_timer_vtable)
        {
            return;
        }
        void *const *const slots = *static_cast<void *const *const *>(timer);
        const float game =
            reinterpret_cast<FrameTimeFn>(slots[constants::TIMER_GET_FRAME_TIME_SLOT])(timer, constants::TIMER_GAME);
        const float real = reinterpret_cast<RealFrameTimeFn>(slots[constants::TIMER_GET_REAL_FRAME_TIME_SLOT])(timer);
        if (!std::isfinite(game) || !std::isfinite(real) || real <= 0.0f)
        {
            return;
        }
        s_frame_ms = game > real * FROZEN_SHARE ? static_cast<double>(real) * 1000.0 : 0.0;
    }

    double frame_ms() noexcept
    {
        return s_frame_ms;
    }
} // namespace SoundFramework::game::game_clock
