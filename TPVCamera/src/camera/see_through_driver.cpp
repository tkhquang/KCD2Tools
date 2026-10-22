/**
 * @file camera/see_through_driver.cpp
 * @brief The tree see-through window, the near cut and the character's shader twins per camera frame (see
 *        see_through_driver.hpp).
 */

#include "camera/see_through_driver.hpp"
#include "camera/collision_trace.hpp"
#include "config.hpp"
#include "constants.hpp"
#include "hooks/character_fade.hpp"
#include "hooks/shader_twins.hpp"
#include "render_occlusion.hpp"

#include <DetourModKit.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <tuple>

namespace TPVCamera
{
    namespace
    {
        // The tree see-through window (see run_tree_see_through). The height above the window's floor over which the
        // cut fades in (meters). The floor is TreeSeeThroughStartHeight of the character's height above his feet, his
        // hips by default: grass, a bush or a trunk that hides only his feet and legs is how a person standing in a
        // field looks, so nothing below the hips opens: with the camera up, the plants in front of his legs meet his
        // plane below the floor and stay; with the camera down in the grass, the blades between the lens and him meet
        // his plane at his chest and head, inside the window, and open.
        constexpr float k_tree_hole_floor_feather = 0.25f;
        // Half the character's width at the shoulders and the top of the head above the eye (meters); the character's
        // height when the body is unknown, and the least height the window takes (lying down); half the character's
        // depth (meters, his back and a shield or cloak on it) and the steepest view slope (rise over run) it is
        // counted for, since a camera looking down sees his back meet the window's plane higher than it stands and his
        // toes lower; the margin kept around the character, inside the fully open part of the window (the dithered rim
        // lies outside it); how many frames of the character's travel the window grows by, since the renderer draws the
        // frame before the camera's and a window that did not grow would trail a galloping rider; the fastest travel
        // that counts (m/s, a faster step is a teleport or a load) and the rate (1/s) the travel speed is smoothed at;
        // the seconds the window takes to open or close; and the least distance across the ground (meters) from the
        // camera to the character's centre, below which the camera looks straight down at him and there is no plane to
        // open a window on.
        constexpr float k_tree_hole_half_width = 0.4f;
        constexpr float k_tree_hole_head_above_eye = 0.15f;
        constexpr float k_tree_hole_default_height = 1.85f;
        constexpr float k_tree_hole_min_height = 0.6f;
        constexpr float k_tree_hole_half_depth = 0.25f;
        constexpr float k_tree_hole_max_slope = 2.0f;
        constexpr float k_tree_hole_margin = 0.15f;
        constexpr float k_tree_hole_lead_frames = 2.0f;
        constexpr float k_tree_hole_max_speed = 20.0f;
        constexpr float k_tree_hole_speed_rate = 8.0f;
        constexpr float k_tree_hole_ramp_seconds = 0.2f;
        constexpr float k_tree_hole_min_reach = 0.1f;
        // The near cut (see run_tree_see_through): the swapped plants and bare frames lose every pixel nearer to the
        // lens than k_near_cut_full (view depth, meters) and fade back in up to k_near_cut_clear, so a branch or a beam
        // right in front of the lens thins away while the rest of it stays. A frame is listed while its box comes
        // within k_near_cut_list_reach of the camera, farther than the cut reaches, so it is swapped before a beam of
        // it gets there; at most k_near_cut_max_frames are.
        constexpr float k_near_cut_full = 1.0f;
        constexpr float k_near_cut_clear = 2.0f;
        constexpr float k_near_cut_list_reach = 3.0f;
        constexpr int k_near_cut_max_frames = 8;

        // The tree see-through on the main thread (the camera detour): how far the window is open (0 shut .. 1 open),
        // the character's smoothed travel speed (m/s), and whether the window was last asked to be open, for the log.
        float s_tree_hole_open = 0.0f;
        float s_tree_hole_speed = 0.0f;
        bool s_tree_hole_wanted = false;

