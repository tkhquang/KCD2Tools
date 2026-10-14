/** @file hooks/shader_token_diff.hpp
 *  @brief Lays a spliced token map out the way the engine stores one, and says how two token maps differ (the DEV
 *         twin clone's dry-run). Pure and bounded like shader_token_compare.hpp: no engine calls, no allocation, and
 *         the work is linear in maps that its bounds already cap.
 */
#ifndef TPVCAMERA_HOOKS_SHADER_TOKEN_DIFF_HPP
#define TPVCAMERA_HOOKS_SHADER_TOKEN_DIFF_HPP

#include "shader_token_compare.hpp"
#include "shader_token_splice.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace TPVCamera::ShaderTokenDiff
{
    /** @brief The bytes serialize() writes for a plan; 0 when the plan holds no map. */
    [[nodiscard]] inline std::size_t serialized_size(const ShaderSplice::Plan &plan) noexcept
    {
        if (plan.status != ShaderSplice::Status::Ok || plan.tokens == nullptr || plan.table == nullptr)
        {
            return 0;
        }
        std::size_t bytes = sizeof(std::uint32_t) + std::size_t{plan.tokens->count} * sizeof(std::uint32_t);
        for (const auto *entry = plan.table->first; entry != plan.table->last; ++entry)
        {
            bytes += sizeof(std::uint32_t) + std::string_view{entry->text}.size() + 1;
        }
        return bytes;
    }

    /** @brief Writes a plan's map as mfStoreCacheTokenMap stores one: the count, the tokens, then each string as its
     *         id and its text with a NUL.
     *  @return The bytes written, or 0 when the plan holds no map or out is smaller than serialized_size().
     */
    [[nodiscard]] inline std::size_t serialize(const ShaderSplice::Plan &plan, std::span<std::uint8_t> out) noexcept
    {
        const std::size_t bytes = serialized_size(plan);
        if (bytes == 0 || bytes > out.size())
        {
            return 0;
        }
        std::uint8_t *at = out.data();
        const std::uint32_t count = plan.tokens->count;
        std::memcpy(at, &count, sizeof(count));
        at += sizeof(count);
        std::memcpy(at, plan.tokens->elements, std::size_t{count} * sizeof(std::uint32_t));
        at += std::size_t{count} * sizeof(std::uint32_t);
        for (const auto *entry = plan.table->first; entry != plan.table->last; ++entry)
        {
            const std::string_view text{entry->text};
            std::memcpy(at, &entry->id, sizeof(entry->id));
            at += sizeof(entry->id);
            std::memcpy(at, text.data(), text.size());
            at += text.size();
            *at++ = 0;
        }
        return bytes;
    }

    /** @brief How two token maps differ, from the closest to the furthest apart. */
    enum class Kind : std::uint8_t
    {
        Equal,         // the same tokens, and every string they name has the same text (unused strings may differ)
        StringTable,   // the same tokens (the leading '#define' lines aside), but a string they name has other text
                       // or is in one table only
        HeaderDefines, // only the '#define' lines the stream opens with differ, and no string the rest names
        Tokens,        // the programs differ
        Malformed,     // a map is malformed or past shader_token_compare.hpp's bounds
    };

    /** @brief A kind for logs. */
    [[nodiscard]] constexpr std::string_view describe(Kind kind) noexcept
    {
        switch (kind)
        {
        case Kind::Equal:
            return "equal";
        case Kind::StringTable:
            return "the same tokens, a string they name differs or is in one table only";
        case Kind::HeaderDefines:
            return "only the leading #define lines differ";
        case Kind::Tokens:
            return "the tokens differ";
        case Kind::Malformed:
            return "a token map is malformed";
        }
        return "unknown";
    }

    /** @brief What classify() found. */
    struct Difference
    {
        Kind kind = Kind::Malformed;
        bool identical = false;     // Equal, and byte for byte
        std::uint32_t at = 0;       // Tokens: the first index that differs; StringTable: an id they name
        std::uint32_t expected = 0; // Tokens: the expected map's token there (0 past its end)
        std::uint32_t actual = 0;   // Tokens: the actual map's token there (0 past its end)
        std::uint32_t expected_count = 0;
        std::uint32_t actual_count = 0;
    };

    /** @brief The end of the '#define' lines a stream opens with: each runs through its 0 token (as SHData lays a
     *         '#define' fragment out, see ShaderSplice::fragment_end()).
     */
    [[nodiscard]] inline std::size_t header_end(const ShaderSplice::TokenMap &map) noexcept
    {
        std::size_t at = 0;
        while (at < map.count && ShaderSplice::is_define(ShaderTokens::word(map.tokens.data() + at * 4)))
        {
            ++at;
            while (at < map.count && ShaderTokens::word(map.tokens.data() + at * 4) != 0)
            {
                ++at;
            }
            ++at;
        }
        return std::min<std::size_t>(at, map.count);
    }

    /** @brief Whether a string differs: none, one the tokens name (its id), or a malformed table. */
    enum class Named : std::uint8_t
    {
        None,
        Found,
        Malformed,
    };

    /** @brief The id of a string the tokens of @p map from @p from on name among @p ids (ascending), or 0. Keyword
     *         ids never count: they need no table entry.
     */
    [[nodiscard]] inline std::uint32_t first_named(const ShaderSplice::TokenMap &map, std::size_t from,
                                                   std::span<const std::uint32_t> ids) noexcept
    {
        if (ids.empty())
        {
            return 0;
        }
        for (std::size_t i = from; i < map.count; ++i)
        {
            const std::uint32_t id = ShaderTokens::word(map.tokens.data() + i * 4);
            if (id >= shader_twin_blobs::k_splice_keyword_count && id >= ids.front() && id <= ids.back() &&
                std::binary_search(ids.begin(), ids.end(), id))
            {
                return id;
            }
        }
        return 0;
    }

    /** @brief Walks @p map's table and @p other in id order for a string whose text differs, or that one table has
     *         and the other does not, that the tokens of @p map from @p from on name.
     *  @details Exact for any number of differing strings: they are checked against the stream 256 at a time, so a
     *           long list of strings nothing names never reads as a named one. Each check is one pass over the stream,
     *           so the work stays bounded by shader_token_compare.hpp's bounds.
     */
    [[nodiscard]] inline Named named_difference(const ShaderSplice::TokenMap &map, std::span<const std::uint8_t> other,
                                                std::size_t from, std::uint32_t &id) noexcept
    {
        id = 0;
        std::array<std::uint32_t, 256> different{};
        std::size_t differences = 0;
        const auto check = [&]() noexcept
        {
            id = first_named(map, from, {different.data(), differences});
            differences = 0;
            return id != 0;
        };
        ShaderTokens::Table ta{map.table};
        ShaderTokens::Table tb{other};
        ta.advance();
        tb.advance();
        while (ta.valid && tb.valid && (ta.present || tb.present))
        {
            const bool take_a = ta.present && (!tb.present || ta.id <= tb.id);
            const bool take_b = tb.present && (!ta.present || tb.id <= ta.id);
            if (!(take_a && take_b && ta.text == tb.text))
            {
                different[differences++] = take_a ? ta.id : tb.id;
                if (differences == different.size() && check())
                {
                    return Named::Found;
                }
            }
            if (take_a)
            {
                ta.advance();
            }
            if (take_b)
            {
                tb.advance();
            }
        }
        if (!ta.valid || !tb.valid)
        {
            return Named::Malformed;
        }
        return check() ? Named::Found : Named::None;
    }

    /** @brief How @p actual differs from @p expected, each a token map in mfStoreCacheTokenMap's layout.
     *  @details The streams are compared token for token. Equal streams are Equal when every string they name has the
     *           same text in both tables (keyword ids need none), else StringTable with that string's id. Streams that
     *           are equal once each drops the '#define' lines it opens with are HeaderDefines when the strings the rest
     *           names match, else StringTable. Anything else is Tokens, with the first index that differs.
     */
    [[nodiscard]] inline Difference classify(std::span<const std::uint8_t> expected,
                                             std::span<const std::uint8_t> actual) noexcept
    {
        Difference result;
        ShaderSplice::TokenMap a;
        ShaderSplice::TokenMap b;
        if (!ShaderSplice::read_token_map(expected, a) || !ShaderSplice::read_token_map(actual, b))
        {
            return result;
        }
        result.expected_count = a.count;
        result.actual_count = b.count;
        const auto token = [](const ShaderSplice::TokenMap &map, std::size_t i) noexcept
        { return ShaderTokens::word(map.tokens.data() + i * 4); };
        // Streams equal from @p from on: the strings they name there decide between @p equal and StringTable.
        const auto strings = [&a, &b, &result](std::size_t from, Kind equal) noexcept
        {
            std::uint32_t id = 0;
            switch (named_difference(a, b.table, from, id))
            {
            case Named::None:
                result.kind = equal;
                break;
            case Named::Found:
                result.kind = Kind::StringTable;
                result.at = id;
                break;
            case Named::Malformed:
            default:
                result.kind = Kind::Malformed;
                break;
            }
        };

        if (a.count == b.count && std::memcmp(a.tokens.data(), b.tokens.data(), a.tokens.size()) == 0)
        {
            strings(0, Kind::Equal);
            result.identical = result.kind == Kind::Equal && expected.size() == actual.size() &&
                               std::memcmp(expected.data(), actual.data(), expected.size()) == 0;
            return result;
        }
        std::size_t first = 0;
        while (first < a.count && first < b.count && token(a, first) == token(b, first))
        {
            ++first;
        }
        result.at = static_cast<std::uint32_t>(first);
        result.expected = first < a.count ? token(a, first) : 0;
        result.actual = first < b.count ? token(b, first) : 0;
        const std::size_t ha = header_end(a);
        const std::size_t hb = header_end(b);
        if (a.count - ha == b.count - hb &&
            std::memcmp(a.tokens.data() + ha * 4, b.tokens.data() + hb * 4, (a.count - ha) * 4) == 0)
        {
            strings(ha, Kind::HeaderDefines);
            return result;
        }
        result.kind = Kind::Tokens;
        return result;
    }
} // namespace TPVCamera::ShaderTokenDiff

#endif // TPVCAMERA_HOOKS_SHADER_TOKEN_DIFF_HPP
