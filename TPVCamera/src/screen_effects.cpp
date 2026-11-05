/**
 * @file screen_effects.cpp
 * @brief Hides the game's screen effects each view's list names (see screen_effects.hpp).
 */

#include "screen_effects.hpp"
#include "aob_resolver.hpp"
#include "code_proofs.hpp"
#include "config.hpp"
#include "constants.hpp"
#include "core/text/token_list.hpp"
#include "global_state.hpp"
#include "hooks/vtable_slots.hpp"

#include <DetourModKit.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <span>
#include <string>

namespace TPVCamera
{
    namespace
    {
        // C_UIOverlay::SetAlpha(layer, alpha), the particle manager's FindEffect and enable-by-name slots, and
        // C3DEngine::SetPostEffectParam (see layout_offsets.hpp).
        using SetAlphaFunc = void(__fastcall *)(std::uintptr_t overlay, std::uint8_t layer, std::int32_t alpha);
        using FindEffectFunc = std::uintptr_t(__fastcall *)(std::uintptr_t particles, const char *name,
                                                            const char *source, bool load_resources);
        using SetEffectEnabledFunc = void(__fastcall *)(std::uintptr_t particles, const char *name, bool enabled);
        using SetPostEffectParamFunc = void(__fastcall *)(std::uintptr_t engine, const char *name, float value,
                                                          bool force);

        // The name FindEffect logs when an effect is missing, so the log points at the mod that asked.
        constexpr const char *k_find_source = "TPVCamera screen effects";
        constexpr const char *k_droplets_param = "WaterDroplets_Amount";

        // The screen particles of each group: the effects the game's own HideOverlays list names, plus their child
        // effects. A child keeps its own enabled flag, so turning the parent off leaves it spawning: washing's
        // close_eyes starts its child "open" (the eyelids opening from fully closed) 0.55 s in. The engine finds a
        // child by "library.effect.child" (FindLoadedEffect walks FindChild).
        constexpr std::array<const char *, 2> k_wash_particles{"professions.washing.close_eyes",
                                                               "professions.washing.close_eyes.open"};
        constexpr std::array<const char *, 3> k_sleepy_particles{
            "WH_Particels.other.eye_close", "WH_Particels.other.eye_open", "WH_Particels.other.eye_blink"};
        constexpr std::array<const char *, 1> k_head_blood_particles{"collisions.combat.blood_head"};

        struct ParticleGroup
        {
            ScreenEffect effect;
            std::span<const char *const> names;
        };
        constexpr std::array<ParticleGroup, 3> k_particle_groups{{
            {ScreenEffect::Wash, k_wash_particles},
            {ScreenEffect::Sleepy, k_sleepy_particles},
            {ScreenEffect::HeadBlood, k_head_blood_particles},
        }};

        // The Overlay.gfx layers this module hides, with the group each belongs to.
        struct OverlayLayer
        {
            std::uint8_t layer;
            ScreenEffect effect;
        };
        constexpr std::array<OverlayLayer, 5> k_overlay_layers{{
            {Constants::UI_OVERLAY_LAYER_HELMET, ScreenEffect::Helmet},
            {Constants::UI_OVERLAY_LAYER_HIT_FIRST, ScreenEffect::Hit},
            {Constants::UI_OVERLAY_LAYER_HIT_FIRST + 1, ScreenEffect::Hit},
            {Constants::UI_OVERLAY_LAYER_HIT_FIRST + 2, ScreenEffect::Hit},
            {Constants::UI_OVERLAY_LAYER_HIT_LAST, ScreenEffect::Hit},
        }};

        /** @brief The proven C_UIOverlay and its SetAlpha. */
        struct UiOverlay
        {
            std::uintptr_t object = 0;
            std::uintptr_t vtable = 0;
            SetAlphaFunc set_alpha = nullptr;
        };

        /** @brief The proven engine calls, resolved once from gEnv. */
        struct EngineCalls
        {
            std::uintptr_t particles = 0;
            FindEffectFunc find_effect = nullptr;
            SetEffectEnabledFunc set_effect_enabled = nullptr;
            std::uintptr_t engine = 0;
            SetPostEffectParamFunc set_post_effect_param = nullptr;
        };

