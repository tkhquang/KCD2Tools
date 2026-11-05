/**
 * @file game/game_state.cpp
 * @brief The anchors binding, the HUD mask, the calendar, the minigame manager and the photo mode manager (see
 *        game_state.hpp and game_memory.hpp).
 */

#include "game/game_state.hpp"
#include "constants.hpp"
#include "game/game_memory.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace HUDClock::game
{
    namespace
    {
        Anchors s_anchors{};
        /// Set after s_anchors is written and cleared before it is forgotten.
        std::atomic<bool> s_bound{false};

        /** @brief The pointer held by the image static @p global, or nullptr. */
        [[nodiscard]] void *static_pointer(std::uintptr_t global) noexcept
        {
            return global == 0 ? nullptr : read<void *>(reinterpret_cast<const void *>(global), 0);
        }

        /** @brief The game context, which lives for the process once created, or nullptr before it exists. */
        [[nodiscard]] void *game_context(const Anchors &anchors) noexcept
        {
            void *const context = static_pointer(anchors.game_context_global);
            return context != nullptr && DMK::memory::is_plausible_ptr(DMK::Address{context}) ? context : nullptr;
        }

        /**
         * @brief The core::MINIGAMES bit of the minigame at map node @p node when the player plays it, else 0.
         * @details The minigame is matched against each class's primary vtable. Its owner is the actor that plays it,
         *          and only the player's actor carries the C_Player primary vtable.
         */
        [[nodiscard]] std::uint32_t player_minigame_bit(const Anchors &anchors, std::uintptr_t node) noexcept
        {
            const auto minigame =
                read_guarded<void *>(reinterpret_cast<const void *>(node), constants::MAP_NODE_MAPPED_OFFSET);
            const std::uintptr_t vtable = minigame ? vtable_of(*minigame) : 0;
            if (vtable == 0)
            {
                return 0;
            }
            for (std::size_t i = 0; i < anchors.minigame_vtables.size(); ++i)
            {
                if (vtable != anchors.minigame_vtables[i])
                {
                    continue;
                }
                // The class check passed, so the C_Minigame owner field is trusted.
                void *const owner = read<void *>(*minigame, constants::MINIGAME_OWNER_OFFSET);
                return checked(owner, anchors.player_vtable) != nullptr ? std::uint32_t{1} << i : 0;
            }
            return 0;
        }
    } // namespace

    void bind(const Anchors &anchors) noexcept
    {
        s_anchors = anchors;
        s_bound.store(true, std::memory_order_release);
    }

    void unbind() noexcept
    {
        s_bound.store(false, std::memory_order_release);
        s_anchors = Anchors{};
    }

    const Anchors *bound_anchors() noexcept
    {
        return s_bound.load(std::memory_order_acquire) ? &s_anchors : nullptr;
    }

    std::optional<std::uint32_t> hud_part_bits() noexcept
    {
        const Anchors *anchors = bound_anchors();
        if (anchors == nullptr)
        {
            return std::nullopt;
        }
        // The mask's destructor clears the static, so a null or foreign object means no HUD.
        void *const mask = checked(static_pointer(anchors->hud_mask_global), anchors->hud_mask_vtable);
        if (mask == nullptr)
        {
            return std::nullopt;
        }
        return read<std::uint32_t>(mask, constants::HUD_MASK_BITS_OFFSET);
    }

    std::optional<std::int64_t> world_time_ms() noexcept
    {
        const Anchors *anchors = bound_anchors();
        if (anchors == nullptr)
        {
            return std::nullopt;
        }
        // The context lives for the process once created, so a raw read of its static never sees a freed object. Its
        // RPG module and calendar come and go with the game session, so each is checked by its vtable.
        void *const context = game_context(*anchors);
        if (context == nullptr)
        {
            return std::nullopt;
        }
        void *const rpg =
            checked(read<void *>(context, constants::CONTEXT_RPG_MODULE_OFFSET), anchors->rpg_module_vtable);
        void *const calendar =
            rpg == nullptr
                ? nullptr
                : checked(read<void *>(rpg, constants::RPG_MODULE_CALENDAR_OFFSET), anchors->calendar_vtable);
        if (calendar == nullptr)
        {
            return std::nullopt;
        }
        return read<std::int64_t>(calendar, constants::CALENDAR_WORLD_TIME_OFFSET);
    }

    std::optional<std::uint32_t> player_minigames() noexcept
    {
        const Anchors *anchors = bound_anchors();
        if (anchors == nullptr || !anchors->minigames)
        {
            return std::nullopt;
        }
        void *const context = game_context(*anchors);
        if (context == nullptr)
        {
            return std::nullopt;
        }
        void *const player_module = read<void *>(context, constants::CONTEXT_PLAYER_MODULE_OFFSET);
        if (checked(player_module, anchors->player_module_vtable) == nullptr)
        {
            return std::nullopt;
        }

        // The manager has no vtable to check, so every read from here on is guarded.
        void *const manager = read<void *>(player_module, constants::PLAYER_MODULE_MINIGAMES_OFFSET);
        const auto *const map = static_cast<const std::byte *>(manager) + constants::MINIGAMES_MAP_OFFSET;
        const auto head = read_guarded<std::uintptr_t>(map, constants::MAP_HEAD_OFFSET);
        const auto size = read_guarded<std::uint64_t>(map, constants::MAP_SIZE_OFFSET);
        if (!head || !size || *size > constants::MAX_MINIGAMES)
        {
            return std::nullopt;
        }

        // Every node but the head, depth first from the root (the head's parent link). Each leaf link points back to
        // the head, so a branch ends there. The element count and the stack size bound the walk on a corrupt map.
        std::uint32_t bits = 0;
        std::array<std::uintptr_t, constants::MAX_MINIGAMES> pending{};
        std::size_t count = 0;
        std::size_t visited = 0;
        if (const auto root =
                read_guarded<std::uintptr_t>(reinterpret_cast<const void *>(*head), constants::MAP_NODE_PARENT_OFFSET))
        {
            pending[count++] = *root;
        }
        while (count != 0 && visited < *size)
        {
            const std::uintptr_t node = pending[--count];
            const auto *const at = reinterpret_cast<const void *>(node);
            const auto is_nil = read_guarded<std::uint8_t>(at, constants::MAP_NODE_IS_NIL_OFFSET);
            if (node == *head || !is_nil || *is_nil != 0)
            {
                continue;
            }
            ++visited;
            bits |= player_minigame_bit(*anchors, node);
            for (const std::ptrdiff_t link : {constants::MAP_NODE_LEFT_OFFSET, constants::MAP_NODE_RIGHT_OFFSET})
            {
                const auto child = read_guarded<std::uintptr_t>(at, link);
                if (child && *child != *head && count < pending.size())
                {
                    pending[count++] = *child;
                }
            }
        }
        return bits;
    }

    bool photo_mode_active() noexcept
    {
        const Anchors *anchors = bound_anchors();
        if (anchors == nullptr || !anchors->photo_mode)
        {
            return false;
        }
        void *const context = game_context(*anchors);
        if (context == nullptr)
        {
            return false;
        }
        void *const manager = read<void *>(context, constants::CONTEXT_PHOTOMODE_MANAGER_OFFSET);
        return checked(manager, anchors->photomode_manager_vtable) != nullptr &&
               read<std::uint8_t>(manager, constants::PHOTOMODE_ACTIVE_OFFSET) != 0;
    }
} // namespace HUDClock::game
