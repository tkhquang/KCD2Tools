/**
 * @file clock.cpp
 * @brief The clock's frame (see clock.hpp).
 *
 * The clock shows while any HUD part it follows shows, by the game's own HUD mask, so it leaves with the HUD wherever
 * the game hides it, without rules of its own. In a minigame it names (ShowInMinigames), it shows while the game shows
 * the minigame's own screen rather than a menu over it, by the same mask. With ShowInPhotoMode, it also shows while
 * photo mode runs, where the game hides the HUD. Within that, it stays up while pinned
 * (AlwaysShow, flipped by ToggleKey) or for ShowSeconds after ShowKey or a chime hour. The movie holds every part's
 * style, place and text until the game reloads it, so a frame sends only what changed: the styles and places after an
 * INI change or a new movie, the line texts and the dial's hour when the game minute, the computer's minute (for
 * {realtime} and {realdate}) or the settings change.
 */

#include "clock.hpp"
#include "config.hpp"
#include "core/clock_style.hpp"
#include "core/clock_text.hpp"
#include "core/key_press.hpp"
#include "core/layout.hpp"
#include "game/clock_movie.hpp"
#include "game/game_state.hpp"

#include <DetourModKit.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>

namespace HUDClock::clock
{
    namespace
    {
        using SteadyClock = std::chrono::steady_clock;

        constexpr std::size_t KEY_COUNT = static_cast<std::size_t>(Key::Show) + 1;

        /**
         * @brief The edges of one key, written by the input thread and read by the frame.
         * @details The press count is stored after the press time with release order, so a frame that sees a new press
         *          also sees its time.
         */
        struct KeyEdges
        {
            /// Steady-clock ticks of the current press, 0 while the key is up.
            std::atomic<std::int64_t> down_since{0};
            /// Presses so far. The frame acts at most once per press.
            std::atomic<std::uint32_t> presses{0};
        };
        std::array<KeyEdges, KEY_COUNT> s_keys;
        std::atomic<bool> s_stop_requested{false};
        std::atomic<bool> s_stopped{false};

        /** @brief What the main thread keeps between frames. Only run_frame() and reset() touch it. */
        struct FrameState
        {
            SettingsCache settings;
            /// True while the clock stays up. AlwaysShow sets it, ToggleKey flips it.
            bool pinned{true};
            /// The press count of each key the frame already handled.
            std::array<std::uint32_t, KEY_COUNT> handled_presses{};
            /// The settings the movie's style and place were sent for, valid while look_sent.
            Settings sent_look{};
            bool look_sent{false};
            void *element{nullptr};
            std::uintptr_t player{0};
            /// The game and computer minutes the texts were made for. -1 makes them again.
            std::int64_t text_key{-1};
            std::array<std::string, core::LINE_COUNT> texts;
            /// The AlwaysShow the pin last took, so only a change of the key resets a ToggleKey choice.
            std::optional<bool> always_show;
            /// End of the current ShowSeconds window.
            SteadyClock::time_point shown_until{};
            /// The chime period the world time was in on the last frame with a time, or -1.
            std::int64_t chime{-1};
            /// The player's minigames on the last frame, logged when they change.
            std::uint32_t minigames{0};
        };
        FrameState s_frame;

        [[nodiscard]] game::PartPlace part_place(const core::Placement &place) noexcept
        {
            return game::PartPlace{
                .target = place.target,
                .point = static_cast<int>(place.point),
                .pivot = static_cast<int>(place.pivot),
                .x = place.offset_x,
                .y = place.offset_y,
            };
        }

        /** @brief Sends every part's style and place of @p settings to the movie. */
        [[nodiscard]] bool send_look(void *element, const Settings &settings) noexcept
        {
            const DialSettings &dial = settings.dial;
            bool sent = game::set_dial_style(
                element,
                game::DialStyle{.show = dial.show, .size = dial.size, .opacity = dial.opacity, .shadow = dial.shadow}
            );
            sent = game::set_part_place(element, core::DIAL_PART, part_place(dial.place)) && sent;
            for (std::size_t i = 0; i < core::LINE_COUNT; ++i)
            {
                const LineSettings &line = settings.lines[i];
                const int part = static_cast<int>(i) + 1;
                const game::LineStyle style{
                    .font = static_cast<int>(line.font),
                    .size = line.font_size,
                    .color = line.color,
                    .opacity = line.opacity,
                    .shadow = line.shadow,
                };
                sent = game::set_line_style(element, part, style) && sent;
                sent = game::set_part_place(element, part, part_place(line.place)) && sent;
            }
            return sent;
        }

