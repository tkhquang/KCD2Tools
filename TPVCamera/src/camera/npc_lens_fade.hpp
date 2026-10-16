/**
 * @file camera/npc_lens_fade.hpp
 * @brief The NPC lens fade: people and animals right in front of the third-person lens dither out with what they carry.
 *
 * @details The lens near plane (camera/lens.hpp) is pushed out to 0.4 m so leaves and prop corners at the lens are cut
 *          away, and it never stops for a living body, so a person the camera comes that close to is cut open and shows
 *          the inside of its skin. With NpcLensFade on, a person or an animal whose drawn parts come within
 *          k_npc_clear_depth of the lens in the view fades, fully (down to NpcLensFadeMinOpacity) from
 *          k_npc_full_depth in, through the character fade's detour (hooks/character_fade.hpp); their eyes, hair and
 *          eye film draw with the close-up fade's twins while they dissolve (set_npc_twin_nodes()). Which models count
 *          is the camera rules' @actor line: only parts the rules leave to the camera as a character take part.
 */
#ifndef TPVCAMERA_CAMERA_NPC_LENS_FADE_HPP
#define TPVCAMERA_CAMERA_NPC_LENS_FADE_HPP

#include "camera/view_box.hpp"
#include "math_utils.hpp"

#include <cstdint>

namespace TPVCamera
{
    /** @brief One camera frame's input to the NPC lens fade. */
    struct NpcLensFadeInput
    {
        ViewAxes view{};                  // the final camera pose and its frustum
        std::uintptr_t player_entity = 0; // the player's CEntity: never faded here (the close-up fade owns it)
        Vector3 player_origin{};          // the player's feet, which tell the ridden mount apart
        float real_seconds = 0.0f;        // the frame's real time step: the fade runs on real time
        bool mounted = false;             // riding: the actor whose box holds the player's feet is the mount
        bool enabled = false;             // NpcLensFade on, the character fade hook armed, third person shown
    };

    /**
     * @brief Measures the people and animals at the lens and publishes how far each fades.
     * @details Each actor's fade follows the nearest view depth of its parts' boxes (camera/view_box.hpp) through a
     *          smoothstep from k_npc_full_depth to k_npc_clear_depth, rising over k_npc_fade_in_seconds and falling
     *          over k_npc_fade_out_seconds of real time; every part fades by its actor's amount, so a sword fades with
     *          the hand that holds it. An actor that leaves the band, or the whole fade turned off, fades back in over
     *          the same time. The ridden mount never fades. Each actor's start and end of a fade, and its amount as it
     *          crosses each quarter, is logged at Trace with its name, rule and depth. Main thread only, once per frame
     *          (the frame's second build skips it).
     */
    void update_npc_lens_fade(const NpcLensFadeInput &in);

    /** @brief Drops every NPC's fade at once and publishes none: the camera left third person. */
    void reset_npc_lens_fade() noexcept;
} // namespace TPVCamera

#endif // TPVCAMERA_CAMERA_NPC_LENS_FADE_HPP