        // While no feature draws with a twin (shader_twins_wanted()), the camera advances the twins with nothing
        // listed for k_twins_settle_seconds (longer than TREE_HOLE_HOLD_SECONDS), until every tree and frame has gone
        // back to its stock shader and the character's set is unpublished, and then no longer. The seconds counted so
        // far (main thread only).
        constexpr float k_twins_settle_seconds = Constants::TREE_HOLE_HOLD_SECONDS + 0.5f;
        float s_twins_off_seconds = 0.0f;

        /**
         * @brief True while the twins idle: no feature draws with one, and the swapped nodes have gone back. The
         *        camera then leaves them alone. Counts @p delta_time toward the settle while they do not idle yet.
         */
        [[nodiscard]] bool twins_idle(float delta_time)
        {
            if (shader_twins_wanted())
            {
                if (s_twins_off_seconds > 0.0f)
                {
                    s_twins_off_seconds = 0.0f;
                    (void)DMK::log().try_log(DMK::LogLevel::Debug, "ShaderTwins: advanced again (a feature drawing "
                                                                   "with them is on)");
                }
                return false;
            }
            if (s_twins_off_seconds >= k_twins_settle_seconds && shader_twin_stats().swapped == 0)
            {
                return true;
            }
            s_twins_off_seconds += std::max(delta_time, 1e-3f);
            if (s_twins_off_seconds >= k_twins_settle_seconds && shader_twin_stats().swapped == 0)
            {
                (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                         "ShaderTwins: idle (UseTreeSeeThrough, UseLensCut and CloseUpFade are off)");
            }
            return false;
        }

        /**
         * @brief The character's own render node for the shader twins: the one the character fade's detour last saw
         * draw
         *        in the main view, the node his temporary render objects name, or else the attachment walk's.
         * @details Both are his CRenderProxy: the walk reads it from his entity's proxy map, the detour is handed it by
         * the
         *          engine. They differ only for a frame or two after a load, until the new proxy draws. A difference that
         *          lasts means the walk's layout does not fit this build, so the carried nodes it lists match no draw
         *          either and what he carries stays solid while he fades; that is logged once as a warning. Main thread
         *          only.
         */
        std::uintptr_t character_own_node(const CharacterNodes &character)
        {
            // Camera frames the two may differ for before the warning.
            constexpr int k_mismatch_frames = 30;
            static int s_mismatch_frames = 0;
            static bool s_mismatch_logged = false;
            const std::uintptr_t drawn = character_render_node();
            const std::uintptr_t walked = character.root();
            if (drawn == 0 || walked == 0 || drawn == walked)
            {
                s_mismatch_frames = 0;
            }
            else if (++s_mismatch_frames >= k_mismatch_frames && !s_mismatch_logged)
            {
                s_mismatch_logged = true;
                (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                         "CloseUpFade: the attachment walk found the character's render node at {:#x}, "
                                         "but the character fade sees it draw at {:#x}; what it carries may stay solid",
                                         walked, drawn);
            }
            return drawn != 0 ? drawn : walked;
        }

        /**
         * @brief Advances the shader twins by one third-person camera frame.
         * @details The twins fade the character's eyes, eyelashes, hair, beard and eye film with him in the close-up
         * fade
         *          (see eye_fade_ready()). While it can run they run on his render nodes: his own (character_own_node()),
         *          then the carried ones @p character walked this frame. The trees and frames take @p window, @p trees
         *          and @p frames (run_tree_see_through()). Main thread only, once per frame (not on the frame's second
         *          build).
         * @param close_fade Whether the close-up fade can run this frame: CloseUpFade on, with the character fade hook.
         * @param window The tree see-through's window and the near cut this frame.
         * @param trees The trees in the window.
         * @param frames The bare frames near the lens.
         * @param delta_time Seconds this frame advances.
         * @param character The character's render nodes the attachment walk found this frame.
         */
        void run_character_twins(bool close_fade, const TreeHoleWindow &window, std::span<void *const> trees,
                                 std::span<void *const> frames, float delta_time, const CharacterNodes &character)
        {
            static_assert(std::tuple_size_v<decltype(CharacterNodes::nodes)> == Constants::CHARACTER_TWIN_MAX_NODES,
                          "The swap set names the character's own node and every carried one.");
            std::array<std::uintptr_t, Constants::CHARACTER_TWIN_MAX_NODES> nodes{};
            std::size_t count = 0;
            if (const std::uintptr_t own = close_fade ? character_own_node(character) : 0; own != 0)
            {
                const std::span<const std::uintptr_t> carried = character.carried();
                nodes[0] = own;
                std::ranges::copy(carried, nodes.begin() + 1);
                count = 1 + carried.size();
            }
            update_shader_twins(std::span<const std::uintptr_t>(nodes.data(), count), window, trees, frames,
                                delta_time);
        }
    } // namespace

