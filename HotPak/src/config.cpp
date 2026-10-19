/**
 * @file config.cpp
 * @brief Configuration registration through DMK::config.
 */

#include "config.hpp"
#include "constants.hpp"
#include "pak_names.hpp"

#include <DetourModKit.hpp>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace HotPak
{
    namespace
    {
        constexpr int DEFAULT_POLL_MS = 500;
        constexpr int MIN_POLL_MS = 100;
        constexpr int MAX_POLL_MS = 10000;
        constexpr int DEFAULT_RESET_WINDOW_S = 60;
        constexpr int MIN_RESET_WINDOW_S = 5;
        constexpr int MAX_RESET_WINDOW_S = 600;

        std::mutex s_paks_mutex;
        std::vector<std::wstring> s_paks;
        std::atomic<int> s_poll_ms{DEFAULT_POLL_MS};
        std::atomic<int> s_reset_window_s{DEFAULT_RESET_WINDOW_S};

        /** @brief Binds an integer key whose value outside [@p low, @p high] is clamped with a warning. */
        void bind_bounded(
            const DMK::config::SectionBinder &section,
            std::string_view key,
            std::atomic<int> &out,
            int default_value,
            int low,
            int high
        )
        {
            section.bind_int(
                key,
                key,
                [key, &out, low, high](int value)
                {
                    const int bounded = std::clamp(value, low, high);
                    if (bounded != value)
                    {
                        DMK::log().warning("Config: {}={} is outside {}-{}; using {}", key, value, low, high, bounded);
                    }
                    out.store(bounded, std::memory_order_relaxed);
                },
                default_value
            );
        }
    } // namespace

    void register_config_items()
    {
        const DMK::config::SectionBinder settings = DMK::config::section("Settings");
        settings.bind_log_level("LogLevel", constants::DEFAULT_LOG_LEVEL);
        settings.bind_string(
            "Paks",
            "Paks",
            [](std::string_view value)
            {
                PakList parsed = parse_pak_list(value);
                for (const std::string &rejected : parsed.rejected)
                {
                    DMK::log().warning("Config: Paks entry '{}' is not a .pak file; ignored", rejected);
                }
                const std::lock_guard lock(s_paks_mutex);
                s_paks = std::move(parsed.paths);
            },
            ""
        );
        bind_bounded(settings, "PollMs", s_poll_ms, DEFAULT_POLL_MS, MIN_POLL_MS, MAX_POLL_MS);
        bind_bounded(
            settings,
            "ResetWindowSeconds",
            s_reset_window_s,
            DEFAULT_RESET_WINDOW_S,
            MIN_RESET_WINDOW_S,
            MAX_RESET_WINDOW_S
        );
    }

    WatchSettings watch_settings()
    {
        WatchSettings out;
        {
            const std::lock_guard lock(s_paks_mutex);
            out.paks = s_paks;
        }
        out.poll = std::chrono::milliseconds{s_poll_ms.load(std::memory_order_relaxed)};
        return out;
    }

    std::chrono::milliseconds reset_window() noexcept
    {
        return std::chrono::seconds{s_reset_window_s.load(std::memory_order_relaxed)};
    }

} // namespace HotPak
