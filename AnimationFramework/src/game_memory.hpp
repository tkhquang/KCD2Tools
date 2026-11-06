/**
 * @file game_memory.hpp
 * @brief Guarded reads of game objects and the engine's virtual calls, shared by the hooks and the console bindings.
 */
#ifndef ANIMFW_GAME_MEMORY_HPP
#define ANIMFW_GAME_MEMORY_HPP

#include <DetourModKit.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace AnimationFramework
{
    /** @brief Reads a @p T at @p address through a guarded read, or std::nullopt when it faults. */
    template <class T> [[nodiscard]] std::optional<T> read_value(std::uintptr_t address) noexcept
    {
        const auto value = DMK::memory::read<T>(DMK::Address{address});
        if (!value.has_value())
        {
            return std::nullopt;
        }
        return *value;
    }

    /** @brief Reads a pointer at @p address, or 0 when it faults. */
    [[nodiscard]] inline std::uintptr_t read_ptr(std::uintptr_t address) noexcept
    {
        return read_value<std::uintptr_t>(address).value_or(0);
    }

    /**
     * @brief Copies the NUL-terminated string at @p address into @p out, allocation free.
     * @details Each guarded read stops at the next page boundary, so a short string that ends just before an unmapped
     *          page still reads: a read can fault only on a page the string itself reaches into.
     * @return The length, or 0 on a fault, an empty string, or a string that does not fit in @p out with its NUL.
     * @note Callback-safe.
     */
    [[nodiscard]] inline std::size_t read_c_string(std::uintptr_t address, std::span<char> out) noexcept
    {
        constexpr std::uintptr_t page_size = 0x1000;
        constexpr std::size_t chunk_size = 64;
        if (address == 0 || out.empty())
        {
            return 0;
        }
        std::array<std::byte, chunk_size> chunk{};
        std::size_t length = 0;
        while (length < out.size())
        {
            const std::uintptr_t cursor = address + length;
            const std::size_t to_page_end = static_cast<std::size_t>(page_size - (cursor & (page_size - 1)));
            const std::size_t step = std::min({chunk_size, to_page_end, out.size() - length});
            if (!DMK::memory::read_into(DMK::Address{cursor}, std::span<std::byte>{chunk.data(), step}).has_value())
            {
                return 0;
            }
            for (std::size_t i = 0; i < step; ++i)
            {
                const char c = static_cast<char>(chunk[i]);
                out[length] = c;
                if (c == '\0')
                {
                    return length;
                }
                ++length;
            }
        }
        return 0;
    }

    /**
     * @brief Calls slot @p slot of @p object's vtable with @p args after the `this` pointer.
     * @details The engine's virtuals use the x64 calling convention with `this` first. A result returned through a
     *          hidden out-pointer (an XmlNodeRef) is passed explicitly as the first argument after `this`.
     * @warning @p object must be a live engine object whose vtable holds the slot. The call is not guarded.
     */
    template <typename Result, typename... Args> Result vcall(void *object, std::size_t slot, Args... args)
    {
        using Function = Result(__fastcall *)(void *, Args...);
        const auto vtable = *static_cast<void ***>(object);
        return reinterpret_cast<Function>(vtable[slot])(object, args...);
    }
} // namespace AnimationFramework

#endif // ANIMFW_GAME_MEMORY_HPP
