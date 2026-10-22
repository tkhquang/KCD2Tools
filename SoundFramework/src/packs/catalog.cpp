/**
 * @file packs/catalog.cpp
 * @brief Pack discovery and merging.
 */

#include "packs/catalog.hpp"
#include "packs/ini_document.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <system_error>

namespace SoundFramework::packs
{
    namespace
    {
        namespace fs = std::filesystem;

        [[nodiscard]] std::optional<std::string> read_text(const fs::path &path)
        {
            std::ifstream stream(path, std::ios::binary);
            if (!stream)
            {
                return std::nullopt;
            }
            std::ostringstream text;
            text << stream.rdbuf();
            return std::move(text).str();
        }

        [[nodiscard]] std::string file_name_text(const fs::path &path)
        {
            return path_text(path.filename());
        }

        /** @brief The pack INIs inside one mod folder, by file name. */
        [[nodiscard]] std::vector<PackFile> pack_files_of(const fs::path &mod_dir, const std::string &mod_id)
        {
            std::vector<PackFile> files;
            std::error_code error;
            const fs::path pack_dir = mod_dir / fs::path(std::u8string(PACK_FOLDER.begin(), PACK_FOLDER.end()));
            if (!fs::is_directory(pack_dir, error))
            {
                return files;
            }
            for (fs::directory_iterator it(pack_dir, error), end; !error && it != end; it.increment(error))
            {
                const fs::path &path = it->path();
                if (it->is_regular_file(error) && iequals(path_text(path.extension()), ".ini"))
                {
                    files.push_back(
                        PackFile{
                            .mod_id = mod_id,
                            .ini = path,
                            .write_time = it->last_write_time(error),
                        }
                    );
                }
            }
            std::ranges::sort(
                files,
                [](const PackFile &a, const PackFile &b)
                { return sound_key(file_name_text(a.ini)) < sound_key(file_name_text(b.ini)); }
            );
            return files;
        }

        /** @brief Drops variants missing on disk; reports each one. */
        void keep_existing_files(SoundDef &sound, std::vector<std::string> &problems)
        {
            std::erase_if(
                sound.files,
                [&](const fs::path &file)
                {
                    std::error_code error;
                    if (fs::is_regular_file(file, error))
                    {
                        return false;
                    }
                    problems.push_back(
                        std::format("{}: [{}] file not found: {}", sound.origin, sound.name, path_text(file))
                    );
                    return true;
                }
            );
        }
    } // namespace

