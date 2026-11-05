/**
 * @file game/game_memory.hpp
 * @brief Reads of game memory and the bound anchors.
 *
 * A virtual call goes through a vtable matched to its RTTI type first, so a shifted layout on an unknown build fails
 * the call instead of a jump through the wrong table. Fields of an object whose vtable passed its check are trusted.
 */
#ifndef HUDCLOCK_GAME_GAME_MEMORY_HPP
#define HUDCLOCK_GAME_GAME_MEMORY_HPP

#include "anchors.hpp"

#include <DetourModKit.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace HUDClock::game
{
    /**
     * @brief Stores the resolved anchors. Call once before any other function of the game layer.
     * @note Setup only, before the frame hook arms.
     */
    void bind(const Anchors &anchors) noexcept;

    /**
     * @brief Forgets the anchors. Every later call of the game layer is a no-op that reports failure.
     * @note Teardown only, after the frame hook retired.
     */
    void unbind() noexcept;

    /** @brief The bound anchors, or nullptr before bind() and after unbind(). */
    [[nodiscard]] const Anchors *bound_anchors() noexcept;

    /**
     * @brief Reads the field at @p offset of @p object without a guard.
     * @details @p object must be live for the current frame: gEnv, a static of the image, or an object whose vtable
     *          passed its RTTI check.
     */
    template <class T> [[nodiscard]] T read(const void *object, std::ptrdiff_t offset) noexcept
    {
        return DMK::memory::unchecked::read<T>(DMK::Address{object}.offset(offset));
    }

    /**
     * @brief The vtable pointer of @p object, or 0 when the pointer is null, implausible or unreadable.
     * @details The read is guarded, so a stale or foreign pointer fails its RTTI check and never faults the game.
     */
    [[nodiscard]] inline std::uintptr_t vtable_of(const void *object) noexcept
    {
        const DMK::Address address{object};
        if (!DMK::memory::is_plausible_ptr(address))
        {
            return 0;
        }
        const auto vtable = DMK::memory::read<std::uintptr_t>(address);
        return vtable ? *vtable : 0;
    }

    /**
     * @brief Reads the field at @p offset of @p object with a guard, for objects the mod cannot check by RTTI.
     * @return The value, or std::nullopt when the pointer is implausible or the read faults.
     */
    template <class T> [[nodiscard]] std::optional<T> read_guarded(const void *object, std::ptrdiff_t offset) noexcept
    {
        const DMK::Address address{object};
        if (!DMK::memory::is_plausible_ptr(address))
        {
            return std::nullopt;
        }
        const auto value = DMK::memory::read<T>(address.offset(offset));
        return value ? std::optional<T>{*value} : std::nullopt;
    }

    /** @brief @p object when its vtable is @p vtable, a resolved RTTI vtable, or nullptr. */
    [[nodiscard]] inline void *checked(void *object, std::uintptr_t vtable) noexcept
    {
        return vtable != 0 && vtable_of(object) == vtable ? object : nullptr;
    }

    /** @brief Slot @p index of @p vtable, a vtable that passed its RTTI check and its slot pin. */
    [[nodiscard]] inline std::uintptr_t slot(std::uintptr_t vtable, std::size_t index) noexcept
    {
        return DMK::memory::unchecked::read<std::uintptr_t>(
            DMK::Address{vtable}.offset(static_cast<std::ptrdiff_t>(index * sizeof(std::uintptr_t)))
        );
    }
} // namespace HUDClock::game

#endif // HUDCLOCK_GAME_GAME_MEMORY_HPP
