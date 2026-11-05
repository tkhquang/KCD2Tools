/**
 * @file engine/console_commands.hpp
 * @brief The console command henrysenses_trigger and the variable henrysenses_active, so other mods' Lua scripts can
 *        switch highlight groups through System.ExecuteCommand and read which are on through System.GetCVar.
 *
 *     henrysenses_trigger <Group>[,<Group>...] [pulse|toggle|on|off]
 *
 * <Group> is a [Highlight.<Name>] section name, case-insensitive; trigger_groups() in highlight/groups.hpp owns what
 * each action does. henrysenses_active holds the names of the groups switched on now, comma-separated in INI order.
 *
 * The console keeps a raw pointer to the command function, and the dev build unmaps this code on a hot reload. So the
 * command is added and removed only on the main thread, the thread that runs console commands: none can be running
 * while it is removed. The variable stays registered for the session (its name and help are leaked copies), so a later
 * generation finds it again.
 */
#ifndef HENRYSENSES_CONSOLE_COMMANDS_HPP
#define HENRYSENSES_CONSOLE_COMMANDS_HPP

#include <string_view>

namespace HenrySenses
{
    /**
     * @brief Adds the command and the variable on the first call that finds the game's console.
     * @return True on the call that added them, so the caller publishes henrysenses_active at once.
     * @note Main thread, once per controller tick.
     */
    [[nodiscard]] bool console_commands_tick() noexcept;

    /**
     * @brief Writes @p names into henrysenses_active.
     * @param names The groups switched on now, comma-separated.
     * @note Main thread. Does nothing before console_commands_tick() registered the variable.
     */
    void publish_active_groups(std::string_view names) noexcept;

    /**
     * @brief Removes the command and empties henrysenses_active.
     * @note Main thread (the main-thread half of shutdown).
     */
    void remove_console_commands() noexcept;
} // namespace HenrySenses

#endif // HENRYSENSES_CONSOLE_COMMANDS_HPP
