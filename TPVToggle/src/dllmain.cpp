#include <windows.h>
#include <Psapi.h>
#include <vector>
#include <string>
#include <thread>
#include <chrono>

#include <DetourModKit.hpp>

#include "constants.hpp"
#include "config.hpp"
#include "global_state.hpp"

#include "hooks/core_hooks.hpp"
#include "hooks/camera_hooks.hpp"

using namespace GlobalState;

namespace Mod
{

    // Camera mode flag values (WORD type)
    const WORD CAMERA_MODE_FPV = 0;
    const WORD CAMERA_MODE_TPV = 1;

    // Mod State
    std::vector<bool> g_toggle_key_was_pressed;
    std::vector<bool> g_fpv_key_was_pressed;
    std::vector<bool> g_tpv_key_was_pressed;

    bool g_mod_shutting_down = false;
    const int INIT_RETRY_MILLISECONDS = 500; // How often to retry finding game systems

    std::thread g_input_monitoring_thread;

    // Forward Declarations
    void InitializeModLogic();
    void ShutdownModLogic();
    [[noreturn]] void MonitorInputAndToggle();
    void SetCameraMode(WORD mode);
    WORD GetCurrentCameraMode();
    void ToggleViewAction();

} // namespace Mod

void Mod::InitializeModLogic()
{
    DMKLogger::configure(Constants::MOD_NAME, Constants::getLogFilename(), "%Y-%m-%d %H:%M:%S");
    DMKLogger &logger = DMKLogger::getInstance();

    loadConfig();

    logger.setLogLevel(DMKLogger::stringToLogLevel(g_config.log_level_str));
    logger.log(DMK::LOG_INFO, "KCD2_TPVToggle InitializeModLogic started.");
    DMKConfig::logAll();

    DMKMemory::initMemoryCache();

    logger.log(DMK::LOG_INFO, "Waiting for target game module and C_CameraManager instance...");

    HMODULE h_game_module = nullptr;
    unsigned int retry_log_counter = 0;
    const unsigned int log_every_n_retries = 4; // Log "still waiting" every ~2 seconds

    // Loop indefinitely until CameraManager is found or shutdown is signaled
    while (!Mod::g_mod_shutting_down)
    {
        h_game_module = GetModuleHandleA(Constants::MODULE_NAME);
        if (h_game_module)
        {
            // TODO: Placeholder
            break;
        }

        retry_log_counter++;
        if (retry_log_counter % log_every_n_retries == 0)
        {
            logger.log(DMK::LOG_DEBUG, "Still waiting for C_CameraManager...");
        }

        if (!Mod::g_mod_shutting_down)
            std::this_thread::sleep_for(std::chrono::milliseconds(INIT_RETRY_MILLISECONDS));
    }

    if (Mod::g_mod_shutting_down)
    {
        logger.log(DMK::LOG_INFO, "Mod shutdown signaled during initialization wait. Initialization aborted.");
        return;
    }

    // Get module information
    MODULEINFO mod_info = {};
    if (!GetModuleInformation(GetCurrentProcess(), h_game_module, &mod_info, sizeof(mod_info)))
    {
        logger.log(DMK::LOG_ERROR, "Failed to get module information: " + std::to_string(GetLastError()));
        return;
    }

    g_ModuleBase = reinterpret_cast<uintptr_t>(mod_info.lpBaseOfDll);
    g_ModuleSize = mod_info.SizeOfImage;

    initializeCoreHooks(g_ModuleBase, g_ModuleSize);
    initializeCameraHooks(g_ModuleBase, g_ModuleSize);

    Mod::g_toggle_key_was_pressed.assign(g_config.toggle_keys.size(), false);
    Mod::g_fpv_key_was_pressed.assign(g_config.fpv_keys.size(), false);
    Mod::g_tpv_key_was_pressed.assign(g_config.tpv_keys.size(), false);

    Mod::g_input_monitoring_thread = std::thread(Mod::MonitorInputAndToggle);

    logger.log(DMK::LOG_INFO, "KCD2_TPVToggle initialized successfully and input monitoring started.");
}

void Mod::SetCameraMode(WORD mode)
{

    DMKLogger &logger = DMKLogger::getInstance();
    // TODO: Placeholder
}

