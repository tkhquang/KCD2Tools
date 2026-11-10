/**
 * @file core/game_paks.cpp
 * @brief Offline reads of the game's animation files (see game_paks.hpp).
 */

#include "core/game_paks.hpp"

#include "core/clip_header.hpp"
#include "core/text_util.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>

namespace AnimationFramework
{
    namespace
    {
        /**
         * @brief The name prefixes of the paks that hold what the tools read: Animations.pak (databases, clips, the
         *        clip index), the character paks (male.chr is in IPL_Characters-part3.pak) and Tables.pak. The others
         *        (voice, levels, textures) are left closed: some hold tens of thousands of entries.
         */
        constexpr std::array<std::string_view, 4> TOOL_PAKS{"animations", "characters", "ipl_characters", "tables"};
        constexpr std::string_view CLIP_INDEX = "animations/animations.img";
    } // namespace

    std::expected<GamePaks, std::string> GamePaks::open(const std::filesystem::path &game_root)
    {
        const std::filesystem::path data = game_root / "Data";
        std::error_code error;
        if (!std::filesystem::is_directory(data, error))
        {
            return std::unexpected{std::format("{} has no Data folder: is it the game's folder?", game_root.string())};
        }
        std::vector<std::filesystem::path> files;
        for (const auto &entry : std::filesystem::directory_iterator{data, error})
        {
            const std::string name = lowercase(entry.path().filename().string());
            if (lowercase(entry.path().extension().string()) == ".pak")
            {
                if (std::ranges::any_of(
                        TOOL_PAKS,
                        [&name](std::string_view prefix) { return name.starts_with(prefix); }
                    ))
                {
                    files.push_back(entry.path());
                }
            }
        }
        std::ranges::sort(files, {}, [](const std::filesystem::path &p) { return lowercase(p.filename().string()); });
        GamePaks paks;
        for (const std::filesystem::path &file : files)
        {
            std::optional<std::vector<ZipEntry>> index = read_zip_index(file);
            if (!index)
            {
                return std::unexpected{std::format("{} does not read as a pak", file.string())};
            }
            const std::size_t at = paks.m_paks.size();
            for (std::size_t i = 0; i < index->size(); ++i)
            {
                paks.m_files.insert_or_assign(
                    (*index)[i].name,
                    Location{
                        .pak = at,
                        .entry = i,
                    }
                );
            }
            paks.m_paks.push_back(
                Pak{
                    .path = file,
                    .index = std::move(*index),
                }
            );
        }
        const std::optional<std::string> index = paks.read(CLIP_INDEX);
        if (!index)
        {
            return std::unexpected{std::format("{} holds no Animations.pak with a clip index", data.string())};
        }
        paks.m_clips = read_clip_image(std::as_bytes(std::span{*index}));
        paks.m_clip_paths.reserve(paks.m_clips.size());
        for (const ClipHeader &header : paks.m_clips)
        {
            paks.m_clip_paths.push_back(clip_header_path(header));
        }
        return paks;
    }

    std::optional<std::string> GamePaks::read(std::string_view path) const
    {
        const auto it = m_files.find(std::string{path});
        if (it == m_files.end())
        {
            return std::nullopt;
        }
        const Pak &pak = m_paks[it->second.pak];
        return read_zip_file(pak.path, pak.index[it->second.entry]);
    }

    std::expected<FoundClip, std::string> GamePaks::find_clip(std::string_view name) const
    {
        std::string wanted = fold_path(name);
        const bool by_path = wanted.find('/') != std::string::npos;
        if (by_path && !wanted.ends_with(".caf"))
        {
            wanted += ".caf";
        }
        for (std::size_t i = 0; i < m_clips.size(); ++i)
        {
            const ClipHeader &header = m_clips[i];
            const std::string &path = m_clip_paths[i];
            if (by_path ? path != wanted : clip_stem(path) != wanted)
            {
                continue;
            }
            std::uint32_t pack = 0;
            std::memcpy(&pack, header.data() + CLIP_HEADER_DBA_CRC_OFFSET, sizeof(pack));
            if (pack == 0)
            {
                const std::optional<std::string> data = read(path);
                if (!data)
                {
                    return std::unexpected{std::format("{} is indexed but no pak holds it", path)};
                }
                auto clip = read_caf_file(std::as_bytes(std::span{*data}));
                if (!clip)
                {
                    return std::unexpected{std::format("{}: {}", path, clip.error())};
                }
                clip->name = path;
                return FoundClip{
                    .clip = std::move(*clip),
                    .path = path,
                    .source = "the game",
                };
            }
            for (const auto &[file, location] : m_files)
            {
                if (!file.ends_with(".dba") ||
                    zip_crc32({reinterpret_cast<const std::uint8_t *>(file.data()), file.size()}) != pack)
                {
                    continue;
                }
                const std::optional<std::string> data = read(file);
                const std::vector<std::string> stems{clip_stem(path)};
                auto clips =
                    data ? read_dba_clips(std::as_bytes(std::span{*data}), stems)
                         : std::expected<std::vector<CafClip>, std::string>{std::unexpected{std::string{"unreadable"}}};
                if (!clips)
                {
                    return std::unexpected{std::format("{}: {}", file, clips.error())};
                }
                const auto it =
                    std::ranges::find_if(*clips, [&path](const CafClip &c) { return path_equals(c.name, path); });
                if (it == clips->end())
                {
                    return std::unexpected{std::format("{} does not hold {}", file, path)};
                }
                return FoundClip{
                    .clip = std::move(*it),
                    .path = path,
                    .source = file,
                };
            }
            return std::unexpected{std::format("{} is packed in a .dba no pak holds", path)};
        }
        return std::unexpected{std::format("{}{}", NO_CLIP_ERROR_PREFIX, name)};
    }

    std::expected<std::vector<SkeletonBone>, std::string> GamePaks::skeleton_at(std::string_view path) const
    {
        const std::optional<std::string> data = read(path);
        if (!data)
        {
            return std::unexpected{std::format("no pak holds {}", path)};
        }
        return read_skeleton_file(std::as_bytes(std::span{*data}));
    }
} // namespace AnimationFramework
