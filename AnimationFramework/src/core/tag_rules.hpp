/**
 * @file core/tag_rules.hpp
 * @brief The rules behind custom tags: the order the tags rank in, the tag combinations Henry's queries try, and when
 *        an answer found with them may replace the game's own.
 *
 * A custom tag (`thirdPerson`, or one a mod declares) never changes which option the game picks, only what it plays: a
 * query that carries custom tags is answered with an option only when that option carries every custom tag the query
 * was run with and covers every tag of the option the game picks without them. Several active tags are tried as
 * combinations, the most important first (rank()), so an option for two tags beats an option for one, and of two
 * options for different tags the higher-ranked tag's wins.
 */
#ifndef ANIMFW_CORE_TAG_RULES_HPP
#define ANIMFW_CORE_TAG_RULES_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace AnimationFramework::tag_rules
{
    /// Custom tags a game session can hold (the bits of an active mask).
    inline constexpr std::size_t MAX_CUSTOM_TAGS = 16;
    /// Active tags one query combines at most: n tags cost up to 2^n lookups, so lower-ranked ones beyond it wait.
    inline constexpr int MAX_ACTIVE_TAGS = 4;

    /**
     * @brief Gives every bit that differs between @p from and @p other its value in @p from, in @p live. The other bits
     *        of @p live stay, so what the game changed meanwhile is kept. Grouped tags share bits, which is why this
     *        works on bits and not on tag names. take_changed_bits(live, before, after) undoes what turned @p before
     *        into @p after, and take_changed_bits(live, after, before) does it again.
     */
    constexpr void take_changed_bits(
        std::span<std::uint8_t> live,
        std::span<const std::uint8_t> from,
        std::span<const std::uint8_t> other
    ) noexcept
    {
        const std::size_t size = live.size() < from.size() ? live.size() : from.size();
        for (std::size_t i = 0; i < size && i < other.size(); ++i)
        {
            const auto changed = static_cast<std::uint8_t>(from[i] ^ other[i]);
            live[i] = static_cast<std::uint8_t>((live[i] & ~changed) | (from[i] & changed));
        }
    }

    /**
     * @brief CRC-32 (IEEE, reflected) of @p text with 'A'..'Z' lowered, the hash Mannequin keys tag names by
     *        (CTagDefinition::Find).
     */
    [[nodiscard]] constexpr std::uint32_t mannequin_crc(std::string_view text) noexcept
    {
        std::uint32_t crc = 0xFFFFFFFFu;
        for (const char c : text)
        {
            const auto lowered = static_cast<std::uint8_t>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
            crc ^= lowered;
            for (int bit = 0; bit < 8; ++bit)
            {
                crc = (crc & 1u) != 0 ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
            }
        }
        return ~crc;
    }

    /// A play tag `animfw_option_<n>` asks for option n of the tag set the fragment's query matches.
    inline constexpr std::string_view OPTION_TAG_PREFIX = "animfw_option_";

    /**
     * @brief The option number of the first OPTION_TAG_PREFIX tag in the '+'-joined @p tag_list, or -1 for none (or a
     *        tag whose number is missing, not decimal or too large).
     */
    [[nodiscard]] constexpr std::int64_t requested_option(std::string_view tag_list) noexcept
    {
        constexpr std::int64_t max_option = 0xFFFF;
        for (std::size_t start = 0; start <= tag_list.size();)
        {
            std::size_t end = tag_list.find('+', start);
            end = end == std::string_view::npos ? tag_list.size() : end;
            const std::string_view tag = tag_list.substr(start, end - start);
            start = end + 1;
            if (!tag.starts_with(OPTION_TAG_PREFIX))
            {
                continue;
            }
            const std::string_view digits = tag.substr(OPTION_TAG_PREFIX.size());
            std::int64_t value = digits.empty() ? -1 : 0;
            for (const char c : digits)
            {
                if (c < '0' || c > '9' || value > max_option)
                {
                    return -1;
                }
                value = value * 10 + (c - '0');
            }
            return value > max_option ? -1 : value;
        }
        return -1;
    }

    /** @brief Whether the tag state @p view carries every bit of @p plain (tags and tag-group values alike). */
    [[nodiscard]] bool covers(std::span<const std::uint8_t> view, std::span<const std::uint8_t> plain) noexcept;

    /**
     * @brief @p active (bit i: tag i is on, a higher bit ranks higher) with only its MAX_ACTIVE_TAGS highest bits kept.
     */
    [[nodiscard]] std::uint32_t capped(std::uint32_t active) noexcept;

    /**
     * @brief The tag combination a query tries after @p current, 0 after the last. Start from @p active itself.
     * @details Numerically descending subsets of @p active: every combination holding the highest-ranked tag comes
     *          before any without it, and within those the most complete first. So `{a, b}` beats `{a}`, and `{a}`
     *          beats `{b, c}` when a outranks b and c.
     */
    [[nodiscard]] constexpr std::uint32_t next_subset(std::uint32_t active, std::uint32_t current) noexcept
    {
        return (current - 1u) & active;
    }

    /** @brief Names from a comma, semicolon or space separated list, lowered, blanks dropped, first occurrence kept. */
    [[nodiscard]] std::vector<std::string> parse_tag_list(std::string_view text);

    /** @brief A declared tag as the ranking sees it. */
    struct Declared
    {
        std::string name;
        std::int32_t priority = 0;
    };

    /**
     * @brief The enabled declarations in rising rank (the result's first index ranks lowest), as indexes into
     *        @p declared.
     * @details The INI's priority list ranks above every other tag, its first name highest. The rest follow by
     *          declared priority (higher first), then declaration order (earlier first). A name in @p disabled is left
     *          out. Names compare case-insensitively. A repeated declaration keeps its first occurrence.
     * @param ini_priority Lowercase names, as parse_tag_list returns them.
     * @param disabled Lowercase names, as parse_tag_list returns them.
     */
    [[nodiscard]] std::vector<std::size_t> rank(
        std::span<const Declared> declared,
        std::span<const std::string> ini_priority,
        std::span<const std::string> disabled
    );
} // namespace AnimationFramework::tag_rules

#endif // ANIMFW_CORE_TAG_RULES_HPP
