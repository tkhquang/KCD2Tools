/**
 * @file hooks/hook_install.hpp
 * @brief Installs one counted inline hook into the mod's HookStack with its trampoline published before the arm.
 */
#ifndef TPVCAMERA_HOOK_INSTALL_HPP
#define TPVCAMERA_HOOK_INSTALL_HPP

#include <DetourModKit/error.hpp>
#include <DetourModKit/hook.hpp>

#include <atomic>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace TPVCamera
{

    /**
     * @brief Installs one counted inline hook on @p target, stores it in @p hooks, and publishes its trampoline before
     *        the arm.
     * @details The detour can run as soon as enable() arms the patch, so @p original holds the trampoline first. The
     *          stack stores the hook before the arm, so a hook whose arm fails with the patch live stays owned through
     *          teardown. DMK counts every call into @p Detour, so HookStack::retire can prove that no game thread still
     *          runs it or returns through the trampoline.
     * @tparam Detour The detour: noexcept, with trivially copyable parameters and result.
     * @param hooks The mod's hook stack.
     * @param name The hook name for DMK diagnostics. A later generation with the same name claims the position.
     * @param target The resolved function entry.
     * @param original Receives the trampoline that calls the original function.
     * @return An empty value when the hook is armed, or the library Error that refused the create or the arm.
     * @throws std::bad_alloc If the hook name or the hook stack cannot allocate.
     * @note Setup/control-plane only.
     */
    template <auto Detour, typename Func>
    [[nodiscard]] DMK::Result<void> install_inline_hook(DMK::hook::HookStack &hooks, std::string_view name,
                                                        std::uintptr_t target, std::atomic<Func> &original)
    {
        auto result = DMK::hook::inline_at<Detour>(DMK::hook::InlineRequest{
            .name = std::string{name},
            .target = DMK::Address{target},
        });
        if (!result.has_value())
        {
            return std::unexpected(result.error());
        }
        original.store(result->template original<Func>(), std::memory_order_release);
        if (auto armed = hooks.push(std::move(*result)).enable(); !armed.has_value())
        {
            return std::unexpected(armed.error());
        }
        return {};
    }

} // namespace TPVCamera

#endif // TPVCAMERA_HOOK_INSTALL_HPP
