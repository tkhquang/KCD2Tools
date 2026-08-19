/**
 * @file game_structures.hpp
 * @brief Game-specific structure definitions for Kingdom Come: Deliverance II.
 *
 * Contains the minimal reverse-engineered struct the camera reads and writes.
 * Other engine fields (input events, entity world matrix) are accessed through
 * raw byte offsets from constants.hpp rather than typed structs.
 */
#ifndef TPVCAMERA_GAME_STRUCTURES_HPP
#define TPVCAMERA_GAME_STRUCTURES_HPP

#include <DetourModKit/memory.hpp>

#include <type_traits>

namespace TPVCamera::GameStructures
{

    /**
     * @brief 3x4 Matrix used by the game engine (CryEngine).
     * @details Row-major 3x4 storage. CryEngine uses the column-vector convention
     *          (world = M * local), so the 3x3 block holds the local-axis basis vectors
     *          in its columns (X-Right = col0, Y-Forward = col1, Z-Up = col2) and column 3
     *          holds translation. The camera reads the entity world matrix and writes the
     *          render camera matrix through this layout (m[row][col]).
     */
    struct Matrix34f
    {
        float m[3][4]; // m[0] is row0, etc.; column 3 is translation.
    };

} // namespace TPVCamera::GameStructures

// memory::read<T> accepts a class only when every bit pattern is a valid object representation, so a
// class must opt in. Matrix34f is a padding-free 12-float array: every foreign byte pattern it can hold
// decodes to a valid value, which makes the reinterpret defined.
namespace DetourModKit::detail
{
    template <> struct enable_representation_safe_aggregate<TPVCamera::GameStructures::Matrix34f> : std::true_type
    {
    };
} // namespace DetourModKit::detail

static_assert(sizeof(TPVCamera::GameStructures::Matrix34f) == 12 * sizeof(float),
              "Matrix34f must stay padding-free for memory::read<Matrix34f> to be a defined reinterpret.");

#endif // TPVCAMERA_GAME_STRUCTURES_HPP
