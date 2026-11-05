/**
 * @file core/endpoint_index.cpp
 * @brief The endpoint index (see endpoint_index.hpp).
 */

#include "core/endpoint_index.hpp"

#include <algorithm>

namespace EngineTweaks::core
{
    void EndpointIndex::clear() noexcept
    {
        m_by_endpoint.clear();
        m_node_count = 0;
    }

    void EndpointIndex::add(const LinkNode &link)
    {
        m_by_endpoint[link.from].push_back(link.node);
        if (link.to != link.from)
        {
            m_by_endpoint[link.to].push_back(link.node);
        }
        ++m_node_count;
    }

    bool EndpointIndex::remove(const LinkNode &link) noexcept
    {
        const bool listed = remove_from(link.from, link.node);
        if (link.to != link.from && !remove_from(link.to, link.node))
        {
            return false;
        }
        if (!listed)
        {
            return false;
        }
        --m_node_count;
        return true;
    }

    void EndpointIndex::collect(std::uintptr_t endpoint, std::vector<std::uintptr_t> &out) const
    {
        const auto found = m_by_endpoint.find(endpoint);
        if (found != m_by_endpoint.end())
        {
            out.insert(out.end(), found->second.begin(), found->second.end());
        }
    }

    std::optional<std::uintptr_t> EndpointIndex::last(std::uintptr_t endpoint) const noexcept
    {
        const auto found = m_by_endpoint.find(endpoint);
        if (found == m_by_endpoint.end())
        {
            return std::nullopt;
        }
        return found->second.back();
    }

    bool EndpointIndex::remove_from(std::uintptr_t endpoint, std::uintptr_t node) noexcept
    {
        const auto found = m_by_endpoint.find(endpoint);
        if (found == m_by_endpoint.end())
        {
            return false;
        }
        std::vector<std::uintptr_t> &nodes = found->second;
        // The search runs from the back, where last() takes its node from.
        const auto found_reverse = std::find(nodes.rbegin(), nodes.rend(), node);
        if (found_reverse == nodes.rend())
        {
            return false;
        }
        // Order inside a list carries no meaning, so a swap with the last entry keeps the removal O(1).
        *(found_reverse.base() - 1) = nodes.back();
        nodes.pop_back();
        if (nodes.empty())
        {
            m_by_endpoint.erase(found);
        }
        return true;
    }
} // namespace EngineTweaks::core
