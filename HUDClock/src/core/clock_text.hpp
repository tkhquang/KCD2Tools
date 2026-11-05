/**
 * @file core/clock_text.hpp
 * @brief The clock's text: the game time as a clock or as a period of the day, laid out by the INI's Format.
 *
 * Plain C++ with no game, Windows or DetourModKit dependency, so its tests run without the game.
 */
#ifndef HUDCLOCK_CORE_CLOCK_TEXT_HPP
#define HUDCLOCK_CORE_CLOCK_TEXT_HPP

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace HUDClock::core
{
    /// The game's world time runs in milliseconds.
    inline constexpr std::int64_t MS_PER_MINUTE = 60'000;
    inline constexpr std::int64_t MS_PER_HOUR = 60 * MS_PER_MINUTE;
    inline constexpr std::int64_t MS_PER_DAY = 24 * MS_PER_HOUR;

    /** @brief An hour and a minute of the game day. */
    struct ClockTime
    {
        /// 0 to 23.
        int hour{0};
        /// 0 to 59.
        int minute{0};

        friend bool operator==(const ClockTime &, const ClockTime &) = default;
    };

    /** @brief The time of day of the world time @p world_ms, the calendar's milliseconds since its start. */
    [[nodiscard]] ClockTime clock_time(std::int64_t world_ms) noexcept;

    /** @brief How {time} reads. */
    enum class HourFormat : std::uint8_t
    {
        /// 00:00 to 23:59.
        H24,
        /// 12:00 AM to 11:59 PM.
        H12,
    };

    /** @brief The HourFormat that @p text names (24 or 12, an optional "h"), or std::nullopt. */
    [[nodiscard]] std::optional<HourFormat> parse_hour_format(std::string_view text) noexcept;

    /** @brief The words after a 12-hour time. */
    struct AmPm
    {
        std::string am{"AM"};
        std::string pm{"PM"};
    };

    /** @brief @p text as "<morning>, <afternoon>", or std::nullopt when it does not hold two items. */
    [[nodiscard]] std::optional<AmPm> parse_am_pm(std::string_view text);

    /** @brief A named period of the day that runs from its start hour to the next period's start. */
    struct Period
    {
        int start_hour{0};
        std::string name;
    };

    /** @brief The periods parsed from the INI, sorted by start hour, and the items that did not parse. */
    struct ParsedPeriods
    {
        std::vector<Period> periods;
        std::size_t dropped{0};
    };

    /**
     * @brief Parses "<hour>: <name>, <hour>: <name>, ...".
     * @details An item without a colon, with an hour outside 0 to 23, with an empty name, or with an hour that an
     *          earlier item already took counts as dropped.
     */
    [[nodiscard]] ParsedPeriods parse_periods(std::string_view text);

    /**
     * @brief The name of the period that holds @p hour.
     * @details Before the first start hour, the day's last period still runs from the evening before.
     * @return The name, or an empty view when @p periods is empty.
     */
    [[nodiscard]] std::string_view period_name(const std::vector<Period> &periods, int hour) noexcept;

    /** @brief @p time as 24-hour "HH:MM", or as 12-hour "H:MM <am/pm>". */
    [[nodiscard]] std::string format_time(ClockTime time, HourFormat format, const AmPm &am_pm);

    /// The game day of the world time @p world_ms, as Calendar.GetWorldDay() counts it.
    [[nodiscard]] constexpr std::int64_t world_day(std::int64_t world_ms) noexcept
    {
        return world_ms / MS_PER_DAY;
    }

    /** @brief How the tokens of every line read. */
    struct TextSettings
    {
        HourFormat hour_format{HourFormat::H24};
        AmPm am_pm{};
        std::vector<Period> periods;
        /// The strftime layout of {realdate}.
        std::string real_date_format{"%Y-%m-%d"};
    };

    /** @brief The moment the clock shows: the game time and the computer's local time. */
    struct ClockMoment
    {
        ClockTime game{};
        /// The game day, world_day().
        std::int64_t day{0};
        /// The computer's local time, as localtime_s fills it.
        std::tm real{};
    };

    /**
     * @brief True when every % code of @p layout is one strftime knows: a A b B c d D e F H I j m M p R S T U w W x X y
     *        Y z Z, or %%.
     * @details The Windows CRT ends the process on any other code, so a layout from the INI is checked before use.
     */
    [[nodiscard]] bool is_valid_date_layout(std::string_view layout) noexcept;

    /** @brief True when @p format holds a token that reads the computer's clock ({realtime} or {realdate}). */
    [[nodiscard]] bool uses_real_time(std::string_view format) noexcept;

    /**
     * @brief The text of the line layout @p format for @p moment under @p settings.
     * @details {time} is the game time and {tod} its period of the day, {day} the game day, {realtime} the computer's
     *          time in HourFormat, {realdate} the computer's date in RealDateFormat. Any other text shows as written.
     */
    [[nodiscard]] std::string render(const TextSettings &settings, std::string_view format, const ClockMoment &moment);
} // namespace HUDClock::core

#endif // HUDCLOCK_CORE_CLOCK_TEXT_HPP
