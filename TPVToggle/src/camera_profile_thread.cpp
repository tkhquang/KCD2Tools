/**
 * @file camera_profile_thread.cpp
 * @brief Implements continuous camera offset adjustment via DMK::InputManager.
 *
 * Edge-triggered profile actions (save, cycle, reset, update, delete, master toggle)
 * are handled by DMK::InputManager press callbacks. This worker only handles
 * continuous offset adjustment by querying is_binding_active() for the six offset
 * direction bindings at ~60 Hz.
 */

#include "camera_profile_thread.hpp"
#include "camera_profile.hpp"
#include "config.hpp"
#include "global_state.hpp"
#include "utils.hpp"

#include <DetourModKit.hpp>

namespace TPVToggle
{

void camera_profile_body(std::stop_token st)
{
    DMK::Logger &logger = DMK::Logger::get_instance();
    logger.info("CameraProfile: Started (InputManager-driven offset adjustment)");

    DMK::InputManager &input_mgr = DMK::InputManager::get_instance();

    while (sleep_until_stop(st, 16)) // ~60 Hz
    {
        try
        {
            if (!camera_state().adjustmentMode.load())
                continue;

            const float step = settings().offsetAdjustmentStep.load();
            CameraProfileManager &profile_mgr = CameraProfileManager::getInstance();

            if (input_mgr.is_binding_active("offset_x_inc"))
                profile_mgr.adjustOffset(step, 0.0f, 0.0f);
            if (input_mgr.is_binding_active("offset_x_dec"))
                profile_mgr.adjustOffset(-step, 0.0f, 0.0f);
            if (input_mgr.is_binding_active("offset_y_inc"))
                profile_mgr.adjustOffset(0.0f, step, 0.0f);
            if (input_mgr.is_binding_active("offset_y_dec"))
                profile_mgr.adjustOffset(0.0f, -step, 0.0f);
            if (input_mgr.is_binding_active("offset_z_inc"))
                profile_mgr.adjustOffset(0.0f, 0.0f, step);
            if (input_mgr.is_binding_active("offset_z_dec"))
                profile_mgr.adjustOffset(0.0f, 0.0f, -step);
        }
        catch (const std::exception &e)
        {
            logger.error("CameraProfile: Exception: {}", e.what());
            if (!sleep_until_stop(st, 1000))
                return;
        }
        catch (...)
        {
            logger.error("CameraProfile: Unknown exception");
            if (!sleep_until_stop(st, 1000))
                return;
        }
    }

    logger.info("CameraProfile: Exiting");
}

} // namespace TPVToggle
