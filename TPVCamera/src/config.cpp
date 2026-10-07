/**
 * @file config.cpp
 * @brief Configuration registration for the TPV Camera mod using DMK::config.
 *
 * Registration order is: log level, then the LiveSettings atomics and the state-policy
 * masks. Every item is bound through a DMK::config::SectionBinder, so each INI section name is
 * written once and the keys under it read as a group. The input bindings (press and hold) are
 * registered separately by the mod lifecycle in tpv_camera.cpp, since their callbacks act on the
 * camera state. DMK::config::load() / log_all() are driven by the lifecycle once every item is
 * registered.
 */

#include "config.hpp"
#include "constants.hpp"
#include "game_state.hpp"
#include "presets/camera_preset.hpp"

#include <DetourModKit.hpp>

#include <charconv>
#include <system_error>

namespace TPVCamera
{
    namespace
    {
        // The aim preview's default color, pale gold: as the INI spells it, and as 0xRRGGBB.
        constexpr std::string_view k_aim_preview_color_text = "255,225,150";
        constexpr uint32_t k_aim_preview_color = 0xFFE196u;
        constexpr uint32_t k_max_color_channel = 255;

        /** @brief The default pale gold, with a warning for a value that was given but is not "r,g,b". */
        [[nodiscard]] uint32_t rgb_default(std::string_view text)
        {
            if (!text.empty())
            {
                DMK::log().warning("Config: AimPreviewColor '{}' is not r,g,b (each 0 to 255); using {}",
                                   std::string(text), k_aim_preview_color_text);
            }
            return k_aim_preview_color;
        }

        /**
         * @brief "r,g,b" (each 0..255, spaces allowed around each value) to 0xRRGGBB.
         * @details Parsed with std::from_chars, so the result does not depend on the locale. Anything else gives the
         *          default pale gold with a warning.
         */
        [[nodiscard]] uint32_t parse_rgb(std::string_view text)
        {
            uint32_t rgb = 0;
            std::string_view rest = text;
            for (int channel = 0; channel < 3; ++channel)
            {
                const std::size_t comma = rest.find(',');
                const bool last = channel == 2;
                // Exactly two commas: none after the last channel, one after each other.
                if (last != (comma == std::string_view::npos))
                {
                    return rgb_default(text);
                }
                const std::string field = DMK::string::trim(rest.substr(0, comma));
                uint32_t value = 0;
                const auto [end, error] = std::from_chars(field.data(), field.data() + field.size(), value);
                if (field.empty() || error != std::errc{} || end != field.data() + field.size() ||
                    value > k_max_color_channel)
                {
                    return rgb_default(text);
                }
                rgb = (rgb << 8) | value;
                rest = last ? std::string_view{} : rest.substr(comma + 1);
            }
            return rgb;
        }
    } // namespace

    LiveSettings &settings() noexcept
    {
        static LiveSettings s;
        // Seed the preset-owned atomics from the factory DEFAULT exactly once on first access. A default-
        // constructed CameraPreset carries the DEFAULT framing values (camera_preset.hpp is the single source
        // of those values, equal to the built-in DEFAULT preset), so the live atomics start at the factory
        // default until the render-thread resolver applies the per-state preset each active frame. Seeding here
        // rather than via literal initializers keeps the default values in one place so they cannot drift.
        // s has static storage, so the lambda references it directly: a simple capture of a static-storage
        // variable is ill-formed (MSVC C3495 under /permissive-).
        [[maybe_unused]] static const bool seeded = []
        {
            Presets::apply_to_live(Presets::CameraPreset{}, s);
            return true;
        }();
        return s;
    }

