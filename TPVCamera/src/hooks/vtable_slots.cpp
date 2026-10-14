/**
 * @file hooks/vtable_slots.cpp
 * @brief Vtable slot readers for hook targets (see vtable_slots.hpp).
 */

#include "hooks/vtable_slots.hpp"

#include <DetourModKit.hpp>

namespace TPVCamera
{
    uintptr_t read_image_vtable_slot(uintptr_t vtable, size_t slot, uintptr_t module_base, size_t module_size)
    {
        const DMK::Region image{DMK::Address{module_base}, module_size};
        const auto slot_value = DMK::memory::read<uintptr_t>(DMK::Address{vtable + slot * sizeof(uintptr_t)});
        return slot_value.has_value() && image.contains(DMK::Address{*slot_value}) ? *slot_value : 0;
    }

    bool code_window_matches(uintptr_t target, const DMK::scan::Pattern &signature, size_t window,
                             const DMK::Region &image)
    {
        if (target == 0 || window == 0)
        {
            return false;
        }
        const DMK::Region head{DMK::Address{target}, window};
        return image.contains(head.base) && image.contains(head.base.offset(static_cast<std::ptrdiff_t>(window) - 1)) &&
               DMK::scan::scan(signature, head, 1, DMK::scan::Pages::Executable).has_value();
    }

    uintptr_t read_checked_vtable_slot(uintptr_t vtable, size_t slot, const DMK::scan::Pattern &signature,
                                       size_t window, uintptr_t module_base, size_t module_size)
    {
        const DMK::Region image{DMK::Address{module_base}, module_size};
        const uintptr_t target = read_image_vtable_slot(vtable, slot, module_base, module_size);
        return code_window_matches(target, signature, window, image) ? target : 0;
    }
} // namespace TPVCamera