        /** @brief True when any line shows the computer's clock. */
        [[nodiscard]] bool uses_real_time(const Settings &settings) noexcept
        {
            for (const LineSettings &line : settings.lines)
            {
                if (core::uses_real_time(line.format))
                {
                    return true;
                }
            }
            return false;
        }

        /** @brief Hides the element when it shows. */
        void hide(void *element) noexcept
        {
            if (element != nullptr && game::clock_visible(element))
            {
                game::show_clock(element, false);
            }
        }

        /** @brief Opens a ShowSeconds window from @p now. */
        void open_window(FrameState &frame, const Settings &settings, SteadyClock::time_point now) noexcept
        {
            frame.shown_until =
                now +
                std::chrono::duration_cast<SteadyClock::duration>(std::chrono::duration<float>{settings.show_seconds});
        }

        /**
         * @brief Opens a window when the world time enters a new chime period (ShowEveryHours hours from midnight).
         * @details The first frame with a time only notes its period, so loading a save does not chime.
         */
        void update_chime(
            FrameState &frame,
            const Settings &settings,
            std::optional<std::int64_t> time,
            SteadyClock::time_point now
        ) noexcept
        {
            if (!time || settings.show_every_hours <= 0)
            {
                frame.chime = -1;
                return;
            }
            const std::int64_t chime = *time / (settings.show_every_hours * core::MS_PER_HOUR);
            if (frame.chime != -1 && chime != frame.chime)
            {
                open_window(frame, settings, now);
            }
            frame.chime = chime;
        }

        /**
         * @brief True once for each press of @p key that was held for @p hold_seconds, or on the press when it is 0.
         * @details A press released before its hold time is spent without an action, so a quick tap does nothing.
         */
        [[nodiscard]] bool
        take_press(FrameState &frame, Key key, float hold_seconds, SteadyClock::time_point now) noexcept
        {
            const auto index = static_cast<std::size_t>(key);
            const KeyEdges &edges = s_keys[index];
            std::uint32_t &handled = frame.handled_presses[index];
            const std::uint32_t presses = edges.presses.load(std::memory_order_acquire);
            if (presses == handled)
            {
                return false;
            }
            const auto hold =
                std::chrono::duration_cast<SteadyClock::duration>(std::chrono::duration<float>{hold_seconds});
            const core::PressStep step = core::step_press(
                edges.down_since.load(std::memory_order_relaxed),
                now.time_since_epoch().count(),
                hold.count()
            );
            if (step == core::PressStep::Wait)
            {
                return false;
            }
            handled = presses;
            return step == core::PressStep::Act;
        }

        /** @brief Flips the clock between staying up and hidden, the ToggleKey action. */
        void toggle(FrameState &frame) noexcept
        {
            frame.pinned = !frame.pinned;
            (void)DMK::log().try_log(DMK::LogLevel::Info, "Clock {}", frame.pinned ? "shown" : "hidden");
        }

        /** @brief The computer's local time now. */
        [[nodiscard]] std::tm local_now() noexcept
        {
            std::tm local{};
            const std::time_t now = std::time(nullptr);
            (void)localtime_s(&local, &now);
            return local;
        }

