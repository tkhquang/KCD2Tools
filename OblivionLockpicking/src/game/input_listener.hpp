/**
 * @file game/input_listener.hpp
 * @brief The mod's engine input listener: every input event, on the game's main thread, before the action maps.
 *
 * The listener sits in the input system's console listener set (gEnv->pInput, CDXInput slots 3 and 4). The engine
 * notifies that set before the exclusive listener and the regular listeners (the action maps, the Flash UI, the
 * hardware mouse), and a listener that returns true stops the event there. The engine reads the set without a lock
 * while it dispatches, so attach and detach only from the game's main thread. A detach from inside the handler is
 * safe, because the set defers the removal until its dispatch ends.
 */
#ifndef OBLIVIONLP_GAME_INPUT_LISTENER_HPP
#define OBLIVIONLP_GAME_INPUT_LISTENER_HPP

#include <cstdint>
#include <string_view>

namespace OblivionLockpicking::game
{
    /** @brief SInputEvent::state, a 32-bit field whose values fit a byte. */
    enum class InputState : std::uint8_t
    {
        Pressed = 1,
        Released = 2,
        /// An analog value moved: a stick, a trigger or the mouse.
        Changed = 8,
    };

    /** @brief The fields of one engine input event the mod reads. */
    struct InputEvent
    {
        /// The kind of change.
        InputState state{InputState::Changed};
        /// The engine's key name: "mouse1", "d", "xi_a", "pad_cross", "xi_thumbrx".
        std::string_view name;
        /// Stick deflection from -1 to 1, trigger travel from 0 to 1, mouse travel, or 1 and 0 for a button.
        float value{0.0f};
    };

    /** @brief Called for each event while attached, on the main thread. A true return consumes the event. */
    using InputHandler = bool (*)(const InputEvent &event) noexcept;

    /**
     * @brief Adds the listener with @p handler to the console listener set.
     * @return False when the input system is unavailable or is not the expected CDXInput.
     * @note Main thread only, outside the engine's input dispatch.
     */
    [[nodiscard]] bool attach_input_listener(InputHandler handler) noexcept;

    /**
     * @brief Removes the listener through the input system and slot that the attach used.
     * @details The removal needs no bound anchors, so it also works after game::unbind().
     * @note Main thread only. Safe from inside the handler.
     */
    void detach_input_listener() noexcept;
} // namespace OblivionLockpicking::game

#endif // OBLIVIONLP_GAME_INPUT_LISTENER_HPP
