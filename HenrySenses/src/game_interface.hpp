/**
 * @file game_interface.hpp
 * @brief Publishes the game's global-context storage slot (the camera-manager and minigame root).
 */
#ifndef HENRYSENSES_GAME_INTERFACE_HPP
#define HENRYSENSES_GAME_INTERFACE_HPP

#include <DetourModKit/error.hpp>

namespace HenrySenses
{
    /**
     * @brief Publishes the resolved global-context slot for the gate reads in game_state.cpp.
     * @return An empty value when the Context anchor resolved, or ErrorCode::NoMatch when it did not.
     * @note Call after resolve_all_anchors().
     */
    [[nodiscard]] DMK::Result<void> initialize_game_interface();

    /** @brief Clears the published global-context slot. */
    void cleanup_game_interface() noexcept;

} // namespace HenrySenses

#endif // HENRYSENSES_GAME_INTERFACE_HPP
