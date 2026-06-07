/**
 * @file tpv_camera.cpp
 * @brief Mod lifecycle: configuration, hooks, and input bindings.
 *
 * init() and shutdown() are invoked by DMK::Bootstrap from a dedicated worker
 * thread, so the heavy setup and teardown run off the Windows loader lock.
 */

#include "tpv_camera.hpp"
#include "config.hpp"
#include "constants.hpp"
#include "global_state.hpp"
#include "game_interface.hpp"
#include "version.hpp"
#include "hooks/camera_hook.hpp"
#include "hooks/ui_overlay_hooks.hpp"
#include "hooks/ui_menu_hooks.hpp"
#include "hooks/interaction_hook.hpp"
#include "hooks/player_onaction_hook.hpp"
#include "presets/preset_store.hpp"
#include "overlay/overlay.hpp"

#include <DetourModKit.hpp>

#include <string>

#include <windows.h>
#include <psapi.h>

#include <chrono>
#include <functional>
#include <string_view>
#include <vector>

namespace TPVCamera
{

// RAII guards for the press bindings registered via DMK::Config::register_press_combo
// (cleared during shutdown so the callbacks cannot run during teardown).
static std::vector<DMK::Config::InputBindingGuard> s_binding_guards;

/**
 * @brief Returns true when a menu or overlay is up; the hotkeys ignore presses then.
 */
[[nodiscard]] static bool is_ui_blocking_input()
{
    // The preset overlay also blocks the camera hotkeys while it wants the keyboard/mouse, so a
    // press consumed by ImGui (e.g. typing a preset name) never doubles as a camera toggle.
    return is_game_menu_open() || overlay_state().active.load(std::memory_order_relaxed) ||
           Overlay::wants_input();
}

/**
 * @brief Engages free-look orbit, seeding the orbit angles to the configured centre.
 * @details Seeds yaw/pitch to 0,0 (directly behind the player -- the camera's resting offset) so a
 *          fresh engage always starts from the centred pose, then flips orbit_active on. Shared by
 *          the orbit toggle, the momentary hold binding, and the start-of-session auto-enable so
 *          "engage" means exactly the same thing at every entry point.
 */
static void orbit_engage()
{
    CameraState &cam = camera_state();
    cam.orbit_yaw.store(0.0f);
    cam.orbit_pitch.store(0.0f);
    cam.orbit_active.store(true);
}

/**
 * @brief Disengages free-look orbit and drops any stranded movement-input latch.
 * @details Turning orbit off clears no run-state on its own, so a latch left > 0 (a held-move
 *          release swallowed on a combat action-map swap) would re-trip the body-turn the next time
 *          orbit engages; player_onaction_reset() drops it here. Shared by the orbit toggle and the
 *          hold-release path.
 */
static void orbit_disengage()
{
    camera_state().orbit_active.store(false);
    const float stranded = player_onaction_reset();
    DMK::Logger::get_instance().trace("Orbit: disengaged; cleared move-latch (had magnitude {:.2f})", stranded);
}

/**
 * @brief Resolves the game module base and size into TPVCamera::module_info().
 * @details Polls for the module for up to ~3 seconds because an ASI can attach
 *          fractionally before WHGame.dll finishes mapping.
 */
static bool validate_game_module()
{
    DMK::Logger &logger = DMK::Logger::get_instance();

    HMODULE game_module = nullptr;
    for (int i = 0; i < 30 && !game_module; ++i)
    {
        game_module = GetModuleHandleA(Constants::MODULE_NAME);
        if (!game_module)
            Sleep(100);
    }

    if (!game_module)
    {
        logger.error("Failed to find module: {}", Constants::MODULE_NAME);
        return false;
    }

    MODULEINFO mod_info = {};
    if (!GetModuleInformation(GetCurrentProcess(), game_module, &mod_info, sizeof(mod_info)))
    {
        logger.error("Failed to get module information: {}", GetLastError());
        return false;
    }

    ModuleInfo &mod = module_info();
    mod.base = reinterpret_cast<uintptr_t>(mod_info.lpBaseOfDll);
    mod.size = mod_info.SizeOfImage;

    if (mod.size == 0)
    {
        logger.error("Module has zero size");
        return false;
    }

    logger.info("Module validated: {} (Size: {} bytes)", DMK::Format::format_address(mod.base), mod.size);
    return true;
}

/**
 * @brief Installs every game hook in dependency order.
 * @details The third-person camera hook is mandatory (it is the mod). The view-flag
 *          interface and the menu/overlay suppression hooks degrade to a warning on
 *          failure: a missing view flag is treated as not-built-in-TPV, and missing
 *          menu/overlay detection only means the offset is not auto-suppressed under UI.
 */
static bool initialize_hooks()
{
    DMK::Logger &logger = DMK::Logger::get_instance();
    const ModuleInfo &mod = module_info();

    // Built-in view flag, read by the camera gate to avoid stacking on the engine's own TPV.
    if (!initialize_game_interface(mod.base, mod.size))
    {
        logger.warning("Game interface initialization failed - built-in TPV detection disabled");
    }

    if (!initialize_ui_menu_hooks(mod.base, mod.size))
    {
        logger.warning("UI Menu hooks initialization failed - menu suppression disabled");
    }

    if (!initialize_ui_overlay_hooks(mod.base, mod.size))
    {
        logger.warning("UI Overlay hooks initialization failed - overlay suppression disabled");
    }

    if (!initialize_interaction_hook(mod.base, mod.size))
    {
        logger.warning("Interaction hook initialization failed - camera-space interaction disabled");
    }

    // Device-agnostic movement intent for the orbit move-detection. Best-effort: a miss falls back to
    // body-position speed, so the camera still works without it.
    if (!initialize_player_onaction_hook(mod.base, mod.size))
    {
        logger.warning("Player OnAction hook initialization failed - orbit move-detection uses body speed");
    }

    // The third-person camera itself. A hard failure here means the mod cannot function.
    if (!initialize_camera(mod.base, mod.size))
    {
        logger.error("Critical: third-person camera hook installation failed - mod cannot function");
        return false;
    }

    return true;
}

/**
 * @brief Registers the press bindings, fusing each INI key with its InputManager
 *        binding via DMK::Config::register_press_combo.
 * @details The returned RAII guards are stashed so the callbacks stay live until
 *          shutdown. All callbacks act on the camera state; rendering is
 *          additionally gated by should_apply_view(), so a state flip under a
 *          menu is harmless, but the toggles still ignore presses while a UI is up so
 *          the hotkey cannot fire from under an inventory/dialog.
 */
static void register_press_bindings()
{
    auto add_press = [](std::string_view section, std::string_view ini_key, std::string_view log_name,
                        std::string_view binding, std::function<void()> on_press, std::string_view default_combo)
    {
        s_binding_guards.push_back(DMK::Config::register_press_combo(
            section, ini_key, log_name, binding, std::move(on_press), default_combo));
    };

    // Enter/exit third-person at runtime. Default: F3, or hold LB + press RB on a controller.
    add_press("Settings", "ToggleViewKey", "Toggle View Key", "toggle_view",
              [] {
                  if (is_ui_blocking_input())
                      return;
                  CameraState &cam = camera_state();
                  const bool new_state = !cam.applying.load();
                  cam.applying.store(new_state);
                  DMK::Logger::get_instance().info("Third-person camera {}", new_state ? "ENABLED" : "DISABLED");
              }, "F3,Gamepad_LB+Gamepad_RB");

    // Force first-person (turn the offset off). Always allowed: turning the camera off is safe.
    add_press("Settings", "ForceFPVKey", "Force FPV Key", "force_fpv",
              [] {
                  camera_state().applying.store(false);
                  DMK::Logger::get_instance().info("Third-person camera DISABLED (force FPV)");
              }, "");

    // Force third-person (turn the offset on).
    add_press("Settings", "ForceTPVKey", "Force TPV Key", "force_tpv",
              [] {
                  if (is_ui_blocking_input())
                      return;
                  camera_state().applying.store(true);
                  DMK::Logger::get_instance().info("Third-person camera ENABLED (force TPV)");
              }, "");

    // Free-look orbit: press to TOGGLE on/off. While on, the mouse orbits the camera around the
    // character (the look stays put), you can still move and act, and starting to move turns the
    // character to face the camera direction. The momentary OrbitHoldKey below is the alternative
    // freelook style (hold to engage, release to return). OrbitExcludeState only auto-disables
    // free-look when you ENTER those states (e.g. mounting); it does not forbid manually turning it
    // back on there (Combat and Mount are excluded because free-look currently fights the game's own
    // camera control in those states).
    // Default: F4, or hold LB + click the left stick (LS) on a controller.
    add_press("Orbit", "OrbitToggleKey", "Orbit Toggle Key", "orbit_toggle",
              [] {
                  if (is_ui_blocking_input())
                      return;
                  const bool new_state = !camera_state().orbit_active.load();
                  if (new_state)
                      orbit_engage();
                  else
                      orbit_disengage();
                  DMK::Logger::get_instance().info("Orbit camera {}", new_state ? "ENABLED" : "DISABLED");
              }, "F4,Gamepad_LB+Gamepad_LS");

    // Open/close the preset-manager overlay. Always allowed (it is the mod's own UI), so it can be
    // opened over a game menu; the camera keeps rendering live so preset edits are visible.
    add_press("Settings", "ToggleOverlayKey", "Toggle Overlay Key", "toggle_overlay",
              [] { Overlay::toggle(); }, "Home");
}

/**
 * @brief Registers the hold bindings after the INI key lists have been loaded.
 * @details DMK has no hold-combo helper, so the INI keys are parsed by register_config_items and
 *          the InputManager hold bindings are created here from the resulting lists. The zoom
 *          callbacks are empty -- the frustum-builder detour queries their hold state by name each
 *          frame to drive the follow distance. The orbit-hold callback instead engages/releases
 *          free-look directly on its key edges (momentary freelook), so nothing polls it per frame.
 */
static void register_hold_bindings()
{
    DMK::InputManager &input_mgr = DMK::InputManager::get_instance();
    input_mgr.register_hold(k_zoom_in_binding, g_config.zoom_in_keys, [](bool) {});
    input_mgr.register_hold(k_zoom_out_binding, g_config.zoom_out_keys, [](bool) {});

    // Momentary free-look (freelook, as in ArmA / DayZ / PUBG): hold OrbitHoldKey to engage the
    // orbit and release to return to the precise camera-aim view. register_hold fires the callback
    // with true on the press edge and false on release (and false at shutdown for an active hold),
    // so the press engages and the release disengages. s_engaged_by_hold records whether THIS hold
    // turned orbit on, so the release only undoes what the hold engaged: if orbit was already
    // toggled on via OrbitToggleKey, the press is a no-op and the release leaves it on. The engage
    // is gated on the UI like the toggle; the release is always honoured (even under UI) so a
    // momentary orbit can never get stuck on. The callback runs only on the input poll thread (and,
    // for the shutdown release, after that thread has joined), so the static needs no lock.
    input_mgr.register_hold(k_orbit_hold_binding, g_config.orbit_hold_keys,
                            [](bool pressed) {
                                static bool s_engaged_by_hold = false;
                                if (pressed)
                                {
                                    if (is_ui_blocking_input() || camera_state().orbit_active.load())
                                        return;
                                    orbit_engage();
                                    s_engaged_by_hold = true;
                                    DMK::Logger::get_instance().info("Orbit camera ENABLED (hold)");
                                }
                                else if (s_engaged_by_hold)
                                {
                                    orbit_disengage();
                                    s_engaged_by_hold = false;
                                    DMK::Logger::get_instance().info("Orbit camera DISABLED (hold released)");
                                }
                            });
}

/**
 * @brief Starts the INI hot-reload watcher.
 * @details The register_atomic setters re-apply the live settings on each reload,
 *          so the callback only reports the outcome.
 */
static void enable_hot_reload()
{
    DMK::Logger &logger = DMK::Logger::get_instance();

    const DMK::Config::AutoReloadStatus status = DMK::Config::enable_auto_reload(
        std::chrono::milliseconds{250}, [](bool content_changed) {
            DMK::Logger &reload_logger = DMK::Logger::get_instance();
            if (content_changed)
                reload_logger.info("INI auto-reload: live settings applied");
            else
                reload_logger.info("INI auto-reload: no content change");
        });

    if (status == DMK::Config::AutoReloadStatus::Started)
        logger.info("INI hot-reload watcher started (250 ms debounce)");
    else
        logger.warning("INI hot-reload watcher not started (status {})", static_cast<int>(status));
}

bool init()
{
    DMK::Logger &logger = DMK::Logger::get_instance();
    logger.info("----------------------------------------");
    Version::log_version_info();

    // Register every config item and the press bindings, then load and log once.
    register_config_items();
    register_press_bindings();
    DMK::Config::load(Constants::get_config_filename());
    DMK::Config::log_all();

    // Camera presets are user-owned and created automatically: the file is seeded from the embedded
    // factory defaults on first run, any missing built-in is re-added, and a corrupt file falls back to
    // those defaults, so loading never fails and never blocks the mod. The built-in state presets
    // (DEFAULT/COMBAT/AIMING/MOUNT/STEALTH) and any user presets feed the render-thread resolver via the
    // published binding table.
    const std::string presets_path =
        DMK::Filesystem::get_runtime_directory_utf8() + "\\" + Constants::get_presets_filename();
    Presets::PresetStore::instance().load(presets_path);

    // Memory cache is a hot-path accelerator; a failure is non-fatal because the
    // readability checks fall back to direct VirtualQuery calls.
    if (DMK::Memory::init_cache())
        logger.info("Memory cache system initialized");
    else
        logger.warning("Memory cache init failed; readability checks fall back to syscalls");

    if (!validate_game_module())
        return false;

    if (!initialize_hooks())
        return false;

    // Hold bindings need the loaded key lists; register them before starting the poll.
    register_hold_bindings();
    DMK::InputManager::get_instance().start();
    logger.info("InputManager started");

    enable_hot_reload();

    // Apply the start-of-session auto-enable flags (read once here; disabled by default). The view
    // gate (should_apply_view) still suppresses the offset under menus/loading, so an auto-enabled
    // view simply eases in once gameplay is reached. Orbit engages with the camera (it is gated on
    // the offset being active), starting from the centred 0,0 angle.
    {
        const LiveSettings &startup = settings();
        CameraState &cam = camera_state();
        if (startup.auto_enable_tpv.load(std::memory_order_relaxed))
        {
            cam.applying.store(true, std::memory_order_relaxed);
            logger.info("AutoEnableTPV: third-person camera enabled on start");
        }
        if (startup.auto_enable_orbit.load(std::memory_order_relaxed))
        {
            orbit_engage();
            logger.info("AutoEnableOrbit: free-look orbit enabled on start");
        }
    }

    // Start the preset-manager overlay (self-hosted ImGui). A failure is non-fatal: the camera and
    // its hotkeys still work, only the in-game preset editor is unavailable.
    if (Overlay::start())
        logger.info("Preset overlay started (toggle with ToggleOverlayKey)");
    else
        logger.warning("Preset overlay failed to start; presets still apply from JSON/INI");

    logger.info("Initialization completed successfully");
    return true;
}

void shutdown()
{
    DMK::Logger &logger = DMK::Logger::get_instance();
    logger.info("Shutdown: starting teardown");

    // Stop the INI watcher first so no reload setter runs during teardown.
    DMK::Config::disable_auto_reload();

    // Stop the overlay UI thread before touching the preset store so no UI mutation races teardown.
    Overlay::stop();

    // Persist any unsaved preset edits.
    Presets::PresetStore::instance().flush();

    // Disable the press callbacks so they cannot run during teardown.
    s_binding_guards.clear();

    // DMK_Shutdown() removes every HookManager hook on its own (production: Bootstrap
    // calls it after this returns; dev: the logic DLL's Shutdown() calls it), so the
    // inline/mid hooks are not removed here. Only the game interface's resolved context
    // pointer is cleared.
    cleanup_game_interface();

    logger.info("Shutdown: teardown complete");
}

} // namespace TPVCamera
