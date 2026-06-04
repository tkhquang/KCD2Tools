/**
 * @file game_state.hpp
 * @brief Derives the current game state (combat, dialogue, minigame, mount, menu, overlay) from
 *        authoritative engine signals, for the INI-driven camera policy.
 *
 * Every signal is read from a discrete selector UPSTREAM of the camera pose smoother, so a state
 * edge is exact and does not lag like a smoothed value would. Combat, dialogue and minigame are
 * read from the active wh::game camera class (via RTTI on the camera manager's active-camera
 * pointer); mount from a per-actor flag on the player; menu and overlay from the existing UI hooks.
 */
#ifndef TPVCAMERA_GAME_STATE_HPP
#define TPVCAMERA_GAME_STATE_HPP

#include "constants.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace TPVCamera
{

/**
 * @enum GameState
 * @brief Bit flags for the game states the camera policy can react to.
 * @details Used as a uint32_t bit set (one set bit per active state). Minigames contribute an umbrella
 *          @ref Minigame bit (any minigame on screen) PLUS one mutually-exclusive child bit that
 *          identifies which minigame (see the Minigame* values and parse_state_mask).
 */
enum class GameState : uint32_t
{
    Menu = 1u << 0,     ///< In-game pause menu open.
    Overlay = 1u << 1,  ///< A blocking UI overlay is up (inventory, map, dialog screen, codex).
    Combat = 1u << 2,   ///< Combat camera active (weapon drawn; also set while aiming a missile weapon, which additionally sets @ref Aiming).
    Mount = 1u << 3,    ///< Player mounted on horseback (E_StanceCategory horse = 5).
    Dialogue = 1u << 4, ///< Dialogue camera active.
    Minigame = 1u << 5, ///< Any minigame on screen (umbrella; a Minigame* child below identifies which).
    Aiming = 1u << 6,   ///< Aiming or drawing a missile weapon (bow/crossbow).
    Crouch = 1u << 7,   ///< Player crouching / sneaking (E_StanceCategory crouch = 6).
    // Remaining E_StanceCategory values (the stance enum at C_ActorModel+0x80).
    // These are MUTUALLY EXCLUSIVE with Mount/Crouch and with each other (one stance field): standing(1)
    // and undefined(0) carry no bit (DEFAULT preset handles standing).
    Lying = 1u << 8,    ///< Lying down (E_StanceCategory lying = 2): sleeping in bed.
    Sitting = 1u << 9,  ///< Sitting (E_StanceCategory sitting = 3): bench / chair.
    Kneel = 1u << 10,   ///< Kneeling (E_StanceCategory kneel = 4).
    Cart = 1u << 11,    ///< Riding / driving a cart (E_StanceCategory cart = 7).
    // Per-minigame children. Each is set together with the umbrella @ref Minigame bit while that minigame
    // is on screen; the player is in at most one at a time. Read from the C_MinigameManager (NOT the
    // camera), so first-person minigames like lockpicking and reading are detected too (see
    // poll_active_minigame). The bit order mirrors the engine's E_MenuType factory.
    MinigameSharpening = 1u << 12,    ///< Sharpening a blade on a grindstone.
    MinigameReading = 1u << 13,       ///< Reading a book or scroll.
    MinigameAlchemy = 1u << 14,       ///< Brewing at an alchemy bench.
    MinigameHerbGathering = 1u << 15, ///< Picking a herb.
    MinigameLockpicking = 1u << 16,   ///< Picking a lock.
    MinigameHoleDigging = 1u << 17,   ///< Digging (treasure / grave).
    MinigameDice = 1u << 18,          ///< Dice (the one minigame that uses the dedicated minigame camera).
    MinigamePickpocketing = 1u << 19, ///< Pickpocketing an NPC.
    MinigameStoneThrowing = 1u << 20, ///< Throwing a stone.
    MinigameBattleArchery = 1u << 21, ///< Archery range / contest.
    MinigameDistract = 1u << 22,      ///< Distraction minigame.
    MinigameBlacksmithing = 1u << 23, ///< Forging at the anvil.
    MinigameForgeBuilder = 1u << 24,  ///< Operating the forge bellows.
};

/// Number of defined GameState bits; sizes the debounce timer array.
inline constexpr uint32_t k_game_state_bit_count = 25;

/// Returns the raw bit value of a GameState flag.
[[nodiscard]] constexpr uint32_t state_bit(GameState state) noexcept
{
    return static_cast<uint32_t>(state);
}

/**
 * @struct MinigameInfo
 * @brief One row of the minigame registry: the concrete RTTI name used to identify it live, its child
 *        GameState bit, the lowercase INI / bind_state token, and the overlay display label.
 * @details Single source of truth shared by the live detector (poll_active_minigame), the INI and
 *          bind-token parsers (parse_state_mask, parse_bind_mask, bind_mask_to_tokens) and the overlay
 *          bind editor, so the minigame vocabulary cannot drift between them.
 */
struct MinigameInfo
{
    const char *rtti_name;  ///< wh::playermodule::C_* RTTI type-descriptor name (see constants.hpp).
    GameState bit;          ///< Child bit set alongside the umbrella @ref GameState::Minigame.
    std::string_view token; ///< Lowercase INI / bind_state token.
    const char *label;      ///< Human-readable label for the overlay bind editor.
};

/// Every minigame, ordered by how commonly a user is likely to want it (the overlay lists it in order).
inline constexpr std::array<MinigameInfo, 13> k_minigames{{
    {Constants::C_MINIGAME_LOCKPICKING_RTTI_NAME, GameState::MinigameLockpicking, "lockpicking", "Lockpicking"},
    {Constants::C_MINIGAME_DICE_RTTI_NAME, GameState::MinigameDice, "dice", "Dice"},
    {Constants::C_MINIGAME_READING_RTTI_NAME, GameState::MinigameReading, "reading", "Reading"},
    {Constants::C_MINIGAME_ALCHEMY_RTTI_NAME, GameState::MinigameAlchemy, "alchemy", "Alchemy"},
    {Constants::C_MINIGAME_PICKPOCKETING_RTTI_NAME, GameState::MinigamePickpocketing, "pickpocketing", "Pickpocketing"},
    {Constants::C_MINIGAME_BLACKSMITHING_RTTI_NAME, GameState::MinigameBlacksmithing, "blacksmithing", "Blacksmithing"},
    {Constants::C_MINIGAME_FORGE_BUILDER_RTTI_NAME, GameState::MinigameForgeBuilder, "forgebuilder", "Forge bellows"},
    {Constants::C_MINIGAME_SHARPENING_RTTI_NAME, GameState::MinigameSharpening, "sharpening", "Sharpening"},
    {Constants::C_MINIGAME_HERB_GATHERING_RTTI_NAME, GameState::MinigameHerbGathering, "herbgathering", "Herb gathering"},
    {Constants::C_MINIGAME_HOLE_DIGGING_RTTI_NAME, GameState::MinigameHoleDigging, "holedigging", "Hole digging"},
    {Constants::C_MINIGAME_STONE_THROWING_RTTI_NAME, GameState::MinigameStoneThrowing, "stonethrowing", "Stone throwing"},
    {Constants::C_MINIGAME_BATTLE_ARCHERY_RTTI_NAME, GameState::MinigameBattleArchery, "battlearchery", "Archery"},
    {Constants::C_MINIGAME_DISTRACT_RTTI_NAME, GameState::MinigameDistract, "distract", "Distraction"},
}};

/**
 * @brief Parses a comma-separated state-token list into a GameState bit mask.
 * @details Tokens are case-insensitive and surrounding whitespace is ignored. Recognized tokens:
 *          Menu, Overlay, Combat, Mount, Dialogue, Aiming, Crouch (alias Stealth), Lying, Sitting,
 *          Kneel, Cart; Minigame (matches ANY minigame); and the per-minigame tokens Sharpening,
 *          Reading, Alchemy, HerbGathering, Lockpicking, HoleDigging, Dice, Pickpocketing,
 *          StoneThrowing, BattleArchery, Distract, Blacksmithing, ForgeBuilder. An empty list yields
 *          0; an unrecognized token is logged at WARNING level and skipped.
 * @param csv Comma-separated token list (e.g. "Combat,Dialogue,Minigame").
 * @return The OR of the recognized tokens' bits.
 */
[[nodiscard]] uint32_t parse_state_mask(std::string_view csv);

/**
 * @brief Reads the current game-state bit mask from the live engine signals.
 * @details Menu and overlay come from the UI hooks; combat and dialogue from the active wh::game
 *          camera class (resolved via the global-context to camera-manager chain and classified by
 *          RTTI, cached so the steady state is a single pointer compare); the minigame umbrella and its
 *          per-minigame child from the C_MinigameManager (so first-person minigames such as lockpicking
 *          are detected, not just the dice camera); mount and crouch/stealth from the player's
 *          C_ActorModel STANCE enum (mounted = 5, crouch = 6). Every
 *          engine read is SEH-guarded, so a failed read omits that bit rather than faulting. Intended
 *          to be called once per frame
 *          from the camera detour (the render thread); the camera classification cache is a plain
 *          static and is therefore not safe to call concurrently.
 * @param c_player Live C_Player address used for the mount read, or 0 to skip the mount bit.
 * @return The raw (un-debounced) GameState bit mask.
 */
[[nodiscard]] uint32_t poll_game_state(uintptr_t c_player) noexcept;

/**
 * @brief Applies per-bit hysteresis to a raw state mask so brief flicker does not pop the camera.
 * @details A bit must differ from the stable value for at least @p hold_seconds before it flips,
 *          so a momentary combat or dialogue transition cannot toggle a forced view on and off.
 *          Holds file-scope state and so must be called from a single thread (the render thread).
 * @param raw_mask Raw mask from poll_game_state().
 * @param delta_seconds Seconds elapsed since the previous call.
 * @param hold_seconds Dwell time a bit must hold its new value before it flips; <= 0 disables it.
 * @return The debounced (stable) mask.
 */
[[nodiscard]] uint32_t debounce_game_state(uint32_t raw_mask, float delta_seconds, float hold_seconds) noexcept;

} // namespace TPVCamera

#endif // TPVCAMERA_GAME_STATE_HPP
