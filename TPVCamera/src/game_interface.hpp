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

#include <DetourModKit/error.hpp>

namespace TPVCamera
{

    /**
     * @brief Stores the resolved global-context pointer for the game-state camera reads.
     * @details Reads the Context anchor (resolved by resolve_all_anchors()) and stores the slot for the
     *          game-state camera reads (game_state.cpp); without this call those reads find no state.
     * @return An empty value when the context anchor resolved, or ErrorCode::NoMatch when its cascade missed.
     * @note Call after resolve_all_anchors().
     */
    [[nodiscard]] DMK::Result<void> initialize_game_interface();

    /**
     * @brief Clean up game interface resources.
     */
    void cleanup_game_interface();

} // namespace TPVCamera

#endif // TPVCAMERA_GAME_INTERFACE_HPP
