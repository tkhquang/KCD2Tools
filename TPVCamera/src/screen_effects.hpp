/**
 * @file screen_effects.hpp
 * @brief Hides the game's screen effects per view, as [Camera] HideScreenEffectsTPV / HideScreenEffectsFPV list them.
 *
 * The game paints some effects onto the screen as if it were the player's eyes: the helmet's visor bars, the hit
 * flashes, the eyelids and water drops of washing, the sleepy eyelids and the blood of a head wound. They look wrong
 * while the camera shows the player from outside, and some players want them gone in first person too, so each view
 * has its own list. A group is switched off while the current view's list names it and back on otherwise. Every lever
 * is one the game itself uses:
 *   - The visor and the hit flashes are layers of Libs/UI/Overlay.gfx. C_UIOverlay::SetAlpha sets a layer's image
 *     alpha to 0 and back to 100. The object arrives as the `this` of the anchored HideOverlays / ShowOverlays hooks.
 *   - The eyelids, the wash and the head blood are screen particles. The particle manager's enable-by-name slot turns
 *     them off and on, as the game's own HideOverlays and ShowOverlays do.
 *   - The water drops are the WaterDroplets post effect, held at 0 while the game asks for drops.
 */
#ifndef TPVCAMERA_SCREEN_EFFECTS_HPP
#define TPVCAMERA_SCREEN_EFFECTS_HPP

#include <cstdint>
#include <string_view>

namespace TPVCamera
{
    /** @brief The groups the HideScreenEffects lists name, one bit each. */
    enum class ScreenEffect : std::uint8_t
    {
        Helmet,    ///< The helmet's visor bars (Overlay.gfx layer 0).
        Hit,       ///< The hit flashes: blood, head hit, blackout and vignette (layers 3..6).
        Wash,      ///< Washing's eyelids (professions.washing.close_eyes) and water drops (WaterDroplets).
        Sleepy,    ///< The tired eyelids (WH_Particels.other.eye_close / eye_open / eye_blink).
        HeadBlood, ///< The blood of a head wound (collisions.combat.blood_head).
        Count,
    };

    /** @brief The mask bit of @p effect. */
    [[nodiscard]] constexpr std::uint32_t screen_effect_bit(ScreenEffect effect) noexcept
    {
        return 1u << static_cast<std::uint32_t>(effect);
    }

    /**
     * @brief Parses a comma-separated HideScreenEffectsTPV / HideScreenEffectsFPV list (Helmet, Hit, Wash, Sleepy,
     *        HeadBlood; any case) into ScreenEffect bits.
     * @details "None" and an empty list give 0. An unknown token is skipped with a warning.
     */
    [[nodiscard]] std::uint32_t parse_screen_effect_mask(std::string_view csv);

    /**
     * @brief Takes the game's C_UIOverlay from a HideOverlays / ShowOverlays call.
     * @details The object is kept once its vtable proves the class: its HideOverlays and ShowOverlays slots must hold
     *          the anchored functions and its SetAlpha slot must hold Proof::k_uiOverlaySetAlpha. A repeat call with
     *          the same object returns at once.
     * @note Main thread (the overlay hooks' detours).
     */
    void capture_ui_overlay(void *ui_overlay) noexcept;

    /**
     * @brief Turns the hidden screen particles off again after the game's ShowOverlays turned them all back on.
     * @note Main thread (the ShowOverlays detour, after the original ran).
     */
    void reassert_screen_effects() noexcept;

    /**
     * @brief Hides the groups the current view's list names (HideScreenEffectsTPV while @p third_person, else
     *        HideScreenEffectsFPV) and shows every other group.
     * @details Calls the game only on a change: when the view or a list changes, when a hidden overlay layer gets a
     *          new clip, and while the game asks for water drops with Wash hidden.
     * @param genv The game's gEnv, for the particle manager and the 3D engine.
     * @note Main thread, every game-view frame (the frustum build).
     */
    void update_screen_effects(std::uintptr_t genv, bool third_person) noexcept;
} // namespace TPVCamera

#endif // TPVCAMERA_SCREEN_EFFECTS_HPP
