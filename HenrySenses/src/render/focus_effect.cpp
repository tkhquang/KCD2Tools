/**
 * @file render/focus_effect.cpp
 * @brief Focus: the game's VisualArtifacts post effect, masked to everything but the highlighted objects.
 */

#include "render/focus_effect.hpp"
#include "constants.hpp"
#include "rtti_types.hpp"
#include "engine/engine_env.hpp"
#include "engine/seh.hpp"
#include "render/engine_silhouette.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace HenrySenses
{
    namespace
    {
        /** @brief The engine's Vec4 as the post-effect calls take it. */
        struct PostVec4
        {
            float x;
            float y;
            float z;
            float w;
        };

        using SetVec4Fn =
            void(__fastcall *)(std::uintptr_t engine, const char *name, const PostVec4 *value, bool force);
        using SetStringFn = void(__fastcall *)(std::uintptr_t engine, const char *name, const char *value);
        using GetVec4Fn = void(__fastcall *)(std::uintptr_t engine, const char *name, PostVec4 *value);

        constexpr PostVec4 WHITE{1.0f, 1.0f, 1.0f, 1.0f};
        // A tint change smaller than this is not sent again.
        constexpr float TINT_EPSILON = 1.0f / 512.0f;
        // Post-effect parameters keep one copy per render frame slot, and the main thread writes the slot of the frame
        // it fills, which alternates. A Vec4 is copied into the next slot at every frame sync; a texture is not
        // (CParamTexture::SyncMainWithRender copies nothing), so a mask set once reaches every other frame only. The
        // mask is sent every frame while focus holds the effect (the engine returns at once when the name matches) and
        // the white mask for this many frames after a release.
        constexpr int RELEASE_FRAMES = 3;

        // Whether the mod holds the effect's tint and mask, the mask it set, and the tint it last sent.
        bool s_owned = false;
        std::string s_mask_name;
        PostVec4 s_sent = WHITE;
        // Frames the white mask is still sent after a release, so every frame slot drops the coverage mask.
        int s_release_frames_left = 0;
        // The fade-in: the linear time fraction of the rise (shown eased), and the tick it was last advanced at.
        float s_rise = 0.0f;
        std::int64_t s_rise_ms = 0;
        // The longest step the fade-in takes in one tick, so a hitch or a load does not skip it.
        constexpr std::int64_t MAX_RISE_STEP_MS = 100;
        constexpr float MAX_FADE_IN_SECONDS = 10.0f;
        // The game's own VisualArtifacts effect was running when a focus began (logged once until it ends).
        bool s_yielded = false;
        std::uint64_t s_activations = 0;

        [[nodiscard]] bool is_white(const PostVec4 &value) noexcept
        {
            return value.x == 1.0f && value.y == 1.0f && value.z == 1.0f && value.w == 1.0f;
        }

        [[nodiscard]] bool differs(const PostVec4 &a, const PostVec4 &b) noexcept
        {
            return std::fabs(a.x - b.x) > TINT_EPSILON || std::fabs(a.y - b.y) > TINT_EPSILON ||
                   std::fabs(a.z - b.z) > TINT_EPSILON;
        }

        /**
         * @brief The 3D engine and its post-effect calls, read and checked once per use.
         */
        struct EngineCalls
        {
            std::uintptr_t engine{0};
            std::uintptr_t set_vec4{0};
            std::uintptr_t set_string{0};
            std::uintptr_t get_vec4{0};
        };

        [[nodiscard]] EngineCalls engine_calls() noexcept
        {
            EngineCalls calls{};
            const std::uintptr_t engine = genv_interface(constants::GENV_3DENGINE_OFFSET);
            if (engine == 0 || !object_is(GameClass::ThreeDEngine, engine))
            {
                return calls;
            }
            calls.set_vec4 = read_vtable_slot(engine, constants::ENGINE_3D_VTABLE_SET_POST_EFFECT_VEC4_OFFSET);
            calls.set_string = read_vtable_slot(engine, constants::ENGINE_3D_VTABLE_SET_POST_EFFECT_STRING_OFFSET);
            calls.get_vec4 = read_vtable_slot(engine, constants::ENGINE_3D_VTABLE_GET_POST_EFFECT_VEC4_OFFSET);
            if (calls.set_vec4 != 0 && calls.set_string != 0 && calls.get_vec4 != 0)
            {
                calls.engine = engine;
            }
            return calls;
        }

        [[nodiscard]] bool guarded_set_vec4(const EngineCalls &calls, const char *name, const PostVec4 &value) noexcept
        {
            __try
            {
                reinterpret_cast<SetVec4Fn>(calls.set_vec4)(calls.engine, name, &value, true);
                return true;
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return false;
            }
        }

        [[nodiscard]] bool guarded_set_string(const EngineCalls &calls, const char *name, const char *value) noexcept
        {
            __try
            {
                reinterpret_cast<SetStringFn>(calls.set_string)(calls.engine, name, value);
                return true;
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return false;
            }
        }

        [[nodiscard]] bool guarded_get_vec4(const EngineCalls &calls, const char *name, PostVec4 &value) noexcept
        {
            __try
            {
                reinterpret_cast<GetVec4Fn>(calls.get_vec4)(calls.engine, name, &value);
                return true;
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return false;
            }
        }

        /**
         * @brief The background's colour multiplier: the tint darkened by @p darken, eased in by @p strength.
         */
        [[nodiscard]] PostVec4 focus_tint(float strength, float darken, std::uint32_t tint) noexcept
        {
            const float fade = std::clamp(strength, 0.0f, 1.0f);
            const float keep = 1.0f - std::clamp(darken, 0.0f, 1.0f);
            auto channel = [&](int shift)
            {
                const float target = static_cast<float>((tint >> shift) & 0xFFu) / 255.0f * keep;
                return 1.0f + (target - 1.0f) * fade;
            };
            return PostVec4{channel(16), channel(8), channel(0), 1.0f};
        }

        /** @brief The fade curve (smoothstep), as the group fade-outs use it. */
        [[nodiscard]] float ease(float t) noexcept
        {
            return t * t * (3.0f - 2.0f * t);
        }

        /** @brief The inverse of ease() on [0, 1]. */
        [[nodiscard]] float uneased(float level) noexcept
        {
            return 0.5f - std::sin(std::asin(1.0f - 2.0f * std::clamp(level, 0.0f, 1.0f)) / 3.0f);
        }

        /**
         * @brief The focus level for this tick: a rising @p target eased in over @p fade_in seconds, a falling one
         *        followed as it comes. A press during a fade-out rises from the level the fade-out reached.
         */
        [[nodiscard]] float fade_in_level(float target, float fade_in, std::int64_t now_ms) noexcept
        {
            const std::int64_t step =
                s_rise_ms != 0 ? std::clamp<std::int64_t>(now_ms - s_rise_ms, 0, MAX_RISE_STEP_MS) : 0;
            s_rise_ms = now_ms;
            target = std::clamp(target, 0.0f, 1.0f);
            fade_in = std::clamp(fade_in, 0.0f, MAX_FADE_IN_SECONDS);
            if (fade_in <= 0.0f)
            {
                s_rise = uneased(target);
                return target;
            }
            s_rise = std::min(1.0f, s_rise + static_cast<float>(step) / (fade_in * 1000.0f));
            const float level = ease(s_rise);
            if (target < level)
            {
                s_rise = uneased(target);
                return target;
            }
            return level;
        }

        /**
         * @brief Gives the effect's parameters back when the mod holds them; afterwards sends the white mask to the
         *        remaining frame slots (see RELEASE_FRAMES).
         */
        void release_effect() noexcept
        {
            s_yielded = false;
            if (!s_owned)
            {
                set_focus_coverage_wanted(false);
                if (s_release_frames_left > 0)
                {
                    --s_release_frames_left;
                    if (const EngineCalls calls = engine_calls(); calls.engine != 0)
                    {
                        (void)guarded_set_string(
                            calls,
                            constants::POST_EFFECT_VISUAL_ARTIFACTS_MASK,
                            constants::ENGINE_WHITE_TEXTURE
                        );
                    }
                }
                return;
            }
            // A white tint switches the effect off; the white mask leaves the game's own use of it unmasked.
            if (const EngineCalls calls = engine_calls(); calls.engine != 0)
            {
                (void)guarded_set_vec4(calls, constants::POST_EFFECT_VISUAL_ARTIFACTS_TINT, WHITE);
                (void)guarded_set_string(
                    calls,
                    constants::POST_EFFECT_VISUAL_ARTIFACTS_MASK,
                    constants::ENGINE_WHITE_TEXTURE
                );
            }
            set_focus_coverage_wanted(false);
            s_release_frames_left = RELEASE_FRAMES - 1;
            s_owned = false;
            s_sent = WHITE;
            s_mask_name.clear();
            (void)DMK::log().try_log(DMK::LogLevel::Debug, "Focus: off");
        }
    } // namespace

    void
    update_focus_effect(float strength, float darken, std::uint32_t tint, float fade_in, std::int64_t now_ms) noexcept
    {
        const float level = fade_in_level(strength, fade_in, now_ms);
        const PostVec4 value = focus_tint(level, darken, tint);
        if (level <= 0.0f || !differs(value, WHITE))
        {
            release_effect();
            return;
        }
        const EngineCalls calls = engine_calls();
        if (calls.engine == 0)
        {
            return;
        }
        if (!s_owned)
        {
            // The game's bane and berserker potions use the same effect; while one runs, the focus waits.
            PostVec4 current = WHITE;
            if (!guarded_get_vec4(calls, constants::POST_EFFECT_VISUAL_ARTIFACTS_TINT, current) || !is_white(current))
            {
                if (!s_yielded)
                {
                    s_yielded = true;
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Info,
                        "Focus: the game's own VisualArtifacts effect is running (tint {:.2f} {:.2f} {:.2f}); focus "
                        "waits for it to end",
                        current.x,
                        current.y,
                        current.z
                    );
                }
                // The focus eases in once the game's effect has ended.
                s_rise = 0.0f;
                return;
            }
            std::string name;
            try
            {
                name = focus_coverage_texture_name();
            }
            catch (...)
            {
                return;
            }
            if (name.empty())
            {
                return;
            }
            // The coverage is drawn from this frame on; the effect reads it from the next.
            set_focus_coverage_wanted(true);
            if (!guarded_set_string(calls, constants::POST_EFFECT_VISUAL_ARTIFACTS_MASK, name.c_str()))
            {
                set_focus_coverage_wanted(false);
                return;
            }
            s_owned = true;
            s_yielded = false;
            s_release_frames_left = 0;
            s_mask_name = std::move(name);
            s_sent = WHITE;
            ++s_activations;
            (void)DMK::log().try_log(DMK::LogLevel::Debug, "Focus: on (mask '{}')", s_mask_name);
        }
        else
        {
            // Each frame slot takes the mask only when its own frame sends it (see RELEASE_FRAMES).
            (void)guarded_set_string(calls, constants::POST_EFFECT_VISUAL_ARTIFACTS_MASK, s_mask_name.c_str());
        }
        if (differs(value, s_sent) && guarded_set_vec4(calls, constants::POST_EFFECT_VISUAL_ARTIFACTS_TINT, value))
        {
            s_sent = value;
        }
    }

    void release_focus_effect() noexcept
    {
        s_rise = 0.0f;
        s_rise_ms = 0;
        release_effect();
    }

    void log_focus_effect_state()
    {
        DMK::log().info(
            "Focus: {} mask='{}' tint={:.2f} {:.2f} {:.2f}, coverage frames={}, activations={}{}",
            s_owned ? "on" : "off",
            s_mask_name,
            s_sent.x,
            s_sent.y,
            s_sent.z,
            focus_coverage_frames(),
            s_activations,
            s_yielded ? " (waiting for the game's own effect)" : ""
        );
    }

} // namespace HenrySenses
