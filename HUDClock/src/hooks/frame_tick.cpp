/**
 * @file hooks/frame_tick.cpp
 * @brief The CharacterManager::Update detour (see frame_tick.hpp).
 */

#include "hooks/frame_tick.hpp"
#include "clock.hpp"
#include "hooks/hook_install.hpp"

#include <DetourModKit.hpp>

#include <atomic>
#include <cstdint>

namespace HUDClock
{
    namespace
    {
        using CharacterManagerUpdateFunc = void(__fastcall *)(void *manager, bool paused);
        std::atomic<CharacterManagerUpdateFunc> s_update_original{nullptr};

        /**
         * @brief CharacterManager::Update detour: the game's update, then the clock's frame.
         * @note Main thread, once per frame. No-throw.
         */
        void __fastcall detour_character_manager_update(void *manager, bool paused) noexcept
        {
            const CharacterManagerUpdateFunc original = s_update_original.load(std::memory_order_acquire);
            if (original != nullptr)
            {
                original(manager, paused);
            }
            clock::run_frame();
        }
    } // namespace

    DMK::Result<void> install_frame_tick_hook(std::uintptr_t target, DMK::hook::HookStack &hooks)
    {
        if (target == 0)
        {
            return std::unexpected(DMK::Error{DMK::ErrorCode::NoMatch, "HUDClock::frame_tick/target"});
        }
        return install_inline_hook<&detour_character_manager_update>(
            hooks,
            "CharacterManagerUpdate",
            target,
            s_update_original
        );
    }
} // namespace HUDClock
