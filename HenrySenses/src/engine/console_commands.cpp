/**
 * @file engine/console_commands.cpp
 * @brief henrysenses_trigger and henrysenses_active, added and removed on the main thread.
 */

#include "engine/console_commands.hpp"
#include "constants.hpp"
#include "engine/engine_env.hpp"
#include "engine/guarded_call.hpp"
#include "highlight/groups.hpp"
#include "rtti_types.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <exception>
#include <optional>
#include <string>
#include <string_view>

namespace HenrySenses
{
    namespace
    {
        /// The console's command signature: void (*)(IConsoleCmdArgs *).
        using CommandFn = void (*)(void *args);

        constexpr const char *TRIGGER_COMMAND = "henrysenses_trigger";
        constexpr const char *TRIGGER_USAGE = "henrysenses_trigger <Group>[,<Group>...] [pulse|toggle|on|off]";
        constexpr const char *TRIGGER_HELP =
            "Switches Henry's Senses highlight groups.\n"
            "Usage: henrysenses_trigger <Group>[,<Group>...] [pulse|toggle|on|off]\n"
            "<Group> is a [Highlight.<Name>] section name. With no second word each group does what its key does.";
        constexpr const char *ACTIVE_CVAR = "henrysenses_active";
        constexpr const char *ACTIVE_CVAR_HELP =
            "The Henry's Senses highlight groups switched on now, comma-separated. Henry's Senses rewrites it.";

        /// The console the command was added to. Main thread only, like every state below.
        std::uintptr_t s_console = 0;
        /// henrysenses_active, or 0 when it is unavailable.
        std::uintptr_t s_active_cvar = 0;
        /// The last value written into henrysenses_active.
        std::string s_published{};
        /// The command is registered.
        bool s_registered = false;
        /// The console failed its identity check or refused the command, so the command stays off.
        bool s_refused = false;

        /** @brief Argument @p index of the console's argument list, empty when the call fails or it has none. */
        [[nodiscard]] std::string_view arg(std::uintptr_t args, int index) noexcept
        {
            const std::optional<const char *> text =
                vcall<const char *>(args, constants::COMMAND_ARGS_VTABLE_GET_ARG_OFFSET, index);
            return text.has_value() && *text != nullptr ? std::string_view{*text} : std::string_view{};
        }

        /** @brief Joins arguments [first, last) with single spaces, so a group name with spaces works unquoted. */
        [[nodiscard]] std::string joined_args(std::uintptr_t args, int first, int last)
        {
            std::string text;
            for (int index = first; index < last; ++index)
            {
                if (!text.empty())
                {
                    text += ' ';
                }
                text += arg(args, index);
            }
            return text;
        }

