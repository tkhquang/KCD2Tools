/**
 * @file dllmain.cpp
 * @brief Main initialization and cleanup for the TPV Toggle mod.
 *
 * Handles module loading, configuration, hook setup, and thread management.
 */

#include "logger.h"
#include "config.h"
#include "utils.h"
#include "constants.h"
#include "version.h"
#include "toggle_thread.h"
#include "game_interface.h"
#include "global_state.h"
#include "camera_profile.h"
#include "camera_profile_thread.h"
#include "hooks/overlay_hook.h"
#include "hooks/event_hooks.h"
#include "hooks/fov_hook.h"
#include "hooks/tpv_camera_hook.h"
#include "hooks/tpv_input_hook.h"
// #include "hooks/entity_hooks.h"

#include "MinHook.h"

#include <windows.h>
#include <psapi.h>
#include <thread>
#include <stdexcept>

// Configuration state
Config g_config;

/**
 * @brief Safely cleans up all resources and threads.
 */
void cleanupResources()
{
    Logger &logger = Logger::getInstance();
    logger.log(LOG_INFO, "Cleanup: Starting cleanup process...");

    // Clear the memory cache
    clearMemoryCache();

    // Signal threads to exit
    if (g_exitEvent)
    {
        SetEvent(g_exitEvent);
        Sleep(100); // Allow threads time to process exit signal
    }

    // Wait for threads to complete (with timeout)
    HANDLE threads[] = {g_hMonitorThread, g_hOverlayThread};
    DWORD thread_count = 0;
    if (g_hMonitorThread)
        threads[thread_count++] = g_hMonitorThread;
    if (g_hOverlayThread)
        threads[thread_count++] = g_hOverlayThread;

    if (thread_count > 0)
    {
        DWORD wait_result = WaitForMultipleObjects(thread_count, threads, TRUE, 2000); // 2 second timeout
        if (wait_result == WAIT_TIMEOUT)
        {
            logger.log(LOG_WARNING, "Cleanup: Thread wait timeout - some threads may not have exited cleanly");
        }
    }

    // Clean up thread handles
    if (g_hMonitorThread)
    {
        CloseHandle(g_hMonitorThread);
        g_hMonitorThread = NULL;
    }
    if (g_hOverlayThread)
    {
        CloseHandle(g_hOverlayThread);
        g_hOverlayThread = NULL;
    }

    // Clean up hooks and interfaces in reverse order of initialization
    cleanupEventHooks();
    cleanupFovHook();
    cleanupOverlayHook();
    cleanupGameInterface();
    cleanupTpvCameraHook();
    cleanupTpvInputHook();
    // cleanupEntityHooks();

    // Uninitialize MinHook
    MH_Uninitialize();

    // Clean up exit event
    if (g_exitEvent)
    {
        CloseHandle(g_exitEvent);
        g_exitEvent = NULL;
    }

    logger.log(LOG_INFO, "Cleanup: All resources freed successfully");
}

/**
 * @brief Validates that the target game module is loaded and accessible.
 * @return true if module is valid, false otherwise.
 */
bool validateGameModule()
{
    Logger &logger = Logger::getInstance();

    // Wait for module to load
    HMODULE game_module = NULL;
    for (int i = 0; i < 30 && !game_module; ++i)
    {
        game_module = GetModuleHandleA(Constants::MODULE_NAME);
        if (!game_module)
            Sleep(100);
    }

    if (!game_module)
    {
        logger.log(LOG_ERROR, "Failed to find module: " + std::string(Constants::MODULE_NAME));
        return false;
    }

    // Get module information
    MODULEINFO mod_info = {};
    if (!GetModuleInformation(GetCurrentProcess(), game_module, &mod_info, sizeof(mod_info)))
    {
        logger.log(LOG_ERROR, "Failed to get module information: " + std::to_string(GetLastError()));
        return false;
    }

    g_ModuleBase = reinterpret_cast<uintptr_t>(mod_info.lpBaseOfDll);
    g_ModuleSize = mod_info.SizeOfImage;

    if (g_ModuleSize == 0)
    {
        logger.log(LOG_ERROR, "Module has zero size");
        return false;
    }

    logger.log(LOG_INFO, "Module validated: " + format_address(g_ModuleBase) +
                             " (Size: " + std::to_string(g_ModuleSize) + " bytes)");
    return true;
}

