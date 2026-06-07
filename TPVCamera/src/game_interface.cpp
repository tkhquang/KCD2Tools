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

bool initialize_game_interface(uintptr_t module_base, size_t module_size)
{
    DMK::Logger &logger = DMK::Logger::get_instance();

    try
    {
        logger.info("GameInterface: Initializing with dynamic AOB scanning...");

        // The cascade's RipRelative candidates each resolve the same global-context storage slot
        // (the RIP-relative MOV/load target), so the returned address is the slot directly. The
        // module-scoped resolver guarantees the slot lies inside the game image or returns 0.
        const uintptr_t ctx_slot = resolve_address(Aob::k_contextCandidates, "GlobalContextPtr", module_base, module_size);
        if (ctx_slot == 0)
        {
            throw std::runtime_error("Context pointer cascade did not resolve");
        }

        g_global_context_ptr_address = reinterpret_cast<std::byte *>(ctx_slot);

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
    g_global_context_ptr_address = nullptr;
}

} // namespace TPVCamera
