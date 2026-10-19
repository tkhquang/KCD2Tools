/**
 * @file audio/replacements.cpp
 * @brief The event-to-sound table and the event start handler.
 *
 * The table is immutable once published and swapped as a whole, so the audio thread reads it without a lock. Event
 * paths are cached per description handle, because asking FMOD for a path copies a string out of the strings bank.
 */

#include "audio/replacements.hpp"
#include "audio/engine.hpp"
#include "constants.hpp"
#include "fmod/fmod_api.hpp"
#include "game/entity_tracker.hpp"

#include <DetourModKit.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace SoundFramework::audio::replacements
{
    namespace
    {
        /** @brief One sound replacing one event, optionally only at named entities. */
        struct Rule
        {
            std::string key;
            /// Entity names (lower case); empty means every start of the event.
            std::vector<std::string> only_for;
        };

        struct Table
        {
            /// Lower-case event path to its rules: entity-filtered rules first, then at most one unfiltered rule.
            std::unordered_map<std::string, std::vector<Rule>> rules_of_event;
        };

        std::atomic<std::shared_ptr<const Table>> s_table;

        std::mutex s_path_mutex;
        /// Event path by description handle; descriptions stay valid while their bank is loaded.
        std::unordered_map<void *, std::string> s_paths;

        std::mutex s_filter_mutex;
        std::string s_filter;
        std::atomic<bool> s_logging{false};

        [[nodiscard]] char lower(char c) noexcept
        {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        }

        /** @brief The lower-case path of @p description, cached; empty when FMOD cannot name it. */
        [[nodiscard]] std::string path_of(void *description)
        {
            {
                const std::lock_guard lock(s_path_mutex);
                const auto found = s_paths.find(description);
                if (found != s_paths.end())
                {
                    return found->second;
                }
            }
            std::array<char, 512> buffer{};
            int retrieved = 0;
            std::string path;
            if (fmod::api()
                    .description_get_path(description, buffer.data(), static_cast<int>(buffer.size()), &retrieved) ==
                fmod::OK)
            {
                path.assign(buffer.data());
                std::ranges::transform(path, path.begin(), lower);
            }
            const std::lock_guard lock(s_path_mutex);
            s_paths.emplace(description, path);
            return path;
        }

        [[nodiscard]] bool log_matches(std::string_view path)
        {
            const std::lock_guard lock(s_filter_mutex);
            return s_filter == "*" || path.find(s_filter) != std::string_view::npos;
        }
    } // namespace

    void publish(const packs::Catalog &catalog)
    {
        auto table = std::make_shared<Table>();
        std::vector<std::string> watched;
        for (const auto &[key, sound] : catalog.sounds)
        {
            for (const std::string &event : sound.replaces)
            {
                std::vector<Rule> &rules = table->rules_of_event[event];
                if (sound.replaces_for.empty())
                {
                    // One unfiltered rule per event, kept last so filtered rules are tried first.
                    if (!rules.empty() && rules.back().only_for.empty())
                    {
                        DMK::log().warning(
                            "Replacements: {} is claimed by both [{}] and [{}]; [{}] wins",
                            event,
                            rules.back().key,
                            key,
                            key
                        );
                        rules.back().key = key;
                    }
                    else
                    {
                        rules.push_back(
                            Rule{
                                .key = key,
                            }
                        );
                    }
                    continue;
                }
                const auto unfiltered =
                    std::ranges::find_if(rules, [](const Rule &rule) { return rule.only_for.empty(); });
                rules.insert(
                    unfiltered,
                    Rule{
                        .key = key,
                        .only_for = sound.replaces_for,
                    }
                );
                for (const std::string &name : sound.replaces_for)
                {
                    if (std::ranges::find(watched, name) == watched.end())
                    {
                        watched.push_back(name);
                    }
                }
            }
        }
        DMK::log().info(
            "Replacements: {} game event(s) replaced by pack sounds; {} entity name(s) tracked",
            table->rules_of_event.size(),
            watched.size()
        );
        game::entities::set_watch_list(std::move(watched));
        s_table.store(std::move(table));
    }

    void clear() noexcept
    {
        s_table.store(nullptr);
        const std::lock_guard lock(s_path_mutex);
        s_paths.clear();
    }

    void set_event_log_filter(std::string_view filter)
    {
        std::string lowered(filter);
        std::ranges::transform(lowered, lowered.begin(), lower);
        const std::lock_guard lock(s_filter_mutex);
        s_filter = std::move(lowered);
        s_logging.store(!s_filter.empty(), std::memory_order_release);
    }

    void on_event_start(void *instance) noexcept
    {
        try
        {
            const std::shared_ptr<const Table> table = s_table.load();
            const bool logging = s_logging.load(std::memory_order_acquire);
            if (instance == nullptr || ((table == nullptr || table->rules_of_event.empty()) && !logging))
            {
                return;
            }
            const fmod::Api &api = fmod::api();
            void *description = nullptr;
            if (api.event_get_description(instance, &description) != fmod::OK || description == nullptr)
            {
                return;
            }
            const std::string path = path_of(description);
            if (path.empty())
            {
                return;
            }
            fmod::Attributes3d attributes{};
            const bool positioned = api.event_get_3d_attributes(instance, &attributes) == fmod::OK;
            if (logging && log_matches(path))
            {
                // FMOD space is (x, z, y) of the world; logged back in world coordinates.
                DMK::log().info(
                    "Event: {} at {:.2f} {:.2f} {:.2f}{}",
                    path,
                    attributes.position.x,
                    attributes.position.z,
                    attributes.position.y,
                    positioned ? "" : " (no 3D attributes)"
                );
            }
            if (table == nullptr)
            {
                return;
            }
            const auto rules = table->rules_of_event.find(path);
            if (rules == table->rules_of_event.end())
            {
                return;
            }
            // FMOD space (x, z, y) back to the world, where the tracked entities live.
            const game::entities::Position at{attributes.position.x, attributes.position.z, attributes.position.y};
            const auto applies = [&](const Rule &rule)
            {
                return rule.only_for.empty() ||
                       (positioned && game::entities::any_near(rule.only_for, at, constants::REPLACES_FOR_RADIUS));
            };
            const auto rule = std::ranges::find_if(rules->second, applies);
            if (rule == rules->second.end())
            {
                return;
            }
            // Muted only once the replacement is queued, so a full queue leaves the game's sound playing.
            if (request_replacement(rule->key, instance, positioned ? std::optional(attributes) : std::nullopt))
            {
                (void)api.event_set_volume(instance, 0.0f);
            }
        }
        catch (...)
        {
            // The game's audio thread must never see an exception; a failed lookup only skips this start.
        }
    }
} // namespace SoundFramework::audio::replacements
