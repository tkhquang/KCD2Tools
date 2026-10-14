/**
 * @file hooks/vtable_slots.hpp
 * @brief Reads a hook target from a live vtable slot and proves it before it is trusted.
 */
#ifndef TPVCAMERA_HOOKS_VTABLE_SLOTS_HPP
#define TPVCAMERA_HOOKS_VTABLE_SLOTS_HPP

#include <DetourModKit/region.hpp>
#include <DetourModKit/scan.hpp>

#include <cstddef>
#include <cstdint>

namespace TPVCamera
{
    /**
     * @brief Reads a vtable slot and returns its target when it lies in the game image, else 0.
     */
    [[nodiscard]] std::uintptr_t read_image_vtable_slot(std::uintptr_t vtable, std::size_t slot,
                                                        std::uintptr_t module_base, std::size_t module_size);

    /**
     * @brief True when @p signature matches within the first @p window bytes of the code at @p target, all inside
     *        @p image.
     * @details A window of the pattern's own length requires the match to start exactly at @p target. Proves the
     *          instructions a call or hook depends on at an address read from live data, before it is trusted.
     */
    [[nodiscard]] bool code_window_matches(std::uintptr_t target, const DMK::scan::Pattern &signature,
                                           std::size_t window, const DMK::Region &image);

    /**
     * @brief Reads a vtable slot and returns its target when it lies in the game image and @p signature matches
     *        within its first @p window bytes, else 0, so a slot that no longer holds the expected function is
     *        refused rather than hooked or called.
     */
    [[nodiscard]] std::uintptr_t read_checked_vtable_slot(std::uintptr_t vtable, std::size_t slot,
                                                          const DMK::scan::Pattern &signature, std::size_t window,
                                                          std::uintptr_t module_base, std::size_t module_size);
} // namespace TPVCamera

#endif // TPVCAMERA_HOOKS_VTABLE_SLOTS_HPP
