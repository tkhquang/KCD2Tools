/**
 * @file config.hpp
 * @brief Configuration model and registration for the TPV Camera mod.
 *
 * @details LiveSettings holds every value read on a hot path (the per-frame detour) or across
 *          threads. They are std::atomic so INI hot-reload (which runs the setters on the
 *          library's auto-reload watcher thread) never races the game thread, and are bound with
 *          DMK::config::bind / bind_parsed through a section binder. The input bindings (press and
 *          hold, including their key combos) are registered separately in tpv_camera.cpp via
 *          DMK::config::press_combo / hold_combo.
 */
#ifndef TPVCAMERA_CONFIG_HPP
#define TPVCAMERA_CONFIG_HPP

#include "core/policy/state_policy.hpp"

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
        // Preset-owned framing: the main-thread preset resolver OVERWRITES these every active frame
        // (apply_to_live) and the frustum-builder detour reads them. They are NOT INI settings - they
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
        std::atomic<float> follow_yaw{};   // static resting orbit yaw around the pivot, degrees (0 = directly behind)
        std::atomic<float> follow_pitch{}; // static resting orbit pitch around the pivot, degrees (0 = level)
        std::atomic<float> arm_follow_up{}; // looking up: share of the look pitch the arm follows (CameraPreset)
        std::atomic<float> lens_drop_up{};  // looking up: how far the camera is lowered, meters (CameraPreset)
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
        std::atomic<float> gamepad_orbit_curve{};   // right-stick response exponent (CameraPreset)
        std::atomic<float> gamepad_orbit_smoothing{}; // right-stick rate settle time, seconds (CameraPreset)
        std::atomic<float> orbit_pitch_min{};       // lowest free-look pitch, degrees
        std::atomic<float> orbit_pitch_max{};       // highest free-look pitch, degrees
        std::atomic<float> orbit_return_speed{};    // ease-back-to-center speed on release (0 = stay)
        std::atomic<float> orbit_follow_speed{};    // mount speed (m/s) at which riding turns free-look back at
                                                    // orbit_return_speed (0 = off)
        std::atomic<float>
            orbit_smoothing{}; // free-look angle low-pass strength, 0..1 (0 = off/raw, higher = smoother but more lag)
        std::atomic<bool> orbit_level_aim{};        // level the real player aim/head while orbiting
        std::atomic<bool> orbit_body_turn{};        // force the character body to face the camera heading while moving
        std::atomic<bool> orbit_continuous_align{}; // GTA-style: body continuously follows the camera while moving
                                                    // (false = capture heading once, hold it)
        std::atomic<bool> freeze_orbit_on_cursor{
            true}; // hold the free-look orbit still while the game shows the OS cursor (a UI is up: menu / loot / trade
                   // / dialogue) so cursor motion does not turn the camera

        // Camera collision. Enable/Skin/ReturnSpeed/Shoulder are preset-owned (seeded/overwritten like the framing
        // above); CollisionRadius, the occluder fade, the tree see-through, HeadClearance and the close-up fade are
        // always-live INI settings, so they keep their registered defaults.
        std::atomic<bool> enable_collision{};        // keep the view out of walls
        std::atomic<float> collision_skin{};         // gap kept from the surface any probe found, meters
        std::atomic<float> collision_return_speed{}; // ease-out speed once an obstruction clears (0 = instant)
        std::atomic<bool> shoulder_collision{};      // narrow the shoulder offset at a wall before the arm
        std::atomic<float> collision_radius{0.15f};  // the probes' half-width (the fan's ray offset), meters
        // Beam gate, a flex of the rule that whatever matches no visible object stops the camera: such a hit stops it
        // only when all five of the fan's rays are blocked, by it or by anything else, so an unknown collider narrower
        // than the probe with open space beside it (a post, a pole, a stray proxy) is walked past without a fade, and
        // whatever stands behind it still counts. Terrain and the objects the camera rules class keep their
        // behaviour. An object about the probe's width can block four rays on one frame and five on the next; the
        // 0.3 s collision hold bridges that. Off = every unmatched hit stops the camera.
        std::atomic<bool> use_beam_gate{false};
        // How long a pull-in takes to settle, seconds. 0 = at once: the camera never trails the surface that stopped it
        // by more than CollisionSkin less the lens margin, so a big pull-in is one frame's jump. Above 0 it eases in,
        // ~95% of the way in this long, while it moves through open air: the view of the character can be blocked
        // for that long, and the lens never passes into or through a surface (where it would, the pull-in is at once).
        std::atomic<float> collision_pull_in_seconds{0.12f};
        // Occluder fade: props between the camera and the character (posts, fences, open carts, lamps) dither out
        // instead of pulling the camera in front of them, so the camera keeps its distance. The camera passes through
        // bushes and tree trunks, but a bush or a tree never fades whole for hiding the character (the tree see-through
        // opens a window in it), and a bush fades only when the camera is buried in it. Terrain, buildings, walls and
        // rocks (as the camera rules class them, see core/rules/camera_rules.hpp) and objects that match no scanned
        // render node still stop the camera. Off = plain collision on the nearest solid surface.
        std::atomic<bool> use_occluder_fade{true};
        // How much of a faded object always stays: 0 (it can almost vanish) .. 1 (stays solid). A prop or a bush the
        // camera is buried in, or a prop whose physics the camera is inside, starts fading at once but keeps this
        // too; what only touches the lens is cut by the near clip plane (use_lens_cut).
        std::atomic<float> occluder_fade_min_opacity{0.2f};
        // Tree see-through (see hooks/shader_twins.hpp): the trees, bushes and grass between the camera and the
        // character open a dithered window around the character, only where they stand nearer than him. The rest of
        // the plant and its shadow stay. Independent of the occluder fade. Off = trees and bushes stay solid (the
        // leaves right at the lens are still cut while use_lens_cut is on).
        std::atomic<bool> use_tree_see_through{true};
        // How much of the plants in the window always stays: 0 (fully open, with a dithered rim) .. 1 (stays solid).
        std::atomic<float> tree_see_through_min_opacity{0.2f};
        // The window's size relative to the character: 0.5 .. 2.
        std::atomic<float> tree_see_through_size{1.0f};
        // Where the window starts, as a share of the character's height above his feet: 0 (his feet) .. 1 (the top of
        // his head, so nothing opens); 0.5 = his hips.
        std::atomic<float> tree_see_through_start_height{0.5f};
        // The dithered rim around the window, as a share of the window's size: 0 (hard edge) .. 1 (as wide again).
        std::atomic<float> tree_see_through_softness{0.35f};
        // Lens cut (see camera/lens.hpp): in third person the near clip plane is pushed out, held short of the
        // ground, buildings and the character's head, and the trees, bushes and bare frames close to the lens thin
        // away in the dither (the near cut), so a leaf, a branch or a beam right at the lens does not fill the view.
        // Off = the game's own near plane, and no near cut.
        std::atomic<bool> use_lens_cut{true};
        // First-person fallback: when collision leaves the camera less than this many meters from the pivot (a low
        // lintel in a doorway, a wall right behind), it would sit inside the player's head. Instead it eases onto the
        // real eye and the head is hidden by the game's own first-person rig, until the camera has this much room
        // plus a small margin again. With close_up_fade the fade below replaces that switch and this only places it.
        // 0 = the camera needs no room: no switch, and the fade reaches its full amount against the head.
        // Live-editable.
        std::atomic<float> head_clearance{0.35f};
        // Close-up fade: instead of the first-person switch above, the character dithers out as the camera closes in on
        // it (from HeadClearance plus a band down to HeadClearance), so the view never jumps to the eye. Needs the
        // character fade hook; without it the first-person switch stays.
        std::atomic<bool> close_up_fade{true};
        // How much of the character always stays in the close-up fade: 0 (he can vanish) .. 1 (he stays solid).
        std::atomic<float> close_up_fade_min_opacity{0.25f};
        // People and animals whose drawn parts come within 1.2 m of the lens in the view dither out with what they
        // carry, fully from 0.45 m in, where the lens near plane would otherwise cut them open
        // (camera/npc_lens_fade.hpp); NpcLensFadeMinOpacity of them always stays.
        std::atomic<bool> npc_lens_fade{false};
        std::atomic<float> npc_lens_fade_min_opacity{0.25f};

        // State-driven camera policy (see game_state.hpp). Each mask is a GameState bit set parsed
        // from a comma-separated INI token list, read on the per-frame detour and the input thread.
        // The policy is EDGE-triggered: forced_fpv_mask / forced_tpv_mask switch the view ONCE when a
        // listed state begins (forced-FPV wins on overlap); the player may still toggle manually while
        // the state lasts, and the pre-entry view is restored when the state ends unless the player
        // changed it meanwhile. orbit_exclude_mask suspends free-look on entry to a listed state and
        // restores it on exit. A state takes effect the frame it starts; state_exit_hold_seconds is how long one
        // must have ended before the camera leaves it, so a brief gap does not trigger a switch.
        std::atomic<bool> enable_state_behavior{true};
        std::atomic<uint32_t> forced_fpv_mask{0};
        std::atomic<uint32_t> forced_tpv_mask{0};
        std::atomic<uint32_t> orbit_exclude_mask{0};
        std::atomic<float> state_exit_hold_seconds{0.2f};

        // Continuous HARD suppression gate (INDEPENDENT of enable_state_behavior): while the game is in
        // any listed state the TPV offset is suppressed and CANNOT be toggled back on, unlike the
        // edge-triggered forced_*_mask policy. Menu and Overlay are read from the live UI signals so they
        // suppress instantly (no debounce frame of TPV in a menu); every other state is matched against
        // the debounced game-state mask. Default seeded from the INI (Menu,Overlay).
        std::atomic<uint32_t> suppress_tpv_mask{0};

        // States in which the native turn-in-place animation is switched off (the body is locked to the look again,
        // as in first person). A continuous gate like suppress_tpv_mask, NOT gated by enable_state_behavior. Default
        // seeded from the INI (the states whose own camera or script drives the body: combat, mounts, dialogue,
        // minigames, aiming, and the scripted stances).
        std::atomic<uint32_t> native_turn_exclude_mask{0};

        // Whether reloading a crossbow (spanning a bolt) already counts as the Aiming state. The game raises its aim
        // flag the moment prepare is pressed, so with this off the Aiming state (the AIMING preset and every rule keyed
        // on Aiming) waits until the reload is done; on keeps the game's own timing.
        std::atomic<bool> reload_counts_as_aiming{false};

        // Preset manager (see presets/). Presets are always active: the main-thread resolver selects a
        // camera preset by the debounced game state (DEFAULT/COMBAT/AIMING/MOUNT/STEALTH) - or by the
        // overlay's editing pin - and eases the preset-owned framing fields above toward it each active
        // frame, OVERWRITING them. Those framing fields are NOT INI settings; they live in the presets
        // JSON (created automatically from embedded defaults, see presets/default_presets.hpp) and are
        // tuned in the overlay. preset_blend_in_seconds is the active preset's blend time (CameraPreset::
        // blend_in_seconds), which the FOV ease follows too.
        std::atomic<float> preset_blend_in_seconds{0.52f};
        // The PresetBlendSpeed key of an INI from before the per-preset blend times, read only to migrate a presets
        // file of schema 2 or older (PresetStore::load); the shipped INI no longer has it.
        std::atomic<float> legacy_preset_blend_speed{8.0f};

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

        // Native turn-in-place animation (always-live, NOT preset-owned). The mod keeps the game in first person,
        // where the body is locked to the look and never plays its turn animations. While the third-person view is
        // engaged this reports "third person" to the free-roam locomotion action only, so the body lags the look until
        // it leads by native_turn_angle and then turns with the game's own turn-in-place animations until it faces the
        // look. The turn's steps are kept on the spot, so the camera does not move during or after the turn.
        std::atomic<bool> native_turn_animation{true};
        // How far (degrees) the look can lead the body before a turn starts; the game's own value is 35. Every turn
        // then continues until the body faces the look. Clamped to 10..90.
        std::atomic<float> native_turn_angle{35.0f};
        // Seconds the camera must rest before the body also turns to face a smaller lead (more than 12 degrees but less
        // than native_turn_angle). 0 = only turn past native_turn_angle.
        std::atomic<float> native_turn_settle_delay{0.8f};

        // Ranged (always-live, NOT preset-owned). The player's shot leaves the bow along the bow's own axis, so in
        // third person it misses the screen center by the shoulder parallax. ranged_aim_at_crosshair re-aims it as the
        // first-person shot with the crosshair on the point the crosshair ray hits, so it lands where that shot lands
        // (third person only, never in free-look); scatter shot's pellets are turned with their volley and keep the
        // game's spread. ranged_max_correction (degrees) leaves a shot untouched when
        // the re-aim lies further than that from the bow's own aim, and also limits the sideways turn that brings the
        // landing in line with the crosshair. ranged_show_trail draws each player shot's flight path, the crosshair
        // point and the impact for ranged_trail_seconds.
        std::atomic<bool> ranged_aim_at_crosshair{true};
        std::atomic<float> ranged_max_correction{45.0f};
        std::atomic<bool> ranged_show_trail{false};
        std::atomic<float> ranged_trail_seconds{8.0f};
        // Drop of the player's own shots per weapon, in either view, as a multiple of the game's gravity (the same for
        // every ammo): 1 keeps the game's arc, 0 flies straight. Bow arrows, crossbow bolts, the handgonne's ball and
        // scatter shot. A third-person shot comes down straight above or below the crosshair point by its weapon's
        // drop.
        std::atomic<float> ranged_gravity_bow{1.0f};
        std::atomic<float> ranged_gravity_crossbow{1.0f};
        std::atomic<float> ranged_gravity_firearm{1.0f};
        // The aim preview: where the shot or throw will come down while a bow is held drawn (or a crossbow raised) in
        // third person, or a distraction stone is held ready in first or third person, as a ring on that spot and
        // (ranged_aim_preview_arc) a dashed arc from the bow or hand, in ranged_aim_preview_color (0xRRGGBB) at
        // ranged_aim_preview_opacity, drawn over walls unless ranged_aim_preview_through_walls is off.
        std::atomic<bool> ranged_show_aim_preview{false};
        std::atomic<bool> ranged_aim_preview_arc{true};
        std::atomic<float> ranged_aim_preview_opacity{0.6f};
        std::atomic<bool> ranged_aim_preview_through_walls{true};
        std::atomic<uint32_t> ranged_aim_preview_color{0xFFE196u};

        // Start-of-session auto-enable flags, read ONCE during init().
        std::atomic<bool> auto_enable_tpv{true};    // enter third-person automatically on game start
        std::atomic<bool> auto_enable_orbit{false}; // engage free-look orbit automatically on game start

        // Advanced. Per-side search radius (bytes) the RTTI self-heal scans around each nominal offset to
        // recover a field after a game patch shifts the struct layout (see offset_heal.cpp). Read once when
        // the heal runs (not a hot path); clamped to the DMK maximum. Larger tolerates a bigger insertion at
        // a slightly higher risk of a wrong heal onto a same-typed neighbour. Not for normal users to touch.
        std::atomic<int> self_heal_window{0x100};

        // Advanced. Writes every built-in signature, with the baselines captured from the live game, to
        // KCD2_TPVCamera.signatures.captured.ini at startup (see export_signatures in aob_resolver.hpp). Read once
        // after the anchors resolve.
        std::atomic<bool> export_signatures{false};

        // Advanced. Writes the built-in camera rules, byte for byte, to KCD2_TPVCamera_rules.default.txt (a reference
        // the mod never reads) at startup, and again each time an INI reload turns it on (see export_built_in in
        // camera_rules_files.hpp).
        std::atomic<bool> export_rules{false};
    };

    /** @brief Returns the process-wide live (atomic) settings. */
    [[nodiscard]] LiveSettings &settings() noexcept;

    /**
     * @brief The [StateBehavior] lists as they stand now, one row per situation (see core/policy/state_policy.hpp).
     * @details Reads the five lists and EnableStateBehavior once each (relaxed: each is a value of its own, and a list
     *          an INI reload changes mid-read takes effect a frame later). Any thread.
     */
    [[nodiscard]] StatePolicyTable state_policy() noexcept;

    /**
     * @brief Registers the log level, the LiveSettings atomics, and the state-policy masks with DMK::config.
     * @details Must be called before DMK::config::load(). The input bindings (press and hold) are
     *          registered separately in tpv_camera.cpp.
     */
    void register_config_items();

} // namespace TPVCamera

#endif // TPVCAMERA_CONFIG_HPP
