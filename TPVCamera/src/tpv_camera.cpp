/**
 * @file tpv_camera.cpp
 * @brief Mod lifecycle: configuration, hooks, and input bindings.
 *
 * init() runs on the DMK bootstrap worker thread, off the Windows loader lock, and receives the live
 * Session. shutdown() is driven by the host's detach path (dllmain.cpp in production, the logic DLL's
 * Shutdown() export in the dev build) and must likewise run off the loader lock, because hooks are
 * caller-owned and this is the only path that restores the patched prologues.
 */

#include "tpv_camera.hpp"
#include "aob_resolver.hpp"
#include "config.hpp"
#include "constants.hpp"
#include "global_state.hpp"
#include "offset_heal.hpp"
#include "rtti_types.hpp"
#include "game_interface.hpp"
#include "version.hpp"
#include "hooks/archery_hook.hpp"
#include "hooks/camera_hook.hpp"
#include "hooks/ui_overlay_hooks.hpp"
#include "hooks/ui_menu_hooks.hpp"
#include "hooks/interaction_hook.hpp"
#include "hooks/player_onaction_hook.hpp"
#include "hooks/shader_twins.hpp"
#include "presets/preset_store.hpp"
#include "overlay/overlay.hpp"

#include <DetourModKit.hpp>

#include <string>

#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string_view>

namespace TPVCamera
{

    // The Session's binding scope, borrowed for the process lifetime by init(). Every guard from
    // DMK::config::press_combo / hold_combo goes in here rather than a mod-owned vector: ~Session clears
    // the scope FIRST (reverse insertion order) so no callback can run during subsystem teardown, and a
    // still-held hold-combo guard delivers one balancing on_state_change(false) on clear, so a torn-down
    // orbit hold cannot strand free-look on. On the process-termination path Session::abandon() retains
    // the guards and their captures instead of destroying callables under the loader lock.
    static DMK::input::Scope *s_binding_scope = nullptr;

    // Every hook the mod installs. HookSet disables and destroys newest first, the only safe order for layered
    // patches on one target: the newer layer trampoline chains through the older jump, so the base must
    // be restored last. shutdown() retires it while the code pages are still mapped.
    static HookSet s_hooks;

    /**
     * @brief Returns true when a menu or overlay is up; the hotkeys ignore presses then.
     */
    [[nodiscard]] static bool is_ui_blocking_input()
    {
        // The preset overlay also blocks the camera hotkeys while it wants the keyboard/mouse, so a
        // press consumed by ImGui (e.g. typing a preset name) never doubles as a camera toggle.
        return is_game_menu_open() || overlay_state().active.load(std::memory_order_relaxed) || Overlay::wants_input();
    }

    /**
     * @brief Engages free-look orbit from the pose on screen.
     * @details Flips orbit_active on and keeps the orbit angles it has: centered once a return has finished, or
     *          where an OrbitReturnSpeed of 0 or a return still under way left them. The rig takes over from the
     *          camera where it is, so engaging never moves it. Shared by the orbit toggle, the momentary hold
     *          binding, and the start-of-session auto-enable so "engage" means exactly the same thing at every
     *          entry point.
     */
    static void orbit_engage()
    {
        // A hold released before this engage does not concern the orbit-exclude policy.
        camera_state().orbit_hold_released.store(false);
        camera_state().orbit_active.store(true);
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
        DMK::log().trace("Orbit: disengaged; cleared move-latch (had magnitude {:.2f})", stranded);
    }

