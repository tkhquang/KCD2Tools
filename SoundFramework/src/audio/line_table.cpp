/**
 * @file audio/line_table.cpp
 * @brief The published voice-line table.
 */

#include "audio/line_table.hpp"

#include <DetourModKit.hpp>

#include <algorithm>
#include <atomic>
#include <utility>
#include <vector>

namespace SoundFramework::audio::line_table
{
    namespace
    {
        std::atomic<std::shared_ptr<const Lines>> s_lines;
        std::atomic<std::shared_ptr<const Moods>> s_moods;

        /// What the readers get before the first publish and after clear(); built at load, so a read never allocates.
        const std::shared_ptr<const Lines> s_no_lines = std::make_shared<const Lines>();
        const std::shared_ptr<const Moods> s_no_moods = std::make_shared<const Moods>();

        struct VoiceIndex
        {
            std::unordered_map<std::string, std::string> exact;
            /// Patterns holding '*', longest first (the most specific wins), then by line name.
            std::vector<std::pair<std::string, std::string>> patterns;
        };
        std::atomic<std::shared_ptr<const VoiceIndex>> s_voices;

        [[nodiscard]] std::shared_ptr<const VoiceIndex> build_voice_index(const packs::Catalog &catalog)
        {
            auto index = std::make_shared<VoiceIndex>();
            for (const auto &[name, line] : catalog.lines)
            {
                for (const std::string &pattern : line.replaces_voice)
                {
                    if (pattern.find('*') != std::string::npos)
                    {
                        index->patterns.emplace_back(pattern, name);
                        continue;
                    }
                    const auto [found, added] = index->exact.try_emplace(pattern, name);
                    if (!added && found->second != name)
                    {
                        // The map's order is arbitrary, so the smaller name wins to keep the pick stable.
                        DMK::log().warning(
                            "Catalog: game line '{}' is replaced by both '{}' and '{}'; using '{}'",
                            pattern,
                            found->second,
                            name,
                            std::min(found->second, name)
                        );
                        found->second = std::min(found->second, name);
                    }
                }
            }
            std::ranges::sort(
                index->patterns,
                [](const auto &a, const auto &b)
                { return a.first.size() != b.first.size() ? a.first.size() > b.first.size() : a.second < b.second; }
            );
            if (!index->exact.empty() || !index->patterns.empty())
            {
                DMK::log().info(
                    "Catalog: {} game voice line key(s) and {} pattern(s) replaced",
                    index->exact.size(),
                    index->patterns.size()
                );
            }
            return index;
        }
    } // namespace

    void publish(const packs::Catalog &catalog)
    {
        s_lines.store(std::make_shared<const Lines>(catalog.lines));
        s_moods.store(std::make_shared<const Moods>(catalog.moods));
        s_voices.store(build_voice_index(catalog));
    }

    std::optional<std::string> voice_replacement(std::string_view text_key)
    {
        const std::shared_ptr<const VoiceIndex> index = s_voices.load();
        if (index == nullptr || (index->exact.empty() && index->patterns.empty()))
        {
            return std::nullopt;
        }
        const auto exact = index->exact.find(std::string(text_key));
        if (exact != index->exact.end())
        {
            return exact->second;
        }
        for (const auto &[pattern, name] : index->patterns)
        {
            if (packs::wildcard_match(pattern, text_key))
            {
                return name;
            }
        }
        return std::nullopt;
    }

    std::shared_ptr<const Lines> current() noexcept
    {
        std::shared_ptr<const Lines> lines = s_lines.load();
        return lines != nullptr ? lines : s_no_lines;
    }

    std::shared_ptr<const Moods> moods() noexcept
    {
        std::shared_ptr<const Moods> table = s_moods.load();
        return table != nullptr ? table : s_no_moods;
    }

    void clear() noexcept
    {
        s_voices.store(nullptr);
        s_moods.store(nullptr);
        s_lines.store(nullptr);
    }
} // namespace SoundFramework::audio::line_table