    void run_tree_see_through(const Vector3 &camera, const Vector3 &eye, const Vector3 &feet, bool body_valid,
                              float body_step, bool wanted, float view_blend, float delta_time,
                              const CharacterNodes &character)
    {
        if (!shader_twins_available() || twins_idle(delta_time))
        {
            return;
        }
        DMK_PROFILE_SCOPE("camera.see_through");
        LiveSettings &cfg = settings();
        const float strength =
            1.0f - std::clamp(cfg.tree_see_through_min_opacity.load(std::memory_order_relaxed), 0.0f, 1.0f);
        const bool near_cut = wanted && cfg.use_lens_cut.load(std::memory_order_relaxed);
        wanted = wanted && cfg.use_tree_see_through.load(std::memory_order_relaxed) && strength > 0.0f;
        if (wanted != s_tree_hole_wanted)
        {
            s_tree_hole_wanted = wanted;
            (void)DMK::log().try_log(DMK::LogLevel::Debug, "TreeSeeThrough: window {}", wanted ? "opening" : "closing");
        }
        const float ramp = delta_time / k_tree_hole_ramp_seconds;
        s_tree_hole_open = wanted ? std::min(1.0f, s_tree_hole_open + ramp) : std::max(0.0f, s_tree_hole_open - ramp);
        const float travel =
            (body_valid && delta_time > 1e-4f) ? std::min(body_step / delta_time, k_tree_hole_max_speed) : 0.0f;
        s_tree_hole_speed += (travel - s_tree_hole_speed) * (1.0f - std::exp(-k_tree_hole_speed_rate * delta_time));

        const Vector3 up{0.0f, 0.0f, 1.0f};
        const Vector3 top = eye + up * k_tree_hole_head_above_eye;
        const Vector3 bottom = body_valid ? feet : top - up * k_tree_hole_default_height;
        const float height = std::max(top.z - bottom.z, k_tree_hole_min_height);
        const float size = std::clamp(cfg.tree_see_through_size.load(std::memory_order_relaxed), 0.5f, 2.0f);
        const float softness = std::clamp(cfg.tree_see_through_softness.load(std::memory_order_relaxed), 0.0f, 1.0f);
        const float margin = k_tree_hole_margin + s_tree_hole_speed * delta_time * k_tree_hole_lead_frames;
        TreeHoleWindow window{};
        window.anchor = (top + bottom) * 0.5f;
        window.strength = strength * s_tree_hole_open * std::clamp(view_blend, 0.0f, 1.0f);
        const Vector3 toward{window.anchor.x - camera.x, window.anchor.y - camera.y, 0.0f};
        const float reach = toward.magnitude();
        // Seen from above (or below), the character's back meets the window's plane higher than it stands and his toes
        // lower, by his half depth times the view's slope, so his height on the plane grows by that.
        const float slope = std::min(std::abs(camera.z - window.anchor.z) / std::max(reach, k_tree_hole_min_reach),
                                     k_tree_hole_max_slope);
        const float half_extent_width = k_tree_hole_half_width * size + margin;
        const float half_extent_height = (0.5f * height + k_tree_hole_half_depth * slope) * size + margin;
        // The shader starts its dithered rim at (1 - softness) of the half sizes it is given. The window is fully open
        // over the character and his margin, and the rim lies outside them, softness of those half sizes wide: the
        // half sizes grow by (1 + softness), and the rim's share of them is softness / (1 + softness).
        const float rim_scale = 1.0f + softness;
        window.half_width = half_extent_width * rim_scale;
        window.half_height = half_extent_height * rim_scale;
        window.softness = softness / rim_scale;
        // Nothing opens below TreeSeeThroughStartHeight of his height (the shader cuts there, faded in over
        // k_tree_hole_floor_feather), and the window keeps its size back to the lens (a tube, not a cone narrowing to
        // the lens).
        const float start_height =
            std::clamp(cfg.tree_see_through_start_height.load(std::memory_order_relaxed), 0.0f, 1.0f);
        window.floor_z = bottom.z + start_height * height;
        window.floor_feather = k_tree_hole_floor_feather;
        window.tunnel = 1.0f;
        window.near_strength = near_cut ? std::clamp(view_blend, 0.0f, 1.0f) : 0.0f;
        window.near_full = k_near_cut_full;
        window.near_clear = k_near_cut_clear;

        // The bare frames near the lens, listed first so the list's cap never drops one for a plant.
        std::array<void *, k_near_cut_max_frames> frames{};
        int n_frames = 0;
        if (window.near_strength > 0.0f)
        {
            n_frames = frames_near(camera, k_near_cut_list_reach, frames.data(), static_cast<int>(frames.size()));
        }

        // The window stands on the vertical plane through the character's centre, facing the camera across the ground,
        // with its corners in order around it; they hold the rim too. The tube runs from the same rectangle moved to
        // the camera, so every tree, bush or grass patch a pixel of it can open is listed.
        std::array<void *, Constants::TREE_HOLE_MAX_NODES> trees{};
        int n_trees = 0;
        if (window.strength > 0.0f && reach >= k_tree_hole_min_reach)
        {
            const Vector3 facing = toward / reach;
            const Vector3 across = Vector3{-facing.y, facing.x, 0.0f} * window.half_width;
            const Vector3 rise = up * window.half_height;
            const Vector3 far_corners[4] = {window.anchor - across - rise, window.anchor + across - rise,
                                            window.anchor + across + rise, window.anchor - across + rise};
            const Vector3 back = camera - window.anchor;
            const Vector3 near_corners[4] = {far_corners[0] + back, far_corners[1] + back, far_corners[2] + back,
                                             far_corners[3] + back};
            n_trees =
                trees_in_window(near_corners, far_corners, trees.data(), static_cast<int>(trees.size()) - n_frames);
        }
        const std::span<void *const> listed(trees.data(), static_cast<std::size_t>(n_trees));
        const std::span<void *const> listed_frames(frames.data(), static_cast<std::size_t>(n_frames));
        log_tree_window_change(listed, listed_frames, window);
        // The same module fades the character's eyes, eyelashes, hair and eye film with him in the close-up fade.
        run_character_twins(cfg.close_up_fade.load(std::memory_order_relaxed) && character_fade_available(), window,
                            listed, listed_frames, delta_time, character);
        watch_character_render(shader_twin_stats().swapped > 0);
    }

    void stop_tree_see_through(float delta_time)
    {
        s_tree_hole_open = 0.0f;
        s_tree_hole_speed = 0.0f;
        if (s_tree_hole_wanted)
        {
            s_tree_hole_wanted = false;
            (void)DMK::log().try_log(DMK::LogLevel::Debug, "TreeSeeThrough: window closed (first person)");
        }
        if (!shader_twins_available() || twins_idle(delta_time))
        {
            return;
        }
        const TreeHoleWindow shut{};
        log_tree_window_change({}, {}, shut);
        // First person has no close-up fade, so the character's twins do not run either.
        update_shader_twins({}, shut, {}, {}, delta_time);
        watch_character_render(false);
    }
} // namespace TPVCamera
