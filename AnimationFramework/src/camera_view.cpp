/**
 * @file camera_view.cpp
 * @brief The `mod_camera_view` reader (see camera_view.hpp).
 */

#include "camera_view.hpp"
#include "console_vars.hpp"
#include "constants.hpp"

#include <DetourModKit.hpp>

#include <cstdint>

namespace AnimationFramework
{
    namespace
    {
        // The variable's ICVar. The console keeps it for the process, and the contract forbids unregistering it.
        // Only the frame tick reads these.
        void *s_cvar = nullptr;
        std::int32_t s_last_value = -1;
        bool s_console_warned = false;
        // Latches a failed find or registration, so the frame tick does not retry it every frame.
        bool s_cvar_failed = false;
    } // namespace

    void bind_camera_view() noexcept
    {
        s_cvar = nullptr;
        s_last_value = -1;
        s_console_warned = false;
        s_cvar_failed = false;
    }

    CameraView poll_camera_view() noexcept
    {
        if (s_cvar_failed)
        {
            return CameraView::FirstPerson;
        }
        if (s_cvar == nullptr)
        {
            if (!console_ready())
            {
                if (!s_console_warned)
                {
                    s_console_warned = true;
                    (void)DMK::log().try_log(DMK::LogLevel::Debug, "CameraView: the console is not up yet");
                }
                return CameraView::FirstPerson;
            }
            bool registered = false;
            s_cvar =
                find_or_register_int_cvar(constants::CAMERA_VIEW_CVAR, constants::CAMERA_VIEW_CVAR_HELP, registered);
            if (s_cvar == nullptr)
            {
                s_cvar_failed = true;
                (void)DMK::log().try_log(
                    DMK::LogLevel::Warning,
                    "CameraView: {} could not be found or registered; the view reads as first person",
                    constants::CAMERA_VIEW_CVAR
                );
                return CameraView::FirstPerson;
            }
            if (registered)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Info,
                    "CameraView: registered {} (no camera mod had yet)",
                    constants::CAMERA_VIEW_CVAR
                );
            }
        }
        const std::int32_t value = cvar_int(s_cvar);
        if (value != s_last_value)
        {
            s_last_value = value;
            (void)DMK::log().try_log(
                DMK::LogLevel::Info,
                "CameraView: {} = {} ({} person)",
                constants::CAMERA_VIEW_CVAR,
                value,
                value >= 1 ? "third" : "first"
            );
        }
        return value >= 1 ? CameraView::ThirdPerson : CameraView::FirstPerson;
    }

    void unbind_camera_view() noexcept
    {
        s_cvar = nullptr;
    }
} // namespace AnimationFramework
