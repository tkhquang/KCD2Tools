/**
 * @file config.cpp
 * @brief Configuration registration through DMK::config.
 *
 * The scalar keys store into atomics. The text keys (each line's Format, AmPm, Periods, RealDateFormat) store into one
 * struct under a mutex, because they are not atomic. publish_settings() bumps a generation after each load, so the
 * frame copies the settings only after a change.
 */

#include "config.hpp"
#include "constants.hpp"
#include "strings.hpp"

#include <DetourModKit.hpp>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace HUDClock
{
    namespace
    {
        constexpr core::HourFormat DEFAULT_HOUR_FORMAT = core::HourFormat::H24;
        constexpr std::string_view DEFAULT_AM_PM = "AM, PM";
        /// The table of the Show Time II mod, which names each part of the day as Henry would.
        constexpr std::string_view DEFAULT_PERIODS =
            "0: Midnight, 1: Late night, 5: Dawn, 6: Early morning, 9: Late morning, 12: Noon, 13: Early afternoon, "
            "15: Late afternoon, 17: Early evening, 19: Late evening, 21: Dusk, 22: Early night";
        constexpr std::string_view DEFAULT_REAL_DATE_FORMAT = "%Y-%m-%d";
        constexpr core::Font DEFAULT_FONT = core::Font::Regular;
        constexpr std::string_view DEFAULT_COLOR = "E8DCC4";
        constexpr std::uint32_t DEFAULT_COLOR_VALUE = 0xE8DCC4;
        constexpr std::string_view DEFAULT_FOLLOW_HUD = "Compass, Stats";
        /// Compass and Stats, the parts DEFAULT_FOLLOW_HUD names.
        constexpr std::uint32_t DEFAULT_FOLLOW_MASK = 0x3;
        constexpr std::string_view DEFAULT_SHOW_IN_MINIGAMES = "Alchemy, Blacksmithing, Dice, Reading";
        /// Alchemy, Blacksmithing, Dice and Reading, the minigames DEFAULT_SHOW_IN_MINIGAMES names.
        constexpr std::uint32_t DEFAULT_MINIGAME_MASK = 0xF;

        constexpr float MAX_OFFSET = 4000.0f;
        constexpr float MIN_FONT_SIZE = 6.0f;
        constexpr float MAX_FONT_SIZE = 200.0f;
        constexpr float MAX_OPACITY = 100.0f;
        constexpr float MIN_DIAL_SIZE = 8.0f;
        constexpr float MAX_DIAL_SIZE = 400.0f;
        constexpr float MIN_SHOW_SECONDS = 0.5f;
        constexpr float MAX_SHOW_SECONDS = 600.0f;
        constexpr float MAX_KEY_HOLD_SECONDS = 10.0f;
        constexpr int MAX_SHOW_EVERY_HOURS = 24;

        /// Where each part sits by default: the dial at the top-left corner, Line1 centred under it (on the dial's
        /// place while the dial is hidden), and each later line centred under the one before.
        constexpr core::Placement DEFAULT_DIAL_PLACE{
            .target = core::SCREEN_TARGET,
            .point = core::Anchor::TopLeft,
            .pivot = core::Anchor::Left,
            .offset_x = 40.0f,
            .offset_y = 48.0f,
        };
        constexpr core::Placement DEFAULT_LINE1_PLACE{
            .target = core::DIAL_PART,
            .point = core::Anchor::Bottom,
            .pivot = core::Anchor::Top,
            .offset_x = 0.0f,
            .offset_y = 4.0f,
        };
        [[nodiscard]] constexpr core::Placement default_line_place(std::size_t line) noexcept
        {
            if (line == 0)
            {
                return DEFAULT_LINE1_PLACE;
            }
            return core::Placement{
                .target = static_cast<int>(line),
                .point = core::Anchor::Bottom,
                .pivot = core::Anchor::Top,
                .offset_x = 0.0f,
                .offset_y = 2.0f,
            };
        }
        constexpr float LINE1_FONT_SIZE = 24.0f;
        constexpr float LATER_LINE_FONT_SIZE = 16.0f;
        constexpr std::string_view LINE1_FORMAT = "{time}";

        /** @brief The text keys, which are not atomic. */
        struct TextKeys
        {
            std::array<std::string, core::LINE_COUNT> formats{std::string{LINE1_FORMAT}};
            core::AmPm am_pm{};
            std::vector<core::Period> periods = core::parse_periods(DEFAULT_PERIODS).periods;
            std::string real_date_format{DEFAULT_REAL_DATE_FORMAT};
        };

        /// Guards s_text, which the INI watcher thread stores and the caches copy.
        std::mutex s_text_mutex;
        TextKeys s_text;
        /// Bumped by publish_settings(). A cache copies when it holds another value.
        std::atomic<std::uint32_t> s_generation{0};

        /** @brief One part's place, as atomics. The target is stored as its int bits. */
        struct PlaceAtoms
        {
            std::atomic<std::uint32_t> target{};
            std::atomic<std::uint32_t> point{};
            std::atomic<std::uint32_t> pivot{};
            std::atomic<float> offset_x{};
            std::atomic<float> offset_y{};
        };

        struct LineAtoms
        {
            std::atomic<std::uint32_t> font{static_cast<std::uint32_t>(DEFAULT_FONT)};
            std::atomic<float> font_size{LATER_LINE_FONT_SIZE};
            std::atomic<std::uint32_t> color{DEFAULT_COLOR_VALUE};
            std::atomic<float> opacity{90.0f};
            std::atomic<bool> shadow{true};
            PlaceAtoms place;
        };

        struct DialAtoms
        {
            std::atomic<bool> show{true};
            std::atomic<float> size{60.0f};
            std::atomic<float> opacity{90.0f};
            std::atomic<bool> shadow{true};
            PlaceAtoms place;
        };

        // Atomics, so the INI watcher thread can store while the game thread reads. Each holds its key's default,
        // which registration reads.
        std::atomic<bool> s_enabled{true};
        std::atomic<std::uint32_t> s_hour_format{static_cast<std::uint32_t>(DEFAULT_HOUR_FORMAT)};
        std::array<LineAtoms, core::LINE_COUNT> s_lines;
        DialAtoms s_dial;
        std::atomic<std::uint32_t> s_follow_mask{DEFAULT_FOLLOW_MASK};
        std::atomic<std::uint32_t> s_minigame_mask{DEFAULT_MINIGAME_MASK};
        std::atomic<bool> s_show_in_photo_mode{false};
        std::atomic<bool> s_always_show{true};
        std::atomic<float> s_show_seconds{5.0f};
        std::atomic<float> s_toggle_key_hold_seconds{0.0f};
        std::atomic<float> s_show_key_hold_seconds{0.0f};
        std::atomic<int> s_show_every_hours{0};

        template <class T> [[nodiscard]] T load(const std::atomic<T> &value) noexcept
        {
            return value.load(std::memory_order_relaxed);
        }

        /** @brief Logs that @p key's @p value did not parse, so @p fallback applies. */
        void
        warn_value(std::string_view key, std::string_view value, std::string_view expected, std::string_view fallback)
        {
            (void)DMK::log()
                .try_log(DMK::LogLevel::Warning, "INI: {}={} is not {}, so {} applies", key, value, expected, fallback);
        }

        /** @brief Applies @p store to s_text under its mutex. */
        template <class Store> void store_text(Store store)
        {
            const std::lock_guard lock(s_text_mutex);
            store(s_text);
        }

        [[nodiscard]] float clamp_offset(float value) noexcept
        {
            return std::clamp(value, -MAX_OFFSET, MAX_OFFSET);
        }

        [[nodiscard]] float clamp_opacity(float value) noexcept
        {
            return std::clamp(value, 0.0f, MAX_OPACITY);
        }

        /** @brief Binds a key that names one of the nine points of a box. */
        void bind_point(
            const DMK::config::SectionBinder &section,
            std::string_view key,
            std::atomic<std::uint32_t> &out,
            core::Anchor fallback
        )
        {
            out.store(static_cast<std::uint32_t>(fallback), std::memory_order_relaxed);
            section.bind_parsed(
                key,
                key,
                out,
                [key, fallback](std::string_view value) -> std::uint32_t
                {
                    const std::optional<core::Anchor> parsed = core::parse_anchor(value);
                    if (!parsed)
                    {
                        warn_value(
                            key,
                            value,
                            "TopLeft, Top, TopRight, Left, Center, Right, BottomLeft, Bottom or BottomRight",
                            core::anchor_name(fallback)
                        );
                    }
                    return static_cast<std::uint32_t>(parsed.value_or(fallback));
                },
                core::anchor_name(fallback)
            );
        }

        /** @brief Binds AttachTo, AttachPoint, Pivot, OffsetX and OffsetY of one part's section. */
        void bind_place(const DMK::config::SectionBinder &section, PlaceAtoms &atoms, const core::Placement &fallback)
        {
            atoms.target.store(static_cast<std::uint32_t>(fallback.target), std::memory_order_relaxed);
            section.bind_parsed(
                "AttachTo",
                "AttachTo",
                atoms.target,
                [fallback](std::string_view value) -> std::uint32_t
                {
                    const std::optional<int> parsed = core::parse_target(value);
                    if (!parsed)
                    {
                        warn_value(
                            "AttachTo",
                            value,
                            "Screen, Dial, Line1, Line2, Line3 or Line4",
                            core::part_name(fallback.target)
                        );
                    }
                    return static_cast<std::uint32_t>(parsed.value_or(fallback.target));
                },
                core::part_name(fallback.target)
            );
            bind_point(section, "AttachPoint", atoms.point, fallback.point);
            bind_point(section, "Pivot", atoms.pivot, fallback.pivot);
            atoms.offset_x.store(fallback.offset_x, std::memory_order_relaxed);
            atoms.offset_y.store(fallback.offset_y, std::memory_order_relaxed);
            section.bind("OffsetX", atoms.offset_x);
            section.bind("OffsetY", atoms.offset_y);
        }

        /** @brief Binds a Color key. */
        void bind_color(const DMK::config::SectionBinder &section, std::atomic<std::uint32_t> &out)
        {
            section.bind_parsed(
                "Color",
                "Color",
                out,
                [](std::string_view value) -> std::uint32_t
                {
                    const std::optional<std::uint32_t> parsed = core::parse_color(value);
                    if (!parsed)
                    {
                        warn_value("Color", value, "a RRGGBB hex color", DEFAULT_COLOR);
                    }
                    return parsed.value_or(DEFAULT_COLOR_VALUE);
                },
                DEFAULT_COLOR
            );
        }

        /** @brief Binds the keys of text line @p line (0 for Line1). */
        void bind_line(std::size_t line)
        {
            const DMK::config::SectionBinder section =
                DMK::config::section(core::part_name(static_cast<int>(line) + 1));
            LineAtoms &atoms = s_lines[line];
            section.bind_string(
                "Format",
                "Format",
                [line](std::string_view value) -> void
                {
                    store_text([&](TextKeys &text) -> void { text.formats[line] = std::string{strings::trim(value)}; });
                },
                line == 0 ? LINE1_FORMAT : std::string_view{}
            );
            section.bind_parsed(
                "Font",
                "Font",
                atoms.font,
                [](std::string_view value) -> std::uint32_t
                {
                    const std::optional<core::Font> parsed = core::parse_font(value);
                    if (!parsed)
                    {
                        warn_value(
                            "Font",
                            value,
                            "Regular, Bold, Light, Manuscript or Display",
                            core::font_name(DEFAULT_FONT)
                        );
                    }
                    return static_cast<std::uint32_t>(parsed.value_or(DEFAULT_FONT));
                },
                core::font_name(DEFAULT_FONT)
            );
            atoms.font_size.store(line == 0 ? LINE1_FONT_SIZE : LATER_LINE_FONT_SIZE, std::memory_order_relaxed);
            section.bind("FontSize", atoms.font_size);
            bind_color(section, atoms.color);
            section.bind("Opacity", atoms.opacity);
            section.bind("Shadow", atoms.shadow);
            bind_place(section, atoms.place, default_line_place(line));
        }

        void bind_dial()
        {
            const DMK::config::SectionBinder section = DMK::config::section(core::part_name(core::DIAL_PART));
            section.bind("Show", s_dial.show);
            section.bind("Size", s_dial.size);
            section.bind("Opacity", s_dial.opacity);
            section.bind("Shadow", s_dial.shadow);
            bind_place(section, s_dial.place, DEFAULT_DIAL_PLACE);
        }

        [[nodiscard]] core::Placement load_place(const PlaceAtoms &atoms) noexcept
        {
            const auto point = std::min<std::uint32_t>(load(atoms.point), 8);
            const auto pivot = std::min<std::uint32_t>(load(atoms.pivot), 8);
            return core::Placement{
                .target = static_cast<int>(load(atoms.target)),
                .point = static_cast<core::Anchor>(point),
                .pivot = static_cast<core::Anchor>(pivot),
                .offset_x = clamp_offset(load(atoms.offset_x)),
                .offset_y = clamp_offset(load(atoms.offset_y)),
            };
        }

        /** @brief Breaks any attachment loop of @p settings and logs the parts it moved to the screen. */
        void resolve_attachments(Settings &settings) noexcept
        {
            std::array<int, core::PART_COUNT> targets{};
            targets[core::DIAL_PART] = settings.dial.place.target;
            for (std::size_t i = 0; i < core::LINE_COUNT; ++i)
            {
                targets[i + 1] = settings.lines[i].place.target;
            }
            const core::ResolvedTargets resolved = core::resolve_targets(targets);
            settings.dial.place.target = resolved.targets[core::DIAL_PART];
            for (std::size_t i = 0; i < core::LINE_COUNT; ++i)
            {
                settings.lines[i].place.target = resolved.targets[i + 1];
            }
            for (int part = 0; part < static_cast<int>(core::PART_COUNT); ++part)
            {
                if ((resolved.broken & (1u << part)) != 0)
                {
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Warning,
                        "INI: [{}] AttachTo closes a loop of attachments, so it attaches to the Screen",
                        core::part_name(part)
                    );
                }
            }
        }
    } // namespace

    void register_config_items()
    {
        const DMK::config::SectionBinder settings = DMK::config::section("Settings");
        settings.bind_log_level("LogLevel", constants::DEFAULT_LOG_LEVEL);
        settings.bind("Enabled", s_enabled);

        const DMK::config::SectionBinder clock = DMK::config::section("Clock");
        clock.bind_parsed(
            "HourFormat",
            "HourFormat",
            s_hour_format,
            [](std::string_view value) -> std::uint32_t
            {
                const std::optional<core::HourFormat> parsed = core::parse_hour_format(value);
                if (!parsed)
                {
                    warn_value("HourFormat", value, "24 or 12", "24");
                }
                return static_cast<std::uint32_t>(parsed.value_or(DEFAULT_HOUR_FORMAT));
            },
            "24"
        );
        clock.bind_string(
            "AmPm",
            "AmPm",
            [](std::string_view value) -> void
            {
                std::optional<core::AmPm> parsed = core::parse_am_pm(value);
                if (!parsed)
                {
                    warn_value("AmPm", value, "two words split by a comma", DEFAULT_AM_PM);
                }
                store_text([&](TextKeys &text) -> void { text.am_pm = std::move(parsed).value_or(core::AmPm{}); });
            },
            DEFAULT_AM_PM
        );
        clock.bind_string(
            "Periods",
            "Periods",
            [](std::string_view value) -> void
            {
                core::ParsedPeriods parsed = core::parse_periods(value);
                if (parsed.dropped != 0)
                {
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Warning,
                        "INI: Periods drops {} item(s). Each item is <start hour 0-23>: <name>, one per hour.",
                        parsed.dropped
                    );
                }
                if (parsed.periods.empty())
                {
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Warning,
                        "INI: Periods names no period, so the default periods apply"
                    );
                    parsed.periods = core::parse_periods(DEFAULT_PERIODS).periods;
                }
                store_text([&](TextKeys &text) -> void { text.periods = std::move(parsed.periods); });
            },
            DEFAULT_PERIODS
        );
        clock.bind_string(
            "RealDateFormat",
            "RealDateFormat",
            [](std::string_view value) -> void
            {
                std::string layout{strings::trim(value)};
                if (!core::is_valid_date_layout(layout))
                {
                    warn_value(
                        "RealDateFormat",
                        value,
                        "a date layout of the % codes a A b B c d D e F H I j m M p R S T U w W x X y Y z Z",
                        DEFAULT_REAL_DATE_FORMAT
                    );
                    layout = DEFAULT_REAL_DATE_FORMAT;
                }
                store_text([&](TextKeys &text) -> void { text.real_date_format = std::move(layout); });
            },
            DEFAULT_REAL_DATE_FORMAT
        );

        bind_dial();
        for (std::size_t line = 0; line < core::LINE_COUNT; ++line)
        {
            bind_line(line);
        }

        const DMK::config::SectionBinder visibility = DMK::config::section("Visibility");
        visibility.bind_parsed(
            "FollowHud",
            "FollowHud",
            s_follow_mask,
            [](std::string_view value) -> std::uint32_t
            {
                const core::ParsedNames parsed = core::parse_hud_parts(value);
                if (parsed.dropped != 0)
                {
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Warning,
                        "INI: FollowHud drops {} name(s) that are not HUD parts",
                        parsed.dropped
                    );
                }
                if (parsed.mask == 0)
                {
                    warn_value("FollowHud", value, "a list of HUD parts", DEFAULT_FOLLOW_HUD);
                    return DEFAULT_FOLLOW_MASK;
                }
                return parsed.mask;
            },
            DEFAULT_FOLLOW_HUD
        );
        visibility.bind_parsed(
            "ShowInMinigames",
            "ShowInMinigames",
            s_minigame_mask,
            [](std::string_view value) -> std::uint32_t
            {
                // An empty list is valid: the clock then shows in no minigame.
                const core::ParsedNames parsed = core::parse_minigames(value);
                if (parsed.dropped != 0)
                {
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Warning,
                        "INI: ShowInMinigames drops {} name(s) that are not minigames",
                        parsed.dropped
                    );
                }
                return parsed.mask;
            },
            DEFAULT_SHOW_IN_MINIGAMES
        );
        visibility.bind("ShowInPhotoMode", s_show_in_photo_mode);
        visibility.bind("AlwaysShow", s_always_show);
        visibility.bind("ShowSeconds", s_show_seconds);
        visibility.bind("ToggleKey.HoldTimeInSeconds", s_toggle_key_hold_seconds);
        visibility.bind("ShowKey.HoldTimeInSeconds", s_show_key_hold_seconds);
        visibility.bind("ShowEveryHours", s_show_every_hours);
    }

    void publish_settings() noexcept
    {
        // 0 stays unused, so a fresh cache always differs from a published generation.
        if (s_generation.fetch_add(1, std::memory_order_acq_rel) + 1 == 0)
        {
            s_generation.fetch_add(1, std::memory_order_acq_rel);
        }
    }

    bool SettingsCache::refresh()
    {
        const std::uint32_t generation = s_generation.load(std::memory_order_acquire);
        if (generation == m_generation)
        {
            return false;
        }
        // A store in progress keeps the old copy for this frame. The game thread never waits for the watcher.
        const std::unique_lock lock(s_text_mutex, std::try_to_lock);
        if (!lock.owns_lock())
        {
            return false;
        }
        Settings settings{
            .enabled = load(s_enabled),
            .text =
                core::TextSettings{
                    .hour_format = static_cast<core::HourFormat>(load(s_hour_format)),
                    .am_pm = s_text.am_pm,
                    .periods = s_text.periods,
                    .real_date_format = s_text.real_date_format,
                },
            .dial =
                DialSettings{
                    .show = load(s_dial.show),
                    .size = std::clamp(load(s_dial.size), MIN_DIAL_SIZE, MAX_DIAL_SIZE),
                    .opacity = clamp_opacity(load(s_dial.opacity)),
                    .shadow = load(s_dial.shadow),
                    .place = load_place(s_dial.place),
                },
            .follow_mask = load(s_follow_mask),
            .minigame_mask = load(s_minigame_mask),
            .show_in_photo_mode = load(s_show_in_photo_mode),
            .always_show = load(s_always_show),
            .show_seconds = std::clamp(load(s_show_seconds), MIN_SHOW_SECONDS, MAX_SHOW_SECONDS),
            .toggle_key_hold_seconds = std::clamp(load(s_toggle_key_hold_seconds), 0.0f, MAX_KEY_HOLD_SECONDS),
            .show_key_hold_seconds = std::clamp(load(s_show_key_hold_seconds), 0.0f, MAX_KEY_HOLD_SECONDS),
            .show_every_hours = std::clamp(load(s_show_every_hours), 0, MAX_SHOW_EVERY_HOURS),
        };
        for (std::size_t i = 0; i < core::LINE_COUNT; ++i)
        {
            const LineAtoms &atoms = s_lines[i];
            settings.lines[i] = LineSettings{
                .format = s_text.formats[i],
                .font = static_cast<core::Font>(load(atoms.font)),
                .font_size = std::clamp(load(atoms.font_size), MIN_FONT_SIZE, MAX_FONT_SIZE),
                .color = load(atoms.color) & 0xFFFFFFu,
                .opacity = clamp_opacity(load(atoms.opacity)),
                .shadow = load(atoms.shadow),
                .place = load_place(atoms.place),
            };
        }
        resolve_attachments(settings);
        m_settings = std::move(settings);
        m_generation = generation;
        return true;
    }
} // namespace HUDClock
