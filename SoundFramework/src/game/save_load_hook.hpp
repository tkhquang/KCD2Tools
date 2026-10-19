/**
 * @file game/save_load_hook.hpp
 * @brief The save-load hook: loading a save stops the framework's voice lines and sounds.
 */
#ifndef SNDFW_GAME_SAVE_LOAD_HOOK_HPP
#define SNDFW_GAME_SAVE_LOAD_HOOK_HPP

#include "anchors.hpp"

#include <DetourModKit.hpp>

namespace SoundFramework::game::save_load_hook
{
    /**
     * @brief Hooks SaveGameManager::LoadCryEngineData when its anchors resolved.
     * @return An empty value, or the install error (already logged). Missing anchors only leave the framework's lines
     *         and sounds playing across a save load.
     * @note Setup/control-plane only.
     */
    [[nodiscard]] DMK::Result<void> install(const Anchors &anchors, DMK::hook::HookStack &hooks);
} // namespace SoundFramework::game::save_load_hook

#endif // SNDFW_GAME_SAVE_LOAD_HOOK_HPP
