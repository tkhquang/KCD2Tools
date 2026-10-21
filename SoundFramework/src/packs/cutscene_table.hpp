/**
 * @file packs/cutscene_table.hpp
 * @brief The cutscene speaker table: which actor entities move their faces for each cutscene voice line.
 *
 * Cutscenes play their voice lines from shared audio entities, so the game itself never links a line to the actor who
 * says it. The table holds that link, taken from the game's cutscene sequences and dialogue data. Each row is
 * `line id<TAB>mode<TAB>actor,actor,...`, and `#` starts a comment line.
 */
#ifndef SNDFW_PACKS_CUTSCENE_TABLE_HPP
#define SNDFW_PACKS_CUTSCENE_TABLE_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace SoundFramework::packs
{
    /** @brief How a cutscene line moves the faces of its row's actors. */
    enum class CutsceneMode : std::uint8_t
    {
        /// The actors say the line: its voice drives their mouths.
        Speaker,
        /// Henry says the line as the player character, who has no cutscene actor.
        Player,
        /// No known speaker: each actor's mouth follows the voice as much as its own animation moves it.
        Faces,
    };

    /** @brief One row of the table: a cutscene that plays the line, and the actors it moves. */
    struct CutsceneEntry
    {
        CutsceneMode mode = CutsceneMode::Speaker;
        std::vector<std::string> actors;
    };

    /// Line id (lower case) to its rows, one per cutscene that plays the line.
    using CutsceneTable = std::unordered_map<std::string, std::vector<CutsceneEntry>>;

    /**
     * @brief Parses the table text @p text.
     * @param rejected Receives the number of malformed rows (unknown mode, missing fields, no actors where the mode
     *        needs them), which the table leaves out.
     */
    [[nodiscard]] CutsceneTable parse_cutscene_table(std::string_view text, std::size_t &rejected);

    /** @brief The table key of a voice file or localization path: its file name without extension, in lower case. */
    [[nodiscard]] std::string cutscene_line_id(std::string_view voice_path);
} // namespace SoundFramework::packs

#endif // SNDFW_PACKS_CUTSCENE_TABLE_HPP
