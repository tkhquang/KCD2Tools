/**
 * @file toggle_thread.hpp
 * @brief Worker body for overlay-driven view state changes.
 */
#ifndef TOGGLE_THREAD_HPP
#define TOGGLE_THREAD_HPP

#include <stop_token>

namespace TPVToggle
{

/**
 * @brief StoppableWorker body that processes overlay-driven view-state changes.
 * @details Waits for the game interface to be ready, then applies the overlay
 *          FPV / TPV-restore requests posted by the UI overlay hooks. Returns
 *          promptly once the stop token is signalled. Key input is handled by
 *          DMK::InputManager callbacks, not here.
 * @param st The worker's stop token.
 */
void overlay_monitor_body(std::stop_token st);

} // namespace TPVToggle

#endif // TOGGLE_THREAD_HPP
