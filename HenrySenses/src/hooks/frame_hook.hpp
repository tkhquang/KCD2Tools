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

#include <DetourModKit/error.hpp>
#include <DetourModKit/hook.hpp>

namespace HenrySenses
{
    /**
     * @brief Installs the PostUpdate hook.
     * @param hooks The mod's hook stack. The counted detour lets HookStack::retire prove that no frame is inside it.
     * @return An empty value, or the Error that refused the install.
     */
    [[nodiscard]] DMK::Result<void> initialize_frame_hook(DMK::hook::HookStack &hooks);

    /**
     * @brief Stops the detour from calling the tick (it still calls the original).
     */
    void disarm_frame_hook() noexcept;

} // namespace HenrySenses

#endif // HENRYSENSES_FRAME_HOOK_HPP
