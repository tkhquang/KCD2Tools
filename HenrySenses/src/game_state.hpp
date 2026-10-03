/**
 * @file game_state.hpp
 * @brief Game-state signals and the HideIn situation lists that hide highlights during dialogue, combat or minigames.
 *
 * Every signal is a guarded memory read, so no hook is needed. Combat and dialogue come from the active wh::game
 * camera class, and the minigame bit from the C_MinigameManager's active-minigame list. A failed read omits its bit
 * rather than faulting.
 */
#ifndef HENRYSENSES_GAME_STATE_HPP
#define HENRYSENSES_GAME_STATE_HPP

#include <cstdint>
#include <string>
#include <string_view>

namespace HenrySenses
{
    /**
     * @enum GameState
     * @brief Bit flags for the game states the gates react to.
     */
    enum class GameState : std::uint32_t
    {
        /// Combat camera active.
        Combat = 1u << 1,
        /// Dialogue camera active.
        Dialogue = 1u << 2,
        /// A minigame owned by the player is on screen.
        Minigame = 1u << 3,
    };

    /**
     * @brief Returns the raw bit value of a GameState flag.
     * @param state The flag.
     * @return Its bit.
     */
    [[nodiscard]] constexpr std::uint32_t state_bit(GameState state) noexcept
    {
        return static_cast<std::uint32_t>(state);
    }

    /**
     * @brief Reads the current game-state bit mask from the live engine signals.
     * @param c_player Live C_Player used to confirm minigame ownership, or 0 to accept the first active minigame.
     * @return The raw (un-debounced) mask.
     * @note Thread-safe: guarded reads and cached RTTI identities only.
     */
    [[nodiscard]] std::uint32_t poll_game_state(std::uintptr_t c_player) noexcept;

    /**
     * @struct StateList
     * @brief A parsed situation list: its mask, and the tokens it did not know.
     */
    struct StateList
    {
        std::uint32_t mask{0};
        /// The unknown tokens, comma-separated as written; empty when every token was known.
        std::string unknown{};
    };

    /**
     * @brief Parses a comma-separated situation list (Dialogue, Combat, Minigame; case-insensitive; empty is none).
     *        Dialog is accepted for Dialogue.
     * @param list The INI value.
     * @return The mask and the unknown tokens.
     */
    [[nodiscard]] StateList parse_state_list(std::string_view list);

    /**
     * @brief Spells a situation mask as its list ("Dialogue, Combat"), for log lines; "none" for an empty mask.
     */
    [[nodiscard]] std::string state_list_text(std::uint32_t mask);

    /**
     * @brief The situations [Settings] HideIn lists: the fallback of every group that names none of its own.
     */
    [[nodiscard]] std::uint32_t default_hide_mask() noexcept;

} // namespace HenrySenses

#endif // HENRYSENSES_GAME_STATE_HPP
