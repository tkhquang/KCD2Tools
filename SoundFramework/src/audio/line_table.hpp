/**
 * @file audio/line_table.hpp
 * @brief The packs' voice lines, published by the engine whenever the packs load and read by the speech code on the
 * main
 *        thread.
 */
#ifndef SNDFW_AUDIO_LINE_TABLE_HPP
#define SNDFW_AUDIO_LINE_TABLE_HPP

#include "packs/catalog.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace SoundFramework::audio::line_table
{
    /// Voice lines by lower-case name.
    using Lines = std::unordered_map<std::string, packs::LineDef>;
    /// Moods by lower-case name.
    using Moods = std::unordered_map<std::string, packs::MoodDef>;

    /** @brief Replaces the tables with @p catalog's lines and moods. Thread safe. */
    void publish(const packs::Catalog &catalog);

    /** @brief The current lines; never null. Thread safe. */
    [[nodiscard]] std::shared_ptr<const Lines> current() noexcept;

    /** @brief The current moods; never null. Thread safe. */
    [[nodiscard]] std::shared_ptr<const Moods> moods() noexcept;

    /**
     * @brief The pack line (lower-case name) that replaces the game line with text key @p text_key (lower case): an
     *        exact ReplacesVoice key first, then the longest matching pattern. Thread safe.
     */
    [[nodiscard]] std::optional<std::string> voice_replacement(std::string_view text_key);

    /** @brief Drops the table. */
    void clear() noexcept;
} // namespace SoundFramework::audio::line_table

#endif // SNDFW_AUDIO_LINE_TABLE_HPP
