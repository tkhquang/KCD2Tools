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

namespace TPVCamera
{

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

        // Log level drives Logger verbosity directly on load() and reload().
        settings_section.bind_log_level("LogLevel", Constants::DEFAULT_LOG_LEVEL);

        // Start-of-session auto-enable flags, read once during init(). TPV is ON by default (this is a
        // third-person camera, so it engages once gameplay is reached, not in menus/loading); orbit stays off.
        settings_section.bind<bool>("AutoEnableTPV", "Auto Enable TPV", s.auto_enable_tpv, true);
        settings_section.bind<bool>("AutoEnableOrbit", "Auto Enable Orbit", s.auto_enable_orbit, false);

        // Advanced: RTTI self-heal search radius (see offset_heal.cpp). Not for normal users.
        advanced.bind<int>("SelfHealWindow", "Self Heal Window", s.self_heal_window, 0x100);

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

        // State-driven camera policy. The four *State values are comma-separated GameState token lists
        // (Menu, Overlay, Combat, Mount, Dialogue, Minigame; Dice is an alias for Minigame), parsed into
        // bit masks by parse_state_mask. bind_parsed is the library's INI-string-to-atomic-uint32 binding:
        // it applies the parse at registration with the default below and again on every load() / reload(),
        // storing the result relaxed, so editing a list in the INI re-applies live and the parse stays
        // idempotent. parse_state_mask is already the pure string_view -> uint32 function it wants.
        state_behavior.bind<bool>("EnableStateBehavior", "Enable State Behavior", s.enable_state_behavior, true);
        state_behavior.bind_parsed(
            "ForcedFPVState", "Forced FPV State", s.forced_fpv_mask, parse_state_mask,
            "Aiming,Cart,Dice,Reading,Alchemy,Blacksmithing,ForgeBuilder,Sharpening,StoneThrowing,BattleArchery");
        state_behavior.bind_parsed("ForcedTPVState", "Forced TPV State", s.forced_tpv_mask, parse_state_mask, "");
        state_behavior.bind_parsed("OrbitExcludeState", "Orbit Exclude State", s.orbit_exclude_mask, parse_state_mask,
                                   "Menu,Overlay,Cart,Combat,Mount,Minigame");
        state_behavior.bind<float>("StateSwitchHoldSeconds", "State Switch Hold Seconds", s.state_switch_hold_seconds,
                                   0.2f);
        // SuppressTPVState is the always-on HARD gate (read in should_apply_view): in any listed state the
        // TPV offset is suppressed and cannot be toggled back on. Separate from the edge-triggered Forced*
        // masks above and NOT gated by EnableStateBehavior. All states are honored (Menu/Overlay instant,
        // every other state debounced).
        state_behavior.bind_parsed("SuppressTPVState", "Suppress TPV State", s.suppress_tpv_mask, parse_state_mask,
                                   "Overlay");

        // Preset manager (always active). PresetBlendSpeed is the exponential ease rate used when
        // switching presets on a state edge.
        presets.bind<float>("PresetBlendSpeed", "Preset Blend Speed", s.preset_blend_speed, 8.0f);
    }

} // namespace TPVCamera
