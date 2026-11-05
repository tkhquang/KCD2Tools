/**
 * @file hooks/vtable_slots.hpp
 * @brief Reads code the mod reaches through live data, a vtable slot or a call's target, and proves it before it is
 *        trusted (see code_proofs.hpp).
 */
#ifndef TPVCAMERA_HOOKS_VTABLE_SLOTS_HPP
#define TPVCAMERA_HOOKS_VTABLE_SLOTS_HPP

#include "code_proofs.hpp"

#include <DetourModKit/region.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace TPVCamera
{
    /**
     * @brief Reads a vtable slot and returns its target when it lies in the game image, else 0.
     */
    [[nodiscard]] std::uintptr_t read_image_vtable_slot(std::uintptr_t vtable, std::size_t slot,
                                                        std::uintptr_t module_base, std::size_t module_size) noexcept;

    /**
     * @brief True when the code at @p target, inside @p image, holds one of @p proof's shapes.
     * @details The shapes are tried in order, each within its own window from @p target, so the code is proven before
     *          the mod calls or hooks it.
     */
    [[nodiscard]] bool code_proven(std::uintptr_t target, std::span<const Proof::Shape> proof,
                                   const DMK::Region &image) noexcept;

    /**
     * @brief Reads a vtable slot and returns its target when it lies in the game image and holds one of @p proof's
     *        shapes (code_proven()), else 0, so a slot that no longer holds the expected function is refused rather
     *        than called or hooked.
     */
    [[nodiscard]] std::uintptr_t read_proven_vtable_slot(std::uintptr_t vtable, std::size_t slot,
                                                         std::span<const Proof::Shape> proof,
                                                         std::uintptr_t module_base, std::size_t module_size) noexcept;
} // namespace TPVCamera

#endif // TPVCAMERA_HOOKS_VTABLE_SLOTS_HPP
