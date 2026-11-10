/**
 * @file console_vars.cpp
 * @brief Shared console variables and commands (see console_vars.hpp).
 */

#include "console_vars.hpp"
#include "constants.hpp"
#include "game_memory.hpp"

#include <windows.h>

#include <array>
#include <cstring>
#include <string>

namespace AnimationFramework
{
    namespace
    {
        // Set before the frame tick arms and cleared after it retires. Only main-thread code reads them.
        std::uintptr_t s_genv = 0;
        std::uintptr_t s_console_vtable = 0;

        /**
         * @brief A NUL-terminated copy of @p text on the process heap, never freed once the console holds it.
         * @details The console keeps a variable's name and help pointers as given, so they must outlive this image.
         *          One copy per string, made once per process by whichever mod registers the variable.
         */
        [[nodiscard]] char *leaked_copy(std::string_view text) noexcept
        {
            auto *block = static_cast<char *>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, text.size() + 1));
            if (block != nullptr)
            {
                std::memcpy(block, text.data(), text.size());
            }
            return block;
        }

        /** @brief The game's console when its vtable is the one resolved at init, else nullptr. */
        [[nodiscard]] void *console() noexcept
        {
            if (s_genv == 0 || s_console_vtable == 0)
            {
                return nullptr;
            }
            const std::uintptr_t object = read_ptr(s_genv + constants::GENV_CONSOLE_OFFSET);
            return object != 0 && read_ptr(object) == s_console_vtable ? reinterpret_cast<void *>(object) : nullptr;
        }

        /** @brief The variable @p name, or what @p create makes of it when no mod has registered it yet. */
        template <typename Create>
        [[nodiscard]] void *
        find_or_register(std::string_view name, std::string_view help, bool &registered, Create create) noexcept
        {
            registered = false;
            void *console_object = console();
            if (console_object == nullptr || name.empty())
            {
                return nullptr;
            }
            if (void *found = find_cvar(name); found != nullptr)
            {
                return found;
            }
            char *leaked_name = leaked_copy(name);
            char *leaked_help = leaked_copy(help);
            void *created = leaked_name != nullptr && leaked_help != nullptr
                                ? create(console_object, leaked_name, leaked_help)
                                : nullptr;
            if (created == nullptr)
            {
                // The console keeps no pointer to a copy when it creates no variable.
                for (char *block : {leaked_name, leaked_help})
                {
                    if (block != nullptr)
                    {
                        HeapFree(GetProcessHeap(), 0, block);
                    }
                }
                return nullptr;
            }
            registered = true;
            return created;
        }
    } // namespace

    void bind_console(std::uintptr_t genv, std::uintptr_t console_vtable) noexcept
    {
        s_genv = genv;
        s_console_vtable = console_vtable;
    }

    bool console_ready() noexcept
    {
        return console() != nullptr;
    }

    void *find_cvar(std::string_view name) noexcept
    {
        void *console_object = console();
        if (console_object == nullptr || name.empty())
        {
            return nullptr;
        }
        try
        {
            const std::string terminated{name};
            return vcall<void *>(console_object, constants::CONSOLE_GET_CVAR_SLOT, terminated.c_str());
        }
        catch (...)
        {
            return nullptr;
        }
    }

    void *find_or_register_int_cvar(std::string_view name, std::string_view help, bool &registered) noexcept
    {
        return find_or_register(
            name,
            help,
            registered,
            [](void *console_object, const char *leaked_name, const char *leaked_help) -> void *
            {
                return vcall<void *>(
                    console_object,
                    constants::CONSOLE_REGISTER_INT_SLOT,
                    leaked_name,
                    std::int32_t{0},
                    std::int32_t{0},
                    leaked_help,
                    static_cast<void *>(nullptr)
                );
            }
        );
    }

    void *find_or_register_string_cvar(std::string_view name, std::string_view help, bool &registered) noexcept
    {
        return find_or_register(
            name,
            help,
            registered,
            [](void *console_object, const char *leaked_name, const char *leaked_help) -> void *
            {
                return vcall<void *>(
                    console_object,
                    constants::CONSOLE_REGISTER_STRING_SLOT,
                    leaked_name,
                    "",
                    std::int32_t{0},
                    leaked_help,
                    static_cast<void *>(nullptr)
                );
            }
        );
    }

    std::int32_t cvar_int(void *cvar) noexcept
    {
        return cvar != nullptr ? vcall<std::int32_t>(cvar, constants::CVAR_GET_IVAL_SLOT) : 0;
    }

    std::string_view cvar_text(void *cvar) noexcept
    {
        const char *text = cvar != nullptr ? vcall<const char *>(cvar, constants::CVAR_GET_STRING_SLOT) : nullptr;
        return text != nullptr ? std::string_view{text} : std::string_view{};
    }

    void set_cvar_text(void *cvar, const std::string &text) noexcept
    {
        if (cvar != nullptr)
        {
            vcall<void>(cvar, constants::CVAR_SET_STRING_SLOT, text.c_str());
        }
    }

    void add_console_command(const char *name, ConsoleCommand command, const char *help) noexcept
    {
        if (void *console_object = console(); console_object != nullptr)
        {
            vcall<void>(console_object, constants::CONSOLE_ADD_COMMAND_SLOT, name, command, std::int32_t{0}, help);
        }
    }

    void remove_console_command(const char *name) noexcept
    {
        if (void *console_object = console(); console_object != nullptr)
        {
            vcall<void>(console_object, constants::CONSOLE_REMOVE_COMMAND_SLOT, name);
        }
    }

    int command_argument_count(void *args) noexcept
    {
        return vcall<int>(args, constants::COMMAND_ARGS_COUNT_SLOT);
    }

    std::string_view command_argument(void *args, int index) noexcept
    {
        const char *text = vcall<const char *>(args, constants::COMMAND_ARGS_GET_SLOT, index);
        return text != nullptr ? std::string_view{text} : std::string_view{};
    }

    void game_log_line(std::string_view line) noexcept
    {
        if (s_genv == 0 || read_ptr(s_genv + constants::GENV_LOG_GUARD_OFFSET) == 0)
        {
            return;
        }
        void *log = reinterpret_cast<void *>(read_ptr(s_genv + constants::GENV_LOG_OFFSET));
        if (log == nullptr || read_ptr(reinterpret_cast<std::uintptr_t>(log)) == 0)
        {
            return;
        }
        try
        {
            const std::string text{line};
            // A va_list on x64 Windows is a pointer to the arguments, each in an 8-byte slot.
            const std::array<const char *, 1> arguments{text.c_str()};
            vcall<void>(
                log,
                constants::LOG_WRITE_SLOT,
                "KCD2_AnimationFramework",
                std::int32_t{0},
                constants::LOG_TYPE_ALWAYS,
                "%s",
                reinterpret_cast<const char *>(arguments.data())
            );
        }
        catch (...)
        {
        }
    }

    void run_console_command_later(std::string_view command) noexcept
    {
        void *console_object = console();
        if (console_object == nullptr)
        {
            return;
        }
        try
        {
            const std::string text{command};
            vcall<void>(console_object, constants::CONSOLE_EXECUTE_STRING_SLOT, text.c_str(), false, true);
        }
        catch (...)
        {
        }
    }

    void unbind_console() noexcept
    {
        s_genv = 0;
        s_console_vtable = 0;
    }
} // namespace AnimationFramework
