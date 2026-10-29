/**
 * @file game/main_thread.hpp
 * @brief The main-thread hook on CSystem::Update: once per frame, before the game's update, it runs the work that must
 *        happen on the game's main thread (console command registration, entity tracking).
 *
 * Another mod that hooks the same function can write its jump over the framework's patch with a copy of the bytes from
 * before it, so no frame reaches the framework again. A watchdog sees the frames stop while a foreign patch opens the
 * function and hooks it again on top of that patch.
 */
#ifndef SNDFW_GAME_MAIN_THREAD_HPP
#define SNDFW_GAME_MAIN_THREAD_HPP

#include "anchors.hpp"

#include <DetourModKit.hpp>

namespace SoundFramework::game::main_thread
{
    /**
     * @brief Hooks CSystem::Update, arms it and starts the watchdog.
     * @param hooks The stack that owns the hook. The watchdog pushes its later hooks onto it until @ref stop.
     * @return An empty value, or the install error (already logged).
     * @note Setup/control-plane only; configure the commands and the entity tracker first.
     */
    [[nodiscard]] DMK::Result<void> start(const Anchors &anchors, DMK::hook::HookStack &hooks);

    /**
     * @brief Stops and joins the watchdog, so it adds no hook to the stack.
     * @note Setup/control-plane only. Call it before the stack retires. Idempotent.
     */
    void stop() noexcept;

    /** @brief True on the thread that runs CSystem::Update (false until its first frame). */
    [[nodiscard]] bool is_main_thread() noexcept;
} // namespace SoundFramework::game::main_thread

#endif // SNDFW_GAME_MAIN_THREAD_HPP