    /**
     * @brief Resolves the game module base and size into TPVCamera::module_info().
     * @details Region::module_named is the library's module-range lookup; it reports the image's current extent
     *          and yields an empty region (size 0) when the module is not mapped, so it doubles as the presence
     *          test. Polls for up to ~3 seconds because an ASI can attach fractionally before WHGame.dll
     *          finishes mapping. Every scan and identity sweep in the mod is confined to the region this stores.
     */
    static DMK::Result<void> validate_game_module()
    {
        DMK::Logger &logger = DMK::log();

        DMK::Region image{};
        for (int i = 0; i < 30 && image.size == 0; ++i)
        {
            image = DMK::Region::module_named(Constants::MODULE_NAME);
            if (image.size == 0)
                Sleep(100);
        }

        if (image.size == 0)
        {
            logger.error("Failed to find module: {}", Constants::MODULE_NAME);
            return std::unexpected(DMK::Error{DMK::ErrorCode::ProcessMismatch, "TPVCamera::init/module"});
        }

        ModuleInfo &mod = module_info();
        mod.base = image.base.raw();
        mod.size = image.size;

        // The ASLR-insensitive PE build identity of the image every anchor and offset in this mod was authored
        // against. Logging it makes a patch-day report self-diagnosing: two logs whose identity tokens differ
        // are two different game builds, so an anchor miss below is drift rather than a broken install.
        const DMK::scan::ImageIdentity identity = DMK::scan::image_identity(image);
        logger.info("Module validated: {} (Size: {} bytes, build {:#x})", DMK::format::format_address(mod.base),
                    mod.size, identity.token());
        return {};
    }

    /**
     * @brief Reports a best-effort subsystem that refused to initialize, keeping the mod running.
     * @details The typed Error is what each initialize_* returns now, so the degraded-feature line carries the
     *          library's own code and raising site rather than a stringified summary. Mandatory failures do not
     *          come through here; they propagate out of initialize_hooks() as the Error itself.
     * @param outcome The subsystem's result.
     * @param degraded What the mod loses when this subsystem is absent.
     */
    static void warn_if_degraded(const DMK::Result<void> &outcome, std::string_view degraded)
    {
        if (!outcome.has_value())
        {
            DMK::log().warning("{} ({})", degraded, outcome.error().message());
        }
    }

    /**
     * @brief Installs every game hook in dependency order.
     * @details The third-person camera hook is mandatory (it is the mod), so its Error propagates verbatim and
     *          init() fails with the library's own code. The view-flag interface and the menu/overlay
     *          suppression hooks degrade to a warning on failure: a missing view flag is treated as
     *          not-built-in-TPV, and missing menu/overlay detection only means the offset is not
     *          auto-suppressed under UI.
     */
    static DMK::Result<void> initialize_hooks()
    {
        DMK::Logger &logger = DMK::log();
        const ModuleInfo &mod = module_info();

        // Resolve every game-image anchor in one parallel pass, confined to the WHGame.dll image range,
        // before any module init reads its target. resolve_all_anchors() logs a per-anchor status, the feature
        // gates, and a quality summary. Each init below reads its addresses through its feature's gate, and a
        // mandatory feature whose gate failed fails the init that needs it.
        resolve_all_anchors(mod.base, mod.size);
        if (settings().export_signatures.load(std::memory_order_relaxed))
        {
            export_signatures();
        }

        // Build the cached class-vtable identities over the WHGame.dll image, before any detour is armed.
        // Every per-frame "is this vtable type X" test resolves through these, so the RTTI sweep happens
        // here once per type instead of on the render thread. Scoped to the game image on purpose: the DMK
        // default Region::host() is the game EXE, and every tracked type lives in WHGame.dll.
        init_game_types(DMK::Region{DMK::Address{mod.base}, mod.size});

        // Start the self-heal scheduler and register its groups before any detour is armed, so the very first
        // frame that resolves a base can already scan. Every group is gated on a live base the render path
        // publishes, so nothing is scanned until the corresponding object exists.
        start_offset_heal();

        // Built-in view flag, read by the camera gate to avoid stacking on the engine's own TPV.
        warn_if_degraded(initialize_game_interface(),
                         "Game interface initialization failed - built-in TPV detection disabled");

        warn_if_degraded(initialize_ui_menu_hooks(s_hooks),
                         "UI Menu hooks initialization failed - menu suppression disabled");

        warn_if_degraded(initialize_ui_overlay_hooks(s_hooks),
                         "UI Overlay hooks initialization failed - overlay suppression disabled");

        warn_if_degraded(initialize_interaction_hook(s_hooks),
                         "Interaction hook initialization failed - camera-space interaction disabled");

        // Device-agnostic movement intent for the orbit move-detection. Best-effort: a miss falls back to
        // body-position speed, so the camera still works without it.
        warn_if_degraded(initialize_player_onaction_hook(s_hooks),
                         "Player OnAction hook initialization failed - orbit move-detection uses body speed");

        // The third-person camera itself. A hard failure here means the mod cannot function, so its Error is
        // returned unchanged rather than flattened into a mod-invented code.
        if (auto camera = initialize_camera(mod.base, mod.size, s_hooks); !camera.has_value())
        {
            logger.error("Critical: third-person camera hook installation failed ({}) - mod cannot function",
                         camera.error().message());
            return camera;
        }

        // Arrows to the crosshair point and the arrow trail. After the camera, whose init readies the world raycast
        // the crosshair target is cast with. Best-effort: a miss leaves the game's own archery aim.
        warn_if_degraded(initialize_archery_hook(gated_anchor_address(Feature::Engine, AnchorId::Genv), s_hooks),
                         "Archery hook initialization failed - arrows keep the game's own aim in third person");

        // The eyes, eyelashes, hair, beard and eye film that fade with the character in the close-up fade.
        // Best-effort: without it the close-up fade hides the head instead.
        warn_if_degraded(initialize_shader_twins(s_hooks),
                         "Shader twin initialization failed - the close-up fade hides the head instead of fading the "
                         "eyes and hair");

        return {};
    }

