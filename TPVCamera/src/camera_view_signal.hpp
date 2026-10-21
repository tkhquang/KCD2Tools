/**
 * @file camera_view_signal.hpp
 * @brief Publishes the camera view for other mods through the shared `mod_camera_view` console variable.
 *
 * The contract for every mod that uses the variable:
 *   - Values: 0 is first person (the game's default) and 1 is third person. Values from 2 are reserved, and a reader
 *     treats any value of 1 or more as a view of the player's body from outside.
 *   - Find or register: a mod looks the variable up and registers it as an int (value 0, no flags) only when it is
 *     missing. The first mod to load creates it, and no mod owns it.
 *   - The console owns the variable. Its name and help text live in process-heap copies that are never freed, and it
 *     has no change callback, so nothing in the console points into a mod image that a hot reload unmaps.
 *   - Only the active camera mod writes it, and it writes 0 when it unloads.
 *   - Readers read it on the main thread once per frame and never cache the value.
 */
#ifndef TPVCAMERA_CAMERA_VIEW_SIGNAL_HPP
#define TPVCAMERA_CAMERA_VIEW_SIGNAL_HPP

#include <cstdint>

namespace TPVCamera
{
    /**
     * @brief Writes 1 to `mod_camera_view` while @p third_person, else 0, and registers the variable first when no
     *        mod has yet.
     * @details Writes only on a change. The first write finds the variable through checked console slots, and a
     *          failure turns the publisher off for the session (the console code does not change while the game runs).
     * @param genv The game's gEnv, for its console.
     * @note Main thread only (the game-view frustum build calls it every frame).
     */
    void publish_camera_view(std::uintptr_t genv, bool third_person) noexcept;

    /**
     * @brief Writes 0 when the camera last published 1, so the view does not stay third person after the camera goes.
     * @note Teardown only, once the hooks retired. Any thread: the write is one aligned int store, and no console
     *       code runs.
     */
    void reset_camera_view() noexcept;
} // namespace TPVCamera

#endif // TPVCAMERA_CAMERA_VIEW_SIGNAL_HPP
