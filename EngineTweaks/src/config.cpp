/**
 * @file config.cpp
 * @brief Configuration registration through DMK::config. Every key stores into an atomic the detours read.
 */

#include "config.hpp"
#include "constants.hpp"

#include <DetourModKit.hpp>

#include <atomic>

namespace EngineTweaks
{
    namespace
    {
        std::atomic<bool> s_enabled{true};
        std::atomic<bool> s_menu_view_distance{true};
        std::atomic<bool> s_link_index{true};
        std::atomic<bool> s_inventory_refresh{true};
        std::atomic<bool> s_perk_copy{true};
        std::atomic<bool> s_sleep_prediction{true};
        std::atomic<bool> s_sleep_prediction_without_items{true};
        std::atomic<bool> s_verify_link_index{false};
        std::atomic<bool> s_verify_sleep_prediction{false};

        [[nodiscard]] const std::atomic<bool> &switch_of(Fix fix) noexcept
        {
            switch (fix)
            {
            case Fix::MenuViewDistance:
                return s_menu_view_distance;
            case Fix::LinkIndex:
                return s_link_index;
            case Fix::InventoryRefresh:
                return s_inventory_refresh;
            case Fix::PerkCopy:
                return s_perk_copy;
            case Fix::SleepPrediction:
                return s_sleep_prediction;
            case Fix::SleepPredictionWithoutItems:
                return s_sleep_prediction_without_items;
            }
            return s_enabled;
        }
    } // namespace

    void register_config_items()
    {
        const DMK::config::SectionBinder settings = DMK::config::section("Settings");
        settings.bind_log_level("LogLevel", constants::DEFAULT_LOG_LEVEL);
        settings.bind("Enabled", s_enabled);

        const DMK::config::SectionBinder fixes = DMK::config::section("Fixes");
        fixes.bind("MenuViewDistance", s_menu_view_distance);
        fixes.bind("LinkIndex", s_link_index);
        fixes.bind("InventoryRefresh", s_inventory_refresh);
        fixes.bind("PerkCopy", s_perk_copy);
        fixes.bind("SleepPrediction", s_sleep_prediction);
        fixes.bind("SleepPredictionWithoutItems", s_sleep_prediction_without_items);

        const DMK::config::SectionBinder debug = DMK::config::section("Debug");
        debug.bind("VerifyLinkIndex", s_verify_link_index);
        debug.bind("VerifySleepPrediction", s_verify_sleep_prediction);
    }

    bool fix_enabled(Fix fix) noexcept
    {
        return s_enabled.load(std::memory_order_relaxed) && switch_of(fix).load(std::memory_order_relaxed);
    }

    bool verify_link_index() noexcept
    {
        return s_verify_link_index.load(std::memory_order_relaxed);
    }

    bool verify_sleep_prediction() noexcept
    {
        return s_verify_sleep_prediction.load(std::memory_order_relaxed);
    }
} // namespace EngineTweaks
