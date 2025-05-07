#include "camera_profile_thread.h"
#include "camera_profile.h"
#include "logger.h"
#include "utils.h"
#include "global_state.h"
#include "config.h"

#include <unordered_map>

// External config reference
extern Config g_config;

DWORD WINAPI CameraProfileThread(LPVOID param)
{
    Logger &logger = Logger::getInstance();
    logger.log(LOG_INFO, "CameraProfileThread: Started");

    // Read thread parameters
    CameraProfileThreadData *data = static_cast<CameraProfileThreadData *>(param);
    if (!data)
    {
        logger.log(LOG_ERROR, "CameraProfileThread: NULL data received");
        return 1;
    }

    float adjustmentStep = data->adjustmentStep;
    delete data; // Clean up thread data

    // Initialize key tracking
    std::unordered_map<int, bool> key_down_states;

    // Add all camera profile keys to tracking
    auto initialize_keys = [&](const auto &keys)
    {
        for (int vk : keys)
        {
            if (vk != 0)
            {
                key_down_states[vk] = false;
            }
        }
    };

    initialize_keys(g_config.master_toggle_keys);
    initialize_keys(g_config.profile_save_keys);
    initialize_keys(g_config.profile_cycle_keys);
    initialize_keys(g_config.profile_reset_keys);
    initialize_keys(g_config.offset_x_inc_keys);
    initialize_keys(g_config.offset_x_dec_keys);
    initialize_keys(g_config.offset_y_inc_keys);
    initialize_keys(g_config.offset_y_dec_keys);
    initialize_keys(g_config.offset_z_inc_keys);
    initialize_keys(g_config.offset_z_dec_keys);

    // Main thread loop
    while (WaitForSingleObject(g_exitEvent, 16) != WAIT_OBJECT_0)
    { // ~60 Hz polling
        try
        {
            // Process master toggle keys
            auto process_toggle_keys = [&]()
            {
                for (int vk : g_config.master_toggle_keys)
                {
                    if (vk != 0)
                    {
                        bool pressed = (GetAsyncKeyState(vk) & 0x8000) != 0;
                        if (pressed && !key_down_states[vk])
                        {
                            // Toggle adjustment mode
                            bool newMode = !g_cameraAdjustmentMode.load();
                            g_cameraAdjustmentMode.store(newMode);
                            logger.log(LOG_INFO, "CameraProfileThread: Adjustment mode " +
                                                     std::string(newMode ? "ENABLED" : "DISABLED"));
                        }
                        key_down_states[vk] = pressed;
                    }
                }
            };

            // Process profile save keys
            auto process_save_keys = [&]()
            {
                for (int vk : g_config.profile_save_keys)
                {
                    if (vk != 0)
                    {
                        bool pressed = (GetAsyncKeyState(vk) & 0x8000) != 0;
                        if (pressed && !key_down_states[vk])
                        {
                            // Generate auto profile name based on current time
                            auto now = std::chrono::system_clock::now();
                            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                          now.time_since_epoch()) %
                                      1000;
                            auto timer = std::chrono::system_clock::to_time_t(now);
                            std::stringstream ss;
                            ss << "Profile_";
                            ss << std::put_time(std::localtime(&timer), "%H%M%S");
                            ss << "_" << std::setfill('0') << std::setw(3) << ms.count();

                            std::string profileName = ss.str();
                            CameraProfileManager::getInstance().saveCurrentProfile(profileName);
                        }
                        key_down_states[vk] = pressed;
                    }
                }
            };

            // Process profile cycle keys
            auto process_cycle_keys = [&]()
            {
                for (int vk : g_config.profile_cycle_keys)
                {
                    if (vk != 0)
                    {
                        bool pressed = (GetAsyncKeyState(vk) & 0x8000) != 0;
                        if (pressed && !key_down_states[vk])
                        {
                            CameraProfileManager::getInstance().cycleToNextProfile();
                            // Update global offset
                            g_currentCameraOffset = CameraProfileManager::getInstance().getCurrentOffset();
                        }
                        key_down_states[vk] = pressed;
                    }
                }
            };

            // Process profile reset keys
            auto process_reset_keys = [&]()
            {
                for (int vk : g_config.profile_reset_keys)
                {
                    if (vk != 0)
                    {
                        bool pressed = (GetAsyncKeyState(vk) & 0x8000) != 0;
                        if (pressed && !key_down_states[vk])
                        {
                            CameraProfileManager::getInstance().resetToDefault();
                            // Update global offset
                            g_currentCameraOffset = CameraProfileManager::getInstance().getCurrentOffset();
                        }
                        key_down_states[vk] = pressed;
                    }
                }
            };

            // Always process master toggle keys
            process_toggle_keys();

            // Process other keys only if adjustment mode is active
            if (g_cameraAdjustmentMode.load())
            {
                process_save_keys();
                process_cycle_keys();
                process_reset_keys();

                // Process X offset adjustment
                for (int vk : g_config.offset_x_inc_keys)
                {
                    if (vk != 0)
                    {
                        bool pressed = (GetAsyncKeyState(vk) & 0x8000) != 0;
                        if (pressed)
                        { // Continuous adjustment while held
                            CameraProfileManager::getInstance().adjustOffset(adjustmentStep, 0.0f, 0.0f);
                        }
                        key_down_states[vk] = pressed;
                    }
                }

                for (int vk : g_config.offset_x_dec_keys)
                {
                    if (vk != 0)
                    {
                        bool pressed = (GetAsyncKeyState(vk) & 0x8000) != 0;
                        if (pressed)
                        { // Continuous adjustment while held
                            CameraProfileManager::getInstance().adjustOffset(-adjustmentStep, 0.0f, 0.0f);
                        }
                        key_down_states[vk] = pressed;
                    }
                }

                // Process Y offset adjustment
                for (int vk : g_config.offset_y_inc_keys)
                {
                    if (vk != 0)
                    {
                        bool pressed = (GetAsyncKeyState(vk) & 0x8000) != 0;
                        if (pressed)
                        { // Continuous adjustment while held
                            CameraProfileManager::getInstance().adjustOffset(0.0f, adjustmentStep, 0.0f);
                        }
                        key_down_states[vk] = pressed;
                    }
                }

                for (int vk : g_config.offset_y_dec_keys)
                {
                    if (vk != 0)
                    {
                        bool pressed = (GetAsyncKeyState(vk) & 0x8000) != 0;
                        if (pressed)
                        { // Continuous adjustment while held
                            CameraProfileManager::getInstance().adjustOffset(0.0f, -adjustmentStep, 0.0f);
                        }
                        key_down_states[vk] = pressed;
                    }
                }

                // Process Z offset adjustment
                for (int vk : g_config.offset_z_inc_keys)
                {
                    if (vk != 0)
                    {
                        bool pressed = (GetAsyncKeyState(vk) & 0x8000) != 0;
                        if (pressed)
                        { // Continuous adjustment while held
                            CameraProfileManager::getInstance().adjustOffset(0.0f, 0.0f, adjustmentStep);
                        }
                        key_down_states[vk] = pressed;
                    }
                }

                for (int vk : g_config.offset_z_dec_keys)
                {
                    if (vk != 0)
                    {
                        bool pressed = (GetAsyncKeyState(vk) & 0x8000) != 0;
                        if (pressed)
                        { // Continuous adjustment while held
                            CameraProfileManager::getInstance().adjustOffset(0.0f, 0.0f, -adjustmentStep);
                        }
                        key_down_states[vk] = pressed;
                    }
                }
            }
        }
        catch (const std::exception &e)
        {
            logger.log(LOG_ERROR, "CameraProfileThread: Error: " + std::string(e.what()));
            Sleep(1000); // Throttle on error
        }
        catch (...)
        {
            logger.log(LOG_ERROR, "CameraProfileThread: Unknown error");
            Sleep(1000); // Throttle on error
        }
    }

    logger.log(LOG_INFO, "CameraProfileThread: Exiting");
    return 0;
}
