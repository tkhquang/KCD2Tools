/**
 * @file hooks/character_fade.hpp
 * @brief Dithers out the player's character when the third-person camera has no room behind it.
 *
 * A detour on CRenderProxy::Render raises the CLodValue dissolve byte of the parameters the player's render proxy draws
 * with, in the main view only, so the character fades with the engine's own LOD-crossfade dither and keeps its shadow.
 * The camera frame sets how far the character fades each frame.
 */
#ifndef TPVCAMERA_HOOKS_CHARACTER_FADE_HPP
#define TPVCAMERA_HOOKS_CHARACTER_FADE_HPP

#include "hook_set.hpp"

#include <DetourModKit/error.hpp>

#include <cstdint>

namespace TPVCamera
{

    /**
     * @brief Installs the CRenderProxy::Render detour from the CharacterFade anchor.
     * @details Best-effort: on a miss the camera keeps the first-person switch when it is too close.
     * @param hooks The mod's hook set. The handle is stored before its arm.
     * @return An empty value when the hook was installed, else the library Error (ErrorCode::NoMatch when the anchor
     *         did not resolve).
     * @throws std::bad_alloc If the hook name or the hook set cannot allocate.
     * @note Call after resolve_all_anchors().
     */
    [[nodiscard]] DMK::Result<void> initialize_character_fade(HookSet &hooks);

    /** @brief True once the character fade detour is armed. */
    [[nodiscard]] bool character_fade_available() noexcept;

    /**
     * @brief Sets how far the player's character dithers out this frame.
     * @details The detour matches the render proxy whose entity is @p entity and raises its dissolve byte in the main
     *          view for the call. A value older than FADE_STALE_MS is ignored, so the character comes back when the
     *          camera stops calling this. The amount is logged at Debug as it crosses each quarter, and once a second
     *          while the character fades, with the main-view draws the detour raised per frame. Main thread only (the
     *          camera frame).
     * @param entity The player's CEntity, or 0 for none.
     * @param amount 0 (solid) to 1 (almost gone).
     */
    void set_character_fade(std::uintptr_t entity, float amount) noexcept;

} // namespace TPVCamera

#endif // TPVCAMERA_HOOKS_CHARACTER_FADE_HPP
