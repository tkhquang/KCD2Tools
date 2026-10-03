/**
 * @file engine/engine_env.cpp
 * @brief gEnv resolution, guarded interface reads and the local-player chain.
 */

#include "engine/engine_env.hpp"
#include "aob_resolver.hpp"
#include "constants.hpp"
#include "global_state.hpp"
#include "offset_heal.hpp"
#include "rtti_types.hpp"
#include "engine/seh.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace HenrySenses
{
    namespace
    {
        std::atomic<std::uintptr_t> s_genv{0};
        // CCryAction lives for the whole process once created, so it is resolved once and cached.
        std::atomic<std::uintptr_t> s_cry_action{0};
        std::atomic<std::uintptr_t> s_logged_player{0};

        using GetFrameworkFn = std::uintptr_t(__fastcall *)(std::uintptr_t game);

        /**
         * @brief Calls IGame::GetIGameFramework under SEH.
         * @param fn The screened virtual target.
         * @param game The IGame instance.
         * @return The framework pointer, or 0 on a fault.
         */
        [[nodiscard]] std::uintptr_t call_get_framework(std::uintptr_t fn, std::uintptr_t game) noexcept
        {
            __try
            {
                return reinterpret_cast<GetFrameworkFn>(fn)(game);
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return 0;
            }
        }

        /**
         * @brief Resolves CCryAction once through IGame::GetIGameFramework and caches it.
         * @return The CCryAction address, or 0.
         */
        [[nodiscard]] std::uintptr_t resolve_cry_action() noexcept
        {
            const std::uintptr_t cached = s_cry_action.load(std::memory_order_relaxed);
            if (cached != 0)
            {
                return cached;
            }

            const std::uintptr_t game = genv_interface(constants::GENV_PGAME_OFFSET);
            if (game == 0)
            {
                return 0;
            }
            const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{game});
            if (!vtable || !in_game_image(*vtable))
            {
                return 0;
            }
            const auto fn =
                DMK::memory::read<std::uintptr_t>(DMK::Address{*vtable + constants::IGAME_GET_FRAMEWORK_VTABLE_OFFSET});
            if (!fn || !in_game_image(*fn))
            {
                return 0;
            }
            const std::uintptr_t cry_action = call_get_framework(*fn, game);
            if (!DMK::memory::is_plausible_ptr(DMK::Address{cry_action}))
            {
                return 0;
            }
            s_cry_action.store(cry_action, std::memory_order_relaxed);
            return cry_action;
        }
    } // namespace

    DMK::Result<void> initialize_engine_env()
    {
        const std::uintptr_t resolved = gated_anchor_address(Feature::Core, AnchorId::Genv);
        if (resolved == 0 || !DMK::memory::is_plausible_ptr(DMK::Address{resolved}))
        {
            return std::unexpected(DMK::Error{DMK::ErrorCode::NoMatch, "engine_env/genv_anchor"});
        }
        s_genv.store(resolved, std::memory_order_release);
        DMK::log().info("EngineEnv: gEnv at {}", DMK::format::format_address(resolved));
        return {};
    }

    void shutdown_engine_env() noexcept
    {
        s_genv.store(0, std::memory_order_release);
        s_cry_action.store(0, std::memory_order_relaxed);
        s_logged_player.store(0, std::memory_order_relaxed);
    }

    std::uintptr_t genv() noexcept
    {
        return s_genv.load(std::memory_order_acquire);
    }

    std::uintptr_t genv_interface(std::ptrdiff_t member_offset) noexcept
    {
        const std::uintptr_t base = genv();
        if (base == 0)
        {
            return 0;
        }
        const auto value = DMK::memory::read<std::uintptr_t>(DMK::Address{base + member_offset});
        if (!value || !DMK::memory::is_plausible_ptr(DMK::Address{*value}))
        {
            return 0;
        }
        return *value;
    }

    std::uintptr_t cry_action() noexcept
    {
        return resolve_cry_action();
    }

    std::uintptr_t resolve_c_player() noexcept
    {
        const std::uintptr_t cry_action = resolve_cry_action();
        if (cry_action == 0)
        {
            return 0;
        }
        note_framework_base(cry_action);

        // CCryAction -> CActionGame -> C_Player in one walk; the trace keeps CActionGame for the self-heal even when
        // the player hop fails.
        RuntimeOffsets &offsets = runtime_offsets();
        const std::array<std::ptrdiff_t, 3> chain{
            offset_value(offsets.ccryaction_actiongame),
            offset_value(offsets.cactiongame_local_actor),
            0
        };
        std::array<DMK::Address, 3> trace{};
        const auto player = DMK::memory::walk(DMK::Address{cry_action}, chain, trace);
        if (trace[0].raw() == 0)
        {
            return 0;
        }
        note_action_game_base(trace[0].raw());
        if (!player)
        {
            return 0;
        }
        if (!object_is(GameClass::Player, player->raw()))
        {
            // A populated slot that is not a C_Player is the signature of a CActionGame layout drift; let the
            // self-heal recover the offset on its own cadence.
            request_local_actor_recovery();
            return 0;
        }

        note_player_base(player->raw());
        game_world_ready().store(true, std::memory_order_relaxed);

        // Log the player only when it changes (first resolve, then after a load) so the address is available to
        // external tooling without flooding the log.
        if (s_logged_player.exchange(player->raw(), std::memory_order_relaxed) != player->raw())
        {
            (void)DMK::log().try_log(
                DMK::LogLevel::Debug,
                "EngineEnv: C_Player={:#018x} (CCryAction={:#018x})",
                player->raw(),
                cry_action
            );
        }
        return player->raw();
    }

    std::uintptr_t player_entity(std::uintptr_t c_player) noexcept
    {
        if (c_player == 0)
        {
            return 0;
        }
        const auto entity =
            DMK::memory::read<std::uintptr_t>(DMK::Address{c_player + offset_value(runtime_offsets().c_player_entity)});
        if (!entity || !object_is(GameClass::Entity, *entity))
        {
            return 0;
        }
        return *entity;
    }

    bool in_game_image(std::uintptr_t address) noexcept
    {
        const ModuleInfo &mod = module_info();
        return mod.base != 0 && address >= mod.base && address < mod.base + mod.size;
    }

    std::uintptr_t follow_jump_thunk(std::uintptr_t function) noexcept
    {
        constexpr std::uint8_t jmp_rel32 = 0xE9;
        constexpr int max_hops = 2;
        std::uintptr_t current = function;
        for (int hop = 0; hop < max_hops; ++hop)
        {
            const auto opcode = DMK::memory::read<std::uint8_t>(DMK::Address{current});
            if (!opcode || *opcode != jmp_rel32)
            {
                break;
            }
            const auto rel = DMK::memory::read<std::int32_t>(DMK::Address{current + 1});
            if (!rel)
            {
                break;
            }
            const std::uintptr_t next = current + 5 + static_cast<std::intptr_t>(*rel);
            if (!in_game_image(next))
            {
                break;
            }
            current = next;
        }
        return current;
    }

    std::uintptr_t read_vtable_slot(std::uintptr_t object, std::ptrdiff_t slot_offset) noexcept
    {
        if (object == 0 || !DMK::memory::is_plausible_ptr(DMK::Address{object}))
        {
            return 0;
        }
        const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{object});
        if (!vtable || !in_game_image(*vtable))
        {
            return 0;
        }
        const auto function = DMK::memory::read<std::uintptr_t>(DMK::Address{*vtable + slot_offset});
        if (!function || !in_game_image(*function))
        {
            return 0;
        }
        return *function;
    }

    std::uintptr_t validated_vtable_slot(std::uintptr_t object, std::ptrdiff_t slot_offset, AnchorId expected) noexcept
    {
        const std::uintptr_t anchor = anchor_address(expected);
        const std::uintptr_t function = read_vtable_slot(object, slot_offset);
        if (anchor == 0 || function == 0)
        {
            return 0;
        }
        if (function != anchor && follow_jump_thunk(function) != anchor)
        {
            return 0;
        }
        return function;
    }

    std::string read_c_string(std::uintptr_t text, std::size_t max_length)
    {
        constexpr std::uintptr_t chunk_size = 64;
        std::string out;
        if (!DMK::memory::is_plausible_ptr(DMK::Address{text}))
        {
            return out;
        }
        std::uintptr_t cursor = text;
        while (out.size() < max_length)
        {
            std::array<char, chunk_size> chunk{};
            // An aligned chunk never crosses a page, so the read covers the string's page only.
            const auto length = static_cast<std::size_t>(chunk_size - cursor % chunk_size);
            if (!DMK::memory::read_into(DMK::Address{cursor}, std::as_writable_bytes(std::span{chunk.data(), length})))
            {
                return {};
            }
            for (std::size_t i = 0; i < length; ++i)
            {
                if (chunk[i] == '\0' || out.size() == max_length)
                {
                    return out;
                }
                out.push_back(chunk[i]);
            }
            cursor += length;
        }
        return out;
    }

} // namespace HenrySenses
