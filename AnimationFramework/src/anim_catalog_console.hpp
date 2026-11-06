/**
 * @file anim_catalog_console.hpp
 * @brief The animation catalog for other mods' scripts (core/anim_catalog.hpp), which it builds on the first request.
 *
 * Game Lua cannot call into the framework, so a script talks over a console variable, as it does for its settings. The
 * commands run at once, so the answer is there on the next line of the script:
 *   - `System.SetCVar("animfw_catalog_text", text)`, then
 *     `System.ExecuteCommand("animfw_catalog_find <fragments|all> <first> <count> [seat|horse]")`, then
 *     `System.GetCVar("animfw_catalog_text")` reads one page of the lines that hold the text: their total on the first
 *     line, then the lines. `all` lists each option too, after the fragments, and seat or horse keeps only what has a
 *     version there.
 *   - `System.ExecuteCommand("animfw_catalog_line <fragment>")`, then `GetCVar`, reads one fragment's line, empty when
 *     Henry has no such fragment.
 *   - `System.ExecuteCommand("animfw_catalog_hide <prefix>")` leaves fragments with this name prefix out of searches: a
 *     mod's internal ones, which only it plays.
 */
#ifndef ANIMFW_ANIM_CATALOG_CONSOLE_HPP
#define ANIMFW_ANIM_CATALOG_CONSOLE_HPP

namespace AnimationFramework
{
    /** @brief Adds the variable and the commands once the console is up. Main thread, every frame. */
    void run_anim_catalog_frame_work() noexcept;

    /** @brief Removes the commands, which point into this image, and drops the catalog. Main thread, teardown. */
    void remove_anim_catalog() noexcept;
} // namespace AnimationFramework

#endif // ANIMFW_ANIM_CATALOG_CONSOLE_HPP
