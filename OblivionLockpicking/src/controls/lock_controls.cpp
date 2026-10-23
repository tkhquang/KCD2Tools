/**
 * @file controls/lock_controls.cpp
 * @brief Key states from the engine's input events, the consumption of the bound keys, and the listener's lifetime.
 */

#include "controls/lock_controls.hpp"
#include "game/input_listener.hpp"

#include <DetourModKit.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace OblivionLockpicking::controls
{
    namespace
    {
        /// The lock stays live this long after a play frame, because a pause or the end of the lock stops the frames.
        constexpr std::uint64_t LIVE_MS = 200;
        /// Travel at which an analog control bound as a key, such as a trigger, counts as held.
        constexpr float KEY_DOWN_TRAVEL = 0.5f;

        [[nodiscard]] std::uint64_t now_ms() noexcept
        {
            return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                  std::chrono::steady_clock::now().time_since_epoch()
            )
                                                  .count());
        }

        /** @brief Which names hold each entry of a key list down, and whether one went down since the last take. */
        struct KeyStates
        {
            /// One bit per name of the entry (KeyMatch::source) that is held.
            std::array<std::uint8_t, KEY_LIST_CAPACITY> held{};
            /// True when an entry went down since the last take.
            bool pressed{false};

            /** @brief True while any entry is held. */
            [[nodiscard]] bool any_held() const noexcept
            {
                return std::any_of(held.begin(), held.end(), [](std::uint8_t bits) { return bits != 0; });
            }

            /** @brief Records that the name @p match names went @p down or up. */
            void apply(KeyMatch match, bool down) noexcept
            {
                std::uint8_t &bits = held[static_cast<std::size_t>(match.entry)];
                const bool was_down = bits != 0;
                const auto bit = static_cast<std::uint8_t>(1u << match.source);
                bits = down ? static_cast<std::uint8_t>(bits | bit) : static_cast<std::uint8_t>(bits & ~bit);
                pressed = pressed || (!was_down && bits != 0);
            }
        };

        /** @brief What the listener gathered for the play frames. Main thread only. */
        struct State
        {
            /// The bindings the key states belong to.
            Bindings bindings;
            /// Press state of each key of Bindings::move_left_keys.
            KeyStates move_left;
            /// Press state of each key of Bindings::move_right_keys.
            KeyStates move_right;
            /// Press state of each key of Bindings::set_keys.
            KeyStates set;
            /// Press state of each key of Bindings::push_keys.
            KeyStates push;
            /// Horizontal deflection of the bound stick.
            float stick_x{0.0f};
            /// Vertical deflection of the bound stick.
            float stick_y{0.0f};
            /// The steady-clock time in milliseconds until which the lock counts as live.
            std::uint64_t live_until_ms{0};
        };

        State s_state;
        /// True once the input system refused the listener, which it also refuses on a later frame.
        bool s_unavailable = false;
        /// True while the listener is attached. release_listener() reads it from the module's shutdown thread.
        std::atomic<bool> s_attached{false};
        /// True once shutdown asked the main thread to detach the listener.
        std::atomic<bool> s_release_requested{false};

        void detach() noexcept
        {
            game::detach_input_listener();
            s_attached.store(false, std::memory_order_release);
        }

        bool on_event(const game::InputEvent &event) noexcept
        {
            State &state = s_state;
            if (s_release_requested.load(std::memory_order_acquire) || now_ms() > state.live_until_ms)
            {
                detach();
                return false;
            }
            const StickAxes axes = stick_axes(state.bindings.pad_stick);
            if (event.state == game::InputState::Changed && !axes.x.empty())
            {
                if (same_key(axes.x, event.name))
                {
                    state.stick_x = event.value;
                    return false;
                }
                if (same_key(axes.y, event.name))
                {
                    state.stick_y = event.value;
                    return false;
                }
            }

            bool down = false;
            switch (event.state)
            {
            case game::InputState::Pressed:
                down = true;
                break;
            case game::InputState::Released:
                down = false;
                break;
            case game::InputState::Changed:
                down = event.value >= KEY_DOWN_TRAVEL;
                break;
            default:
                return false;
            }
            bool bound = false;
            if (const KeyMatch match = find_key(state.bindings.move_left_keys, event.name); match.entry >= 0)
            {
                state.move_left.apply(match, down);
                bound = true;
            }
            if (const KeyMatch match = find_key(state.bindings.move_right_keys, event.name); match.entry >= 0)
            {
                state.move_right.apply(match, down);
                bound = true;
            }
            if (const KeyMatch match = find_key(state.bindings.set_keys, event.name); match.entry >= 0)
            {
                state.set.apply(match, down);
                bound = true;
            }
            if (const KeyMatch match = find_key(state.bindings.push_keys, event.name); match.entry >= 0)
            {
                state.push.apply(match, down);
                bound = true;
            }
            return bound;
        }
    } // namespace

    FrameInputs take_frame(const Bindings &bindings) noexcept
    {
        State &state = s_state;
        if (s_release_requested.load(std::memory_order_acquire))
        {
            if (s_attached.load(std::memory_order_relaxed))
            {
                detach();
            }
            return FrameInputs{};
        }
        if (!s_attached.load(std::memory_order_relaxed) && !s_unavailable)
        {
            state = State{
                .bindings = bindings,
            };
            if (game::attach_input_listener(&on_event))
            {
                s_attached.store(true, std::memory_order_release);
            }
            else
            {
                s_unavailable = true;
                (void)DMK::log().try_log(
                    DMK::LogLevel::Warning,
                    "Controls: the game's input system is unavailable, so only the mouse works"
                );
            }
        }
        else if (!(state.bindings == bindings))
        {
            // The held states belong to the old key lists.
            state.bindings = bindings;
            state.move_left = KeyStates{};
            state.move_right = KeyStates{};
            state.set = KeyStates{};
            state.push = KeyStates{};
        }
        state.live_until_ms = now_ms() + LIVE_MS;

        const FrameInputs inputs{
            .stick_x = state.stick_x,
            .stick_y = state.stick_y,
            .move_left = state.move_left.pressed,
            .move_right = state.move_right.pressed,
            .move_left_held = state.move_left.any_held(),
            .move_right_held = state.move_right.any_held(),
            .push = state.push.pressed,
            .push_held = state.push.any_held(),
            .set = state.set.pressed,
        };
        state.move_left.pressed = false;
        state.move_right.pressed = false;
        state.push.pressed = false;
        state.set.pressed = false;
        return inputs;
    }

    bool release_listener() noexcept
    {
        s_release_requested.store(true, std::memory_order_release);
        return !s_attached.load(std::memory_order_acquire);
    }

    void keep_listener() noexcept
    {
        s_release_requested.store(false, std::memory_order_release);
    }
} // namespace OblivionLockpicking::controls
