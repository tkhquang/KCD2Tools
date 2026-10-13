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

    struct CharacterNodes;

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
     * @brief Sets how far the player's character, and what it carries, dither out this frame.
     * @details The detour matches the render proxy whose entity is @p entity, and the carried items' render proxies in
     *          @p nodes, and raises their dissolve byte in the main view for the call. That one raise fades a carried
     *          character model (a bow, a crossbow) and a carried static mesh (a quiver, a sword, the nocked arrow)
     *          alike: the proxy hands the same parameters to a character slot and to a static slot, whose
     *          CStatObj::Render passes the byte on to AddOrCreatePersistentRenderObject. A value older than
     *          FADE_STALE_MS is ignored, so the character comes back when the camera stops calling this. The amount is
     *          logged at Debug as it crosses each quarter, and once a second while the character fades, with the
     *          carried set's churn, the walk time and the main-view draws the detour raised per frame for each kind of
     *          node. Main thread only (the camera frame).
     * @param entity The player's CEntity, or 0 for none.
     * @param amount 0 (solid) to 1 (almost gone).
     * @param nodes What the character carries, from collect_character_nodes, or nullptr for nothing; the first
     *        CHARACTER_FADE_MAX_CARRIED items are kept.
     */
    void set_character_fade(std::uintptr_t entity, float amount, const CharacterNodes *nodes = nullptr) noexcept;

    /**
     * @brief The render node of the player's character (its CRenderProxy, which its temporary render objects name at
     *        +0x30), as the character fade's detour last saw it draw in the main view, or 0.
     * @details It is the entity last given to set_character_fade, seen whether or not it fades; another entity there
     *          clears it until that one draws. Any thread.
     */
    [[nodiscard]] std::uintptr_t character_render_node() noexcept;

} // namespace TPVCamera

#endif // TPVCAMERA_HOOKS_CHARACTER_FADE_HPP
