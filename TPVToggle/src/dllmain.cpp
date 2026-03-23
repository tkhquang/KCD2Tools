/**
 * @file dllmain.cpp
 * @brief Main initialization and cleanup for the TPV Toggle mod using DetourModKit.
 *
 * Handles module loading, configuration, hook setup, and thread management.
 */

#include "config.h"
#include "constants.h"
#include "version.h"
#include "toggle_thread.h"
#include "game_interface.h"
#include "global_state.h"
#include "camera_profile.h"
#include "camera_profile_thread.h"
#include "hooks/event_hooks.h"
#include "hooks/fov_hook.h"
#include "hooks/tpv_camera_hook.h"
#include "hooks/tpv_input_hook.h"
#include "hooks/ui_overlay_hooks.h"
#include "hooks/ui_menu_hooks.h"
// #include "hooks/entity_hooks.h"

#include <DetourModKit.hpp>

#include <windows.h>
#include <psapi.h>
#include <stdexcept>
#include <memory>

// Configuration state
Config g_config;

/**
 * @brief Safely cleans up all mod resources and threads.
 */
void cleanupResources()
{
    DMKLogger &logger = DMKLogger::get_instance();
    logger.info("Cleanup: Starting cleanup process...");

    // Signal threads to exit
    if (g_exitEvent)
    {
        SetEvent(g_exitEvent);
    }

    // Wait for monitor thread to complete
    if (g_hMonitorThread)
    {
        if (WaitForSingleObject(g_hMonitorThread, 2000) == WAIT_TIMEOUT)
        {
            logger.warning("Cleanup: Monitor thread wait timeout");
        }
        CloseHandle(g_hMonitorThread);
        g_hMonitorThread = NULL;
    }

    // Wait for camera profile thread to complete
    if (g_hCameraProfileThread)
    {
        if (WaitForSingleObject(g_hCameraProfileThread, 2000) == WAIT_TIMEOUT)
        {
            logger.warning("Cleanup: Camera profile thread wait timeout");
        }
        CloseHandle(g_hCameraProfileThread);
        g_hCameraProfileThread = NULL;
    }

    // Clean up hooks and interfaces in reverse order of initialization
    cleanupUiMenuHooks();
    cleanupUiOverlayHooks();
    cleanupEventHooks();
    cleanupFovHook();
    cleanupGameInterface();
    cleanupTpvCameraHook();
    cleanupTpvInputHook();
    // cleanupEntityHooks();

    // Clean up exit event
    if (g_exitEvent)
    {
        CloseHandle(g_exitEvent);
        g_exitEvent = NULL;
    }

    logger.info("Cleanup: All resources freed successfully");

    // Shut down all DMK singletons in correct dependency order
    DMK_Shutdown();
}

/**
 * @brief Validates that the target game module is loaded and accessible.
 */
bool validateGameModule()
{
    DMKLogger &logger = DMKLogger::get_instance();

    HMODULE game_module = NULL;
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

    g_ModuleBase = reinterpret_cast<uintptr_t>(mod_info.lpBaseOfDll);
    g_ModuleSize = mod_info.SizeOfImage;

    if (g_ModuleSize == 0)
    {
        logger.error("Module has zero size");
        return false;
    }

    logger.info("Module validated: {} (Size: {} bytes)",
                DMKFormat::format_address(g_ModuleBase), g_ModuleSize);
    return true;
}

/**
 * @brief Initializes all required hooks using DetourModKit HookManager.
 */
