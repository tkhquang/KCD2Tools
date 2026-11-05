/**
 * @file game/main_thread.hpp
 * @brief The main-thread tick in CSystem::Update: once per frame, before the game's update, it runs the work that must
 *        happen on the game's main thread (console command registration, entity tracking).
 *
 * The tick is a mid hook at CSystem::Update + 0x51, not an inline hook on the entry. The Lua runner kcd2lua finds the
 * function by a signature over its first 81 bytes and patches the entry with MinHook. A patch on the entry therefore
 * breaks that scan or loses to the MinHook patch. Only the prologue runs before the site, and the site precedes the
 * frame-start update of the timer, so the frame work runs once per call at the start of the frame.
 */
#ifndef SNDFW_GAME_MAIN_THREAD_HPP
#define SNDFW_GAME_MAIN_THREAD_HPP

#include "anchors.hpp"

#include <DetourModKit.hpp>

namespace SoundFramework::game::main_thread
{
    /**
     * @brief Installs and arms the main-thread tick at Anchors::system_update_tick.
     * @return An empty value, or the install error (already logged).
     * @note Setup/control-plane only; configure the commands and the entity tracker first.
     */
    [[nodiscard]] DMK::Result<void> start(const Anchors &anchors, DMK::hook::HookStack &hooks);

    /** @brief True on the thread that runs CSystem::Update (false until its first frame). */
    [[nodiscard]] bool is_main_thread() noexcept;
} // namespace SoundFramework::game::main_thread

#endif // SNDFW_GAME_MAIN_THREAD_HPP
