/**
 * @file hooks/fov_hook.hpp
 * @brief Header for TPV FOV hook functionality.
 *
 * Provides functions to initialize and manage the hook that modifies
 * the field of view when in third-person view mode.
 */
#ifndef FOV_HOOK_HPP
#define FOV_HOOK_HPP

#include <cstdint>
#include <cstddef>

namespace TPVToggle
{

/** @brief Signature of the engine TPV FOV calculation function (trampoline type). */
using TpvFovCalculateFunc = void(__fastcall *)(float *pViewStruct, float deltaTime);

/**
 * @brief Initialize the TPV FOV hook.
 * @param module_base Base address of the target game module.
 * @param module_size Size of the target game module in bytes.
 * @param desired_fov_degrees Desired FOV in degrees (or -1 to disable).
 * @return true if initialization successful, false otherwise.
 */
[[nodiscard]] bool initializeFovHook(uintptr_t module_base, size_t module_size, float desired_fov_degrees);

} // namespace TPVToggle

#endif // FOV_HOOK_HPP
