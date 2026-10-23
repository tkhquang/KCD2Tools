/**
 * @file game/lock_movie.hpp
 * @brief The mod's own Scaleform element, and the vanilla lockpicking prompts among the HUD's control prompts.
 *
 * The game loads the element from the mod's pak, and the mod finds it by name. The mod shows and hides it through the
 * element and drives it through its Flash player's Invoke. The movie (ui/KCD2_OblivionLockpicking.as) only draws, and
 * every rule and timing stays in C++. The vanilla lockpicking prompts leave the HUD while the tumbler lock is up. Call
 * these functions from the lockpicking Update, where the game drives its own lockpicking view.
 */
#ifndef OBLIVIONLP_GAME_LOCK_MOVIE_HPP
#define OBLIVIONLP_GAME_LOCK_MOVIE_HPP

#include "sim/tumbler_sim.hpp"

#include <array>

namespace OblivionLockpicking::game
{
    /** @brief One frame of the lock for the movie's update(). */
    struct MovieFrame
    {
        /// Pick position in tumbler units, 0 at the leftmost tumbler.
        float pick{0.0f};
        /// How far the pick tip is lifted while it kicks a tumbler up, in 1080p pixels.
        float kick{0.0f};
        /// Tumbler heights, 0 at rest to 1 at the top.
        std::array<float, sim::MAX_TUMBLERS> heights{};
        /// True when every tumbler is set and the game turns the lock open, so the gears turn.
        bool opening{false};
        /**
         * @brief True while the game plays its failure and its break roll broke the pick.
         * @details The pick snaps, and a new one slides in when the flag clears.
         */
        bool breaking{false};
        /// Size of the lock, 1 for its 1080p layout.
        float scale{1.0f};
        /// True for a fancy lock, false for a common one.
        bool fancy{false};
        /// Tumblers the player set.
        int set_count{0};
        /// Tumblers the lock needs.
        int total{0};
        /// True when the bar above the lock shows.
        bool show_header{true};
        /// True when the control hints below the lock show.
        bool show_footer{true};
    };

    /** @brief The mod's element, or nullptr when its pak is not installed or the UI anchors did not resolve. */
    [[nodiscard]] void *find_lock_movie() noexcept;

    /** @brief Shows or hides @p element, the result of find_lock_movie(). A show loads a movie the game unloaded. */
    void show_lock_movie(void *element, bool visible) noexcept;

    /** @brief True when the game shows @p element now. The game can hide it and then unloads its movie. */
    [[nodiscard]] bool lock_movie_visible(void *element) noexcept;

    /**
     * @brief Hands one frame to the movie's update().
     * @return False when the element is hidden, has no Flash player, or the call failed. A hidden element is never
     *         called, because the call loads its movie again only for the game to unload it.
     */
    bool update_lock_movie(void *element, const MovieFrame &frame) noexcept;

    /**
     * @brief Calls the movie's begin() at the start of a lock and after each show of the element.
     * @details A hidden element runs no frames, so the movie cannot see a lock end by itself. After begin(), the next
     *          update() reads the header texts again in the game's current language and mends the pick.
     */
    void begin_lock_movie(void *element) noexcept;

    /**
     * @brief Hands the control hint row under the lock (controls::control_hint_row) to the movie's controls().
     * @return False when the element is hidden, has no Flash player, or the call failed.
     */
    bool set_lock_movie_controls(void *element, const char *row) noexcept;

    /**
     * @brief Shows or hides everything @p element's movie draws (its _root), and leaves the element as the game set it.
     * @details The call does nothing while the element is hidden, because its movie can be unloaded and a call loads it
     *          again.
     */
    void show_movie_content(void *element, bool visible) noexcept;

    /**
     * @brief Takes the vanilla minigame's prompts (rotate the lock, move the cursor) off the HUD's control prompts,
     *        and leaves the game's Help and Stop. Call on every frame of a lock.
     * @details The game sends its prompts again on each change, and the HUD reloads after a pause, so one call does not
     *          last. A hidden HUD is never called, because a call loads its movie again.
     */
    void hide_lock_prompts() noexcept;
} // namespace OblivionLockpicking::game

#endif // OBLIVIONLP_GAME_LOCK_MOVIE_HPP
