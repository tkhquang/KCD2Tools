/**
 * @file game/cutscene_voices.hpp
 * @brief Voice-driven mouths for cutscene voice lines.
 *
 * Cutscenes play their voice lines through the FMOD event constants::CINEMATIC_VOICE_EVENT from shared audio entities,
 * never through an actor's speak manager, and their English mouths are baked into each actor's cutscene animation. The
 * game keeps no link from a line to the actor who says it, so the cutscene speaker table (packs/cutscene_table.hpp)
 * supplies it. When the game's lines get voice-driven mouths (speech::drives_game_lines()), each cutscene line drives
 * the faces its table rows name.
 */
#ifndef SNDFW_GAME_CUTSCENE_VOICES_HPP
#define SNDFW_GAME_CUTSCENE_VOICES_HPP

#include "anchors.hpp"

#include <filesystem>

namespace SoundFramework::game::cutscene_voices
{
    /**
     * @brief Loads the cutscene speaker table when the CutsceneVoices feature passed its gate: @p override_file when it
     *        exists, else the table built into this module. Without a table, cutscenes keep the game's faces (logged).
     * @note Setup/control-plane only.
     */
    void configure(const Anchors &anchors, const std::filesystem::path &override_file) noexcept;

    /**
     * @brief Resolves the cutscene voice event's GUID on the game's Studio system @p system once its strings bank
     *        knows the event path, retrying at most once a second until then.
     * @note Called from the FMOD update hook on the game's audio thread, every audio frame.
     */
    void on_studio_system(void *system) noexcept;

    /**
     * @brief Queues the voice file of a starting cutscene voice event for the next frame. Any other event costs one
     *        GUID comparison.
     * @note Called from the FMOD start hook on the game's audio thread, before the original start.
     */
    void on_event_start(void *instance) noexcept;

    /**
     * @brief Starts the faces of the queued lines, and ends the lines whose events stopped.
     * @note Main thread only.
     */
    void on_frame() noexcept;

    /**
     * @brief Drops the table, the queue and the running lines; later event starts pass through.
     * @note Setup/control-plane only, after the FMOD hooks are disarmed.
     */
    void shutdown() noexcept;
} // namespace SoundFramework::game::cutscene_voices

#endif // SNDFW_GAME_CUTSCENE_VOICES_HPP
