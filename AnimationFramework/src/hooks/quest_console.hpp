/**
 * @file hooks/quest_console.hpp
 * @brief Gives the quest function ExecuteConsoleString a body, so Skald quests can run console commands and Lua.
 *
 * Retail registers wh::conceptmodule::ExecuteConsoleString ("Executes given string in developer console") for Skald
 * `Function` nodes but stores an empty function in its method wrapper. The two invoke slots of that wrapper are hooked:
 * for the length of one call the wrapper's function pointer is the framework's, which hands the command to the console
 * (deferred to its next update, on the main thread). The console runs a command that starts with '#' as Lua.
 */
#ifndef ANIMFW_HOOKS_QUEST_CONSOLE_HPP
#define ANIMFW_HOOKS_QUEST_CONSOLE_HPP

#include "anchors.hpp"

#include <DetourModKit/hook.hpp>

namespace AnimationFramework
{
    /** @brief Installs the two invoke hooks when the wrapper was found. Logs and returns on any failure. */
    void install_quest_console_hooks(const Anchors &anchors, DMK::hook::HookStack &hooks) noexcept;
} // namespace AnimationFramework

#endif // ANIMFW_HOOKS_QUEST_CONSOLE_HPP
