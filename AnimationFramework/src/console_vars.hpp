/**
 * @file console_vars.hpp
 * @brief Console variables shared with other mods, found or registered, then read, and console commands.
 *
 * The contract, shared by every mod that uses such a variable (mod_camera_view, the custom tag variables):
 *   - Find or register: a mod looks the variable up first and registers it only when it is missing (value 0, no
 *     flags), so whichever loads first creates it and nobody owns it.
 *   - The console owns the value. The name and help text live in a process-heap buffer that is never freed, and no
 *     change callback is set, so nothing in the console points into a mod's image that a hot reload unmaps.
 *   - Register, set and read on the main thread. Readers read once per frame and never cache the value.
 */
#ifndef ANIMFW_CONSOLE_VARS_HPP
#define ANIMFW_CONSOLE_VARS_HPP

#include <DetourModKit/defines.hpp>
#include <DetourModKit/logger.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace AnimationFramework
{
    /**
     * @brief Binds to the game's console. Without a console that matches @p console_vtable, nothing is found or read.
     * @note Setup only.
     */
    void bind_console(std::uintptr_t genv, std::uintptr_t console_vtable) noexcept;

    /** @brief Whether the console is bound and up. Main thread only. */
    [[nodiscard]] bool console_ready() noexcept;

    /**
     * @brief The variable @p name, registered with value 0, no flags and @p help when no mod has yet. Nullptr while
     *        the console is not up or when registering fails.
     * @param registered Set to true when this call registered it.
     * @note Main thread only.
     */
    [[nodiscard]] void *
    find_or_register_int_cvar(std::string_view name, std::string_view help, bool &registered) noexcept;

    /** @brief The integer value of @p cvar (from find_or_register_int_cvar). Main thread only. */
    [[nodiscard]] std::int32_t cvar_int(void *cvar) noexcept;

    /** @brief As find_or_register_int_cvar, for a text variable that starts empty. */
    [[nodiscard]] void *
    find_or_register_string_cvar(std::string_view name, std::string_view help, bool &registered) noexcept;

    /** @brief The text of @p cvar, valid until it next changes. Main thread only. */
    [[nodiscard]] std::string_view cvar_text(void *cvar) noexcept;

    /** @brief Sets the text of @p cvar. Main thread only. */
    void set_cvar_text(void *cvar, const std::string &text) noexcept;

    /** @brief What the console calls a command with: the IConsoleCmdArgs. */
    using ConsoleCommand = void(__fastcall *)(void *args);

    /**
     * @brief The console's entry for command body @p Work: an exception from it never reaches the console.
     * @tparam Work The command body, which receives the IConsoleCmdArgs.
     * @tparam Failure The error line logged when @p Work throws.
     */
    template <void (*Work)(void *), const char *Failure> void __fastcall guarded_command(void *args) noexcept
    {
        try
        {
            Work(args);
        }
        catch (...)
        {
            (void)DMK::log().log_noexcept(DMK::LogLevel::Error, Failure);
        }
    }

    /**
     * @brief Adds a command. Remove it before the image goes away.
     * @note Main thread only: the console reads its command table there and runs every command there.
     */
    void add_console_command(const char *name, ConsoleCommand command, const char *help) noexcept;

    /** @brief Removes a command. Main thread only, for the reason add_console_command gives. */
    void remove_console_command(const char *name) noexcept;

    /** @brief How many arguments a command got, its own name being the first. */
    [[nodiscard]] int command_argument_count(void *args) noexcept;

    /** @brief The command's argument @p index, empty when there is none. */
    [[nodiscard]] std::string_view command_argument(void *args, int index) noexcept;

    /** @brief Writes @p line to the game's log, which shows it in the console and kcd.log. Main thread. */
    void game_log_line(std::string_view line) noexcept;

    /** @brief Has the console run @p command on its next update, as if typed. Main thread. */
    void run_console_command_later(std::string_view command) noexcept;

    /** @brief Forgets the console binding. Teardown only, once the hooks have retired. */
    void unbind_console() noexcept;
} // namespace AnimationFramework

#endif // ANIMFW_CONSOLE_VARS_HPP
