/**
 * @file table_links.hpp
 * @brief The order in which the game links table databases again after one of them reloads.
 */
#ifndef HOTPAK_TABLE_LINKS_HPP
#define HOTPAK_TABLE_LINKS_HPP

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace HotPak
{
    /**
     * @brief Collects every database whose rows point into @p root's rows, directly or through another, in the order
     *        the manager links them again: ascending dependency depth, ties in discovery order. @p root is excluded.
     * @details The manager's reload gathers the dependents breadth-first, sorts them by the depth its dependency graph
     *          assigned (one more than a database's deepest dependency), then links each one. A database therefore
     *          links after every database in the set that it references.
     * @param dependents_of Returns the direct dependents of a database.
     * @param depth_of Returns a database's dependency depth.
     * @param limit The most dependents collected. A graph that reaches more is truncated there.
     */
    template <class DependentsOf, class DepthOf>
        requires std::invocable<DependentsOf &, std::uintptr_t> && std::invocable<DepthOf &, std::uintptr_t>
    [[nodiscard]] std::vector<std::uintptr_t>
    relink_order(std::uintptr_t root, DependentsOf dependents_of, DepthOf depth_of, std::size_t limit)
    {
        std::vector<std::uintptr_t> found{root};
        for (std::size_t i = 0; i < found.size() && found.size() <= limit; ++i)
        {
            for (const std::uintptr_t dependent : dependents_of(found[i]))
            {
                if (dependent != 0 && found.size() <= limit &&
                    std::find(found.begin(), found.end(), dependent) == found.end())
                {
                    found.push_back(dependent);
                }
            }
        }
        found.erase(found.begin());

        struct Ranked
        {
            std::uintptr_t object = 0;
            std::uint32_t depth = 0;
        };
        std::vector<Ranked> ranked;
        ranked.reserve(found.size());
        for (const std::uintptr_t object : found)
        {
            ranked.push_back(
                Ranked{
                    .object = object,
                    .depth = static_cast<std::uint32_t>(depth_of(object)),
                }
            );
        }
        std::stable_sort(
            ranked.begin(),
            ranked.end(),
            [](const Ranked &left, const Ranked &right) { return left.depth < right.depth; }
        );
        for (std::size_t i = 0; i < ranked.size(); ++i)
        {
            found[i] = ranked[i].object;
        }
        return found;
    }

} // namespace HotPak

#endif // HOTPAK_TABLE_LINKS_HPP
