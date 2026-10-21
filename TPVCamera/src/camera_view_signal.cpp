/**
 * @file camera_view_signal.cpp
 * @brief The `mod_camera_view` publisher (see camera_view_signal.hpp).
 */

#include "camera_view_signal.hpp"
#include "aob_resolver.hpp"
#include "constants.hpp"
#include "global_state.hpp"
#include "hooks/vtable_slots.hpp"
#include "rtti_types.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace TPVCamera
{
    namespace
    {
        // CXConsole's RegisterInt(name, value, flags, help, change function) and GetCVar(name), each returning the
        // variable or nullptr.
        using RegisterIntFunc = std::uintptr_t(__fastcall *)(std::uintptr_t console, const char *name,
                                                             std::int32_t value, std::int32_t flags, const char *help,
                                                             void *on_change);
        using GetCVarFunc = std::uintptr_t(__fastcall *)(std::uintptr_t console, const char *name);

        // publish_camera_view writes these on the main thread, and reset_camera_view reads them once the hooks
        // retired. The address of the variable's value field, 0 until found. The console keeps the variable for the
        // process.
        std::uintptr_t s_value = 0;
        // Set when the lookup failed. The console code does not change while the game runs, so a retry every frame
        // would only repeat the failure.
        bool s_unavailable = false;
        // The value this generation last wrote, or -1 before the first write.
        std::int32_t s_published = -1;

        /**
         * @brief A NUL-terminated copy of @p text on the process heap, never freed.
         * @details The console keeps a variable's name and help pointers as given, so they must outlive this image.
         */
        [[nodiscard]] const char *leaked_copy(std::string_view text) noexcept
        {
            auto *block = static_cast<char *>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, text.size() + 1));
            if (block != nullptr)
            {
                std::memcpy(block, text.data(), text.size());
            }
            return block;
        }

        /** @brief The pointer-sized value at @p address, 0 when it does not read. */
        [[nodiscard]] std::uintptr_t read_pointer(std::uintptr_t address) noexcept
        {
            const auto value = DMK::memory::read<std::uintptr_t>(DMK::Address{address});
            return value ? *value : 0;
        }

        /**
         * @brief The target of console slot @p slot when it matches @p frame at its first byte or @p body within
         *        @p body_window bytes, else 0.
         */
        [[nodiscard]] std::uintptr_t console_slot(std::uintptr_t vtable, std::size_t slot,
                                                  const DMK::scan::Pattern &frame, const DMK::scan::Pattern &body,
                                                  std::size_t body_window)
        {
            const ModuleInfo &game = module_info();
            const std::uintptr_t target =
                read_checked_vtable_slot(vtable, slot, frame, frame.max_match_length(), game.base, game.size);
            return target != 0 ? target
                               : read_checked_vtable_slot(vtable, slot, body, body_window, game.base, game.size);
        }

        /**
         * @brief Finds the variable, or registers it when no mod has yet, and returns the address of its value, or 0.
         * @details The console must be CXConsole, and its RegisterInt and GetCVar slots must hold the functions that
         *          Aob::k_consoleRegisterInt* and Aob::k_consoleGetCVar* describe. The variable must be a
         *          CXConsoleVariableInt whose GetIVal or GetI64Val reads CVAR_INT_VALUE_OFFSET, so a variable that
         *          another mod registered with another type is refused.
         */
        [[nodiscard]] std::uintptr_t find_or_register(std::uintptr_t genv)
        {
            const std::uintptr_t console = read_pointer(genv + Constants::GENV_CONSOLE_OFFSET);
            const std::uintptr_t console_vtable = console != 0 ? read_pointer(console) : 0;
            if (console_vtable == 0 || !vtable_is(GameClass::Console, console_vtable))
            {
                return 0;
            }
            const auto get_cvar = reinterpret_cast<GetCVarFunc>(
                console_slot(console_vtable, Constants::CONSOLE_VTABLE_GET_CVAR_SLOT, Aob::k_consoleGetCVarFrame,
                             Aob::k_consoleGetCVarLookup, Aob::k_consoleGetCVarLookupWindow));
            const auto register_int = reinterpret_cast<RegisterIntFunc>(console_slot(
                console_vtable, Constants::CONSOLE_VTABLE_REGISTER_INT_SLOT, Aob::k_consoleRegisterIntFrame,
                Aob::k_consoleRegisterIntStore, Aob::k_consoleRegisterIntStoreWindow));
            if (get_cvar == nullptr || register_int == nullptr)
            {
                return 0;
            }
            std::uintptr_t variable = get_cvar(console, Constants::CAMERA_VIEW_CVAR_NAME);
            if (variable == 0)
            {
                const char *name = leaked_copy(Constants::CAMERA_VIEW_CVAR_NAME);
                const char *help = leaked_copy(Constants::CAMERA_VIEW_CVAR_HELP);
                if (name == nullptr || help == nullptr)
                {
                    return 0;
                }
                variable = register_int(console, name, 0, 0, help, nullptr);
                if (variable != 0)
                {
                    (void)DMK::log().try_log(DMK::LogLevel::Info, "CameraView: registered {} for other mods",
                                             Constants::CAMERA_VIEW_CVAR_NAME);
                }
            }
            const std::uintptr_t variable_vtable = variable != 0 ? read_pointer(variable) : 0;
            if (variable_vtable == 0 || !vtable_is(GameClass::ConsoleVariableInt, variable_vtable))
            {
                return 0;
            }
            const ModuleInfo &game = module_info();
            const bool value_proven =
                read_checked_vtable_slot(variable_vtable, Constants::CVAR_INT_VTABLE_GET_IVAL_SLOT,
                                         Aob::k_cvarIntGetIVal, Aob::k_cvarIntGetIVal.max_match_length(), game.base,
                                         game.size) != 0 ||
                read_checked_vtable_slot(variable_vtable, Constants::CVAR_INT_VTABLE_GET_I64VAL_SLOT,
                                         Aob::k_cvarIntGetI64Val, Aob::k_cvarIntGetI64Val.max_match_length(), game.base,
                                         game.size) != 0;
            return value_proven ? variable + Constants::CVAR_INT_VALUE_OFFSET : 0;
        }

        /**
         * @brief Stores @p value in the variable.
         * @details A direct store, not ICVar::Set: Set runs the console's change hooks, which belong to the main
         *          thread, and the teardown write runs on the thread that unloads the mod. No reader needs those
         *          hooks, because readers poll the value every frame.
         */
        void write(std::int32_t value) noexcept
        {
            if (DMK::memory::write_in_place(DMK::Address{s_value}, value))
            {
                s_published = value;
                (void)DMK::log().try_log(DMK::LogLevel::Debug, "CameraView: {} = {}", Constants::CAMERA_VIEW_CVAR_NAME,
                                         value);
            }
        }
    } // namespace

    void publish_camera_view(std::uintptr_t genv, bool third_person) noexcept
    {
        const std::int32_t value = third_person ? 1 : 0;
        if (value == s_published || s_unavailable || genv == 0)
        {
            return;
        }
        if (s_value == 0)
        {
            try
            {
                s_value = find_or_register(genv);
            }
            catch (...)
            {
                s_value = 0;
            }
            if (s_value == 0)
            {
                s_unavailable = true;
                (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                         "CameraView: {} could not be found or registered; other mods do not see the "
                                         "camera view",
                                         Constants::CAMERA_VIEW_CVAR_NAME);
                return;
            }
        }
        write(value);
    }

    void reset_camera_view() noexcept
    {
        if (s_published == 1)
        {
            write(0);
        }
    }
} // namespace TPVCamera
