/**
 * @file fmod/update_hook.hpp
 * @brief Hook on fmodstudio.dll's Studio::System::update export: the game's per-frame Studio update.
 */
#ifndef SNDFW_FMOD_UPDATE_HOOK_HPP
#define SNDFW_FMOD_UPDATE_HOOK_HPP

#include <DetourModKit.hpp>

namespace SoundFramework::fmod
{
    /**
     * @brief Hooks the export and arms it. Each update the game runs then hands its Studio system to the engine, so
     *        the framework knows the system from the first frame, also while the listener stands still.
     * @return An empty value, or the install error (already logged). A missing export only logs a warning: the
     *         listener hook still learns the system once the listener moves.
     * @note Setup/control-plane only.
     */
    [[nodiscard]] DMK::Result<void> install_update_hook(DMK::hook::HookStack &hooks);
} // namespace SoundFramework::fmod

#endif // SNDFW_FMOD_UPDATE_HOOK_HPP
