/**
 * @file fmod/listener_hook.hpp
 * @brief Hook on fmodstudio.dll's Studio::System::setListenerAttributes export: the game's per-frame listener update.
 */
#ifndef SNDFW_FMOD_LISTENER_HOOK_HPP
#define SNDFW_FMOD_LISTENER_HOOK_HPP

#include <DetourModKit.hpp>

namespace SoundFramework::fmod
{
    /**
     * @brief Hooks the export and arms it. Each call the game makes for listener 0 then reaches the framework tick with
     *        the game's Studio system and the listener attributes, after the original ran.
     * @return An empty value, or the install error (already logged).
     * @note Setup/control-plane only.
     */
    [[nodiscard]] DMK::Result<void> install_listener_hook(DMK::hook::HookStack &hooks);
} // namespace SoundFramework::fmod

#endif // SNDFW_FMOD_LISTENER_HOOK_HPP
