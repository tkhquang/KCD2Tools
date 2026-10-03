/**
 * @file config.cpp
 * @brief Configuration registration for the Henry's Senses mod using DMK::config.
 *
 * Every item binds through a DMK::config::SectionBinder, so each INI section name is written once and the keys under
 * it read as a group. DMK::config::load() / log_all() are driven by the lifecycle once every item is registered.
 */

#include "config.hpp"
#include "constants.hpp"
#include "game_state.hpp"
#include "highlight/groups.hpp"

#include <DetourModKit.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>

namespace HenrySenses
{
    namespace
    {
        // [Settings] Key. A string cannot be a lock-free atomic, so the watcher-thread setter and the reader share it
        // under a mutex; neither side runs on a hook callback path.
        std::mutex s_default_key_mutex;
        std::string s_default_key{"H,Gamepad_RB+Gamepad_Start"};

        /**
         * @brief Trims leading and trailing ASCII whitespace from a view (no allocation).
         * @param text The view to trim.
         * @return The trimmed sub-view.
         */
        [[nodiscard]] std::string_view trim_view(std::string_view text) noexcept
        {
            constexpr std::string_view whitespace = " \t\r\n";
            const std::size_t begin = text.find_first_not_of(whitespace);
            if (begin == std::string_view::npos)
            {
                return {};
            }
            const std::size_t end = text.find_last_not_of(whitespace);
            return text.substr(begin, end - begin + 1);
        }

        /**
         * @brief ASCII case-insensitive equality of a token against a lowercase literal.
         * @param token The INI token.
         * @param lower_literal The lowercase literal to compare against.
         * @return True on a case-insensitive match.
         */
        [[nodiscard]] bool token_equals(std::string_view token, std::string_view lower_literal) noexcept
        {
            if (token.size() != lower_literal.size())
            {
                return false;
            }
            for (std::size_t i = 0; i < token.size(); ++i)
            {
                if (ascii_lower(token[i]) != lower_literal[i])
                {
                    return false;
                }
            }
            return true;
        }

        /**
         * @brief Parses [Settings] Mode.
         * @param text The INI value.
         * @return The GroupMode value; an unknown token selects Pulse.
         */
        [[nodiscard]] std::uint32_t parse_mode(std::string_view text)
        {
            const std::string_view token = trim_view(text);
            const std::optional<GroupMode> mode = parse_group_mode(token);
            if (!mode.has_value() && !token.empty())
            {
                DMK::log().warning("Config: unknown Mode '{}'; using Pulse", std::string(token));
            }
            return static_cast<std::uint32_t>(mode.value_or(GroupMode::Pulse));
        }

        /**
         * @brief Parses [Render] Backend.
         * @param text The INI value.
         * @return The RenderBackend value; an unknown token selects Auto.
         */
        [[nodiscard]] std::uint32_t parse_backend(std::string_view text)
        {
            const std::string_view token = trim_view(text);
            RenderBackend backend = RenderBackend::Auto;
            if (token_equals(token, "engine"))
            {
                backend = RenderBackend::Engine;
            }
            else if (token_equals(token, "markers"))
            {
                backend = RenderBackend::Markers;
            }
            else if (!token.empty() && !token_equals(token, "auto"))
            {
                DMK::log().warning("Config: unknown Backend '{}'; using Auto", std::string(token));
            }
            return static_cast<std::uint32_t>(backend);
        }

        /**
         * @brief Parses [Render] Style.
         * @param text The INI value.
         * @return The RenderStyle value; an unknown token selects OutlineFill.
         */
        [[nodiscard]] std::uint32_t parse_style(std::string_view text)
        {
            const std::string_view token = trim_view(text);
            if (token_equals(token, "fill"))
            {
                return static_cast<std::uint32_t>(RenderStyle::Fill);
            }
            if (token_equals(token, "outline"))
            {
                return static_cast<std::uint32_t>(RenderStyle::Outline);
            }
            if (!token.empty() && !token_equals(token, "outlinefill"))
            {
                DMK::log().warning("Config: unknown Style '{}'; using OutlineFill", std::string(token));
            }
            return static_cast<std::uint32_t>(RenderStyle::OutlineFill);
        }

