/**
 * @file game/speech.cpp
 * @brief Actor speech through C_ActorSpeakMan, and subtitles through I_UISubtitles.
 *
 * Requests and subtitle strings are built with the game's own helpers (the request constructor, its string assignment
 * and destructor), exactly as the game's wh_snd_playfile command builds them, so every string the game keeps is a
 * string it allocated; nothing it holds points into this DLL.
 */

#include "game/speech.hpp"
#include "audio/engine.hpp"
#include "audio/line_table.hpp"
#include "audio/voice_analysis.hpp"
#include "constants.hpp"
#include "game/entity_tracker.hpp"
#include "game/game_clock.hpp"
#include "game/lip_drive.hpp"
#include "game/main_thread.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace SoundFramework::game::speech
{
    namespace
    {
        /**
         * @brief The speak manager's request (0x38 bytes): three strings (file, line text key, "100"), flag bytes at
         *        +0x18..+0x1B (+0x18: the text key and speaker name are localized in the chat bubble, +0x1A: the AI
         *        hears it, +0x1B: force), callback at +0x20, a byte at +0x28 and floats at +0x2C (timeout, seconds),
         *        +0x30 and +0x34. Built and released only through the game's constructor and destructor.
         */
        struct alignas(8) SpeakRequest
        {
            std::uint8_t bytes[0x38];
        };

        using GetActorFn = void *(__fastcall *)(void *actor_system, std::uint32_t id);
        using GetSpeakManFn = void *(__fastcall *)(void *actor);
        using GetIdFn = std::uint32_t(__fastcall *)(const void *entity);
        using SpeakFn = char(__fastcall *)(void *speak_man, const SpeakRequest *request);
        using StopFn = void(__fastcall *)(void *speak_man);
        using RequestCtorFn = SpeakRequest *(__fastcall *)(SpeakRequest * request);
        using RequestDtorFn = void(__fastcall *)(SpeakRequest *request);
        using AssignFn = void *(__fastcall *)(void *string, const char *text);
        using ShowSubtitleFn =
            void(__fastcall
                     *)(void *subtitles, const void *text, const void *speaker, bool flag, bool force, bool localize);
        using ClearSubtitlesFn = void(__fastcall *)(void *subtitles);

        constexpr std::uint64_t SUBTITLE_TAIL_MS = 300;

        /** @brief Per-line play history: the previous take and when the line was last said. */
        struct LineHistory
        {
            std::size_t last_take = SIZE_MAX;
            std::uint64_t last_said_ms = 0;
        };
        /// Main-thread only.
        std::unordered_map<std::string, LineHistory> s_line_history;
        /// Main-thread only: live preset overrides from sfw_mood_override.
        std::unordered_map<std::string, std::vector<face::PoseWeight>> s_mood_overrides;
        std::minstd_rand s_random{static_cast<std::minstd_rand::result_type>(GetTickCount64())};
        constexpr std::uint32_t SUBTITLE_FALLBACK_MS = 5000;

        Anchors s_anchors{};
        bool s_enabled = false;
        std::atomic<int> s_lip_offset_ms{0};
        /// Voice-clock milliseconds until the shown subtitle is cleared; negative when none is up.
        double s_subtitle_left_ms = -1.0;
        /// Set while say() calls Speak, so the Speak hook leaves the framework's own lines alone.
        bool s_in_say = false;
        /// The learned lip model, or null for the basic spectral mouth.
        std::shared_ptr<const audio::LipModel> s_lip_model;
        /// [LipSync] Mode: the learned model drives the mouth when one is loaded.
        std::atomic<bool> s_lip_advanced{true};

        /**
         * @brief Decodes each line's take and computes its mouth off the main thread: the learned model in parts, the
         *        basic mouth whole. on_frame() collects the results.
         */
        class LipWorker
        {
        public:
            struct Job
            {
                std::uint64_t ticket = 0;
                /// The take's file in the game file system (localization path plus .ogg).
                std::string voice_file;
                /// The learned model (advanced) or the spectral mouth (basic).
                bool learned = true;
                /// Positive [LipSync] OffsetMs: the mouth runs this far ahead of the sound.
                std::uint32_t lead_ms = 0;
                /// When the line started, which submit() stamps.
                std::chrono::steady_clock::time_point submitted;
            };

            /** @brief One part of a line's track: its rows from first_step on. */
            struct Result
            {
                std::uint64_t ticket = 0;
                std::size_t first_step = 0;
                /// Steps in the whole track, for the log.
                std::size_t total_steps = 0;
                audio::PoseTrack poses;
                /// Basic mode: the whole spectral mouth, instead of poses.
                std::vector<audio::MouthPose> track;
                /// From the line's start to this part being ready.
                double milliseconds = 0.0;
            };

            void start(std::shared_ptr<const audio::LipModel> model)
            {
                stop();
                m_model = std::move(model);
                m_worker.emplace("SoundFramework.lipsync", [this](std::stop_token stop) { loop(stop); });
            }

            void stop() noexcept
            {
                // The stop request wakes the wait in loop(), and the worker joins before its lines are dropped.
                m_worker.reset();
                m_lines.clear();
                const std::lock_guard lock(m_mutex);
                m_jobs.clear();
                m_done.clear();
                m_cancelled.clear();
            }

            void submit(Job job)
            {
                job.submitted = std::chrono::steady_clock::now();
                {
                    const std::lock_guard lock(m_mutex);
                    m_jobs.push_back(std::move(job));
                }
                m_wake.notify_one();
            }

            /** @brief Drops the line @p ticket: its queued job, and the parts still to compute. */
            void cancel(std::uint64_t ticket)
            {
                const std::lock_guard lock(m_mutex);
                std::erase_if(m_jobs, [&](const Job &job) { return job.ticket == ticket; });
                m_cancelled.push_back(ticket);
            }

            [[nodiscard]] std::vector<Result> take()
            {
                const std::lock_guard lock(m_mutex);
                return std::exchange(m_done, {});
            }

        private:
            /** @brief A line being worked on: its features once prepared, and the parts still to compute. */
            struct Line
            {
                Job job;
                std::vector<float> features;
                std::size_t frames = 0;
                std::size_t lead = 0;
                std::vector<audio::StepRange> parts;
                std::size_t next = 0;
                bool prepared = false;

                /** @brief When playback reaches the line's next uncomputed step (its start, before the features). */
                [[nodiscard]] std::chrono::steady_clock::time_point due() const noexcept
                {
                    const std::size_t step = prepared ? parts[next].begin - lead : 0;
                    return job.submitted + std::chrono::milliseconds(step * audio::lip_features::STEP_MS);
                }
            };

            /**
             * @brief Waits for work, then moves the new jobs into the lines and drops the cancelled lines.
             * @return False once a stop is requested.
             */
            [[nodiscard]] bool collect(const std::stop_token &stop)
            {
                std::unique_lock lock(m_mutex);
                if (!m_wake.wait(lock, stop, [this] { return !m_jobs.empty() || !m_lines.empty(); }))
                {
                    return false;
                }
                for (Job &job : m_jobs)
                {
                    m_lines.push_back(
                        Line{
                            .job = std::move(job),
                        }
                    );
                }
                m_jobs.clear();
                for (const std::uint64_t ticket : m_cancelled)
                {
                    std::erase_if(m_lines, [&](const Line &line) { return line.job.ticket == ticket; });
                }
                m_cancelled.clear();
                return true;
            }

            void loop(const std::stop_token &stop) noexcept
            {
                while (!stop.stop_requested())
                {
                    try
                    {
                        if (!collect(stop))
                        {
                            return;
                        }
                    }
                    catch (...)
                    {
                        (void)DMK::log().log_noexcept(
                            DMK::LogLevel::Error,
                            "LipSync: a queued line was lost; its mouth stays still"
                        );
                        continue;
                    }
                    if (m_lines.empty())
                    {
                        continue;
                    }
                    // The part playback reaches first goes next, so a new line's opening part never waits behind a
                    // long line's later parts.
                    const auto line = std::ranges::min_element(m_lines, {}, &Line::due);
                    bool more = false;
                    try
                    {
                        more = advance(*line);
                    }
                    catch (...)
                    {
                        // A failed line is dropped, so it cannot fail again on every pass.
                        (void)DMK::log().try_log(
                            DMK::LogLevel::Error,
                            "LipSync: the mouth of {} failed; it stays still",
                            line->job.voice_file
                        );
                    }
                    if (!more)
                    {
                        m_lines.erase(line);
                    }
                }
            }

            /**
             * @brief Decodes @p line and prepares its features (or computes its whole basic mouth), or computes its
             *        next part; each result goes to on_frame().
             * @return True while the line has parts left.
             */
            bool advance(Line &line)
            {
                constexpr std::uint32_t step_ms = audio::lip_features::STEP_MS;
                if (!line.prepared)
                {
                    // Decoding a long take takes a while, so it happens here instead of when the line starts.
                    const audio::DecodedAudio decoded = audio::decode_mono(line.job.voice_file);
                    if (decoded.samples.empty() || decoded.sample_rate <= 0.0f)
                    {
                        DMK::log().warning("Speech: cannot decode {}; the mouth stays still", line.job.voice_file);
                        return false;
                    }
                    if (!line.job.learned)
                    {
                        const std::vector<audio::VoiceFrame> frames =
                            audio::analyze_voice(decoded.samples, decoded.sample_rate, step_ms);
                        Result result{
                            .ticket = line.job.ticket,
                            .track = audio::mouth_track(frames, step_ms, line.job.lead_ms),
                        };
                        result.total_steps = result.track.size();
                        publish(std::move(result), line.job);
                        return false;
                    }
                    // The features cover the whole recording, since every band is normalized over all of it.
                    line.features = audio::lip_features_of(decoded.samples, decoded.sample_rate);
                    line.frames = line.features.size() / audio::lip_features::MELS;
                    line.lead = std::min<std::size_t>(line.job.lead_ms / step_ms, line.frames);
                    line.parts = audio::lip_parts(line.lead, line.frames);
                    line.prepared = true;
                    return !line.parts.empty();
                }
                const audio::StepRange part = line.parts[line.next++];
                publish(
                    Result{
                        .ticket = line.job.ticket,
                        .first_step = part.begin - line.lead,
                        .total_steps = line.frames - line.lead,
                        .poses = m_model->run_range(line.features, line.frames, part.begin, part.end),
                    },
                    line.job
                );
                return line.next < line.parts.size();
            }

            /** @brief Stamps @p result with the time since @p job's line started and hands it to on_frame(). */
            void publish(Result result, const Job &job)
            {
                result.milliseconds =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - job.submitted).count();
                const std::lock_guard lock(m_mutex);
                m_done.push_back(std::move(result));
            }

            std::shared_ptr<const audio::LipModel> m_model;
            std::mutex m_mutex;
            std::condition_variable_any m_wake;
            std::deque<Job> m_jobs;
            std::vector<Result> m_done;
            std::vector<std::uint64_t> m_cancelled;
            /// Worker thread only.
            std::vector<Line> m_lines;
            std::optional<DMK::StoppableWorker> m_worker;
        };

        LipWorker s_lip_worker;

        /** @brief A line the framework started, watched until its speaker stops playing it. */
        struct Watched
        {
            std::uint64_t id = 0;
            /// The lip drive line, or 0 without one.
            std::uint64_t ticket = 0;
            std::uint32_t entity = 0;
            /// The line's request path, which the speak manager keeps while the line is its current one.
            std::string path;
            std::uint64_t started_ms = 0;
            std::uint32_t length_ms = 0;
            /// The speak manager's sound id once the line's sound started; 0 before.
            std::uint32_t sound = 0;
            /// The sound the speaker was playing when the line was asked for: the game plays the line after it.
            std::uint32_t queued_behind = 0;
            /// The subtitle shown when the line's sound starts (empty text for none).
            std::string subtitle_speaker;
            std::string subtitle_text;
            bool subtitle_is_key = false;
            bool subtitle = false;
        };

        /// Main-thread state.
        std::vector<Watched> s_watched;
        std::uint64_t s_next_watch = 1;
        /// The watched line whose subtitle is up, or 0.
        std::uint64_t s_subtitle_watch = 0;
        /// A line that is neither playing nor waiting to play is given up this long after it was asked for.
        constexpr std::uint64_t START_GRACE_MS = 2000;
        /// A line that stops this much before its length was cut short (and says so in the log).
        constexpr std::uint64_t CUT_SHORT_MS = 500;
        /// A save load seen off the main thread; the next frame stops the lines.
        std::atomic<bool> s_stop_pending{false};

        /** @brief Everything say() needs to speak a line, kept while the line waits for its speaker. */
        struct SayRequest
        {
            std::string entity_name;
            std::string voice_path;
            std::string speaker;
            std::string bubble;
            std::string subtitle;
            bool subtitle_is_key = false;
            std::vector<face::PoseWeight> mood;
            float mood_strength = 1.0f;
        };

        /** @brief A framework line waiting for its speaker to finish the line it is saying. */
        struct Waiting
        {
            std::uint32_t entity = 0;
            std::uint64_t asked_ms = 0;
            SayRequest say;
        };

        /// Main-thread state: at most one waiting line per speaker, the newest.
        std::vector<Waiting> s_waiting;
        /// A line still waiting this long after it was asked for is dropped.
        constexpr std::uint64_t SPEAKER_WAIT_LIMIT_MS = 10'000;

        /** @brief A request built by the game's constructor and released by its destructor. */
        class GameRequest
        {
        public:
            GameRequest() noexcept { reinterpret_cast<RequestCtorFn>(s_anchors.request_ctor)(&m_request); }
            ~GameRequest() { reinterpret_cast<RequestDtorFn>(s_anchors.request_dtor)(&m_request); }
            GameRequest(const GameRequest &) = delete;
            GameRequest &operator=(const GameRequest &) = delete;

            /** @brief Copies @p text into the string at @p offset (0x00, 0x08 or 0x10). */
            void assign(std::size_t offset, const std::string &text) noexcept
            {
                reinterpret_cast<AssignFn>(s_anchors.string_assign)(m_request.bytes + offset, text.c_str());
            }

            /** @brief The string at @p offset, as the game passes a `const string&`. */
            [[nodiscard]] const void *string_at(std::size_t offset) const noexcept { return m_request.bytes + offset; }

            template <typename T> void set(std::size_t offset, T value) noexcept
            {
                std::memcpy(m_request.bytes + offset, &value, sizeof(T));
            }

            [[nodiscard]] const SpeakRequest *get() const noexcept { return &m_request; }

        private:
            SpeakRequest m_request{};
        };

        [[nodiscard]] void *const *vtable_of(const void *object) noexcept
        {
            return *static_cast<void *const *const *>(object);
        }

        /** @brief The game context, or nullptr before the game created it. */
        [[nodiscard]] std::uint8_t *context() noexcept
        {
            return *reinterpret_cast<std::uint8_t *const *>(s_anchors.context_slot);
        }

        /** @brief The game's I_UISubtitles, checked against C_UISubtitles, or nullptr. */
        [[nodiscard]] void *subtitles(std::uint8_t *ctx) noexcept
        {
            if (ctx == nullptr)
            {
                return nullptr;
            }
            auto *const ui = *reinterpret_cast<std::uint8_t **>(ctx + constants::CONTEXT_UI);
            auto *const hud = ui == nullptr ? nullptr : *reinterpret_cast<std::uint8_t **>(ui + constants::UI_HUD);
            auto *const iface =
                hud == nullptr ? nullptr : *reinterpret_cast<std::uint8_t **>(hud + constants::HUD_SUBTITLES);
            if (iface == nullptr ||
                reinterpret_cast<std::uintptr_t>(vtable_of(iface - constants::SUBTITLES_INTERFACE_OFFSET)) !=
                    s_anchors.ui_subtitles_vtable)
            {
                return nullptr;
            }
            return iface;
        }

        /** @brief The speak manager of the actor whose entity id is @p id, or nullptr (logged unless @p quiet). */
        [[nodiscard]] void *speak_man_of(std::uint8_t *ctx, std::uint32_t id, bool quiet = false) noexcept
        {
            void *const actor_system = *reinterpret_cast<void **>(ctx + constants::CONTEXT_ACTOR_SYSTEM);
            if (actor_system == nullptr ||
                reinterpret_cast<std::uintptr_t>(vtable_of(actor_system)) != s_anchors.actor_system_vtable)
            {
                if (!quiet)
                {
                    (void)DMK::log().log_noexcept(
                        DMK::LogLevel::Error,
                        "Speech: the game context holds no CActorSystem"
                    );
                }
                return nullptr;
            }
            void *const actor = reinterpret_cast<GetActorFn>(vtable_of(
                actor_system
            )[constants::ACTOR_SYSTEM_GET_ACTOR_SLOT])(actor_system, id);
            if (actor == nullptr)
            {
                if (!quiet)
                {
                    (void)DMK::log().try_log(DMK::LogLevel::Warning, "Speech: entity {:#x} is not an actor", id);
                }
                return nullptr;
            }
            const auto getter = reinterpret_cast<std::uintptr_t>(vtable_of(actor)[constants::ACTOR_GET_SPEAK_MAN_SLOT]);
            if (getter != s_anchors.get_speak_man)
            {
                if (!quiet)
                {
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Warning,
                        "Speech: actor {:#x} has an unexpected speak-manager getter",
                        id
                    );
                }
                return nullptr;
            }
            return reinterpret_cast<GetSpeakManFn>(getter)(actor);
        }

        void show_subtitle(
            std::uint8_t *ctx,
            const std::string &speaker,
            const std::string &text,
            std::uint32_t ms,
            bool localize
        ) noexcept;

        /** @brief @p speak_man's current line path (empty between lines). */
        [[nodiscard]] std::string_view current_line(const std::uint8_t *speak_man) noexcept
        {
            const char *const text = *reinterpret_cast<const char *const *>(speak_man + constants::SPEAK_MAN_LINE_PATH);
            if (text == nullptr)
            {
                return {};
            }
            const std::int32_t length = *reinterpret_cast<const std::int32_t *>(text - 8);
            return length > 0 ? std::string_view(text, static_cast<std::size_t>(length)) : std::string_view{};
        }

        /** @brief The sound @p speak_man is playing, or 0. */
        [[nodiscard]] std::uint32_t playing_sound(const std::uint8_t *speak_man) noexcept
        {
            std::uint32_t sound = 0;
            std::memcpy(&sound, speak_man + constants::SPEAK_MAN_SOUND_ID, sizeof(sound));
            return sound;
        }

        /** @brief Stops what @p line drives because its sound has ended: the mouth, the mood, and a cut-short subtitle.
         */
        void end_watched(const Watched &line, std::uint64_t now)
        {
            if (line.ticket != 0)
            {
                lip_drive::end_line(line.ticket);
                s_lip_worker.cancel(line.ticket);
            }
            const std::uint64_t played = now - line.started_ms;
            if (line.sound == 0 || played + CUT_SHORT_MS < line.length_ms)
            {
                if (line.subtitle && s_subtitle_watch == line.id && s_subtitle_left_ms >= 0.0)
                {
                    s_subtitle_left_ms = 0.0;
                }
                DMK::log().info(
                    "Speech: {} {} after {} of {} ms; its mouth and subtitle stop with it",
                    line.path,
                    line.sound == 0 ? "never started" : "was cut short",
                    played,
                    line.length_ms
                );
            }
        }

        /**
         * @brief @p line's sound @p sound has just started: the face's clock starts now, and the subtitle (when it is
         *        this line's) is timed from now too.
         */
        void started(std::uint8_t *ctx, Watched &line, std::uint32_t sound, std::uint64_t now)
        {
            DMK::log().info("Speech: {} started playing {} ms after it was asked", line.path, now - line.started_ms);
            line.sound = sound;
            line.started_ms = now;
            if (line.ticket != 0)
            {
                lip_drive::sound_started(line.ticket);
            }
            if (line.subtitle)
            {
                show_subtitle(ctx, line.subtitle_speaker, line.subtitle_text, line.length_ms, line.subtitle_is_key);
                s_subtitle_watch = line.id;
                DMK::log().info("Speech: subtitle for {} ms", line.length_ms);
            }
        }

        /** @brief Ends each watched line whose speaker no longer plays it: finished, stopped, replaced or never
         * started. */
        void watch_lines()
        {
            if (s_watched.empty())
            {
                return;
            }
            std::uint8_t *const ctx = context();
            if (ctx == nullptr)
            {
                return;
            }
            const std::uint64_t now = GetTickCount64();
            std::erase_if(
                s_watched,
                [&](Watched &line)
                {
                    const auto *const speak_man =
                        static_cast<const std::uint8_t *>(speak_man_of(ctx, line.entity, true));
                    const std::uint32_t sound = speak_man != nullptr ? playing_sound(speak_man) : 0;
                    if (line.sound != 0)
                    {
                        // Playing: alive exactly as long as the speaker plays its sound. A line asked for meanwhile
                        // (one of the game's own, or another of ours) waits for it to finish.
                        if (sound == line.sound)
                        {
                            return false;
                        }
                        end_watched(line, now);
                        return true;
                    }
                    const bool pending = speak_man != nullptr && speak_man[constants::SPEAK_MAN_START_PENDING] != 0;
                    const std::string_view current =
                        speak_man != nullptr ? current_line(speak_man) : std::string_view{};
                    if (!current.empty() && current != line.path)
                    {
                        // A newer line took its place before it started.
                        end_watched(line, now);
                        return true;
                    }
                    if (sound != 0 && sound != line.queued_behind && !pending)
                    {
                        // The speaker plays one line at a time, so the first sound to start after the one it waited
                        // behind is this line's.
                        started(ctx, line, sound, now);
                        return false;
                    }
                    // Still waiting: behind the sound that was playing, while its play request is pending, or while
                    // it is the speaker's current line (its lip-sync animation may still be streaming in).
                    const bool waiting = pending || current == line.path || (sound != 0 && sound == line.queued_behind);
                    if (waiting || now - line.started_ms < START_GRACE_MS)
                    {
                        return false;
                    }
                    end_watched(line, now);
                    return true;
                }
            );
        }

        /**
         * @brief Stops every framework line now. The speaker's own stop ends the sound (and the game's lip sync) while
         *        the speaker still holds the line, then the line's face and subtitle end.
         */
        void stop_lines(std::string_view why)
        {
            if (!s_waiting.empty())
            {
                DMK::log().info("Speech: {} dropped {} waiting line(s)", why, s_waiting.size());
                s_waiting.clear();
            }
            if (s_watched.empty())
            {
                return;
            }
            std::uint8_t *const ctx = context();
            std::size_t sounds = 0;
            for (const Watched &line : s_watched)
            {
                auto *const speak_man =
                    ctx == nullptr ? nullptr : static_cast<std::uint8_t *>(speak_man_of(ctx, line.entity, true));
                // Playing: the speaker still plays the line's sound. Waiting: the line is still the speaker's current
                // one. Anything else on the speaker is not the framework's to stop.
                const bool holds = speak_man != nullptr && (line.sound != 0 ? playing_sound(speak_man) == line.sound
                                                                            : current_line(speak_man) == line.path);
                if (holds)
                {
                    reinterpret_cast<StopFn>(s_anchors.speak_stop)(speak_man);
                    ++sounds;
                }
                if (line.ticket != 0)
                {
                    lip_drive::end_line(line.ticket);
                    s_lip_worker.cancel(line.ticket);
                }
            }
            if (s_subtitle_watch != 0 && s_subtitle_left_ms >= 0.0)
            {
                s_subtitle_left_ms = 0.0;
            }
            s_subtitle_watch = 0;
            DMK::log().info("Speech: {} stopped {} line(s), {} with a sound playing", why, s_watched.size(), sounds);
            s_watched.clear();
        }

        void show_subtitle(
            std::uint8_t *ctx,
            const std::string &speaker,
            const std::string &text,
            std::uint32_t ms,
            bool localize
        ) noexcept
        {
            void *const iface = subtitles(ctx);
            if (iface == nullptr)
            {
                (void)DMK::log().log_noexcept(DMK::LogLevel::Warning, "Speech: subtitles unavailable");
                return;
            }
            // A temporary request lends its game-built strings: field 0 holds the text, field 8 the speaker.
            GameRequest strings;
            strings.assign(0x00, text);
            strings.assign(0x08, speaker);
            reinterpret_cast<ShowSubtitleFn>(vtable_of(iface)[constants::SUBTITLES_SHOW_SLOT])(
                iface,
                strings.string_at(0x00),
                strings.string_at(0x08),
                false,
                false,
                localize
            );
            s_subtitle_left_ms = static_cast<double>(ms + SUBTITLE_TAIL_MS);
        }

        /** @brief The entity id of the actor that owns @p speak_man, or 0 when the chain does not check out. */
        [[nodiscard]] std::uint32_t entity_of(const void *speak_man) noexcept
        {
            const void *const actor = *reinterpret_cast<const void *const *>(
                static_cast<const std::uint8_t *>(speak_man) + constants::SPEAK_MAN_ACTOR
            );
            if (actor == nullptr ||
                reinterpret_cast<std::uintptr_t>(vtable_of(actor)[constants::ACTOR_GET_SPEAK_MAN_SLOT]) !=
                    s_anchors.get_speak_man)
            {
                return 0;
            }
            const void *const entity = *reinterpret_cast<const void *const *>(
                static_cast<const std::uint8_t *>(actor) + constants::ACTOR_ENTITY
            );
            if (entity == nullptr || reinterpret_cast<std::uintptr_t>(vtable_of(entity)) != s_anchors.entity_vtable)
            {
                return 0;
            }
            return reinterpret_cast<GetIdFn>(vtable_of(entity)[constants::ENTITY_GET_ID_SLOT])(entity);
        }

        /**
         * @brief What follows a spoken line: the face performance (voice-driven mouth when the game found no lip-sync
         *        animation, plus the mood) and the subtitle.
         */
        void follow_up(
            std::uint8_t *ctx,
            std::uint32_t id,
            const void *speak_man,
            const std::string &voice_file,
            std::optional<std::uint32_t> length,
            const std::string &speaker,
            const std::string &subtitle,
            bool subtitle_is_key,
            const std::vector<face::PoseWeight> &mood,
            float mood_strength
        )
        {
            // The game animates this line's mouth itself when Speak found a lip-sync animation for it.
            const bool lip_sync = static_cast<const std::uint8_t *>(speak_man)[constants::SPEAK_MAN_HAS_LIP_SYNC] != 0;
            std::uint64_t ticket = 0;
            if (id != 0 && lip_drive::active() && (!lip_sync || !mood.empty()))
            {
                constexpr std::uint32_t step_ms = 20;
                const int offset = s_lip_offset_ms.load(std::memory_order_relaxed);
                lip_drive::Performance performance{
                    .step_ms = step_ms,
                    .delay_ms = static_cast<std::uint32_t>(std::max(-offset, 0)),
                    .duration_ms = length.value_or(0),
                    .mood_strength = mood_strength,
                };
                // No lip-sync animation for this line: the mouth comes from the voice itself. The worker decodes the
                // take and computes it (the learned model, or the basic loudness-and-spectrum mouth); it joins the
                // running line a frame or two later.
                const bool learned = s_lip_model != nullptr && s_lip_advanced.load(std::memory_order_relaxed);
                performance.mouth_pending = !lip_sync && length.has_value();
                for (const face::PoseWeight &pose : mood)
                {
                    // The game's own lip sync owns the mouth; a mood then only touches the upper face.
                    if (!lip_sync || face::region_of(pose.pose) == face::Region::Upper)
                    {
                        performance.mood.push_back(pose);
                    }
                }
                DMK::log().info(
                    "Speech: face performance, {} ms, {} mouth, {} mood pose(s) x {:.2f}, offset {} ms",
                    performance.duration_ms,
                    lip_sync                     ? "the game's"
                    : !performance.mouth_pending ? "no"
                    : learned                    ? "learned"
                                                 : "basic",
                    performance.mood.size(),
                    mood_strength,
                    offset
                );
                const bool pending = performance.mouth_pending;
                ticket = lip_drive::perform(id, std::move(performance));
                if (pending && ticket != 0)
                {
                    s_lip_worker.submit(
                        LipWorker::Job{
                            .ticket = ticket,
                            .voice_file = voice_file,
                            .learned = learned,
                            .lead_ms = static_cast<std::uint32_t>(std::max(offset, 0)),
                        }
                    );
                }
            }
            const bool watch = id != 0 && (ticket != 0 || !subtitle.empty());
            if (!watch)
            {
                if (!subtitle.empty())
                {
                    show_subtitle(ctx, speaker, subtitle, length.value_or(SUBTITLE_FALLBACK_MS), subtitle_is_key);
                    DMK::log().info("Speech: subtitle for {} ms", length.value_or(SUBTITLE_FALLBACK_MS));
                }
                return;
            }
            // Watched until the speaker stops playing it: the face and the subtitle start with its sound, which may
            // wait behind a line the speaker is still saying, and stop with it.
            s_watched.push_back(
                Watched{
                    .id = s_next_watch++,
                    .ticket = ticket,
                    .entity = id,
                    .path = voice_file.ends_with(".ogg") ? voice_file.substr(0, voice_file.size() - 4) : voice_file,
                    .started_ms = GetTickCount64(),
                    .length_ms = length.value_or(SUBTITLE_FALLBACK_MS),
                    .queued_behind = playing_sound(static_cast<const std::uint8_t *>(speak_man)),
                    .subtitle_speaker = speaker,
                    .subtitle_text = subtitle,
                    .subtitle_is_key = subtitle_is_key,
                    .subtitle = !subtitle.empty(),
                }
            );
        }

        /** @brief A take of @p line (never the previous one when there is a choice); nothing while it cools down. */
        [[nodiscard]] std::optional<std::size_t>
        pick_take(const packs::LineDef &line, const LineHistory &history, std::uint64_t now)
        {
            if (line.cooldown > 0.0f && history.last_said_ms != 0 &&
                now - history.last_said_ms < static_cast<std::uint64_t>(line.cooldown * 1000.0f))
            {
                DMK::log().debug("Speech: '{}' is cooling down", line.name);
                return std::nullopt;
            }
            const std::size_t count = line.voices.size();
            std::size_t take = count <= 1 ? 0 : std::uniform_int_distribution<std::size_t>(0, count - 1)(s_random);
            if (count > 1 && take == history.last_take)
            {
                take = (take + 1 + std::uniform_int_distribution<std::size_t>(0, count - 2)(s_random)) % count;
            }
            return take;
        }

        /**
         * @brief True while @p speak_man has a line: its sound is playing or about to start, or the line is still its
         *        current one (for example while its lip-sync animation streams in).
         */
        [[nodiscard]] bool speaker_busy(const std::uint8_t *speak_man) noexcept
        {
            return playing_sound(speak_man) != 0 || speak_man[constants::SPEAK_MAN_START_PENDING] != 0 ||
                   !current_line(speak_man).empty();
        }

        /** @brief Speaks @p say through @p speak_man now, then starts its face, subtitle and line watch. */
        void speak_now(std::uint8_t *ctx, std::uint32_t id, std::uint8_t *speak_man, const SayRequest &say)
        {
            // The line's length times the subtitle and the mood. It comes from the file's header, so nothing is
            // decoded here; the lip sync worker decodes the take for the mouth.
            const std::string voice_file = say.voice_path + ".ogg";
            const bool needs_audio = !say.subtitle.empty() || lip_drive::active();
            const std::optional<std::uint32_t> length = needs_audio ? audio::length_ms(voice_file) : std::nullopt;
            if (needs_audio && !length.has_value())
            {
                DMK::log().warning("Speech: cannot open {}; timing falls back to defaults", voice_file);
            }
            // The same request the game's wh_snd_playfile builds.
            GameRequest request;
            request.set<std::uint64_t>(0x20, 0);
            request.assign(0x00, say.voice_path);
            // The line's text key: the speak manager shows it in a floating chat bubble while the line plays.
            request.assign(0x08, say.bubble);
            request.set<std::uint16_t>(0x18, 0x100);
            // +0x18: the bubble's text and the speaker's name are localization keys (the bubble UI translates both).
            request.set<std::uint8_t>(0x18, say.bubble.empty() ? 0 : 1);
            request.set<std::uint8_t>(0x1A, 1);
            request.set<std::uint8_t>(0x28, 0);
            request.set<float>(0x2C, 5.0f);
            request.set<float>(0x30, 5.0f);
            request.assign(0x10, "100");
            s_in_say = true;
            (void)reinterpret_cast<SpeakFn>(s_anchors.speak)(speak_man, request.get());
            s_in_say = false;
            const bool lip_sync = speak_man[constants::SPEAK_MAN_HAS_LIP_SYNC] != 0;
            DMK::log().info(
                "Speech: '{}' (id {:#x}) speaks {}{}",
                say.entity_name,
                id,
                say.voice_path,
                lip_sync ? " with lip sync" : ""
            );
            follow_up(
                ctx,
                id,
                speak_man,
                voice_file,
                length,
                say.speaker,
                say.subtitle,
                say.subtitle_is_key,
                say.mood,
                say.mood_strength
            );
        }

        /** @brief Speaks each waiting line whose speaker is free now; drops one that waited too long. */
        void serve_waiting()
        {
            if (s_waiting.empty())
            {
                return;
            }
            std::uint8_t *const ctx = context();
            if (ctx == nullptr)
            {
                return;
            }
            const std::uint64_t now = GetTickCount64();
            std::erase_if(
                s_waiting,
                [&](const Waiting &waiting)
                {
                    auto *const speak_man = static_cast<std::uint8_t *>(speak_man_of(ctx, waiting.entity, true));
                    if (speak_man == nullptr)
                    {
                        DMK::log().warning(
                            "Speech: '{}' is gone; {} is dropped",
                            waiting.say.entity_name,
                            waiting.say.voice_path
                        );
                        return true;
                    }
                    if (!speaker_busy(speak_man))
                    {
                        speak_now(ctx, waiting.entity, speak_man, waiting.say);
                        return true;
                    }
                    if (now - waiting.asked_ms < SPEAKER_WAIT_LIMIT_MS)
                    {
                        return false;
                    }
                    DMK::log().warning(
                        "Speech: '{}' was still speaking after {} s; {} is dropped",
                        waiting.say.entity_name,
                        SPEAKER_WAIT_LIMIT_MS / 1000,
                        waiting.say.voice_path
                    );
                    return true;
                }
            );
        }

        [[nodiscard]] std::string text_key_of(const packs::LineDef &line, std::size_t take)
        {
            return line.text_keys.empty() ? std::string{} : line.text_keys[line.text_keys.size() == 1 ? 0 : take];
        }
    } // namespace

    void configure(const Anchors &anchors) noexcept
    {
        s_anchors = anchors;
        s_enabled = anchors.speech;
    }

    bool
    say(const std::string &entity_name,
        const std::string &voice_path,
        const std::string &speaker,
        const std::string &bubble,
        const std::string &subtitle,
        bool subtitle_is_key,
        const std::vector<face::PoseWeight> &mood,
        float mood_strength)
    {
        if (!s_enabled)
        {
            DMK::log().error("Speech: the speech anchors did not resolve; voice lines are off");
            return false;
        }
        std::uint8_t *const ctx = context();
        if (ctx == nullptr)
        {
            DMK::log().warning("Speech: no game context yet");
            return false;
        }
        const std::uint32_t id = entities::find_id(entity_name);
        if (id == 0)
        {
            DMK::log().warning("Speech: no entity named '{}'", entity_name);
            return false;
        }
        auto *const speak_man = static_cast<std::uint8_t *>(speak_man_of(ctx, id));
        if (speak_man == nullptr)
        {
            return false;
        }
        SayRequest request{
            .entity_name = entity_name,
            .voice_path = voice_path,
            .speaker = speaker,
            .bubble = bubble,
            .subtitle = subtitle,
            .subtitle_is_key = subtitle_is_key,
            .mood = mood,
            .mood_strength = mood_strength,
        };
        if (speaker_busy(speak_man))
        {
            // Speak cuts a busy speaker's line off without notifying the line's listener (speak manager +0x68), so
            // that listener never hides its subtitle. The game itself waits for a free speaker, so the line waits
            // too.
            std::erase_if(s_waiting, [&](const Waiting &waiting) { return waiting.entity == id; });
            s_waiting.push_back(
                Waiting{
                    .entity = id,
                    .asked_ms = GetTickCount64(),
                    .say = std::move(request),
                }
            );
            DMK::log().info("Speech: '{}' is speaking; {} waits for the line to end", entity_name, voice_path);
            return true;
        }
        speak_now(ctx, id, speak_man, request);
        return true;
    }

    bool say_line(const std::string &entity_name, const std::string &line_name)
    {
        const std::shared_ptr<const audio::line_table::Lines> lines = audio::line_table::current();
        const std::string key = packs::sound_key(line_name);
        const auto found = lines->find(key);
        if (found == lines->end())
        {
            DMK::log().warning("Speech: no voice line named '{}' (sfw_list writes the catalog to the log)", line_name);
            return false;
        }
        const packs::LineDef &line = found->second;
        LineHistory &history = s_line_history[key];
        const std::uint64_t now = GetTickCount64();
        const std::optional<std::size_t> picked = pick_take(line, history, now);
        if (!picked.has_value())
        {
            return false;
        }
        const std::size_t take = *picked;
        const std::string text_key = text_key_of(line, take);
        const bool bubble = line.display == packs::Display::Bubble || line.display == packs::Display::Both;
        const bool subtitle = line.display == packs::Display::Subtitle || line.display == packs::Display::Both;
        if (!say(
                entity_name,
                line.voices[take],
                line.speaker,
                bubble ? text_key : std::string{},
                subtitle ? text_key : std::string{},
                true,
                line.mood,
                line.mood_strength
            ))
        {
            return false;
        }
        history.last_take = take;
        history.last_said_ms = now;
        return true;
    }

    std::optional<packs::MoodDef> find_mood(std::string_view name)
    {
        const std::string key = packs::sound_key(name);
        const std::shared_ptr<const audio::line_table::Moods> moods = audio::line_table::moods();
        const auto defined = moods->find(key);
        std::optional<packs::MoodDef> mood;
        if (defined != moods->end())
        {
            mood = defined->second;
        }
        else if (key == face::NEUTRAL)
        {
            mood = packs::MoodDef{
                .name = key,
            };
        }
        // An override replaces the poses only; the mood keeps its MaxStrength.
        const auto overridden = s_mood_overrides.find(key);
        if (overridden != s_mood_overrides.end())
        {
            if (!mood.has_value())
            {
                mood = packs::MoodDef{
                    .name = key,
                };
            }
            mood->poses = overridden->second;
        }
        return mood;
    }

    void override_mood(std::string_view name, std::vector<face::PoseWeight> poses)
    {
        if (poses.empty())
        {
            s_mood_overrides.erase(packs::sound_key(name));
            return;
        }
        s_mood_overrides[packs::sound_key(name)] = std::move(poses);
    }

    std::optional<Swap> prepare_swap(std::string_view text_key)
    {
        if (!s_enabled || s_in_say || text_key.empty())
        {
            return std::nullopt;
        }
        std::string lowered(text_key);
        std::ranges::transform(
            lowered,
            lowered.begin(),
            [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
        );
        const std::optional<std::string> name = audio::line_table::voice_replacement(lowered);
        if (!name.has_value())
        {
            return std::nullopt;
        }
        const std::shared_ptr<const audio::line_table::Lines> lines = audio::line_table::current();
        const auto found = lines->find(*name);
        if (found == lines->end())
        {
            return std::nullopt;
        }
        const packs::LineDef &line = found->second;
        LineHistory &history = s_line_history[*name];
        const std::uint64_t now = GetTickCount64();
        const std::optional<std::size_t> take = pick_take(line, history, now);
        if (!take.has_value())
        {
            return std::nullopt;
        }
        Swap swap{
            .line = line.name,
            .voice = line.voices[*take],
            .text_key = text_key_of(line, *take),
            .bubble = line.display == packs::Display::Bubble || line.display == packs::Display::Both,
            .subtitle = line.display == packs::Display::Subtitle || line.display == packs::Display::Both,
            .speaker = line.speaker,
            .mood = line.mood,
            .mood_strength = line.mood_strength,
        };
        // The take's length from its header, as say() does; the lip sync worker decodes it for the mouth.
        if (swap.subtitle || lip_drive::active())
        {
            swap.length_ms = audio::length_ms(swap.voice + ".ogg");
            if (!swap.length_ms.has_value())
            {
                DMK::log().warning("Speech: cannot open {}.ogg; timing falls back to defaults", swap.voice);
            }
        }
        history.last_take = *take;
        history.last_said_ms = now;
        DMK::log()
            .info("Speech: game line '{}' plays as '{}' take {} ({})", text_key, line.name, *take + 1, swap.voice);
        return swap;
    }

    void yield_to_dialog(void *speak_man) noexcept
    {
        if (!s_anchors.dialog_handover || speak_man == nullptr)
        {
            return;
        }
        try
        {
            const std::uint32_t id = entity_of(speak_man);
            if (id == 0 || std::ranges::none_of(s_watched, [&](const Watched &line) { return line.entity == id; }))
            {
                return;
            }
            const std::uint64_t now = GetTickCount64();
            for (const Watched &line : s_watched)
            {
                if (line.entity != id)
                {
                    continue;
                }
                if (s_subtitle_watch == line.id)
                {
                    // The dialog has just shown its own subtitle over ours: never clear it.
                    s_subtitle_watch = 0;
                    s_subtitle_left_ms = -1.0;
                }
                if (line.ticket != 0)
                {
                    lip_drive::end_line(line.ticket);
                    s_lip_worker.cancel(line.ticket);
                }
                DMK::log().info("Speech: {} gives way to a dialog line after {} ms", line.path, now - line.started_ms);
            }
            std::erase_if(s_watched, [&](const Watched &line) { return line.entity == id; });
            reinterpret_cast<StopFn>(s_anchors.speak_stop)(speak_man);
        }
        catch (...)
        {
            (void)DMK::log().log_noexcept(DMK::LogLevel::Error, "Speech: the hand-over to a dialog line failed");
        }
    }

    void on_save_load() noexcept
    {
        if (!s_enabled)
        {
            return;
        }
        if (!main_thread::is_main_thread())
        {
            s_stop_pending.store(true, std::memory_order_release);
            return;
        }
        try
        {
            stop_lines("a save load");
        }
        catch (...)
        {
            (void)DMK::log().log_noexcept(DMK::LogLevel::Error, "Speech: the save load did not stop every line");
        }
    }

    void finish_swap(const void *speak_man, const Swap &swap)
    {
        std::uint8_t *const ctx = context();
        const std::uint32_t id = entity_of(speak_man);
        if (id == 0)
        {
            DMK::log().warning(
                "Speech: the speaker of '{}' has no entity; the face stays as the game leaves it",
                swap.line
            );
        }
        follow_up(
            ctx,
            id,
            speak_man,
            swap.voice + ".ogg",
            swap.length_ms,
            swap.speaker,
            swap.subtitle ? swap.text_key : std::string{},
            true,
            swap.mood,
            swap.mood_strength
        );
    }

    void set_lip_model(std::shared_ptr<const audio::LipModel> model) noexcept
    {
        s_lip_worker.stop();
        s_lip_model = std::move(model);
        try
        {
            s_lip_worker.start(s_lip_model);
        }
        catch (...)
        {
            (void)DMK::log().log_noexcept(
                DMK::LogLevel::Error,
                "LipSync: the lip sync worker failed to start; voice lines keep a still mouth"
            );
            s_lip_model.reset();
        }
    }

    void set_lip_advanced(bool advanced) noexcept
    {
        s_lip_advanced.store(advanced, std::memory_order_relaxed);
    }

    void shutdown() noexcept
    {
        s_lip_worker.stop();
        s_lip_model.reset();
        s_watched.clear();
        s_waiting.clear();
        s_subtitle_watch = 0;
    }

    void set_lip_offset_ms(int offset_ms) noexcept
    {
        s_lip_offset_ms.store(std::clamp(offset_ms, -500, 500), std::memory_order_relaxed);
    }

    void on_frame() noexcept
    {
        try
        {
            if (s_stop_pending.exchange(false, std::memory_order_acq_rel))
            {
                stop_lines("a save load");
            }
            watch_lines();
            serve_waiting();
        }
        catch (...)
        {
            // A throw inside erase_if leaves its vector in a valid but unspecified order, so neither list can be
            // trusted. Dropping both is the consistent state: the faces still end on their own clocks.
            s_watched.clear();
            s_waiting.clear();
            (void)DMK::log().log_noexcept(
                DMK::LogLevel::Error,
                "Speech: the line watch failed; current and waiting lines are no longer watched"
            );
        }
        try
        {
            for (LipWorker::Result &result : s_lip_worker.take())
            {
                if (!result.track.empty())
                {
                    DMK::log().info(
                        "Speech: basic mouth, {} step(s) ready {:.1f} ms after the line started",
                        result.track.size(),
                        result.milliseconds
                    );
                    lip_drive::attach_track(result.ticket, std::move(result.track));
                    continue;
                }
                DMK::log().info(
                    "Speech: lip model, steps {} to {} of {} x {} channel(s) ready {:.1f} ms after the line started",
                    result.first_step,
                    result.first_step + result.poses.frames(),
                    result.total_steps,
                    result.poses.channels,
                    result.milliseconds
                );
                lip_drive::attach_poses(result.ticket, result.first_step, std::move(result.poses));
            }
        }
        catch (...)
        {
            (void)DMK::log().log_noexcept(DMK::LogLevel::Error, "Speech: lip sync results were dropped this frame");
        }
        if (s_subtitle_left_ms < 0.0)
        {
            return;
        }
        s_subtitle_left_ms -= game_clock::frame_ms();
        if (s_subtitle_left_ms > 0.0)
        {
            return;
        }
        s_subtitle_left_ms = -1.0;
        std::uint8_t *const ctx = context();
        (void)DMK::log().log_noexcept(DMK::LogLevel::Info, "Speech: subtitle cleared");
        void *const iface = subtitles(ctx);
        if (iface != nullptr)
        {
            reinterpret_cast<ClearSubtitlesFn>(vtable_of(iface)[constants::SUBTITLES_CLEAR_SLOT])(iface);
        }
    }
} // namespace SoundFramework::game::speech
