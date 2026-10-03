/**
 * @file render/focus_effect.hpp
 * @brief Focus: darkens and tints everything but the highlighted objects while a group with Focus = true shows.
 *
 * The darkening is the game's own VisualArtifacts post effect (the look of its bane and berserker potions): the
 * screen times a colour tint, blended with the unmodified screen by a mask texture's red channel. The render thread
 * draws that mask from the silhouette mask (engine_silhouette: 1 on the background, 0 on highlighted objects); this
 * module points the effect at it and drives the tint. Every call runs on the main thread.
 */
#ifndef HENRYSENSES_FOCUS_EFFECT_HPP
#define HENRYSENSES_FOCUS_EFFECT_HPP

#include <cstdint>

namespace HenrySenses
{
    /**
     * @brief Applies the focus for this tick.
     * @param strength The fade of the focus groups showing, 0 to 1; 0 releases the effect. A rise eases in over
     *        @p fade_in; a fall (the groups' own fade-out) is followed as it comes.
     * @param darken [Render] FocusDarken, 0 to 1.
     * @param tint [Render] FocusTint as 0xRRGGBB.
     * @param fade_in [Render] FocusFadeIn in seconds.
     * @param now_ms The tick's steady-clock time in milliseconds.
     */
    void
    update_focus_effect(float strength, float darken, std::uint32_t tint, float fade_in, std::int64_t now_ms) noexcept;

    /**
     * @brief Gives the effect's parameters back (white tint, white mask), so nothing stays darkened, and restarts
     *        the fade-in.
     * @details Called when there is no player, at a level change and at shutdown.
     */
    void release_focus_effect() noexcept;

    /** @brief Logs the focus state (the state report). */
    void log_focus_effect_state();

} // namespace HenrySenses

#endif // HENRYSENSES_FOCUS_EFFECT_HPP
