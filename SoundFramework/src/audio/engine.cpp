/**
 * @file audio/engine.cpp
 * @brief The playback worker: requests in, FMOD channels and game event instances out.
 *
 * Every FMOD object the engine creates belongs to the worker: requests are queued under a mutex and drained by the
 * worker, which alone touches the sound cache, the bus locks and the instance list. FMOD's API is thread safe, so the
 * worker calls it directly. Sounds are opened with FMOD's own file system, because the game routes FMOD's file
 * callbacks to CryPak, which sees only pak paths.
 */

#include "audio/engine.hpp"
#include "audio/line_table.hpp"
#include "audio/replacements.hpp"
#include "fmod/event_hook.hpp"
#include "packs/catalog.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <deque>
#include <mutex>
#include <random>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace SoundFramework::audio
{
    namespace
    {
        using fmod::Attributes3d;
        using fmod::Vector;
        using packs::SoundDef;

        constexpr std::chrono::milliseconds TICK{16};
        /// How often the pack INIs are checked for edits.
        constexpr std::uint64_t PACK_POLL_MS = 1000;
        /// How long a play may wait for the game's audio (system or bus) before it is dropped.
        constexpr std::uint64_t PENDING_LIMIT_MS = 2000;
        /// How long the game's bank count must hold still before the event variants are looked up and loaded again.
        constexpr std::uint64_t BANK_SETTLE_MS = 1000;
        /// Requests beyond this many unhandled ones are dropped, so a runaway script cannot grow the queue.
        constexpr std::size_t QUEUE_LIMIT = 256;

        enum class Kind : std::uint8_t
        {
            Play,
            Stop,
            StopAll,
            Reload,
            List,
        };

        struct Request
        {
            Kind kind = Kind::Play;
            std::string name;
            std::optional<WorldPosition> at;
            /// Replacements: the muted game event to follow, and its attributes at start (FMOD space).
            void *event = nullptr;
            std::optional<Attributes3d> event_attributes;
            std::uint64_t queued_ms = 0;
        };

        /** @brief What the listener hook last saw. Written on the game's audio thread, read by the worker. */
        struct Capture
        {
            void *system = nullptr;
            Attributes3d listener{};
            /// The listener hook has run: on_system() can record the system before the listener is ever set.
            bool listener_set = false;
        };

        struct BusEntry
        {
            void *bus = nullptr;
            void *group = nullptr;
            bool failed = false;
        };

        struct Instance
        {
            std::string key;
            /// The channel of a file variant; null for an event variant.
            void *channel = nullptr;
            /// The game event instance of an event variant, already released so FMOD frees it when it stops.
            void *own_event = nullptr;
            /// A stream plays its own Sound object, released when the instance ends.
            void *stream = nullptr;
            /// The game event this instance replaces and follows, until the event stops.
            void *follow = nullptr;
            /// The instance does not end by itself, so a replacement ends with the game event it follows.
            bool loop = false;
        };

        /** @brief Per-sound play history: the last variant and the last play time. */
        struct History
        {
            std::size_t last_variant = SIZE_MAX;
            std::uint64_t last_play_ms = 0;
        };

        /** @brief Worker state. Touched only by the worker, and by shutdown() after the worker joined. */
        struct State
        {
            std::filesystem::path mods_dir;
            std::filesystem::path defaults_file;
            packs::Catalog catalog;
            void *system = nullptr;
            void *core = nullptr;
            std::unordered_map<std::string, BusEntry> buses;
            /// Loaded samples by "<mode>|<path>"; the same file opened with another mode is another sample.
            std::unordered_map<std::string, void *> samples;
            /// Descriptions of the catalog's event variants by path, with their sample data loaded; null when missing.
            std::unordered_map<std::string, void *> descriptions;
            /// The game's bank count at the last check; -1 before the catalog attaches.
            int bank_count = -1;
            /// When to look the event variants up and load them again; 0 for no pending pass.
            std::uint64_t reload_events_ms = 0;
            std::deque<Instance> instances;
            std::unordered_map<std::string, History> history;
            std::vector<Request> pending;
            std::minstd_rand random{static_cast<std::minstd_rand::result_type>(GetTickCount64())};
            std::uint64_t next_poll_ms = 0;
            bool announced = false;
        };

        std::mutex s_capture_mutex;
        Capture s_capture;
        /**
         * @brief The system on_system() last recorded, so its every-frame calls skip the lock. Only a filter:
         *        s_capture under its mutex stays the value every reader uses, so relaxed order is enough.
         */
        std::atomic<void *> s_seen_system{nullptr};

        std::mutex s_request_mutex;
        std::vector<Request> s_requests;

        State s_state;
        std::optional<DMK::StoppableWorker> s_worker;

        [[nodiscard]] std::uint64_t now_ms() noexcept
        {
            return GetTickCount64();
        }

        /** @brief Queues @p request for the worker; false when the queue is full (logged after the lock). */
        bool enqueue(Request request)
        {
            request.queued_ms = now_ms();
            {
                const std::lock_guard lock(s_request_mutex);
                if (s_requests.size() < QUEUE_LIMIT)
                {
                    s_requests.push_back(std::move(request));
                    return true;
                }
            }
            // The FMOD start and parameter hooks queue replacements through here.
            (void)DMK::log().try_log(DMK::LogLevel::Warning, "Engine: request queue full; '{}' dropped", request.name);
            return false;
        }

        [[nodiscard]] float random01() noexcept
        {
            return std::uniform_real_distribution<float>(0.0f, 1.0f)(s_state.random);
        }

        void log_catalog_load(const packs::CatalogLoad &load)
        {
            for (const std::string &problem : load.problems)
            {
                DMK::log().warning("Packs: {}", problem);
            }
            for (const std::string &note : load.notes)
            {
                DMK::log().info("Packs: {}", note);
            }
            for (const packs::PackFile &file : load.catalog.files)
            {
                DMK::log().info("Packs: read {}", packs::path_text(file.ini));
            }
            DMK::log().info(
                "Packs: {} sound(s) from {} pack file(s) under {}",
                load.catalog.sounds.size(),
                load.catalog.files.size(),
                packs::path_text(s_state.mods_dir)
            );
        }

        void stop_instance(Instance &instance) noexcept
        {
            const fmod::Api &api = fmod::api();
            if (instance.own_event != nullptr)
            {
                (void)api.event_stop(instance.own_event, fmod::STOP_IMMEDIATE);
                return;
            }
            (void)api.channel_stop(instance.channel);
            if (instance.stream != nullptr)
            {
                (void)api.sound_release(instance.stream);
                instance.stream = nullptr;
            }
        }

        /** @brief Stops every instance and releases every sample, stream and event sample load; bus locks stay. */
        void release_sounds() noexcept
        {
            const fmod::Api &api = fmod::api();
            for (Instance &instance : s_state.instances)
            {
                stop_instance(instance);
            }
            s_state.instances.clear();
            // Releasing a sample stops every channel still playing it.
            for (auto &[key, sound] : s_state.samples)
            {
                (void)api.sound_release(sound);
            }
            s_state.samples.clear();
            for (auto &[path, description] : s_state.descriptions)
            {
                if (description != nullptr)
                {
                    (void)api.description_unload_sample_data(description);
                }
            }
            s_state.descriptions.clear();
            s_state.bank_count = -1;
            s_state.reload_events_ms = 0;
        }

        void release_buses() noexcept
        {
            const fmod::Api &api = fmod::api();
            for (auto &[path, entry] : s_state.buses)
            {
                if (entry.bus != nullptr)
                {
                    (void)api.bus_unlock_channel_group(entry.bus);
                }
            }
            s_state.buses.clear();
        }

        void release_all() noexcept
        {
            if (fmod::api().sound_release == nullptr)
            {
                return;
            }
            release_sounds();
            release_buses();
        }

        /** @brief Resolves and locks @p path's bus on first use; the group arrives on a later tick. */
        [[nodiscard]] BusEntry &bus_entry(const std::string &path)
        {
            BusEntry &entry = s_state.buses[path];
            const fmod::Api &api = fmod::api();
            if (entry.bus == nullptr && !entry.failed)
            {
                const fmod::Result got = api.studio_get_bus(s_state.system, path.c_str(), &entry.bus);
                if (got != fmod::OK || entry.bus == nullptr)
                {
                    DMK::log().error("Engine: bus {} unavailable (FMOD_RESULT {})", path, got);
                    entry.bus = nullptr;
                    entry.failed = true;
                }
                else
                {
                    (void)api.bus_lock_channel_group(entry.bus);
                }
            }
            if (entry.bus != nullptr && entry.group == nullptr)
            {
                // The lock completes on the game's next Studio update, so this may need a few ticks.
                (void)api.bus_get_channel_group(entry.bus, &entry.group);
            }
            return entry;
        }

        /** @brief Looks up the description of @p path and loads its sample data; null when no loaded bank holds it. */
        [[nodiscard]] void *load_event(const std::string &path) noexcept
        {
            const fmod::Api &api = fmod::api();
            void *description = nullptr;
            if (api.studio_get_event(s_state.system, path.c_str(), &description) != fmod::OK || description == nullptr)
            {
                return nullptr;
            }
            (void)api.description_load_sample_data(description);
            return description;
        }

        /**
         * @brief Locks every bus the catalog uses and loads every event variant, so the first play of a sound starts at
         *        once instead of after its loading.
         */
        void attach_catalog()
        {
            bool missing = false;
            for (const auto &[key, sound] : s_state.catalog.sounds)
            {
                if (!sound.files.empty())
                {
                    (void)bus_entry(sound.bus);
                }
                for (const std::string &path : sound.events)
                {
                    const auto [entry, added] = s_state.descriptions.try_emplace(path, nullptr);
                    if (added)
                    {
                        entry->second = load_event(path);
                        missing = missing || entry->second == nullptr;
                    }
                }
            }
            (void)fmod::api().studio_get_bank_count(s_state.system, &s_state.bank_count);
            // The game can still be loading its banks, so a missing event waits for the settled pass to report it.
            s_state.reload_events_ms = missing ? now_ms() + BANK_SETTLE_MS : 0;
        }

        /**
         * @brief Looks every event variant up and loads it again once the game's bank count has held still after a
         *        change, and reports each event that no loaded bank holds.
         * @details A sample data load requested before the bank with the event's audio is in reports success but stays
         *          empty, and the event then plays silent after the bank arrives. The game loads its banks after the
         *          framework first sees its Studio system, and a bank unload leaves its descriptions stale, so each
         *          pass unloads the old description and looks the path up again.
         */
        void track_banks()
        {
            const fmod::Api &api = fmod::api();
            int count = 0;
            if (s_state.descriptions.empty() || api.studio_get_bank_count(s_state.system, &count) != fmod::OK)
            {
                return;
            }
            const std::uint64_t now = now_ms();
            if (count != s_state.bank_count)
            {
                s_state.bank_count = count;
                s_state.reload_events_ms = now + BANK_SETTLE_MS;
                return;
            }
            if (s_state.reload_events_ms == 0 || now < s_state.reload_events_ms)
            {
                return;
            }
            s_state.reload_events_ms = 0;
            for (auto &[path, description] : s_state.descriptions)
            {
                if (description != nullptr)
                {
                    (void)api.description_unload_sample_data(description);
                }
                description = load_event(path);
                if (description == nullptr)
                {
                    DMK::log().warning("Engine: game event {} is in none of the game's loaded banks", path);
                }
            }
            DMK::log().debug("Engine: the game has {} bank(s); event audio loaded again", count);
        }

        void load_packs()
        {
            const packs::CatalogLoad load = packs::load_catalog(s_state.mods_dir, s_state.defaults_file);
            log_catalog_load(load);
            s_state.catalog = load.catalog;
            replacements::publish(s_state.catalog);
            line_table::publish(s_state.catalog);
            s_state.history.clear();
            s_state.next_poll_ms = now_ms() + PACK_POLL_MS;
            if (s_state.core != nullptr)
            {
                attach_catalog();
            }
        }

        void reload_packs(std::string_view why)
        {
            DMK::log().info("Engine: reloading packs ({}); stopping {} sound(s)", why, s_state.instances.size());
            release_sounds();
            load_packs();
        }

        /** @brief Adopts @p system: drops everything made on a previous one and fetches its core system. */
        [[nodiscard]] bool adopt(void *system)
        {
            const fmod::Api &api = fmod::api();
            release_all();
            s_state.system = system;
            s_state.core = nullptr;
            const fmod::Result core = api.studio_get_core_system(system, &s_state.core);
            DMK::log().info(
                "Engine: using the game's Studio system {} (core {}, FMOD_RESULT {})",
                DMK::format::format_address(reinterpret_cast<std::uintptr_t>(system)),
                DMK::format::format_address(reinterpret_cast<std::uintptr_t>(s_state.core)),
                core
            );
            if (core != fmod::OK)
            {
                s_state.core = nullptr;
                return false;
            }
            attach_catalog();
            return true;
        }

        [[nodiscard]] std::uint32_t mode_of(const SoundDef &sound) noexcept
        {
            std::uint32_t mode = sound.loop ? fmod::MODE_LOOP_NORMAL : fmod::MODE_LOOP_OFF;
            mode |= sound.stream ? fmod::MODE_CREATESTREAM : fmod::MODE_CREATESAMPLE;
            mode |= sound.space == packs::Space::Positioned ? fmod::MODE_3D | fmod::MODE_3D_LINEARSQUAREROLLOFF
                                                            : fmod::MODE_2D;
            return mode;
        }

        /** @brief Opens @p file from disk with FMOD's own file system. */
        [[nodiscard]] void *open_sound(const std::filesystem::path &file, std::uint32_t mode)
        {
            fmod::CreateSoundExInfo info;
            info.ignore_set_file_system = 1;
            const std::string path = packs::path_text(file);
            void *sound = nullptr;
            const fmod::Result created = fmod::api().create_sound(s_state.core, path.c_str(), mode, &info, &sound);
            if (created != fmod::OK)
            {
                DMK::log().error("Engine: cannot open {} (FMOD_RESULT {})", path, created);
                return nullptr;
            }
            return sound;
        }

        [[nodiscard]] void *sample_for(const std::filesystem::path &file, std::uint32_t mode)
        {
            std::string key = std::to_string(mode) + "|" + packs::path_text(file);
            const auto found = s_state.samples.find(key);
            if (found != s_state.samples.end())
            {
                return found->second;
            }
            void *sound = open_sound(file, mode);
            if (sound != nullptr)
            {
                s_state.samples.emplace(std::move(key), sound);
            }
            return sound;
        }

        /** @brief Picks a variant (files, then events) at random, never the previous one when there is a choice. */
        [[nodiscard]] std::size_t pick_variant(const SoundDef &sound, History &history) noexcept
        {
            const std::size_t count = sound.files.size() + sound.events.size();
            if (count <= 1)
            {
                return 0;
            }
            std::size_t pick = std::uniform_int_distribution<std::size_t>(0, count - 1)(s_state.random);
            if (pick == history.last_variant)
            {
                pick = (pick + 1 + std::uniform_int_distribution<std::size_t>(0, count - 2)(s_state.random)) % count;
            }
            return pick;
        }

        /**
         * @brief Moves a replacement onto its game event's current position.
         * @return False when the event has stopped and the replacement loops, so it must end with it.
         */
        [[nodiscard]] bool follow_event(Instance &instance) noexcept
        {
            const fmod::Api &api = fmod::api();
            int state = fmod::PLAYBACK_STOPPED;
            if (api.event_get_playback_state(instance.follow, &state) != fmod::OK || state == fmod::PLAYBACK_STOPPED)
            {
                // A one-shot replacement plays out; a looping one ends with the event it replaces.
                instance.follow = nullptr;
                return !instance.loop;
            }
            Attributes3d attributes{};
            if (api.event_get_3d_attributes(instance.follow, &attributes) == fmod::OK)
            {
                if (instance.own_event != nullptr)
                {
                    (void)api.event_set_3d_attributes(instance.own_event, &attributes);
                }
                else
                {
                    (void)api.channel_set_3d_attributes(instance.channel, &attributes.position, &attributes.velocity);
                }
            }
            return true;
        }

        /** @brief True while @p instance still sounds: its channel plays, or its event has not stopped. */
        [[nodiscard]] bool still_playing(const Instance &instance) noexcept
        {
            const fmod::Api &api = fmod::api();
            if (instance.own_event != nullptr)
            {
                int state = fmod::PLAYBACK_STOPPED;
                return api.event_get_playback_state(instance.own_event, &state) == fmod::OK &&
                       state != fmod::PLAYBACK_STOPPED;
            }
            int playing = 0;
            return api.channel_is_playing(instance.channel, &playing) == fmod::OK && playing != 0;
        }

        /** @brief Drops instances that ended or were stolen, releasing their streams. */
        void prune_instances() noexcept
        {
            const fmod::Api &api = fmod::api();
            std::erase_if(
                s_state.instances,
                [&](Instance &instance)
                {
                    if (still_playing(instance))
                    {
                        if (instance.follow == nullptr || follow_event(instance))
                        {
                            return false;
                        }
                        stop_instance(instance);
                        return true;
                    }
                    if (instance.stream != nullptr)
                    {
                        (void)api.sound_release(instance.stream);
                    }
                    return true;
                }
            );
        }

        /** @brief The outcome of one play attempt. */
        enum class PlayOutcome : std::uint8_t
        {
            Done,
            Retry,
        };

        /** @brief Where a play of @p sound goes: the request's position, the replaced event's, or the listener. */
        [[nodiscard]] Attributes3d
        placement(const SoundDef &sound, const Request &request, const Attributes3d &listener)
        {
            Attributes3d where = listener;
            where.velocity = Vector{0.0f, 0.0f, 0.0f};
            if (sound.space != packs::Space::Positioned)
            {
                return where;
            }
            if (request.event_attributes.has_value())
            {
                where.position = request.event_attributes->position;
                where.velocity = request.event_attributes->velocity;
            }
            else if (request.at.has_value())
            {
                where.position = fmod::from_world(request.at->x, request.at->y, request.at->z);
            }
            return where;
        }

        /** @brief Stops the oldest instance of @p key when @p sound is at its instance limit, so a new one fits. */
        void make_room(const SoundDef &sound, const std::string &key) noexcept
        {
            const auto same = [&](const Instance &instance) noexcept { return instance.key == key; };
            if (std::ranges::count_if(s_state.instances, same) >= sound.max_instances)
            {
                // MaxInstances is at least 1, so an instance of this key exists here.
                const auto oldest = std::ranges::find_if(s_state.instances, same);
                stop_instance(*oldest);
                s_state.instances.erase(oldest);
            }
        }

        /** @brief Starts @p file on a channel of @p group, the sound's bus; false when it cannot play. */
        [[nodiscard]] bool play_file(
            const SoundDef &sound,
            const std::filesystem::path &file,
            void *group,
            const Attributes3d &where,
            float volume,
            float pitch,
            Instance &out
        )
        {
            const std::uint32_t mode = mode_of(sound);
            void *stream = sound.stream ? open_sound(file, mode) : nullptr;
            void *source = sound.stream ? stream : sample_for(file, mode);
            if (source == nullptr)
            {
                return false;
            }
            make_room(sound, out.key);
            const fmod::Api &api = fmod::api();
            void *channel = nullptr;
            const fmod::Result played = api.play_sound(s_state.core, source, group, 1, &channel);
            if (played != fmod::OK || channel == nullptr)
            {
                DMK::log().error("Engine: playSound({}) -> FMOD_RESULT {}", sound.name, played);
                if (stream != nullptr)
                {
                    (void)api.sound_release(stream);
                }
                return false;
            }
            (void)api.channel_set_volume(channel, volume);
            (void)api.channel_set_pitch(channel, pitch);
            if (sound.space == packs::Space::Positioned)
            {
                (void)api.channel_set_3d_attributes(channel, &where.position, &where.velocity);
                (void)api.channel_set_3d_min_max(channel, sound.min_distance, sound.max_distance);
            }
            (void)api.channel_set_paused(channel, 0);
            out.channel = channel;
            out.stream = stream;
            out.loop = sound.loop;
            return true;
        }

        /**
         * @brief Starts a new instance of the game event @p path; false when no loaded bank holds it or FMOD refuses
         *        it.
         */
        [[nodiscard]] bool play_event(
            const SoundDef &sound,
            const std::string &path,
            const Attributes3d &where,
            float volume,
            float pitch,
            Instance &out
        )
        {
            const auto found = s_state.descriptions.find(path);
            if (found == s_state.descriptions.end() || found->second == nullptr)
            {
                return false;
            }
            void *description = found->second;
            make_room(sound, out.key);
            const fmod::Api &api = fmod::api();
            void *instance = nullptr;
            const fmod::Result created = api.description_create_instance(description, &instance);
            if (created != fmod::OK || instance == nullptr)
            {
                DMK::log().error("Engine: [{}] cannot create {} (FMOD_RESULT {})", sound.name, path, created);
                return false;
            }
            (void)api.event_set_volume(instance, volume);
            (void)api.event_set_pitch(instance, pitch);
            (void)api.event_set_3d_attributes(instance, &where);
            const fmod::Result started = fmod::start_own_event(instance);
            // FMOD frees a released instance once it stops, and its handle stays valid until then.
            (void)api.event_release(instance);
            if (started != fmod::OK)
            {
                DMK::log().error("Engine: [{}] cannot start {} (FMOD_RESULT {})", sound.name, path, started);
                return false;
            }
            out.own_event = instance;
            // An event FMOD does not mark one-shot counts as a loop, so a replacement ends with its game event.
            int oneshot = 0;
            out.loop = api.description_is_oneshot(description, &oneshot) != fmod::OK || oneshot == 0;
            return true;
        }

        [[nodiscard]] PlayOutcome play(const Request &request, const Attributes3d &listener)
        {
            const std::string key = request.event != nullptr ? request.name : packs::sound_key(request.name);
            const auto found = s_state.catalog.sounds.find(key);
            if (found == s_state.catalog.sounds.end())
            {
                DMK::log().warning(
                    "Engine: no sound named '{}' (sfw_list writes the catalog to this log)",
                    request.name
                );
                return PlayOutcome::Done;
            }
            const SoundDef &sound = found->second;
            if (s_state.core == nullptr)
            {
                return PlayOutcome::Retry;
            }

            History &history = s_state.history[key];
            const std::uint64_t now = now_ms();
            if (sound.cooldown > 0.0f && history.last_play_ms != 0 &&
                now - history.last_play_ms < static_cast<std::uint64_t>(sound.cooldown * 1000.0f))
            {
                return PlayOutcome::Done;
            }

            const std::size_t variant = pick_variant(sound, history);
            const bool file = variant < sound.files.size();
            void *group = nullptr;
            if (file)
            {
                const BusEntry &bus = bus_entry(sound.bus);
                if (bus.failed)
                {
                    return PlayOutcome::Done;
                }
                if (bus.group == nullptr)
                {
                    return PlayOutcome::Retry;
                }
                group = bus.group;
            }

            const float volume = sound.volume * (1.0f - sound.volume_variation * random01());
            const float pitch = sound.pitch * (1.0f + sound.pitch_variation * (2.0f * random01() - 1.0f));
            const Attributes3d where = placement(sound, request, listener);
            Instance instance{
                .key = key,
                .follow = sound.space == packs::Space::Positioned ? request.event : nullptr,
            };
            const bool started =
                file ? play_file(sound, sound.files[variant], group, where, volume, pitch, instance)
                     : play_event(sound, sound.events[variant - sound.files.size()], where, volume, pitch, instance);
            if (!started)
            {
                return PlayOutcome::Done;
            }

            history.last_variant = variant;
            history.last_play_ms = now;
            s_state.instances.push_back(std::move(instance));
            DMK::log().debug(
                "Engine: {} variant {} at {:.1f},{:.1f},{:.1f} (FMOD space) volume {:.2f} pitch {:.2f}",
                sound.name,
                variant,
                where.position.x,
                where.position.y,
                where.position.z,
                volume,
                pitch
            );
            return PlayOutcome::Done;
        }

        void stop_named(const std::string &name)
        {
            const std::string key = packs::sound_key(name);
            std::erase_if(
                s_state.instances,
                [&](Instance &instance)
                {
                    if (instance.key != key)
                    {
                        return false;
                    }
                    stop_instance(instance);
                    return true;
                }
            );
        }

        void stop_all() noexcept
        {
            for (Instance &instance : s_state.instances)
            {
                stop_instance(instance);
            }
            s_state.instances.clear();
        }

        void write_list()
        {
            std::vector<const SoundDef *> sounds;
            sounds.reserve(s_state.catalog.sounds.size());
            for (const auto &[key, sound] : s_state.catalog.sounds)
            {
                sounds.push_back(&sound);
            }
            std::ranges::sort(sounds, {}, [](const SoundDef *sound) { return packs::sound_key(sound->name); });
            DMK::log().info(
                "Catalog: {} sound(s), {} playing; {} voice line(s); {} mood(s)",
                sounds.size(),
                s_state.instances.size(),
                s_state.catalog.lines.size(),
                s_state.catalog.moods.size()
            );
            for (const auto &[key, mood] : s_state.catalog.moods)
            {
                DMK::log().info("Catalog: mood {} ({} pose(s)) from {}", mood.name, mood.poses.size(), mood.origin);
            }
            for (const SoundDef *sound : sounds)
            {
                DMK::log().info(
                    "Catalog: {} ({} file(s), {} event(s), {}, {}{}{}) from {}",
                    sound->name,
                    sound->files.size(),
                    sound->events.size(),
                    sound->bus,
                    sound->space == packs::Space::Positioned ? "3d" : "2d",
                    sound->loop ? ", loop" : "",
                    sound->stream ? ", stream" : "",
                    sound->origin
                );
            }
            for (const auto &[key, line] : s_state.catalog.lines)
            {
                DMK::log().info(
                    "Catalog: line {} ({} take(s), {} text key(s)) from {}",
                    line.name,
                    line.voices.size(),
                    line.text_keys.size(),
                    line.origin
                );
            }
        }

        void run_requests(const Attributes3d &listener)
        {
            std::vector<Request> batch = std::exchange(s_state.pending, {});
            {
                const std::lock_guard lock(s_request_mutex);
                std::ranges::move(s_requests, std::back_inserter(batch));
                s_requests.clear();
            }
            const std::uint64_t now = now_ms();
            for (Request &request : batch)
            {
                switch (request.kind)
                {
                case Kind::Play:
                    if (play(request, listener) == PlayOutcome::Retry)
                    {
                        if (now - request.queued_ms < PENDING_LIMIT_MS)
                        {
                            s_state.pending.push_back(std::move(request));
                        }
                        else
                        {
                            DMK::log().warning("Engine: '{}' dropped: the game's audio is not ready", request.name);
                        }
                    }
                    break;
                case Kind::Stop:
                    stop_named(request.name);
                    break;
                case Kind::StopAll:
                    stop_all();
                    break;
                case Kind::Reload:
                    reload_packs("requested");
                    break;
                case Kind::List:
                    write_list();
                    break;
                }
            }
        }

        void tick()
        {
            Capture capture;
            {
                const std::lock_guard lock(s_capture_mutex);
                capture = s_capture;
            }
            if (capture.system != nullptr && capture.system != s_state.system)
            {
                (void)adopt(capture.system);
            }
            if (s_state.core != nullptr)
            {
                track_banks();
                prune_instances();
                if (!s_state.announced)
                {
                    s_state.announced = true;
                    DMK::log().info("Engine: ready; {} sound(s) available", s_state.catalog.sounds.size());
                }
            }
            if (now_ms() >= s_state.next_poll_ms)
            {
                s_state.next_poll_ms = now_ms() + PACK_POLL_MS;
                if (packs::catalog_changed(s_state.catalog))
                {
                    reload_packs("a pack file changed");
                }
            }
            run_requests(capture.listener);
        }
    } // namespace

    void on_listener(void *system, const Attributes3d &listener) noexcept
    {
        {
            const std::lock_guard lock(s_capture_mutex);
            s_capture.system = system;
            s_capture.listener = listener;
            s_capture.listener_set = true;
        }
        // Core channels are spatialized against the core listener; give it the attributes the game just gave the
        // Studio listener, in the same space, so the framework's sounds sit where the game's sounds sit.
        const fmod::Api &api = fmod::api();
        void *core = nullptr;
        if (api.studio_get_core_system != nullptr && api.studio_get_core_system(system, &core) == fmod::OK)
        {
            (void)api.set_3d_listener(core, 0, &listener.position, &listener.velocity, &listener.forward, &listener.up);
        }
    }

    std::optional<WorldPosition> listener_position() noexcept
    {
        const std::lock_guard lock(s_capture_mutex);
        if (!s_capture.listener_set)
        {
            return std::nullopt;
        }
        // FMOD space swaps the world's y and z (see fmod::from_world).
        const fmod::Vector &at = s_capture.listener.position;
        return WorldPosition{at.x, at.z, at.y};
    }

    void on_system(void *system) noexcept
    {
        if (s_seen_system.load(std::memory_order_relaxed) == system)
        {
            return;
        }
        const std::lock_guard lock(s_capture_mutex);
        s_capture.system = system;
        s_seen_system.store(system, std::memory_order_relaxed);
    }

    bool start(std::filesystem::path mods_dir, std::filesystem::path defaults_file)
    {
        if (s_worker.has_value())
        {
            return true;
        }
        s_state = State{};
        s_state.mods_dir = std::move(mods_dir);
        s_state.defaults_file = std::move(defaults_file);
        load_packs();
        try
        {
            s_worker.emplace(
                "SoundFramework.engine",
                [](std::stop_token token)
                {
                    while (!token.stop_requested())
                    {
                        // A failed tick drops its own work only; the worker keeps serving later requests.
                        try
                        {
                            tick();
                        }
                        catch (const std::exception &error)
                        {
                            (void)DMK::log().try_log(DMK::LogLevel::Error, "Engine: tick failed ({})", error.what());
                        }
                        std::this_thread::sleep_for(TICK);
                    }
                }
            );
        }
        catch (const std::exception &error)
        {
            DMK::log().error("Engine: worker start failed ({})", error.what());
            return false;
        }
        return true;
    }

    void shutdown() noexcept
    {
        // The start hook is retired by now; the table goes first so nothing new is queued.
        replacements::clear();
        line_table::clear();
        // The worker owns the state while it runs, so it joins before anything is released.
        s_worker.reset();
        release_all();
        s_state = State{};
        {
            const std::lock_guard lock(s_request_mutex);
            s_requests.clear();
        }
        const std::lock_guard lock(s_capture_mutex);
        s_capture = Capture{};
        s_seen_system.store(nullptr, std::memory_order_relaxed);
    }

    void request_play(std::string name, std::optional<WorldPosition> at)
    {
        enqueue(
            Request{
                .kind = Kind::Play,
                .name = std::move(name),
                .at = at,
            }
        );
    }

    bool request_replacement(std::string key, void *event, std::optional<Attributes3d> attributes)
    {
        return enqueue(
            Request{
                .kind = Kind::Play,
                .name = std::move(key),
                .event = event,
                .event_attributes = attributes,
            }
        );
    }

    std::optional<std::uint32_t> length_ms(const std::string &game_path)
    {
        void *system = nullptr;
        {
            const std::lock_guard lock(s_capture_mutex);
            system = s_capture.system;
        }
        const fmod::Api &api = fmod::api();
        void *core = nullptr;
        if (system == nullptr || api.studio_get_core_system(system, &core) != fmod::OK || core == nullptr)
        {
            return std::nullopt;
        }
        void *sound = nullptr;
        const std::uint32_t mode = fmod::MODE_OPENONLY | fmod::MODE_ACCURATETIME | fmod::MODE_CREATESTREAM;
        if (api.create_sound(core, game_path.c_str(), mode, nullptr, &sound) != fmod::OK || sound == nullptr)
        {
            return std::nullopt;
        }
        std::uint32_t length = 0;
        const fmod::Result got = api.sound_get_length(sound, &length, fmod::TIMEUNIT_MS);
        (void)api.sound_release(sound);
        if (got != fmod::OK)
        {
            return std::nullopt;
        }
        return length;
    }

    DecodedAudio decode_mono(const std::string &game_path)
    {
        DecodedAudio decoded;
        void *system = nullptr;
        {
            const std::lock_guard lock(s_capture_mutex);
            system = s_capture.system;
        }
        const fmod::Api &api = fmod::api();
        void *core = nullptr;
        if (system == nullptr || api.studio_get_core_system(system, &core) != fmod::OK || core == nullptr)
        {
            return decoded;
        }
        void *sound = nullptr;
        if (api.create_sound(core, game_path.c_str(), fmod::MODE_OPENONLY, nullptr, &sound) != fmod::OK ||
            sound == nullptr)
        {
            return decoded;
        }
        int type = 0;
        int format = 0;
        int channels = 0;
        int bits = 0;
        float frequency = 0.0f;
        int priority = 0;
        if (api.sound_get_format(sound, &type, &format, &channels, &bits) != fmod::OK ||
            api.sound_get_defaults(sound, &frequency, &priority) != fmod::OK || channels <= 0 || frequency <= 0.0f ||
            (format != fmod::FORMAT_PCM16 && format != fmod::FORMAT_PCMFLOAT))
        {
            (void)api.sound_release(sound);
            return decoded;
        }
        const std::size_t sample_bytes = format == fmod::FORMAT_PCM16 ? 2 : 4;
        std::vector<std::uint8_t> buffer(16384 * sample_bytes * static_cast<std::size_t>(channels));
        // Long takes run to millions of samples: size the output once instead of growing it.
        std::uint32_t pcm_frames = 0;
        if (api.sound_get_length(sound, &pcm_frames, fmod::TIMEUNIT_PCM) == fmod::OK)
        {
            decoded.samples.reserve(pcm_frames);
        }
        for (;;)
        {
            std::uint32_t read = 0;
            const fmod::Result got =
                api.sound_read_data(sound, buffer.data(), static_cast<std::uint32_t>(buffer.size()), &read);
            const std::size_t frames = read / (sample_bytes * static_cast<std::size_t>(channels));
            for (std::size_t f = 0; f < frames; ++f)
            {
                float mixed = 0.0f;
                for (int c = 0; c < channels; ++c)
                {
                    const std::size_t at =
                        (f * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)) * sample_bytes;
                    if (format == fmod::FORMAT_PCM16)
                    {
                        std::int16_t pcm = 0;
                        std::memcpy(&pcm, buffer.data() + at, 2);
                        mixed += pcm / 32768.0f;
                    }
                    else
                    {
                        float value = 0.0f;
                        std::memcpy(&value, buffer.data() + at, 4);
                        mixed += value;
                    }
                }
                decoded.samples.push_back(mixed / static_cast<float>(channels));
            }
            if (read == 0 || got != fmod::OK)
            {
                break;
            }
        }
        (void)api.sound_release(sound);
        decoded.sample_rate = frequency;
        return decoded;
    }

    void request_stop(std::string name)
    {
        enqueue(
            Request{
                .kind = Kind::Stop,
                .name = std::move(name),
            }
        );
    }

    void request_stop_all()
    {
        enqueue(
            Request{
                .kind = Kind::StopAll,
            }
        );
    }

    void request_reload()
    {
        enqueue(
            Request{
                .kind = Kind::Reload,
            }
        );
    }

    void request_list()
    {
        enqueue(
            Request{
                .kind = Kind::List,
            }
        );
    }
} // namespace SoundFramework::audio
