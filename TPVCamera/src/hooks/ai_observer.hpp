/**
 * @file hooks/ai_observer.hpp
 * @brief Keeps the AI's camera observer at the first-person eye while the third-person offset renders.
 */
#ifndef TPVCAMERA_HOOKS_AI_OBSERVER_HPP
#define TPVCAMERA_HOOKS_AI_OBSERVER_HPP

#include "math_utils.hpp"

#include <DetourModKit/hook.hpp>

#include <cstddef>
#include <cstdint>

namespace TPVCamera
{
    /**
     * @brief Hooks the camera observer's update, found through its RTTI vtable (no signature scan).
     * @details Best-effort: without it the camera still works, and NPCs just beyond the pulled-back camera's reach stay
     *          hidden in third person, as they would for the unmodded game with its view camera there. The slot's
     *          target must lie in the game image and still read the view camera (Aob::k_cameraObserverUpdateBody
     *          among its first instructions), so a patch that moves the slot leaves the hook uninstalled rather than
     *          hooking another function.
     */
    void install_camera_observer_hook(std::uintptr_t module_base, std::size_t module_size, DMK::hook::HookStack &hooks);
} // namespace TPVCamera

#endif // TPVCAMERA_HOOKS_AI_OBSERVER_HPP
