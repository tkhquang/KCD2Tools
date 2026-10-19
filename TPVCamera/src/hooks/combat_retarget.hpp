/**
 * @file hooks/combat_retarget.hpp
 * @brief Moves the combat lock-on to another hostile after a kill, through the game's own set-opponent.
 *
 * After a kill the lock-on keeps the dead opponent until the stance ends, seconds later, and its own pick takes only a
 * hostile nearer than the body meanwhile. The camera chooses the next opponent (request_combat_retarget). The detour
 * sets it right after the combat player's own update, on the thread that runs it, by the same set-opponent call the
 * controller makes when it picks a target itself. That call sets the combat target. The game moves the lock-on to it
 * once its own switch rule allows, which holds it until the attack under way (the killing blow or the finishing
 * move) ends.
 */
#ifndef TPVCAMERA_HOOKS_COMBAT_RETARGET_HPP
#define TPVCAMERA_HOOKS_COMBAT_RETARGET_HPP

#include <DetourModKit/hook.hpp>

#include <cstddef>
#include <cstdint>

namespace TPVCamera
{
    /**
     * @brief Hooks C_CombatPlayer's update and resolves the game's set-opponent (Feature::CombatRetarget).
     * @details Best-effort: when the set-opponent anchor, the combat player's vtable or its update slot does not prove
     *          out, nothing is installed and requests are dropped.
     * @note Setup only, after resolve_all_anchors() and init_game_types().
     */
    void install_combat_retarget_hook(std::uintptr_t module_base, std::size_t module_size, DMK::hook::HookStack &hooks);

    /**
     * @brief Sets the standing request: the lock-on moves to the actor @p human after each combat player update while
     *        the stance holds a dead opponent and the actor still lives (read_combat_retarget).
     * @details An @p id of 0 withdraws it. A target an earlier request set, and the game has not taken yet, then goes
     *          back to the held dead opponent.
     * @param human The actor's C_Human, as read from the lock-on's list.
     * @param id The actor's EntityId, or 0 for none.
     * @param hold_end_until While a move stands, the game clock (1/100000 s) the kill's end-of-stance timer must not
     *        run out before (defer_kill_end_timer).
     * @note Any thread. The camera rewrites it every frame.
     */
    void request_combat_retarget(std::uintptr_t human, std::uint32_t id, std::int64_t hold_end_until) noexcept;

    /**
     * @brief The EntityId a request last set as the combat target, 0 when none since the last call.
     * @details The call repeats every update while the request stands. The game ignores a repeat of the current target.
     * @note Any thread. A read clears it.
     */
    [[nodiscard]] std::uint32_t take_combat_retarget_done() noexcept;
} // namespace TPVCamera

#endif // TPVCAMERA_HOOKS_COMBAT_RETARGET_HPP
