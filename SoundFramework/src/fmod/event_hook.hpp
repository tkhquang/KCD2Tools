/**
 * @file fmod/event_hook.hpp
 * @brief Hook on fmodstudio.dll's Studio::EventInstance::start export: every game sound starts there.
 */
#ifndef SNDFW_FMOD_EVENT_HOOK_HPP
#define SNDFW_FMOD_EVENT_HOOK_HPP

#include <DetourModKit.hpp>

namespace SoundFramework::fmod
{
    /**
     * @brief Hooks the export and arms it. Each event start then reaches the replacement table and the event log
     *        before the original start runs.
     * @return An empty value, or the install error (already logged).
     * @note Setup/control-plane only.
     */
    [[nodiscard]] DMK::Result<void> install_event_hook(DMK::hook::HookStack &hooks);
} // namespace SoundFramework::fmod

#endif // SNDFW_FMOD_EVENT_HOOK_HPP
