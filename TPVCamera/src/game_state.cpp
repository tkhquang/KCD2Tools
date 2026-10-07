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
#include "config.hpp"
#include "rtti_types.hpp"
#include "constants.hpp"
#include "global_state.hpp"
#include "offset_heal.hpp"
#include "hooks/ui_menu_hooks.hpp"

#include <DetourModKit.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <initializer_list>
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
            // The memo short-circuits the NEGATIVE case, an active camera that is neither tracked class, in
            // one compare. vtable_is() answers the positive cases from a cached identity, so a camera SWITCH
            // costs a pointer compare rather than an RTTI walk.
            static uintptr_t s_last_vtable = 0;
            static uint32_t s_last_bits = 0;
            if (vtable == s_last_vtable)
            {
                return s_last_bits;
            }

            uint32_t bits = 0;
            if (vtable_is(GameClass::CameraCombat, vtable))
            {
                bits = state_bit(GameState::Combat);
            }
            else if (vtable_is(GameClass::CameraDialog, vtable))
            {
                bits = state_bit(GameState::Dialogue);
            }

            s_last_vtable = vtable;
            s_last_bits = bits;
            return bits;
        }

        /**
         * @brief Resolves the active camera and classifies it, returning 0 on any failed read.
         * @details Dereferences the global-context slot to the context object first (so the self-heal can scan the
         *          anchored base), then once the world is live heals the context member offsets one-shot, then walks
         *          context -> camera manager (self-healed OFFSET_MANAGER_PTR_STORAGE) -> active camera
         *          (OFFSET_ACTIVE_CAMERA) -> vtable, each link SEH-guarded and screened as a plausible user-space
         *          pointer. The manager is the same wh::game::C_CameraManager the built-in TPV flag is read from.
         */
        [[nodiscard]] uint32_t poll_active_camera_state() noexcept
        {
            const auto context_slot = g_global_context_ptr_address.load(std::memory_order_relaxed);
            if (!context_slot)
            {
                return 0;
            }
            // Resolve the context object first so the self-heal can run against it. Gating the heal on the world
            // being live (rather than on the manager slot being populated) keeps a camera-manager OFFSET drift
            // recoverable: the heal scans the anchored context base, never navigating through the offset it heals.
            const auto context = DMK::memory::read<uintptr_t>(DMK::Address{reinterpret_cast<uintptr_t>(context_slot)});
            if (!context || !DMK::memory::is_plausible_ptr(DMK::Address{*context}))
            {
                return 0;
            }
            if (game_world_ready().load(std::memory_order_relaxed))
            {
                note_context_base(*context);
            }
            // One guarded walk: context object -> camera manager (self-healed OFFSET_MANAGER_PTR_STORAGE) -> active
            // camera (OFFSET_ACTIVE_CAMERA). memory::walk screens every dereferenced link under a single fault
            // guard and hands back the leaf ADDRESS; the vtable value read from it is not range-checked by the
            // walk, so it is screened here before use.
            const std::array<std::ptrdiff_t, 3> camera_chain{runtime_offsets().context_manager.load().value,
                                                             Constants::OFFSET_ACTIVE_CAMERA, 0};
            const auto camera = DMK::memory::walk(DMK::Address{*context}, camera_chain);
            if (!camera)
            {
                return 0;
            }
            const auto vtable = DMK::memory::read<uintptr_t>(*camera);
            if (!vtable || !DMK::memory::is_plausible_ptr(DMK::Address{*vtable}))
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
            for (std::size_t i = 0; i < k_minigames.size(); ++i)
            {
                if (minigame_vtable_is(i, vtable))
                {
                    bit = state_bit(k_minigames[i].bit);
                    break;
                }
            }

            s_last_vtable = vtable;
            s_last_bit = bit;
            return bit;
        }

        /** @brief The player's active minigame: the I_Minigame object and its vtable, or zeros when there is none. */
        struct ActiveMinigame
        {
            uintptr_t object{0};
            uintptr_t vtable{0};
        };

        /**
         * @brief Finds the minigame the player is in, via the C_MinigameManager.
         * @details The minigame state is NOT readable from the active camera (only dice swaps the camera; see
         *          classify_camera_vtable), so it is read from the manager that owns every active minigame. The
         *          chain is reached from the same global context the camera manager hangs off:
         *          context -> minigame subsystem -> C_MinigameManager -> a circular intrusive list of active
         *          minigames (an empty list links its sentinel head to itself). Each node holds the I_Minigame*,
         *          and the minigame's owner is the C_Human/C_Player it belongs to. The list is walked (bounded)
         *          for the entry the player owns. It falls back to the first entry only when c_player == 0; with a
         *          live c_player and no owner match, the player is in no minigame. Every read is SEH-guarded, so a
         *          failed read reports "no minigame" rather than faulting.
         * @param c_player Live C_Player address used to confirm ownership, or 0 to accept the first entry.
         * @return The player's minigame, or zeros when the player is in none.
         */
        [[nodiscard]] ActiveMinigame find_player_minigame(uintptr_t c_player) noexcept
        {
            const auto context_slot = g_global_context_ptr_address.load(std::memory_order_relaxed);
            if (!context_slot)
            {
                return {};
            }
            // Walk g_global_context -> minigame subsystem -> manager -> circular-list sentinel head under one
            // fault guard (each dereferenced link screened by the walk's plausibility floor). memory::walk hands
            // back the leaf ADDRESS; the head value read from it is screened here, because the walk does not
            // range-check a value it never dereferences. An empty map links the head's next back to the head
            // itself, so the begin read below is deliberately NOT plausibility-screened.
            const std::array<std::ptrdiff_t, 4> minigame_chain{
                0, runtime_offsets().context_minigame_subsystem.load().value, Constants::OFFSET_MINIGAME_MANAGER,
                Constants::OFFSET_MINIGAME_MAP_HEAD};
            const auto head_slot =
                DMK::memory::walk(DMK::Address{reinterpret_cast<uintptr_t>(context_slot)}, minigame_chain);
            if (!head_slot)
            {
                return {};
            }
            const auto head = DMK::memory::read<uintptr_t>(*head_slot);
            if (!head || !DMK::memory::is_plausible_ptr(DMK::Address{*head}))
            {
                return {};
            }
            const auto begin = DMK::memory::read<uintptr_t>(DMK::Address{*head + Constants::OFFSET_MINIGAME_NODE_NEXT});
            if (!begin)
            {
                return {};
            }

            // Bounded walk so a corrupt list cannot spin. Prefer the player-owned entry; remember the first valid
            // minigame as the single-player fallback.
            constexpr int k_max_nodes = 16;
            uintptr_t node = *begin;
            ActiveMinigame fallback{};
            for (int i = 0; i < k_max_nodes && node != *head && DMK::memory::is_plausible_ptr(DMK::Address{node}); ++i)
            {
                const auto minigame =
                    DMK::memory::read<uintptr_t>(DMK::Address{node + Constants::OFFSET_MINIGAME_NODE_VALUE});
                if (minigame && DMK::memory::is_plausible_ptr(DMK::Address{*minigame}))
                {
                    const auto vtable = DMK::memory::read<uintptr_t>(DMK::Address{*minigame});
                    if (vtable && DMK::memory::is_plausible_ptr(DMK::Address{*vtable}))
                    {
                        if (c_player != 0)
                        {
                            const auto owner = DMK::memory::read<uintptr_t>(
                                DMK::Address{*minigame + Constants::OFFSET_MINIGAME_OWNER});
                            if (owner && *owner == c_player)
                            {
                                return ActiveMinigame{*minigame, *vtable};
                            }
                        }
                        // The first-entry fallback is only for the pre-resolve window (c_player == 0). When a
                        // live c_player was supplied but no entry's owner matched it, the player is NOT in a
                        // minigame, so a non-player-owned entry must not be reported via the fallback.
                        if (c_player == 0 && fallback.vtable == 0)
                        {
                            fallback = ActiveMinigame{*minigame, *vtable};
                        }
                    }
                }
                const auto next =
                    DMK::memory::read<uintptr_t>(DMK::Address{node + Constants::OFFSET_MINIGAME_NODE_NEXT});
                if (!next)
                {
                    break;
                }
                node = *next;
            }

            return fallback;
        }

        /** @brief The minigame bits of the player's minigame: the umbrella Minigame bit and its child, or 0. */
        [[nodiscard]] uint32_t poll_active_minigame(uintptr_t c_player) noexcept
        {
            const ActiveMinigame minigame = find_player_minigame(c_player);
            return minigame.vtable != 0 ? state_bit(GameState::Minigame) | classify_minigame_vtable(minigame.vtable)
                                        : 0;
        }

        /**
         * @brief Finds the player's C_ActorShootingExpansion in the C_ActionActor component map, or 0.
         * @details The map is a handful of nodes, walked with a bounded stack rather than by its key, so the lookup
         *          does not depend on the component's type id; the match is the value whose vtable is the shooting
         *          expansion's.
         */
        [[nodiscard]] uintptr_t find_shooting_expansion(uintptr_t c_player) noexcept
        {
            // C_Player -> C_ActionActor -> the map's head (sentinel) node; walk screens each hop.
            const std::array<std::ptrdiff_t, 3> head_chain{
                Constants::C_PLAYER_ACTION_ACTOR_OFFSET,
                Constants::ACTION_ACTOR_COMPONENT_MAP_OFFSET,
                0,
            };
            const auto head_slot = DMK::memory::walk(DMK::Address{c_player}, head_chain);
            if (!head_slot || !DMK::memory::is_plausible_ptr(*head_slot))
            {
                return 0;
            }
            const uintptr_t head = head_slot->raw();
            const auto root = DMK::memory::read<uintptr_t>(DMK::Address{head + Constants::MSVC_MAP_NODE_PARENT_OFFSET});
            if (!root)
            {
                return 0;
            }
            // The component map holds a handful of nodes: these bound the walk and its stack on a corrupt tree.
            constexpr int k_max_nodes = 64;
            constexpr std::size_t k_max_stack = 32;
            std::array<uintptr_t, k_max_stack> stack{};
            std::size_t top = 0;
            stack[top++] = *root;
            for (int visited = 0; top > 0 && visited < k_max_nodes; ++visited)
            {
                const uintptr_t node = stack[--top];
                const auto is_nil =
                    DMK::memory::read<uint8_t>(DMK::Address{node + Constants::MSVC_MAP_NODE_ISNIL_OFFSET});
                if (!is_nil || *is_nil != 0)
                {
                    continue;
                }
                const auto value =
                    DMK::memory::read<uintptr_t>(DMK::Address{node + Constants::MSVC_MAP_NODE_POINTER_VALUE_OFFSET});
                if (value && DMK::memory::is_plausible_ptr(DMK::Address{*value}))
                {
                    const auto vtable = DMK::memory::read<uintptr_t>(DMK::Address{*value});
                    if (vtable && vtable_is(GameClass::ShootingExpansion, *vtable))
                    {
                        return *value;
                    }
                }
                for (const std::ptrdiff_t link :
                     {Constants::MSVC_MAP_NODE_LEFT_OFFSET, Constants::MSVC_MAP_NODE_RIGHT_OFFSET})
                {
                    const auto child = DMK::memory::read<uintptr_t>(DMK::Address{node + link});
                    if (child && *child != 0 && *child != head && top < stack.size())
                    {
                        stack[top++] = *child;
                    }
                }
            }
            return 0;
        }

        /**
         * @brief The C_Item the player's shooting expansion holds at @p offset (the weapon or the ready ammo), or 0.
         * @details The expansion is found from the live player each call: the map walk is a handful of reads, and no
         *          pointer outlives the player that holds it.
         */
        [[nodiscard]] uintptr_t shooting_item(uintptr_t c_player, std::ptrdiff_t offset) noexcept
        {
            const uintptr_t expansion = c_player != 0 ? find_shooting_expansion(c_player) : 0;
            if (expansion == 0)
            {
                return 0;
            }
            const auto item = DMK::memory::read<uintptr_t>(DMK::Address{expansion + offset});
            return item && DMK::memory::is_plausible_ptr(DMK::Address{*item}) ? *item : 0;
        }

        /** @brief The vtable of the player's running shot's current phase, or 0 when no shot runs. */
        [[nodiscard]] uintptr_t shooting_phase_vtable(uintptr_t c_player) noexcept
        {
            const uintptr_t expansion = find_shooting_expansion(c_player);
            if (expansion == 0)
            {
                return 0;
            }
            const auto main =
                DMK::memory::read<uintptr_t>(DMK::Address{expansion + Constants::SHOOTING_EXPANSION_MAIN_OFFSET});
            if (!main || *main == 0 || !DMK::memory::is_plausible_ptr(DMK::Address{*main}))
            {
                return 0;
            }
            const auto main_vtable = DMK::memory::read<uintptr_t>(DMK::Address{*main});
            if (!main_vtable || !vtable_is(GameClass::ShootingMain, *main_vtable))
            {
                return 0;
            }
            const auto sub =
                DMK::memory::read<uintptr_t>(DMK::Address{*main + Constants::SHOOTING_MAIN_SUBACTION_OFFSET});
            if (!sub || *sub == 0 || !DMK::memory::is_plausible_ptr(DMK::Address{*sub}))
            {
                return 0;
            }
            const auto sub_vtable = DMK::memory::read<uintptr_t>(DMK::Address{*sub});
            return sub_vtable ? *sub_vtable : 0;
        }

        /**
         * @brief True while the player's running shot is in its reload or unload phase (a bolt going in or out).
         * @details Any miss on the chain (no shot running, a drifted layout, an unexpected class) answers false, which
         *          keeps the game's own aim flag as the Aiming state.
         */
        [[nodiscard]] bool player_reloading_missile(uintptr_t c_player) noexcept
        {
            const uintptr_t phase = shooting_phase_vtable(c_player);
            return phase != 0 &&
                   (vtable_is(GameClass::ShootingReloading, phase) || vtable_is(GameClass::ShootingUnloading, phase));
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
            const std::ptrdiff_t missile_offset = runtime_offsets().c_player_missile_controller.load().value;
            const auto vtable = DMK::memory::read<uintptr_t>(DMK::Address{c_player + missile_offset});
            if (!vtable || !DMK::memory::is_plausible_ptr(DMK::Address{*vtable}))
            {
                return false;
            }
            static uintptr_t s_controller_vtable = 0;
            if (s_controller_vtable == 0)
            {
                if (!vtable_is(GameClass::MissileController, *vtable))
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
            const auto aim_flag = DMK::memory::read<uint8_t>(
                DMK::Address{c_player + missile_offset + Constants::MISSILE_CONTROLLER_AIM_FLAG_OFFSET});
            const bool aim_requested = aim_flag && *aim_flag != 0;
            // The flag goes up the moment a crossbow's prepare is pressed and stays up through the whole reload, so
            // unless ReloadCountsAsAiming is on, Aiming waits until the bolt is in.
            const bool held_off = aim_requested &&
                                  !settings().reload_counts_as_aiming.load(std::memory_order_relaxed) &&
                                  player_reloading_missile(c_player);
            static bool s_was_held_off = false;
            if (held_off != s_was_held_off)
            {
                s_was_held_off = held_off;
                (void)DMK::log().try_log(DMK::LogLevel::Debug, "GameState: {}",
                                         held_off ? "crossbow reload started, Aiming waits for the bolt"
                                                  : (aim_requested ? "bolt in, Aiming" : "reload left unfinished"));
            }
            return aim_requested && !held_off;
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
         *          false), and +0x174 is a CONTROL-OVERRIDE REFCOUNT (== 1 for ANY single control state -
         *          mounting OR an item pickup OR a scripted interaction), so reading it would false-trigger the
         *          MOUNT preset on pickups; the stance is immune (a pickup keeps stance == 1).
         * @return The stance enum value, or 0 if the actor model / RTTI / read failed.
         */
        [[nodiscard]] uint32_t poll_stance(uintptr_t c_player) noexcept
        {
            const std::ptrdiff_t c_player_actor_model_offset = runtime_offsets().c_player_actor_model.load().value;
            const auto actor_model = DMK::memory::read<uintptr_t>(DMK::Address{c_player + c_player_actor_model_offset});
            if (!actor_model || !DMK::memory::is_plausible_ptr(DMK::Address{*actor_model}))
            {
                return 0u;
            }
            const auto vtable = DMK::memory::read<uintptr_t>(DMK::Address{*actor_model});
            if (!vtable || !DMK::memory::is_plausible_ptr(DMK::Address{*vtable}))
            {
                return 0u;
            }
            static uintptr_t s_actor_model_vtable = 0;
            if (s_actor_model_vtable == 0)
            {
                if (!vtable_is(GameClass::ActorModel, *vtable))
                {
                    return 0u;
                }
                s_actor_model_vtable = *vtable;
            }
            else if (*vtable != s_actor_model_vtable)
            {
                return 0u;
            }
            const auto stance =
                DMK::memory::read<uint32_t>(DMK::Address{*actor_model + Constants::C_ACTOR_MODEL_STANCE_OFFSET});
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
        DMK::Logger &logger = DMK::log();
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

    bool player_missile_drawn(uintptr_t c_player) noexcept
    {
        const uintptr_t phase = c_player != 0 ? shooting_phase_vtable(c_player) : 0;
        return phase != 0 && vtable_is(GameClass::ShootingAiming, phase);
    }

    uintptr_t player_missile_ammo(uintptr_t c_player) noexcept
    {
        return shooting_item(c_player, Constants::SHOOTING_EXPANSION_AMMO_OFFSET);
    }

    uintptr_t player_missile_weapon(uintptr_t c_player) noexcept
    {
        return shooting_item(c_player, Constants::SHOOTING_EXPANSION_WEAPON_OFFSET);
    }

    HeldDecoy player_held_decoy(uintptr_t c_player) noexcept
    {
        if (c_player == 0)
        {
            return {};
        }
        const ActiveMinigame minigame = find_player_minigame(c_player);
        if (minigame.object == 0 ||
            (classify_minigame_vtable(minigame.vtable) & state_bit(GameState::MinigameDistract)) == 0)
        {
            return {};
        }
        const auto decoy =
            DMK::memory::read<uintptr_t>(DMK::Address{minigame.object + Constants::C_DISTRACT_DECOY_OFFSET});
        if (!decoy || !DMK::memory::is_plausible_ptr(DMK::Address{*decoy}))
        {
            return {};
        }
        const auto vtable = DMK::memory::read<uintptr_t>(DMK::Address{*decoy});
        if (!vtable || !vtable_is(GameClass::Decoy, *vtable))
        {
            return {};
        }
        const auto state =
            DMK::memory::read<int32_t>(DMK::Address{minigame.object + Constants::C_DISTRACT_STATE_OFFSET});
        return HeldDecoy{*decoy, state && *state == Constants::DISTRACT_STATE_HOLDING};
    }

    std::optional<float> player_move_input(uintptr_t c_player) noexcept
    {
        if (c_player == 0)
        {
            return std::nullopt;
        }
        const auto input =
            DMK::memory::read<uintptr_t>(DMK::Address{c_player + runtime_offsets().c_player_input.load().value});
        if (!input || !DMK::memory::is_plausible_ptr(DMK::Address{*input}))
        {
            return std::nullopt;
        }
        const auto vtable = DMK::memory::read<uintptr_t>(DMK::Address{*input});
        if (!vtable || !vtable_is(GameClass::PlayerInput, *vtable))
        {
            return std::nullopt;
        }
        const auto x = DMK::memory::read<float>(DMK::Address{*input + Constants::PLAYER_INPUT_MOVE_X_OFFSET});
        const auto y = DMK::memory::read<float>(DMK::Address{*input + Constants::PLAYER_INPUT_MOVE_Y_OFFSET});
        if (!x || !y || !std::isfinite(*x) || !std::isfinite(*y))
        {
            return std::nullopt;
        }
        return std::max(std::fabs(*x), std::fabs(*y));
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
