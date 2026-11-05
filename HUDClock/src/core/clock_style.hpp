/**
 * @file core/clock_style.hpp
 * @brief The INI names of the clock's place, font, color, the HUD parts it follows and the minigames it shows in.
 *
 * Plain C++ with no game, Windows or DetourModKit dependency, so its tests run without the game.
 */
#ifndef HUDCLOCK_CORE_CLOCK_STYLE_HPP
#define HUDCLOCK_CORE_CLOCK_STYLE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace HUDClock::core
{
    /** @brief One of the nine points of a box (the screen or a part). The values are the movie's point numbers. */
    enum class Anchor : std::uint8_t
    {
        TopLeft,
        Top,
        TopRight,
        Left,
        Center,
        Right,
        BottomLeft,
        Bottom,
        BottomRight,
    };

    /** @brief The INI name of @p anchor. */
    [[nodiscard]] std::string_view anchor_name(Anchor anchor) noexcept;

    /** @brief The Anchor that @p text names, in any case, or std::nullopt. */
    [[nodiscard]] std::optional<Anchor> parse_anchor(std::string_view text) noexcept;

    /** @brief The game font the clock uses. The values are the movie's font numbers. */
    enum class Font : std::uint8_t
    {
        /// DefaultFont, the HUD's body text.
        Regular,
        /// DefaultFontBold.
        Bold,
        /// LightFont.
        Light,
        /// Manuscript, the handwritten face of letters and books.
        Manuscript,
        /// DisplayFont, the face of titles.
        Display,
    };

    /** @brief The INI name of @p font. */
    [[nodiscard]] std::string_view font_name(Font font) noexcept;

    /** @brief The Font that @p text names, in any case, or std::nullopt. */
    [[nodiscard]] std::optional<Font> parse_font(std::string_view text) noexcept;

    /** @brief @p text as a RRGGBB color, with an optional leading '#', or std::nullopt. */
    [[nodiscard]] std::optional<std::uint32_t> parse_color(std::string_view text) noexcept;

    /**
     * @brief The HUD parts of the game's HUD mask (wh::framework::E_HudElements), in bit order.
     * @details Bit N of the mask is set while part N shows.
     */
    inline constexpr std::array<std::string_view, 28> HUD_PARTS = {
        "Compass",          "Stats",         "QAMWeapon",    "QAMFood",         "Subtitles",
        "InfoText",         "GameLog",       "Hints",        "DialogLeft",      "DialogRight",
        "Cursor",           "Crime",         "Wanted",       "PopUpBackground", "TutorialMessage",
        "FancyEvent",       "SkillCheck",    "ItemTransfer", "Buffs",           "CommonEvent",
        "DiceCursor",       "Trespassing",   "RatioStrips",  "ShootingContest", "Bubbles",
        "TutorialInDialog", "DiceContainer", "Vignette",
    };

    /**
     * @brief The minigames the clock can show in, in bit order. Each is the game class wh::playermodule::C_<name>.
     */
    inline constexpr std::array<std::string_view, 8> MINIGAMES = {
        "Alchemy",
        "Blacksmithing",
        "Dice",
        "Reading",
        "LockPicking",
        "Pickpocketing",
        "HerbGathering",
        "HoleDigging",
    };

    /**
     * @brief The HUD parts the game shows on a minigame's own screen and hides under a menu opened over it.
     * @details Crime, FancyEvent, SkillCheck and CommonEvent: the alchemy bench and the dice table show them, while
     *          the ingredient list, the help pages and the pause menu over a minigame hide all four.
     */
    inline constexpr std::uint32_t MINIGAME_SCREEN_PARTS =
        (std::uint32_t{1} << 11) | (std::uint32_t{1} << 15) | (std::uint32_t{1} << 16) | (std::uint32_t{1} << 19);

    /** @brief A set of names as a mask (bit N for name N), and the list items that named nothing. */
    struct ParsedNames
    {
        std::uint32_t mask{0};
        std::size_t dropped{0};
    };

    /** @brief Parses a comma-separated list of HUD part names, in any case. */
    [[nodiscard]] ParsedNames parse_hud_parts(std::string_view text) noexcept;

    /** @brief Parses a comma-separated list of minigame names, in any case. */
    [[nodiscard]] ParsedNames parse_minigames(std::string_view text) noexcept;

    /**
     * @brief Whether the game's HUD state lets the clock show.
     * @param hud The HUD mask (bit N for HUD_PARTS[N]).
     * @param follow The HUD parts the clock follows.
     * @param minigames The minigames the player is in (bit N for MINIGAMES[N]).
     * @param show_in The minigames the clock shows in.
     * @return True while a followed part shows, or while the player is in one of @p show_in and its own screen shows.
     */
    [[nodiscard]] constexpr bool
    hud_allows_clock(std::uint32_t hud, std::uint32_t follow, std::uint32_t minigames, std::uint32_t show_in) noexcept
    {
        return (hud & follow) != 0 || ((minigames & show_in) != 0 && (hud & MINIGAME_SCREEN_PARTS) != 0);
    }
} // namespace HUDClock::core

#endif // HUDCLOCK_CORE_CLOCK_STYLE_HPP
