/**
 * @file game_state.cpp
 * @brief Live game-state detection and the HideIn situation lists.
 */

#include "game_state.hpp"
#include "config.hpp"
#include "constants.hpp"
#include "global_state.hpp"
#include "offset_heal.hpp"
#include "rtti_types.hpp"

#include <DetourModKit.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <string_view>
#include <string>
#include <algorithm>

namespace HenrySenses
{
    namespace
    {
        /**
         * @brief Reads the global-context object, publishing it to the self-heal once the world is live.
         * @return The context object address, or 0.
         */
        [[nodiscard]] std::uintptr_t read_context() noexcept
        {
            std::byte *const slot = g_global_context_ptr_address.load(std::memory_order_relaxed);
            if (slot == nullptr)
            {
                return 0;
            }
            const auto context =
                DMK::memory::read<std::uintptr_t>(DMK::Address{reinterpret_cast<std::uintptr_t>(slot)});
            if (!context || !DMK::memory::is_plausible_ptr(DMK::Address{*context}))
            {
                return 0;
            }
            // Gating the heal on the world being live keeps a camera-manager offset drift recoverable without ever
            // scanning a half-built context at the main menu.
            if (game_world_ready().load(std::memory_order_relaxed))
            {
                note_context_base(*context);
            }
            return *context;
        }

        /**
         * @brief Classifies the active wh::game camera into the combat / dialogue bits.
         * @param context Global-context object.
         * @return The matching bit, or 0.
         */
        [[nodiscard]] std::uint32_t poll_active_camera_state(std::uintptr_t context) noexcept
        {
            // One guarded walk: context -> camera manager (self-healed) -> active camera -> vtable slot.
            const std::array<std::ptrdiff_t, 3> camera_chain{
                offset_value(runtime_offsets().context_manager),
                constants::OFFSET_ACTIVE_CAMERA,
                0
            };
            const auto camera = DMK::memory::walk(DMK::Address{context}, camera_chain);
            if (!camera)
            {
                return 0;
            }
            const auto vtable = DMK::memory::read<std::uintptr_t>(*camera);
            if (!vtable || !DMK::memory::is_plausible_ptr(DMK::Address{*vtable}))
            {
                return 0;
            }
            if (vtable_is(GameClass::CameraCombat, *vtable))
            {
                return state_bit(GameState::Combat);
            }
            if (vtable_is(GameClass::CameraDialog, *vtable))
            {
                return state_bit(GameState::Dialogue);
            }
            return 0;
        }

        /**
         * @brief Detects a player-owned minigame through the C_MinigameManager's active list.
         * @details Only dice swaps the active camera, so lockpicking, reading and the other first-person minigames
         *          are detected from the manager instead: context -> C_PlayerModule -> C_MinigameManager -> a
         *          circular list whose sentinel links to itself when empty. The walk is bounded.
         * @param context Global-context object.
         * @param c_player Live C_Player, or 0 to accept the first entry.
         * @return state_bit(Minigame) when the player is in a minigame, else 0.
         */
        [[nodiscard]] std::uint32_t poll_active_minigame(std::uintptr_t context, std::uintptr_t c_player) noexcept
        {
            const std::array<std::ptrdiff_t, 3> minigame_chain{
                offset_value(runtime_offsets().context_minigame_subsystem),
                constants::OFFSET_MINIGAME_MANAGER,
                constants::OFFSET_MINIGAME_MAP_HEAD
            };
            const auto head_slot = DMK::memory::walk(DMK::Address{context}, minigame_chain);
            if (!head_slot)
            {
                return 0;
            }
            const auto head = DMK::memory::read<std::uintptr_t>(*head_slot);
            if (!head || !DMK::memory::is_plausible_ptr(DMK::Address{*head}))
            {
                return 0;
            }
            const auto begin =
                DMK::memory::read<std::uintptr_t>(DMK::Address{*head + constants::OFFSET_MINIGAME_NODE_NEXT});
            if (!begin)
            {
                return 0;
            }

            constexpr int max_nodes = 16;
            std::uintptr_t node = *begin;
            for (int i = 0; i < max_nodes && node != *head && DMK::memory::is_plausible_ptr(DMK::Address{node}); ++i)
            {
                const auto minigame =
                    DMK::memory::read<std::uintptr_t>(DMK::Address{node + constants::OFFSET_MINIGAME_NODE_VALUE});
                if (minigame && DMK::memory::is_plausible_ptr(DMK::Address{*minigame}))
                {
                    if (c_player == 0)
                    {
                        return state_bit(GameState::Minigame);
                    }
                    const auto owner =
                        DMK::memory::read<std::uintptr_t>(DMK::Address{*minigame + constants::OFFSET_MINIGAME_OWNER});
                    if (owner && *owner == c_player)
                    {
                        return state_bit(GameState::Minigame);
                    }
                }
                const auto next =
                    DMK::memory::read<std::uintptr_t>(DMK::Address{node + constants::OFFSET_MINIGAME_NODE_NEXT});
                if (!next)
                {
                    break;
                }
                node = *next;
            }
            return 0;
        }
    } // namespace

