/**
 * @file hooks/quest_console.cpp
 * @brief The ExecuteConsoleString invoke detours (see quest_console.hpp).
 */

#include "hooks/quest_console.hpp"
#include "console_vars.hpp"
#include "constants.hpp"
#include "game_memory.hpp"
#include "hooks/hook_install.hpp"

#include <DetourModKit.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <string_view>

namespace AnimationFramework
{
    namespace
    {
        /// Both invoke slots: (wrapper, result variant, instance, argument or argument list) -> result variant.
        using InvokeFunc = void *(__fastcall *)(void *wrapper, void *result, void *instance, void *arguments);
        std::atomic<InvokeFunc> s_invoke_original{nullptr};
        std::atomic<InvokeFunc> s_invoke_list_original{nullptr};
        std::atomic<std::uintptr_t> s_method_vtable{0};
        /// The game's own function in the wrapper, read on the first call, put back after every call.
        std::atomic<void *> s_native_function{nullptr};

        /**
         * @brief True when @p code is an empty function: `ret` (C3) or `ret 0` (C2 00 00, retail's nullsub_1).
         * @details The pointer comes from the wrapper, where another mod can store its own, so the read is guarded.
         */
        [[nodiscard]] bool is_empty_function(const void *code) noexcept
        {
            const auto bytes = read_value<std::array<std::uint8_t, 3>>(reinterpret_cast<std::uintptr_t>(code));
            return bytes.has_value() &&
                   ((*bytes)[0] == 0xC3 || ((*bytes)[0] == 0xC2 && (*bytes)[1] == 0x00 && (*bytes)[2] == 0x00));
        }

        /** @brief The body: CryStringT holds one pointer to its characters. Deferred to the console's next update. */
        void __fastcall execute_console_string(const char *const *command) noexcept
        {
            if (command == nullptr || *command == nullptr || **command == '\0')
            {
                return;
            }
            const std::string_view text{*command};
            (void)DMK::log().try_log(DMK::LogLevel::Info, "QuestConsole: {}", text);
            run_console_command_later(text);
        }

        /**
         * @brief Runs one invoke with the framework's body in the wrapper when the game's own is empty.
         * @details A second thread that finds the body already in place runs it as is; the first one puts the game's
         *          function back when its call returns. Nothing in the game points at the framework between calls.
         */
        void *
        invoke_with_body(InvokeFunc original, void *wrapper, void *result, void *instance, void *arguments) noexcept
        {
            if (original == nullptr)
            {
                return result;
            }
            bool placed = false;
            void **slot = nullptr;
            if (wrapper != nullptr &&
                *static_cast<const std::uintptr_t *>(wrapper) == s_method_vtable.load(std::memory_order_acquire))
            {
                slot = reinterpret_cast<void **>(
                    static_cast<char *>(wrapper) + constants::CONSOLE_STRING_METHOD_FUNCTION_OFFSET
                );
                std::atomic_ref<void *> function{*slot};
                void *const current = function.load(std::memory_order_acquire);
                void *const body = reinterpret_cast<void *>(&execute_console_string);
                if (current != body && current != nullptr && is_empty_function(current))
                {
                    void *expected = nullptr;
                    s_native_function.compare_exchange_strong(expected, current, std::memory_order_acq_rel);
                    function.store(body, std::memory_order_release);
                    placed = true;
                }
            }
            void *const returned = original(wrapper, result, instance, arguments);
            if (placed)
            {
                std::atomic_ref<void *>{*slot}.store(
                    s_native_function.load(std::memory_order_acquire),
                    std::memory_order_release
                );
            }
            return returned;
        }

        void *__fastcall detour_invoke(void *wrapper, void *result, void *instance, void *argument) noexcept
        {
            return invoke_with_body(
                s_invoke_original.load(std::memory_order_acquire),
                wrapper,
                result,
                instance,
                argument
            );
        }

        void *__fastcall detour_invoke_list(void *wrapper, void *result, void *instance, void *arguments) noexcept
        {
            return invoke_with_body(
                s_invoke_list_original.load(std::memory_order_acquire),
                wrapper,
                result,
                instance,
                arguments
            );
        }
    } // namespace

    void install_quest_console_hooks(const Anchors &anchors, DMK::hook::HookStack &hooks) noexcept
    {
        DMK::Logger &logger = DMK::log();
        if (anchors.console_string_method_vtable == 0 || anchors.console_string_invoke == 0 ||
            anchors.console_string_invoke_list == 0)
        {
            (void)logger.log_noexcept(
                DMK::LogLevel::Warning,
                "QuestConsole: ExecuteConsoleString was not found; quests cannot run console commands"
            );
            return;
        }
        try
        {
            s_method_vtable.store(anchors.console_string_method_vtable, std::memory_order_release);
            auto invoked = install_inline_hook<&detour_invoke>(
                hooks,
                "ConsoleStringInvoke",
                anchors.console_string_invoke,
                s_invoke_original
            );
            if (!invoked.has_value())
            {
                logger.warning("QuestConsole: invoke hook failed ({})", invoked.error().message());
                return;
            }
            auto listed = install_inline_hook<&detour_invoke_list>(
                hooks,
                "ConsoleStringInvokeList",
                anchors.console_string_invoke_list,
                s_invoke_list_original
            );
            if (!listed.has_value())
            {
                logger.warning("QuestConsole: invoke-list hook failed ({})", listed.error().message());
                return;
            }
            logger.info("QuestConsole: quests' ExecuteConsoleString runs console commands");
        }
        catch (...)
        {
            (void)logger.log_noexcept(DMK::LogLevel::Error, "QuestConsole: installing the invoke hooks failed");
        }
    }
} // namespace AnimationFramework
