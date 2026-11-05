/**
 * @file core/clock_text.cpp
 * @brief The clock text (see clock_text.hpp).
 */

#include "core/clock_text.hpp"
#include "strings.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <iterator>

namespace HUDClock::core
{
    namespace
    {
        constexpr std::string_view TIME_TOKEN = "{time}";
        constexpr std::string_view PERIOD_TOKEN = "{tod}";
        constexpr std::string_view DAY_TOKEN = "{day}";
        constexpr std::string_view REAL_TIME_TOKEN = "{realtime}";
        constexpr std::string_view REAL_DATE_TOKEN = "{realdate}";
        constexpr int HOURS_PER_DAY = 24;
        constexpr int HALF_DAY = 12;
        /// Room for a {realdate} layout's result. A longer one shows nothing.
        constexpr std::size_t REAL_DATE_CAPACITY = 128;

        /** @brief @p time as the computer's local date in @p layout, an strftime layout, or empty when it overflows. */
        [[nodiscard]] std::string format_real_date(const std::tm &time, const std::string &layout)
        {
            char buffer[REAL_DATE_CAPACITY]{};
            const std::size_t length = layout.empty() || !is_valid_date_layout(layout)
                                           ? 0
                                           : std::strftime(buffer, sizeof(buffer), layout.c_str(), &time);
            return std::string{buffer, length};
        }

        /** @brief The items of the comma-separated @p text, trimmed. Empty items stay, so a count check sees them. */
        [[nodiscard]] std::vector<std::string_view> split_list(std::string_view text)
        {
            std::vector<std::string_view> items;
            while (true)
            {
                const std::size_t comma = text.find(',');
                items.push_back(strings::trim(text.substr(0, comma)));
                if (comma == std::string_view::npos)
                {
                    return items;
                }
                text.remove_prefix(comma + 1);
            }
        }

        /** @brief @p text as a whole decimal number, or std::nullopt. */
        [[nodiscard]] std::optional<int> parse_int(std::string_view text) noexcept
        {
            int value = 0;
            const char *const end = text.data() + text.size();
            const auto [last, error] = std::from_chars(text.data(), end, value);
            if (text.empty() || error != std::errc{} || last != end)
            {
                return std::nullopt;
            }
            return value;
        }
    } // namespace

    ClockTime clock_time(std::int64_t world_ms) noexcept
    {
        // A negative time cannot come from the calendar, but the remainder stays in the day either way.
        const std::int64_t of_day = ((world_ms % MS_PER_DAY) + MS_PER_DAY) % MS_PER_DAY;
        return ClockTime{
            .hour = static_cast<int>(of_day / MS_PER_HOUR),
            .minute = static_cast<int>((of_day % MS_PER_HOUR) / MS_PER_MINUTE),
        };
    }

    std::optional<HourFormat> parse_hour_format(std::string_view text) noexcept
    {
        text = strings::trim(text);
        if (!text.empty() && strings::lower(text.back()) == 'h')
        {
            text.remove_suffix(1);
        }
        if (text == "24")
        {
            return HourFormat::H24;
        }
        if (text == "12")
        {
            return HourFormat::H12;
        }
        return std::nullopt;
    }

    std::optional<AmPm> parse_am_pm(std::string_view text)
    {
        const std::vector<std::string_view> items = split_list(text);
        if (items.size() != 2)
        {
            return std::nullopt;
        }
        return AmPm{.am = std::string{items[0]}, .pm = std::string{items[1]}};
    }

    ParsedPeriods parse_periods(std::string_view text)
    {
        ParsedPeriods parsed;
        if (strings::trim(text).empty())
        {
            return parsed;
        }
        for (const std::string_view item : split_list(text))
        {
            const std::size_t colon = item.find(':');
            const std::optional<int> hour =
                colon == std::string_view::npos ? std::nullopt : parse_int(strings::trim(item.substr(0, colon)));
            const std::string_view name =
                colon == std::string_view::npos ? std::string_view{} : strings::trim(item.substr(colon + 1));
            const bool taken =
                hour && std::ranges::any_of(parsed.periods, [&](const Period &p) { return p.start_hour == *hour; });
            if (!hour || *hour < 0 || *hour >= HOURS_PER_DAY || name.empty() || taken)
            {
                ++parsed.dropped;
                continue;
            }
            parsed.periods.push_back(Period{.start_hour = *hour, .name = std::string{name}});
        }
        std::ranges::sort(parsed.periods, {}, &Period::start_hour);
        return parsed;
    }

    std::string_view period_name(const std::vector<Period> &periods, int hour) noexcept
    {
        if (periods.empty())
        {
            return {};
        }
        // The last period that started at or before the hour. None started yet: yesterday's last one still runs.
        const auto after = std::ranges::upper_bound(periods, hour, {}, &Period::start_hour);
        return after == periods.begin() ? periods.back().name : std::prev(after)->name;
    }

    std::string format_time(ClockTime time, HourFormat format, const AmPm &am_pm)
    {
        char buffer[8]{};
        if (format == HourFormat::H24)
        {
            (void)std::snprintf(buffer, sizeof(buffer), "%02d:%02d", time.hour, time.minute);
            return buffer;
        }
        const int hour12 = time.hour % HALF_DAY == 0 ? HALF_DAY : time.hour % HALF_DAY;
        (void)std::snprintf(buffer, sizeof(buffer), "%d:%02d", hour12, time.minute);
        const std::string &suffix = time.hour < HALF_DAY ? am_pm.am : am_pm.pm;
        return suffix.empty() ? std::string{buffer} : std::string{buffer} + " " + suffix;
    }

    bool is_valid_date_layout(std::string_view layout) noexcept
    {
        constexpr std::string_view CODES = "aAbBcdDeFHIjmMpRSTUwWxXyYzZ%";
        for (std::size_t i = 0; i < layout.size(); ++i)
        {
            if (layout[i] != '%')
            {
                continue;
            }
            if (i + 1 == layout.size() || CODES.find(layout[i + 1]) == std::string_view::npos)
            {
                return false;
            }
            ++i;
        }
        return true;
    }

    bool uses_real_time(std::string_view format) noexcept
    {
        return format.find(REAL_TIME_TOKEN) != std::string_view::npos ||
               format.find(REAL_DATE_TOKEN) != std::string_view::npos;
    }

    std::string render(const TextSettings &settings, std::string_view format, const ClockMoment &moment)
    {
        std::string text;
        while (!format.empty())
        {
            if (format.starts_with(TIME_TOKEN))
            {
                text += format_time(moment.game, settings.hour_format, settings.am_pm);
                format.remove_prefix(TIME_TOKEN.size());
            }
            else if (format.starts_with(PERIOD_TOKEN))
            {
                text += period_name(settings.periods, moment.game.hour);
                format.remove_prefix(PERIOD_TOKEN.size());
            }
            else if (format.starts_with(DAY_TOKEN))
            {
                text += std::to_string(moment.day);
                format.remove_prefix(DAY_TOKEN.size());
            }
            else if (format.starts_with(REAL_TIME_TOKEN))
            {
                const ClockTime real{.hour = moment.real.tm_hour, .minute = moment.real.tm_min};
                text += format_time(real, settings.hour_format, settings.am_pm);
                format.remove_prefix(REAL_TIME_TOKEN.size());
            }
            else if (format.starts_with(REAL_DATE_TOKEN))
            {
                text += format_real_date(moment.real, settings.real_date_format);
                format.remove_prefix(REAL_DATE_TOKEN.size());
            }
            else
            {
                text += format.front();
                format.remove_prefix(1);
            }
        }
        return text;
    }
} // namespace HUDClock::core
