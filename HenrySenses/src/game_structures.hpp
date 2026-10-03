/**
 * @file game_structures.hpp
 * @brief Plain engine value types the mod reads through guarded memory.
 */
#ifndef HENRYSENSES_GAME_STRUCTURES_HPP
#define HENRYSENSES_GAME_STRUCTURES_HPP

#include <DetourModKit/memory.hpp>

#include <type_traits>

namespace HenrySenses::game_structures
{
    /**
     * @brief CryEngine Vec3 (three packed floats).
     */
    struct Vec3f
    {
        float x;
        float y;
        float z;
    };

    /**
     * @brief CryEngine AABB (min corner, max corner).
     */
    struct Aabb
    {
        Vec3f min;
        Vec3f max;
    };

    /**
     * @brief CryEngine Matrix34: row-major 3x4 storage, column-vector convention (world = M * local).
     * @details Columns 0..2 hold the local X-right, Y-forward and Z-up axes, column 3 the translation.
     */
    struct Matrix34f
    {
        float m[3][4];
    };

} // namespace HenrySenses::game_structures

// memory::read<T> accepts a class only when every bit pattern is a valid object representation, so each padding-free
// all-float aggregate opts in explicitly.
namespace DetourModKit::detail
{
    template <> struct enable_representation_safe_aggregate<HenrySenses::game_structures::Vec3f> : std::true_type
    {
    };
    template <> struct enable_representation_safe_aggregate<HenrySenses::game_structures::Aabb> : std::true_type
    {
    };
    template <> struct enable_representation_safe_aggregate<HenrySenses::game_structures::Matrix34f> : std::true_type
    {
    };
} // namespace DetourModKit::detail

static_assert(sizeof(HenrySenses::game_structures::Vec3f) == 3 * sizeof(float), "Vec3f must stay padding-free.");
static_assert(sizeof(HenrySenses::game_structures::Aabb) == 6 * sizeof(float), "Aabb must stay padding-free.");
static_assert(
    sizeof(HenrySenses::game_structures::Matrix34f) == 12 * sizeof(float),
    "Matrix34f must stay padding-free."
);

#endif // HENRYSENSES_GAME_STRUCTURES_HPP
