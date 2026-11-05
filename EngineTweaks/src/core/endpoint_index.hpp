/**
 * @file core/endpoint_index.hpp
 * @brief An index from each endpoint of a link to the map nodes that hold the link.
 *
 * The game's link map is ordered by (from, to, kind), so the nodes that touch one object as their `to` endpoint lie
 * anywhere in the tree. The index lists, for every endpoint, the nodes whose `from` or `to` equals it, so an unlink
 * reads only those nodes. It knows nothing about the tree: callers add each inserted node and remove each erased one.
 */
#ifndef ENGINETWEAKS_CORE_ENDPOINT_INDEX_HPP
#define ENGINETWEAKS_CORE_ENDPOINT_INDEX_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace EngineTweaks::core
{
    /** @brief The endpoints of one map node. */
    struct LinkNode
    {
        std::uintptr_t node{0};
        std::uintptr_t from{0};
        std::uintptr_t to{0};
    };

    /** @brief Endpoint to node lists. Not thread-safe: the owner serializes every call. */
    class EndpointIndex
    {
    public:
        /** @brief Forgets every node. */
        void clear() noexcept;

        /**
         * @brief Lists @p link under both endpoints, once when they are equal.
         * @throws std::bad_alloc If a list cannot grow. The index is then incomplete and must be cleared.
         */
        void add(const LinkNode &link);

        /**
         * @brief Removes @p link from the lists of both endpoints.
         * @return False when the index did not hold the node, which means it no longer matches the map.
         */
        bool remove(const LinkNode &link) noexcept;

        /**
         * @brief Appends every node listed under @p endpoint to @p out, in the order the index holds them.
         * @throws std::bad_alloc If @p out cannot grow.
         */
        void collect(std::uintptr_t endpoint, std::vector<std::uintptr_t> &out) const;

        /**
         * @brief The node listed last under @p endpoint, or nullopt when none is.
         * @details remove() finds the last node of a list first, so a caller that takes its nodes from here empties a
         *          list in linear time.
         */
        [[nodiscard]] std::optional<std::uintptr_t> last(std::uintptr_t endpoint) const noexcept;

        /** @brief The number of nodes the index holds. */
        [[nodiscard]] std::size_t node_count() const noexcept { return m_node_count; }

    private:
        /** @brief Removes @p node from @p endpoint's list. False when the list did not hold it. */
        bool remove_from(std::uintptr_t endpoint, std::uintptr_t node) noexcept;

        std::unordered_map<std::uintptr_t, std::vector<std::uintptr_t>> m_by_endpoint;
        std::size_t m_node_count{0};
    };
} // namespace EngineTweaks::core

#endif // ENGINETWEAKS_CORE_ENDPOINT_INDEX_HPP
