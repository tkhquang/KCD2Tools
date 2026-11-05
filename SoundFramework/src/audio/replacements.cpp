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
 *
 * The game reuses event instances (it starts the same one again), and a volume set on an instance stays with it. So
 * once an event's volume has been shaped (adjusted, a speech kind, or replaced), every later start of that event sets
 * its volume again, 1 included; otherwise a setting returned to 1, or a removed adjustment, would leave a reused
 * instance at the old volume.
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
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
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
            /// The volume the event would have played at unmuted, restored when its replacement cannot be queued.
            float volume = 1.0f;
            Clock::time_point since;
        };

        /** @brief One Adjusts section: the events it names and the volume their starts take. */
        struct Adjust
        {
            /// Lower-case event paths, `*` matching any run.
            std::vector<std::string> events;
            float volume = 1.0f;
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
            /// Adjusts sections in load order; the last one naming an event wins it.
            std::vector<Adjust> adjusts;
            /// Tells the per-event adjustment cache which table its entries were resolved against; never 0.
            std::uint64_t generation = 0;
        };

        std::atomic<std::shared_ptr<const Table>> s_table;
        std::atomic<std::uint64_t> s_generation{0};

        /** @brief What a start needs to know about one event description, cached per description handle. */
        struct EventInfo
        {
            /// Lower case; empty when FMOD cannot name the event.
            std::string path;
            /// The kind of speech the event plays, from its path alone.
            std::optional<VoiceKind> voice;
            /// The table generation @ref adjust was resolved against; 0 = not yet.
            std::uint64_t adjusted_in = 0;
            /// The volume of the Adjusts section that wins the event, or nothing.
            std::optional<float> adjust;
            /// Sticky: some start of this event had its volume shaped, so every later start sets it again.
            bool shaped = false;
        };

        std::mutex s_path_mutex;
        /// Event facts by description handle; descriptions stay valid while their bank is loaded.
        std::unordered_map<void *, EventInfo> s_events;

        /// Volume per VoiceKind, indexed by its value.
        std::array<std::atomic<float>, 3> s_voice_volumes{1.0f, 1.0f, 1.0f};
        /**
         * @brief Set once a speech volume left 1 or an adjustment was published; until then a start of an event no
         *        pack replaces and no log watches passes straight through. Never reset while loaded: an instance
         *        shaped earlier must still be set back.
         */
        std::atomic<bool> s_shaping{false};

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

        /** @brief The kind of speech the event at lower-case @p path plays, or nothing for any other event. */
        [[nodiscard]] std::optional<VoiceKind> voice_kind_of(std::string_view path) noexcept
        {
            if (path == constants::DIALOGUE_VOICE_EVENT)
            {
                return VoiceKind::Dialogue;
            }
            if (path == constants::CUTSCENE_VOICE_EVENT)
            {
                return VoiceKind::Cutscene;
            }
            if (path.starts_with(constants::BARK_VOICE_EVENT_PREFIX))
            {
                return VoiceKind::Bark;
            }
            return std::nullopt;
        }

        /** @brief The volume of the last adjustment in @p table that names @p path, or nothing. */
        [[nodiscard]] std::optional<float> adjust_of(const Table &table, std::string_view path) noexcept
        {
            for (auto adjust = table.adjusts.rbegin(); adjust != table.adjusts.rend(); ++adjust)
            {
                if (std::ranges::any_of(
                        adjust->events,
                        [&](const std::string &pattern) { return packs::wildcard_match(pattern, path); }
                    ))
                {
                    return adjust->volume;
                }
            }
            return std::nullopt;
        }

        /**
         * @brief The cached facts of @p description; the path is empty when FMOD cannot name it. The path is asked
         *        for once, because FMOD copies it out of the strings bank on every call.
         */
        [[nodiscard]] EventInfo info_of(void *description)
        {
            {
                const std::lock_guard lock(s_path_mutex);
                const auto found = s_events.find(description);
                if (found != s_events.end())
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
            EventInfo info{.path = std::move(path)};
            info.voice = voice_kind_of(info.path);
            const std::lock_guard lock(s_path_mutex);
            // Another thread may have cached it meanwhile; its entry may already carry a resolved adjustment.
            return s_events.try_emplace(description, std::move(info)).first->second;
        }

        /** @brief The lower-case path of @p description, cached; empty when FMOD cannot name it. */
        [[nodiscard]] std::string path_of(void *description)
        {
            return info_of(description).path;
        }

        /** @brief Writes back the adjustment and shaped state @p info carries for @p description. */
        void remember(void *description, const EventInfo &info)
        {
            const std::lock_guard lock(s_path_mutex);
            const auto [cached, inserted] = s_events.try_emplace(description, info);
            if (!inserted)
            {
                cached->second.adjusted_in = info.adjusted_in;
                cached->second.adjust = info.adjust;
                cached->second.shaped = cached->second.shaped || info.shaped;
            }
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
        for (const packs::AdjustDef &adjust : catalog.adjusts)
        {
            table->adjusts.push_back(Adjust{.events = adjust.events, .volume = adjust.volume});
        }
        if (!table->adjusts.empty())
        {
            DMK::log().info("Adjustments: {} section(s) change the volume of game events", table->adjusts.size());
            s_shaping.store(true, std::memory_order_release);
        }
        table->generation = s_generation.fetch_add(1, std::memory_order_relaxed) + 1;
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
        s_events.clear();
    }

    void set_voice_volume(VoiceKind kind, float volume) noexcept
    {
        // NaN fails both comparisons of clamp; it means "unset", so the default applies.
        const float clamped = std::isnan(volume) ? 1.0f : std::clamp(volume, 0.0f, constants::MAX_EVENT_VOLUME);
        s_voice_volumes[static_cast<std::size_t>(kind)].store(clamped, std::memory_order_relaxed);
        if (clamped != 1.0f)
        {
            s_shaping.store(true, std::memory_order_release);
        }
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
            const bool replacing = table != nullptr && !table->rules_of_event.empty();
            if (instance == nullptr || (!replacing && !logging && !s_shaping.load(std::memory_order_acquire)))
            {
                return;
            }
            const fmod::Api &api = fmod::api();
            void *description = nullptr;
            if (api.event_get_description(instance, &description) != fmod::OK || description == nullptr)
            {
                return;
            }
            EventInfo info = info_of(description);
            if (info.path.empty())
            {
                return;
            }
            const std::string &path = info.path;
            bool changed = false;
            // Resolved once per event and table; a cleared table (generation 0) drops a cached adjustment.
            const std::uint64_t generation = table == nullptr ? 0 : table->generation;
            if (info.adjusted_in != generation)
            {
                info.adjust = table == nullptr ? std::nullopt : adjust_of(*table, path);
                info.adjusted_in = generation;
                changed = true;
            }
            const std::vector<Rule> *rules = nullptr;
            if (table != nullptr)
            {
                const auto found = table->rules_of_event.find(path);
                if (found != table->rules_of_event.end())
                {
                    rules = &found->second;
                }
            }
            if (!info.shaped && (info.voice.has_value() || info.adjust.has_value() || rules != nullptr))
            {
                info.shaped = true;
                changed = true;
            }
            if (changed)
            {
                remember(description, info);
            }
            float volume = info.adjust.value_or(1.0f);
            if (info.voice.has_value())
            {
                volume *= s_voice_volumes[static_cast<std::size_t>(*info.voice)].load(std::memory_order_relaxed);
            }
            if (info.shaped)
            {
                (void)api.event_set_volume(instance, volume);
            }
            fmod::Attributes3d attributes{};
            const bool positioned = api.event_get_3d_attributes(instance, &attributes) == fmod::OK;
            if (logging && log_matches(path))
            {
                // FMOD space is (x, z, y) of the world; logged back in world coordinates.
                (void)DMK::log().try_log(
                    DMK::LogLevel::Info,
                    "Event: {} at {:.2f} {:.2f} {:.2f}{}{}",
                    path,
                    attributes.position.x,
                    attributes.position.z,
                    attributes.position.y,
                    positioned ? "" : " (no 3D attributes)",
                    volume != 1.0f ? std::format(" volume {:.2f}", volume) : std::string{}
                );
            }
            if (rules == nullptr)
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
            const auto rule = std::ranges::find_if(*rules, applies);
            if (rule == rules->end())
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
                        .volume = volume,
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
                // A full queue leaves the rest of the game's event audible, at the volume its start gave it.
                (void)api.event_set_volume(instance, due->volume);
            }
        }
        catch (...)
        {
            // The calling game thread must never see an exception; a failed lookup only skips this change.
        }
    }
} // namespace SoundFramework::audio::replacements