    void register_config_items()
    {
        LiveSettings &s = settings();

        // One SectionBinder per INI section: the section name is written once and every key under it binds
        // through the same handle, so a section rename is a one-line change and a key cannot be filed under
        // the wrong section by a typo in a repeated literal.
        const DMK::config::SectionBinder settings_section = DMK::config::section("Settings");
        const DMK::config::SectionBinder advanced = DMK::config::section("Advanced");
        const DMK::config::SectionBinder camera = DMK::config::section("Camera");
        const DMK::config::SectionBinder orbit = DMK::config::section("Orbit");
        const DMK::config::SectionBinder collision = DMK::config::section("Collision");
        const DMK::config::SectionBinder state_behavior = DMK::config::section("StateBehavior");
        const DMK::config::SectionBinder presets = DMK::config::section("Presets");
        const DMK::config::SectionBinder archery = DMK::config::section("Archery");

        // Log level drives Logger verbosity directly on load() and reload().
        settings_section.bind_log_level("LogLevel", Constants::DEFAULT_LOG_LEVEL);

        // Start-of-session auto-enable flags, read once during init(). TPV is ON by default (this is a
        // third-person camera, so it engages once gameplay is reached, not in menus/loading); orbit stays off.
        settings_section.bind<bool>("AutoEnableTPV", "Auto Enable TPV", s.auto_enable_tpv, true);
        settings_section.bind<bool>("AutoEnableOrbit", "Auto Enable Orbit", s.auto_enable_orbit, false);

        // Advanced: RTTI self-heal search radius (see offset_heal.cpp). Not for normal users.
        advanced.bind<int>("SelfHealWindow", "Self Heal Window", s.self_heal_window, 0x100);
        // Advanced: write the built-in signatures and their captured baselines beside the ASI (see aob_resolver.hpp).
        advanced.bind<bool>("ExportSignatures", "Export Signatures", s.export_signatures, false);

        // Camera framing. The follow distance, offsets, eye height, aim focus, follow yaw/pitch, the orbit
        // tuning, and the per-preset collision values are all OWNED BY PRESETS (in the shipped presets JSON,
        // applied to the live atomics each frame), so they are NOT INI settings - tune them in the overlay.
        // Only the non-preset, always-live camera options remain here:
        camera.bind<bool>("InteractFromCamera", "Interact From Camera", s.interact_from_camera, true);
        camera.bind<float>("ViewTransitionDuration", "View Transition Duration", s.view_transition_duration, 0.0f);
        // Camera stability against engine view-shake amplified by the follow distance (see LiveSettings).
        // StableAimBasis builds the rig basis from the clean look-controller aim quat; AimBasisSmoothing
        // low-passes the basis (0 = off). Both always-live.
        camera.bind<bool>("StableAimBasis", "Stable Aim Basis", s.stable_aim_basis, true);
        camera.bind<float>("AimBasisSmoothing", "Aim Basis Smoothing", s.aim_basis_smoothing, 0.3f);
        // The game's own turn-in-place animations while the third-person view is engaged (see LiveSettings).
        camera.bind<bool>("NativeTurnAnimation", "Native Turn Animation", s.native_turn_animation, true);
        camera.bind<float>("NativeTurnAngle", "Native Turn Angle", s.native_turn_angle, 35.0f);
        camera.bind<float>("NativeTurnSettleDelay", "Native Turn Settle Delay", s.native_turn_settle_delay, 0.8f);

        // Archery (non-preset, always-live): third-person shots and distraction stones aimed at the crosshair point,
        // the flight trail, the aim preview and the per-weapon drop.
        archery.bind<bool>("AimAtCrosshair", "Aim At Crosshair", s.archery_aim_at_crosshair, true);
        archery.bind<float>("MaxAimCorrection", "Max Aim Correction", s.archery_max_correction, 30.0f);
        archery.bind<bool>("ShowArrowTrail", "Show Arrow Trail", s.archery_show_trail, false);
        archery.bind<float>("ArrowTrailSeconds", "Arrow Trail Seconds", s.archery_trail_seconds, 8.0f);
        archery.bind<bool>("ShowAimPreview", "Show Aim Preview", s.archery_show_aim_preview, false);
        archery.bind<bool>("AimPreviewArc", "Aim Preview Arc", s.archery_aim_preview_arc, true);
        archery.bind<float>("AimPreviewOpacity", "Aim Preview Opacity", s.archery_aim_preview_opacity, 0.6f);
        archery.bind<bool>("AimPreviewThroughWalls", "Aim Preview Through Walls", s.archery_aim_preview_through_walls,
                           true);
        archery.bind_parsed("AimPreviewColor", "Aim Preview Color", s.archery_aim_preview_color, parse_rgb,
                            k_aim_preview_color_text);
        archery.bind<float>("BowGravity", "Bow Gravity", s.archery_gravity_bow, 1.0f);
        archery.bind<float>("CrossbowGravity", "Crossbow Gravity", s.archery_gravity_crossbow, 1.0f);
        archery.bind<float>("FirearmGravity", "Firearm Gravity", s.archery_gravity_firearm, 1.0f);

        // Free-look orbit (non-preset, always-live; the orbit feel values are per-preset).
        orbit.bind<bool>("FreezeOrbitOnCursor", "Freeze Orbit On Cursor", s.freeze_orbit_on_cursor, true);

        // Camera collision (non-preset, always-live; Enable/Skin/ReturnSpeed are per-preset). UseCoverageCollision
        // is the master switch for the coverage gate and the lateral probe (render occlusion is independent); OFF
        // reverts to plain nearest-solid collision.
        collision.bind<bool>("UseCoverageCollision", "Use Coverage Collision", s.use_coverage_collision, false);
        collision.bind<bool>("UseSphereCollision", "Use Sphere Collision", s.use_sphere_collision, true);
        collision.bind<float>("CollisionRadius", "Collision Radius", s.collision_radius, 0.15f);
        collision.bind<float>("CoverageThreshold", "Coverage Threshold", s.collision_coverage_threshold, 0.8f);
        collision.bind<float>("CameraProbeSize", "Camera Probe Size", s.camera_probe_size, 0.3f);
        collision.bind<bool>("UseRenderOcclusion", "Use Render Occlusion", s.use_render_occlusion, true);
        collision.bind<float>("HeadClearance", "Head Clearance", s.head_clearance, 0.35f);

        // State-driven camera policy. The four *State values are comma-separated GameState token lists
        // (Menu, Overlay, Combat, Mount, Dialogue, Minigame; Dice is an alias for Minigame), parsed into
        // bit masks by parse_state_mask. bind_parsed is the library's INI-string-to-atomic-uint32 binding:
        // it applies the parse at registration with the default below and again on every load() / reload(),
        // storing the result relaxed, so editing a list in the INI re-applies live and the parse stays
        // idempotent. parse_state_mask is already the pure string_view -> uint32 function it wants.
        state_behavior.bind<bool>("EnableStateBehavior", "Enable State Behavior", s.enable_state_behavior, true);
        state_behavior.bind_parsed(
            "ForcedFPVState", "Forced FPV State", s.forced_fpv_mask, parse_state_mask,
            "Cart,Dice,Reading,Alchemy,Blacksmithing,ForgeBuilder,Sharpening,StoneThrowing,BattleArchery");
        state_behavior.bind_parsed("ForcedTPVState", "Forced TPV State", s.forced_tpv_mask, parse_state_mask, "");
        state_behavior.bind_parsed("OrbitExcludeState", "Orbit Exclude State", s.orbit_exclude_mask, parse_state_mask,
                                   "Overlay,Aiming,Cart,Combat,Mount,Minigame");
        state_behavior.bind<float>("StateSwitchHoldSeconds", "State Switch Hold Seconds", s.state_switch_hold_seconds,
                                   0.2f);
        // Whether a crossbow reload already counts as Aiming (see LiveSettings). Not gated by EnableStateBehavior: it
        // shapes the detected state itself.
        state_behavior.bind<bool>("ReloadCountsAsAiming", "Reload Counts As Aiming", s.reload_counts_as_aiming, false);
        // SuppressTPVState is the always-on HARD gate (read in should_apply_view): in any listed state the
        // TPV offset is suppressed and cannot be toggled back on. Separate from the edge-triggered Forced*
        // masks above and NOT gated by EnableStateBehavior. All states are honored (Menu/Overlay instant,
        // every other state debounced).
        state_behavior.bind_parsed("SuppressTPVState", "Suppress TPV State", s.suppress_tpv_mask, parse_state_mask,
                                   "Overlay");
        // States that switch the native turn animation off: the body re-locks to the look there. A continuous gate
        // like SuppressTPVState, NOT gated by EnableStateBehavior.
        state_behavior.bind_parsed("NativeTurnExcludeState", "Native Turn Exclude State", s.native_turn_exclude_mask,
                                   parse_state_mask, "Combat,Aiming,Mount,Cart,Dialogue,Minigame,Lying,Sitting,Kneel");

        // Preset manager (always active). PresetBlendSpeed is the exponential ease rate used when
        // switching presets on a state edge.
        presets.bind<float>("PresetBlendSpeed", "Preset Blend Speed", s.preset_blend_speed, 8.0f);
    }

} // namespace TPVCamera