        void run_frame_unguarded()
        {
            FrameState &frame = s_frame;
            if (frame.settings.refresh())
            {
                frame.text_key = -1;
            }
            const Settings &settings = frame.settings.get();
            if (frame.always_show != settings.always_show)
            {
                frame.always_show = settings.always_show;
                frame.pinned = settings.always_show;
            }
            frame.element = game::find_clock_element(frame.element);

            if (s_stop_requested.load(std::memory_order_acquire))
            {
                hide(frame.element);
                s_stopped.store(true, std::memory_order_release);
                return;
            }

            const SteadyClock::time_point now = SteadyClock::now();
            const std::optional<std::uint32_t> hud = game::hud_part_bits();
            const std::optional<std::int64_t> time = game::world_time_ms();
            if (take_press(frame, Key::Toggle, settings.toggle_key_hold_seconds, now))
            {
                toggle(frame);
            }
            if (take_press(frame, Key::Show, settings.show_key_hold_seconds, now))
            {
                open_window(frame, settings, now);
            }
            update_chime(frame, settings, time, now);
            if (frame.element == nullptr)
            {
                return;
            }

            const bool up = frame.pinned || now < frame.shown_until;
            const std::uint32_t minigames =
                settings.minigame_mask != 0 ? game::player_minigames().value_or(0) : std::uint32_t{0};
            if (minigames != frame.minigames)
            {
                frame.minigames = minigames;
                (void)DMK::log().try_log(DMK::LogLevel::Debug, "Player minigames 0x{:X}", minigames);
            }
            const bool shown_by_hud =
                hud && core::hud_allows_clock(*hud, settings.follow_mask, minigames, settings.minigame_mask);
            const bool shown_by_photo_mode = settings.show_in_photo_mode && game::photo_mode_active();
            const bool wanted = settings.enabled && up && (shown_by_hud || shown_by_photo_mode) && time;
            if (!wanted)
            {
                hide(frame.element);
                return;
            }
            if (!game::clock_visible(frame.element))
            {
                game::show_clock(frame.element, true);
            }

            // A new player is a new movie at its defaults: everything goes again.
            const std::uintptr_t player = game::clock_player(frame.element);
            if (player == 0)
            {
                return;
            }
            if (player != frame.player)
            {
                frame.player = player;
                frame.look_sent = false;
                frame.text_key = -1;
            }
            if (!frame.look_sent || !settings.same_look(frame.sent_look))
            {
                frame.look_sent = send_look(frame.element, settings);
                frame.sent_look = settings;
            }

            // The texts change with the game minute, and with the computer's minute when a line shows the computer's
            // clock. The key packs both, so either change makes the texts again.
            const std::int64_t game_minute = *time / core::MS_PER_MINUTE;
            const bool real = uses_real_time(settings);
            const std::int64_t real_minute = real ? static_cast<std::int64_t>(std::time(nullptr)) / 60 : 0;
            const std::int64_t key = game_minute * 2 + (real_minute & 1);
            if (key == frame.text_key)
            {
                return;
            }
            const core::ClockMoment moment{
                .game = core::clock_time(*time),
                .day = core::world_day(*time),
                .real = real ? local_now() : std::tm{},
            };
            std::array<const char *, core::LINE_COUNT> texts{};
            for (std::size_t i = 0; i < core::LINE_COUNT; ++i)
            {
                frame.texts[i] = core::render(settings.text, settings.lines[i].format, moment);
                texts[i] = frame.texts[i].c_str();
            }
            const double hour = static_cast<double>(*time % core::MS_PER_DAY) / static_cast<double>(core::MS_PER_HOUR);
            if (game::set_line_texts(frame.element, texts) && game::set_clock_hour(frame.element, hour))
            {
                frame.text_key = key;
            }
        }
    } // namespace

    void run_frame() noexcept
    {
        try
        {
            run_frame_unguarded();
        }
        catch (...)
        {
            // Only an allocation of the text can throw. The next frame tries again.
            s_frame.text_key = -1;
        }
    }

    void key_changed(Key key, bool down) noexcept
    {
        KeyEdges &edges = s_keys[static_cast<std::size_t>(key)];
        if (!down)
        {
            edges.down_since.store(0, std::memory_order_relaxed);
            return;
        }
        edges.down_since.store(SteadyClock::now().time_since_epoch().count(), std::memory_order_relaxed);
        edges.presses.fetch_add(1, std::memory_order_release);
    }

    void request_stop() noexcept
    {
        s_stopped.store(false, std::memory_order_release);
        s_stop_requested.store(true, std::memory_order_release);
    }

    bool stopped() noexcept
    {
        return s_stopped.load(std::memory_order_acquire);
    }

    void reset() noexcept
    {
        s_frame.element = nullptr;
        s_frame.player = 0;
        s_frame.look_sent = false;
        s_frame.text_key = -1;
        s_frame.always_show.reset();
        s_frame.chime = -1;
        s_frame.minigames = 0;
        s_frame.shown_until = {};
        // Presses made before the reset are spent, so a later start does not act on them.
        for (std::size_t i = 0; i < s_keys.size(); ++i)
        {
            s_frame.handled_presses[i] = s_keys[i].presses.load(std::memory_order_acquire);
        }
        s_stop_requested.store(false, std::memory_order_release);
        s_stopped.store(false, std::memory_order_release);
    }
} // namespace HUDClock::clock
