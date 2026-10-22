/**
 * @file fmod/event_hook.hpp
 * @brief Hook on fmodstudio.dll's Studio::EventInstance::start export: every game sound starts there.
 */
#ifndef SNDFW_FMOD_EVENT_HOOK_HPP
#define SNDFW_FMOD_EVENT_HOOK_HPP

#include "fmod/fmod_api.hpp"

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

    /**
     * @brief Starts an event instance the framework created, passing straight through the hook, so a pack sound that
     *        plays a game event is never replaced or logged as a game start.
     * @note One thread at a time: the engine worker. The hook recognizes a single framework start.
     */
    [[nodiscard]] Result start_own_event(void *instance) noexcept;
} // namespace SoundFramework::fmod

#endif // SNDFW_FMOD_EVENT_HOOK_HPP
