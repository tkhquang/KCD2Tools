/**
 * @file hooks/frame_hook.hpp
 * @brief Main-thread per-frame tick: an inline hook on CCryAction::PostUpdate.
 *
 * PostUpdate runs once per frame on the game main thread (menus and loading screens included), after the game
 * update and before the frame's world render and the renderer's end-of-frame aux-geom buffer swap. The detour calls
 * the original first, then controller_tick(), so entity writes and aux-geom draws happen on the thread the engine
 * expects.
 */
#ifndef HENRYSENSES_FRAME_HOOK_HPP
#define HENRYSENSES_FRAME_HOOK_HPP

#include "hooks/hook_set.hpp"

#include <DetourModKit/error.hpp>

namespace HenrySenses
{
    /**
     * @brief Installs the PostUpdate hook.
     * @param hooks The mod's hook set.
     * @return An empty value, or the Error that refused the install.
     */
    [[nodiscard]] DMK::Result<void> initialize_frame_hook(HookSet &hooks);

    /**
     * @brief Stops the detour from calling the tick (it still calls the original).
     */
    void disarm_frame_hook() noexcept;

    /**
     * @brief Waits (bounded) until no thread is inside the detour; call after the hook was disabled and before it
     *        is destroyed.
     * @return True when the detour drained; false means a thread may still return through the trampoline.
     */
    [[nodiscard]] bool rundown_frame_hook() noexcept;

} // namespace HenrySenses

#endif // HENRYSENSES_FRAME_HOOK_HPP
