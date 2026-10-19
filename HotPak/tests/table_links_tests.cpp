/**
 * @file tests/table_links_tests.cpp
 * @brief Unit tests of the order in which dependent table databases link again.
 */

#include "check.hpp"
#include "table_links.hpp"

#include <cstdint>
#include <map>
#include <vector>

namespace
{
    using namespace HotPak;

    /** @brief A dependency graph: each database's direct dependents and its depth. */
    struct Graph
    {
        std::map<std::uintptr_t, std::vector<std::uintptr_t>> dependents;
        std::map<std::uintptr_t, std::uint32_t> depth;

        [[nodiscard]] std::vector<std::uintptr_t> order(std::uintptr_t root, std::size_t limit = 64) const
        {
            return relink_order(
                root,
                [this](std::uintptr_t object)
                {
                    const auto it = dependents.find(object);
                    return it == dependents.end() ? std::vector<std::uintptr_t>{} : it->second;
                },
                [this](std::uintptr_t object) { return depth.at(object); },
                limit
            );
        }
    };

    void orders_by_depth()
    {
        // ClothingMaterial (1) -> CharacterComponent (2) -> CharacterUberlod (3), and 1 -> Armor (4) -> Uberlod (3).
        const Graph graph{
            .dependents = {{1, {2, 4}}, {2, {3}}, {4, {3}}},
            .depth = {{1, 0}, {2, 3}, {3, 4}, {4, 1}},
        };
        const std::vector<std::uintptr_t> order = graph.order(1);
        CHECK((order == std::vector<std::uintptr_t>{4, 2, 3}));
    }

    void excludes_root_and_repeats()
    {
        const Graph graph{
            .dependents = {{1, {2, 3, 2}}, {2, {1, 3}}, {3, {}}},
            .depth = {{1, 0}, {2, 1}, {3, 1}},
        };
        const std::vector<std::uintptr_t> order = graph.order(1);
        CHECK((order == std::vector<std::uintptr_t>{2, 3}));
    }

    void ties_keep_discovery_order()
    {
        const Graph graph{
            .dependents = {{1, {5, 3, 4}}},
            .depth = {{1, 0}, {3, 2}, {4, 2}, {5, 2}},
        };
        CHECK((graph.order(1) == std::vector<std::uintptr_t>{5, 3, 4}));
    }

    void stops_at_the_limit()
    {
        const Graph graph{
            .dependents = {{1, {2, 3, 4, 5}}},
            .depth = {{1, 0}, {2, 1}, {3, 1}, {4, 1}, {5, 1}},
        };
        CHECK(graph.order(1, 2).size() == 2);
        CHECK(graph.order(9).empty());
    }
} // namespace

int main()
{
    orders_by_depth();
    excludes_root_and_repeats();
    ties_keep_discovery_order();
    stops_at_the_limit();
    return HotPak::tests::finish("table_links");
}
