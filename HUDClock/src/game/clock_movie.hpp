/**
 * @file game/clock_movie.hpp
 * @brief The mod's Scaleform element: found by name in CFlashUI, shown and hidden through the element, and drawn
 *        through its Flash player's Invoke.
 *
 * The game loads the element from the mod's pak. The movie (ui/KCD2_HUDClock.as) only lays out and draws the text it
 * receives. Call these functions on the main thread, where the game drives its own UI.
 */
#ifndef HUDCLOCK_GAME_CLOCK_MOVIE_HPP
#define HUDCLOCK_GAME_CLOCK_MOVIE_HPP

#include <cstdint>
#include <span>

namespace HUDClock::game
{
    /** @brief One text line's style for the movie's setLineStyle(). */
    struct LineStyle
    {
        /// core::Font as its number.
        int font{0};
        /// Text height in pixels of a 1080p screen.
        double size{22.0};
        /// RRGGBB.
        std::uint32_t color{0xFFFFFF};
        /// 0 to 100.
        double opacity{100.0};
        bool shadow{true};
    };

    /** @brief The dial's style for the movie's setDialStyle(). */
    struct DialStyle
    {
        bool show{false};
        /// Height in pixels of a 1080p screen.
        double size{40.0};
        /// 0 to 100.
        double opacity{100.0};
        bool shadow{true};
    };

    /** @brief One part's place for the movie's setPlace(). */
    struct PartPlace
    {
        /// -1 for the screen, or a part number.
        int target{-1};
        /// core::Anchor numbers: the target's point, and the part's own point that sits on it.
        int point{0};
        int pivot{0};
        /// Offsets right and down, in pixels of a 1080p screen.
        double x{0.0};
        double y{0.0};
    };

    /**
     * @brief The mod's element, or nullptr when its pak is not installed or the UI is not up yet.
     * @param cached The element an earlier call returned, or nullptr. It is returned again while it is still the mod's
     *        element, so a frame does not search the element list.
     */
    [[nodiscard]] void *find_clock_element(void *cached) noexcept;

    /** @brief True when the game shows @p element, the result of find_clock_element(). */
    [[nodiscard]] bool clock_visible(void *element) noexcept;

    /** @brief Shows or hides @p element. A show loads a movie the game unloaded. */
    void show_clock(void *element, bool visible) noexcept;

    /**
     * @brief The identity of @p element's Flash player, or 0 when the element is hidden or has none.
     * @details The game replaces the player when it reloads the movie (gfx_reload_all), and the new movie starts from
     *          its defaults, so a new identity means the style and text go again.
     */
    [[nodiscard]] std::uintptr_t clock_player(void *element) noexcept;

    /**
     * @brief Calls the movie's setLineStyle() for line part @p part (1 to 4), setDialStyle(), setPlace() for part
     *        @p part (0 the dial, 1 to 4 the lines), or setTexts() with every line's text.
     * @return False when the element is hidden, has no Flash player, or the call failed. A hidden element is never
     *         called, because a call loads its movie again only for the game to unload it.
     */
    bool set_line_style(void *element, int part, const LineStyle &style) noexcept;
    bool set_dial_style(void *element, const DialStyle &style) noexcept;
    bool set_part_place(void *element, int part, const PartPlace &place) noexcept;
    bool set_line_texts(void *element, std::span<const char *const, 4> texts) noexcept;

    /** @brief Calls the movie's setHour(), which turns the dial to @p hour, 0 to 24 with the minutes as a fraction. */
    bool set_clock_hour(void *element, double hour) noexcept;
} // namespace HUDClock::game

#endif // HUDCLOCK_GAME_CLOCK_MOVIE_HPP
