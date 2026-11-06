/**
 * @file hooks/frame_tick.hpp
 * @brief The framework's once-per-frame main-thread call: a hook on CharacterManager::Update(bool paused).
 *
 * CSystem::Update calls it every frame on the main thread, after the stream engine and before the game and entity
 * updates that start the animation jobs, so the previous frame's jobs are done. The framework's per-frame work (the
 * console requests, the late clip registration, the camera-view read and the custom tags) runs there, before the
 * game's own update. A hook of its own keeps the framework off ISystem::Update, a target other mods hook.
 */
#ifndef ANIMFW_HOOKS_FRAME_TICK_HPP
#define ANIMFW_HOOKS_FRAME_TICK_HPP

#include <DetourModKit.hpp>

#include <chrono>
#include <cstdint>

namespace AnimationFramework
{
    /**
     * @brief Hooks CharacterManager::Update at @p target.
     * @return An empty value when the hook is armed, or the Error that refused it.
     * @throws std::bad_alloc If the hook name cannot allocate.
     * @note Setup only.
     */
    [[nodiscard]] DMK::Result<void> install_frame_tick_hook(std::uintptr_t target, DMK::hook::HookStack &hooks);

    /**
     * @brief Has the frame tick remove every console command of the framework, and waits for it.
     * @details The console reads its command table and runs every command on the main thread. A removal from another
     *          thread races both, and a command that still runs returns into an image the dev loader unmaps. The tick
     *          removes the commands between two frames and then stops its per-frame work, so no command comes back
     *          before the hooks retire. Before the first frame no command exists, and the call returns at once.
     * @param deadline How long to wait for the main thread to run a frame.
     * @return True once no command of the framework remains. False when no frame ran in time: the request is
     *         withdrawn, and the commands and the per-frame work stay.
     * @note Setup/control-plane only. Never call it on the main thread, which it waits for.
     */
    [[nodiscard]] bool remove_console_commands(std::chrono::milliseconds deadline) noexcept;

    /**
     * @brief Restarts the per-frame work that remove_console_commands stopped. The next frame adds the commands again.
     * @note Setup/control-plane only, after a hook retirement that changed nothing.
     */
    void resume_frame_work() noexcept;
} // namespace AnimationFramework

#endif // ANIMFW_HOOKS_FRAME_TICK_HPP
