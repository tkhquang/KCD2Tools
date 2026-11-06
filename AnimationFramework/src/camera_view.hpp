/**
 * @file camera_view.hpp
 * @brief Reads the shared `mod_camera_view` console variable: the camera view the player sees.
 *
 * The contract (on top of the shared-variable rules in console_vars.hpp):
 *   - Values: 0 first person (the game's default), 1 third person. Values from 2 are reserved. A reader treats any
 *     value of 1 or more as "the player's body is seen from outside".
 *   - Only the active camera mod writes it, and it writes 0 when it turns off or unloads.
 */
#ifndef ANIMFW_CAMERA_VIEW_HPP
#define ANIMFW_CAMERA_VIEW_HPP

#include <cstdint>

namespace AnimationFramework
{
    /** @brief The camera view, as the variable holds it. */
    enum class CameraView : std::int32_t
    {
        FirstPerson = 0,
        ThirdPerson = 1,
    };

    /**
     * @brief Resets the reader (bind_console() first). Without a bound console every read reports first person.
     * @note Setup only.
     */
    void bind_camera_view() noexcept;

    /**
     * @brief Reads the variable (registering it first when no mod has yet) and logs each change of view.
     * @return The current view. Any value of 1 or more reads as ThirdPerson.
     * @note Main thread only (the frame tick calls it).
     */
    [[nodiscard]] CameraView poll_camera_view() noexcept;

    /** @brief Forgets the cached variable. Teardown only, once the hooks have retired. */
    void unbind_camera_view() noexcept;
} // namespace AnimationFramework

#endif // ANIMFW_CAMERA_VIEW_HPP