    /**
     * @brief Registers the press bindings, fusing each INI key with its input binding via
     *        DMK::config::press_combo (through the section binder).
     * @details Each returned RAII guard goes into the Session scope so the callback stays live until
     *          teardown. All callbacks act on the camera state; rendering is
     *          additionally gated by should_apply_view(), so a state flip under a
     *          menu is harmless, but the toggles still ignore presses while a UI is up so
     *          the hotkey cannot fire from under an inventory/dialog.
     */
    static void register_press_bindings()
    {
        // Section binders, so each INI section name appears once here as well as in register_config_items().
        const DMK::config::SectionBinder settings_section = DMK::config::section("Settings");
        const DMK::config::SectionBinder orbit = DMK::config::section("Orbit");

        auto add_press = [](const DMK::config::SectionBinder &section, std::string_view ini_key,
                            std::string_view log_name, std::string_view binding, std::function<void()> on_press,
                            std::string_view default_combo)
        {
            s_binding_scope->add(section.press_combo(ini_key, log_name, binding, std::move(on_press), default_combo));
        };

        // Enter/exit third-person at runtime. Default: F3, or hold LB + press RB on a controller.
        add_press(
            settings_section, "ToggleViewKey", "Toggle View Key", "toggle_view",
            []
            {
                if (is_ui_blocking_input())
                    return;
                CameraState &cam = camera_state();
                const bool new_state = !cam.applying.load();
                cam.applying.store(new_state);
                DMK::log().info("Third-person camera {}", new_state ? "ENABLED" : "DISABLED");
            },
            "F3,Gamepad_LB+Gamepad_RB");

        // Force first-person (turn the offset off). Always allowed: turning the camera off is safe.
        add_press(
            settings_section, "ForceFPVKey", "Force FPV Key", "force_fpv",
            []
            {
                camera_state().applying.store(false);
                DMK::log().info("Third-person camera DISABLED (force FPV)");
            },
            "");

        // Force third-person (turn the offset on).
        add_press(
            settings_section, "ForceTPVKey", "Force TPV Key", "force_tpv",
            []
            {
                if (is_ui_blocking_input())
                    return;
                camera_state().applying.store(true);
                DMK::log().info("Third-person camera ENABLED (force TPV)");
            },
            "");

        // Free-look orbit: press to TOGGLE on/off. While on, the mouse orbits the camera around the
        // character (the look stays put), you can still move and act, and starting to move turns the
        // character to face the camera direction. The momentary OrbitHoldKey below is the alternative
        // freelook style (hold to engage, release to return). OrbitExcludeState only auto-disables
        // free-look when you ENTER those states (e.g. mounting); it does not forbid manually turning it
        // back on there (Combat and Mount are excluded because free-look currently fights the game's own
        // camera control in those states).
        // Default: F4, or hold LB + click the left stick (LS) on a controller.
        add_press(
            orbit, "OrbitToggleKey", "Orbit Toggle Key", "orbit_toggle",
            []
            {
                if (is_ui_blocking_input())
                    return;
                const bool new_state = !camera_state().orbit_active.load();
                if (new_state)
                    orbit_engage();
                else
                    orbit_disengage();
                DMK::log().info("Orbit camera {}", new_state ? "ENABLED" : "DISABLED");
            },
            "F4,Gamepad_LB+Gamepad_LS");

        // Open/close the preset-manager overlay. Always allowed (it is the mod's own UI), so it can be
        // opened over a game menu; the camera keeps rendering live so preset edits are visible.
        add_press(
            settings_section, "ToggleOverlayKey", "Toggle Overlay Key", "toggle_overlay", [] { Overlay::toggle(); },
            "Home");
    }

