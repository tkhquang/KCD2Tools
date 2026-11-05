/**
 * @file config.hpp
 * @brief KCD2_EngineTweaks.ini: the master switch, one switch per fix, and the diagnostics.
 */
#ifndef ENGINETWEAKS_CONFIG_HPP
#define ENGINETWEAKS_CONFIG_HPP

#include <cstdint>

namespace EngineTweaks
{
    /** @brief The fixes, each with its own INI switch under [Fixes]. */
    enum class Fix : std::uint8_t
    {
        /// Keeps e_ViewDistRatio unchanged while a full-screen menu opens, so the scene is not rebuilt.
        MenuViewDistance,
        /// Unlinks a destroyed object through an index instead of a scan of the whole link map.
        LinkIndex,
        /// Refreshes a soul's inventory-wide item flags once per inventory clear instead of once per item.
        InventoryRefresh,
        /// Runs the perk auto-unlock pass once per perk copy instead of once per copied perk.
        PerkCopy,
        /// Simulates only the hours a cached sleep prediction reads.
        SleepPrediction,
        /// Leaves the inventory items out of the soul copy a cached sleep prediction simulates.
        SleepPredictionWithoutItems,
    };

    /** @brief Registers every INI item with DMK::config. Call before the INI loads. */
    void register_config_items();

    /**
     * @brief True when the mod and @p fix are both switched on.
     * @note Callback-safe: two relaxed atomic loads.
     */
    [[nodiscard]] bool fix_enabled(Fix fix) noexcept;

    /**
     * @brief True when every indexed unlink also counts its links with a full walk of the map and logs a mismatch.
     * @note Callback-safe: one relaxed atomic load.
     */
    [[nodiscard]] bool verify_link_index() noexcept;

    /**
     * @brief True when every sleep prediction built without the items is compared hour by hour with builds with them.
     * @note Callback-safe: one relaxed atomic load.
     */
    [[nodiscard]] bool verify_sleep_prediction() noexcept;
} // namespace EngineTweaks

#endif // ENGINETWEAKS_CONFIG_HPP
