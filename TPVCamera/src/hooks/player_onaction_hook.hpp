/**
 * @file player_onaction_hook.hpp
 * @brief Hooks the player OnAction / global action dispatcher and latches movement-input intent.
 *
 * Hooks the action dispatcher (sub_1808EBEE4, the C++ source of Lua Player:OnAction), which fires for
 * every action-map action with its name and post-action-map value. Latching the movement-input magnitude
 * lets the orbit move-detection key on it instead of body-position speed: the input is nonzero the instant a
 * movement key is pressed and stays nonzero while a key is held even when a wall arrests the body, so the
 * camera-relative heading is not falsely released on a collision stop. Both devices, all directions: keyboard
 * digital "moveforward/moveback/moveleft/moveright" and the gamepad left-stick analog axes (xi_movey /
 * xi_movex, with movement_y / movement_x aliases).
 *
 * Best-effort: a pattern miss leaves the feature off (player_onaction_available() returns false) and the
 * move-detection falls back to the body's horizontal speed, so the camera still works.
 */
#ifndef TPVCAMERA_PLAYER_ONACTION_HOOK_HPP
#define TPVCAMERA_PLAYER_ONACTION_HOOK_HPP

#include <cstddef>
#include <cstdint>

namespace TPVCamera
{

/**
 * @brief Installs the player OnAction / action-dispatcher hook.
 * @return true if the dispatcher was located and hooked.
 */
[[nodiscard]] bool initialize_player_onaction_hook(uintptr_t module_base, size_t module_size);

/** @brief Whether the OnAction hook resolved (callers use the input signal only when true). */
[[nodiscard]] bool player_onaction_available();

/**
 * @brief Largest movement-input magnitude across all directions and devices, >= 0.
 * @details The maximum |value| latched across the movement actions: ~1.0 while moving, 0 when released, and
 *          nonzero while a key is held against a wall (intent over speed). Drives the orbit move-detection
 *          latch (so it engages for forward, strafe and reverse alike). Returns 0 when the hook is unavailable.
 */
[[nodiscard]] float player_onaction_move_magnitude();

/**
 * @brief Force-clears every latched movement magnitude to 0 and returns the largest value that was set.
 * @details The latch is normally cleared only by a matching value==0 release event. On a combat action-map
 *          swap (drawing a weapon, entering a minigame) that release can be SWALLOWED, stranding a slot > 0 so
 *          the orbit move-detection re-trips with the keys released -- the camera then keeps turning the body
 *          to a heading no input is driving (the post-combat self-rotation). Callers invoke this on the edges
 *          where a strand can occur (orbit toggle-off, orbit-exclude entry, TPV disengage) to drop the stale
 *          latch. The returned magnitude lets the caller log whether the latch WAS stranded (compare against
 *          the move-stop threshold). Touches only the relaxed atomics, so it is safe from any thread.
 * @return The largest magnitude that was latched at the moment of the reset (0 if all slots were clear).
 */
float player_onaction_reset();

} // namespace TPVCamera

#endif // TPVCAMERA_PLAYER_ONACTION_HOOK_HPP
