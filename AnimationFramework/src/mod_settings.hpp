/**
 * @file mod_settings.hpp
 * @brief Settings files for other mods' scripts, kept in each mod's own folder.
 *
 * Game Lua cannot touch files, so a script hands its text over a console variable:
 *   - `System.SetCVar("animfw_settings_text", text)`, then `System.ExecuteCommand("animfw_settings_save my_mod")`
 *     writes Mods/my_mod/my_mod.settings.
 *   - `System.ExecuteCommand("animfw_settings_load my_mod")`, then `System.GetCVar("animfw_settings_text")` reads it,
 *     empty when the mod has no file yet.
 *
 * The commands run at once, so the text is there on the next line of the script. The framework holds no settings: the
 * file belongs to the mod and goes with it.
 */
#ifndef ANIMFW_MOD_SETTINGS_HPP
#define ANIMFW_MOD_SETTINGS_HPP

namespace AnimationFramework
{
    /** @brief Adds the variable and the commands once the console is up. Main thread, every frame. */
    void run_mod_settings_frame_work() noexcept;

    /** @brief Removes the commands, which point into this image. Main thread, teardown. */
    void remove_mod_settings() noexcept;
} // namespace AnimationFramework

#endif // ANIMFW_MOD_SETTINGS_HPP