WORD Mod::GetCurrentCameraMode()
{
    // TODO: Placeholder
    return Mod::CAMERA_MODE_FPV;
}

void Mod::ToggleViewAction()
{
    WORD current_mode = GetCurrentCameraMode();
    if (current_mode == Mod::CAMERA_MODE_TPV)
        Mod::SetCameraMode(Mod::CAMERA_MODE_FPV);
    else
        Mod::SetCameraMode(Mod::CAMERA_MODE_TPV);
}

[[noreturn]] void Mod::MonitorInputAndToggle()
{
    DMKLogger &logger = DMKLogger::getInstance();
    logger.log(DMK::LOG_INFO, "Input monitoring thread started.");

    while (!Mod::g_mod_shutting_down)
    {
        // TODO: Placeholder
        // Ensure Camera is available before processing keys
        if (false)
        {
            if (!Mod::g_mod_shutting_down)
                std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        bool action_taken_this_cycle = false;

        if (!action_taken_this_cycle && !g_config.toggle_keys.empty())
        {
            for (size_t i = 0; i < g_config.toggle_keys.size(); ++i)
            {
                if (GetAsyncKeyState(g_config.toggle_keys[i]) & 0x8000)
                {
                    if (!Mod::g_toggle_key_was_pressed[i])
                    {
                        Mod::ToggleViewAction();
                        Mod::g_toggle_key_was_pressed[i] = true;
                        action_taken_this_cycle = true;
                        break;
                    }
                }
                else
                {
                    Mod::g_toggle_key_was_pressed[i] = false;
                }
            }
        }
        if (!action_taken_this_cycle && !g_config.fpv_keys.empty())
        {
            for (size_t i = 0; i < g_config.fpv_keys.size(); ++i)
            {
                if (GetAsyncKeyState(g_config.fpv_keys[i]) & 0x8000)
                {
                    if (!Mod::g_fpv_key_was_pressed[i])
                    {
                        Mod::SetCameraMode(Mod::CAMERA_MODE_FPV);
                        Mod::g_fpv_key_was_pressed[i] = true;
                        action_taken_this_cycle = true;
                        break;
                    }
                }
                else
                {
                    Mod::g_fpv_key_was_pressed[i] = false;
                }
            }
        }
        if (!action_taken_this_cycle && !g_config.tpv_keys.empty())
        {
            for (size_t i = 0; i < g_config.tpv_keys.size(); ++i)
            {
                if (GetAsyncKeyState(g_config.tpv_keys[i]) & 0x8000)
                {
                    if (!Mod::g_tpv_key_was_pressed[i])
                    {
                        Mod::SetCameraMode(Mod::CAMERA_MODE_TPV);
                        Mod::g_tpv_key_was_pressed[i] = true;
                        break;
                    }
                }
                else
                {
                    Mod::g_tpv_key_was_pressed[i] = false;
                }
            }
        }
        if (!Mod::g_mod_shutting_down)
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    logger.log(DMK::LOG_INFO, "Input monitoring thread exiting due to shutdown signal.");
}

void Mod::ShutdownModLogic()
{
    DMKLogger &logger = DMKLogger::getInstance();
    logger.log(DMK::LOG_INFO, "KCD2_TPVToggle Shutting Down...");
    Mod::g_mod_shutting_down = true;

    if (Mod::g_input_monitoring_thread.joinable())
    {
        logger.log(DMK::LOG_DEBUG, "Waiting for input monitoring thread to join...");
        Mod::g_input_monitoring_thread.join();
        logger.log(DMK::LOG_DEBUG, "Input monitoring thread joined.");
    }
    cleanupCameraHooks();
    cleanupCoreHooks();

    DMKConfig::clearRegisteredItems();
    DMKMemory::clearMemoryCache();
    logger.log(DMK::LOG_INFO, "KCD2_TPVToggle Shutdown Complete.");
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    (void)lpReserved;

    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    {
        DisableThreadLibraryCalls(hModule);
        std::thread init_thread(Mod::InitializeModLogic);
        init_thread.detach();
    }
    break;
    case DLL_PROCESS_DETACH:
        Mod::ShutdownModLogic();
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    }
    return TRUE;
}