        /** @brief A NUL-terminated copy of @p text on the process heap, never freed: the console keeps the pointer. */
        [[nodiscard]] const char *leaked_copy(std::string_view text) noexcept
        {
            auto *block = static_cast<char *>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, text.size() + 1));
            if (block != nullptr)
            {
                std::memcpy(block, text.data(), text.size());
            }
            return block;
        }

        /**
         * @brief henrysenses_active, registered by the first image that asks and found again by every later one.
         * @details The variable outlives this image (a dev hot reload unmaps it), so its name and help live on the
         *          process heap.
         * @return The variable, or 0 when it cannot be registered or is not a string variable.
         */
        [[nodiscard]] std::uintptr_t find_or_register_active_cvar(std::uintptr_t console) noexcept
        {
            std::uintptr_t cvar =
                vcall<std::uintptr_t>(console, constants::CONSOLE_VTABLE_GET_CVAR_OFFSET, ACTIVE_CVAR).value_or(0);
            if (cvar == 0)
            {
                const char *const name = leaked_copy(ACTIVE_CVAR);
                const char *const help = leaked_copy(ACTIVE_CVAR_HELP);
                if (name == nullptr || help == nullptr)
                {
                    return 0;
                }
                cvar = vcall<std::uintptr_t>(
                           console,
                           constants::CONSOLE_VTABLE_REGISTER_STRING_OFFSET,
                           name,
                           "",
                           0,
                           help,
                           static_cast<void *>(nullptr)
                )
                           .value_or(0);
            }
            return object_is(GameClass::CvarString, cvar) ? cvar : 0;
        }

        /** @brief Writes @p text into henrysenses_active. */
        [[nodiscard]] bool set_active_text(const char *text) noexcept
        {
            return vcall_void(s_active_cvar, constants::CVAR_VTABLE_SET_STRING_OFFSET, text);
        }

        /** @brief The henrysenses_trigger command. The console runs it on the main thread. */
        void trigger_command(void *engine_args) noexcept
        {
            const auto args = reinterpret_cast<std::uintptr_t>(engine_args);
            try
            {
                if (!object_is(GameClass::ConsoleCommandArgs, args))
                {
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Error,
                        "henrysenses_trigger: the argument list is not a CConsoleCommandArgs"
                    );
                    return;
                }
                const int count = vcall<int>(args, constants::COMMAND_ARGS_VTABLE_GET_ARG_COUNT_OFFSET).value_or(0);
                if (count < 2)
                {
                    (void)DMK::log().try_log(DMK::LogLevel::Warning, "henrysenses_trigger: usage: {}", TRIGGER_USAGE);
                    return;
                }
                // The last word is the action when it names one; everything between is the group list.
                TriggerAction action = TriggerAction::Default;
                int names_end = count;
                if (count >= 3)
                {
                    if (const std::optional<TriggerAction> parsed = parse_trigger_action(arg(args, count - 1));
                        parsed.has_value())
                    {
                        action = *parsed;
                        names_end = count - 1;
                    }
                }
                trigger_groups(joined_args(args, 1, names_end), action);
            }
            catch (const std::exception &error)
            {
                (void)DMK::log().try_log(DMK::LogLevel::Error, "henrysenses_trigger: {}", error.what());
            }
            catch (...)
            {
                (void)DMK::log().log_noexcept(DMK::LogLevel::Error, "henrysenses_trigger: unknown error");
            }
        }
    } // namespace

    bool console_commands_tick() noexcept
    {
        if (s_registered || s_refused)
        {
            return false;
        }
        const std::uintptr_t console = genv_interface(constants::GENV_CONSOLE_OFFSET);
        if (console == 0)
        {
            return false;
        }
        if (!object_is(GameClass::Console, console))
        {
            s_refused = true;
            (void)DMK::log().try_log(
                DMK::LogLevel::Error,
                "Console: gEnv's console at {:#x} is not a CXConsole; henrysenses_trigger stays off",
                console
            );
            return false;
        }
        // AddCommand refuses a registered name. An earlier image whose teardown never reached the main thread stays
        // mapped with its command registered, so its command is removed first and this image's replaces it.
        (void)vcall<bool>(console, constants::CONSOLE_VTABLE_REMOVE_COMMAND_OFFSET, TRIGGER_COMMAND);
        if (!vcall_void(
                console,
                constants::CONSOLE_VTABLE_ADD_COMMAND_OFFSET,
                TRIGGER_COMMAND,
                static_cast<CommandFn>(&trigger_command),
                0,
                TRIGGER_HELP
            ))
        {
            s_refused = true;
            (void)DMK::log().try_log(DMK::LogLevel::Error, "Console: AddCommand failed; henrysenses_trigger stays off");
            return false;
        }
        s_console = console;
        s_registered = true;
        s_active_cvar = find_or_register_active_cvar(console);
        s_published.clear();
        if (s_active_cvar != 0 && !set_active_text(""))
        {
            s_active_cvar = 0;
        }
        (void)DMK::log().try_log(
            DMK::LogLevel::Info,
            "Console: henrysenses_trigger added{} (main thread {})",
            s_active_cvar != 0 ? ", henrysenses_active registered" : "; henrysenses_active unavailable",
            GetCurrentThreadId()
        );
        return true;
    }

    void publish_active_groups(std::string_view names) noexcept
    {
        if (s_active_cvar == 0 || names == s_published)
        {
            return;
        }
        try
        {
            s_published.assign(names);
        }
        catch (...)
        {
            s_published.clear();
            return;
        }
        // A failed write leaves the cache empty, so the next change retries.
        if (!set_active_text(s_published.c_str()))
        {
            s_published.clear();
        }
    }

    void remove_console_commands() noexcept
    {
        if (!s_registered)
        {
            return;
        }
        if (s_active_cvar != 0)
        {
            (void)set_active_text("");
        }
        (void)vcall<bool>(s_console, constants::CONSOLE_VTABLE_REMOVE_COMMAND_OFFSET, TRIGGER_COMMAND);
        s_console = 0;
        s_active_cvar = 0;
        s_published.clear();
        s_registered = false;
        (void)DMK::log().try_log(DMK::LogLevel::Info, "Console: henrysenses_trigger removed");
    }
} // namespace HenrySenses
