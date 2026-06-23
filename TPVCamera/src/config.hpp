/**
 * @file config.hpp
 * @brief Configuration model and registration for the TPV Camera mod.
 *
 * @details LiveSettings holds every value read on a hot path (the per-frame detour) or across
 *          threads. They are std::atomic so INI hot-reload (which runs the setters on the
 *          ConfigWatcher thread) never races the game thread, and are bound with
 *          DMK::Config::register_atomic. The input bindings (press and hold, including their key
 *          combos) are registered separately in tpv_camera.cpp via DMK::Config::register_press_combo /
 *          register_hold_combo.
 */
#ifndef TPVCAMERA_CONFIG_HPP
#define TPVCAMERA_CONFIG_HPP

#include <atomic>
#include <cstdint>

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
        std::atomic<float>
            orbit_sensitivity_x{}; // free-look MOUSE yaw sensitivity multiplier (mouse delta * this; negative inverts)
        std::atomic<float> orbit_sensitivity_y{}; // free-look MOUSE pitch sensitivity multiplier (mouse delta * this;
                                                  // negative inverts)
        std::atomic<float>
            gamepad_orbit_speed_x{}; // free-look GAMEPAD right-stick yaw rate, deg/sec at full deflection (neg inverts)
        std::atomic<float> gamepad_orbit_speed_y{}; // free-look GAMEPAD right-stick pitch rate, deg/sec at full
                                                    // deflection (neg inverts)
        std::atomic<float> orbit_pitch_min{};       // lowest free-look pitch, degrees
        std::atomic<float> orbit_pitch_max{};       // highest free-look pitch, degrees
        std::atomic<float> orbit_return_speed{};    // ease-back-to-center speed on release (0 = stay)
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
        // above); UseCoverageCollision, UseSphereCollision, CollisionRadius and the gates below are always-live
        // INI settings, so they keep their registered defaults.
        std::atomic<bool> enable_collision{};        // keep the view out of walls
        std::atomic<float> collision_skin{};         // gap kept before a hit surface, meters (thin-ray path only)
        std::atomic<float> collision_return_speed{}; // ease-out speed once an obstruction clears
        // Master switch for the COVERAGE-based collision heuristics: the coverage gate (CoverageThreshold + the
        // HeadVisibleSkip head-priority gate) that only collides when something actually hides the character, and
        // the lateral frustum-clearance probe (CameraProbeSize). ON by default: the camera sees through thin
        // props / rails the body shows past and stays out of corners; set false for plain collision (the camera
        // stops at the nearest solid surface, no coverage measurement, no lateral probe). Render occlusion is
        // INDEPENDENT of this (its own use_render_occlusion toggle below). Costs extra per-frame geometry
        // inspection in dense scenes, so it is the first thing to disable if camera collision causes FPS drops.
        std::atomic<bool> use_coverage_collision{true};
        // Swept-sphere collision via PrimitiveWorldIntersection: the sphere's contact distance is
        // continuous as the sweep grazes edges, so the camera does not pump in dense geometry the way a
        // single thin ray does. The radius IS the standoff (collision_skin is not applied on this path).
        // Falls back to the thin ray automatically when the engine sweep is unavailable or faults.
        std::atomic<bool> use_sphere_collision{true}; // swept sphere (PWI) vs single thin ray (RWI)
        std::atomic<float> collision_radius{0.15f};   // swept-sphere radius = standoff from surfaces, meters
        // Coverage gate: only collide when an obstruction actually HIDES the character. At a candidate hit the
        // camera samples the character silhouette and casts to each sample; if the occluded fraction is below
        // this threshold (0..1) the obstruction is ignored, so thin poles / rails that leave most of the
        // character visible never jolt the camera (native-TPV feel), while a wall or a near post that hides most
        // of it still collides. Distance-aware (a closer object hides more), unlike a fixed width/size cutoff.
        // 0 = OFF (collide on any hit). Higher ignores more; default 0.5 = clamp once about HALF the character is
        // hidden (0.8 was too permissive -- the body could be ~80% buried before the camera reacted; note the
        // head-weighting means a fully-covered UPPER HALF reads ~0.73, so a 0.8 threshold also needs lower-body
        // occlusion on top). Only consulted while use_coverage_collision is ON. Live-editable.
        std::atomic<float> collision_coverage_threshold{0.5f};
        // Head-priority skip: if at least this FRACTION of the character's HEAD silhouette (the top coverage
        // band) is still VISIBLE, the camera does NOT collide on that occluder regardless of total coverage --
        // "if I can see his head, leave the camera alone". Only when the head is more covered than this does the
        // normal CoverageThreshold gate decide. It can only SUPPRESS a clamp, never force one, so it makes the
        // camera strictly more permissive (tolerates body occlusion while the head shows). 0 = OFF (the
        // total-coverage gate alone decides). Higher tolerates LESS head occlusion before deferring to the gate
        // (e.g. 0.35 = "clamp once the head is >~65% covered"). Default 0.10 = very permissive: pull in only once
        // the head is ~90% hidden. Only consulted while use_coverage_collision is ON. Live-editable.
        std::atomic<float> head_visible_skip{0.10f};
        // Lateral / frustum clearance: the pivot->camera collision probes only ALONG the arm, so a wall BESIDE
        // the camera (a corner, a doorway, a narrow gap) is never seen and intrudes into the view. After the
        // along-arm pull-in, the camera probes its lateral surroundings and pulls further in until a converging
        // side wall is at least this many meters away. It is a geometric clearance, not occlusion, so it ignores
        // the coverage gate (a side wall does not hide the character) and only ever sees static / terrain world
        // (never the player or NPCs). ~0.3 keeps the view out of corner walls; 0 = OFF. Live-editable.
        std::atomic<float> camera_probe_size{0.3f};
        // Render occlusion: also collide the camera with render-only geometry (tent / awning canopy cloth,
        // overhead brushes) that carries no ray-collidable physics, by querying the 3DEngine render octree
        // (GetObjectsInBox) along the pivot->camera arm and clamping below an overhead brush. Always-live INI
        // setting (not preset-owned). Enabling it is INDEPENDENT of use_coverage_collision, BUT when coverage
        // collision is ON it also applies the coverage gate (CoverageThreshold) to render brushes, keyed on the
        // real projected player silhouette: a thin prop the body is visible past (a pole / bird feeder) is then
        // dropped instead of clamping the camera, while a view-burying canopy still clamps. No-ops if the octree
        // is unresolved. Queries the render octree per frame, so disable it too if camera collision costs FPS.
        std::atomic<bool> use_render_occlusion{true};

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

        // Camera stability (always-live, NOT preset-owned). The third-person rig (camera = pivot - forward *
        // distance) amplifies any rotation of the basis into a position swing; the EyeHeight body anchor removes
        // the POSITIONAL bob, these remove the ROTATIONAL component the engine bakes into the eye quat during
        // animations (head-bob and weapon-sway rotation, combat / hit / landing view-shake). StableAimBasis
        // builds the rig basis from the player's clean look-controller aim quat instead of that shaking eye quat
        // (the look carries none of it and equals the eye at rest), falling back to the eye quat if the look
        // chain cannot be resolved. AimBasisSmoothing then low-passes the result (0 = off; higher = smoother but
        // slightly laggier aim). Both default ON. (The look briefly diverges from the rendered view during some
        // scripted animations such as climbing, so a small residual shift can remain there; this is preferred
        // over the much larger general view-shake that follow-the-eye would otherwise reintroduce.)
        std::atomic<bool> stable_aim_basis{true};
        std::atomic<float> aim_basis_smoothing{0.3f};

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
     * @brief Registers the log level, the LiveSettings atomics, and the state-policy strings with DMK::Config.
     * @details Must be called before DMK::Config::load(). The input bindings (press and hold) are
     *          registered separately in tpv_camera.cpp.
     */
    void register_config_items();

} // namespace TPVCamera

#endif // TPVCAMERA_CONFIG_HPP
