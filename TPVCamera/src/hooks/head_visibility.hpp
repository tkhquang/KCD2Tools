/**
 * @file hooks/head_visibility.hpp
 * @brief Keeps the player's head shown from behind while the third-person offset renders, and hidden like in first
 *        person when the camera sits at the eye, the close-up fade has him mostly faded, or the view switch has the
 *        lens at the head.
 */
#ifndef TPVCAMERA_HOOKS_HEAD_VISIBILITY_HPP
#define TPVCAMERA_HOOKS_HEAD_VISIBILITY_HPP

#include "hooks/hook_set.hpp"

#include <cstdint>

namespace TPVCamera
{
    /**
     * @brief Hooks the head-visibility setter. Best-effort: without it the camera still works and the player may
     *        appear headless from behind, so a miss is a warning.
     */
    void install_head_visibility_hook(HookSet &hooks);

    /**
     * @brief Keeps the player head in sync with the offset state once per frame.
     * @details While the offset is active the head must stay shown (hidden only during the first-person fallback,
     *          when the camera sits at the eye, and once the close-up fade has the character mostly dithered out while
     *          the eye fade is not ready, or his face gone while it is, since the stock eyes, eyelashes and eye film
     *          ignore the dither), but the engine only sets head visibility on its own transitions, so re-call the
     *          setter whenever the live hide flag disagrees. When the offset has just turned off, restore the game's
     *          intended hide value exactly once so the first-person rig hides the head again. The hide flag is re-read
     *          rather than cached so this self-heals regardless of who last changed it, and a call is issued only on a
     *          mismatch so the engine is not driven every frame. The player is the live C_Player resolved this frame
     *          when there is one, else the one the setter was last called on. A live player that differs from the
     *          latched one is adopted first (adopt_head_state), because a freshly loaded generation or a new level has
     *          no call latched until the game next switches the view itself.
     * @param active Whether the offset is being applied to the game view this frame.
     * @param c_player The live C_Player (vtable-validated), or 0.
     */
    void reassert_head_visibility(bool active, std::uintptr_t c_player);

    /** @brief Whether the last re-assert ran with the offset active (the head is restored once after it turns off). */
    [[nodiscard]] bool head_was_active() noexcept;

    /**
     * @brief Publishes the first-person fallback: true while the camera has no room behind the player (the arm is
     *        shorter than HeadClearance) and sits at the eye, so the head is hidden like in first person.
     * @note Main thread (the camera frame); read by the head re-assert and the head-visibility detour.
     */
    void set_head_fallback(bool hidden) noexcept;

    /** @brief The first-person fallback set_head_fallback() published. */
    [[nodiscard]] bool head_fallback() noexcept;

    /**
     * @brief Publishes the close-up fade's head hide: true while the character is mostly dithered out and the eye fade
     *        is not ready (the stock Eye shader ignores the dissolve), or with it ready once his face is gone (the
     *        eyelashes and the eye film ignore it too). Unlike the fallback it leaves the camera where it is.
     * @note Main thread (the camera frame); read by the head re-assert and the head-visibility detour.
     */
    void set_head_close_fade(bool hidden) noexcept;

    /** @brief The close-up fade's head hide set_head_close_fade() published. */
    [[nodiscard]] bool head_close_fade() noexcept;

    /**
     * @brief Publishes the view switch's head hide: true while the first-person <-> third-person switch has the lens
     *        within reach of the head, so it is hidden like in first person instead of filling the view from inside.
     * @note Main thread (the camera frame); read by the head re-assert and the head-visibility detour.
     */
    void set_head_transition(bool hidden) noexcept;

    /** @brief The view switch's head hide set_head_transition() published. */
    [[nodiscard]] bool head_transition() noexcept;
} // namespace TPVCamera

#endif // TPVCAMERA_HOOKS_HEAD_VISIBILITY_HPP