    /**
     * @brief Registers the hold bindings, fusing each INI key with its input hold binding via
     *        DMK::config::hold_combo (through the section binder).
     * @details Mirrors register_press_bindings: each returned guard goes into the Session scope so the
     *          callback stays live until teardown clears it. hold_combo carries the default
     *          combo, rebinds the live hold on every INI reload (Input::rebind), and - on the
     *          zoom triggers - registers the optional "<key>.Consume" passthrough-suppression flag. The
     *          zoom callbacks are empty: the frustum-builder detour queries their hold state by name each
     *          frame to drive the follow distance. The orbit-hold callback instead engages/releases
     *          free-look on its key edges (momentary freelook), so nothing polls it per frame.
     */
    static void register_hold_bindings()
    {
        const DMK::config::SectionBinder camera = DMK::config::section("Camera");
        const DMK::config::SectionBinder orbit = DMK::config::section("Orbit");

        auto add_hold = [](const DMK::config::SectionBinder &section, std::string_view ini_key,
                           std::string_view log_name, std::string_view binding,
                           std::function<void(bool)> on_state_change, std::string_view default_combo,
                           std::optional<bool> consume)
        {
            s_binding_scope->add(
                section.hold_combo(ini_key, log_name, binding, std::move(on_state_change), default_combo, consume));
        };

        // Zoom hold keys. Defaults: LShift+PageUp / LShift+PageDown on keyboard, or hold LB + D-pad
        // up/down on a controller. The callbacks are empty because the frustum-builder detour polls the
        // hold state by name each frame (is_binding_active) to drive the follow distance. Consume is on by
        // default and toggled per binding from the INI (ZoomInKey.Consume / ZoomOutKey.Consume): while
        // enabled, DMK masks just the bound D-pad button out of the XInput state the game reads, so the LB
        // + D-pad up/down zoom combo does not also fire the game's inventory/map shortcut as the gesture
        // ends. Only the trigger button is masked (never the LB modifier, and keyboard zoom is never
        // affected), and the mask is latched to the physical D-pad release plus a short grace window, so
        // releasing LB a frame early cannot leak a bare D-pad press.
        add_hold(
            camera, "ZoomInKey", "Zoom In Key", k_zoom_in_binding, [](bool) {},
            "LShift+PageUp,Gamepad_LB+Gamepad_DpadUp", true);
        add_hold(
            camera, "ZoomOutKey", "Zoom Out Key", k_zoom_out_binding, [](bool) {},
            "LShift+PageDown,Gamepad_LB+Gamepad_DpadDown", true);

        // Momentary free-look (freelook, as in ArmA / DayZ / PUBG): hold OrbitHoldKey to engage the orbit
        // and release to return to the precise camera-aim view, separate from the press-to-toggle
        // OrbitToggleKey. Empty default so it is opt-in and never collides with a game key. The combo fires
        // the callback with true on the press edge and false on release; the returned guard also synthesizes
        // one balancing false if the hold is still held when shutdown clears the guard, so a torn-down hold
        // cannot strand orbit on. s_engaged_by_hold records whether THIS hold turned orbit on, so the
        // release only undoes what the hold engaged: if orbit was already toggled on via OrbitToggleKey, the
        // press is a no-op and the release leaves it on. The engage is gated on the UI like the toggle; the
        // release is always honoured (even under UI) so a momentary orbit can never get stuck on. The guard
        // serializes its synthesized release against any in-flight poll-thread delivery, so the static needs
        // no lock.
        add_hold(
            orbit, "OrbitHoldKey", "Orbit Hold Key", k_orbit_hold_binding,
            [](bool pressed)
            {
                static bool s_engaged_by_hold = false;
                if (pressed)
                {
                    if (is_ui_blocking_input() || camera_state().orbit_active.load())
                        return;
                    orbit_engage();
                    s_engaged_by_hold = true;
                    DMK::log().info("Orbit camera ENABLED (hold)");
                }
                else if (s_engaged_by_hold)
                {
                    // A hold let go of while an OrbitExcludeState holds free-look off is not restored when it ends.
                    // Posted before free-look turns off: the policy's restore checks it afterward (see
                    // apply_orbit_exclude_policy).
                    camera_state().orbit_hold_released.store(true);
                    orbit_disengage();
                    s_engaged_by_hold = false;
                    DMK::log().info("Orbit camera DISABLED (hold released)");
                }
            },
            "", std::nullopt);
    }

