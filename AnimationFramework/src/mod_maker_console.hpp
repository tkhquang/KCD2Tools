/**
 * @file mod_maker_console.hpp
 * @brief The mod maker's console commands. `animfw_make_mod` adds an action to a mod being made in the DEV folder (a
 *        new fragment that plays a clip, set up like a game fragment), checks its clip and reloads the databases so the
 *        action plays at once. `animfw_pack_mod` writes the finished mod (mod.manifest and its pak).
 */

#ifndef ANIMFW_MOD_MAKER_CONSOLE_HPP
#define ANIMFW_MOD_MAKER_CONSOLE_HPP

namespace AnimationFramework
{
    /** @brief Adds the commands once the console is up. Main thread, every frame. */
    void run_mod_maker_frame_work() noexcept;

    /** @brief Removes the commands, which point into this image. Main thread, teardown. */
    void remove_mod_maker_console() noexcept;
} // namespace AnimationFramework

#endif // ANIMFW_MOD_MAKER_CONSOLE_HPP
