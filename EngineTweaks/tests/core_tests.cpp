/**
 * @file tests/core_tests.cpp
 * @brief The endpoint index of the link map.
 */

#include "check.hpp"
#include "core/endpoint_index.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

using namespace EngineTweaks::core;

namespace
{
    [[nodiscard]] std::vector<std::uintptr_t> nodes_of(const EndpointIndex &index, std::uintptr_t endpoint)
    {
        std::vector<std::uintptr_t> nodes;
        index.collect(endpoint, nodes);
        std::sort(nodes.begin(), nodes.end());
        return nodes;
    }

    void lists_both_endpoints()
    {
        EndpointIndex index;
        index.add({.node = 0x10, .from = 0xA, .to = 0xB});
        index.add({.node = 0x20, .from = 0xB, .to = 0xC});
        CHECK(index.node_count() == 2);
        CHECK((nodes_of(index, 0xA) == std::vector<std::uintptr_t>{0x10}));
        CHECK((nodes_of(index, 0xB) == std::vector<std::uintptr_t>{0x10, 0x20}));
        CHECK((nodes_of(index, 0xC) == std::vector<std::uintptr_t>{0x20}));
        CHECK(nodes_of(index, 0xD).empty());
    }

    void lists_a_self_link_once()
    {
        EndpointIndex index;
        index.add({.node = 0x10, .from = 0xA, .to = 0xA});
        CHECK((nodes_of(index, 0xA) == std::vector<std::uintptr_t>{0x10}));
        CHECK(index.remove({.node = 0x10, .from = 0xA, .to = 0xA}));
        CHECK(index.node_count() == 0);
        CHECK(nodes_of(index, 0xA).empty());
    }

    void removes_from_both_endpoints()
    {
        EndpointIndex index;
        index.add({.node = 0x10, .from = 0xA, .to = 0xB});
        index.add({.node = 0x20, .from = 0xA, .to = 0xC});
        CHECK(index.remove({.node = 0x10, .from = 0xA, .to = 0xB}));
        CHECK(index.node_count() == 1);
        CHECK((nodes_of(index, 0xA) == std::vector<std::uintptr_t>{0x20}));
        CHECK(nodes_of(index, 0xB).empty());
    }

    void reports_a_node_it_does_not_hold()
    {
        EndpointIndex index;
        index.add({.node = 0x10, .from = 0xA, .to = 0xB});
        CHECK(!index.remove({.node = 0x30, .from = 0xA, .to = 0xB}));
        CHECK(!index.remove({.node = 0x10, .from = 0xA, .to = 0xC}));
        CHECK(!index.remove({.node = 0x10, .from = 0xD, .to = 0xE}));
    }

    void clears()
    {
        EndpointIndex index;
        index.add({.node = 0x10, .from = 0xA, .to = 0xB});
        index.clear();
        CHECK(index.node_count() == 0);
        CHECK(nodes_of(index, 0xA).empty());
        index.add({.node = 0x10, .from = 0xA, .to = 0xB});
        CHECK(index.node_count() == 1);
    }

    void takes_the_last_node()
    {
        EndpointIndex index;
        CHECK(!index.last(0xA).has_value());
        index.add({.node = 0x10, .from = 0xA, .to = 0xB});
        index.add({.node = 0x20, .from = 0xA, .to = 0xC});
        CHECK(index.last(0xA) == std::uintptr_t{0x20});
        CHECK(index.remove({.node = 0x20, .from = 0xA, .to = 0xC}));
        CHECK(index.last(0xA) == std::uintptr_t{0x10});
    }

    void unlinks_an_object_with_many_links()
    {
        // The owner side: one object whose every link is taken through last() until none is left.
        constexpr std::uintptr_t OWNER = 0xFFFF;
        constexpr std::size_t ITEMS = 2000;
        EndpointIndex index;
        for (std::uintptr_t item = 1; item <= ITEMS; ++item)
        {
            index.add({.node = 0x100000 + item, .from = item, .to = OWNER});
        }
        std::size_t taken = 0;
        while (const std::optional<std::uintptr_t> node = index.last(OWNER))
        {
            CHECK(index.remove({.node = *node, .from = *node - 0x100000, .to = OWNER}));
            ++taken;
        }
        CHECK(taken == ITEMS);
        CHECK(index.node_count() == 0);
    }

    void unlinks_many_objects()
    {
        // A soul's items, each linked to its owner, then destroyed one by one as the game's unlink does.
        constexpr std::uintptr_t OWNER = 0xFFFF;
        constexpr std::size_t ITEMS = 2000;
        EndpointIndex index;
        for (std::uintptr_t item = 1; item <= ITEMS; ++item)
        {
            index.add({.node = 0x100000 + item, .from = item, .to = OWNER});
        }
        CHECK(nodes_of(index, OWNER).size() == ITEMS);
        for (std::uintptr_t item = 1; item <= ITEMS; ++item)
        {
            const std::vector<std::uintptr_t> nodes = nodes_of(index, item);
            CHECK(nodes.size() == 1);
            for (const std::uintptr_t node : nodes)
            {
                CHECK(index.remove({.node = node, .from = item, .to = OWNER}));
            }
        }
        CHECK(index.node_count() == 0);
        CHECK(nodes_of(index, OWNER).empty());
    }
} // namespace

int main()
{
    lists_both_endpoints();
    lists_a_self_link_once();
    removes_from_both_endpoints();
    reports_a_node_it_does_not_hold();
    clears();
    takes_the_last_node();
    unlinks_an_object_with_many_links();
    unlinks_many_objects();
    return EngineTweaks::tests::finish("core");
}
