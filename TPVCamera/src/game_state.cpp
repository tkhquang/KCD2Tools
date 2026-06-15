/**
 * @file game_state.cpp
 * @brief Live game-state detection backing the INI-driven camera policy.
 *
 * Combat, dialogue and minigame are read from the active wh::game camera class: the camera manager
 * stores a pointer to the currently active camera, and that object's RTTI type name identifies the
 * mode. The classification is cached on the active-camera vtable so the steady state costs a single
 * pointer compare. Mount is a per-actor flag; menu and overlay come from the UI hooks.
 */

#include "game_state.hpp"
#include "constants.hpp"
#include "global_state.hpp"
#include "hooks/ui_menu_hooks.hpp"

#include <DetourModKit.hpp>

#include <array>
#include <cctype>
#include <string>

namespace TPVCamera
{

namespace
{

/**
 * @brief Classifies an active-camera vtable into the combat / dialogue state bits.
 * @details Caches the last vtable and its classification so the steady state (the active camera
 *          unchanged frame to frame) costs one pointer compare with no RTTI walk; a camera switch
 *          re-walks once. Render-thread only, so the cache is a plain static. Minigames are NOT read
 *          from the camera (only dice swaps the active camera to C_CameraMinigame; lockpicking, reading
 *          and the rest stay first-person), so they are detected separately in poll_active_minigame.
 * @param vtable Runtime vtable pointer of the active camera object.
 * @return The matching GameState bit, or 0 when the active camera is none of the tracked modes.
 */
[[nodiscard]] uint32_t classify_camera_vtable(uintptr_t vtable) noexcept
{
    static uintptr_t s_last_vtable = 0;
    static uint32_t s_last_bits = 0;
    if (vtable == s_last_vtable)
    {
        return s_last_bits;
    }

    uint32_t bits = 0;
    if (DMK::Rtti::vtable_is_type(vtable, Constants::C_CAMERA_COMBAT_RTTI_NAME))
    {
        bits = state_bit(GameState::Combat);
    }
    else if (DMK::Rtti::vtable_is_type(vtable, Constants::C_CAMERA_DIALOG_RTTI_NAME))
    {
        bits = state_bit(GameState::Dialogue);
    }

    s_last_vtable = vtable;
    s_last_bits = bits;
    return bits;
}

/**
 * @brief Resolves the active camera and classifies it, returning 0 on any failed read.
 * @details Walks g_global_context -> camera manager (OFFSET_MANAGER_PTR_STORAGE) -> active camera
 *          (OFFSET_ACTIVE_CAMERA) -> vtable, each link SEH-guarded and screened as a plausible
 *          user-space pointer. The manager is the same wh::game::C_CameraManager the built-in TPV
 *          flag is read from, so the chain reuses the already-resolved context pointer storage.
 */
[[nodiscard]] uint32_t poll_active_camera_state() noexcept
{
    const auto context_slot = g_global_context_ptr_address.load(std::memory_order_relaxed);
    if (!context_slot)
    {
        return 0;
    }
    // One guarded walk: g_global_context -> camera manager (OFFSET_MANAGER_PTR_STORAGE) -> active camera
    // (OFFSET_ACTIVE_CAMERA) -> vtable. seh_read_chain screens every intermediate link with
    // plausible_userspace_ptr under a single fault guard; the terminal vtable value it returns is not
    // range-checked by the chain, so it is screened here to match the previous per-hop walk.
    const auto vtable = DMK::Memory::seh_read_chain<uintptr_t>(
        reinterpret_cast<uintptr_t>(context_slot),
        {0, Constants::OFFSET_MANAGER_PTR_STORAGE, Constants::OFFSET_ACTIVE_CAMERA, 0});
    if (!vtable || !DMK::Memory::plausible_userspace_ptr(*vtable))
    {
        return 0;
    }
    return classify_camera_vtable(*vtable);
}

/**
 * @brief Classifies an active-minigame vtable into its child GameState bit (0 when unrecognized).
 * @details Mirrors classify_camera_vtable: caches the last vtable so the steady state inside a minigame
 *          is one pointer compare with no RTTI walk. Render-thread only, so the cache is a plain static.
 * @param vtable Runtime vtable pointer of the active wh::playermodule::C_Minigame subclass.
 */
[[nodiscard]] uint32_t classify_minigame_vtable(uintptr_t vtable) noexcept
{
    static uintptr_t s_last_vtable = 0;
    static uint32_t s_last_bit = 0;
    if (vtable == s_last_vtable)
    {
        return s_last_bit;
    }

    uint32_t bit = 0;
    for (const MinigameInfo &def : k_minigames)
    {
        if (DMK::Rtti::vtable_is_type(vtable, def.rtti_name))
        {
            bit = state_bit(def.bit);
            break;
        }
    }

    s_last_vtable = vtable;
    s_last_bit = bit;
    return bit;
}

/**
 * @brief Detects whether the player is in a minigame and which one, via the C_MinigameManager.
 * @details The minigame state is NOT readable from the active camera (only dice swaps the camera; see
 *          classify_camera_vtable), so it is read from the manager that owns every active minigame. The
 *          chain is reached from the same global context the camera manager hangs off:
 *          context -> minigame subsystem -> C_MinigameManager -> a circular intrusive list of active
 *          minigames (an empty list links its sentinel head to itself). Each node holds the I_Minigame*,
 *          and the minigame's owner is the C_Human/C_Player it belongs to. The list is walked (bounded)
 *          for the entry the player owns; in single player the only entries are the player's, so a
 *          c_player-owner mismatch (or c_player == 0) falls back to the first entry. Every read is
 *          SEH-guarded, so a failed read reports "no minigame" rather than faulting.
 * @param c_player Live C_Player address used to confirm ownership, or 0 to accept the first entry.
 * @return state_bit(Minigame) | the matching child bit when in a minigame, else 0.
 */
[[nodiscard]] uint32_t poll_active_minigame(uintptr_t c_player) noexcept
{
    const auto context_slot = g_global_context_ptr_address.load(std::memory_order_relaxed);
    if (!context_slot)
    {
        return 0;
    }
    // Walk g_global_context -> minigame subsystem -> manager -> circular-list sentinel head under one
    // fault guard (each intermediate link screened by plausible_userspace_ptr). The head value the chain
    // returns is screened here (the chain does not range-check the terminal read). An empty map links the
    // head's next back to the head itself, so the begin read below is deliberately NOT plausibility-screened.
    const auto head = DMK::Memory::seh_read_chain<uintptr_t>(
        reinterpret_cast<uintptr_t>(context_slot),
        {0, Constants::OFFSET_MINIGAME_SUBSYSTEM, Constants::OFFSET_MINIGAME_MANAGER,
         Constants::OFFSET_MINIGAME_MAP_HEAD});
    if (!head || !DMK::Memory::plausible_userspace_ptr(*head))
    {
        return 0;
    }
    const auto begin = DMK::Memory::seh_read<uintptr_t>(*head + Constants::OFFSET_MINIGAME_NODE_NEXT);
    if (!begin)
    {
        return 0;
    }

    // Bounded walk so a corrupt list cannot spin. Prefer the player-owned entry; remember the first valid
    // minigame as the single-player fallback.
    constexpr int k_max_nodes = 16;
    uintptr_t node = *begin;
    uintptr_t fallback_vtable = 0;
    for (int i = 0; i < k_max_nodes && node != *head && DMK::Memory::plausible_userspace_ptr(node); ++i)
    {
        const auto minigame = DMK::Memory::seh_read<uintptr_t>(node + Constants::OFFSET_MINIGAME_NODE_VALUE);
        if (minigame && DMK::Memory::plausible_userspace_ptr(*minigame))
        {
            const auto vtable = DMK::Memory::seh_read<uintptr_t>(*minigame);
            if (vtable && DMK::Memory::plausible_userspace_ptr(*vtable))
            {
                if (c_player != 0)
                {
                    const auto owner = DMK::Memory::seh_read<uintptr_t>(*minigame + Constants::OFFSET_MINIGAME_OWNER);
                    if (owner && *owner == c_player)
                    {
                        return state_bit(GameState::Minigame) | classify_minigame_vtable(*vtable);
                    }
                }
                // The first-entry fallback is only for the pre-resolve window (c_player == 0). When a
                // live c_player was supplied but no entry's owner matched it, the player is NOT in a
                // minigame, so a non-player-owned entry must not be reported via the fallback.
                if (c_player == 0 && fallback_vtable == 0)
                {
                    fallback_vtable = *vtable;
                }
            }
        }
        const auto next = DMK::Memory::seh_read<uintptr_t>(node + Constants::OFFSET_MINIGAME_NODE_NEXT);
        if (!next)
        {
            break;
        }
        node = *next;
    }

    if (fallback_vtable != 0)
    {
        return state_bit(GameState::Minigame) | classify_minigame_vtable(fallback_vtable);
    }
    return 0;
}

/**
 * @brief Reads the player's missile-weapon aim flag, validated by RTTI, or false on any failure.
 * @details The C_MissileWeaponPlayerController is embedded in C_Player at a fixed offset (it is
 *          constructed inside the C_Player constructor and is player-only, being the input
 *          action-map listener), so it is reached by adding the offset, not dereferencing a pointer.
 *          The embedded object's vtable is validated against the controller RTTI name on the first
 *          read and cached, so the steady state is one pointer compare plus the flag read; a layout
 *          drift (a wrong vtable at the offset) yields false rather than a garbage read.
 */
[[nodiscard]] bool poll_missile_aiming(uintptr_t c_player) noexcept
{
    const auto vtable = DMK::Memory::seh_read<uintptr_t>(c_player + Constants::C_PLAYER_MISSILE_CONTROLLER_OFFSET);
    if (!vtable || !DMK::Memory::plausible_userspace_ptr(*vtable))
    {
        return false;
    }
    static uintptr_t s_controller_vtable = 0;
    if (s_controller_vtable == 0)
    {
        if (!DMK::Rtti::vtable_is_type(*vtable, Constants::C_MISSILE_CONTROLLER_RTTI_NAME))
        {
            return false;
        }
        s_controller_vtable = *vtable;
    }
    else if (*vtable != s_controller_vtable)
    {
        return false;
    }
    // The aim flag is a single BYTE: the surrounding bytes pack a separate "weapon in hand" flag,
    // so reading a dword would also fire when the weapon is merely drawn (in hand not aiming = 0,
    // raised/aiming = 1).
    const auto aim_flag = DMK::Memory::seh_read<uint8_t>(
        c_player + Constants::C_PLAYER_MISSILE_CONTROLLER_OFFSET + Constants::MISSILE_CONTROLLER_AIM_FLAG_OFFSET);
    return aim_flag && *aim_flag != 0;
}

/**
 * @brief Reads the player's current STANCE enum, validated by RTTI, or 0 on any failure.
 * @details C_ActorModel is a POINTER on C_Player (C_PLAYER_ACTOR_MODEL_OFFSET), dereferenced and its
 *          vtable validated against the C_ActorModel RTTI name (cached after the first read, so the
 *          steady state is one pointer compare plus the stance read). A layout drift (a wrong vtable)
 *          yields 0 rather than a garbage read. The 4-byte current-stance enum at
 *          C_ACTOR_MODEL_STANCE_OFFSET is the SINGLE source for both crouch and mount:
 *          1 = standing, 5 = mounted (riding), 6 = crouching/sneaking. (A transient 5 also appears for
 *          one frame during a stand<->crouch switch; the GameState debounce filters that blip, so a held
 *          5 reliably means mounted.) The stance is preferred over two nearby per-flag candidates that
 *          look usable but are not: +0x8 is a 64-bit pointer (not a crouch flag, so its low dword reads
 *          false), and +0x174 is a CONTROL-OVERRIDE REFCOUNT (== 1 for ANY single control state --
 *          mounting OR an item pickup OR a scripted interaction), so reading it would false-trigger the
 *          MOUNT preset on pickups; the stance is immune (a pickup keeps stance == 1).
 * @return The stance enum value, or 0 if the actor model / RTTI / read failed.
 */
[[nodiscard]] uint32_t poll_stance(uintptr_t c_player) noexcept
{
    const auto actor_model = DMK::Memory::seh_read<uintptr_t>(c_player + Constants::C_PLAYER_ACTOR_MODEL_OFFSET);
    if (!actor_model || !DMK::Memory::plausible_userspace_ptr(*actor_model))
    {
        return 0u;
    }
    const auto vtable = DMK::Memory::seh_read<uintptr_t>(*actor_model);
    if (!vtable || !DMK::Memory::plausible_userspace_ptr(*vtable))
    {
        return 0u;
    }
    static uintptr_t s_actor_model_vtable = 0;
    if (s_actor_model_vtable == 0)
    {
        if (!DMK::Rtti::vtable_is_type(*vtable, Constants::C_ACTOR_MODEL_RTTI_NAME))
        {
            return 0u;
        }
        s_actor_model_vtable = *vtable;
    }
    else if (*vtable != s_actor_model_vtable)
    {
        return 0u;
    }
    const auto stance = DMK::Memory::seh_read<uint32_t>(
        *actor_model + Constants::C_ACTOR_MODEL_STANCE_OFFSET);
    return stance ? *stance : 0u;
}

/// Trims leading and trailing ASCII whitespace from a view (no allocation).
[[nodiscard]] std::string_view trim_view(std::string_view text) noexcept
{
    constexpr std::string_view whitespace = " \t\r\n";
    const size_t begin = text.find_first_not_of(whitespace);
    if (begin == std::string_view::npos)
    {
        return {};
    }
    const size_t end = text.find_last_not_of(whitespace);
    return text.substr(begin, end - begin + 1);
}

/// ASCII case-insensitive equality of a token against a lowercase literal.
[[nodiscard]] bool token_equals(std::string_view token, std::string_view lower_literal) noexcept
{
    if (token.size() != lower_literal.size())
    {
        return false;
    }
    for (size_t i = 0; i < token.size(); ++i)
    {
        if (static_cast<char>(std::tolower(static_cast<unsigned char>(token[i]))) != lower_literal[i])
        {
            return false;
        }
    }
    return true;
}

/// Maps a single trimmed token to its GameState bit, or 0 when unrecognized.
[[nodiscard]] uint32_t token_to_bit(std::string_view token) noexcept
{
    if (token_equals(token, "menu"))
    {
        return state_bit(GameState::Menu);
    }
    if (token_equals(token, "overlay"))
    {
        return state_bit(GameState::Overlay);
    }
    if (token_equals(token, "combat"))
    {
        return state_bit(GameState::Combat);
    }
    if (token_equals(token, "mount"))
    {
        return state_bit(GameState::Mount);
    }
    if (token_equals(token, "dialogue"))
    {
        return state_bit(GameState::Dialogue);
    }
    if (token_equals(token, "minigame"))
    {
        return state_bit(GameState::Minigame);
    }
    if (token_equals(token, "aiming"))
    {
        return state_bit(GameState::Aiming);
    }
    // Crouch and Stealth are aliases: KCD2 crouch IS the sneak/stealth stance.
    if (token_equals(token, "crouch") || token_equals(token, "stealth"))
    {
        return state_bit(GameState::Crouch);
    }
    // Remaining E_StanceCategory stances (see poll_stance): lying / sitting / kneel / cart.
    if (token_equals(token, "lying"))
    {
        return state_bit(GameState::Lying);
    }
    if (token_equals(token, "sitting"))
    {
        return state_bit(GameState::Sitting);
    }
    if (token_equals(token, "kneel"))
    {
        return state_bit(GameState::Kneel);
    }
    if (token_equals(token, "cart"))
    {
        return state_bit(GameState::Cart);
    }
    // Per-minigame child tokens (lockpicking, dice, reading, ...). Each resolves to its child bit; the
    // umbrella "minigame" token above matches ANY minigame. poll_active_minigame sets both bits, so a
    // child token reacts only to that minigame while "minigame" reacts to all of them.
    for (const MinigameInfo &def : k_minigames)
    {
        if (token_equals(token, def.token))
        {
            return state_bit(def.bit);
        }
    }
    return 0;
}

} // namespace

uint32_t parse_state_mask(std::string_view csv)
{
    DMK::Logger &logger = DMK::Logger::get_instance();
    uint32_t mask = 0;

    size_t start = 0;
    while (start <= csv.size())
    {
        const size_t comma = csv.find(',', start);
        const size_t end = (comma == std::string_view::npos) ? csv.size() : comma;
        const std::string_view token = trim_view(csv.substr(start, end - start));
        if (!token.empty())
        {
            const uint32_t bit = token_to_bit(token);
            if (bit != 0)
            {
                mask |= bit;
            }
            else
            {
                logger.warning("GameState: ignoring unknown state token '{}'", std::string(token));
            }
        }
        if (comma == std::string_view::npos)
        {
            break;
        }
        start = comma + 1;
    }
    return mask;
}

uint32_t poll_game_state(uintptr_t c_player) noexcept
{
    uint32_t mask = 0;

    if (is_game_menu_open())
    {
        mask |= state_bit(GameState::Menu);
    }
    if (overlay_state().active.load(std::memory_order_relaxed))
    {
        mask |= state_bit(GameState::Overlay);
    }

    mask |= poll_active_camera_state();
    // Minigames (the umbrella Minigame bit plus the specific child) come from the C_MinigameManager, not
    // the camera, so a first-person minigame such as lockpicking is detected. c_player confirms ownership;
    // it also works via the first-entry fallback before the player resolves.
    mask |= poll_active_minigame(c_player);

    if (c_player != 0)
    {
        // Aiming a missile weapon: the embedded missile-weapon controller's aim flag.
        if (poll_missile_aiming(c_player))
        {
            mask |= state_bit(GameState::Aiming);
        }
        // Every body-posture state comes from the player's current STANCE enum (wh::entitymodule::
        // E_StanceCategory at C_ActorModel+0x80, read once): undefined=0, standing=1, lying=2, sitting=3,
        // kneel=4, horse(mount)=5, crouch=6, cart=7. Standing /
        // undefined carry no bit (DEFAULT preset). The active camera stays first-person for these, so the
        // camera-state selector cannot see them; the stance can. The stance is immune to the +0x174
        // control-override REFCOUNT false-trigger (a pickup keeps stance == 1); 1-frame transients during a
        // stance switch are filtered by the GameState debounce.
        switch (poll_stance(c_player))
        {
        case Constants::C_ACTOR_MODEL_STANCE_LYING:
            mask |= state_bit(GameState::Lying);
            break;
        case Constants::C_ACTOR_MODEL_STANCE_SITTING:
            mask |= state_bit(GameState::Sitting);
            break;
        case Constants::C_ACTOR_MODEL_STANCE_KNEEL:
            mask |= state_bit(GameState::Kneel);
            break;
        case Constants::C_ACTOR_MODEL_STANCE_MOUNT:
            mask |= state_bit(GameState::Mount);
            break;
        case Constants::C_ACTOR_MODEL_STANCE_CROUCH:
            mask |= state_bit(GameState::Crouch);
            break;
        case Constants::C_ACTOR_MODEL_STANCE_CART:
            mask |= state_bit(GameState::Cart);
            break;
        default:
            break; // standing / undefined: no stance bit
        }
    }

    return mask;
}

uint32_t debounce_game_state(uint32_t raw_mask, float delta_seconds, float hold_seconds) noexcept
{
    static uint32_t s_stable_mask = 0;
    static std::array<float, k_game_state_bit_count> s_bit_timer{};

    if (hold_seconds <= 0.0f)
    {
        // Debounce disabled (hot-reloadable): pass through and clear the per-bit dwell so a later
        // re-enable does not flip a bit early off a stale, partially-accumulated timer.
        s_stable_mask = raw_mask;
        s_bit_timer.fill(0.0f);
        return raw_mask;
    }

    for (uint32_t i = 0; i < k_game_state_bit_count; ++i)
    {
        const uint32_t bit = 1u << i;
        const bool raw_on = (raw_mask & bit) != 0;
        const bool stable_on = (s_stable_mask & bit) != 0;
        if (raw_on == stable_on)
        {
            // Bit already matches the stable value: reset its dwell timer so a transient blip that
            // clears before the hold elapses never flips the stable mask.
            s_bit_timer[i] = 0.0f;
        }
        else
        {
            s_bit_timer[i] += delta_seconds;
            if (s_bit_timer[i] >= hold_seconds)
            {
                s_stable_mask ^= bit;
                s_bit_timer[i] = 0.0f;
            }
        }
    }
    return s_stable_mask;
}

} // namespace TPVCamera
