/**
 * @file clip_check_console.hpp
 * @brief The `animfw_check_clip` console command: plays a clip on Henry's skeleton and prints what breaks it in game
 *        to the console and kcd.log (bones the skeleton lacks, back slots skeleton mods move, bones a full-body clip
 *        leaves to the bind pose, a loop that jumps, root speed, feet that slide).
 */

#ifndef ANIMFW_CLIP_CHECK_CONSOLE_HPP
#define ANIMFW_CLIP_CHECK_CONSOLE_HPP

namespace AnimationFramework
{
    /** @brief Adds the command once the console is up. Main thread, every frame. */
    void run_clip_check_frame_work() noexcept;

    /** @brief Removes the command, which points into this image. Main thread, teardown. */
    void remove_clip_check_console() noexcept;
} // namespace AnimationFramework

#endif // ANIMFW_CLIP_CHECK_CONSOLE_HPP
