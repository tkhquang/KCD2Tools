/**
 * @file tests/core_tests.cpp
 * @brief The clock text, the INI value parsers, the visibility rule and the key hold.
 */

#include "check.hpp"
#include "core/clock_style.hpp"
#include "core/clock_text.hpp"
#include "core/key_press.hpp"
#include "core/layout.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>

using namespace HUDClock::core;

namespace
{
    /// The INI's default periods, the table of the Show Time II mod.
    constexpr const char *DEFAULT_PERIODS =
        "0: Midnight, 1: Late night, 5: Dawn, 6: Early morning, 9: Late morning, 12: Noon, 13: Early afternoon, "
        "15: Late afternoon, 17: Early evening, 19: Late evening, 21: Dusk, 22: Early night";

    void clock_time_tests()
    {
        // The live calendar read 12910879857 ms on day 149, 10.356 hours into it.
        CHECK((clock_time(12'910'879'857) == ClockTime{.hour = 10, .minute = 21}));
        CHECK((clock_time(0) == ClockTime{.hour = 0, .minute = 0}));
        CHECK((clock_time(MS_PER_DAY - 1) == ClockTime{.hour = 23, .minute = 59}));
        CHECK(
            (clock_time(5 * MS_PER_DAY + 13 * MS_PER_HOUR + 7 * MS_PER_MINUTE) == ClockTime{.hour = 13, .minute = 7})
        );
        CHECK((clock_time(-MS_PER_MINUTE) == ClockTime{.hour = 23, .minute = 59}));
    }

    void format_tests()
    {
        const AmPm am_pm{};
        CHECK(format_time({.hour = 9, .minute = 5}, HourFormat::H24, am_pm) == "09:05");
        CHECK(format_time({.hour = 23, .minute = 59}, HourFormat::H24, am_pm) == "23:59");
        CHECK(format_time({.hour = 0, .minute = 0}, HourFormat::H12, am_pm) == "12:00 AM");
        CHECK(format_time({.hour = 9, .minute = 5}, HourFormat::H12, am_pm) == "9:05 AM");
        CHECK(format_time({.hour = 12, .minute = 30}, HourFormat::H12, am_pm) == "12:30 PM");
        CHECK(format_time({.hour = 18, .minute = 31}, HourFormat::H12, am_pm) == "6:31 PM");
        CHECK(format_time({.hour = 18, .minute = 31}, HourFormat::H12, AmPm{.am = "", .pm = ""}) == "6:31");
        CHECK(parse_hour_format("24") == HourFormat::H24);
        CHECK(parse_hour_format(" 12h ") == HourFormat::H12);
        CHECK(parse_hour_format("12H") == HourFormat::H12);
        CHECK(!parse_hour_format("13"));
        CHECK(!parse_hour_format(""));
        const auto custom = parse_am_pm(" am , pm ");
        CHECK(custom && custom->am == "am" && custom->pm == "pm");
        CHECK(!parse_am_pm("AM"));
        CHECK(!parse_am_pm("a, b, c"));
    }

    void period_tests()
    {
        const ParsedPeriods parsed = parse_periods(DEFAULT_PERIODS);
        CHECK(parsed.dropped == 0);
        CHECK(parsed.periods.size() == 12);
        CHECK(period_name(parsed.periods, 0) == "Midnight");
        CHECK(period_name(parsed.periods, 4) == "Late night");
        CHECK(period_name(parsed.periods, 5) == "Dawn");
        CHECK(period_name(parsed.periods, 8) == "Early morning");
        CHECK(period_name(parsed.periods, 12) == "Noon");
        CHECK(period_name(parsed.periods, 21) == "Dusk");
        CHECK(period_name(parsed.periods, 23) == "Early night");

        // Out of order, with the first start after midnight: the hours before it belong to the last period.
        const ParsedPeriods shuffled = parse_periods("20: Night, 6: Day");
        CHECK(shuffled.periods.size() == 2 && shuffled.periods.front().start_hour == 6);
        CHECK(period_name(shuffled.periods, 3) == "Night");
        CHECK(period_name(shuffled.periods, 6) == "Day");
        CHECK(period_name(shuffled.periods, 22) == "Night");

        const ParsedPeriods bad = parse_periods("24: Late, x: Bad, 3, 4:, 5: Dawn, 5: Again, 6:Day: and more");
        CHECK(bad.dropped == 5);
        CHECK(bad.periods.size() == 2);
        CHECK(period_name(bad.periods, 7) == "Day: and more");
        CHECK(parse_periods("  ").periods.empty() && parse_periods("").dropped == 0);
        CHECK(period_name({}, 5).empty());
    }

    void render_tests()
    {
        TextSettings settings{.periods = parse_periods(DEFAULT_PERIODS).periods};
        std::tm real{};
        real.tm_year = 2026 - 1900;
        real.tm_mon = 9;
        real.tm_mday = 27;
        real.tm_hour = 9;
        real.tm_min = 5;
        const ClockMoment evening{.game = {.hour = 18, .minute = 31}, .day = 149, .real = real};
        CHECK(render(settings, "{time}", evening) == "18:31");
        CHECK(render(settings, "{tod}", evening) == "Early evening");
        settings.hour_format = HourFormat::H12;
        CHECK(render(settings, "{time} - {tod}", evening) == "6:31 PM - Early evening");
        CHECK(render(settings, "Hour {time} {unknown} {tod", evening) == "Hour 6:31 PM {unknown} {tod");
        CHECK(render(settings, "", evening).empty());

        const char *const all = "Day {day}, {time} ({realtime} {realdate})";
        CHECK(render(settings, all, evening) == "Day 149, 6:31 PM (9:05 AM 2026-10-27)");
        settings.hour_format = HourFormat::H24;
        settings.real_date_format = "%d.%m.%Y";
        CHECK(render(settings, all, evening) == "Day 149, 18:31 (09:05 27.10.2026)");
        // A code the CRT does not know would end the process, so the date stays empty instead.
        settings.real_date_format = "%Q";
        CHECK(render(settings, all, evening) == "Day 149, 18:31 (09:05 )");
        CHECK(world_day(12'910'879'857) == 149);
        CHECK(uses_real_time("{realdate}") && uses_real_time("a {realtime}") && !uses_real_time("{time} {day}"));
        CHECK(is_valid_date_layout("%Y-%m-%d %A %%") && is_valid_date_layout("plain"));
        CHECK(!is_valid_date_layout("%Q") && !is_valid_date_layout("%") && !is_valid_date_layout("%Ey"));
    }

    void layout_tests()
    {
        CHECK(parse_target("screen") == SCREEN_TARGET);
        CHECK(parse_target(" Dial ") == DIAL_PART);
        CHECK(parse_target("line3") == 3);
        CHECK(!parse_target("Line5") && !parse_target(""));
        CHECK(part_name(DIAL_PART) == "Dial" && part_name(4) == "Line4" && part_name(SCREEN_TARGET) == "Screen");

        // The default chain: Line1 on the dial, each later line under the one before. No loop.
        const ResolvedTargets chain = resolve_targets({SCREEN_TARGET, 0, 1, 2, 3});
        CHECK(chain.broken == 0);
        CHECK((chain.targets == std::array<int, PART_COUNT>{SCREEN_TARGET, 0, 1, 2, 3}));

        // The dial on Line1 and Line1 on the dial: walking from the dial, Line1 closes the loop.
        const ResolvedTargets pair = resolve_targets({1, 0, 1, 2, 3});
        CHECK(pair.broken == 2u);
        CHECK(pair.targets[0] == 1 && pair.targets[1] == SCREEN_TARGET);

        // A part on itself, and a target outside the parts.
        const ResolvedTargets self = resolve_targets({SCREEN_TARGET, 1, 9, 2, 3});
        CHECK(self.broken == 2u);
        CHECK(self.targets[1] == SCREEN_TARGET && self.targets[2] == SCREEN_TARGET);

        // A three-part loop breaks once, and every chain then ends on the screen.
        const ResolvedTargets ring = resolve_targets({SCREEN_TARGET, 3, 1, 2, SCREEN_TARGET});
        CHECK(ring.broken == 1u << 2);
        for (std::size_t start = 0; start < PART_COUNT; ++start)
        {
            int part = static_cast<int>(start);
            int steps = 0;
            while (part != SCREEN_TARGET && steps <= static_cast<int>(PART_COUNT))
            {
                part = ring.targets[static_cast<std::size_t>(part)];
                ++steps;
            }
            CHECK(part == SCREEN_TARGET);
        }
    }

    void style_tests()
    {
        CHECK(parse_anchor("topleft") == Anchor::TopLeft);
        CHECK(parse_anchor(" BottomRight ") == Anchor::BottomRight);
        CHECK(parse_anchor("Center") == Anchor::Center);
        CHECK(!parse_anchor("Middle"));
        CHECK(anchor_name(Anchor::Right) == "Right");
        CHECK(parse_font("bold") == Font::Bold);
        CHECK(parse_font("Manuscript") == Font::Manuscript);
        CHECK(!parse_font("Arial"));
        CHECK(font_name(Font::Display) == "Display");
        CHECK(parse_color("E8DCC4") == 0xE8DCC4u);
        CHECK(parse_color("#ffffff") == 0xFFFFFFu);
        CHECK(!parse_color("FFF"));
        CHECK(!parse_color("GG0000"));
        CHECK(!parse_color("1234567"));

        const ParsedNames parts = parse_hud_parts("Compass, stats, , Nope");
        CHECK(parts.mask == 0x3u);
        CHECK(parts.dropped == 1);
        CHECK(parse_hud_parts("Vignette").mask == (1u << 27));
        // The game's mask in the pause menu (GameLog only) and in play.
        CHECK((0x40u & parse_hud_parts("Compass, Stats").mask) == 0);
        CHECK((0x12FFCFFu & parse_hud_parts("Compass, Stats").mask) != 0);
        CHECK(parse_hud_parts("").mask == 0 && parse_hud_parts("").dropped == 0);

        const ParsedNames games = parse_minigames("alchemy, Dice, Chess");
        CHECK(games.mask == 0x5u);
        CHECK(games.dropped == 1);
        CHECK(parse_minigames("Alchemy, Blacksmithing, Dice, Reading").mask == 0xFu);
        CHECK(parse_minigames("HoleDigging").mask == (1u << 7));
        CHECK(parse_minigames("").mask == 0 && parse_minigames("").dropped == 0);
    }

    void visibility_tests()
    {
        // The game's HUD masks in play, in the pause menu, and over a running minigame: its own screen, its ingredient
        // list and its help pages.
        constexpr std::uint32_t PLAY = 0x12FFCFF;
        constexpr std::uint32_t PAUSE = 0x40;
        constexpr std::uint32_t ALCHEMY_BENCH = 0x10BECF0;
        constexpr std::uint32_t ALCHEMY_INGREDIENTS = 0x10000F0;
        constexpr std::uint32_t ALCHEMY_HELP = 0x2040;
        constexpr std::uint32_t DICE_TABLE = 0x519E8F0;
        constexpr std::uint32_t FOLLOW = 0x3;
        constexpr std::uint32_t ALCHEMY = 0x1;
        constexpr std::uint32_t DICE = 0x4;
        constexpr std::uint32_t SHOW_IN = 0xF;

        CHECK(hud_allows_clock(PLAY, FOLLOW, 0, SHOW_IN));
        CHECK(!hud_allows_clock(PAUSE, FOLLOW, 0, SHOW_IN));
        // A minigame screen hides Compass and Stats, so only the minigame rule shows the clock there.
        CHECK(!hud_allows_clock(ALCHEMY_BENCH, FOLLOW, 0, SHOW_IN));
        CHECK(hud_allows_clock(ALCHEMY_BENCH, FOLLOW, ALCHEMY, SHOW_IN));
        CHECK(hud_allows_clock(DICE_TABLE, FOLLOW, DICE, SHOW_IN));
        CHECK(!hud_allows_clock(ALCHEMY_BENCH, FOLLOW, ALCHEMY, DICE));
        // A menu over the minigame hides the clock while the minigame still runs.
        CHECK(!hud_allows_clock(ALCHEMY_INGREDIENTS, FOLLOW, ALCHEMY, SHOW_IN));
        CHECK(!hud_allows_clock(ALCHEMY_HELP, FOLLOW, ALCHEMY, SHOW_IN));
        CHECK(!hud_allows_clock(PAUSE, FOLLOW, ALCHEMY, SHOW_IN));
    }

    void key_press_tests()
    {
        constexpr std::int64_t HOLD = 1000;
        // No hold time acts on the press, even when the key is already up again.
        CHECK(step_press(500, 500, 0) == PressStep::Act);
        CHECK(step_press(0, 500, 0) == PressStep::Act);
        // A held key waits for its hold time, then acts.
        CHECK(step_press(500, 900, HOLD) == PressStep::Wait);
        CHECK(step_press(500, 1500, HOLD) == PressStep::Act);
        CHECK(step_press(500, 1600, HOLD) == PressStep::Act);
        // A key that came up before its hold time spends the press.
        CHECK(step_press(0, 900, HOLD) == PressStep::Spend);
    }
} // namespace

int main()
{
    clock_time_tests();
    format_tests();
    period_tests();
    render_tests();
    layout_tests();
    style_tests();
    visibility_tests();
    key_press_tests();
    return HUDClock::tests::finish("core");
}
