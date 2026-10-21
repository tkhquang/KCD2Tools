/**
 * @file audio/engine.hpp
 * @brief Plays the catalog's sounds through the game's FMOD: on game buses, at world positions, with variants,
 *        volume and pitch variation, instance limits and pack hot reload.
 *
 * Requests may come from any thread; a worker of the engine's own carries them out, because the game pushes the FMOD
 * listener only when it changes and so offers no audio-side frame clock.
 */
#ifndef SNDFW_AUDIO_ENGINE_HPP
#define SNDFW_AUDIO_ENGINE_HPP

#include "fmod/fmod_api.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace SoundFramework::audio
{
    /** @brief A position in CryEngine world coordinates (metres, z up). */
    struct WorldPosition
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    /**
     * @brief Records the game's Studio system and listener and mirrors the listener to the core system.
     * @note Called from the listener hook on the game's audio thread, after the original update.
     */
    void on_listener(void *system, const fmod::Attributes3d &listener) noexcept;

    /**
     * @brief Records the game's Studio system, so voice lines can open their recordings before the listener first
     *        moves.
     * @note Called from the update hook on the game's audio thread every audio frame; it returns at once while
     *       @p system is the one already recorded.
     */
    void on_system(void *system) noexcept;

    /**
     * @brief Loads @p defaults_file (the framework's default moods) and the packs under @p mods_dir, then starts the
     *        worker.
     * @return False when the worker failed to start.
     */
    [[nodiscard]] bool start(std::filesystem::path mods_dir, std::filesystem::path defaults_file);

    /** @brief Joins the worker, then stops every sound the engine started and releases everything it holds. */
    void shutdown() noexcept;

    /**
     * @brief Plays the sound named @p name. A 3D sound plays at @p at, or at the listener without it.
     * @note Thread safe; carried out by the worker within a frame or two.
     */
    void request_play(std::string name, std::optional<WorldPosition> at);

    /**
     * @brief Plays the sound with catalog key @p key in place of the game event instance @p event, at @p attributes
     *        (FMOD space) or at the listener, then keeps it on the event's position while the event lives.
     * @return False when the request queue is full (logged); the caller then leaves the game's event audible.
     * @note Thread safe; called from the event start hook.
     */
    [[nodiscard]] bool request_replacement(std::string key, void *event, std::optional<fmod::Attributes3d> attributes);

    /**
     * @brief Where the game's listener (its camera) was last put, in world coordinates, or nothing before the game
     *        first set it.
     * @note Thread safe.
     */
    [[nodiscard]] std::optional<WorldPosition> listener_position() noexcept;

    /**
     * @brief The length of the audio file at @p game_path, opened through the game's file system (pak paths), or
     *        nothing when FMOD cannot open it.
     * @note Thread safe; opens the file header only.
     */
    [[nodiscard]] std::optional<std::uint32_t> length_ms(const std::string &game_path);

    /** @brief A decoded recording: mono samples in -1..1 and their rate. */
    struct DecodedAudio
    {
        std::vector<float> samples;
        float sample_rate = 0.0f;
    };

    /**
     * @brief Decodes the audio file at @p game_path (game file system) to mono; empty when FMOD cannot decode it.
     * @note Thread safe; decodes the whole file, which takes a while for a long take, so call it once per line and
     *       off the main thread.
     */
    [[nodiscard]] DecodedAudio decode_mono(const std::string &game_path);

    /** @brief Stops every playing instance of the sound named @p name. Thread safe. */
    void request_stop(std::string name);

    /** @brief Stops every sound the engine plays. Thread safe. */
    void request_stop_all();

    /** @brief Stops everything, then reads every pack again. Thread safe. */
    void request_reload();

    /** @brief Writes the catalog to the log. Thread safe. */
    void request_list();
} // namespace SoundFramework::audio

#endif // SNDFW_AUDIO_ENGINE_HPP
