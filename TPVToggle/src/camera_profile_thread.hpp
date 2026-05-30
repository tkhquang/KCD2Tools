/**
 * @file camera_profile_thread.hpp
 * @brief Worker body for continuous camera-offset adjustment.
 */
#ifndef CAMERA_PROFILE_THREAD_HPP
#define CAMERA_PROFILE_THREAD_HPP

#include <stop_token>

namespace TPVToggle
{

/**
 * @brief StoppableWorker body for continuous camera-offset adjustment.
 * @details Polls the six offset-direction hold bindings at ~60 Hz and applies
 *          the configured step while a key is held, but only while camera
 *          adjustment mode is enabled. Edge-triggered profile actions (save,
 *          cycle, reset, ...) are handled by DMK::InputManager press callbacks.
 * @param st The worker's stop token.
 */
void camera_profile_body(std::stop_token st);

} // namespace TPVToggle

#endif // CAMERA_PROFILE_THREAD_HPP
