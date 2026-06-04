/**
 * @file global_state.cpp
 * @brief Storage backing the cross-module shared-state accessors.
 */

#include "global_state.hpp"

// Stable unmangled symbol for the resolved game context pointer (see header).
extern "C"
{
    std::byte *g_global_context_ptr_address = nullptr;
}

namespace TPVCamera
{
    ModuleInfo &module_info() noexcept
    {
        static ModuleInfo state;
        return state;
    }

    OverlayState &overlay_state() noexcept
    {
        static OverlayState state;
        return state;
    }

    CameraState &camera_state() noexcept
    {
        static CameraState state;
        return state;
    }

    InteractionAimPose &interaction_aim_pose() noexcept
    {
        static InteractionAimPose pose;
        return pose;
    }

    std::atomic<uint32_t> &game_state_mask() noexcept
    {
        static std::atomic<uint32_t> mask{0};
        return mask;
    }

    std::atomic<bool> &game_world_ready() noexcept
    {
        static std::atomic<bool> ready{false};
        return ready;
    }

} // namespace TPVCamera