        /**
         * @brief Parses [Render] FocusTint.
         * @param text The INI value: RRGGBB in hex.
         * @return 0xRRGGBB; anything else selects white (no tint).
         */
        [[nodiscard]] std::uint32_t parse_rgb(std::string_view text)
        {
            const std::string_view token = trim_view(text);
            std::uint32_t value = 0;
            const bool hex =
                token.size() == 6 && std::all_of(
                                         token.begin(),
                                         token.end(),
                                         [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); }
                                     );
            if (!hex || std::from_chars(token.data(), token.data() + token.size(), value, 16).ec != std::errc{})
            {
                DMK::log().warning("Config: FocusTint '{}' is not RRGGBB; using FFFFFF", std::string(token));
                return 0xFFFFFFu;
            }
            return value;
        }
    } // namespace

    LiveSettings &settings() noexcept
    {
        static LiveSettings s;
        return s;
    }

    std::string default_key()
    {
        const std::lock_guard lock(s_default_key_mutex);
        return s_default_key;
    }

    void register_config_items()
    {
        LiveSettings &s = settings();

        const DMK::config::SectionBinder general = DMK::config::section("Settings");
        const DMK::config::SectionBinder render = DMK::config::section("Render");

        general.bind_log_level("LogLevel", constants::DEFAULT_LOG_LEVEL);
        general.bind<bool>("Enabled", "Enabled", s.enabled, true);
        general.bind_string(
            "Key",
            "Key",
            [](std::string_view value)
            {
                const std::lock_guard lock(s_default_key_mutex);
                s_default_key.assign(trim_view(value));
            },
            "H,Gamepad_RB+Gamepad_Start"
        );
        general.bind<bool>("Key.Consume", "Key Consume", s.key_consume, true);
        general.bind_parsed("Mode", "Mode", s.mode, parse_mode, "Pulse");
        general.bind<float>("Duration", "Duration", s.duration, 5.0f);
        general.bind<float>("FadeOut", "Fade Out", s.fade_out, 1.0f);
        general.bind<float>("Radius", "Radius", s.radius, 20.0f);
        general.bind<int>("ScanIntervalMs", "Scan Interval Ms", s.scan_interval_ms, 250);

        render.bind_parsed("Backend", "Backend", s.backend, parse_backend, "Auto");
        render.bind_parsed("Style", "Style", s.style, parse_style, "OutlineFill");
        render.bind<float>("OutlineWidth", "Outline Width", s.outline_width, 2.0f);
        render.bind<float>("Strength", "Strength", s.strength, 0.5f);
        render.bind<float>("FillOpacity", "Fill Opacity", s.fill_opacity, 0.35f);
        render.bind<float>("Softness", "Softness", s.softness, 0.0f);
        render.bind<float>("FadeStart", "Fade Start", s.fade_start, 12.0f);
        render.bind<float>("MinOpacity", "Min Opacity", s.min_opacity, 1.0f);
        render.bind<float>("FadePower", "Fade Power", s.fade_power, 1.0f);
        render.bind<bool>("SeeThrough", "See Through", s.see_through, true);
        render.bind<bool>("RenderAlways", "Render Always", s.render_always, true);
        render.bind<float>("FocusDarken", "Focus Darken", s.focus_darken, 0.0f);
        render.bind_parsed("FocusTint", "Focus Tint", s.focus_tint, parse_rgb, "B8C4D8");
        render.bind<float>("FocusFadeIn", "Focus Fade In", s.focus_fade_in, 0.4f);

        general.bind_parsed(
            "HideIn",
            "Hide In",
            s.hide_in,
            [](std::string_view text)
            {
                const StateList parsed = parse_state_list(text);
                if (!parsed.unknown.empty())
                {
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Warning,
                        "Config: HideIn ignores unknown situation(s) {} (known: Dialogue, Combat, Minigame)",
                        parsed.unknown
                    );
                }
                return parsed.mask;
            },
            "Dialogue"
        );

        general.bind<bool>("ExportSignatures", "Export Signatures", s.export_signatures, false);
    }

} // namespace HenrySenses
