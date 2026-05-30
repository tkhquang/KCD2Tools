/**
 * @file hooks/tpv_input_hook.hpp
 * @brief Hook for the third-person camera input processing function
 *
 * Provides customizable camera control including sensitivity adjustment
 * and vertical pitch limits for third-person view.
 */
#ifndef TPV_INPUT_HOOK_HPP
#define TPV_INPUT_HOOK_HPP

#include <cstdint>

namespace TPVToggle
{

/**
 * @brief Initializes the TPV camera input hook
 * @param moduleBase Base address of the game's main module
 * @param moduleSize Size of the game's main module
 * @return true if initialization was successful, false otherwise
 */
[[nodiscard]] bool initializeTpvInputHook(uintptr_t moduleBase, size_t moduleSize);

/**
 * @brief Reset camera angles to default values
 * @details Used when switching views or resetting camera state
 */
void resetCameraAngles();

} // namespace TPVToggle

#endif // TPV_INPUT_HOOK_HPP
