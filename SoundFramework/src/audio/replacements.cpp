/**
 * @file audio/replacements.cpp
 * @brief The event-to-sound table and the event start handler.
 *
 * The table is immutable once published and swapped as a whole, so the audio thread reads it without a lock. Event
 * paths are cached per description handle, because asking FMOD for a path copies a string out of the strings bank.
 *
 * A replacement with PlayAt is held from its event's start until the game sets the named parameter on that event
 * instance. Held replacements live in a short list; the parameter hook checks one atomic flag first, so the many
 * parameter changes of events without a held replacement pass straight through.
 */

#include "audio/replacements.hpp"
#include "audio/engine.hpp"
#include "constants.hpp"
#include "fmod/fmod_api.hpp"
#include "game/entity_tracker.hpp"
#include "packs/ini_document.hpp"

#include <DetourModKit.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace SoundFramework::audio::replacements
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        /** @brief One sound replacing one event, optionally only at named entities and at a later stage. */
        struct Rule
        {
            std::string key;
            /// Entity names (lower case); empty means every start of the event.
            std::vector<std::string> only_for;
            /// The parameter (lower case) whose first qualifying set plays the sound; empty plays it at the start.
            std::string play_at;
            /// The lowest value of @ref play_at that counts; any value counts without it.
            std::optional<float> play_at_value;
            /// KeepOriginal: the event stays audible and the sound plays on top of it.
            bool keep_original = false;
        };

        /** @brief A replacement held on a started event until its PlayAt parameter is set. */
        struct Held
        {
            void *instance = nullptr;
            std::string key;
            std::string parameter;
            std::optional<float> value;
            /// Whether the event was muted at its start (no KeepOriginal).
            bool muted = true;
            Clock::time_point since;
        };

        /// Held replacements beyond this many give way, oldest first; each one lasts at most one event.
        constexpr std::size_t HELD_LIMIT = 64;
        /**
         * @brief A hold younger than this is never dropped as finished: the start hook holds before the original start
         *        runs, so a fresh event still reads as stopped for a moment.
         */
        constexpr std::chrono::milliseconds HELD_GRACE{1000};

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

        /// Instances the parameter log remembers values for before it starts over.
        constexpr std::size_t LOGGED_INSTANCE_LIMIT = 256;

        std::mutex s_logged_mutex;
        /// The last value logged per event instance and parameter, so a value the game sets again is logged once.
        std::unordered_map<void *, std::vector<std::pair<std::string, float>>> s_logged;

        std::mutex s_held_mutex;
        std::vector<Held> s_held;
        /// Whether s_held has entries; written under s_held_mutex, read first by the parameter hook.
        std::atomic<bool> s_holding{false};

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

        /**
         * @brief True when @p value differs from the last value logged for @p parameter on @p instance. The game sets
         *        dozens of parameters on every event and some every frame, mostly to the value they already hold.
         */
        [[nodiscard]] bool parameter_changed(void *instance, std::string_view parameter, float value)
        {
            const std::lock_guard lock(s_logged_mutex);
            if (s_logged.size() >= LOGGED_INSTANCE_LIMIT && !s_logged.contains(instance))
            {
                s_logged.clear();
            }
            std::vector<std::pair<std::string, float>> &values = s_logged[instance];
            const auto found =
                std::ranges::find_if(values, [&](const auto &entry) { return entry.first == parameter; });
            if (found == values.end())
            {
                values.emplace_back(std::string(parameter), value);
                return true;
            }
            // Only an exact repeat is skipped.
            if (found->second == value)
            {
                return false;
            }
            found->second = value;
            return true;
        }

        void forget_logged_parameters() noexcept
        {
            const std::lock_guard lock(s_logged_mutex);
            s_logged.clear();
        }

        void drop_holds() noexcept
        {
            const std::lock_guard lock(s_held_mutex);
            s_held.clear();
            s_holding.store(false, std::memory_order_release);
        }

        /**
         * @brief Holds @p held until its parameter is set. Holds of events that stopped before their parameter was set
         *        are dropped first; FMOD is asked about them outside the lock.
         */
        void hold(Held held)
        {
            std::vector<void *> older;
            {
                const std::lock_guard lock(s_held_mutex);
                for (const Held &entry : s_held)
                {
                    if (held.since - entry.since >= HELD_GRACE)
                    {
                        older.push_back(entry.instance);
                    }
                }
            }
            const fmod::Api &api = fmod::api();
            std::vector<void *> finished;
            for (void *instance : older)
            {
                int state = fmod::PLAYBACK_STOPPED;
                if (api.event_get_playback_state(instance, &state) != fmod::OK || state == fmod::PLAYBACK_STOPPED)
                {
                    finished.push_back(instance);
                }
            }
            const std::lock_guard lock(s_held_mutex);
            std::erase_if(
                s_held,
                [&](const Held &entry)
                {
                    return entry.instance == held.instance ||
                           std::ranges::find(finished, entry.instance) != finished.end();
                }
            );
            if (s_held.size() >= HELD_LIMIT)
            {
                s_held.erase(s_held.begin());
            }
            s_held.push_back(std::move(held));
            s_holding.store(true, std::memory_order_release);
        }

        /** @brief Takes the hold on @p instance that @p parameter set to @p value releases, if there is one. */
        [[nodiscard]] std::optional<Held> release(void *instance, std::string_view parameter, float value)
        {
            const std::lock_guard lock(s_held_mutex);
            const auto found = std::ranges::find_if(
                s_held,
                [&](const Held &entry)
                {
                    return entry.instance == instance && packs::iequals(entry.parameter, parameter) &&
                           (!entry.value.has_value() || value >= *entry.value);
                }
            );
            if (found == s_held.end())
            {
                return std::nullopt;
            }
            std::optional<Held> due(std::move(*found));
            s_held.erase(found);
            s_holding.store(!s_held.empty(), std::memory_order_release);
            return due;
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
                Rule rule{
                    .key = key,
                    .only_for = sound.replaces_for,
                    .play_at = sound.play_at,
                    .play_at_value = sound.play_at_value,
                    .keep_original = sound.keep_original,
                };
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
                        rules.back() = std::move(rule);
                    }
                    else
                    {
                        rules.push_back(std::move(rule));
                    }
                    continue;
                }
                const auto unfiltered =
                    std::ranges::find_if(rules, [](const Rule &entry) { return entry.only_for.empty(); });
                rules.insert(unfiltered, std::move(rule));
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
        // A held key may be gone from the new catalog, so events held at this moment play nothing and stay muted.
        drop_holds();
    }

    void clear() noexcept
    {
        s_table.store(nullptr);
        drop_holds();
        forget_logged_parameters();
        const std::lock_guard lock(s_path_mutex);
        s_paths.clear();
    }

    void set_event_log_filter(std::string_view filter)
    {
        std::string lowered(filter);
        std::ranges::transform(lowered, lowered.begin(), lower);
        {
            const std::lock_guard lock(s_filter_mutex);
            s_filter = std::move(lowered);
            s_logging.store(!s_filter.empty(), std::memory_order_release);
        }
        // A new filter shows the parameters of the events it matches from their current values on.
        forget_logged_parameters();
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
                (void)DMK::log().try_log(
                    DMK::LogLevel::Info,
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
            const bool mute = !rule->keep_original;
            if (!rule->play_at.empty())
            {
                // Muted from its start; the replacement waits for the stage PlayAt names.
                hold(
                    Held{
                        .instance = instance,
                        .key = rule->key,
                        .parameter = rule->play_at,
                        .value = rule->play_at_value,
                        .muted = mute,
                        .since = Clock::now(),
                    }
                );
                if (mute)
                {
                    (void)api.event_set_volume(instance, 0.0f);
                }
                return;
            }
            // Muted only once the replacement is queued, so a full queue leaves the game's sound playing.
            if (request_replacement(rule->key, instance, positioned ? std::optional(attributes) : std::nullopt) && mute)
            {
                (void)api.event_set_volume(instance, 0.0f);
            }
        }
        catch (...)
        {
            // The game's audio thread must never see an exception; a failed lookup only skips this start.
        }
    }

    void on_event_parameter(void *instance, const char *name, float value) noexcept
    {
        try
        {
            const bool holding = s_holding.load(std::memory_order_acquire);
            const bool logging = s_logging.load(std::memory_order_acquire);
            if (instance == nullptr || name == nullptr || (!holding && !logging))
            {
                return;
            }
            const std::string_view parameter(name);
            const fmod::Api &api = fmod::api();
            if (logging)
            {
                void *description = nullptr;
                if (api.event_get_description(instance, &description) == fmod::OK && description != nullptr)
                {
                    const std::string path = path_of(description);
                    if (!path.empty() && log_matches(path) && parameter_changed(instance, parameter, value))
                    {
                        (void)DMK::log()
                            .try_log(DMK::LogLevel::Info, "Event: {} parameter '{}' = {}", path, parameter, value);
                    }
                }
            }
            if (!holding)
            {
                return;
            }
            std::optional<Held> due = release(instance, parameter, value);
            if (!due.has_value())
            {
                return;
            }
            fmod::Attributes3d attributes{};
            const bool positioned = api.event_get_3d_attributes(instance, &attributes) == fmod::OK;
            if (!request_replacement(
                    std::move(due->key),
                    instance,
                    positioned ? std::optional(attributes) : std::nullopt
                ) &&
                due->muted)
            {
                // A full queue leaves the rest of the game's event audible. WHGame.dll imports no event volume setter
                // (its only volume import is VCA::setVolume), so an event's own volume stays at FMOD's default of 1.
                (void)api.event_set_volume(instance, 1.0f);
            }
        }
        catch (...)
        {
            // The calling game thread must never see an exception; a failed lookup only skips this change.
        }
    }
} // namespace SoundFramework::audio::replacements
