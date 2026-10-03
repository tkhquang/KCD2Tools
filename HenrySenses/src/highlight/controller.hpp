/**
 * @file highlight/controller.hpp
 * @brief The main-thread brain: group activation and fade, gates, the loot scan schedule and markers.
 *
 * controller_tick() runs once per frame on the game main thread (after CCryAction::PostUpdate). It turns the group
 * hotkey state (pulse, toggle, hold) and the HideIn situations into loot scans (detect/loot_scanner.hpp) every
 * ScanIntervalMs, applies their results, advances the container index walk, publishes the per-frame silhouette
 * parameters, re-asserts the always-visible bookkeeping, and queues the aux-geom markers. While no group can show and
 * nothing is left to take down, a tick only notices the keys, a saved INI and a level change, keeps the indexes a first
 * press relies on current, and lets a focus release finish. It also performs the main-thread half of shutdown.
 */
#ifndef HENRYSENSES_CONTROLLER_HPP
#define HENRYSENSES_CONTROLLER_HPP

#include <DetourModKit/error.hpp>

#include <chrono>
#include <cstdint>

namespace HenrySenses
{
    /**
     * @brief Resets the controller state.
     * @return An empty value.
     */
    [[nodiscard]] DMK::Result<void> initialize_controller();

    /**
     * @brief Runs one main-thread step. Never throws.
     * @note Main thread only (the CCryAction::PostUpdate hook).
     */
    void controller_tick() noexcept;

    /**
     * @brief Reports a saved INI, so the next tick scans again at once.
     * @note Any thread (the INI watcher calls it).
     */
    void controller_notify_config_changed() noexcept;

    /**
     * @enum MainThreadShutdown
     * @brief Outcome of the main-thread half of shutdown.
     */
    enum class MainThreadShutdown : std::uint8_t
    {
        /// The tick restored every node.
        Completed,
        /// No tick ever ran, so nothing was applied.
        NeverTicked,
        /**
         * @brief The tick did not answer in time. The request was withdrawn, so no later tick runs it; the nodes keep
         * their words and always-visible state.
         */
        Abandoned,
        /// A tick claimed the work but did not finish it in time.
        Unfinished,
    };

    /**
     * @brief Asks the main thread to restore every node.
     * @details Node writes and 3D-engine calls are main-thread only, so the teardown thread never
     *          does them itself; on a timeout it withdraws the request instead.
     * @param timeout How long to wait for the tick to acknowledge (and, once claimed, to finish).
     * @return The outcome.
     * @note Teardown thread.
     */
    [[nodiscard]] MainThreadShutdown controller_request_shutdown(std::chrono::milliseconds timeout) noexcept;

} // namespace HenrySenses

#endif // HENRYSENSES_CONTROLLER_HPP
