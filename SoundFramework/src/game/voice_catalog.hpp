/**
 * @file game/voice_catalog.hpp
 * @brief The voice line catalog (packs/voice_lines.hpp) for the console commands: built on a worker the first time a
 *        mod asks, then read from a cache file until the game's paks or the language change.
 *
 * Building reads the game's dialogue files, which takes seconds, so nobody pays for it until a mod asks, and the asking
 * script keeps running: an answer is the word `building` until the catalog is ready.
 */
#ifndef SNDFW_GAME_VOICE_CATALOG_HPP
#define SNDFW_GAME_VOICE_CATALOG_HPP

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace SoundFramework::game::voice_catalog
{
    /**
     * @brief Sets where the game's paks and the cache file are.
     * @note Setup only, before the commands can run.
     */
    void configure(std::filesystem::path game_root, std::filesystem::path cache_file);

    /** @brief The languages the catalog follows, lower case, from the game's console variables. */
    struct Languages
    {
        std::string text;
        std::string voice;
    };

    /**
     * @brief One page of the lines whose speaker or text holds @p wanted: their total on the first line, then the
     *        lines. While the catalog is not ready the first line is `building`, or `failed`, and nothing follows.
     * @param languages Used by the first request of a session, which starts the build.
     * @note Main thread only.
     */
    [[nodiscard]] std::string
    find(std::string_view wanted, std::size_t first, std::size_t count, const Languages &languages);

    /**
     * @brief The line of the voice file at @p path, without its newline. Empty when it has none or the catalog is not
     *        ready.
     * @note Main thread only.
     */
    [[nodiscard]] std::string line(std::string_view path, const Languages &languages);

    /** @brief Stops a build and joins its worker. Teardown only, off the loader lock. */
    void shutdown() noexcept;
} // namespace SoundFramework::game::voice_catalog

#endif // SNDFW_GAME_VOICE_CATALOG_HPP
