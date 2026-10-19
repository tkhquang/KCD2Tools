/**
 * @file game_memory.hpp
 * @brief Guarded reads of game objects shared by the engine layer and the proxy-pass detour.
 */
#ifndef HOTPAK_GAME_MEMORY_HPP
#define HOTPAK_GAME_MEMORY_HPP

#include <DetourModKit.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace HotPak
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
        constexpr std::uintptr_t PAGE_SIZE = 0x1000;
        constexpr std::size_t CHUNK_SIZE = 64;
        if (address == 0 || out.empty())
        {
            return 0;
        }
        std::array<std::byte, CHUNK_SIZE> chunk{};
        std::size_t length = 0;
        while (length < out.size())
        {
            const std::uintptr_t cursor = address + length;
            const std::size_t to_page_end = static_cast<std::size_t>(PAGE_SIZE - (cursor & (PAGE_SIZE - 1)));
            const std::size_t span = std::min({CHUNK_SIZE, to_page_end, out.size() - length});
            if (!DMK::memory::read_into(DMK::Address{cursor}, std::span<std::byte>{chunk.data(), span}).has_value())
            {
                return 0;
            }
            for (std::size_t i = 0; i < span; ++i)
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

} // namespace HotPak

#endif // HOTPAK_GAME_MEMORY_HPP
