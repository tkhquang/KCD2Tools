/**
 * @file sim/tumbler_sim.hpp
 * @brief The Oblivion tumbler lock as a pure, deterministic state machine.
 *
 * The lock has MAX_TUMBLERS tumblers under a pick that slides across them. A push of the pick pops the tumbler above
 * it, which rises, pauses at the top and falls back. A set while it is all the way up latches it. A set at any other
 * moment is a wrong set, which the caller turns into the game's own pick failure.
 *
 * Every push from rest rolls a speed as the original Oblivion's LockPickMenu does. The roll picks one of three bands,
 * never the band of the push before, and a random place inside the band. The lock's speed, its difficulty against the
 * player's skill, scales all three. A tumbler launches up and slows a little as it rises. It pauses at the top for
 * 5 ms (the fastest) to 300 ms (the slowest), then falls back from rest under gravity, slower than it rose.
 *
 * A push before a tumbler lands keeps its speed, as in the Remaster, so the player can catch a missed tumbler on the
 * way down. A held push does that automatically and bounces the tumbler off the pick a third of the way down. The
 * rhythm between the push and the click at the top tells the speeds apart, so a player can work a lock by ear.
 *
 * Nothing here touches the game. Durations are in seconds, positions in tumbler units, and the caller supplies the
 * input edges and the skill. The same seed gives the same lock and the same sequence of rolls.
 */
#ifndef OBLIVIONLP_SIM_TUMBLER_SIM_HPP
#define OBLIVIONLP_SIM_TUMBLER_SIM_HPP

#include <array>
#include <cstdint>

namespace OblivionLockpicking::sim
{
    /// Tumblers of the Oblivion lock.
    inline constexpr int MAX_TUMBLERS = 5;

    /** @brief Where one tumbler is in its push cycle. */
    enum class PinPhase : std::uint8_t
    {
        Down,
        Rising,
        Top,
        Falling,
        Set,
    };

    /**
     * @brief How fast a push turned out, for the sounds: a slow tumbler clicks later and lower than a fast one.
     * @details The speed comes from where the roll landed, not from its band, so on a slow lock every band is Slow.
     */
    enum class Speed : std::uint8_t
    {
        Slow,
        Fast,
        Fastest,
    };

    /**
     * @brief The range of Oblivion's per-lock speed factor over the skill: 1 is a Very Hard lock at low Security, 0.1 a
     *        Very Easy one.
     */
    struct Tuning
    {
        /// The lock speed at skill 0.
        float speed_no_skill{1.0f};
        /// The lock speed at skill 1.
        float speed_full_skill{0.1f};
    };

    /** @brief Keep every set tumbler after a wrong set. */
    inline constexpr int KEEP_ALL_ON_FAIL = -1;

    /** @brief One lock, as the caller derives it from the game. */
    struct LockSetup
    {
        /// Tumblers the player must set, 1..MAX_TUMBLERS. The others start set.
        int pins_to_set{1};
        /// Skill against this lock, 0..1.
        float skill{0.0f};
        /// Multiplier on every duration.
        float time_scale{1.0f};
        /// Seed of the preset tumblers and of every roll.
        std::uint32_t seed{0};
        /// Tumblers the player set that stay up after a wrong set (the earliest set first), or KEEP_ALL_ON_FAIL.
        int keep_on_fail{KEEP_ALL_ON_FAIL};
        /// True when a set on a tumbler at rest pushes it. False does nothing, as in Oblivion.
        bool set_pushes_resting{false};
        /// True when the pick stays under its tumbler while any tumbler moves, as in Oblivion.
        bool one_at_a_time{false};
        /**
         * @brief Seconds after a tumbler leaves the top during which a set still counts, in real time.
         * @details The key press and the screen each lag a frame or two behind the lock. 0 is Oblivion's strict rule.
         */
        float set_grace{0.0f};
        /// The lock speed range.
        Tuning tuning{};
    };

    /** @brief Input of one step. */
    struct Inputs
    {
        /// Pick movement in tumbler units, positive to the right.
        float pick_delta{0.0f};
        /// A push edge, the pick moved up.
        bool push{false};
        /**
         * @brief True while the push is held: a push key down, the stick up, or the mouse still on its way up.
         * @details A held push pushes the tumbler under the pick from rest and catches it again at its speed whenever
         *          it falls back to the pick, so it bounces off the top again and again.
         */
        bool push_held{false};
        /// A set edge.
        bool set{false};
    };

