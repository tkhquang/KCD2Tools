/**
 * @file math_utils.hpp
 * @brief The camera's vector and quaternion (core/math/vec.hpp), opted in to DetourModKit's memory::read.
 */
#ifndef TPVCAMERA_MATH_UTILS_HPP
#define TPVCAMERA_MATH_UTILS_HPP

#include "core/math/vec.hpp"

#include <DetourModKit/memory.hpp>

#include <type_traits>

// memory::read<T> accepts a class only when EVERY bit pattern is a valid object representation, so a
// class must opt in. Both types below are padding-free aggregates of plain floats: no bool, no unscoped
// enum, no member pointer, and sizeof equal to the sum of the members. Every foreign byte pattern they
// can hold decodes to a valid value, which makes the opt-in sound. The static_asserts beside the types
// pin the padding-free property so a later member cannot silently invalidate it.
namespace DetourModKit::detail
{
    template <> struct enable_representation_safe_aggregate<TPVCamera::Vector3> : std::true_type
    {
    };
    template <> struct enable_representation_safe_aggregate<TPVCamera::Quaternion> : std::true_type
    {
    };
} // namespace DetourModKit::detail

#endif // TPVCAMERA_MATH_UTILS_HPP