    /**
     * @brief Starts the INI hot-reload watcher.
     * @details The bound setters re-apply the live settings on each reload. A reload that runs them also rebinds
     *          every key combo (Input::rebind) and re-applies each Consume flag, the only runtime reshapes of the
     *          input bindings. A reshape leaves the published zoom BindingTokens stale, so the callback republishes
     *          them on the watcher thread, a control-plane thread, before it reports the outcome.
     */
    static void enable_hot_reload(const DMK::config::Ini &ini)
    {
        DMK::Logger &logger = DMK::log();

        const DMK::config::AutoReloadStatus status =
            ini.enable_auto_reload(std::chrono::milliseconds{250},
                                   [](bool setters_ran)
                                   {
                                       refresh_zoom_binding_tokens();
                                       DMK::Logger &reload_logger = DMK::log();
                                       if (setters_ran)
                                           reload_logger.info("INI auto-reload: live settings applied");
                                       else
                                           reload_logger.info("INI auto-reload: no setter ran (file unchanged or "
                                                              "unreadable)");
                                   });

        if (status == DMK::config::AutoReloadStatus::Started)
            logger.info("INI hot-reload watcher started (250 ms debounce)");
        else
            logger.warning("INI hot-reload watcher not started (status {})", static_cast<int>(status));
    }

    DMK::Result<void> init(DMK::Session &session, const WheelHostTable *wheel_host)
    {
        DMK::Logger &logger = session.log();
        logger.info("----------------------------------------");
        Version::log_version_info();

        // Borrow the Session's binding scope for the whole process lifetime. The Session outlives every
        // registration below (the bootstrap worker destroys it only after the shutdown event), so the
        // pointer stays valid for as long as any binding can be registered.
        s_binding_scope = &session.scope();

        // Register every config item, then the press and hold bindings, then load and log once through the
        // Session's INI handle. The bindings are all registered before load() so the INI key combos (and the
        // optional consume flags on the gamepad zoom triggers) apply to them during load, exactly as the press
        // combos rebind on load.
        const DMK::config::Ini ini = session.ini();
        register_config_items();
        register_press_bindings();
        register_hold_bindings();
        ini.load(Constants::get_config_filename());
        ini.log_all();

        // Camera presets are user-owned and created automatically: the file is seeded from the embedded
        // factory defaults on first run, any missing built-in is re-added, and a corrupt file falls back to
        // those defaults, so loading never fails and never blocks the mod. The built-in state presets
        // (DEFAULT/COMBAT/AIMING/MOUNT/STEALTH) and any user presets feed the render-thread resolver via the
        // published binding table.
        const std::string presets_path =
            DMK::filesystem::get_runtime_directory_utf8() + "\\" + Constants::get_presets_filename();
        Presets::PresetStore::instance().load(presets_path);

        // Memory cache is a hot-path accelerator; a failure is non-fatal because the
        // readability checks fall back to direct VirtualQuery calls.
        if (DMK::memory::init_cache())
            logger.info("Memory cache system initialized");
        else
            logger.warning("Memory cache init failed; readability checks fall back to syscalls");

        // Each fallible step below returns the library's own typed Error rather than a mod-invented one, so
        // init()'s caller sees the real ErrorCode (and its category) instead of a stringified summary.
        DMK_TRY_VOID(validate_game_module());
        DMK_TRY_VOID(initialize_hooks());

        // The input engine drives every hotkey, so a failed start disables the mod's controls even though
        // the camera itself still renders. Surface the reason rather than discarding it.
        // A resident host means the wheel keepalive is booked against the LOADER, not this module, so this
        // image can still unmap on a reload. Without one, the local MessageHook backend is correct: the
        // release build is a single DLL that is never unloaded, and its keepalive is harmless there.
        DMK::input::Input::Settings input_settings{};
        if (wheel_host != nullptr)
        {
            input_settings.wheel_backend = DMK::input::Input::WheelBackend::ExternalHost;
            input_settings.wheel_host = wheel_host;
            input_settings.wheel_host_required = true;
        }
        if (auto started = session.input().start(input_settings); !started.has_value())
        {
            logger.error("Input engine failed to start ({}); hotkeys unavailable", started.error().message());
            return std::unexpected(started.error());
        }
        logger.info("Input engine started ({} wheel backend)",
                    wheel_host != nullptr ? "resident-host" : "local MessageHook");

        // The input engine now holds the bindings, so resolve the per-frame zoom tokens for the first time.
        refresh_zoom_binding_tokens();

        enable_hot_reload(ini);

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
        return {};
    }

