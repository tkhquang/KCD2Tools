/**
 * @file hooks/frame_tick.hpp
 * @brief The clock's once-per-frame main-thread call: a hook on CharacterManager::Update(bool paused).
 *
 * CSystem::Update calls it on the main thread every frame, paused or not, so the clock follows the HUD in the pause
 * menu too. Its own hook keeps the mod off ISystem::Update, which another mod already hooks.
 */
#ifndef HUDCLOCK_HOOKS_FRAME_TICK_HPP
#define HUDCLOCK_HOOKS_FRAME_TICK_HPP

#include <DetourModKit.hpp>

#include <cstdint>

namespace HUDClock
{
    /**
     * @brief Hooks CharacterManager::Update at @p target.
     * @return An empty value when the hook is armed, or the Error that refused it.
     * @note Setup only.
     */
    [[nodiscard]] DMK::Result<void> install_frame_tick_hook(std::uintptr_t target, DMK::hook::HookStack &hooks);
} // namespace HUDClock

#endif // HUDCLOCK_HOOKS_FRAME_TICK_HPP
