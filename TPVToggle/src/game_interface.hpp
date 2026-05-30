/**
 * @file game_interface.hpp
 * @brief Provides interface to game memory structures and state.
 *
 * Handles the complex pointer chain navigation to access game state like
 * TPV flags and manages memory safety and validation.
 */
#ifndef GAME_INTERFACE_HPP
#define GAME_INTERFACE_HPP

#include <windows.h>
#include <cstdint>
#include <cstddef>

#include "math_utils.hpp"

namespace TPVToggle
{

/**
 * @brief Initialize game interface with dynamic AOB scanning.
 * @param module_base Base address of the target game module.
 * @param module_size Size of the target game module in bytes.
 * @return true if initialization successful, false otherwise.
 */
[[nodiscard]] bool initializeGameInterface(uintptr_t module_base, size_t module_size);

/**
 * @brief Safely resets the scroll accumulator to zero
 * @param logReset Whether to log successful resets (to avoid log spam)
 * @return true if successfully reset, false if pointer invalid or already zero
 */
bool resetScrollAccumulator(bool logReset = false);

/**
 * @brief Clean up game interface resources.
 */
void cleanupGameInterface();

/**
 * @brief Gets the resolved address of the TPV flag.
 * @details Resolves the pointer chain global context -> camera manager -> flag:
 *          (*(*(storage) + OFFSET_ManagerPtrStorage)) + OFFSET_TpvFlag.
 * @return Pointer to TPV flag byte, or nullptr if resolution fails.
 */
[[nodiscard]] volatile std::byte *getResolvedTpvFlagAddress();

/**
 * @brief Gets the current view state (FPV=0, TPV=1).
 * @return 0 for FPV, 1 for TPV, -1 on error.
 */
[[nodiscard]] int getViewState();

/**
 * @brief Sets the view state.
 * @param new_state 0 for FPV, 1 for TPV.
 * @param key_pressed_vk Optional VK code that triggered this change.
 * @return true if successful, false otherwise.
 */
bool setViewState(BYTE new_state, int *key_pressed_vk = nullptr);

/**
 * @brief Toggles between FPV and TPV modes.
 * @param key_pressed_vk Optional VK code that triggered this change.
 * @return true if successful, false otherwise.
 */
bool safeToggleViewState(int *key_pressed_vk = nullptr);

/**
 * @brief Reads the player entity's world transform into position and orientation.
 * @param outPosition Receives the world-space position.
 * @param outOrientation Receives the world-space orientation.
 * @return true on success, false if the player entity is null or unreadable.
 */
bool GetPlayerWorldTransform(Vector3 &outPosition, Quaternion &outOrientation);

} // namespace TPVToggle

#endif // GAME_INTERFACE_HPP