/**
 * @brief Initializes MinHook library and all required hooks.
 * @return true if initialization successful, false otherwise.
 */
bool initializeHooks()
{
    Logger &logger = Logger::getInstance();

    // Initialize MinHook
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK)
    {
        logger.log(LOG_ERROR, "MinHook initialization failed: " + std::string(MH_StatusToString(status)));
        return false;
    }

    // Initialize core game interface (always required)
    if (!initializeGameInterface(g_ModuleBase, g_ModuleSize))
    {
        logger.log(LOG_ERROR, "Critical: Game interface initialization failed - mod cannot function");
        return false;
    }

    // if (!initializeEntityHooks(g_ModuleBase, g_ModuleSize))
    // {
    //     logger.log(LOG_WARNING, "Entity Hooks (for Player & SetWorldTM) initialization failed.");
    // }

    // Initialize optional overlay system
    if (g_config.enable_overlay_feature)
    {
        if (!initializeOverlayHook(g_ModuleBase, g_ModuleSize))
        {
            logger.log(LOG_WARNING, "Overlay hook initialization failed - overlay features disabled");
            g_config.enable_overlay_feature = false;
        }

        if (g_config.enable_overlay_feature)
        {
            if (!initializeEventHooks(g_ModuleBase, g_ModuleSize))
            {
                logger.log(LOG_WARNING, "Event hooks initialization failed - input filtering disabled");
                cleanupOverlayHook();
                g_config.enable_overlay_feature = false;
            }
        }
    }

    // Initialize optional FOV feature
    if (g_config.tpv_fov_degrees > 0.0f)
    {
        if (!initializeFovHook(g_ModuleBase, g_ModuleSize, g_config.tpv_fov_degrees))
        {
            logger.log(LOG_WARNING, "FOV hook initialization failed - FOV modification disabled");
            g_config.tpv_fov_degrees = -1.0f;
        }
    }

    if (!initializeTpvCameraHook(g_ModuleBase, g_ModuleSize))
    {
        logger.log(LOG_WARNING, "TPV Camera Offset Hook initialization failed - Offset feature disabled.");
    }

    if (g_config.tpv_pitch_sensitivity != 1.0f || g_config.tpv_yaw_sensitivity != 1.0f || g_config.tpv_pitch_limits_enabled)
    {
        if (!initializeTpvInputHook(g_ModuleBase, g_ModuleSize))
        {
            logger.log(LOG_WARNING, "TPV Input Hook initialization failed - Camera sensitivity control disabled");
        }
    }

    return true;
}

/**
 * @brief Creates and starts the monitor threads.
 * @return true if all required threads started successfully, false otherwise.
 */
bool startMonitorThreads()
{
    Logger &logger = Logger::getInstance();

    // Prepare toggle data for the main monitor thread
    ToggleData *toggle_data = new (std::nothrow) ToggleData{
        std::move(g_config.toggle_keys),
        std::move(g_config.fpv_keys),
        std::move(g_config.tpv_keys)};

    if (!toggle_data)
    {
        logger.log(LOG_ERROR, "Failed to allocate memory for toggle data");
        return false;
    }

    // Start main monitor thread (always required)
    g_hMonitorThread = CreateThread(NULL, 0, MonitorThread, toggle_data, 0, NULL);
    if (!g_hMonitorThread)
    {
        delete toggle_data;
        logger.log(LOG_ERROR, "Failed to create monitor thread: " + std::to_string(GetLastError()));
        return false;
    }

    // Start overlay monitor thread (optional)
    if (g_config.enable_overlay_feature)
    {
        g_hOverlayThread = CreateThread(NULL, 0, OverlayMonitorThread, NULL, 0, NULL);
        if (!g_hOverlayThread)
        {
            logger.log(LOG_WARNING, "Failed to create overlay monitor thread - overlay features disabled");
            g_config.enable_overlay_feature = false;
        }
    }

    return true;
}

