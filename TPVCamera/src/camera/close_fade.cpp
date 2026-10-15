/**
 * @file camera/close_fade.cpp
 * @brief The close-up fade and the first-person fallback (see close_fade.hpp).
 */

#include "camera/close_fade.hpp"
#include "camera/blend.hpp"
#include "config.hpp"
#include "hooks/character_fade.hpp"
#include "hooks/head_visibility.hpp"
#include "hooks/shader_twins.hpp"

#include <DetourModKit.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace TPVCamera
{
    namespace
    {
        // The close-up fade: the band (meters of arm above HeadClearance) over which the character fades out.
        constexpr float k_close_fade_band = 0.4f;
        // The stock eye, hair and eye-film shaders ignore the dissolve, so the eyes, eyelashes, hair, beard and the wet
        // film over the eyes would stay solid in the dithered face. Once the character is this far faded the head is
        // hidden like in first person (the game's FirstPersonView hiding group, which keeps its shadow), and it is
        // shown again below the lower mark.
        constexpr float k_close_fade_head_hide_on = 0.6f;
        constexpr float k_close_fade_head_hide_off = 0.45f;
        // The shader twins fade the eyes (Eye), the eyelashes, beard and hair (Hair) and the eye film (a transparent
        // Illum) with the character. They warm up in third person ahead of the first close-up (the engine compiles
        // their fade permutations in the background and writes them to the user shader cache). The marks above apply
        // while any twin the character shows is still warming up, while an Eye twin was given up (eye_fade_ready
        // false), or without the twins. With every twin warm (character_fade_ready) the head is never hidden. With the
        // Eye twins warm but a Hair or transparent Illum twin given up, whose items stay solid, it is hidden only at
        // the top of the fade, where the dithered face is already gone: 1.0 is dissolve byte 254, and the 4x4 dither
        // discards every pixel from byte 240 (above its largest threshold, 15/16), which an amount of 0.95 passes.
        constexpr float k_close_fade_faded_head_hide_on = 0.95f;
        constexpr float k_close_fade_faded_head_hide_off = 0.9f;
        // The first-person fallback: meters of extra room before third person resumes, and the ease rate (1/s,
        // frame-rate independent).
        constexpr float k_head_fallback_margin = 0.15f;
        constexpr float k_head_fallback_rate = 12.0f;

        // The close-up fade's head path last logged: -1 none yet, 0 head hidden most of the way, 1 hidden once the
        // face is gone, 2 never hidden. Main thread only.
        int s_logged_head_path = -1;

        /**
         * @brief Dissolves the character by @p close_amount and decides whether his head is hidden on top.
         * @details With the eye fade ready (the shader twins) the face and the eyes dither out with the body and the
         *          head is hidden only once the face is gone; with the eyelashes, hair, beard and eye film ready too
         *          nothing of the head stays solid and it is never hidden; otherwise it is hidden most of the way along.
         */
        void fade_character(const CharacterNodes &character, std::uintptr_t entity, float close_amount, bool close_fade)
        {
            set_character_fade(entity, close_amount, &character);
            const bool eyes_fade = eye_fade_ready();
            const bool all_fade = eyes_fade && character_fade_ready();
            const int head_path = all_fade ? 2 : (eyes_fade ? 1 : 0);
            if (close_fade && head_path != s_logged_head_path)
            {
                s_logged_head_path = head_path;
                static constexpr std::array<const char *, 3> k_head_paths{
                    "hides the head once the character is mostly faded (the eye fade is not ready)",
                    "dithers the eyes out with the character and hides the head once the face is gone (a twin of the "
                    "eyelashes, hair or eye film was given up)",
                    "dithers the whole character out, eyes, eyelashes, hair and eye film included, and keeps the head "
                    "shown",
                };
                (void)DMK::log().try_log(DMK::LogLevel::Debug, "Camera: the close-up fade {} (close amount {:.2f})",
                                         k_head_paths[static_cast<std::size_t>(head_path)], close_amount);
            }
            if (all_fade)
            {
                set_head_close_fade(false);
            }
            else
            {
                const float hide_on = eyes_fade ? k_close_fade_faded_head_hide_on : k_close_fade_head_hide_on;
                const float hide_off = eyes_fade ? k_close_fade_faded_head_hide_off : k_close_fade_head_hide_off;
                const bool head_was_hidden = head_close_fade();
                set_head_close_fade(close_amount >= hide_on || (head_was_hidden && close_amount >= hide_off));
            }
        }
    } // namespace

    Vector3 apply_close_up(CameraState &cam, const CharacterNodes &character, const CloseUpInput &in)
    {
        LiveSettings &cfg = settings();
        const float clearance = cfg.head_clearance.load(std::memory_order_relaxed);
        if (!in.repeat_build)
        {
            const float close_most =
                1.0f - std::clamp(cfg.close_up_fade_min_opacity.load(std::memory_order_relaxed), 0.0f, 1.0f);
            // HeadClearance only places the fade: it reaches its full amount at that much arm (0 = against the head).
            // CloseUpFade alone turns it on and off.
            const float close_amount =
                in.close_fade ? std::clamp((std::max(clearance, 0.0f) + k_close_fade_band - in.arm) / k_close_fade_band,
                                           0.0f, close_most)
                              : 0.0f;
            fade_character(character, in.entity, close_amount, in.close_fade);
        }
        if (clearance <= 0.0f || in.close_fade)
        {
            cam.head_fallback = false;
        }
        else if (!cam.head_fallback && in.arm < clearance)
        {
            cam.head_fallback = true;
            (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                     "Camera: first-person fallback ON (arm {:.2f} m < HeadClearance {:.2f} m)", in.arm,
                                     clearance);
        }
        else if (cam.head_fallback && in.arm > clearance + k_head_fallback_margin)
        {
            cam.head_fallback = false;
            (void)DMK::log().try_log(DMK::LogLevel::Debug, "Camera: first-person fallback OFF (arm {:.2f} m)", in.arm);
        }
        cam.head_fallback_blend = ease_weight(cam.head_fallback_blend, cam.head_fallback ? 1.0f : 0.0f,
                                              k_head_fallback_rate, in.delta_time, 1e-3f);
        // The head hides as soon as the fallback starts (the camera is already close to it) and shows again only once
        // the camera is back out past halfway.
        set_head_fallback(cam.head_fallback || cam.head_fallback_blend > 0.5f);
        if (cam.head_fallback_blend > 0.0f)
        {
            return in.camera + (in.eye - in.camera) * cam.head_fallback_blend;
        }
        return in.camera;
    }
} // namespace TPVCamera
