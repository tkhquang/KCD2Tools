/**
 * @file hooks/hook_install.hpp
 * @brief Installs and arms one counted inline hook in the mod's HookStack in one call.
 */
#ifndef ENGINETWEAKS_HOOKS_HOOK_INSTALL_HPP
#define ENGINETWEAKS_HOOKS_HOOK_INSTALL_HPP

#include <DetourModKit/error.hpp>
#include <DetourModKit/hook.hpp>
#include <DetourModKit/logger.hpp>

#include <atomic>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace EngineTweaks
{

    /**
     * @brief Installs one counted inline hook on @p target into @p hooks and arms it.
     * @details HookStack::install creates the hook, stores the trampoline into @p original, stores the hook, and arms
     *          it under one process coordinator hold. The detour therefore never runs before @p original holds the
     *          trampoline. A failed install leaves @p hooks unchanged. DMK counts every call into @p Detour, so
     *          HookStack::retire can prove that no game thread still runs it or returns through the trampoline.
     * @tparam Detour The detour: noexcept, with trivially copyable parameters and result.
     * @param hooks The mod's hook stack.
     * @param name The hook name for DMK diagnostics. A later generation with the same name claims the position.
     * @param target The resolved function entry.
     * @param original Receives the trampoline that calls the original function.
     * @return An empty value when the hook is armed, or the library Error that refused the create or the arm.
     * @throws std::bad_alloc If the copy of @p name cannot allocate.
     * @note Setup/control-plane only.
     */
    template <auto Detour, typename Func>
    [[nodiscard]] DMK::Result<void> install_inline_hook(
        DMK::hook::HookStack &hooks,
        std::string_view name,
        std::uintptr_t target,
        std::atomic<Func> &original
    )
    {
        auto installed = hooks.install<Detour>(DMK::hook::InlineInstallRequest{
            .name = std::string{name},
            .target = DMK::Address{target},
            .original = &original,
        });
        if (!installed.has_value())
        {
            return std::unexpected(installed.error());
        }
        (void)DMK::log().try_log(DMK::LogLevel::Info, "Hook {}: armed at 0x{:X}", name, target);
        return {};
    }

} // namespace EngineTweaks

#endif // ENGINETWEAKS_HOOKS_HOOK_INSTALL_HPP
