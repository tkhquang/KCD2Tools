/**
 * @file config.hpp
 * @brief Configuration model and registration for the TPV Camera mod.
 *
 * @details Settings split by access pattern:
 *          - LiveSettings holds every value read on a hot path (the per-frame
 *            detour) or across threads. They are std::atomic so INI hot-reload
 *            (which runs the setters on the ConfigWatcher thread) never races the
 *            game thread. They are bound with DMK::Config::register_atomic.
 *          - Config holds the key-combo lists for the hold bindings (zoom and the
 *            momentary free-look orbit), parsed by DMK::Config and consumed once when
 *            the InputManager hold bindings are registered. The press bindings (view
 *            toggle, force FPV/TPV, orbit toggle) are registered separately via
 *            DMK::Config::register_press_combo.
 */
#ifndef TPVCAMERA_CONFIG_HPP
#define TPVCAMERA_CONFIG_HPP

#include <DetourModKit.hpp>

#include <atomic>
#include <cstdint>

/**
 * @struct Config
 * @brief Key lists for the hold bindings (zoom and momentary free-look orbit).
 */
struct Config
{
    // Hold-binding key lists (parsed by DMK::Config; consumed once when the InputManager hold
    // bindings are registered). The detour queries the zoom lists by name per frame to drive the
    // follow distance; the orbit-hold list instead drives a momentary free-look engage on the press
    // edge and a release on the release edge (register_hold is edge-triggered), so it is never polled.
    DMK::Config::KeyComboList zoom_in_keys;
    DMK::Config::KeyComboList zoom_out_keys;
    DMK::Config::KeyComboList orbit_hold_keys;
};

/** @brief Process-wide init-only configuration (defined in config.cpp). */
extern Config g_config;

namespace TPVCamera
{
    /**
     * @struct LiveSettings
     * @brief Hot-path and cross-thread camera settings as atomics so INI hot-reload
     *        is race-free against the game thread.
     */
    struct LiveSettings
    {
        // Preset-owned framing: the render-thread preset resolver OVERWRITES these every active frame
        // (apply_to_live) and the frustum-builder detour reads them. They are NOT INI settings -- they
        // live in the presets JSON and are tuned in the overlay. The factory default VALUES live in
        // CameraPreset (camera_preset.hpp, equal to the built-in DEFAULT preset); settings() seeds these
        // atomics from a default-constructed CameraPreset at startup, so they are only value-initialized
        // here. Do NOT add literal defaults: they would duplicate CameraPreset and drift from it.
        std::atomic<float> follow_distance{};     // follow distance, units behind the pivot
        std::atomic<float> follow_distance_min{}; // closest the zoom keys allow
        std::atomic<float> follow_distance_max{}; // farthest the zoom keys allow
        std::atomic<float> zoom_step{};           // distance change per second while a zoom key is held
        std::atomic<float> offset_up{};           // pivot raise along the world-up axis
        std::atomic<float>
            eye_height{}; // anchor height above the player body origin (0 = anchor to the bobbing FP eye)
        std::atomic<bool> dynamic_eye_sync{}; // re-anchor eye height to the real FP eye on low poses (kneel/pray) out
                                              // of EyeHeight range
        std::atomic<float> offset_right{};    // pivot shift along the camera right axis
        std::atomic<float> aim_focus_distance{}; // crosshair convergence depth, meters (0 = track follow distance)
        std::atomic<float> follow_yaw{};   // static resting orbit yaw around the pivot, degrees (0 = directly behind)
        std::atomic<float> follow_pitch{}; // static resting orbit pitch around the pivot, degrees (0 = level)
        std::atomic<float> fov{};          // third-person field of view, degrees (0 = use the game FOV)

        // Free-look orbit. The orbit-feel values are preset-owned (seeded/overwritten like the framing
        // above); freeze_orbit_on_cursor is an always-live INI setting (not preset-owned), so it keeps
        // its registered default.
        std::atomic<float> orbit_sensitivity{};   // free-look MOUSE sensitivity multiplier (mouse delta * this)
        std::atomic<float> gamepad_orbit_speed{}; // free-look GAMEPAD right-stick rate, deg/sec at full deflection
        std::atomic<float> orbit_pitch_min{};     // lowest free-look pitch, degrees
        std::atomic<float> orbit_pitch_max{};     // highest free-look pitch, degrees
        std::atomic<float> orbit_return_speed{};  // ease-back-to-center speed on release (0 = stay)
        std::atomic<float>
            orbit_smoothing{}; // free-look angle low-pass strength, 0..1 (0 = off/raw, higher = smoother but more lag)
        std::atomic<bool> orbit_level_aim{};        // level the real player aim/head while orbiting
        std::atomic<bool> orbit_body_turn{};        // force the character body to face the camera heading while moving
        std::atomic<bool> orbit_continuous_align{}; // GTA-style: body continuously follows the camera while moving
                                                    // (false = capture heading once, hold it)
        std::atomic<bool> freeze_orbit_on_cursor{
            true}; // hold the free-look orbit still while the game shows the OS cursor (a UI is up: menu / loot / trade
                   // / dialogue) so cursor motion does not turn the camera