    std::string sound_key(std::string_view name)
    {
        std::string key(name);
        std::ranges::transform(
            key,
            key.begin(),
            [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
        );
        return key;
    }

    std::vector<std::string> read_mod_order(const fs::path &mods_dir)
    {
        std::vector<std::string> order;
        const std::optional<std::string> text = read_text(mods_dir / "mod_order.txt");
        if (!text.has_value())
        {
            return order;
        }
        std::string_view rest = *text;
        while (!rest.empty())
        {
            const std::size_t end = rest.find('\n');
            const std::string_view line = trim(rest.substr(0, end));
            rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
            if (!line.empty() && !line.starts_with(';') && !line.starts_with('#'))
            {
                order.emplace_back(line);
            }
        }
        return order;
    }

    std::vector<PackFile> discover_packs(const fs::path &mods_dir)
    {
        std::vector<std::string> mod_ids;
        std::error_code error;
        for (fs::directory_iterator it(mods_dir, error), end; !error && it != end; it.increment(error))
        {
            if (it->is_directory(error))
            {
                mod_ids.push_back(file_name_text(it->path()));
            }
        }
        // Listed mods first in their listed order, then the rest alphabetically.
        const std::vector<std::string> order = read_mod_order(mods_dir);
        const auto rank = [&](const std::string &id)
        {
            const auto listed = std::ranges::find_if(order, [&](const std::string &o) { return iequals(o, id); });
            return listed == order.end() ? order.size() : static_cast<std::size_t>(listed - order.begin());
        };
        std::ranges::sort(
            mod_ids,
            [&](const std::string &a, const std::string &b)
            {
                const std::size_t ra = rank(a);
                const std::size_t rb = rank(b);
                return ra != rb ? ra < rb : sound_key(a) < sound_key(b);
            }
        );

        std::vector<PackFile> files;
        for (const std::string &id : mod_ids)
        {
            std::vector<PackFile> of_mod = pack_files_of(mods_dir / fs::path(std::u8string(id.begin(), id.end())), id);
            std::ranges::move(of_mod, std::back_inserter(files));
        }
        return files;
    }

    CatalogLoad load_catalog(const fs::path &mods_dir, const fs::path &defaults_file)
    {
        CatalogLoad load;
        try
        {
            std::vector<PackFile> files;
            std::error_code error;
            if (!defaults_file.empty() && fs::is_regular_file(defaults_file, error))
            {
                files.push_back(
                    PackFile{
                        .mod_id = "(defaults)",
                        .ini = defaults_file,
                        .write_time = fs::last_write_time(defaults_file, error),
                    }
                );
            }
            std::ranges::move(discover_packs(mods_dir), std::back_inserter(files));
            for (PackFile &file : files)
            {
                const std::optional<std::string> text = read_text(file.ini);
                if (!text.has_value())
                {
                    load.problems.push_back(std::format("{}: cannot be read", path_text(file.ini)));
                    continue;
                }
                PackParse parsed = parse_pack(*text, file.ini);
                std::ranges::move(parsed.problems, std::back_inserter(load.problems));
                for (SoundDef &sound : parsed.sounds)
                {
                    keep_existing_files(sound, load.problems);
                    if (sound.files.empty() && sound.events.empty())
                    {
                        load.problems.push_back(
                            std::format("{}: [{}] skipped: none of its files exist", sound.origin, sound.name)
                        );
                        continue;
                    }
                    std::string key = sound_key(sound.name);
                    const auto existing = load.catalog.sounds.find(key);
                    if (existing != load.catalog.sounds.end())
                    {
                        load.notes.push_back(
                            std::format(
                                "[{}] from {} overrides the one from {}",
                                sound.name,
                                sound.origin,
                                existing->second.origin
                            )
                        );
                        existing->second = std::move(sound);
                    }
                    else
                    {
                        load.catalog.sounds.emplace(std::move(key), std::move(sound));
                    }
                }
                for (MoodDef &mood : parsed.moods)
                {
                    std::string key = sound_key(mood.name);
                    const auto existing = load.catalog.moods.find(key);
                    if (existing != load.catalog.moods.end())
                    {
                        load.notes.push_back(
                            std::format(
                                "mood [{}] from {} overrides the one from {}",
                                mood.name,
                                mood.origin,
                                existing->second.origin
                            )
                        );
                        existing->second = std::move(mood);
                    }
                    else
                    {
                        load.catalog.moods.emplace(std::move(key), std::move(mood));
                    }
                }
                for (LineDef &line : parsed.lines)
                {
                    std::string key = sound_key(line.name);
                    const auto existing = load.catalog.lines.find(key);
                    if (existing != load.catalog.lines.end())
                    {
                        load.notes.push_back(
                            std::format(
                                "[{}] from {} overrides the voice line from {}",
                                line.name,
                                line.origin,
                                existing->second.origin
                            )
                        );
                        existing->second = std::move(line);
                    }
                    else
                    {
                        load.catalog.lines.emplace(std::move(key), std::move(line));
                    }
                }
                load.catalog.files.push_back(std::move(file));
            }
        }
        catch (const std::exception &error)
        {
            load.problems.push_back(std::format("pack loading stopped: {}", error.what()));
        }
        // Every mood is known now: resolve the lines' moods.
        for (auto &[key, line] : load.catalog.lines)
        {
            std::vector<face::PoseWeight> base;
            if (!line.mood_name.empty() && !iequals(line.mood_name, face::NEUTRAL))
            {
                const auto mood = load.catalog.moods.find(sound_key(line.mood_name));
                if (mood == load.catalog.moods.end())
                {
                    load.problems.push_back(
                        std::format("{}: [{}] Mood '{}' is not defined", line.origin, line.name, line.mood_name)
                    );
                }
                else
                {
                    base = mood->second.poses;
                    line.mood_strength = std::min(line.mood_strength, mood->second.max_strength);
                }
            }
            line.mood = face::merge_poses(std::move(base), line.mood_poses);
        }
        return load;
    }

    bool catalog_changed(const Catalog &catalog) noexcept
    {
        for (const PackFile &file : catalog.files)
        {
            std::error_code error;
            const fs::file_time_type now = fs::last_write_time(file.ini, error);
            if (error || now != file.write_time)
            {
                return true;
            }
        }
        return false;
    }
} // namespace SoundFramework::packs
