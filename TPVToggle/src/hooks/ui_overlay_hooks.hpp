/**
 * @file hooks/ui_overlay_hooks.hpp
 * @brief Header for UI overlay hooks functionality.
 *
 * Provides functions to initialize and manage hooks that directly intercept
 * the game's UI overlay show and hide functions, eliminating the need for
 * polling-based overlay state detection.
 */
#ifndef UI_OVERLAY_HOOKS_HPP
#define UI_OVERLAY_HOOKS_HPP

#include <cstdint>
#include <cstddef>

namespace TPVToggle
{

/**
 * @brief Initialize UI overlay hooks.
 * @param module_base Base address of the target game module.
 * @param module_size Size of the target game module in bytes.
 * @return true if initialization successful, false otherwise.
 */
[[nodiscard]] bool initializeUiOverlayHooks(uintptr_t module_base, size_t module_size);

/**
 * @brief Handler for hold-to-scroll key state changes
 * @details Called by the main monitor thread when hold-to-scroll key state changes
 * @param holdKeyPressed Whether a hold key is currently pressed
 * @return true if the state was successfully handled, false otherwise
 */
bool handleHoldToScrollKeyState(bool holdKeyPressed);

} // namespace TPVToggle

#endif // UI_OVERLAY_HOOKS_HPP
