/**
 * @file core/layout.hpp
 * @brief The clock's parts (the dial and the text lines) and how each one attaches to the screen or to another part.
 *
 * A part sits with one of its nine points (its pivot) on one of the nine points of its target, the screen or another
 * part, then moves by its offset. The movie measures the parts and does the arithmetic. This file names the parts and
 * keeps the attachments free of loops.
 *
 * Plain C++ with no game, Windows or DetourModKit dependency, so its tests run without the game.
 */
#ifndef HUDCLOCK_CORE_LAYOUT_HPP
#define HUDCLOCK_CORE_LAYOUT_HPP

#include "core/clock_style.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace HUDClock::core
{
    /// The text lines. Line N is part N.
    inline constexpr std::size_t LINE_COUNT = 4;
    /// The dial (part 0) and the lines.
    inline constexpr std::size_t PART_COUNT = LINE_COUNT + 1;
    /// The part number of the dial.
    inline constexpr int DIAL_PART = 0;
    /// The target number of the screen.
    inline constexpr int SCREEN_TARGET = -1;

    /** @brief The INI name of part or target @p part: Dial, Line1 to Line4, or Screen for SCREEN_TARGET. */
    [[nodiscard]] std::string_view part_name(int part) noexcept;

    /** @brief The target that @p text names (Screen, Dial, Line1 to Line4), in any case, or std::nullopt. */
    [[nodiscard]] std::optional<int> parse_target(std::string_view text) noexcept;

    /** @brief Where one part sits. */
    struct Placement
    {
        /// SCREEN_TARGET, or the part it attaches to.
        int target{SCREEN_TARGET};
        /// The target's point it attaches to.
        Anchor point{Anchor::TopLeft};
        /// Its own point that sits on the target's point.
        Anchor pivot{Anchor::TopLeft};
        /// Moves right, in pixels of a 1080p screen.
        float offset_x{0.0f};
        /// Moves down, in pixels of a 1080p screen.
        float offset_y{0.0f};

        friend bool operator==(const Placement &, const Placement &) = default;
    };

    /** @brief The attachments with every loop broken, and the parts whose attachment was broken. */
    struct ResolvedTargets
    {
        std::array<int, PART_COUNT> targets{};
        /// Bit N set: part N attached in a loop (or to itself) and now attaches to the screen.
        unsigned broken{0};
    };

    /**
     * @brief Breaks every attachment loop of @p targets by attaching the part that closes it to the screen.
     * @details A target outside the parts also becomes the screen.
     */
    [[nodiscard]] ResolvedTargets resolve_targets(const std::array<int, PART_COUNT> &targets) noexcept;
} // namespace HUDClock::core

#endif // HUDCLOCK_CORE_LAYOUT_HPP