/**
 * @brief Main initialization thread that sets up the mod.
 */
DWORD WINAPI MainThread(LPVOID hModule_param)
{
    (void)hModule_param;
    Logger &logger = Logger::getInstance();

    try
    {
        // Log startup banner
        logger.log(LOG_INFO, "----------------------------------------");
        Version::logVersionInfo();

        // Load configuration
        g_config = loadConfig(Constants::getConfigFilename());

        // Apply log level from config
        LogLevel log_level = LOG_INFO;
        if (g_config.log_level == "TRACE")
            log_level = LOG_TRACE;
        else if (g_config.log_level == "DEBUG")
            log_level = LOG_DEBUG;
        else if (g_config.log_level == "WARNING")
            log_level = LOG_WARNING;
        else if (g_config.log_level == "ERROR")
            log_level = LOG_ERROR;
        logger.setLogLevel(log_level);

        // Initialize memory cache
        initMemoryCache();
        logger.log(LOG_INFO, "Memory cache system initialized");

        // Create exit event for thread signaling
        g_exitEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        if (!g_exitEvent)
        {
            throw std::runtime_error("Failed to create exit event: " + std::to_string(GetLastError()));
        }

        // Validate game module
        if (!validateGameModule())
        {
            throw std::runtime_error("Game module validation failed");
        }

        // Initialize hooks
        if (!initializeHooks())
        {
            throw std::runtime_error("Hook initialization failed");
        }

        // Start monitor threads
        if (!startMonitorThreads())
        {
            throw std::runtime_error("Failed to start monitor threads");
        }

        // Initialize and start camera profile system if enabled
        if (g_config.enable_camera_profiles)
        {
            logger.log(LOG_INFO, "Initializing camera profile system...");

            // Set initial global camera offset from config
            g_currentCameraOffset = Vector3(g_config.tpv_offset_x, g_config.tpv_offset_y, g_config.tpv_offset_z);

            // Initialize the camera profile manager with JSON-based persistence
            CameraProfileManager::getInstance().loadProfiles(g_config.profile_directory);

            // Configure transition settings
            CameraProfileManager::getInstance().setTransitionSettings(
                g_config.transition_duration,
                g_config.use_spring_physics,
                g_config.spring_strength,
                g_config.spring_damping);

            // Start camera profile thread
            CameraProfileThreadData *profile_data = new (std::nothrow) CameraProfileThreadData{
                g_config.offset_adjustment_step};

            if (!profile_data)
            {
                logger.log(LOG_ERROR, "Failed to allocate memory for camera profile thread data");
            }
            else
            {
                g_hCameraProfileThread = CreateThread(NULL, 0, CameraProfileThread, profile_data, 0, NULL);
                if (!g_hCameraProfileThread)
                {
                    delete profile_data;
                    logger.log(LOG_ERROR, "Failed to create camera profile thread: " + std::to_string(GetLastError()));
                }
                else
                {
                    logger.log(LOG_INFO, "Camera profile thread started successfully");
                }
            }
        }

        logger.log(LOG_INFO, "Initialization completed successfully");
    }
    catch (const std::exception &e)
    {
        logger.log(LOG_ERROR, "Fatal initialization error: " + std::string(e.what()));
        MessageBoxA(NULL, ("Fatal Error:\n" + std::string(e.what())).c_str(),
                    Constants::MOD_NAME, MB_ICONERROR | MB_OK);
        cleanupResources();
        return 1;
    }
    catch (...)
    {
        logger.log(LOG_ERROR, "Fatal initialization error: Unknown exception");
        MessageBoxA(NULL, "Fatal Unknown Error!", Constants::MOD_NAME, MB_ICONERROR | MB_OK);
        cleanupResources();
        return 1;
    }

    return 0;
}

/**
 * @brief DLL entry point.
 */
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    (void)lpReserved;

    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        CreateThread(NULL, 0, MainThread, hModule, 0, NULL);
        break;

    case DLL_PROCESS_DETACH:
        cleanupResources();
        break;
    }

    return TRUE;
}