        /** @brief What this generation last applied to one overlay layer. */
        struct LayerState
        {
            std::int32_t type = 0;
            bool hidden = false;
        };

        // Every member below is touched on the main thread only (the overlay hooks' detours and the frustum build).
        UiOverlay s_overlay{};
        EngineCalls s_calls{};
        bool s_calls_resolved = false;
        // Set when the overlay object or the engine calls failed their proof, so the warning is logged once.
        bool s_overlay_refused = false;
        bool s_calls_refused = false;
        // The particle groups this generation turned off. Unknown until the first update, which sets every group
        // explicitly, so a hot reload takes over whatever the previous generation left.
        std::uint32_t s_particles_hidden = 0;
        bool s_particles_synced = false;
        std::array<LayerState, k_overlay_layers.size()> s_layers{};
        // Cleared for each newly proven overlay object, so the next update sets every layer explicitly.
        bool s_layers_synced = false;

        [[nodiscard]] std::uintptr_t read_pointer(std::uintptr_t address) noexcept
        {
            const auto value = DMK::memory::read<std::uintptr_t>(DMK::Address{address});
            return value ? *value : 0;
        }

        /**
         * @brief Resolves the particle manager's two slots and the 3D engine's SetPostEffectParam from gEnv, each
         *        proven by its two shapes.
         * @details The game keeps both objects for the process, so the first resolution that succeeds is kept, and a
         *          failed proof turns the engine-side groups off for the session.
         */
        [[nodiscard]] bool resolve_engine_calls(std::uintptr_t genv) noexcept
        {
            if (s_calls_resolved)
            {
                return true;
            }
            if (s_calls_refused || genv == 0)
            {
                return false;
            }
            try
            {
                const ModuleInfo &game = module_info();
                const std::uintptr_t particles = read_pointer(genv + Constants::GENV_PARTICLE_MANAGER_OFFSET);
                const std::uintptr_t engine = read_pointer(genv + Constants::GENV_3DENGINE_OFFSET);
                if (particles == 0 || engine == 0)
                {
                    return false; // Not created yet: the engine fills gEnv during startup.
                }
                const std::uintptr_t particles_vtable = read_pointer(particles);
                const std::uintptr_t engine_vtable = read_pointer(engine);
                const std::uintptr_t find_effect =
                    read_proven_vtable_slot(particles_vtable, Constants::PARTICLE_MANAGER_FIND_EFFECT_SLOT,
                                            Proof::k_particleFindEffect, game.base, game.size);
                const std::uintptr_t set_enabled =
                    read_proven_vtable_slot(particles_vtable, Constants::PARTICLE_MANAGER_SET_EFFECT_ENABLED_SLOT,
                                            Proof::k_particleSetEffectEnabled, game.base, game.size);
                const std::uintptr_t set_param =
                    read_proven_vtable_slot(engine_vtable, Constants::ENGINE_SET_POST_EFFECT_PARAM_SLOT,
                                            Proof::k_engineSetPostEffectParam, game.base, game.size);
                if (find_effect == 0 || set_enabled == 0 || set_param == 0)
                {
                    s_calls_refused = true;
                    (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                             "ScreenEffects: the particle manager or the 3D engine did not match "
                                             "(FindEffect {}, enable {}, SetPostEffectParam {}); the wash, sleepy and "
                                             "head blood effects stay as the game shows them",
                                             find_effect != 0, set_enabled != 0, set_param != 0);
                    return false;
                }
                s_calls = EngineCalls{
                    .particles = particles,
                    .find_effect = reinterpret_cast<FindEffectFunc>(find_effect),
                    .set_effect_enabled = reinterpret_cast<SetEffectEnabledFunc>(set_enabled),
                    .engine = engine,
                    .set_post_effect_param = reinterpret_cast<SetPostEffectParamFunc>(set_param),
                };
                s_calls_resolved = true;
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        /**
         * @brief The overlay object while its vtable is still the one proven at capture, else nullptr.
         * @details The game's getter caches the object for the process and never clears it. The vtable word is still
         *          read before each use, so an object that no longer holds the proven vtable is dropped instead of
         *          called. The hooks hand over the next object on its first HideOverlays or ShowOverlays.
         */
        [[nodiscard]] const UiOverlay *live_overlay() noexcept
        {
            if (s_overlay.object == 0)
            {
                return nullptr;
            }
            if (read_pointer(s_overlay.object) != s_overlay.vtable)
            {
                (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                         "ScreenEffects: the overlay object changed; waiting for the next one");
                s_overlay = UiOverlay{};
                return nullptr;
            }
            return &s_overlay;
        }

        /**
         * @brief Turns one group's screen particles on or off.
         * @details Turning off first asks FindEffect for each effect (definitions only, no resources), because the
         *          enable slot finds only an effect whose library is loaded, and a library the game loads later would
         *          otherwise bring its effect in enabled. Turning on is skipped while a game UI holds the overlays
         *          hidden: the game's own ShowOverlays turns every screen particle back on when that UI closes.
         */
        void set_particle_group(const ParticleGroup &group, bool hidden) noexcept
        {
            if (!hidden && overlay_state().active.load(std::memory_order_relaxed))
            {
                return;
            }
            for (const char *name : group.names)
            {
                if (hidden)
                {
                    (void)s_calls.find_effect(s_calls.particles, name, k_find_source, false);
                }
                s_calls.set_effect_enabled(s_calls.particles, name, !hidden);
            }
        }

        void update_particles(std::uint32_t wanted) noexcept
        {
            for (const ParticleGroup &group : k_particle_groups)
            {
                const std::uint32_t bit = screen_effect_bit(group.effect);
                const bool hidden = (wanted & bit) != 0;
                if (s_particles_synced && hidden == ((s_particles_hidden & bit) != 0))
                {
                    continue;
                }
                set_particle_group(group, hidden);
                s_particles_hidden = hidden ? (s_particles_hidden | bit) : (s_particles_hidden & ~bit);
            }
            s_particles_synced = true;
        }

        /**
         * @brief Holds each hidden overlay layer at alpha 0 and gives a shown one back its 100.
         * @details The layer's type field tells when the game gave it a new clip (a new type, or a type after none),
         *          and a new clip starts at 100, so a hidden layer is set to 0 again then. A layer without a clip
         *          needs no call: its next clip is caught by the type change.
         */
        void update_layers(const UiOverlay &overlay, std::uint32_t wanted) noexcept
        {
            for (std::size_t i = 0; i < k_overlay_layers.size(); ++i)
            {
                const OverlayLayer &entry = k_overlay_layers[i];
                LayerState &state = s_layers[i];
                const auto type_value = DMK::memory::read<std::int32_t>(DMK::Address{
                    overlay.object + Constants::UI_OVERLAY_LAYER_TYPE_OFFSET +
                    static_cast<std::ptrdiff_t>(entry.layer) * static_cast<std::ptrdiff_t>(sizeof(std::int32_t))});
                const std::int32_t type = type_value ? *type_value : 0;
                const bool hidden = (wanted & screen_effect_bit(entry.effect)) != 0;
                if (hidden)
                {
                    if (type != 0 && (!s_layers_synced || !state.hidden || type != state.type))
                    {
                        overlay.set_alpha(overlay.object, entry.layer, 0);
                    }
                }
                else if (state.hidden || (!s_layers_synced && type != 0))
                {
                    overlay.set_alpha(overlay.object, entry.layer, Constants::UI_OVERLAY_ALPHA_SHOWN);
                }
                state = LayerState{.type = type, .hidden = hidden};
            }
            s_layers_synced = true;
        }

        /**
         * @brief Holds the water drops at 0 while the game asks for them.
         * @details The game sets WaterDroplets_Amount (forced) from its HUD update while a splash runs, and records the
         *          amount on the overlay object. A forced value is the main thread's last write of the frame, so this
         *          override holds only when the frustum build runs after the HUD update in the same frame.
         */
        void hold_droplets(const UiOverlay &overlay) noexcept
        {
            const auto amount =
                DMK::memory::read<float>(DMK::Address{overlay.object + Constants::UI_OVERLAY_DROPLETS_OFFSET});
            if (amount && *amount > 0.0f)
            {
                s_calls.set_post_effect_param(s_calls.engine, k_droplets_param, 0.0f, true);
            }
        }
    } // namespace

    std::uint32_t parse_screen_effect_mask(std::string_view csv)
    {
        std::uint32_t mask = 0;
        TokenList::for_each(csv,
                            [&](std::string_view token)
                            {
                                if (TokenList::equals(token, "helmet"))
                                {
                                    mask |= screen_effect_bit(ScreenEffect::Helmet);
                                }
                                else if (TokenList::equals(token, "hit"))
                                {
                                    mask |= screen_effect_bit(ScreenEffect::Hit);
                                }
                                else if (TokenList::equals(token, "wash"))
                                {
                                    mask |= screen_effect_bit(ScreenEffect::Wash);
                                }
                                else if (TokenList::equals(token, "sleepy"))
                                {
                                    mask |= screen_effect_bit(ScreenEffect::Sleepy);
                                }
                                else if (TokenList::equals(token, "headblood"))
                                {
                                    mask |= screen_effect_bit(ScreenEffect::HeadBlood);
                                }
                                else if (!TokenList::equals(token, "none"))
                                {
                                    DMK::log().warning("Config: a HideScreenEffects list ignores unknown effect '{}' "
                                                       "(Helmet, Hit, Wash, Sleepy, HeadBlood or None)",
                                                       std::string(token));
                                }
                            });
        return mask;
    }

    void capture_ui_overlay(void *ui_overlay) noexcept
    {
        const auto object = reinterpret_cast<std::uintptr_t>(ui_overlay);
        if (object == 0 || object == s_overlay.object || s_overlay_refused)
        {
            return;
        }
        try
        {
            const ModuleInfo &game = module_info();
            const std::uintptr_t hide = gated_anchor_address(Feature::OverlayState, AnchorId::OverlayHide);
            const std::uintptr_t show = gated_anchor_address(Feature::OverlayState, AnchorId::OverlayShow);
            const std::uintptr_t vtable = read_pointer(object);
            const bool class_proven =
                vtable != 0 && hide != 0 && show != 0 &&
                read_image_vtable_slot(vtable, Constants::UI_OVERLAY_HIDE_SLOT, game.base, game.size) == hide &&
                read_image_vtable_slot(vtable, Constants::UI_OVERLAY_SHOW_SLOT, game.base, game.size) == show;
            const std::uintptr_t set_alpha =
                class_proven ? read_proven_vtable_slot(vtable, Constants::UI_OVERLAY_SET_ALPHA_SLOT,
                                                       Proof::k_uiOverlaySetAlpha, game.base, game.size)
                             : 0;
            if (set_alpha == 0)
            {
                s_overlay_refused = true;
                (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                         "ScreenEffects: the UI overlay object did not match (class {}, SetAlpha {}); "
                                         "the helmet and hit overlays stay as the game shows them",
                                         class_proven, set_alpha != 0);
                return;
            }
            const bool first = s_overlay.vtable == 0;
            s_overlay =
                UiOverlay{.object = object, .vtable = vtable, .set_alpha = reinterpret_cast<SetAlphaFunc>(set_alpha)};
            s_layers_synced = false;
            (void)DMK::log().try_log(first ? DMK::LogLevel::Info : DMK::LogLevel::Debug,
                                     "ScreenEffects: UI overlay object at 0x{:X}", object);
        }
        catch (...)
        {
        }
    }

    void reassert_screen_effects() noexcept
    {
        if (!s_calls_resolved || s_particles_hidden == 0)
        {
            return;
        }
        for (const ParticleGroup &group : k_particle_groups)
        {
            if ((s_particles_hidden & screen_effect_bit(group.effect)) != 0)
            {
                set_particle_group(group, true);
            }
        }
    }

    void update_screen_effects(std::uintptr_t genv, bool third_person) noexcept
    {
        const LiveSettings &cfg = settings();
        const std::uint32_t wanted = third_person ? cfg.hide_screen_effects_tpv_mask.load(std::memory_order_relaxed)
                                                  : cfg.hide_screen_effects_fpv_mask.load(std::memory_order_relaxed);
        const bool calls = resolve_engine_calls(genv);
        if (calls)
        {
            update_particles(wanted);
        }
        const UiOverlay *overlay = live_overlay();
        if (overlay == nullptr)
        {
            return;
        }
        update_layers(*overlay, wanted);
        if (calls && (wanted & screen_effect_bit(ScreenEffect::Wash)) != 0)
        {
            hold_droplets(*overlay);
        }
    }
} // namespace TPVCamera
