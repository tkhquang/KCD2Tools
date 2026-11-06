/**
 * @file tool/mcp_server.hpp
 * @brief The tool's Model Context Protocol server, so an AI assistant can check clips and make and pack animation mods:
 *        JSON-RPC over stdio (one message per line). It serves the 2026-07-28 revision (the version in each request's
 *        `_meta`, `server/discover`) and the earlier revisions that open with `initialize`.
 */

#ifndef ANIMFW_TOOL_MCP_SERVER_HPP
#define ANIMFW_TOOL_MCP_SERVER_HPP

#include <filesystem>
#include <optional>

namespace AnimationFramework
{
    /**
     * @brief Serves until stdin closes. The game is read from @p game_folder (else the Steam install) the first time a
     *        tool needs it.
     * @return The process exit code, 0.
     */
    [[nodiscard]] int run_mcp_server(std::optional<std::filesystem::path> game_folder);
} // namespace AnimationFramework

#endif // ANIMFW_TOOL_MCP_SERVER_HPP
