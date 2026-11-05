/**
 * @file packs/voice_lines.hpp
 * @brief Every voice file of the game with its text and who says it, read from the game's paks, so a pick list needs no
 *        copy of its own and follows game updates, DLC and the player's language.
 *
 * Inputs, none of them game memory:
 *   - The voice paks' file names: `dialog/<folders>/<stem>.ogg`, where the stem is `<actor code>_<text key>` or the
 *     key. A file outside `dialog/` is left out.
 *   - `text_ui_dialog.xml` of the language's `<language>_xml.pak`: a row per text key. A file with no text is left out.
 *   - The dialogue files under `Quests/` in Scripts.pak: a response names its role and text key, and the dialogue's
 *     cast gives the role a voice actor.
 *
 * A line is `<voice path>\t<speaker>\t<text>`. The path is relative to `Localization/dialog/` and has no extension; the
 * speaker is "Role (Actor)" as far as it is known, maybe empty. An actor code means the actor the dialogues cast most
 * often for the files that start with it: it names who recorded the file, which the cast does not for a line several
 * actors recorded (a bark), so it comes first. With neither a code nor a cast, a role's actor is the one cast most
 * often in it. Henry's lines come first, shortest first, so an empty search shows what a gesture is most likely to
 * want, with the lines that are only punctuation or a stutter at the end of their group.
 */
#ifndef SNDFW_PACKS_VOICE_LINES_HPP
#define SNDFW_PACKS_VOICE_LINES_HPP

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace SoundFramework::packs
{
    /** @brief Collects the catalog's inputs, then writes its lines. */
    class VoiceLineBuilder
    {
    public:
        /** @brief Takes the rows of a `text_ui_dialog.xml`. */
        void add_texts(std::string_view xml);

        /** @brief Takes the responses of a dialogue file. The first response of a text key is the one kept. */
        void add_dialogue(std::string_view xml);

        /** @brief Takes one entry of a voice pak. Call after add_texts: a file with no text is dropped here. */
        void add_voice_file(std::string_view name);

        /** @brief The lines, sorted, each ending in a newline. */
        [[nodiscard]] std::string build() const;

    private:
        struct Spoken
        {
            std::string role;
            std::string actor;
        };

        struct File
        {
            bool henry = false;
            std::string path;
            std::string key;
            std::string code;
            std::string_view text;
        };

        std::unordered_map<std::string, std::string> m_texts;
        std::unordered_map<std::string, Spoken> m_spoken;
        std::vector<File> m_files;
    };

    /** @brief Where the catalog's inputs are. */
    struct VoiceLineSources
    {
        /// The language's `<language>_xml.pak`.
        std::filesystem::path text_pak;
        /// Scripts.pak.
        std::filesystem::path dialogue_pak;
        /// The voice paks, in the order their files are listed before the sort.
        std::vector<std::filesystem::path> voice_paks;
    };

    /**
     * @brief Reads the paks and returns the lines. Inflating every dialogue takes seconds, so this runs on a worker.
     * @param stopped Asked between files: true gives up, returning nothing.
     * @return The lines, or nothing when stopped or when a pak could not be read.
     */
    [[nodiscard]] std::optional<std::string>
    build_voice_lines(const VoiceLineSources &sources, const std::function<bool()> &stopped);

    /** @brief One page of a search. */
    struct VoiceLinePage
    {
        /// Every match of the search, not only this page's.
        std::size_t total = 0;
        /// The page's lines, each ending in a newline.
        std::string lines;
    };

    /** @brief The lines, ready to search. They point into its own text, so it neither copies nor moves. */
    class VoiceLines
    {
    public:
        explicit VoiceLines(std::string text);
        VoiceLines(const VoiceLines &) = delete;
        VoiceLines &operator=(const VoiceLines &) = delete;
        VoiceLines(VoiceLines &&) = delete;
        VoiceLines &operator=(VoiceLines &&) = delete;
        ~VoiceLines() = default;

        [[nodiscard]] std::size_t size() const noexcept { return m_lines.size(); }

        /**
         * @brief One page of the lines whose speaker or text holds @p wanted, ignoring ASCII case. Empty matches all.
         * @details Runs as the player types, so it scans the whole searchable text at once rather than line by line.
         */
        [[nodiscard]] VoiceLinePage find(std::string_view wanted, std::size_t first, std::size_t count) const;

        /** @brief The line of the voice file at @p path, ignoring ASCII case, without its newline, or nothing. */
        [[nodiscard]] std::optional<std::string_view> line(std::string_view path) const;

    private:
        struct Line
        {
            std::string_view text;
            std::string_view path;
        };

        std::string m_text;
        /// The speakers and texts, lowered, in line order.
        std::string m_search;
        /// Where each line starts in m_search.
        std::vector<std::size_t> m_starts;
        std::vector<Line> m_lines;
    };
} // namespace SoundFramework::packs

#endif // SNDFW_PACKS_VOICE_LINES_HPP
