/**
 * @file hooks/frame_hook.hpp
 * @brief Main-thread per-frame tick: an inline hook on CCryAction::PostUpdate.
 */
#ifndef HOTPAK_FRAME_HOOK_HPP
#define HOTPAK_FRAME_HOOK_HPP

#include <DetourModKit.hpp>

#include <cstdint>

namespace HotPak
{
    /**
     * @brief Installs the PostUpdate hook; each frame then runs reload_tick() after the original.
     * @param hooks The mod's hook stack. The counted detour lets HookStack::retire prove that no frame is inside it.
     * @param target The PostUpdate anchor.
     * @return An empty value, or the failure.
     */
    [[nodiscard]] DMK::Result<void> initialize_frame_hook(DMK::hook::HookStack &hooks, std::uintptr_t target);

} // namespace HotPak

#endif // HOTPAK_FRAME_HOOK_HPP
