/**
 * @file packs/catalog.hpp
 * @brief Finds the sound packs of the installed mods and merges them into one catalog of named sounds.
 *
 * A mod ships a pack as `.ini` files in `Mods/<mod id>/SoundFramework/` plus the audio files they name. Mods load in
 * the order of `Mods/mod_order.txt`, then the unlisted ones alphabetically; a later mod's sound replaces an earlier
 * sound of the same name, which lets a mod (or a user) override another pack.
 */
#ifndef SNDFW_PACKS_CATALOG_HPP
#define SNDFW_PACKS_CATALOG_HPP

#include "packs/sound_def.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace SoundFramework::packs
{
    /// The folder inside a mod that holds its pack.
    inline constexpr std::string_view PACK_FOLDER = "SoundFramework";

    struct PackFile
    {
        std::string mod_id;
        std::filesystem::path ini;
        std::filesystem::file_time_type write_time{};
    };

    struct Catalog
    {
        /// Sounds by lower-case name.
        std::unordered_map<std::string, SoundDef> sounds;
        /// Voice lines by lower-case name (a separate namespace from sounds).
        std::unordered_map<std::string, LineDef> lines;
        /// Moods by lower-case name: the defaults file's, then the packs' (a later definition wins).
        std::unordered_map<std::string, MoodDef> moods;
        /// Every pack INI that was read, in load order, with the write time it had.
        std::vector<PackFile> files;
    };

    struct CatalogLoad
    {
        Catalog catalog;
        /// Mistakes in the packs (bad values, missing files).
        std::vector<std::string> problems;
        /// Normal events worth a log line (overrides).
        std::vector<std::string> notes;
    };

    /** @brief @p name in lower case, the catalog key. */
    [[nodiscard]] std::string sound_key(std::string_view name);

    /** @brief The mod ids listed in `mod_order.txt` inside @p mods_dir, in file order; empty when there is none. */
    [[nodiscard]] std::vector<std::string> read_mod_order(const std::filesystem::path &mods_dir);

    /** @brief Every pack INI under @p mods_dir in load order. */
    [[nodiscard]] std::vector<PackFile> discover_packs(const std::filesystem::path &mods_dir);

    /**
     * @brief Reads @p defaults_file (when it exists), then every pack under @p mods_dir, and merges them; then resolves
     *        every voice line's mood. Never throws; I/O failures become problems.
     */
    [[nodiscard]] CatalogLoad
    load_catalog(const std::filesystem::path &mods_dir, const std::filesystem::path &defaults_file = {});

    /** @brief True when a pack INI of @p catalog changed on disk, vanished or cannot be read. */
    [[nodiscard]] bool catalog_changed(const Catalog &catalog) noexcept;
} // namespace SoundFramework::packs

#endif // SNDFW_PACKS_CATALOG_HPP
