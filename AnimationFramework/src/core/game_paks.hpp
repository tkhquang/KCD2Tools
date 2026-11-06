/**
 * @file core/game_paks.hpp
 * @brief The stock game's animation files read straight from its paks, for the tools that run without the game: the
 *        paks of `<game>/Data` that hold animations, characters and tables, a later pak's copy of a file winning.
 */

#ifndef ANIMFW_CORE_GAME_PAKS_HPP
#define ANIMFW_CORE_GAME_PAKS_HPP

#include "core/caf_reader.hpp"
#include "core/clip_header.hpp"
#include "core/mod_tools.hpp"
#include "core/zip_reader.hpp"

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace AnimationFramework
{
    /** @brief The tool paks of one game folder, indexed once, and its clip index. */
    class GamePaks
    {
    public:
        /** @brief Indexes the animation, character and table paks of @p game_root/Data. */
        [[nodiscard]] static std::expected<GamePaks, std::string> open(const std::filesystem::path &game_root);

        /** @brief A file by its path (lowercase, forward slashes), or nothing. */
        [[nodiscard]] std::optional<std::string> read(std::string_view path) const;

        /** @brief A game clip by name or path: from the clip index, out of its `.dba` pack or loose. */
        [[nodiscard]] std::expected<FoundClip, std::string> find_clip(std::string_view name) const;

        /** @brief Henry's skeleton. */
        [[nodiscard]] std::expected<std::vector<SkeletonBone>, std::string> male_skeleton() const;

        /** @brief Every clip the game's index lists, by path (lowercase), in index order. */
        [[nodiscard]] const std::vector<std::string> &clip_paths() const noexcept { return m_clip_paths; }

    private:
        /// One indexed pak.
        struct Pak
        {
            std::filesystem::path path;
            std::vector<ZipEntry> index;
        };
        /// Where a file's winning copy is: its pak and its entry there.
        struct Location
        {
            std::size_t pak = 0;
            std::size_t entry = 0;
        };

        std::vector<Pak> m_paks;
        std::unordered_map<std::string, Location> m_files;
        /// The clip index, read once: each clip's header.
        std::vector<ClipHeader> m_clips;
        /// Each clip's path (lowercase), at its header's position in m_clips.
        std::vector<std::string> m_clip_paths;
    };
} // namespace AnimationFramework

#endif // ANIMFW_CORE_GAME_PAKS_HPP