        // Camera collision. Enable/Skin/ReturnSpeed are preset-owned (seeded/overwritten like the framing
        // above); UseSphereCollision, CollisionRadius and the thin-skip cap below are always-live INI
        // settings, so they keep their registered defaults.
        std::atomic<bool> enable_collision{};        // keep the view out of walls
        std::atomic<float> collision_skin{};         // gap kept before a hit surface, meters (thin-ray path only)
        std::atomic<float> collision_return_speed{}; // ease-out speed once an obstruction clears
        // Swept-sphere collision via PrimitiveWorldIntersection: the sphere's contact distance is
        // continuous as the sweep grazes edges, so the camera does not pump in dense geometry the way a
        // single thin ray does. The radius IS the standoff (collision_skin is not applied on this path).
        // Falls back to the thin ray automatically when the engine sweep is unavailable or faults.
        std::atomic<bool> use_sphere_collision{true}; // swept sphere (PWI) vs single thin ray (RWI)
        std::atomic<float> collision_radius{0.15f};   // swept-sphere radius = standoff from surfaces, meters
        // Skip standalone THIN scenery during camera collision: if a hit physics entity's smallest world-AABB
        // dimension is below this many meters, the camera ignores it and re-casts to the surface behind, so a
        // lone stick / pole / thin sign becomes transparent like grass. 0 (or empty in the INI) = feature OFF
        // (no bbox reads). Cannot help multi-rail FENCES -- those are one large mesh. Live-editable.
        std::atomic<float> collision_thin_skip_max{0.0f};

        // State-driven camera policy (see game_state.hpp). Each mask is a GameState bit set parsed
        // from a comma-separated INI token list, read on the per-frame detour and the input thread.
        // The policy is EDGE-triggered: forced_fpv_mask / forced_tpv_mask switch the view ONCE when a
        // listed state begins (forced-FPV wins on overlap); the player may still toggle manually while
        // the state lasts, and the pre-entry view is restored when the state ends unless the player
        // changed it meanwhile. orbit_exclude_mask suspends free-look on entry to a listed state and
        // restores it on exit. state_switch_hold_seconds debounces edges so a brief transition does
        // not trigger a switch.
        std::atomic<bool> enable_state_behavior{true};
        std::atomic<uint32_t> forced_fpv_mask{0};
        std::atomic<uint32_t> forced_tpv_mask{0};
        std::atomic<uint32_t> orbit_exclude_mask{0};
        std::atomic<float> state_switch_hold_seconds{0.2f};

        // Continuous HARD suppression gate (INDEPENDENT of enable_state_behavior): while the game is in
        // any listed state the TPV offset is suppressed and CANNOT be toggled back on, unlike the
        // edge-triggered forced_*_mask policy. Menu and Overlay are read from the live UI signals so they
        // suppress instantly (no debounce frame of TPV in a menu); every other state is matched against
        // the debounced game-state mask. Default seeded from the INI (Menu,Overlay).
        std::atomic<uint32_t> suppress_tpv_mask{0};

        // Preset manager (see presets/). Presets are always active: the render-thread resolver selects a
        // camera preset by the debounced game state (DEFAULT/COMBAT/AIMING/MOUNT/STEALTH) -- or by the
        // overlay's editing pin -- and eases the preset-owned framing fields above toward it each active
        // frame, OVERWRITING them. Those framing fields are NOT INI settings; they live in the presets
        // JSON (created automatically from embedded defaults, see presets/default_presets.hpp) and are
        // tuned in the overlay. preset_blend_speed is the exponential ease rate (higher = snappier;
        // <= 0 snaps instantly).
        std::atomic<float> preset_blend_speed{8.0f};

        // Always-live "Camera" INI settings (NOT preset-owned; apply_to_live never writes them).
        // Re-origin the player use/interaction cone onto the render camera + crosshair (third person)
        // so the use-target matches screen centre at all ranges.
        std::atomic<bool> interact_from_camera{true};

        // First-person <-> third-person view-switch easing. The frustum detour eases a 0..1 blend
        // toward the target view over this many seconds (smoothstepped) so toggling and UI
        // suppression slide instead of snapping; 0 = instant switch.
        std::atomic<float> view_transition_duration{0.0f};

        // Start-of-session auto-enable flags, read ONCE during init(). Disabled by default.
        std::atomic<bool> auto_enable_tpv{false};   // enter third-person automatically on game start
        std::atomic<bool> auto_enable_orbit{false}; // engage free-look orbit automatically on game start

        // Advanced. Per-side search radius (bytes) the RTTI self-heal scans around each nominal offset to
        // recover a field after a game patch shifts the struct layout (see offset_heal.cpp). Read once when
        // the heal runs (not a hot path); clamped to the DMK maximum. Larger tolerates a bigger insertion at
        // a slightly higher risk of a wrong heal onto a same-typed neighbour. Not for normal users to touch.
        std::atomic<int> self_heal_window{0x100};
    };

    /** @brief Returns the process-wide live (atomic) settings. */
    [[nodiscard]] LiveSettings &settings() noexcept;

    /**
     * @brief Registers every non-press configuration item with DMK::Config.
     * @details Registers the log level, the LiveSettings atomics, and the zoom
     *          hold-binding key lists. Must be called before DMK::Config::load().
     *          Press bindings are registered separately.
     */
    void register_config_items();

} // namespace TPVCamera

#endif // TPVCAMERA_CONFIG_HPP
