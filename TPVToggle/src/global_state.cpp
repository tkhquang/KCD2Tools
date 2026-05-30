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

namespace TPVToggle
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

    ScrollHookState &scroll_hook_state() noexcept
    {
        static ScrollHookState state;
        return state;
    }

    TpvCameraState &camera_state() noexcept
    {
        static TpvCameraState state;
        return state;
    }

    PlayerTransform &player_transform() noexcept
    {
        static PlayerTransform state;
        return state;
    }

} // namespace TPVToggle
