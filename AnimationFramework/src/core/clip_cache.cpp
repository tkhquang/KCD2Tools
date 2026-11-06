/**
 * @file core/clip_cache.cpp
 * @brief Persistent mod clip summaries (see clip_cache.hpp).
 */

#include "core/clip_cache.hpp"

#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace AnimationFramework
{
    namespace
    {
        constexpr std::string_view MAGIC = "KCD2_AnimationFramework clip cache 2\n";
        // Bounds that reject a damaged file before its sizes reach an allocation.
        constexpr std::size_t MAX_KEY = 1024;
        constexpr std::size_t MAX_MOTION = 4096;
        constexpr std::size_t MAX_BONES = 4096;

        /// Appends @p value to @p out little-endian.
        template <typename T> void put_le(std::string &out, T value)
        {
            for (std::size_t i = 0; i < sizeof(T); ++i)
            {
                out.push_back(static_cast<char>((static_cast<std::uint64_t>(value) >> (8 * i)) & 0xFF));
            }
        }

        /** @brief A bounds-checked little-endian reader over the cache file's bytes. */
        class Cursor
        {
        public:
            explicit Cursor(std::string_view data) noexcept : m_data(data) {}

            /// The next little-endian @p T, or nothing past the end.
            template <typename T> [[nodiscard]] std::optional<T> read() noexcept
            {
                if (m_data.size() - m_pos < sizeof(T))
                {
                    return std::nullopt;
                }
                std::uint64_t value = 0;
                for (std::size_t i = 0; i < sizeof(T); ++i)
                {
                    value |= static_cast<std::uint64_t>(static_cast<unsigned char>(m_data[m_pos + i])) << (8 * i);
                }
                m_pos += sizeof(T);
                return static_cast<T>(value);
            }

            /// The next @p count bytes, or nothing past the end.
            [[nodiscard]] std::optional<std::string_view> bytes(std::size_t count) noexcept
            {
                if (m_data.size() - m_pos < count)
                {
                    return std::nullopt;
                }
                const std::string_view out = m_data.substr(m_pos, count);
                m_pos += count;
                return out;
            }

            /// Whether every byte was read.
            [[nodiscard]] bool done() const noexcept { return m_pos == m_data.size(); }

        private:
            std::string_view m_data;
            std::size_t m_pos = 0;
        };
    } // namespace

    std::string ClipCache::key(
        std::string_view pak,
        std::string_view entry,
        std::uint32_t crc32,
        std::uint64_t compressed,
        std::uint64_t size
    )
    {
        std::string out;
        out.reserve(pak.size() + entry.size() + 40);
        out.append(pak).push_back('|');
        out.append(entry).push_back('|');
        out.append(std::to_string(crc32)).push_back('|');
        out.append(std::to_string(compressed)).push_back('|');
        out.append(std::to_string(size));
        return out;
    }

    bool ClipCache::load(const std::filesystem::path &file)
    {
        m_entries.clear();
        m_dirty = false;
        std::ifstream in{file, std::ios::binary};
        if (!in)
        {
            return false;
        }
        const std::string data{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        if (!data.starts_with(MAGIC))
        {
            return false;
        }
        Cursor cursor{std::string_view{data}.substr(MAGIC.size())};
        std::unordered_map<std::string, CachedClip> entries;
        while (!cursor.done())
        {
            const auto key_size = cursor.read<std::uint16_t>();
            if (!key_size || *key_size > MAX_KEY)
            {
                return false;
            }
            const auto key_text = cursor.bytes(*key_size);
            const auto controllers = cursor.read<std::uint32_t>();
            const auto motion_size = cursor.read<std::uint16_t>();
            if (!key_text || !controllers || !motion_size || *motion_size > MAX_MOTION)
            {
                return false;
            }
            const auto motion = cursor.bytes(*motion_size);
            const auto bone_count = cursor.read<std::uint16_t>();
            if (!motion || !bone_count || *bone_count > MAX_BONES)
            {
                return false;
            }
            CachedClip clip;
            clip.controllers = *controllers;
            clip.motion.resize(motion->size());
            std::memcpy(clip.motion.data(), motion->data(), motion->size());
            clip.bones.reserve(*bone_count);
            for (std::uint16_t i = 0; i < *bone_count; ++i)
            {
                const auto bone = cursor.read<std::uint32_t>();
                if (!bone)
                {
                    return false;
                }
                clip.bones.push_back(*bone);
            }
            entries.insert_or_assign(std::string{*key_text}, std::move(clip));
        }
        m_entries = std::move(entries);
        return true;
    }

    bool ClipCache::save(const std::filesystem::path &file, const std::vector<std::string> &keep)
    {
        std::string out{MAGIC};
        const std::unordered_set<std::string> kept{keep.begin(), keep.end()};
        for (const auto &[key_text, clip] : m_entries)
        {
            if ((!kept.empty() && !kept.contains(key_text)) || key_text.size() > MAX_KEY ||
                clip.motion.size() > MAX_MOTION || clip.bones.size() > MAX_BONES)
            {
                continue;
            }
            put_le(out, static_cast<std::uint16_t>(key_text.size()));
            out.append(key_text);
            put_le(out, clip.controllers);
            put_le(out, static_cast<std::uint16_t>(clip.motion.size()));
            out.append(reinterpret_cast<const char *>(clip.motion.data()), clip.motion.size());
            put_le(out, static_cast<std::uint16_t>(clip.bones.size()));
            for (const std::uint32_t bone : clip.bones)
            {
                put_le(out, bone);
            }
        }
        std::filesystem::path temp = file;
        temp += ".tmp";
        std::error_code error;
        {
            std::ofstream stream{temp, std::ios::binary | std::ios::trunc};
            stream.write(out.data(), static_cast<std::streamsize>(out.size()));
            // A write error can surface only when the buffer flushes, so the close is checked too: a short file must
            // never replace the cache.
            stream.close();
            if (!stream)
            {
                std::filesystem::remove(temp, error);
                return false;
            }
        }
        std::filesystem::rename(temp, file, error);
        if (error)
        {
            std::filesystem::remove(temp, error);
            return false;
        }
        m_dirty = false;
        return true;
    }

    const CachedClip *ClipCache::find(const std::string &key_text) const noexcept
    {
        const auto it = m_entries.find(key_text);
        return it != m_entries.end() ? &it->second : nullptr;
    }

    void ClipCache::put(std::string key_text, CachedClip clip)
    {
        m_entries.insert_or_assign(std::move(key_text), std::move(clip));
        m_dirty = true;
    }
} // namespace AnimationFramework
