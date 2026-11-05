/**
 * @file fixes/menu_view_distance.cpp
 * @brief The full-screen UI mode switch detour (see fixes.hpp).
 */

#include "config.hpp"
#include "constants.hpp"
#include "fixes/diag.hpp"
#include "fixes/fixes.hpp"
#include "hooks/hook_install.hpp"

#include <DetourModKit.hpp>

#include <atomic>
#include <cstdint>

namespace EngineTweaks::fixes
{
    namespace
    {
        using FullUiModeSwitchFunc = std::uintptr_t(__fastcall *)(std::uint8_t *mode);
        using CvarGetFloatFunc = float(__fastcall *)(void *cvar);
        using CvarSetFloatFunc = void(__fastcall *)(void *cvar, float value);

        std::atomic<FullUiModeSwitchFunc> s_original{nullptr};

        /** @brief The e_ViewDistRatio ICVar the switch sets, or nullptr. */
        [[nodiscard]] void *view_distance_cvar(const std::uint8_t *mode) noexcept
        {
            const auto helper = *reinterpret_cast<const std::uintptr_t *>(mode + constants::FULL_UI_MODE_HELPER_OFFSET);
            if (helper == 0)
            {
                return nullptr;
            }
            return *reinterpret_cast<void *const *>(helper + constants::FULL_UI_HELPER_VIEW_DIST_CVAR_OFFSET);
        }

        [[nodiscard]] void *const *vtable_of(void *object) noexcept
        {
            return *reinterpret_cast<void *const *const *>(object);
        }

        /**
         * @brief The switch detour: the game's switch, then e_ViewDistRatio back to its value from before an enter.
         * @note Main thread, on each full-screen menu open and close. No-throw.
         */
        std::uintptr_t __fastcall detour_full_ui_mode_switch(std::uint8_t *mode) noexcept
        {
            const FullUiModeSwitchFunc original = s_original.load(std::memory_order_acquire);
            // Only the enter path (flag byte set) raises the cvar. The exit path restores the value the enter saved,
            // which the fix kept unchanged, so it passes through as is.
            const bool enter = mode != nullptr && mode[0] != 0;
            const bool enabled = fix_enabled(Fix::MenuViewDistance);
            void *const cvar = enter && enabled ? view_distance_cvar(mode) : nullptr;
            if (cvar == nullptr)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Trace,
                    "MenuViewDistance: switch passes through ({})",
                    !enter     ? "exit"
                    : !enabled ? "fix off"
                               : "no cvar"
                );
                return original(mode);
            }
            diag::counters().menu_enters.add();
            const auto get_float = reinterpret_cast<CvarGetFloatFunc>(vtable_of(cvar)[constants::CVAR_GET_FLOAT_SLOT]);
            const float before = get_float(cvar);
            const std::uintptr_t result = original(mode);
            const float raised = get_float(cvar);
            const bool restore = raised != before;
            if (restore)
            {
                const auto set_float =
                    reinterpret_cast<CvarSetFloatFunc>(vtable_of(cvar)[constants::CVAR_SET_FLOAT_SLOT]);
                set_float(cvar, before);
                diag::counters().menu_restores.add();
            }
            (void)DMK::log().try_log(
                DMK::LogLevel::Debug,
                "MenuViewDistance: menu entered, the game set e_ViewDistRatio {} -> {}{}",
                before,
                raised,
                restore ? ", set back" : ""
            );
            diag::maybe_log_summary();
            return result;
        }
    } // namespace

    DMK::Result<void> install_menu_view_distance(const Anchors &anchors, DMK::hook::HookStack &hooks)
    {
        if (!anchors.menu_view_distance)
        {
            return {};
        }
        return install_inline_hook<&detour_full_ui_mode_switch>(
            hooks,
            "FullUiModeSwitch",
            anchors.full_ui_mode_switch,
            s_original
        );
    }
} // namespace EngineTweaks::fixes