    std::uint32_t poll_game_state(std::uintptr_t c_player) noexcept
    {
        DMK_PROFILE_FUNCTION();
        std::uint32_t mask = 0;

        if (const std::uintptr_t context = read_context(); context != 0)
        {
            mask |= poll_active_camera_state(context);
            mask |= poll_active_minigame(context, c_player);
        }
        return mask;
    }

    StateList parse_state_list(std::string_view list)
    {
        struct Token
        {
            std::string_view name;
            GameState state;
        };
        constexpr std::array<Token, 4> tokens{{
            {"dialogue", GameState::Dialogue},
            {"dialog", GameState::Dialogue},
            {"combat", GameState::Combat},
            {"minigame", GameState::Minigame},
        }};
        StateList parsed;
        std::size_t start = 0;
        while (start <= list.size())
        {
            const std::size_t comma = list.find(',', start);
            const std::size_t end = comma == std::string_view::npos ? list.size() : comma;
            std::string_view token = list.substr(start, end - start);
            while (!token.empty() && (token.front() == ' ' || token.front() == '\t'))
            {
                token.remove_prefix(1);
            }
            while (!token.empty() && (token.back() == ' ' || token.back() == '\t' || token.back() == '\r'))
            {
                token.remove_suffix(1);
            }
            if (!token.empty())
            {
                const auto known = std::find_if(
                    tokens.begin(),
                    tokens.end(),
                    [token](const Token &entry)
                    {
                        return entry.name.size() == token.size() &&
                               std::equal(
                                   token.begin(),
                                   token.end(),
                                   entry.name.begin(),
                                   [](char a, char b) { return ascii_lower(a) == b; }
                               );
                    }
                );
                if (known != tokens.end())
                {
                    parsed.mask |= state_bit(known->state);
                }
                else
                {
                    parsed.unknown += (parsed.unknown.empty() ? "" : ", ") + std::string(token);
                }
            }
            if (comma == std::string_view::npos)
            {
                break;
            }
            start = comma + 1;
        }
        return parsed;
    }

    std::string state_list_text(std::uint32_t mask)
    {
        constexpr std::array<std::pair<GameState, std::string_view>, 3> names{{
            {GameState::Dialogue, "Dialogue"},
            {GameState::Combat, "Combat"},
            {GameState::Minigame, "Minigame"},
        }};
        std::string text;
        for (const auto &[state, name] : names)
        {
            if ((mask & state_bit(state)) != 0)
            {
                text += (text.empty() ? "" : ", ") + std::string(name);
            }
        }
        return text.empty() ? std::string{"none"} : text;
    }

    std::uint32_t default_hide_mask() noexcept
    {
        return settings().hide_in.load(std::memory_order_relaxed);
    }

} // namespace HenrySenses
