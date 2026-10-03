/**
 * @file engine/octree_query.hpp
 * @brief Render-octree queries: the render nodes in a box, and a node's world bounds.
 */
#ifndef HENRYSENSES_OCTREE_QUERY_HPP
#define HENRYSENSES_OCTREE_QUERY_HPP

#include "constants.hpp"
#include "game_structures.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace HenrySenses
{
    /**
     * @enum OctreeQueryResult
     * @brief Outcome of query_render_nodes().
     */
    enum class OctreeQueryResult
    {
        /// The nodes were copied (possibly none).
        Ok,
        /// The engine, the anchor or the 3D engine identity is unavailable.
        Unavailable,
        /// More nodes than the cap; nothing was copied.
        TooMany,
        /// The engine call faulted; callers should stop querying for the session.
        Faulted,
    };

    /**
     * @brief Copies the render nodes whose bounds overlap @p box (C3DEngine::GetObjectsInBox).
     * @param box World box.
     * @param out Receives the IRenderNode addresses, cleared first.
     * @param cap Upper bound on the node count.
     * @return The outcome.
     * @note Main thread only.
     */
    [[nodiscard]] OctreeQueryResult
    query_render_nodes(const game_structures::Aabb &box, std::vector<std::uintptr_t> &out, std::uint32_t cap);

    /**
     * @brief Copies the render nodes of the given types whose bounds overlap @p box
     *        (C3DEngine::GetObjectsByTypeInBox, once per type).
     * @details The typed query walks only the octree branches and object lists that hold a node of the type, so it
     *          costs a fraction of query_render_nodes() when the box is full of brushes and vegetation. When its vtable
     *          slot cannot be confirmed, the untyped query runs instead and its result is filtered by type.
     * @param box World box.
     * @param types IRenderNode::GetRenderNodeType values (constants::RENDERNODE_TYPE_*).
     * @param out Receives the IRenderNode addresses, cleared first.
     * @param cap Upper bound on the node count (of all types together, or of the untyped query).
     * @param rnd_flags_mask Keeps only the nodes whose render-node flags (IRenderNode + 0x28) share a bit with it, as
     *        the engine's typed walk tests them before it reads a node's bounds; the default keeps every node.
     * @return The outcome.
     * @note Main thread only.
     */
    [[nodiscard]] OctreeQueryResult query_render_nodes_of_types(
        const game_structures::Aabb &box,
        std::span<const std::uint32_t> types,
        std::vector<std::uintptr_t> &out,
        std::uint32_t cap,
        std::uint64_t rnd_flags_mask = constants::ENGINE3D_QUERY_ANY_RNDFLAGS
    );

    /**
     * @brief Reads a render node's type through its IRenderNode::GetRenderNodeType slot.
     * @return The type, or 0 when the call faulted.
     */
    [[nodiscard]] std::uint32_t render_node_type(std::uintptr_t node) noexcept;

    /**
     * @brief Reads a render node's world bounds through its IRenderNode::GetBBox slot.
     * @param node The render node.
     * @return The bounds, or std::nullopt when the call faulted or returned empty bounds.
     */
    [[nodiscard]] std::optional<game_structures::Aabb> render_node_bounds(std::uintptr_t node) noexcept;

} // namespace HenrySenses

#endif // HENRYSENSES_OCTREE_QUERY_HPP
