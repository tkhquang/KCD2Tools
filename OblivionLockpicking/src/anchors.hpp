/**
 * @file anchors.hpp
 * @brief The WHGame.dll anchors and the features they enable.
 *
 * Everything the mod reads, calls or hooks inside the game resolves at startup from RTTI type names and byte
 * signatures, and each feature turns on only when every anchor it uses resolved (anchor::evaluate_gate).
 */
#ifndef OBLIVIONLP_ANCHORS_HPP
#define OBLIVIONLP_ANCHORS_HPP

#include <DetourModKit.hpp>

#include <cstdint>

namespace OblivionLockpicking
{
    /** @brief The resolved anchors, 0 when unresolved, and the features they enable. */
    struct Anchors
    {
        /// SSystemGlobalEnvironment (gEnv), a static struct inside the game image.
        std::uintptr_t genv{0};
        /// Primary vtable of wh::playermodule::C_LockPicking.
        std::uintptr_t lockpicking_vtable{0};
        /// C_LockPicking::Update, slot 19 of that vtable.
        std::uintptr_t lockpicking_update{0};
        /// Primary vtable of wh::entitymodule::C_Player, the only owner the mod takes over.
        std::uintptr_t player_vtable{0};
        /// Primary vtable of wh::rpgmodule::C_Soul, checked before its skill-level slot is called.
        std::uintptr_t soul_vtable{0};
        /// Primary vtable of wh::rpgmodule::C_RPGMinigames, checked before its noise slot is called.
        std::uintptr_t rpg_minigames_vtable{0};
        /// Primary vtable of CEntitySystem, checked before GetEntity is called.
        std::uintptr_t entity_system_vtable{0};
        /// Primary vtable of CXConsole, checked before its slots are called.
        std::uintptr_t console_vtable{0};
        /// Primary vtable of CDXInput, checked before its listener slots are called.
        std::uintptr_t input_vtable{0};
        /// Primary vtable of CFlashUI, checked before its element list is read.
        std::uintptr_t flash_ui_vtable{0};
        /// Primary vtable of CFlashUIElement, checked before its slots are called.
        std::uintptr_t flash_element_vtable{0};
        /// Primary vtable of CFlashPlayer, checked before its slots are called.
        std::uintptr_t flash_player_vtable{0};
        /// Primary vtable of wh::playermodule::C_LockPickingTurnFailAction, checked before it is finished.
        std::uintptr_t turn_fail_vtable{0};
        /// void(emitter*, float dt): emits rate * level * dt to the AI hearing system when the level is non-zero.
        std::uintptr_t emitter_step{0};
        /// game_context*(): the game's context singleton (a guarded static, so it is called, never read raw).
        std::uintptr_t game_context{0};
        /**
         * @brief void(C_LockPicking*): what the end of the failure animation runs in the failure state.
         * @details Fail XP, the fail-open roll, the reset and the return to play, or the end of the minigame without a
         *          pick. It checks the state first, so a second call does nothing.
         */
        std::uintptr_t on_fail_anim_done{0};
        /// void(IAction*): Mannequin IAction::ForceFinish.
        std::uintptr_t action_force_finish{0};
        /// void(C_UIHudMessages*, u8 code): shows the lockpicking HUD texts (code 4 is "Lockpicking attempt failed").
        std::uintptr_t hud_lockpicking_message{0};

        /// The tumbler lock itself: the session, its controls, its noise and its movie. Off keeps the mod inert.
        bool minigame{false};
        /// The game's own sounds at the lock: the sweet-spot click without SoundFramework, and the pick break.
        bool vanilla_sounds{false};
        /// A wrong set's failure can end before its animation plays.
        bool short_failure{false};
        /// The HUD failure text can be held back for the tumbler lock's wrong sets.
        bool quiet_failure{false};
        /// The player's Thievery level can be read, for KeepSetTumblersOnFail=Thievery.
        bool thievery{false};
    };

    /**
     * @brief Resolves every anchor inside @p game, logs each one, and gates each feature on its own anchors.
     * @note Setup/control-plane only.
     */
    [[nodiscard]] Anchors resolve_anchors(DMK::Region game);
} // namespace OblivionLockpicking

#endif // OBLIVIONLP_ANCHORS_HPP
