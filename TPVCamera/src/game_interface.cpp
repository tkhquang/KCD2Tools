/**
 * @file game_interface.cpp
 * @brief Resolves the game's global-context pointer (the camera-manager root).
 *
 * The context pointer lives behind a RIP-relative load in the module image.
 * initialize_game_interface() resolves the storage slot once; the game-state
 * detection (game_state.cpp) walks context -> camera manager from there.
 */

#include "game_interface.hpp"
#include "aob_resolver.hpp"
#include "global_state.hpp"

#include <DetourModKit.hpp>

#include <stdexcept>

using DMK::Format::format_address;

namespace TPVCamera
{

bool initialize_game_interface()
{
    DMK::Logger &logger = DMK::Logger::get_instance();

    try
    {
        logger.info("GameInterface: Initializing from resolved anchors...");

        // The Context cascade's RipRelative candidates each resolve the same global-context storage slot
        // (the RIP-relative MOV/load target), so anchor_address returns the slot directly, or 0 if the
        // module-scoped resolution missed.
        const uintptr_t ctx_slot = anchor_address(AnchorId::Context);
        if (ctx_slot == 0)
        {
            throw std::runtime_error("Context pointer cascade did not resolve");
        }

        g_global_context_ptr_address.store(reinterpret_cast<std::byte *>(ctx_slot), std::memory_order_relaxed);

        logger.info("GameInterface: Global context pointer storage at {}", format_address(ctx_slot));

        return true;
    }
    catch (const std::exception &e)
    {
        logger.error("GameInterface: Initialization failed: {}", e.what());
        return false;
    }
}

void cleanup_game_interface()
{
    g_global_context_ptr_address.store(nullptr, std::memory_order_relaxed);
}

} // namespace TPVCamera
