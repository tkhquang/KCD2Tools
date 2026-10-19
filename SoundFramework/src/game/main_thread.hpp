/**
 * @file game/main_thread.hpp
 * @brief The main-thread hook on CSystem::Update: once per frame, before the game's update, it runs the work that must
 *        happen on the game's main thread (console command registration, entity tracking).
 */
#ifndef SNDFW_GAME_MAIN_THREAD_HPP
#define SNDFW_GAME_MAIN_THREAD_HPP

#include "anchors.hpp"

#include <DetourModKit.hpp>

namespace SoundFramework::game::main_thread
{
    /**
     * @brief Hooks CSystem::Update and arms it.
     * @return An empty value, or the install error (already logged).
     * @note Setup/control-plane only; configure the commands and the entity tracker first.
     */
    [[nodiscard]] DMK::Result<void> start(const Anchors &anchors, DMK::hook::HookStack &hooks);

    /** @brief True on the thread that runs CSystem::Update (false until its first frame). */
    [[nodiscard]] bool is_main_thread() noexcept;
} // namespace SoundFramework::game::main_thread

#endif // SNDFW_GAME_MAIN_THREAD_HPP