bool initializeHooks()
{
    DMKLogger &logger = DMKLogger::get_instance();

    // Initialize core game interface (always required)
    if (!initializeGameInterface(g_ModuleBase, g_ModuleSize))
    {
        logger.error("Critical: Game interface initialization failed - mod cannot function");
        return false;
    }

    // Initialize UI Menu hooks for menu detection
    if (!initializeUiMenuHooks(g_ModuleBase, g_ModuleSize))
    {
        logger.warning("UI Menu hooks initialization failed - menu detection disabled");
    }

    // Initialize UI Overlay hooks for overlay detection
    if (g_config.enable_overlay_feature)
    {
        if (!initializeUiOverlayHooks(g_ModuleBase, g_ModuleSize))
        {
            logger.error("UI Overlay hooks initialization failed - overlay features disabled");
            g_config.enable_overlay_feature = false;
        }
        else
        {
            // Initialize event hooks for scroll input filtering
            if (!initializeEventHooks(g_ModuleBase, g_ModuleSize))
            {
                logger.warning("Event hooks initialization failed - input filtering disabled");
            }
        }
    }

    // Initialize optional FOV feature
    if (g_config.tpv_fov_degrees > 0.0f)
    {
        if (!initializeFovHook(g_ModuleBase, g_ModuleSize, g_config.tpv_fov_degrees))
        {
            logger.warning("FOV hook initialization failed - FOV modification disabled");
            g_config.tpv_fov_degrees = -1.0f;
        }
    }

    if (!initializeTpvCameraHook(g_ModuleBase, g_ModuleSize))
    {
        logger.warning("TPV Camera Offset Hook initialization failed - Offset feature disabled");
    }

    if (g_config.tpv_pitch_sensitivity != 1.0f || g_config.tpv_yaw_sensitivity != 1.0f ||
        g_config.tpv_pitch_limits_enabled || g_config.enable_overlay_feature)
    {
        if (!initializeTpvInputHook(g_ModuleBase, g_ModuleSize))
        {
            logger.warning("TPV Input Hook initialization failed - Camera sensitivity control disabled");
        }
    }

    return true;
}

/**
 * @brief Registers all key bindings with DMKInputManager.
 */
void registerInputBindings()
{
    DMKLogger &logger = DMKLogger::get_instance();
    DMKInputManager &input_mgr = DMKInputManager::get_instance();

    // View toggle/switch keys
    input_mgr.register_press("toggle_view", g_config.toggle_keys, []() {
        if (getResolvedTpvFlagAddress())
            safeToggleViewState();
    });

    input_mgr.register_press("force_fpv", g_config.fpv_keys, []() {
        if (getResolvedTpvFlagAddress())
            setViewState(0);
    });

    input_mgr.register_press("force_tpv", g_config.tpv_keys, []() {
        if (getResolvedTpvFlagAddress())
            setViewState(1);
    });

    // Hold-to-scroll key
    input_mgr.register_hold("hold_scroll", g_config.hold_scroll_keys, [](bool held) {
        g_holdToScrollActive.store(held, std::memory_order_relaxed);
        handleHoldToScrollKeyState(held);
    });

    // Camera profile keys (only if profiles enabled)
    if (g_config.enable_camera_profiles)
    {
        input_mgr.register_press("profile_master_toggle", g_config.master_toggle_keys, []() {
            bool newMode = !g_cameraAdjustmentMode.load();
            g_cameraAdjustmentMode.store(newMode);
            DMKLogger::get_instance().info("Camera adjustment mode {}",
                newMode ? "ENABLED" : "DISABLED");
        });

        input_mgr.register_press("profile_save", g_config.profile_save_keys, []() {
            if (g_cameraAdjustmentMode.load())
                CameraProfileManager::getInstance().createNewProfileFromLiveState("General");
        });

        input_mgr.register_press("profile_update", g_config.profile_update_keys, []() {
            if (g_cameraAdjustmentMode.load())
                CameraProfileManager::getInstance().updateActiveProfileWithLiveState();
        });

        input_mgr.register_press("profile_delete", g_config.profile_delete_keys, []() {
            if (g_cameraAdjustmentMode.load())
                CameraProfileManager::getInstance().deleteActiveProfile();
        });

        input_mgr.register_press("profile_cycle", g_config.profile_cycle_keys, []() {
            if (g_cameraAdjustmentMode.load())
                CameraProfileManager::getInstance().cycleToNextProfile();
        });

        input_mgr.register_press("profile_reset", g_config.profile_reset_keys, []() {
            if (g_cameraAdjustmentMode.load())
                CameraProfileManager::getInstance().resetToDefault();
        });

        // Register offset adjustment keys as hold bindings for is_binding_active() queries
        input_mgr.register_hold("offset_x_inc", g_config.offset_x_inc_keys, [](bool) {});
        input_mgr.register_hold("offset_x_dec", g_config.offset_x_dec_keys, [](bool) {});
        input_mgr.register_hold("offset_y_inc", g_config.offset_y_inc_keys, [](bool) {});
        input_mgr.register_hold("offset_y_dec", g_config.offset_y_dec_keys, [](bool) {});
        input_mgr.register_hold("offset_z_inc", g_config.offset_z_inc_keys, [](bool) {});
        input_mgr.register_hold("offset_z_dec", g_config.offset_z_dec_keys, [](bool) {});
    }

    logger.info("Input bindings registered ({} total)", input_mgr.binding_count());
}

