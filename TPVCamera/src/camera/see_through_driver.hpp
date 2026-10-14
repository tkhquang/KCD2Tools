/**
 * @file camera/see_through_driver.hpp
 * @brief Drives the tree see-through window, the near cut and the character's shader twins from the camera frame.
 */
#ifndef TPVCAMERA_CAMERA_SEE_THROUGH_DRIVER_HPP
#define TPVCAMERA_CAMERA_SEE_THROUGH_DRIVER_HPP

#include "character_attachments.hpp"
#include "math_utils.hpp"

namespace TPVCamera
{
    /**
     * @brief Hands the tree see-through its window and its plants, and the near cut its frames, for this camera frame.
     * @details Trees and bushes are never faded whole for hiding the character, so the trees, bushes and grass between
     *          the camera and the character open a dithered window around him instead, only where they stand nearer
     *          than him: the rest of the plant and its shadow stay (see hooks/shader_twins.hpp). The window is an
     *          ellipse on the character's vertical plane facing the camera, centred on his box (the feet to the top of
     *          the head). It is fully open over his shoulders and his height (taller by his depth when the camera looks
     *          down at him), both scaled by TreeSeeThroughSize, plus a margin that grows with his speed, and its
     *          dithered rim, TreeSeeThroughSoftness of that size wide, lies outside. The shader cuts it at
     *          TreeSeeThroughStartHeight of his height, his hips by default, so plants that hide only his feet and
     *          legs stay, and grass opens only where it hides his chest or head (the camera down in it). The plants in
     *          the window keep TreeSeeThroughMinOpacity of themselves. The window keeps its size back to the lens (a
     *          tube), so the blades just in front of the lens that would hide him go too instead of being cut in a
     *          circle around him. Every scanned tree, bush or grass patch whose box meets the tube from the window's
     *          bounding rectangle back to the camera is listed: a broad phase, since the shader decides per pixel. The
     *          window eases open and shut over k_tree_hole_ramp_seconds and closes with the view blend, and the plants
     *          stay listed while it is open at all. Whenever @p wanted, whatever the tree settings, the swapped plants
     *          and the bare frames within k_near_cut_list_reach of the camera also make the near cut: their pixels
     *          within k_near_cut_clear of the lens thin away, all of them within k_near_cut_full, with the view blend;
     *          a frame gets only that, so a pole in front of the character stays. Also keeps the character-render
     *          watch on while plants or frames are swapped, and runs the character's twins with the trees
     *          (run_character_twins(), the eyes, hair and eye film fade) while CloseUpFade is on, whatever the tree
     *          settings. Main thread only, once per frame (not on the frame's second build).
     * @param camera The camera the occluder fade cast its sightlines from: the near end of the tube.
     * @param eye The first-person eye, which carries the head's height in every pose.
     * @param feet The body origin, when @p body_valid.
     * @param body_valid Whether @p feet and @p body_step were read this frame.
     * @param body_step The body's move across the ground since the last frame (meters).
     * @param wanted Whether the window and the near cut should be on: third person, with no dialogue, menu or overlay
     *        up.
     * @param view_blend The first-person (0) to third-person (1) blend.
     * @param delta_time Seconds this frame advances.
     * @param character The character's render nodes the attachment walk found this frame.
     */
    void run_tree_see_through(const Vector3 &camera, const Vector3 &eye, const Vector3 &feet, bool body_valid,
                              float body_step, bool wanted, float view_blend, float delta_time,
                              const CharacterNodes &character);

    /**
     * @brief Shuts the tree see-through window at once and lists no tree, for a first-person frame.
     * @details The trees still swapped go back to the stock shader once the shader twins module's hold runs out, so
     *          this keeps being called while any is left. Main thread only, once per frame.
     */
    void stop_tree_see_through(float delta_time);
} // namespace TPVCamera

#endif // TPVCAMERA_CAMERA_SEE_THROUGH_DRIVER_HPP
