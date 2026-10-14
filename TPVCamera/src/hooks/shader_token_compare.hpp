/** @file hooks/shader_token_compare.hpp
 *  @brief Bounded comparison of native CHWShader token maps, without allocation or hashing proofs.
 */
#ifndef TPVCAMERA_HOOKS_SHADER_TOKEN_COMPARE_HPP
#define TPVCAMERA_HOOKS_SHADER_TOKEN_COMPARE_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace TPVCamera::ShaderTokens
{
    // Retail mfStoreCacheTokenMap: uint32 count, count uint32 tokens, then sorted (uint32 id, ASCIIZ) entries.
    // Unknown/larger layouts keep the native twin. These bounds limit work as well as memory reads.
    inline constexpr std::size_t k_max_bytes = 256 * 1024;
    inline constexpr std::size_t k_max_tokens = 32768;

    /// The little-endian uint32 at p, which need not be aligned.
    [[nodiscard]] inline std::uint32_t word(const std::uint8_t *p) noexcept
    {
        std::uint32_t value;
        std::memcpy(&value, p, sizeof(value));
        return value;
    }

    /**
     * @brief A cursor over a token map's string table: the (uint32 id, ASCIIZ text) records after the tokens.
     * @details advance() moves to the next record. present is false past the last one; valid turns false for good at
     *          a record cut short, a text without its NUL, or an id that does not rise.
     */
    struct Table
    {
        std::span<const std::uint8_t> remaining;
        std::uint32_t id = 0;
        std::string_view text;
        bool present = false;
        bool valid = true;

        /// Reads the next record into id and text, or clears present at the end or on a malformed record.
        void advance() noexcept
        {
            const auto previous = id;
            const bool had_previous = present;
            present = false;
            if (remaining.empty())
            {
                return;
            }
            if (remaining.size() < 5)
            {
                valid = false;
                return;
            }
            id = word(remaining.data());
            const auto *start = reinterpret_cast<const char *>(remaining.data() + 4);
            const auto *end = static_cast<const char *>(std::memchr(start, 0, remaining.size() - 4));
            if (end == nullptr || (had_previous && id <= previous))
            {
                valid = false;
                return;
            }
            text = {start, static_cast<std::size_t>(end - start)};
            remaining = remaining.subspan(5 + text.size());
            present = true;
        }
    };

    /** @brief Exact token stream and referenced-string equality; unrelated FX strings may differ. */
    [[nodiscard]] inline bool equivalent(std::span<const std::uint8_t> stock,
                                         std::span<const std::uint8_t> twin) noexcept
    {
        if (stock.size() < 4 || twin.size() < 4 || stock.size() > k_max_bytes || twin.size() > k_max_bytes)
        {
            return false;
        }
        const auto count = word(stock.data());
        if (count == 0 || count > k_max_tokens || count != word(twin.data()))
        {
            return false;
        }
        const std::size_t token_bytes = count * sizeof(std::uint32_t);
        if (stock.size() < 4 + token_bytes || twin.size() < 4 + token_bytes ||
            std::memcmp(stock.data() + 4, twin.data() + 4, token_bytes) != 0)
        {
            return false;
        }
        Table a{stock.subspan(4 + token_bytes)}, b{twin.subspan(4 + token_bytes)};
        a.advance();
        b.advance();
        std::array<std::uint32_t, 64> different{};
        std::size_t differences = 0;
        while (a.valid && b.valid && (a.present || b.present))
        {
            const bool take_a = a.present && (!b.present || a.id <= b.id);
            const bool take_b = b.present && (!a.present || b.id <= a.id);
            if (!(take_a && take_b && a.text == b.text))
            {
                if (differences == different.size())
                {
                    return false;
                }
                different[differences++] = take_a ? a.id : b.id;
            }
            if (take_a)
            {
                a.advance();
            }
            if (take_b)
            {
                b.advance();
            }
        }
        if (!a.valid || !b.valid)
        {
            return false;
        }
        // Keyword tokens need no table entry. Every differing/missing string must be unused by the program.
        const auto end = different.begin() + static_cast<std::ptrdiff_t>(differences);
        if (differences == 0)
        {
            return true;
        }
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto token = word(stock.data() + 4 + i * 4);
            if (token >= different[0] && token <= different[differences - 1] &&
                std::binary_search(different.begin(), end, token))
            {
                return false;
            }
        }
        return true;
    }
} // namespace TPVCamera::ShaderTokens

#endif // TPVCAMERA_HOOKS_SHADER_TOKEN_COMPARE_HPP