/**
 * @brief Creates and starts the overlay monitor thread.
 */
bool startMonitorThread()
{
    DMKLogger &logger = DMKLogger::get_instance();

    g_hMonitorThread = CreateThread(NULL, 0, MonitorThread, NULL, 0, NULL);
    if (!g_hMonitorThread)
    {
        logger.error("Failed to create monitor thread: {}", GetLastError());
        return false;
    }

    logger.info("Monitor thread started successfully");
    return true;
}

/**
 * @brief Main initialization thread that sets up the mod.
 */
DWORD WINAPI MainThread([[maybe_unused]] LPVOID hModule_param)
{
    DMKLogger &logger = DMKLogger::get_instance();

    try
    {
        logger.info("----------------------------------------");
        Version::logVersionInfo();

        // Load configuration
        g_config = loadConfig(Constants::getConfigFilename());

        // Apply log level from config using DMK's built-in parser
        logger.set_log_level(DMKLogger::string_to_log_level(g_config.log_level));

        // Enable async logging for reduced latency on hook callbacks
        logger.enable_async_mode();

        // Initialize memory cache with defaults (256 entries, 50ms expiry)
        DMKMemory::init_cache();
        logger.info("Memory cache system initialized");

        // Create exit event for thread signaling
        g_exitEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        if (!g_exitEvent)
        {
            throw std::runtime_error("Failed to create exit event: " + std::to_string(GetLastError()));
        }

        if (!validateGameModule())
        {
            throw std::runtime_error("Game module validation failed");
        }

        if (!initializeHooks())
        {
            throw std::runtime_error("Hook initialization failed");
        }

        if (!startMonitorThread())
        {
            throw std::runtime_error("Failed to start monitor thread");
        }

        // Register all key bindings and start InputManager
        registerInputBindings();
        DMKInputManager::get_instance().start();
        logger.info("InputManager started");

        // Initialize camera profile system if enabled
        if (g_config.enable_camera_profiles)
        {
            logger.info("Initializing camera profile system...");

            g_currentCameraOffset = Vector3(g_config.tpv_offset_x, g_config.tpv_offset_y, g_config.tpv_offset_z);

            CameraProfileManager::getInstance().loadProfiles(g_config.profile_directory);
            CameraProfileManager::getInstance().setTransitionSettings(
                g_config.transition_duration,
                g_config.use_spring_physics,
                g_config.spring_strength,
                g_config.spring_damping);

            auto profile_data = std::make_unique<CameraProfileThreadData>(
                CameraProfileThreadData{g_config.offset_adjustment_step});

            g_hCameraProfileThread = CreateThread(NULL, 0, CameraProfileThread, profile_data.get(), 0, NULL);
            if (!g_hCameraProfileThread)
            {
                logger.error("Failed to create camera profile thread: {}", GetLastError());
            }
            else
            {
                profile_data.release();
            }
        }

        logger.info("Initialization completed successfully");

        // Block until shutdown is signaled — keeps cleanup off the loader lock
        WaitForSingleObject(g_exitEvent, INFINITE);
    }
    catch (const std::exception &e)
    {
        logger.error("Fatal initialization error: {}", e.what());
        MessageBoxA(NULL, ("Fatal Error:\n" + std::string(e.what())).c_str(),
                    Constants::MOD_NAME, MB_ICONERROR | MB_OK);
    }
    catch (...)
    {
        logger.error("Fatal initialization error: Unknown exception");
        MessageBoxA(NULL, "Fatal Unknown Error!", Constants::MOD_NAME, MB_ICONERROR | MB_OK);
    }

    cleanupResources();
    return 0;
}

/**
 * @brief DLL entry point.
 */
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call,
                      [[maybe_unused]] LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    {
        DisableThreadLibraryCalls(hModule);

        // Configure DetourModKit Logger before starting
        DMKLogger::configure(Constants::MOD_NAME, std::string(Constants::MOD_NAME) + ".log", "%Y-%m-%d %H:%M:%S");

        HANDLE hThread = CreateThread(NULL, 0, MainThread, hModule, 0, NULL);
        if (hThread)
            CloseHandle(hThread);
        break;
    }

    case DLL_PROCESS_DETACH:
        // Signal MainThread to run cleanup outside the loader lock
        if (g_exitEvent)
            SetEvent(g_exitEvent);
        break;
    }

    return TRUE;
}
