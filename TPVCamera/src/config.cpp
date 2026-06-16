/**
 * @file config.cpp
 * @brief Configuration registration for the TPV Camera mod using DMK::Config.
 *
 * Registration order is: log level, LiveSettings atomics, then the zoom
 * hold-binding key lists. Press bindings (view toggle, force FPV/TPV, orbit) are
 * registered separately (see tpv_camera.cpp). DMK::Config::load() / log_all() are
 * driven by the mod lifecycle once every item is registered.
 */

#include "config.hpp"
#include "constants.hpp"
#include "game_state.hpp"
#include "hooks/camera_hook.hpp"
#include "presets/camera_preset.hpp"

#include <DetourModKit.hpp>

// Process-wide init-only configuration, shared with the hooks.
Config g_config;

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
        // Log level drives Logger verbosity directly on load() and reload().
        DMK::Config::register_log_level("Settings", "LogLevel", Constants::DEFAULT_LOG_LEVEL);

        LiveSettings &s = settings();

        // Start-of-session auto-enable flags, read once during init(). TPV is ON by default (this is a
        // third-person camera, so it engages once gameplay is reached, not in menus/loading); orbit stays off.
        DMK::Config::register_atomic<bool>("Settings", "AutoEnableTPV", "Auto Enable TPV", s.auto_enable_tpv, true);
        DMK::Config::register_atomic<bool>("Settings", "AutoEnableOrbit", "Auto Enable Orbit", s.auto_enable_orbit,
                                           false);

        // Advanced: RTTI self-heal search radius (see offset_heal.cpp). Not for normal users.
        DMK::Config::register_atomic<int>("Advanced", "SelfHealWindow", "Self Heal Window", s.self_heal_window, 0x100);

        // Camera framing. The follow distance, offsets, eye height, aim focus, follow yaw/pitch, the orbit
        // tuning, and the per-preset collision values are all OWNED BY PRESETS (in the shipped presets JSON,
        // applied to the live atomics each frame), so they are NOT INI settings -- tune them in the overlay.
        // Only the non-preset, always-live camera options remain here:
        DMK::Config::register_atomic<bool>("Camera", "InteractFromCamera", "Interact From Camera",
                                           s.interact_from_camera, true);
        DMK::Config::register_atomic<float>("Camera", "ViewTransitionDuration", "View Transition Duration",
                                            s.view_transition_duration, 0.0f);

        // Free-look orbit (non-preset, always-live; the orbit feel values are per-preset).
        DMK::Config::register_atomic<bool>("Orbit", "FreezeOrbitOnCursor", "Freeze Orbit On Cursor",
                                           s.freeze_orbit_on_cursor, true);
        // OrbitHoldKey is the MOMENTARY free-look key (freelook): hold it to engage the orbit and release
        // it to return to the precise camera-aim view, separate from the press-to-toggle OrbitToggleKey.
        // It is a hold binding, so (like the zoom keys) the list is stashed in g_config for
        // register_hold_bindings to consume once, and the setter re-binds the live hold in place on a
        // hot-reload via update_binding_combos (see ZoomInKey for why that second call is needed). Empty
        // by default so it is opt-in and never collides with a game key or the toggle binding.
        DMK::Config::register_key_combo(
            "Orbit", "OrbitHoldKey", "Orbit Hold Key",
            [](const DMK::Config::KeyComboList &c)
            {
                g_config.orbit_hold_keys = c;
                DMK::InputManager::get_instance().update_binding_combos(k_orbit_hold_binding, c);
            },
            "");

        // Camera collision (non-preset, always-live; Enable/Skin/ReturnSpeed are per-preset). UseCoverageCollision
        // is the master switch for the coverage gate and the lateral probe (render occlusion is independent); OFF
        // reverts to plain nearest-solid collision.
        DMK::Config::register_atomic<bool>("Collision", "UseCoverageCollision", "Use Coverage Collision",
                                           s.use_coverage_collision, false);
        DMK::Config::register_atomic<bool>("Collision", "UseSphereCollision", "Use Sphere Collision",
                                           s.use_sphere_collision, true);
        DMK::Config::register_atomic<float>("Collision", "CollisionRadius", "Collision Radius", s.collision_radius,
                                            0.15f);
        DMK::Config::register_atomic<float>("Collision", "CoverageThreshold", "Coverage Threshold",
                                            s.collision_coverage_threshold, 0.8f);
        DMK::Config::register_atomic<float>("Collision", "CameraProbeSize", "Camera Probe Size", s.camera_probe_size,
                                            0.3f);
        DMK::Config::register_atomic<bool>("Collision", "UseRenderOcclusion", "Use Render Occlusion",
                                           s.use_render_occlusion, true);

        // State-driven camera policy. The three *State values are comma-separated GameState token lists
        // (Menu, Overlay, Combat, Mount, Dialogue, Minigame; Dice is an alias for Minigame), parsed into
        // bit masks by parse_state_mask. The setters run on load and on every hot-reload, so editing a
        // list in the INI re-applies live; each setter overwrites its mask, so the parse is idempotent.
        DMK::Config::register_atomic<bool>("StateBehavior", "EnableStateBehavior", "Enable State Behavior",
                                           s.enable_state_behavior, true);
        DMK::Config::register_string(
            "StateBehavior", "ForcedFPVState", "Forced FPV State", [&mask = s.forced_fpv_mask](const std::string &value)
            { mask.store(parse_state_mask(value), std::memory_order_relaxed); },
            "Aiming,Cart,Dice,Reading,Alchemy,Blacksmithing,ForgeBuilder,Sharpening,StoneThrowing,BattleArchery");
        DMK::Config::register_string(
            "StateBehavior", "ForcedTPVState", "Forced TPV State", [&mask = s.forced_tpv_mask](const std::string &value)
            { mask.store(parse_state_mask(value), std::memory_order_relaxed); }, "");
        DMK::Config::register_string(
            "StateBehavior", "OrbitExcludeState", "Orbit Exclude State",
            [&mask = s.orbit_exclude_mask](const std::string &value)
            { mask.store(parse_state_mask(value), std::memory_order_relaxed); },
            "Menu,Overlay,Cart,Combat,Mount,Minigame");
        DMK::Config::register_atomic<float>("StateBehavior", "StateSwitchHoldSeconds", "State Switch Hold Seconds",
                                            s.state_switch_hold_seconds, 0.2f);
        // SuppressTPVState is the always-on HARD gate (read in should_apply_view): in any listed state the
        // TPV offset is suppressed and cannot be toggled back on. Separate from the edge-triggered Forced*
        // masks above and NOT gated by EnableStateBehavior. All states are honored (Menu/Overlay instant,
        // every other state debounced).
        DMK::Config::register_string(
            "StateBehavior", "SuppressTPVState", "Suppress TPV State",
            [&mask = s.suppress_tpv_mask](const std::string &value)
            { mask.store(parse_state_mask(value), std::memory_order_relaxed); }, "Overlay");

        // Preset manager (always active). PresetBlendSpeed is the exponential ease rate used when
        // switching presets on a state edge.
        DMK::Config::register_atomic<float>("Presets", "PresetBlendSpeed", "Preset Blend Speed", s.preset_blend_speed,
                                            8.0f);

        // Zoom hold keys (the detour polls them by name each frame to drive the follow distance).
        // Defaults: LShift+PageUp / LShift+PageDown on keyboard, or hold LB + D-pad up/down on a controller.
        // The setters keep g_config in sync (read once by register_hold_bindings to create the InputManager
        // hold binding) AND call update_binding_combos so a hot-reload of the keys re-binds the live hold in
        // place. Without that second call the reload would update g_config but leave the hold binding on its
        // startup keys, so editing ZoomIn/OutKey would not apply until restart (unlike the press combos, whose
        // register_press_combo already re-binds on reload).
        DMK::Config::register_key_combo(
            "Camera", "ZoomInKey", "Zoom In Key",
            [](const DMK::Config::KeyComboList &c)
            {
                g_config.zoom_in_keys = c;
                DMK::InputManager::get_instance().update_binding_combos(k_zoom_in_binding, c);
            },
            "LShift+PageUp,Gamepad_LB+Gamepad_DpadUp");
        DMK::Config::register_key_combo(
            "Camera", "ZoomOutKey", "Zoom Out Key",
            [](const DMK::Config::KeyComboList &c)
            {
                g_config.zoom_out_keys = c;
                DMK::InputManager::get_instance().update_binding_combos(k_zoom_out_binding, c);
            },
            "LShift+PageDown,Gamepad_LB+Gamepad_DpadDown");
    }

} // namespace TPVCamera
