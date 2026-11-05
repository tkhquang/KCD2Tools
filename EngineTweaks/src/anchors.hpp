/**
 * @file anchors.hpp
 * @brief The WHGame.dll functions the fixes hook or call, and the fixes they enable.
 *
 * Every function resolves at startup from several independent rungs: its own entry bytes, a body sequence resolved to
 * its entry through the unwind table, a call site in a caller, or a vtable slot. Every rung that resolves must name the
 * same function. A rung that proves a layout a fix reads is mandatory for that fix.
 */
#ifndef ENGINETWEAKS_ANCHORS_HPP
#define ENGINETWEAKS_ANCHORS_HPP

#include <DetourModKit.hpp>

#include <cstdint>

namespace EngineTweaks
{
    /** @brief The resolved functions and values, 0 when unresolved, and the fixes they enable. */
    struct Anchors
    {
        /// C_UIFullUIModeHelper's switch into and out of full-screen UI mode.
        std::uintptr_t full_ui_mode_switch{0};
        /// C_LinkablesManager's unlink of one object: an erase_if over the whole link map.
        std::uintptr_t link_unlink_scan{0};
        /**
         * @brief The byte that makes the unlink stop after its first erase. The fix replaces the unlink only while it
         *        is 0.
         */
        std::uintptr_t link_unlink_first_only_flag{0};
        /// The link map's erase(node).
        std::uintptr_t link_map_erase_node{0};
        /// The link map's try_emplace.
        std::uintptr_t link_map_insert{0};
        /// C_Inventory::Clear.
        std::uintptr_t inventory_clear{0};
        /// C_InventorySoul's refresh of the soul's inventory-wide item flags.
        std::uintptr_t inventory_soul_refresh{0};
        /// The perk container copy.
        std::uintptr_t perk_copy{0};
        /// The perk auto-unlock pass.
        std::uintptr_t perk_auto_unlock{0};
        /// C_RPGUtils's cached skip-time prediction.
        std::uintptr_t sleep_prediction{0};
        /// The RPG parameter block's guarded getter, called by the sleep prediction itself.
        std::uintptr_t rpg_params_getter{0};
        /// The divisor that turns game seconds into hours in the sleep prediction.
        float seconds_per_hour{0.0f};
        /// The prediction cache's validity check, which proves the cache stamp the fix clears.
        std::uintptr_t sleep_prediction_cache_check{0};
        /// C_SoulSimulator's step of one simulated hour.
        std::uintptr_t soul_sim_step{0};
        /// Clones a source inventory's items into a destination inventory, as part of a soul copy.
        std::uintptr_t inventory_clone_items{0};

        bool menu_view_distance{false};
        bool link_index{false};
        bool inventory_refresh{false};
        bool perk_copy_fix{false};
        bool sleep_prediction_fix{false};
        bool sleep_prediction_items_fix{false};
    };

    /**
     * @brief Resolves every anchor inside @p game, logs each one, and gates each fix on the anchors it needs.
     * @note Setup/control-plane only.
     */
    [[nodiscard]] Anchors resolve_anchors(DMK::Region game);
} // namespace EngineTweaks

#endif // ENGINETWEAKS_ANCHORS_HPP