    /** @brief What happened during one step. */
    struct StepResult
    {
        /// True when a tumbler started to rise, from rest or again on its way down.
        bool pushed{false};
        /// True when a tumbler reached the top of its chamber, which is when it clicks.
        bool reached_top{false};
        /// The speed of the tumbler that reached the top. A slow one clicks lower than a fast one.
        Speed top_speed{Speed::Fast};
        /// True when a tumbler left the top and started to fall.
        bool dropped{false};
        /// The speed of the tumbler that started to fall. A slow one slides down, a fast one snaps.
        Speed drop_speed{Speed::Fast};
        /// True when a tumbler fell back to rest.
        bool landed{false};
        /// True when a tumbler was set.
        bool set{false};
        /// Seconds after its pause that the set landed, inside the grace. Negative for a set at the top.
        float set_late{-1.0f};
        /// True when a set landed on a tumbler that was not all the way up.
        bool wrong_set{false};
        /// The phase of the tumbler of the wrong set, for the log.
        PinPhase wrong_phase{PinPhase::Down};
        /// Seconds into that phase, for the log.
        float wrong_time{0.0f};
        /// The height of that tumbler, for the log.
        float wrong_height{0.0f};
        /// True once every tumbler is set, until the next reset().
        bool solved{false};
        /// True while an unset tumbler is off the bottom, which is when the lock makes noise.
        bool moving{false};
    };

    /** @brief A read-only view of one tumbler for drawing. */
    struct PinView
    {
        PinPhase phase{PinPhase::Down};
        /// 0 at rest, 1 at the top, and a little above while it pauses there, as Oblivion draws it.
        float height{0.0f};
    };

    /** @brief The tumbler lock. A default-constructed lock is solved and empty, so call reset() first. */
    class TumblerSim
    {
    public:
        /** @brief Starts a fresh lock: presets the extra tumblers and centers the pick. */
        void reset(const LockSetup &setup) noexcept;

        /**
         * @brief Advances the lock by @p dt seconds and applies one input.
         * @details The tumblers advance first, then the pick moves, then a set and then a push act on the tumbler under
         *          the pick, so the inputs always see the state drawn for this frame.
         */
        [[nodiscard]] StepResult step(float dt, const Inputs &inputs) noexcept;

        /** @brief Pick position in tumbler units, 0..MAX_TUMBLERS-1. */
        [[nodiscard]] float pick_position() const noexcept { return m_pick; }

        /** @brief The tumbler under the pick. */
        [[nodiscard]] int selected() const noexcept;

        /** @brief Tumblers still to set. */
        [[nodiscard]] int remaining() const noexcept;

        /** @brief Tumbler @p index for drawing. An index out of range reads as a tumbler at rest. */
        [[nodiscard]] PinView pin(int index) const noexcept;

        /** @brief The speed tumbler @p index moves at, for tests and logs. */
        [[nodiscard]] Speed speed(int index) const noexcept;

        /** @brief True once every tumbler is set. */
        [[nodiscard]] bool solved() const noexcept { return m_solved; }

    private:
        /** @brief One tumbler. */
        struct Pin
        {
            PinPhase phase{PinPhase::Down};
            /// The speed class of the last roll.
            Speed speed{Speed::Fast};
            /// Seconds in the current phase. In the rise, it counts from the launch, the push jump included.
            float time{0.0f};
            /// Launch speed in tumbler heights per second of Oblivion time. It slows by a third of itself per second.
            float launch{0.0f};
            /**
             * @brief How far above the top the tumbler sits while it pauses.
             * @details Oblivion's last frame of the rise carries it past the top. The drop back to the top at the end
             *          of the pause shows that the set window closed.
             */
            float overshoot{0.0f};
            /// TimeScale of the lock. Oblivion time is this phase time divided by it.
            float scale{1.0f};
            /// Seconds of the rise.
            float rise{0.0f};
            /// Seconds of the pause at the top.
            float hang{0.0f};
            /// Seconds of the fall.
            float fall{0.0f};
            /// True for a tumbler that started set.
            bool preset{false};
            /// Order in which the player set it, for the keep-on-fail rule. 0 when the player did not set it.
            int set_order{0};
        };

        /** @brief The phase changes one tumbler went through during one advance. */
        struct Transitions
        {
            /// True when the tumbler reached the top.
            bool reached_top{false};
            /// True when the tumbler left the top.
            bool dropped{false};
            /// True when the tumbler fell back to rest.
            bool landed{false};
        };

        [[nodiscard]] float next_unit() noexcept;
        [[nodiscard]] static float height_of(const Pin &pin) noexcept;
        /** @brief Advances one tumbler, and carries a long step through several phases. */
        [[nodiscard]] Transitions advance(Pin &pin, float dt) noexcept;
        void push(Pin &pin) noexcept;
        void roll(Pin &pin) noexcept;
        void fail() noexcept;
        void refresh_solved() noexcept;

        std::array<Pin, MAX_TUMBLERS> m_pins{};
        LockSetup m_setup{};
        float m_pick{0.0f};
        std::uint32_t m_rng{1};
        /// The band of the last roll on any tumbler. The next roll takes one of the other two. -1 before the first.
        int m_last_band{-1};
        int m_set_counter{0};
        bool m_solved{true};
    };
} // namespace OblivionLockpicking::sim

#endif // OBLIVIONLP_SIM_TUMBLER_SIM_HPP
