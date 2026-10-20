/**
 * @file hooks/combat_pick.hpp
 * @brief Hands the combat lock-on's target pick the camera's choice of direction instead of the rendered camera's.
 *
 * The lock-on picks its opponent along the view camera's forward: a hostile counts when his bearing from the player is
 * within atan2(0.5 m, his distance) of it, and the nearest of those wins (sub_180686A50). The controller runs the pick
 * again on its own while the lock holds, and its commit moves the lock to what the pick chose. The third-person camera
 * breaks that pick twice:
 *   - A block moves the lock to the attacker, and the steer then turns the player at about 80 deg/s. A pick along the
 *     look still finds the first opponent and moves the lock back. While the game steers toward a living opponent, the
 *     pick goes along the line to him instead.
 *   - The lock framing turns the camera 15 to 25 deg off the opponent, past the corridor of a near one. While the
 *     player turns the view and the framing turns the camera, the pick goes along the rig's look before the framing,
 *     as in first person.
 * A mid hook at the pick's read of the view camera hands it that direction.
 */
#ifndef TPVCAMERA_HOOKS_COMBAT_PICK_HPP
#define TPVCAMERA_HOOKS_COMBAT_PICK_HPP

#include <DetourModKit/hook.hpp>

#include <cstdint>

namespace TPVCamera
{
    /**
     * @brief Hooks the target pick's read of the view camera (Feature::CombatPickForward).
     * @details Best-effort: when the site does not resolve, nothing is installed and the pick reads the rendered view.
     * @note Setup only, after resolve_all_anchors().
     */
    void install_combat_pick_forward_hook(DMK::hook::HookStack &hooks);

    /**
     * @brief Sets the horizontal forward (@p x, @p y) the target pick goes along instead of the rendered camera's.
     * @details It lapses on its own shortly after the last call, so a camera that stops building leaves the pick on the
     *          rendered camera again.
     * @note Any thread. The camera calls it every frame the lock-on steers or the lock framing turns the camera.
     */
    void publish_combat_pick_forward(float x, float y) noexcept;

    /**
     * @brief Withdraws the forward publish_combat_pick_forward() set: the pick reads the rendered camera again.
     * @note Any thread.
     */
    void withdraw_combat_pick_forward() noexcept;

    /**
     * @brief How many target picks went along the published forward since the last call.
     * @note Any thread. A read clears it.
     */
    [[nodiscard]] std::uint32_t take_combat_pick_forward_count() noexcept;
} // namespace TPVCamera

#endif // TPVCAMERA_HOOKS_COMBAT_PICK_HPP
