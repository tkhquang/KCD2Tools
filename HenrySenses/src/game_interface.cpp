/**
 * @file game_interface.cpp
 * @brief Publishes the game's global-context storage slot.
 */

#include "game_interface.hpp"
#include "aob_resolver.hpp"
#include "global_state.hpp"

#include <DetourModKit.hpp>

#include <cstddef>
#include <cstdint>

namespace HenrySenses
{
    DMK::Result<void> initialize_game_interface()
    {
        // The Context cascade resolves the storage slot itself (the RIP-relative load target), so the slot is the
        // anchor address directly.
        const std::uintptr_t ctx_slot = gated_anchor_address(Feature::GameState, AnchorId::Context);
        if (ctx_slot == 0)
        {
            return std::unexpected(DMK::Error{DMK::ErrorCode::NoMatch, "game_interface/context_anchor"});
        }

        g_global_context_ptr_address.store(reinterpret_cast<std::byte *>(ctx_slot), std::memory_order_relaxed);
        DMK::log().info("GameInterface: global context slot at {}", DMK::format::format_address(ctx_slot));
        return {};
    }

    void cleanup_game_interface() noexcept
    {
        g_global_context_ptr_address.store(nullptr, std::memory_order_relaxed);
    }

} // namespace HenrySenses
