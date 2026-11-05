/**
 * @file game/voice_catalog.cpp
 * @brief Builds, caches and serves the voice line catalog (see voice_catalog.hpp).
 */

#include "game/voice_catalog.hpp"
#include "packs/sound_def.hpp"
#include "packs/voice_lines.hpp"

#include <DetourModKit.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace SoundFramework::game::voice_catalog
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr std::string_view FALLBACK_LANGUAGE = "english";
        constexpr std::string_view TEXT_PAK_SUFFIX = "_xml.pak";
        constexpr std::string_view PAK_EXTENSION = ".pak";
        constexpr std::string_view DLC_PAK_PREFIX = "ipl_";
        constexpr std::string_view DIALOGUE_PAK = "Scripts.pak";
        constexpr std::string_view BUILDING_ANSWER = "building\n";
        constexpr std::string_view FAILED_ANSWER = "failed\n";
        // Raised when a line's format changes, so an older cache file rebuilds.
        constexpr int CACHE_FORMAT = 1;

        enum class State
        {
            Idle,
            Building,
            Ready,
            Failed,
        };

        fs::path s_game_root;
        fs::path s_cache_file;
        std::atomic<State> s_state{State::Idle};
        // Published by the worker, read by the main thread.
        std::atomic<std::shared_ptr<const packs::VoiceLines>> s_lines;
        std::optional<DMK::StoppableWorker> s_worker;

        [[nodiscard]] std::string lowered_name(const fs::path &path)
        {
            std::string name = packs::path_text(path.filename());
            std::ranges::transform(
                name,
                name.begin(),
                [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
            );
            return name;
        }

        /** @brief True for one of a language's voice paks: `<language>.pak`, or split as `<language>-part<n>.pak`. */
        [[nodiscard]] bool is_voice_pak(std::string_view name, std::string_view language)
        {
            if (!name.ends_with(PAK_EXTENSION) || name.ends_with(TEXT_PAK_SUFFIX))
            {
                return false;
            }
            if (name.starts_with(DLC_PAK_PREFIX))
            {
                name.remove_prefix(DLC_PAK_PREFIX.size());
            }
            return name.size() > language.size() && name.starts_with(language) &&
                   (name[language.size()] == '.' || name[language.size()] == '-');
        }

        /** @brief The paks of the languages, falling back to English for one this install does not have. */
        [[nodiscard]] std::optional<packs::VoiceLineSources> find_sources(const Languages &languages)
        {
            const fs::path localization = s_game_root / L"Localization";
            std::error_code error;
            std::vector<fs::path> paks;
            for (fs::directory_iterator entry{localization, error}; !error && entry != fs::directory_iterator{};
                 entry.increment(error))
            {
                paks.push_back(entry->path());
            }
            std::ranges::sort(paks, {}, lowered_name);

            const auto text_pak = [&paks](std::string_view language) -> std::optional<fs::path>
            {
                const std::string wanted = std::string{language} + std::string{TEXT_PAK_SUFFIX};
                const auto it = std::ranges::find(paks, wanted, lowered_name);
                return it != paks.end() ? std::optional{*it} : std::nullopt;
            };
            const auto voice_paks = [&paks](std::string_view language)
            {
                std::vector<fs::path> found;
                std::ranges::copy_if(
                    paks,
                    std::back_inserter(found),
                    [language](const fs::path &pak) { return is_voice_pak(lowered_name(pak), language); }
                );
                return found;
            };

            std::optional<fs::path> text = languages.text.empty() ? std::nullopt : text_pak(languages.text);
            if (!text.has_value())
            {
                text = text_pak(FALLBACK_LANGUAGE);
            }
            std::vector<fs::path> voices =
                languages.voice.empty() ? std::vector<fs::path>{} : voice_paks(languages.voice);
            if (voices.empty())
            {
                voices = voice_paks(FALLBACK_LANGUAGE);
            }
            if (!text.has_value() || voices.empty())
            {
                return std::nullopt;
            }
            return packs::VoiceLineSources{
                .text_pak = std::move(*text),
                .dialogue_pak = s_game_root / L"Data" / DIALOGUE_PAK,
                .voice_paks = std::move(voices),
            };
        }

        /** @brief What the cache depends on: its format and every input's name, size and time. */
        [[nodiscard]] std::string fingerprint_of(const packs::VoiceLineSources &sources)
        {
            std::string fingerprint = std::format("format {}", CACHE_FORMAT);
            const auto add = [&fingerprint](const fs::path &pak)
            {
                std::error_code error;
                const std::uintmax_t size = fs::file_size(pak, error);
                const auto time = fs::last_write_time(pak, error).time_since_epoch().count();
                fingerprint += std::format("|{}:{}:{}", packs::path_text(pak.filename()), size, time);
            };
            add(sources.text_pak);
            add(sources.dialogue_pak);
            std::ranges::for_each(sources.voice_paks, add);
            return fingerprint;
        }

        /** @brief The cached lines, when the file was written for this @p fingerprint. */
        [[nodiscard]] std::optional<std::string> load_cache(const std::string &fingerprint)
        {
            std::ifstream file{s_cache_file, std::ios::binary};
            std::string first;
            if (!file || !std::getline(file, first) || first != fingerprint)
            {
                return std::nullopt;
            }
            return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
        }

        void save_cache(const std::string &fingerprint, const std::string &text)
        {
            std::ofstream file{s_cache_file, std::ios::binary | std::ios::trunc};
            file << fingerprint << '\n' << text;
            if (!file.good())
            {
                DMK::log().warning("VoiceCatalog: could not write {}", packs::path_text(s_cache_file));
            }
        }

        void build(const std::stop_token &stop, const Languages &languages)
        {
            const auto started = std::chrono::steady_clock::now();
            const std::optional<packs::VoiceLineSources> sources = find_sources(languages);
            if (!sources.has_value())
            {
                DMK::log().error("VoiceCatalog: no text or voice paks for '{}', '{}'", languages.text, languages.voice);
                s_state.store(State::Failed, std::memory_order_release);
                return;
            }
            const std::string fingerprint = fingerprint_of(*sources);
            std::optional<std::string> text = load_cache(fingerprint);
            const bool cached = text.has_value();
            if (!cached)
            {
                text = packs::build_voice_lines(*sources, [&stop] { return stop.stop_requested(); });
            }
            if (!text.has_value())
            {
                if (!stop.stop_requested())
                {
                    DMK::log().error("VoiceCatalog: a pak could not be read");
                }
                s_state.store(State::Failed, std::memory_order_release);
                return;
            }
            if (!cached)
            {
                save_cache(fingerprint, *text);
            }
            auto lines = std::make_shared<const packs::VoiceLines>(std::move(*text));
            const auto elapsed = std::chrono::steady_clock::now() - started;
            DMK::log().info(
                "VoiceCatalog: {} lines {} in {} ms",
                lines->size(),
                cached ? "from the cache" : "built",
                std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
            );
            s_lines.store(std::move(lines), std::memory_order_release);
            s_state.store(State::Ready, std::memory_order_release);
        }

        /** @brief The lines, starting the build on the first request of a session. Null until they are ready. */
        [[nodiscard]] std::shared_ptr<const packs::VoiceLines> request(const Languages &languages)
        {
            if (s_state.load(std::memory_order_acquire) == State::Idle)
            {
                // Set before the worker starts, which can finish and publish Ready before emplace returns.
                s_state.store(State::Building, std::memory_order_release);
                try
                {
                    s_worker.emplace(
                        "SoundFramework.voicecatalog",
                        [languages](std::stop_token stop)
                        {
                            try
                            {
                                build(stop, languages);
                            }
                            catch (...)
                            {
                                (void)DMK::log().log_noexcept(DMK::LogLevel::Error, "VoiceCatalog: the build failed");
                                s_state.store(State::Failed, std::memory_order_release);
                            }
                        }
                    );
                }
                catch (...)
                {
                    s_state.store(State::Failed, std::memory_order_release);
                    throw;
                }
            }
            return s_lines.load(std::memory_order_acquire);
        }
    } // namespace

    void configure(std::filesystem::path game_root, std::filesystem::path cache_file)
    {
        s_game_root = std::move(game_root);
        s_cache_file = std::move(cache_file);
    }

    std::string find(std::string_view wanted, std::size_t first, std::size_t count, const Languages &languages)
    {
        const std::shared_ptr<const packs::VoiceLines> lines = request(languages);
        if (lines == nullptr)
        {
            return std::string{
                s_state.load(std::memory_order_acquire) == State::Failed ? FAILED_ANSWER : BUILDING_ANSWER
            };
        }
        const packs::VoiceLinePage page = lines->find(wanted, first, count);
        return std::to_string(page.total) + "\n" + page.lines;
    }

    std::string line(std::string_view path, const Languages &languages)
    {
        const std::shared_ptr<const packs::VoiceLines> lines = request(languages);
        const std::optional<std::string_view> found = lines != nullptr ? lines->line(path) : std::nullopt;
        return found.has_value() ? std::string{*found} : std::string{};
    }

    void shutdown() noexcept
    {
        s_worker.reset();
        s_lines.store(nullptr);
        s_state.store(State::Idle);
    }
} // namespace SoundFramework::game::voice_catalog
