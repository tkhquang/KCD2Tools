/**
 * @file tpv_toggle.hpp
 * @brief Mod lifecycle entry points driven by DMK::Bootstrap.
 */
#ifndef TPV_TOGGLE_HPP
#define TPV_TOGGLE_HPP

namespace TPVToggle
{

/**
 * @brief Initializes the whole mod: config, hooks, input bindings, workers.
 * @details Runs on the Bootstrap worker thread (off the loader lock). Loads and
 *          logs configuration, validates the game module, installs hooks,
 *          registers input bindings, starts the poll workers, and enables INI
 *          hot-reload.
 * @return true on success; false if a critical subsystem failed.
 */
[[nodiscard]] bool init();

/**
 * @brief Tears the mod down: stops workers, removes hooks, clears bindings.
 * @details Does not call DMK_Shutdown(); DMK::Bootstrap owns that ordering and
 *          invokes it after this returns.
 */
void shutdown();

} // namespace TPVToggle

#endif // TPV_TOGGLE_HPP
