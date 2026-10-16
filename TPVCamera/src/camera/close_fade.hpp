/**
 * @file camera/close_fade.hpp
 * @brief Too close to the character: the close-up fade, and the first-person fallback without it.
 */
#ifndef TPVCAMERA_CAMERA_CLOSE_FADE_HPP
#define TPVCAMERA_CAMERA_CLOSE_FADE_HPP

#include "character_attachments.hpp"
#include "global_state.hpp"
#include "math_utils.hpp"

#include <cstdint>

namespace TPVCamera
{
    /** @brief One camera frame's input to the close-up fade and the first-person fallback. */
    struct CloseUpInput
    {
        Vector3 camera{}; // the camera after collision
        Vector3 eye{};    // the first-person eye the fallback eases the camera onto
        // The arm after collision, measured from the head where it really is: the pivot plus the turn hold, which keeps
        // the pivot off the body while it steps through a turn. A close zoom counts too.
        float arm = 0.0f;
        std::uintptr_t entity = 0; // the player entity the character fade dissolves
        float delta_time = 0.0f;
        bool close_fade = false;   // CloseUpFade with the character fade hook available
        bool switching = false;    // the first-person <-> third-person switch is under way (0 < view_blend < 1)
        bool repeat_build = false; // the frame's second build
    };

    /**
     * @brief Keeps the camera out of the character's head when there is no room behind him.
     * @details With no room behind the player - a low lintel in a doorway (the collision holds the camera under it), a
     *          wall right behind - collision pulls the camera onto the pivot, inside the head, and the view shows the
     *          head's inside until the player moves. With CloseUpFade the camera stays where the collision put it and
     *          the character dithers out instead, as far as CloseUpFadeMinOpacity lets him from HeadClearance of arm
     *          (0: against the head) and starting k_close_fade_band before it, so the view never jumps; the dither
     *          follows at the occluder fade's timings while the head decisions take the measured amount at once; the
     *          fade then replaces the fallback below, and HeadClearance only places it. What he carries fades with
     *          him, found by its attachment link. When his eyes and hair cannot fade with him, his head is hidden like in
     *          first person once he is mostly faded (set_head_close_fade()). Otherwise (or without the character fade
     *          hook), below HeadClearance the camera eases onto the real eye and the head is hidden by the game's own
     *          first-person rig (set_head_fallback(), read by the head re-assert and the head-visibility detour), so
     *          the view is plain first person until there is room again. A margin keeps a doorway from switching the
     *          first-person view on and off as the arm hovers around the threshold. While the view switch runs, its
     *          arm grows from the eye or shrinks to it, so the fallback holds its state, and the fade is held to
     *          the lens's distance from the eye: the character stays solid while the lens passes through the head
     *          (hidden by the switch) instead of dithering out and back. Main thread only.
     * @param character The character's render nodes, dissolved with him.
     * @return The camera position after the fallback's ease toward the eye.
     */
    [[nodiscard]] Vector3 apply_close_up(CameraState &cam, const CharacterNodes &character, const CloseUpInput &in);

    /**
     * @brief Drops the close-up fade's slewed amount when the view leaves third person, so the next close-up starts
     *        from a solid character. Main thread only.
     */
    void reset_close_up() noexcept;
} // namespace TPVCamera

#endif // TPVCAMERA_CAMERA_CLOSE_FADE_HPP
