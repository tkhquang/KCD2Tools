/**
 * @file fmod/parameter_hook.hpp
 * @brief Hook on fmodstudio.dll's Studio::EventInstance::setParameterByName export: the game's parameter changes on
 *        running events.
 */
#ifndef SNDFW_FMOD_PARAMETER_HOOK_HPP
#define SNDFW_FMOD_PARAMETER_HOOK_HPP

#include <DetourModKit.hpp>

namespace SoundFramework::fmod
{
    /**
     * @brief Hooks the export and arms it. Each parameter the game sets on an event then reaches the replacement table
     *        and the event log before the original call runs.
     * @return An empty value, or the install error (already logged). A missing export only logs a warning: replacements
     *         with PlayAt then never play, and every other replacement works as before.
     * @note Setup/control-plane only.
     */
    [[nodiscard]] DMK::Result<void> install_parameter_hook(DMK::hook::HookStack &hooks);
} // namespace SoundFramework::fmod

#endif // SNDFW_FMOD_PARAMETER_HOOK_HPP
