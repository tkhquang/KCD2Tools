/**
 * @file core/clip_cache.hpp
 * @brief A persistent cache of what the framework reads out of mod clips (.caf): the motion parameters chunk and the
 *        controller chunk count each clip's index entry is built from, and the bones it keys, which the load-time
 *        clip check reads.
 *
 * A clip read loads the whole .caf through the engine, and a mod can ship a thousand of them (Swords on the Back's
 * cutscene fix: 1,068 clips, 1.2 GB unpacked), so an uncached scan at startup and on every DEV hot reload costs seconds
 * on the game thread. An entry is keyed by the pak, the entry name, and the zip entry's CRC-32 and sizes, so any change
 * to the clip or its pak's copy of it misses and is read again. The file is a small binary log: a version line, then
 * one record per clip. An unreadable or foreign file is ignored and rewritten on the next save.
 */
#ifndef ANIMFW_CORE_CLIP_CACHE_HPP
#define ANIMFW_CORE_CLIP_CACHE_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace AnimationFramework
{
    /** @brief What the framework keeps of one clip. */
    struct CachedClip
    {
        /// The motion parameters chunk.
        std::vector<std::byte> motion;
        /// The controller chunk count.
        std::uint32_t controllers = 0;
        /// The bone controller ids the clip keys.
        std::vector<std::uint32_t> bones;
    };

    /** @brief The cached clips by key, loaded from and saved to one file. */
    class ClipCache
    {
    public:
        /** @brief The key of a clip: @p pak (as mounted), @p entry (zip name), the zip entry's checksum and sizes. */
        [[nodiscard]] static std::string
        key(std::string_view pak,
            std::string_view entry,
            std::uint32_t crc32,
            std::uint64_t compressed,
            std::uint64_t size);

        /** @brief Replaces the contents with @p file's. False (and empty) when it is missing or not a cache. */
        bool load(const std::filesystem::path &file);

        /**
         * @brief Writes the contents to @p file (through a temporary file renamed over it) and clears the dirty mark.
         * @param keep Only the keys in it are written, so clips no mod ships any more drop out. Empty keeps all.
         * @return False on failure, which leaves the cache dirty.
         */
        bool save(const std::filesystem::path &file, const std::vector<std::string> &keep = {});

        /// The clip cached under @p key, or null. The pointer is valid until the next load or put.
        [[nodiscard]] const CachedClip *find(const std::string &key) const noexcept;
        /// Caches @p clip under @p key, replacing any clip there, and marks the cache dirty.
        void put(std::string key, CachedClip clip);

        /// The number of cached clips.
        [[nodiscard]] std::size_t size() const noexcept { return m_entries.size(); }
        /// Whether a put happened since the last load or successful save.
        [[nodiscard]] bool dirty() const noexcept { return m_dirty; }

    private:
        std::unordered_map<std::string, CachedClip> m_entries;
        bool m_dirty = false;
    };
} // namespace AnimationFramework

#endif // ANIMFW_CORE_CLIP_CACHE_HPP
