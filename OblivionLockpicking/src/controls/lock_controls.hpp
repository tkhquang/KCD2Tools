/**
 * @file controls/lock_controls.hpp
 * @brief The lock's controls, read from the engine's input events while a lock is in play.
 *
 * The first play frame of a lock attaches the engine input listener (game/input_listener.hpp). The listener detaches
 * itself at its first event after the play frames stop, when the lock ends or the game pauses, so it costs nothing
 * outside lockpicking. While the lock is live, it consumes the events of the bound move, set and push keys, so those
 * keys do nothing else in the game. The play frames and the engine's input dispatch both run on the game's main thread.
 */
#ifndef OBLIVIONLP_CONTROLS_LOCK_CONTROLS_HPP
#define OBLIVIONLP_CONTROLS_LOCK_CONTROLS_HPP

#include "controls/bindings.hpp"

namespace OblivionLockpicking::controls
{
    /** @brief One play frame's controls. */
    struct FrameInputs
    {
        /// Horizontal deflection of the bound stick, -1 to 1, right positive.
        float stick_x{0.0f};
        /// Vertical deflection of the bound stick, -1 to 1, up positive.
        float stick_y{0.0f};
        /// True when a move-left key went down since the previous frame.
        bool move_left{false};
        /// True when a move-right key went down since the previous frame.
        bool move_right{false};
        /// True while a move-left key is down.
        bool move_left_held{false};
        /// True while a move-right key is down.
        bool move_right_held{false};
        /// True when a push key went down since the previous frame.
        bool push{false};
        /// True while a push key is down.
        bool push_held{false};
        /// True when a set key went down since the previous frame.
        bool set{false};
    };

    /**
     * @brief Attaches the listener when it is not attached, keeps the lock live and returns the controls since the
     *        previous call.
     * @note Main thread only, once per play frame.
     */
    [[nodiscard]] FrameInputs take_frame(const Bindings &bindings) noexcept;

    /**
     * @brief Asks the listener to detach, which the main thread does at its next input event or play frame.
     * @details The request stays until keep_listener() withdraws it, and take_frame() returns no input meanwhile.
     * @return True once the listener is detached, so this module can unload.
     * @note Any thread.
     */
    [[nodiscard]] bool release_listener() noexcept;

    /**
     * @brief Withdraws the release request, so the next play frame attaches the listener again.
     * @details For a shutdown that left the hooks armed and the mod running.
     * @note Any thread.
     */
    void keep_listener() noexcept;
} // namespace OblivionLockpicking::controls

#endif // OBLIVIONLP_CONTROLS_LOCK_CONTROLS_HPP
