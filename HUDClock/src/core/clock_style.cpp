/**
 * @file core/clock_style.cpp
 * @brief The INI names of the clock's place, font, color, HUD parts and minigames (see clock_style.hpp).
 */

#include "core/clock_style.hpp"
#include "strings.hpp"

#include <charconv>

namespace HUDClock::core
{
    namespace
    {
        constexpr std::array<std::string_view, 9> ANCHOR_NAMES = {
            "TopLeft",
            "Top",
            "TopRight",
            "Left",
            "Center",
            "Right",
            "BottomLeft",
            "Bottom",
            "BottomRight",
        };
        constexpr std::array<std::string_view, 5> FONT_NAMES = {"Regular", "Bold", "Light", "Manuscript", "Display"};
        constexpr std::size_t COLOR_DIGITS = 6;

        /** @brief The index of @p text in @p names, in any case, or std::nullopt. */
        template <std::size_t N>
        [[nodiscard]] std::optional<std::size_t>
        index_of(const std::array<std::string_view, N> &names, std::string_view text) noexcept
        {
            text = strings::trim(text);
            for (std::size_t i = 0; i < N; ++i)
            {
                if (strings::equals_ignoring_case(text, names[i]))
                {
                    return i;
                }
            }
            return std::nullopt;
        }

        /** @brief Parses a comma-separated list of @p names, in any case, as a mask of bit N for name N. */
        template <std::size_t N>
        [[nodiscard]] ParsedNames
        parse_names(const std::array<std::string_view, N> &names, std::string_view text) noexcept
        {
            static_assert(N <= 32, "the mask holds 32 names");
            ParsedNames parsed;
            while (true)
            {
                const std::size_t comma = text.find(',');
                const std::string_view item = strings::trim(text.substr(0, comma));
                if (!item.empty())
                {
                    const std::optional<std::size_t> bit = index_of(names, item);
                    if (bit)
                    {
                        parsed.mask |= std::uint32_t{1} << *bit;
                    }
                    else
                    {
                        ++parsed.dropped;
                    }
                }
                if (comma == std::string_view::npos)
                {
                    return parsed;
                }
                text.remove_prefix(comma + 1);
            }
        }
    } // namespace

    std::string_view anchor_name(Anchor anchor) noexcept
    {
        const auto index = static_cast<std::size_t>(anchor);
        return index < ANCHOR_NAMES.size() ? ANCHOR_NAMES[index] : ANCHOR_NAMES[0];
    }

    std::optional<Anchor> parse_anchor(std::string_view text) noexcept
    {
        const std::optional<std::size_t> index = index_of(ANCHOR_NAMES, text);
        return index ? std::optional{static_cast<Anchor>(*index)} : std::nullopt;
    }

    std::string_view font_name(Font font) noexcept
    {
        const auto index = static_cast<std::size_t>(font);
        return index < FONT_NAMES.size() ? FONT_NAMES[index] : FONT_NAMES[0];
    }

    std::optional<Font> parse_font(std::string_view text) noexcept
    {
        const std::optional<std::size_t> index = index_of(FONT_NAMES, text);
        return index ? std::optional{static_cast<Font>(*index)} : std::nullopt;
    }

    std::optional<std::uint32_t> parse_color(std::string_view text) noexcept
    {
        text = strings::trim(text);
        if (text.starts_with('#'))
        {
            text.remove_prefix(1);
        }
        std::uint32_t value = 0;
        const char *const end = text.data() + text.size();
        const auto [last, error] = std::from_chars(text.data(), end, value, 16);
        if (text.size() != COLOR_DIGITS || error != std::errc{} || last != end)
        {
            return std::nullopt;
        }
        return value;
    }

    ParsedNames parse_hud_parts(std::string_view text) noexcept
    {
        return parse_names(HUD_PARTS, text);
    }

    ParsedNames parse_minigames(std::string_view text) noexcept
    {
        return parse_names(MINIGAMES, text);
    }
} // namespace HUDClock::core
