/**
 * @file game/console_commands.hpp
 * @brief The framework's console commands (sfw_play, sfw_stop, sfw_stop_all, sfw_reload, sfw_list), which also give
 *        Lua a way in through System.ExecuteCommand.
 *
 * The console stores a raw pointer to each command function, and this code lives in a DLL that hot reload unmaps.
 * So the commands are added and removed only on the game's main thread, the thread that runs console commands, from the
 * main-thread hook (game/main_thread): no command of the framework can be running while it is removed.
 */
#ifndef SNDFW_GAME_CONSOLE_COMMANDS_HPP
#define SNDFW_GAME_CONSOLE_COMMANDS_HPP

#include "anchors.hpp"

#include <chrono>

namespace SoundFramework::game::commands
{
    /**
     * @brief Arms the commands: the main thread adds them on the next frame that has a console.
     * @note Setup/control-plane only; call before the main-thread hook is armed.
     */
    void configure(const Anchors &anchors) noexcept;

    /**
     * @brief Adds or removes the commands as wanted.
     * @param system The CSystem whose Update the main-thread hook is running.
     * @note Main thread only, from the main-thread hook.
     */
    void on_frame(void *system) noexcept;

    /**
     * @brief Asks the main thread to remove the commands and waits for it.
     * @return True when no command of the framework is registered any more; false when the main thread did not run
     *         within @p deadline, in which case the framework must stay loaded.
     * @note Never call it from the main thread.
     */
    [[nodiscard]] bool remove_commands(std::chrono::milliseconds deadline) noexcept;
} // namespace SoundFramework::game::commands

#endif // SNDFW_GAME_CONSOLE_COMMANDS_HPP
