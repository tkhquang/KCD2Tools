/**
 * @file game_interface.hpp
 * @brief Resolves the game's global-context pointer (the camera-manager root).
 *
 * The global-context -> camera-manager chain is the entry point the game-state
 * detection walks to read the active camera (see game_state.cpp). This unit
 * resolves and caches the context-pointer storage from the module image once.
 */
#ifndef TPVCAMERA_GAME_INTERFACE_HPP
#define TPVCAMERA_GAME_INTERFACE_HPP

#include <cstdint>
#include <cstddef>

namespace TPVCamera
{

/**
 * @brief Initialize game interface with dynamic AOB scanning.
 * @details Resolves the global-context pointer storage from the module image and stores it for the
 *          game-state camera reads (game_state.cpp); without this call those reads find no state.
 * @param module_base Base address of the target game module.
 * @param module_size Size of the target game module in bytes.
 * @return true if initialization successful, false otherwise.
 */
[[nodiscard]] bool initialize_game_interface(uintptr_t module_base, size_t module_size);

/**
 * @brief Clean up game interface resources.
 */
void cleanup_game_interface();

} // namespace TPVCamera

#endif // TPVCAMERA_GAME_INTERFACE_HPP
