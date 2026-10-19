/**
 * @file packs/sound_def.hpp
 * @brief One named sound of a pack, and the parser that turns a pack INI into sounds.
 *
 * A pack INI has one section per sound:
 * @code
 * [mymod_bell]
 * Files = bell_1.ogg, bell_2.ogg   ; relative to this INI, one picked at random per play
 * Bus = world                      ; alias (see bus_path) or a raw FMOD path such as bus:/dieg/w_obj/obj
 * Space = 3d                       ; 3d (positioned) or 2d
 * Volume = 0.8
 * Replaces = event:/animals/dog/dog_bark   ; optional: game events this sound plays instead of
 * ReplacesFor = tvez_vorech                ; optional: only when the event starts at one of these entities
 * @endcode
 */
#ifndef SNDFW_PACKS_SOUND_DEF_HPP
#define SNDFW_PACKS_SOUND_DEF_HPP

#include "face/mood.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace SoundFramework::packs
{
    enum class Space : std::uint8_t
    {
        Positioned,
        Flat,
    };

    struct SoundDef
    {
        std::string name;
        /// Absolute paths of the variants; each play picks one, never the same one twice in a row.
        std::vector<std::filesystem::path> files;
        /// FMOD bus path whose channel group the sound plays into.
        std::string bus;
        Space space = Space::Positioned;
        float volume = 1.0f;
        /// Each play scales the volume by a random factor in [1 - v, 1].
        float volume_variation = 0.0f;
        float pitch = 1.0f;
        /// Each play scales the pitch by a random factor in [1 - v, 1 + v].
        float pitch_variation = 0.0f;
        /// Full volume up to here (metres).
        float min_distance = 2.0f;
        /// Silent from here on (metres).
        float max_distance = 40.0f;
        bool loop = false;
        /// Decode while playing instead of loading the whole file first (long music or ambience).
        bool stream = false;
        /// Instances of this sound that may play at once; a new play stops the oldest one beyond this.
        int max_instances = 8;
        /// Plays requested sooner than this after the last one are dropped (seconds).
        float cooldown = 0.0f;
        /**
         * @brief Game FMOD event paths (lower case) this sound replaces: each start of one is muted and this sound
         *        plays at its position instead, following it while it lives.
         */
        std::vector<std::string> replaces;
        /// Entity names (lower case): when set, an event is replaced only when it starts at one of these entities.
        std::vector<std::string> replaces_for;
        /// Where the sound was defined, for log lines: "<ini path>:<line>".
        std::string origin;
    };

    /** @brief Where a voice line's text shows while it plays. */
    enum class Display : std::uint8_t
    {
        None,
        /// The bottom subtitle (dialogue style), headed by the speaker name.
        Subtitle,
        /// The floating chat bubble above the speaker's head.
        Bubble,
        Both,
    };

    /**
     * @brief A voice line: one or more takes spoken through the game's speech system, with optional localized text.
     *
     * @code
     * [mymod_guard_hello]
     * Voice = Localization/dialog/mymod/mymd_hello_01, Localization/dialog/mymod/mymd_hello_02
     * TextKey = hello_01, hello_02     ; one key for all takes, or one per take
     * Speaker = soul_ui_name_guard     ; bottom-subtitle name key
     * Display = bubble                 ; subtitle | bubble | both | none
     * Cooldown = 30
     * Mood = angry                     ; a mood defined in the defaults file or any pack, held while the line plays
     * MoodStrength = 0.8               ; 0..2: below 1 tones the mood down, above 1 exaggerates it
     * MoodPoses = L_browsLowerer:0.6   ; extra or overriding pose weights, or a mood of its own
     * @endcode
     */
    struct LineDef
    {
        std::string name;
        /// Localization paths of the takes, without extension; each say picks one, never the same twice in a row.
        std::vector<std::string> voices;
        /// Text keys: empty, one shared by every take, or one per take.
        std::vector<std::string> text_keys;
        /// Speaker name key for the bottom subtitle.
        std::string speaker;
        Display display = Display::None;
        /// Says requested sooner than this after the last one are dropped (seconds).
        float cooldown = 0.0f;
        /// The mood's pose weights (the named mood merged with MoodPoses), filled when the catalog resolves moods.
        std::vector<face::PoseWeight> mood;
        /// Scales every mood weight.
        float mood_strength = 1.0f;
        /// The Mood value as written; empty for none.
        std::string mood_name;
        /// MoodPoses: weights added to, or overriding, the named mood.
        std::vector<face::PoseWeight> mood_poses;
        /// ReplacesVoice: the game's line text keys (lower case; `*` matches any run) this line plays instead of.
        std::vector<std::string> replaces_voice;
        std::string origin;
    };

    /**
     * @brief A mood: a named set of face pose weights that voice lines hold while they play.
     *
     * @code
     * [crying]
     * Poses = L_browsUp_in:0.9, R_browsUp_in:0.9, L_cornerDepressor:0.8, R_cornerDepressor:0.8
     * MaxStrength = 1                  ; 0..2, the most any line scales this mood
     * @endcode
     */
    struct MoodDef
    {
        std::string name;
        std::vector<face::PoseWeight> poses;
        /// MaxStrength: a line's MoodStrength (or sfw_say's strength) above it stops here.
        float max_strength = face::MAX_MOOD_STRENGTH;
        std::string origin;
    };

    struct PackParse
    {
        std::vector<SoundDef> sounds;
        std::vector<LineDef> lines;
        std::vector<MoodDef> moods;
        /// Problems worth telling the pack author about, one per line, each naming the INI line.
        std::vector<std::string> problems;
    };

    /** @brief Default bus of a sound whose section names none: the world-objects bus. */
    inline constexpr std::string_view DEFAULT_BUS = "bus:/dieg/w_obj/obj";

    /**
     * @brief The FMOD bus path for a Bus value: an alias, or a raw path starting with "bus:/".
     * @return The path, or nothing for an unknown alias.
     */
    [[nodiscard]] std::optional<std::string> bus_path(std::string_view value);

    /** @brief @p path as UTF-8 text for log lines (path::string() throws on characters outside the ANSI code page). */
    [[nodiscard]] std::string path_text(const std::filesystem::path &path);

    /** @brief True for names made of letters, digits, '_', '.' and '-', which keeps console arguments unambiguous. */
    [[nodiscard]] bool valid_sound_name(std::string_view name) noexcept;

    /** @brief True when @p text matches @p pattern, where `*` matches any run of characters (case-sensitive). */
    [[nodiscard]] bool wildcard_match(std::string_view pattern, std::string_view text) noexcept;

    /**
     * @brief Parses the text of the pack INI at @p ini_path into sounds.
     * @note Pure: file paths are resolved against the INI's directory but not checked on disk. A section with an
     *       invalid name, no files, or a value that cannot be read is skipped and reported.
     */
    [[nodiscard]] PackParse parse_pack(std::string_view text, const std::filesystem::path &ini_path);
} // namespace SoundFramework::packs

#endif // SNDFW_PACKS_SOUND_DEF_HPP
