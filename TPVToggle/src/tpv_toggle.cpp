/**
 * @file tpv_toggle.cpp
 * @brief Mod lifecycle: configuration, hooks, input bindings, and poll workers.
 *
 * init() and shutdown() are invoked by DMK::Bootstrap from a dedicated worker
 * thread, so the heavy setup and teardown run off the Windows loader lock.
 */

#include "tpv_toggle.hpp"
#include "config.hpp"
#include "constants.hpp"
#include "global_state.hpp"
#include "game_interface.hpp"
#include "version.hpp"
#include "camera_profile.hpp"
#include "toggle_thread.hpp"
#include "camera_profile_thread.hpp"
#include "hooks/event_hooks.hpp"
#include "hooks/tpv_camera_hook.hpp"
#include "hooks/tpv_input_hook.hpp"
#include "hooks/ui_overlay_hooks.hpp"
#include "hooks/ui_menu_hooks.hpp"

#include <DetourModKit.hpp>
#include <DetourModKit/worker.hpp>

#include <windows.h>
#include <psapi.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace TPVToggle
{

// Poll workers and the RAII guards for the press bindings registered via
// DMK::Config::register_press_combo (cleared during shutdown).
static std::unique_ptr<DMK::StoppableWorker> s_overlayWorker;
static std::unique_ptr<DMK::StoppableWorker> s_cameraProfileWorker;
static std::vector<DMK::Config::InputBindingGuard> s_bindingGuards;

/**
 * @brief Resolves the game module base and size into TPVToggle::module_info().
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
 * @details The game interface is mandatory; the rest degrade to disabled
 *          features on failure rather than aborting initialization.
 */
static bool initialize_hooks()
{
    DMK::Logger &logger = DMK::Logger::get_instance();
    const ModuleInfo &mod = module_info();

    if (!initializeGameInterface(mod.base, mod.size))
    {
        logger.error("Critical: Game interface initialization failed - mod cannot function");
        return false;
    }

    if (!initializeUiMenuHooks(mod.base, mod.size))
    {
        logger.warning("UI Menu hooks initialization failed - menu detection disabled");
    }

    if (g_config.enable_overlay_feature)
    {
        if (!initializeUiOverlayHooks(mod.base, mod.size))
        {
            logger.error("UI Overlay hooks initialization failed - overlay features disabled");
            g_config.enable_overlay_feature = false;
        }
        else if (!initializeEventHooks(mod.base, mod.size))
        {
            logger.warning("Event hooks initialization failed - input filtering disabled");
        }
    }

    // The TPV FOV override piggy-backs on the TPV camera detour below; the
    // detour reads settings().tpvFovDegrees from the hot-reload atomic, so no
    // separate FOV hook is installed here.
    if (!initializeTpvCameraHook(mod.base, mod.size))
    {
        logger.warning("TPV Camera Offset Hook initialization failed - Offset feature disabled");
    }

    // The input hook is needed for sensitivity, pitch limits, and the menu-open
    // input skip; install it whenever any of those could be in play.
    if (settings().yawSensitivity.load() != 1.0f || settings().pitchSensitivity.load() != 1.0f ||
        settings().pitchLimitsEnabled.load() || g_config.enable_overlay_feature)
    {
        if (!initializeTpvInputHook(mod.base, mod.size))
        {
            logger.warning("TPV Input Hook initialization failed - Camera sensitivity control disabled");
        }
    }

    return true;
}

/**
 * @brief Registers the press bindings, fusing each INI key with its InputManager
 *        binding via DMK::Config::register_press_combo.
 * @details The returned RAII guards are stashed so the callbacks stay live until
 *          shutdown. Profile callbacks are inert unless camera adjustment mode is
 *          active, so they are safe to register unconditionally.
 */
static void register_press_bindings()
{
    auto add_press = [](std::string_view section, std::string_view ini_key, std::string_view log_name,
                        std::string_view binding, std::function<void()> on_press, std::string_view default_combo)
    {
        s_bindingGuards.push_back(DMK::Config::register_press_combo(
            section, ini_key, log_name, binding, std::move(on_press), default_combo));
    };

    add_press("Settings", "ToggleKey", "Toggle Key", "toggle_view",
              [] { if (getResolvedTpvFlagAddress()) safeToggleViewState(); }, "0x72"); // F3
    add_press("Settings", "FPVKey", "FPV Key", "force_fpv",
              [] { if (getResolvedTpvFlagAddress()) setViewState(0); }, "");
    add_press("Settings", "TPVKey", "TPV Key", "force_tpv",
              [] { if (getResolvedTpvFlagAddress()) setViewState(1); }, "");

    add_press("CameraProfiles", "MasterToggleKey", "Master Toggle Key", "profile_master_toggle",
              [] {
                  if (!settings().enableCameraProfiles.load())
                      return;
                  const bool new_mode = !camera_state().adjustmentMode.load();
                  camera_state().adjustmentMode.store(new_mode);
                  DMK::Logger::get_instance().info("Camera adjustment mode {}", new_mode ? "ENABLED" : "DISABLED");
              }, "0x7A"); // F11
    add_press("CameraProfiles", "ProfileSaveKey", "Profile Save Key", "profile_save",
              [] { if (camera_state().adjustmentMode.load()) CameraProfileManager::getInstance().createNewProfileFromLiveState("General"); }, "0x61"); // Numpad 1
    add_press("CameraProfiles", "ProfileUpdateKey", "Profile Update Key", "profile_update",
              [] { if (camera_state().adjustmentMode.load()) CameraProfileManager::getInstance().updateActiveProfileWithLiveState(); }, "0x67"); // Numpad 7
    add_press("CameraProfiles", "ProfileDeleteKey", "Profile Delete Key", "profile_delete",
              [] { if (camera_state().adjustmentMode.load()) CameraProfileManager::getInstance().deleteActiveProfile(); }, "0x69"); // Numpad 9
    add_press("CameraProfiles", "ProfileCycleKey", "Profile Cycle Key", "profile_cycle",
              [] { if (camera_state().adjustmentMode.load()) CameraProfileManager::getInstance().cycleToNextProfile(); }, "0x63"); // Numpad 3
    add_press("CameraProfiles", "ProfileResetKey", "Profile Reset Key", "profile_reset",
              [] { if (camera_state().adjustmentMode.load()) CameraProfileManager::getInstance().resetToDefault(); }, "0x65"); // Numpad 5
}

/**
 * @brief Registers the hold bindings after the INI key lists have been loaded.
 * @details DMK has no hold-combo helper, so the INI keys are parsed by
 *          register_config_items and the InputManager hold bindings are created
 *          here from the resulting lists. The offset bindings carry empty
 *          callbacks; the profile worker queries them via is_binding_active().
 */
static void register_hold_bindings()
{
    DMK::InputManager &input_mgr = DMK::InputManager::get_instance();

    input_mgr.register_hold("hold_scroll", g_config.hold_scroll_keys, [](bool held) {
        overlay_state().holdToScrollActive.store(held, std::memory_order_relaxed);
        handleHoldToScrollKeyState(held);
    });

    input_mgr.register_hold("offset_x_inc", g_config.offset_x_inc_keys, [](bool) {});
    input_mgr.register_hold("offset_x_dec", g_config.offset_x_dec_keys, [](bool) {});
    input_mgr.register_hold("offset_y_inc", g_config.offset_y_inc_keys, [](bool) {});
    input_mgr.register_hold("offset_y_dec", g_config.offset_y_dec_keys, [](bool) {});
    input_mgr.register_hold("offset_z_inc", g_config.offset_z_inc_keys, [](bool) {});
    input_mgr.register_hold("offset_z_dec", g_config.offset_z_dec_keys, [](bool) {});
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
    Version::logVersionInfo();

    // Register every config item and the press bindings, then load and log once.
    register_config_items();
    register_press_bindings();
    DMK::Config::load(Constants::getConfigFilename());
    DMK::Config::log_all();

    // Fall back to the module directory when no profile directory was configured.
    if (g_config.profile_directory.empty())
    {
        g_config.profile_directory = DMK::Filesystem::get_runtime_directory_utf8();
        if (g_config.profile_directory.empty())
            g_config.profile_directory = ".";
    }

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

    if (settings().enableCameraProfiles.load())
    {
        logger.info("Initializing camera profile system...");

        // One-time startup seed of the live offset before the profile writer
        // worker exists; loadProfiles() republishes it under the profile mutex.
        camera_state().offset.store(Vector3(settings().tpvOffsetX.load(),
                                            settings().tpvOffsetY.load(),
                                            settings().tpvOffsetZ.load()));

        if (!CameraProfileManager::getInstance().loadProfiles(g_config.profile_directory))
        {
            logger.warning("Camera profile load failed; starting from defaults");
        }
        CameraProfileManager::getInstance().setTransitionSettings(
            g_config.transition_duration, g_config.use_spring_physics,
            g_config.spring_strength, g_config.spring_damping);

        s_cameraProfileWorker = std::make_unique<DMK::StoppableWorker>("KCD2_TPV_CameraProfile", &camera_profile_body);
    }

    s_overlayWorker = std::make_unique<DMK::StoppableWorker>("KCD2_TPV_OverlayMonitor", &overlay_monitor_body);

    enable_hot_reload();

    logger.info("Initialization completed successfully");
    return true;
}

void shutdown()
{
    DMK::Logger &logger = DMK::Logger::get_instance();
    logger.info("Shutdown: starting teardown");

    // Stop the INI watcher first so no reload setter runs during teardown.
    DMK::Config::disable_auto_reload();

    // Two-phase worker stop: request stop on both so they unwind in parallel,
    // then join in reverse-dependency order (camera writer before the monitor).
    if (s_cameraProfileWorker)
        s_cameraProfileWorker->request_stop();
    if (s_overlayWorker)
        s_overlayWorker->request_stop();

    if (s_cameraProfileWorker)
    {
        logger.info("Shutdown: joining camera-profile worker");
        s_cameraProfileWorker->shutdown();
        s_cameraProfileWorker.reset();
    }
    if (s_overlayWorker)
    {
        logger.info("Shutdown: joining overlay-monitor worker");
        s_overlayWorker->shutdown();
        s_overlayWorker.reset();
    }

    // Disable the press callbacks so they cannot run during teardown.
    s_bindingGuards.clear();

    // DMK_Shutdown() removes every HookManager hook on its own (production:
    // Bootstrap calls it after this returns; dev: the logic DLL's Shutdown()
    // calls it), so the inline/mid hooks are not removed here. Only the teardown
    // it cannot do runs now: the event hook restores the scroll-accumulator
    // instruction it patched into the game image, so the NOP is never latched as
    // the "original" across a hot-reload, and the game interface clears its
    // resolved context pointer.
    cleanupEventHooks();
    cleanupGameInterface();

    logger.info("Shutdown: teardown complete");
}

} // namespace TPVToggle
