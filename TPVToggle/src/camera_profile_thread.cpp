/**
 * @file camera_profile_thread.cpp
 * @brief Implements continuous camera offset adjustment using DMKInputManager.
 *
 * Edge-triggered profile actions (save, cycle, reset, update, delete, master toggle)
 * are handled by DMKInputManager press callbacks registered in dllmain.cpp.
 * This thread only handles continuous offset adjustment by querying
 * is_binding_active() for the six offset direction bindings at ~60 Hz.
 */

#include "camera_profile_thread.h"
#include "camera_profile.h"
#include "global_state.h"

#include <DetourModKit.hpp>

using DetourModKit::LogLevel;

/**
 * @brief Camera profile offset adjustment thread.
 * @details Polls InputManager binding states at ~60 Hz to apply continuous
 *          offset adjustments while keys are held. Only active when camera
 *          adjustment mode is enabled.
 */
DWORD WINAPI CameraProfileThread(LPVOID param)
{
    DMKLogger &logger = DMKLogger::get_instance();
    logger.log(LogLevel::Info, "CameraProfileThread: Started");

    CameraProfileThreadData *data = static_cast<CameraProfileThreadData *>(param);
    if (!data)
    {
        logger.log(LogLevel::Error, "CameraProfileThread: NULL data received");
        return 1;
    }
    float adjustmentStep = data->adjustmentStep;
    delete data;

    DMKInputManager &input_mgr = DMKInputManager::get_instance();

    logger.log(LogLevel::Info, "CameraProfileThread: Using InputManager for offset adjustment queries");

    while (WaitForSingleObject(g_exitEvent, 16) != WAIT_OBJECT_0) // ~60 Hz
    {
        try
        {
            if (!g_cameraAdjustmentMode.load())
                continue;

            // Query InputManager for held offset keys and apply continuous adjustment
            CameraProfileManager &profile_mgr = CameraProfileManager::getInstance();

            if (input_mgr.is_binding_active("offset_x_inc"))
                profile_mgr.adjustOffset(adjustmentStep, 0.0f, 0.0f);
            if (input_mgr.is_binding_active("offset_x_dec"))
                profile_mgr.adjustOffset(-adjustmentStep, 0.0f, 0.0f);
            if (input_mgr.is_binding_active("offset_y_inc"))
                profile_mgr.adjustOffset(0.0f, adjustmentStep, 0.0f);
            if (input_mgr.is_binding_active("offset_y_dec"))
                profile_mgr.adjustOffset(0.0f, -adjustmentStep, 0.0f);
            if (input_mgr.is_binding_active("offset_z_inc"))
                profile_mgr.adjustOffset(0.0f, 0.0f, adjustmentStep);
            if (input_mgr.is_binding_active("offset_z_dec"))
                profile_mgr.adjustOffset(0.0f, 0.0f, -adjustmentStep);
        }
        catch (const std::exception &e)
        {
            logger.log(LogLevel::Error, "CameraProfileThread: Exception: " + std::string(e.what()));
            Sleep(1000);
        }
        catch (...)
        {
            logger.log(LogLevel::Error, "CameraProfileThread: Unknown exception");
            Sleep(1000);
        }
    }

    logger.log(LogLevel::Info, "CameraProfileThread: Exiting");
    return 0;
}
