/**
 * @file fixes/fixes.hpp
 * @brief The hook installers of every fix and the teardown of their state.
 *
 * Each installer hooks only when its anchors resolved. The detours read the fix's INI switch on every call, so a fix
 * switched off in the INI calls straight through to the game's code.
 */
#ifndef ENGINETWEAKS_FIXES_FIXES_HPP
#define ENGINETWEAKS_FIXES_FIXES_HPP

#include "anchors.hpp"

#include <DetourModKit.hpp>

namespace EngineTweaks::fixes
{
    /**
     * @brief Keeps e_ViewDistRatio at its value when a full-screen menu opens.
     * @details The game sets e_ViewDistRatio to 100 when it enters full-screen UI mode and restores it at the exit. Any
     *          change of that cvar makes CryEngine re-register every object of the level, rebuild the terrain buffers
     *          and reset the vegetation groups at the next frame, which costs hundreds of milliseconds in a dense area.
     *          The detour sets the value back right after the game's own switch, inside the same call, so the scene
     *          refresh never sees a change. The exit restores the value the game saved, which is that same
     *          value, so it changes nothing either.
     */
    [[nodiscard]] DMK::Result<void> install_menu_view_distance(const Anchors &anchors, DMK::hook::HookStack &hooks);

    /**
     * @brief Replaces the link map's whole-map unlink scan with an endpoint index.
     * @details Every item is a linkable object with a link in C_LinkablesManager's map, and the destruction of one
     *          scans the whole map for its links. The destruction of every item of a copied soul therefore costs items
     *          times links. The index lists, per endpoint, the nodes that hold it. The insert and erase(node) hooks
     *          keep it current, and a node count that disagrees with the map's size (after the map's whole-tree clear
     *          path, for example) rebuilds it from one walk of the tree before it is used.
     */
    [[nodiscard]] DMK::Result<void> install_link_index(const Anchors &anchors, DMK::hook::HookStack &hooks);

    /** @brief Forgets the link index. Call only after the hooks retired. */
    void reset_link_index() noexcept;

    /**
     * @brief Defers C_InventorySoul's item-flag refresh to the end of an inventory clear.
     * @details Each item removal makes the soul's inventory refresh two flags from every remaining item, so an
     *          inventory clear is quadratic in its items. Inside C_Inventory::Clear on the same thread, the refresh is
     *          only recorded, and it runs once when the outermost clear returns, from the final contents.
     */
    [[nodiscard]] DMK::Result<void> install_inventory_refresh(const Anchors &anchors, DMK::hook::HookStack &hooks);

    /**
     * @brief Runs the perk auto-unlock pass once per perk container copy.
     * @details The copy adds every perk through AddPerk, and AddPerk runs the auto-unlock pass over every perk
     *          definition after each add. The detour sets the container's own suspend bit for the copy, then runs the
     *          pass until it unlocks nothing more.
     */
    [[nodiscard]] DMK::Result<void> install_perk_copy(const Anchors &anchors, DMK::hook::HookStack &hooks);

    /**
     * @brief Simulates only the hours a cached sleep prediction reads.
     * @details The bed prompt asks C_RPGUtils for the prediction of one hour of sleep, which builds a 24-hour table on
     *          a copy of the player's soul and reads its first entry. Inside that request, the hour steps past the
     *          requested hour return the copy unchanged. The prediction cache remembers how many hours each request
     *          shape built. A later request of the same shape that needs more hours comes from the sleep or wait
     *          slider, so it rebuilds the whole table once and the cache answers the next hours. With
     *          SleepPredictionWithoutItems, the copy also leaves the inventory items out: the clone and destruction of
     *          every item is most of the cost that remains, and VerifySleepPrediction compares both results.
     */
    [[nodiscard]] DMK::Result<void> install_sleep_prediction(const Anchors &anchors, DMK::hook::HookStack &hooks);

    /**
     * @brief Makes the next sleep prediction rebuild its table, so no shortened table outlives this generation.
     * @note Call only after the hooks retired.
     */
    void reset_sleep_prediction() noexcept;
} // namespace EngineTweaks::fixes

#endif // ENGINETWEAKS_FIXES_FIXES_HPP
