/**
 * @file sim/tumbler_sim.cpp
 * @brief The Oblivion tumbler lock state machine.
 */

#include "sim/tumbler_sim.hpp"

#include <algorithm>
#include <cmath>

namespace OblivionLockpicking::sim
{
    namespace
    {
        /// Longest step the lock integrates at once, so a frame hitch loses at most this much lock time.
        constexpr float MAX_STEP_SECONDS = 0.25f;
        /// Shortest phase duration, so a tiny TimeScale cannot stall the phase loop.
        constexpr float MIN_PHASE_SECONDS = 0.001f;
        /// The xorshift state used when the seed mixes to zero, the one state xorshift cannot leave.
        constexpr std::uint32_t FALLBACK_RNG_STATE = 0x9E3779B9u;
        /**
         * @brief The height (0 at rest, 1 at the top) at which a held push catches a tumbler in its fall.
         * @details The tumbler drops about a third of its travel onto the pick and bounces back up, so a set still
         *          needs the right moment of its arrival at the top.
         */
        constexpr float BOUNCE_HEIGHT = 0.65f;

        [[nodiscard]] constexpr bool is_moving(PinPhase phase) noexcept
        {
            return phase == PinPhase::Rising || phase == PinPhase::Top || phase == PinPhase::Falling;
        }

        [[nodiscard]] constexpr float lerp(float a, float b, float t) noexcept
        {
            return a + (b - a) * t;
        }

        // Oblivion's tumbler motion, in tumbler heights and seconds. The bands start at 0, 0.4 and 0.8 of the lock's
        // speed.
        /// Bands of a roll.
        constexpr int BANDS = 3;
        /// Width of one band, as a share of the lock's speed.
        constexpr float BAND_WIDTH = 0.2f;
        /// Launch speed of a roll of zero.
        constexpr float LAUNCH_BASE = 3.0f;
        /// Launch speed added per unit of roll.
        constexpr float LAUNCH_RANGE = 17.0f;
        /// Floor of the launch speed above LAUNCH_BASE, per unit of lock speed.
        constexpr float LAUNCH_FLOOR = 2.0f;
        /// Deceleration per second, in the rise and in the fall, as a share of the launch speed.
        constexpr float DECELERATION = 1.0f / 3.0f;
        /// Pause at the top for a roll of zero.
        constexpr float HANG_LONGEST = 0.300f;
        /// Pause removed per unit of roll.
        constexpr float HANG_RANGE = 0.295f;
        /// Time a push moves the tumbler along at once, as Oblivion's fixed 47 ms first step after a push.
        constexpr float PUSH_JUMP_SECONDS = 0.047f;
        /// The overshoot at the top is the rise's last speed for one 60 fps frame.
        constexpr float OVERSHOOT_FRAME_SECONDS = 1.0f / 60.0f;
        /// Largest overshoot, as a share of the travel, which keeps the tumbler under the lock's front plate.
        constexpr float OVERSHOOT_MAX = 0.15f;
        /// Rolls from here on sound Fast, halfway between the first two bands at full lock speed.
        constexpr float FAST_ROLL = 0.3f;
        /// Rolls from here on sound Fastest, halfway between the last two bands at full lock speed.
        constexpr float FASTEST_ROLL = 0.7f;
        /// The lowest TimeScale the motion divides by.
        constexpr float MIN_TIME_SCALE = 0.01f;

        /** @brief Height after @p t seconds of a tumbler launched at @p launch, which slows as it rises. */
        [[nodiscard]] float rise_height(float launch, float t) noexcept
        {
            return launch * t - DECELERATION * launch * t * t * 0.5f;
        }

        /** @brief Seconds a tumbler launched at @p launch takes to rise to @p height: rise_height() inverted. */
        [[nodiscard]] float rise_time(float launch, float height) noexcept
        {
            const float deceleration = DECELERATION * launch;
            return (launch - std::sqrt(std::max(launch * launch - 2.0f * deceleration * height, 0.0f))) / deceleration;
        }
    } // namespace

    void TumblerSim::reset(const LockSetup &setup) noexcept
    {
        m_setup = setup;
        m_setup.pins_to_set = std::clamp(setup.pins_to_set, 1, MAX_TUMBLERS);
        m_setup.skill = std::clamp(setup.skill, 0.0f, 1.0f);
        m_setup.time_scale = std::max(setup.time_scale, MIN_TIME_SCALE);

        // The mix gives unrelated locks to neighboring entity ids.
        m_rng = (setup.seed * 2654435761u) ^ 0x5BD1E995u;
        if (m_rng == 0)
        {
            m_rng = FALLBACK_RNG_STATE;
        }

        m_pins = {};
        m_last_band = -1;
        std::array<int, MAX_TUMBLERS> order{};
        for (int i = 0; i < MAX_TUMBLERS; ++i)
        {
            order[static_cast<std::size_t>(i)] = i;
        }
        // Fisher-Yates over the tumbler indices. The head of the permutation starts set.
        for (int i = MAX_TUMBLERS - 1; i > 0; --i)
        {
            const int j = std::min(static_cast<int>(next_unit() * static_cast<float>(i + 1)), i);
            std::swap(order[static_cast<std::size_t>(i)], order[static_cast<std::size_t>(j)]);
        }
        const int preset_count = MAX_TUMBLERS - m_setup.pins_to_set;
        for (int k = 0; k < preset_count; ++k)
        {
            Pin &pin = m_pins[static_cast<std::size_t>(order[static_cast<std::size_t>(k)])];
            pin.phase = PinPhase::Set;
            pin.preset = true;
        }

        m_pick = static_cast<float>(MAX_TUMBLERS - 1) * 0.5f;
        m_set_counter = 0;
        refresh_solved();
    }

