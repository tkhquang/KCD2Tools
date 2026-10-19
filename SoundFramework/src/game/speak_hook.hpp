/**
 * @file game/speak_hook.hpp
 * @brief The speak manager's Speak hook: every voice line the game plays (dialog, barks, wh_snd_playfile) passes
 *        through it.
 */
#ifndef SNDFW_GAME_SPEAK_HOOK_HPP
#define SNDFW_GAME_SPEAK_HOOK_HPP

#include "anchors.hpp"

#include <DetourModKit.hpp>

#include <string_view>

namespace SoundFramework::game::speak_hook
{
    /**
     * @brief Hooks Speak.
     * @note Setup/control-plane only.
     */
    [[nodiscard]] DMK::Result<void> install(const Anchors &anchors, DMK::hook::HookStack &hooks);

    /** @brief Logs each spoken line whose file or text key contains @p filter ("*" = all, empty = off). */
    void set_log_filter(std::string_view filter);
} // namespace SoundFramework::game::speak_hook

#endif // SNDFW_GAME_SPEAK_HOOK_HPP
