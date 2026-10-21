/**
 * @file game/speech.hpp
 * @brief Makes an actor speak a voice file through the game's own speech system, with a subtitle.
 *
 * The game plays a dialogue line through the speaking actor's speak manager: the voice file comes from the
 * localization paks, and the manager drives the facial (lip-sync) stream and the AI's hearing of the line. Subtitles
 * are a separate UI call. Both are reached through the game context; every object on the way is checked against its
 * RTTI vtable before it is called.
 */
#ifndef SNDFW_GAME_SPEECH_HPP
#define SNDFW_GAME_SPEECH_HPP

#include "anchors.hpp"
#include "audio/lip_model.hpp"
#include "face/mood.hpp"
#include "packs/sound_def.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace SoundFramework::game::speech
{
    /** @brief [LipSync] DubbedVoices: when the framework animates the mouths of the game's own voice lines. */
    enum class DubbedVoices : std::uint8_t
    {
        /// When the voice language has no facial animation of its own, so the game would show English mouths.
        Auto,
        /// Always, in place of the game's own lip-sync animation.
        On,
        /// Never: the game's lines keep its own animation.
        Off,
    };

    /**
     * @brief Enables speech when its anchors resolved, and lists the voice languages that have facial animation.
     * @param facials_directory `<game root>/Data/Facials`, where the game looks for each voice language's facial
     *        animation pak.
     * @note Setup/control-plane only; call before the main-thread hook is armed.
     */
    void configure(const Anchors &anchors, const std::filesystem::path &facials_directory) noexcept;

    /**
     * @brief Makes the entity named @p entity_name speak @p voice_path and shows @p subtitle for the line's length.
     * @param voice_path A localization path without extension, as the game's dialogue uses
     *        (`Localization/dialog/<folder>/<line id>`).
     * @param speaker The subtitle's speaker name as a localization key (for example `soul_ui_name_guard`); empty for
     *        no name. The UI shows a key it cannot resolve as "@key".
     * @param bubble Localization key of the text for the game's floating chat bubble above the speaker's head (shown
     *        within wh_dlg_ChatBubblesDistance when wh_dlg_ChatBubbles is on, never during the player's dialogue);
     *        empty for no bubble.
     * @param subtitle Text for the bottom subtitle; empty for none.
     * @param subtitle_is_key True when @p subtitle is a localization key the UI translates.
     * @param mood Pose weights the face holds while the line plays (upper face only when the game animates the
     *        mouth); empty for none.
     * @param mood_strength Scales every mood weight.
     * @return False when the entity, its actor or the speech system is unavailable (logged). True also when the line
     *         waits: a speaker in the middle of a line finishes it first, as the game's own lines do, and a line still
     *         waiting after 10 s is dropped (logged).
     * @note Main thread only (console commands and the main-thread hook run there).
     */
    [[nodiscard]] bool
    say(const std::string &entity_name,
        const std::string &voice_path,
        const std::string &speaker,
        const std::string &bubble,
        const std::string &subtitle,
        bool subtitle_is_key = false,
        const std::vector<face::PoseWeight> &mood = {},
        float mood_strength = 1.0f);

    /**
     * @brief Makes the entity named @p entity_name say the pack voice line named @p line_name: picks a take (never the
     *        previous one when there is a choice), honours the line's cooldown, and shows its text as the line's
     *        Display asks.
     * @return False when there is no such line, it is cooling down, or say() failed (logged).
     * @note Main thread only.
     */
    [[nodiscard]] bool say_line(const std::string &entity_name, const std::string &line_name);

    /**
     * @brief Loads @p model for voice-driven mouths in advanced mode (null keeps the basic spectral mouth) and starts
     *        the worker that decodes lines and computes their mouths.
     * @note Setup/control-plane only.
     */
    void set_lip_model(std::shared_ptr<const audio::LipModel> model) noexcept;

    /**
     * @brief Chooses the learned model (advanced, needs a loaded model) or the basic spectral mouth for the lines that
     *        start from now on.
     * @note Thread safe; the INI sets it, also on a live reload.
     */
    void set_lip_advanced(bool advanced) noexcept;

    /**
     * @brief Shifts voice-driven mouth movement: positive moves the mouth earlier than the sound, negative later.
     * @note Thread safe; the INI and the sfw_lipsync_offset command set it.
     */
    void set_lip_offset_ms(int offset_ms) noexcept;

    /**
     * @brief Sets [LipSync] DubbedVoices for the lines that start from now on.
     * @note Thread safe. The INI sets it, also on a live reload.
     */
    void set_dubbed_voices(DubbedVoices mode) noexcept;

    /**
     * @brief Whether the framework animates the mouths of the game's own voice lines now: [LipSync] DubbedVoices, and
     *        for AUTO whether the voice language has facial animation of its own.
     * @note Main thread only, from the Speak hook, before the game's Speak runs.
     */
    [[nodiscard]] bool drives_game_lines();

    /**
     * @brief After the game's Speak played its own line @p voice_path through @p speak_man: the mouth from the voice,
     *        in place of the game's lip-sync animation. A speaker farther from the camera than the engine animates
     *        faces keeps the game's face.
     * @note Main thread only, from the Speak hook, when drives_game_lines() said so.
     */
    void follow_game_line(const void *speak_man, const std::string &voice_path);

    /**
     * @brief Mood @p name: its poses (a live override from sfw_mood_override, else the moods file and packs) and its
     *        MaxStrength; nothing for an unknown mood.
     * @note Main thread only.
     */
    [[nodiscard]] std::optional<packs::MoodDef> find_mood(std::string_view name);

    /**
     * @brief Replaces (or creates) mood @p name's poses until the framework reloads (authoring aid); an empty list
     *        removes the override.
     * @note Main thread only.
     */
    void override_mood(std::string_view name, std::vector<face::PoseWeight> poses);

    /** @brief A game voice line about to be played as a pack line (ReplacesVoice), with its take's length. */
    struct Swap
    {
        std::string line;
        /// The take's localization path, without extension.
        std::string voice;
        /// The take's text key, or empty.
        std::string text_key;
        bool bubble = false;
        bool subtitle = false;
        std::string speaker;
        std::vector<face::PoseWeight> mood;
        float mood_strength = 1.0f;
        std::optional<std::uint32_t> length_ms;
    };

    /**
     * @brief The pack line standing in for the game line with text key @p text_key: picks a take (never the previous
     *        one) and reads its length; nothing when no pack line replaces it or that line is cooling down.
     * @note Main thread only, from the Speak hook, before the game's Speak runs.
     */
    [[nodiscard]] std::optional<Swap> prepare_swap(std::string_view text_key);

    /**
     * @brief The game's dialog system is about to make @p speak_man say a conversation line: a framework line playing
     *        or waiting on that speaker stops now (and gives up its subtitle), so the conversation line plays at once.
     * @note Main thread only, from the Speak hook, before the game's Speak runs.
     */
    void yield_to_dialog(void *speak_man) noexcept;

    /**
     * @brief After the game's Speak played @p swap through @p speak_man: the face performance and the subtitle.
     * @note Main thread only, from the Speak hook.
     */
    void finish_swap(const void *speak_man, const Swap &swap);

    /**
     * @brief A save is loading: every framework line stops (its sound, face and subtitle), at once on the main thread
     *        and otherwise on the next frame.
     * @note Thread safe; called from the save-load hook, which runs only when the speaker's stop resolved.
     */
    void on_save_load() noexcept;

    /**
     * @brief Stops the lip model worker (joins it) and drops the model.
     * @note Setup/control-plane only; call before the module unloads.
     */
    void shutdown() noexcept;

    /**
     * @brief Ends the framework lines whose sound stopped, attaches finished mouths and clears a subtitle whose time is
     *        up.
     * @note Main thread only.
     */
    void on_frame() noexcept;
} // namespace SoundFramework::game::speech

#endif // SNDFW_GAME_SPEECH_HPP