    StepResult TumblerSim::step(float dt, const Inputs &inputs) noexcept
    {
        StepResult result{};
        const float step_seconds = std::clamp(dt, 0.0f, MAX_STEP_SECONDS);
        for (Pin &pin : m_pins)
        {
            const Transitions moved = advance(pin, step_seconds);
            if (moved.reached_top)
            {
                result.reached_top = true;
                result.top_speed = pin.speed;
            }
            if (moved.dropped)
            {
                result.dropped = true;
                result.drop_speed = pin.speed;
            }
            result.landed = moved.landed || result.landed;
        }
        const bool any_moving =
            std::any_of(m_pins.begin(), m_pins.end(), [](const Pin &pin) { return is_moving(pin.phase); });
        if (!m_setup.one_at_a_time || !any_moving)
        {
            m_pick = std::clamp(m_pick + inputs.pick_delta, 0.0f, static_cast<float>(MAX_TUMBLERS - 1));
        }

        if (!m_solved)
        {
            const int index = selected();
            Pin &pin = m_pins[static_cast<std::size_t>(index)];
            // The set resolves first: a push in the same frame must not move a tumbler off the top under the latch.
            if (inputs.set)
            {
                // A set just after the pause ended still counts: the player saw the tumbler at the top.
                const bool in_grace = pin.phase == PinPhase::Falling && pin.time <= m_setup.set_grace;
                if (in_grace)
                {
                    result.set_late = pin.time;
                }
                switch (in_grace ? PinPhase::Top : pin.phase)
                {
                case PinPhase::Top:
                    pin.phase = PinPhase::Set;
                    pin.time = 0.0f;
                    pin.set_order = ++m_set_counter;
                    result.set = true;
                    break;
                case PinPhase::Down:
                    // Oblivion and the Remaster only click here. The lenient option pushes instead.
                    if (m_setup.set_pushes_resting)
                    {
                        push(pin);
                        result.pushed = true;
                    }
                    break;
                case PinPhase::Rising:
                case PinPhase::Falling:
                    result.wrong_phase = pin.phase;
                    result.wrong_time = pin.time;
                    result.wrong_height = height_of(pin);
                    fail();
                    result.wrong_set = true;
                    break;
                case PinPhase::Set:
                    break;
                }
            }
            const bool caught = pin.phase == PinPhase::Falling && height_of(pin) <= BOUNCE_HEIGHT;
            const bool held = inputs.push_held && (pin.phase == PinPhase::Down || caught);
            if (!result.wrong_set && !result.pushed &&
                ((inputs.push && (pin.phase == PinPhase::Down || pin.phase == PinPhase::Falling)) || held))
            {
                push(pin);
                result.pushed = true;
            }
            refresh_solved();
        }

        result.solved = m_solved;
        result.moving = std::any_of(m_pins.begin(), m_pins.end(), [](const Pin &pin) { return is_moving(pin.phase); });
        return result;
    }

    int TumblerSim::selected() const noexcept
    {
        return std::clamp(static_cast<int>(std::lround(m_pick)), 0, MAX_TUMBLERS - 1);
    }

    int TumblerSim::remaining() const noexcept
    {
        return static_cast<int>(
            std::count_if(m_pins.begin(), m_pins.end(), [](const Pin &pin) { return pin.phase != PinPhase::Set; })
        );
    }

    PinView TumblerSim::pin(int index) const noexcept
    {
        if (index < 0 || index >= MAX_TUMBLERS)
        {
            return {};
        }
        const Pin &pin = m_pins[static_cast<std::size_t>(index)];
        return PinView{
            .phase = pin.phase,
            .height = height_of(pin),
        };
    }

    Speed TumblerSim::speed(int index) const noexcept
    {
        return index < 0 || index >= MAX_TUMBLERS ? Speed::Fast : m_pins[static_cast<std::size_t>(index)].speed;
    }

    float TumblerSim::next_unit() noexcept
    {
        // xorshift32: small, deterministic and identical on every compiler, which the tests rely on.
        m_rng ^= m_rng << 13;
        m_rng ^= m_rng >> 17;
        m_rng ^= m_rng << 5;
        return static_cast<float>(m_rng >> 8) * (1.0f / 16777216.0f);
    }

