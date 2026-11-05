/**
 * @file anchors.hpp
 * @brief The WHGame.dll anchors and the features they enable.
 *
 * Everything the mod reads, calls or hooks inside the game resolves at startup from RTTI type names and byte
 * signatures. Each feature turns on only when every anchor it uses resolved (anchor::evaluate_gate).
 */
#ifndef HUDCLOCK_ANCHORS_HPP
#define HUDCLOCK_ANCHORS_HPP

#include "core/clock_style.hpp"

#include <DetourModKit.hpp>

#include <array>
#include <cstdint>

namespace HUDClock
{
    /** @brief The resolved anchors, 0 when unresolved, and the features they enable. */
    struct Anchors
    {
        /// SSystemGlobalEnvironment (gEnv), a static struct inside the game image.
        std::uintptr_t genv{0};
        /// Primary vtable of CFlashUI, checked before its element list is read.
        std::uintptr_t flash_ui_vtable{0};
        /// Primary vtable of CFlashUIElement, checked before its slots are called.
        std::uintptr_t flash_element_vtable{0};
        /// Primary vtable of CFlashPlayer, checked before its slots are called.
        std::uintptr_t flash_player_vtable{0};
        /// Primary vtable of wh::guimodule::C_UIHudMask, checked before its HUD part bits are read.
        std::uintptr_t hud_mask_vtable{0};
        /// The static that holds the C_UIHudMask singleton, null while no HUD exists.
        std::uintptr_t hud_mask_global{0};
        /// The static that caches the game context, null until the game creates it.
        std::uintptr_t game_context_global{0};
        /// Primary vtable of wh::rpgmodule::C_RPGModule, checked before its calendar is read.
        std::uintptr_t rpg_module_vtable{0};
        /// Primary vtable of wh::rpgmodule::C_Calendar, checked before its world time is read.
        std::uintptr_t calendar_vtable{0};
        /// CharacterManager::Update(bool paused), the main-thread frame call the clock runs in.
        std::uintptr_t character_manager_update{0};
        /// Primary vtable of wh::playermodule::C_PlayerModule, checked before its minigame manager is read.
        std::uintptr_t player_module_vtable{0};
        /// Primary vtable of wh::entitymodule::C_Player, the owner of each minigame the player plays.
        std::uintptr_t player_vtable{0};
        /// Primary vtable of each core::MINIGAMES class, in that order.
        std::array<std::uintptr_t, core::MINIGAMES.size()> minigame_vtables{};
        /// Primary vtable of wh::game::C_PhotomodeManager, checked before its photo mode flag is read.
        std::uintptr_t photomode_manager_vtable{0};

        /// The clock itself. Off keeps the mod inert.
        bool clock{false};
        /// The clock in the minigames ShowInMinigames names. Off leaves the clock to the HUD parts it follows.
        bool minigames{false};
        /// The clock in photo mode (ShowInPhotoMode). Off leaves the clock to the HUD parts it follows.
        bool photo_mode{false};
    };

    /**
     * @brief Resolves every anchor inside @p game, logs each one, and gates each feature on its anchors.
     * @note Setup/control-plane only.
     */
    [[nodiscard]] Anchors resolve_anchors(DMK::Region game);
} // namespace HUDClock

#endif // HUDCLOCK_ANCHORS_HPP
