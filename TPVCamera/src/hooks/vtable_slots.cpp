/**
 * @file hooks/vtable_slots.cpp
 * @brief Vtable slot readers and code proofs for hook and call targets (see vtable_slots.hpp).
 */

#include "hooks/vtable_slots.hpp"

#include <DetourModKit.hpp>

#include <algorithm>

namespace TPVCamera
{
    namespace
    {
        /** @brief True when @p shape matches within its window from @p target, the whole window inside @p image. */
        [[nodiscard]] bool shape_matches(uintptr_t target, const Proof::Shape &shape, const DMK::Region &image) noexcept
        {
            const DMK::Region head{DMK::Address{target}, shape.window};
            return image.contains(head.base) &&
                   image.contains(head.base.offset(static_cast<std::ptrdiff_t>(shape.window) - 1)) &&
                   DMK::scan::scan(shape.pattern, head, 1, DMK::scan::Pages::Executable).has_value();
        }
    } // namespace

    uintptr_t read_image_vtable_slot(uintptr_t vtable, size_t slot, uintptr_t module_base, size_t module_size) noexcept
    {
        const DMK::Region image{DMK::Address{module_base}, module_size};
        const auto slot_value = DMK::memory::read<uintptr_t>(DMK::Address{vtable + slot * sizeof(uintptr_t)});
        return slot_value.has_value() && image.contains(DMK::Address{*slot_value}) ? *slot_value : 0;
    }

    bool code_proven(uintptr_t target, std::span<const Proof::Shape> proof, const DMK::Region &image) noexcept
    {
        return target != 0 && std::ranges::any_of(proof, [&](const Proof::Shape &shape)
                                                  { return shape_matches(target, shape, image); });
    }

    uintptr_t read_proven_vtable_slot(uintptr_t vtable, size_t slot, std::span<const Proof::Shape> proof,
                                      uintptr_t module_base, size_t module_size) noexcept
    {
        const DMK::Region image{DMK::Address{module_base}, module_size};
        const uintptr_t target = read_image_vtable_slot(vtable, slot, module_base, module_size);
        return code_proven(target, proof, image) ? target : 0;
    }
} // namespace TPVCamera
