/**
 * @file game_interface.cpp
 * @brief Resolves the game's global-context pointer (the camera-manager root).
 *
 * The context pointer lives behind a RIP-relative load in the module image.
 * initialize_game_interface() resolves the storage slot once; the game-state
 * detection (game_state.cpp) walks context -> camera manager from there.
 */

#include "game_interface.hpp"
#include "constants.hpp"
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

        auto ctx_pat = DMK::Scanner::parse_aob(Constants::CONTEXT_PTR_LOAD_AOB_PATTERN);
        if (!ctx_pat.has_value())
        {
            throw std::runtime_error("Failed to parse context pointer AOB pattern");
        }

        const std::byte *ctx_aob = DMK::Scanner::find_pattern(reinterpret_cast<const std::byte *>(module_base), module_size, *ctx_pat);
        if (!ctx_aob)
        {
            throw std::runtime_error("Context pointer AOB pattern not found");
        }

        logger.debug("GameInterface: Found context AOB at {}", format_address(reinterpret_cast<uintptr_t>(ctx_aob)));

        // Resolve RIP-relative target: mov rax, [rip + offset] (48 8B 05 xx xx xx xx).
        // The AOB match is 2 bytes before the MOV instruction.
        auto ctx_target_addr = DMK::Scanner::resolve_rip_relative(ctx_aob + 2, 3, 7);
        if (!ctx_target_addr.has_value())
        {
            throw std::runtime_error("Failed to resolve context pointer RIP-relative address");
        }

        g_global_context_ptr_address = reinterpret_cast<std::byte *>(ctx_target_addr.value());

        logger.info("GameInterface: Global context pointer storage at {}", format_address(ctx_target_addr.value()));

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
