/**
 * @file config.hpp
 * @brief KCD2_HUDClock.ini: what each part of the clock shows, where, how it looks, and when the clock shows.
 */
#ifndef HUDCLOCK_CONFIG_HPP
#define HUDCLOCK_CONFIG_HPP

#include "core/clock_style.hpp"
#include "core/clock_text.hpp"
#include "core/layout.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace HUDClock
{
    /** @brief One text line: its layout of tokens, its style and its place. An empty Format hides it. */
    struct LineSettings
    {
        std::string format;
        core::Font font{};
        /// Text height in pixels of a 1080p screen. The clock scales with the screen height, as the HUD does.
        float font_size{};
        /// RRGGBB.
        std::uint32_t color{};
        /// 0 (invisible) to 100 (opaque).
        float opacity{};
        /// True when a dark copy of the text sits behind it.
        bool shadow{};
        core::Placement place{};

        /** @brief True when the style and place, which setLineStyle and setPlace carry, equal those of @p other. */
        [[nodiscard]] bool same_look(const LineSettings &other) const noexcept
        {
            return font == other.font && font_size == other.font_size && color == other.color &&
                   opacity == other.opacity && shadow == other.shadow && place == other.place;
        }
    };

    /** @brief The sun and moon dial. */
    struct DialSettings
    {
        bool show{};
        /// Height in pixels of a 1080p screen.
        float size{};
        /// 0 (invisible) to 100 (opaque).
        float opacity{};
        /// True when a dark copy of the arc sits behind it.
        bool shadow{};
        core::Placement place{};

        friend bool operator==(const DialSettings &, const DialSettings &) = default;
    };

    /**
     * @brief The loaded settings. Every value is clamped to a usable range, and the attachments hold no loop.
     * @details The INI defaults live in config.cpp, beside the storage each key binds to.
     */
    struct Settings
    {
        /// Master switch. Off keeps the clock hidden.
        bool enabled{};
        /// How the tokens read: HourFormat, AmPm, Periods and RealDateFormat.
        core::TextSettings text{};
        std::array<LineSettings, core::LINE_COUNT> lines{};
        DialSettings dial{};
        /// The HUD parts (bits of the game's HUD mask) the clock follows: it shows while any of them shows.
        std::uint32_t follow_mask{};
        /// The minigames (bits of core::MINIGAMES) the clock also shows in, while their own screen shows.
        std::uint32_t minigame_mask{};
        /// True when the clock also shows in photo mode.
        bool show_in_photo_mode{};
        /// True when the clock stays on screen. False shows it only for ShowSeconds after ShowKey or a chime hour.
        bool always_show{};
        /// Real seconds the clock shows after ShowKey or a chime hour.
        float show_seconds{};
        /// Real seconds ToggleKey must stay down before it acts. 0 acts on the press.
        float toggle_key_hold_seconds{};
        /// Real seconds ShowKey must stay down before it acts. 0 acts on the press.
        float show_key_hold_seconds{};
        /// Every this many game hours, from midnight, the clock shows for ShowSeconds. 0 never.
        int show_every_hours{};

        /** @brief True when every part's style and place equal those of @p other. The line texts may differ. */
        [[nodiscard]] bool same_look(const Settings &other) const noexcept
        {
            for (std::size_t i = 0; i < lines.size(); ++i)
            {
                if (!lines[i].same_look(other.lines[i]))
                {
                    return false;
                }
            }
            return dial == other.dial;
        }
    };

    /** @brief Registers every INI item with DMK::config. Call before DMK::config::load(). */
    void register_config_items();

    /**
     * @brief Marks the settings as changed, so every SettingsCache takes them on its next refresh.
     * @note Call after each load or reload of the INI.
     */
    void publish_settings() noexcept;

    /** @brief One thread's copy of the settings, refreshed without a wait. */
    class SettingsCache
    {
    public:
        /**
         * @brief Takes the settings as last published when they changed since the last call.
         * @details While the INI watcher holds the text settings at that moment, the copy stays as it was and a later
         *          call takes the change. An attachment loop is broken and logged here.
         * @return True when the copy changed.
         * @note Use each cache from one thread only. It allocates only when the settings changed.
         */
        bool refresh();

        /** @brief The copy. Valid after the first refresh() that returned true. */
        [[nodiscard]] const Settings &get() const noexcept { return m_settings; }

    private:
        Settings m_settings{};
        /// The generation the copy holds. 0 is never published, so the first refresh takes the settings.
        std::uint32_t m_generation{0};
    };
} // namespace HUDClock

#endif // HUDCLOCK_CONFIG_HPP