    float TumblerSim::height_of(const Pin &pin) noexcept
    {
        switch (pin.phase)
        {
        case PinPhase::Rising:
            // The pick launches it. It slows a little in the rise and reaches the top with speed left.
            return std::clamp(rise_height(pin.launch, pin.time / pin.scale), 0.0f, 1.0f);
        case PinPhase::Top:
            return 1.0f + pin.overshoot;
        case PinPhase::Set:
            return 1.0f;
        case PinPhase::Falling:
        {
            // From rest under the same deceleration: slowly at first, and slower than it rose.
            const float x = std::clamp(pin.time / pin.fall, 0.0f, 1.0f);
            return 1.0f - x * x;
        }
        case PinPhase::Down:
            break;
        }
        return 0.0f;
    }

    TumblerSim::Transitions TumblerSim::advance(Pin &pin, float dt) noexcept
    {
        Transitions moved{};
        // A long step can finish several phases, so carry the remainder into the next one.
        float remaining_time = dt;
        while (remaining_time > 0.0f && is_moving(pin.phase))
        {
            const float duration = pin.phase == PinPhase::Rising ? pin.rise
                                   : pin.phase == PinPhase::Top  ? pin.hang
                                                                 : pin.fall;
            const float left = duration - pin.time;
            if (remaining_time < left)
            {
                pin.time += remaining_time;
                break;
            }
            remaining_time -= left;
            pin.time = 0.0f;
            if (pin.phase == PinPhase::Rising)
            {
                pin.phase = PinPhase::Top;
                moved.reached_top = true;
            }
            else if (pin.phase == PinPhase::Top)
            {
                pin.phase = PinPhase::Falling;
                moved.dropped = true;
            }
            else
            {
                pin.phase = PinPhase::Down;
                moved.landed = true;
            }
        }
        return moved;
    }

    void TumblerSim::roll(Pin &pin) noexcept
    {
        const float lock_speed = lerp(m_setup.tuning.speed_no_skill, m_setup.tuning.speed_full_skill, m_setup.skill);
        // Oblivion keeps one band across every tumbler and takes, with one random bit, one of the two others.
        int band = 0;
        if (m_last_band < 0)
        {
            band = std::min(static_cast<int>(next_unit() * static_cast<float>(BANDS)), BANDS - 1);
        }
        else
        {
            band = static_cast<int>(next_unit() * 2.0f);
            if (band >= m_last_band)
            {
                ++band;
            }
        }
        m_last_band = band;
        const float roll = (2.0f * static_cast<float>(band) + next_unit()) * BAND_WIDTH * lock_speed;
        pin.speed = roll < FAST_ROLL ? Speed::Slow : roll < FASTEST_ROLL ? Speed::Fast : Speed::Fastest;
        pin.launch = std::max(LAUNCH_BASE + LAUNCH_RANGE * roll, LAUNCH_BASE + LAUNCH_FLOOR * lock_speed);
        pin.scale = m_setup.time_scale;
        pin.rise = std::max(rise_time(pin.launch, 1.0f) * pin.scale, MIN_PHASE_SECONDS);
        pin.hang = std::max((HANG_LONGEST - HANG_RANGE * roll) * pin.scale, MIN_PHASE_SECONDS);
        pin.fall = std::max(std::sqrt(2.0f / (DECELERATION * pin.launch)) * pin.scale, MIN_PHASE_SECONDS);
        const float top_speed = pin.launch * (1.0f - DECELERATION * rise_time(pin.launch, 1.0f));
        pin.overshoot = std::min(top_speed * OVERSHOOT_FRAME_SECONDS, OVERSHOOT_MAX);
    }

    void TumblerSim::push(Pin &pin) noexcept
    {
        if (pin.phase == PinPhase::Down)
        {
            roll(pin);
            pin.phase = PinPhase::Rising;
            pin.time = std::min(PUSH_JUMP_SECONDS * pin.scale, pin.rise);
        }
        else if (pin.phase == PinPhase::Falling)
        {
            // Caught on its way down (the Remaster): it rises again from where it is, at its own speed.
            const float height = height_of(pin);
            pin.phase = PinPhase::Rising;
            pin.time = std::min(rise_time(pin.launch, height) * pin.scale, pin.rise);
        }
    }

    void TumblerSim::fail() noexcept
    {
        // The pick slips. Every tumbler in motion drops, the one of the wrong set included.
        for (Pin &pin : m_pins)
        {
            if (is_moving(pin.phase))
            {
                pin.phase = PinPhase::Down;
                pin.time = 0.0f;
            }
        }
        if (m_setup.keep_on_fail == KEEP_ALL_ON_FAIL)
        {
            return;
        }
        // The tumblers set first stay up, up to the kept count. The later ones drop.
        for (Pin &pin : m_pins)
        {
            if (pin.phase == PinPhase::Set && !pin.preset && pin.set_order > m_setup.keep_on_fail)
            {
                pin.phase = PinPhase::Down;
                pin.time = 0.0f;
                pin.set_order = 0;
            }
        }
        // Set orders run from 1 to m_set_counter without a gap, so the survivors keep theirs, and the next set counts
        // on from the last one kept.
        m_set_counter = std::min(m_set_counter, m_setup.keep_on_fail);
    }

    void TumblerSim::refresh_solved() noexcept
    {
        m_solved = remaining() == 0;
    }
} // namespace OblivionLockpicking::sim