    RetireStatus shutdown()
    {
        DMK::Logger &logger = DMK::log();
        logger.info("Shutdown: starting teardown");

        // One-line health snapshot taken while the hooks are still installed, because the teardown below
        // removes them. Both caller-owned reports are handed to collect() so the library rolls up the hook
        // population, the self-heal drift, and the anchor quality in one place rather than the mod counting
        // them itself. total_intentional_leaks counts caller-requested leaks alongside the library's own
        // defensive pins, so read it as a delta across one operation rather than as an absolute.
        std::array<DMK::rtti::DriftEntry, 12> drift_report{};
        const std::size_t drift_count = offset_heal_drift_report(drift_report);
        const DMK::diagnostics::Snapshot diag = DMK::diagnostics::collect(
            std::span<const DMK::rtti::DriftEntry>(drift_report.data(), drift_count), anchor_report());
        logger.info("Diagnostics: {} hooks ({} active, {} disabled), {} intentional leaks", diag.hooks_total,
                    diag.hooks_active, diag.hooks_disabled, diag.total_intentional_leaks);
        logger.info("Diagnostics: self-heal {}/{} landmarks healed; anchors {}/{} resolved ({} failed, {} at risk)",
                    diag.drift_healed, diag.drift_total, diag.anchor_quality.resolved, diag.anchor_quality.total,
                    diag.anchor_quality.failed, diag.anchor_quality.manual_at_risk);

        // Switch the native turn animation off BEFORE the hooks go, while the frustum detour that does it on the game
        // thread is still installed. Otherwise the locomotion action keeps the LockBodyTurn reference it took while the
        // mod reported third person, and the body stays free of the look until the player's next step. Idempotent, and
        // a no-op if the native turn animation was never engaged.
        release_native_turn_animation();
        // Stop re-aiming and tracking shots; the shot detours pass straight through until the hooks retire.
        release_archery();
        // Unpublish the shader twins' swap set while the hooks are still installed, so the character's items go back
        // to the stock shaders. The twin shaders stay loaded on purpose.
        shutdown_shader_twins();

        // Join the mod's workers before any hook goes: the INI watcher, so no reload setter runs during teardown, and
        // the overlay render thread, so no UI mutation races the preset flush.
        DMK::config::disable_auto_reload();
        Overlay::stop();

        // A worker whose join failed still runs code in this image and holds its module reference. Its dependencies,
        // the hooks included, must then stay live for the process, so the teardown stops here.
        if (DMK::diagnostics::module_pin_count(DMK::diagnostics::ModulePinReason::Worker) != 0)
        {
            logger.error("Shutdown: a mod worker did not join; the hooks stay installed and the module must stay "
                         "mapped");
            return RetireStatus::Failed;
        }

        // Persist any unsaved preset edits.
        Presets::PresetStore::instance().flush();

        // Hooks are caller-owned and the library removes none of them, so this is the only path that restores the
        // patched prologues. The set disables every hook, waits until no game thread is inside a detour, and only then
        // destroys the handles.
        const RetireStatus hooks = s_hooks.retire();
        if (hooks == RetireStatus::Retired)
        {
            // No detour runs any more, so the zoom tokens the frustum detour read can go, and so can the sets for the
            // fade a render thread handed the shader twins while they shut down.
            release_zoom_binding_tokens();
            release_shader_twin_records();
            logger.info("Shutdown: teardown complete");
        }
        return hooks;
    }

} // namespace TPVCamera
