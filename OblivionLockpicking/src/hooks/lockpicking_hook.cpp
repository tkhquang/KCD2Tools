/**
 * @file hooks/lockpicking_hook.cpp
 * @brief Runs the tumbler lock inside the vanilla lockpicking session and reports through the vanilla exits.
 *
 * The vanilla session keeps the enter and leave animations, the success commit (XP, stats, unlock, crime), the
 * pick-break roll, noise, barks and the failure animation. The detour changes only what the search-and-turn states
 * (3, 4, 5) see each frame:
 *
 *  1. It takes the frame's inputs (mouse deltas, right stick, the rotate action) out of the session before the original
 *     Update runs, so the vanilla cursor and the lock stay still. It puts the held values back after the original,
 *     because the action handlers write them only when the input changes.
 *  2. It turns them into the Oblivion controls and steps the tumbler lock. Mouse X or the bound stick moves the pick.
 *     An upward mouse move, the stick or a push key pushes, and a set key sets. The keys and the stick come from the
 *     engine's input events (controls/lock_controls.hpp), which also keep the bound keys from other game actions.
 *  3. A wrong set zeroes the pick durability before the original runs. UpdateLock's branch for a still lock then calls
 *     the vanilla Fail under the vanilla gate, so the break roll and every consequence are the game's own. The detour
 *     holds back the HUD text "Lockpicking attempt failed", because the attempt goes on. It then ends the failure on
 *     the same frame, the way the game ends it for an empty hand, so the 3.4 s failure animation never plays: it drops
 *     Henry's head and the camera with it. The pick break sounds at the lock, and the controls pause for
 *     FailLockoutSeconds, as Oblivion paused them for its pick break. FailLockoutSeconds=0 plays the game's failure.
 *  4. A solved lock arms the vanilla success hold (state 6, 0.5 s) after the original ran, with the same plain write
 *     UpdateLock uses. Update counts it down and commits the success as for a vanilla lock that turns open.
 *  5. Each tumbler event (push, top, fall, land, set) plays a sound from the mod's SoundFramework pack when
 *     SoundFramework is installed. Without it, a tumbler at the top plays the vanilla sweet-spot click. A tumbler
 *     in motion makes noise for nearby NPCs. The level comes from the game's own noise query, perks and buffs included,
 *     and goes out through the session's hearing emitter, as UpdateLock does for a lock that turns.
 *
 * Every function on the detour path is noexcept and logs only through Logger::try_log, because an exception that
 * escapes a detour terminates the game.
 */

#include "hooks/lockpicking_hook.hpp"
#include "config.hpp"
#include "constants.hpp"
#include "controls/lock_controls.hpp"
#include "game/lock_movie.hpp"
#include "game/lockpicking.hpp"
#include "sim/tumbler_sim.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <numbers>
#include <optional>
#include <string>

namespace OblivionLockpicking
{
    namespace
    {
        using constants::LockState;
        /// Update returns the byte of its last branch, and the minigame manager receives it unchanged.
        using UpdateFn = char(__fastcall *)(void *self, float dt);
        /// The HUD's lockpicking message handler, which shows the text of a message code.
        using HudMessageFn = void(__fastcall *)(void *hud, std::uint8_t code);

        UpdateFn s_update_original = nullptr;
        HudMessageFn s_hud_message_original = nullptr;
        /// Set only around the original Update on the frame of a wrong set, the frame its Fail sends the failure text.
        std::atomic<bool> s_hide_fail_message{false};

        /// Time constant of the mouse flick detector: upward travel older than this fades out.
        constexpr float FLICK_DECAY_SECONDS = 0.12f;
        /// The flick re-arms once its upward travel decays below this share of the threshold.
        constexpr float FLICK_REARM_SHARE = 0.3f;
        /// A flick must be this much more vertical than horizontal, so a sideways slide of the pick never pushes.
        constexpr float FLICK_VERTICAL_RATIO = 1.2f;
        /// Pad stick deflection that counts as a push up.
        constexpr float STICK_PUSH = 0.6f;
        /// A held move key steps the pick again after this delay.
        constexpr float KEY_REPEAT_DELAY = 0.35f;
        /// A held move key then steps the pick at this interval.
        constexpr float KEY_REPEAT_INTERVAL = 0.15f;
        /// Frames in a row the movie can fail to update before the lock goes back to the vanilla minigame.
        constexpr int MOVIE_FAILURE_LIMIT = 10;
        /**
         * @brief Times in one session the detour shows the element again after the game hid it.
         * @details Each show loads the movie and its textures again, so the lock goes back to the vanilla minigame
         *          past this limit instead of a reload loop.
         */
        constexpr int MOVIE_RESHOW_LIMIT = 3;
        /// How far the pick tip rises at most while it kicks a tumbler up, in 1080p pixels.
        constexpr float PICK_KICK = 14.0f;

        /** @brief The one player lock the detour drives. Only the detour touches it. */
        struct LockSession
        {
            /// The C_LockPicking under the detour, or nullptr.
            void *instance{nullptr};
            /// The lock entity of the session.
            std::uint32_t entity{0};
            /// True once the tumbler lock exists for this session.
            bool started{false};
            /**
             * @brief Picks the game counted broken when the last wrong set went to the vanilla failure.
             * @details The failure runs whether or not its break roll breaks the pick, so only a higher count means a
             *          break.
             */
            std::int32_t broken_before_fail{0};
            /// Seconds the controls stay paused after the last wrong set.
            float lockout{0.0f};
            /// True once the vanilla success hold is armed.
            bool committed{false};
            /// True once the lock left play for good (TransitioningOut or later), so no later state belongs to it.
            bool ended{false};
            /// True once the thread check decided where the cues go and whether the input events are read.
            bool thread_checked{false};
            /// True when the session updates on the game's main thread, the only thread for the console and input.
            bool main_thread{false};
            /// The mod's Scaleform element, which draws the lock.
            void *movie{nullptr};
            /// True when the movie is absent or cannot draw, so the rest of this session is the vanilla minigame.
            bool vanilla{false};
            /// True when the element was visible at the last check.
            bool movie_shown{false};
            /// True once the movie's begin() ran for this lock and this show of the element.
            bool movie_begun{false};
            /// Times the detour showed the element again after the game hid it.
            int movie_reshows{0};
            /// Frames in a row the movie failed to update.
            int movie_failures{0};
            /// True for a fancy lock, which the movie draws in its fancy style.
            bool fancy{false};
            /// True when the detour hid the content of the vanilla Flash element.
            bool view_hidden{false};
            /// The control hint row under the lock, with button icons.
            std::string hint_row;
            /// The bindings the hint row shows.
            controls::Bindings hinted_bindings;
            /// The device the hint row shows.
            controls::HintDevice hinted_device{controls::HintDevice::KeyboardMouse};
            /// True once the hint row matches the current bindings and device.
            bool hints_written{false};
            /// True once the movie has the current hint row.
            bool hints_sent{false};
            /// True when the stick was up on the previous frame.
            bool stick_was_up{false};
            /// The direction of the held move key: -1 left, 1 right, 0 none.
            int repeat_direction{0};
            /// Seconds to the next step of the held move key.
            float repeat_timer{0.0f};
            /// Upward mouse travel with exponential decay, in the game's lockpicking mouse units.
            float flick_up{0.0f};
            /// Sideways mouse travel with exponential decay, in the game's lockpicking mouse units.
            float flick_side{0.0f};
            /// True while a flick holds its push, until its upward travel decays.
            bool flick_latched{false};
            /// Tumblers this lock needs.
            int pins_to_set{0};
            /// Seconds since the last input log line.
            float log_timer{0.0f};
            /// Horizontal mouse travel since the last input log line.
            float logged_mouse_x{0.0f};
            /// Vertical mouse travel since the last input log line.
            float logged_mouse_y{0.0f};
            /// The tumbler lock.
            sim::TumblerSim lock;
        };

        LockSession s_lock;
        /// The detour's settings. Only the detour touches them.
        SettingsCache s_settings;

        /// Set tumblers that Oblivion's Master Security rank keeps after a broken pick.
        constexpr int MAX_KEPT_TUMBLERS = 4;
        /// The lock difficulty (0 to 1) each tumbler adds, so the game's five difficulty tiers map to 1 to 5 tumblers.
        constexpr float DIFFICULTY_PER_TUMBLER = 0.2f;
        /// The durability that a session whose maximum reads zero gets back after its wrong set.
        constexpr float FALLBACK_DURABILITY = 100.0f;

        /**
         * @brief Set tumblers kept after a wrong set at Thievery @p level, as Oblivion's Security ranks spread over the
         *        skill range: 0 below level 8, 1 from 8, 2 from 15, 3 from 23, 4 at 30.
         */
        [[nodiscard]] int kept_for_thievery(float level) noexcept
        {
            const float bounded = std::clamp(level, 0.0f, constants::SKILL_MAX_LEVEL);
            return static_cast<int>(bounded * static_cast<float>(MAX_KEPT_TUMBLERS) / constants::SKILL_MAX_LEVEL);
        }

        /** @brief Tumbler count for a lock: one per difficulty tier, and the lowest for a value that is not finite. */
        [[nodiscard]] int pins_for_difficulty(float difficulty) noexcept
        {
            const float bounded = std::isfinite(difficulty) ? std::clamp(difficulty, 0.0f, 1.0f) : 0.0f;
            return std::min(static_cast<int>(bounded / DIFFICULTY_PER_TUMBLER), sim::MAX_TUMBLERS - 1) + 1;
        }

        /** @brief Builds the tumbler lock from what vanilla Start computed for this lock. */
        void start_lock(LockSession &session, void *self, const Settings &settings) noexcept
        {
            session.entity = game::read<std::uint32_t>(self, constants::LP_LOCK_ENTITY_OFFSET);
            session.movie = game::find_lock_movie();
            if (session.movie == nullptr)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Warning,
                    "Lock: the lock movie is not loaded (is the mod's pak installed?), so this lock is vanilla"
                );
                session.vanilla = true;
                return;
            }
            const float difficulty = game::read<float>(self, constants::LP_LOCK_DIFFICULTY_OFFSET);
            const float tolerance = game::read<float>(self, constants::LP_TOLERANCE_OFFSET);

            sim::LockSetup setup;
            setup.pins_to_set = pins_for_difficulty(difficulty);
            // The sweet-spot size already carries Thievery, perks, buffs, drunkenness and the lock's difficulty. It is
            // the player's skill against this lock, the same input Oblivion's Security has. A size that is not finite
            // counts as no skill.
            const float relative =
                (tolerance - settings.tolerance_min) / (settings.tolerance_max - settings.tolerance_min);
            setup.skill = std::isfinite(relative) ? std::clamp(relative, 0.0f, 1.0f) : 0.0f;
            setup.time_scale = settings.time_scale;
            // The lock's entity id keeps the preset tumblers and the rolls stable across retries of the same lock.
            setup.seed = session.entity;
            const std::optional<float> thievery = game::thievery_level(game::player_soul(self));
            switch (settings.keep_set_tumblers_on_fail)
            {
            case KeepOnFail::None:
                setup.keep_on_fail = 0;
                break;
            case KeepOnFail::Thievery:
                // An unreadable level keeps every set tumbler, so a failed read never costs the player progress.
                setup.keep_on_fail = thievery.has_value() ? kept_for_thievery(*thievery) : sim::KEEP_ALL_ON_FAIL;
                break;
            case KeepOnFail::All:
                setup.keep_on_fail = sim::KEEP_ALL_ON_FAIL;
                break;
            }
            setup.set_pushes_resting = settings.set_pushes_resting;
            setup.one_at_a_time = settings.one_tumbler_at_a_time;
            setup.set_grace = settings.set_grace_seconds;
            setup.tuning.speed_no_skill = settings.speed_no_skill;
            setup.tuning.speed_full_skill = settings.speed_full_skill;
            session.lock.reset(setup);
            session.pins_to_set = setup.pins_to_set;
            session.started = true;
            session.fancy = game::read<std::uint8_t>(self, constants::LP_LOCK_FANCINESS_OFFSET) == 1;

            (void)DMK::log().try_log(
                DMK::LogLevel::Info,
                "Lock: entity {:#x}, difficulty {:.2f} -> {} tumbler(s), tolerance {:.3f} -> skill {:.2f}, "
                "thievery {:.0f}, a wrong set keeps up to {}",
                session.entity,
                difficulty,
                setup.pins_to_set,
                tolerance,
                setup.skill,
                thievery.value_or(-1.0f),
                setup.keep_on_fail == sim::KEEP_ALL_ON_FAIL ? setup.pins_to_set : setup.keep_on_fail
            );
        }

        /**
         * @brief Checks once, after the game window exists, whether the session updates on the window's thread.
         * @details That thread is the game's main thread, the only thread for the console (SoundFramework, the hint
         *          device) and the input system. The result decides where this session's cues go.
         */
        void check_thread(LockSession &session) noexcept
        {
            if (session.thread_checked)
            {
                return;
            }
            const std::uint32_t window_thread = game::main_thread_id();
            if (window_thread == 0)
            {
                return;
            }
            session.thread_checked = true;
            session.main_thread = GetCurrentThreadId() == window_thread;
            const bool framework = game::detect_sound_framework(session.main_thread);
            (void)DMK::log().try_log(
                DMK::LogLevel::Info,
                "Lock: sounds through {}",
                framework ? "SoundFramework" : "the vanilla click"
            );
            if (!session.main_thread)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Warning,
                    "Lock: the session does not update on the main thread, so only the mouse works"
                );
            }
        }

        /**
         * @brief Rewrites the hint row when the bindings or the device of the game's own hints change, and hands the
         *        row to the movie.
         */
        void sync_hints(LockSession &session, const Settings &settings) noexcept
        {
            const controls::HintDevice device = session.main_thread
                                                    ? controls::hint_device_from_code(game::hint_device_code())
                                                    : controls::HintDevice::KeyboardMouse;
            if (!session.hints_written || session.hinted_device != device ||
                !(session.hinted_bindings == settings.bindings))
            {
                try
                {
                    session.hint_row = controls::control_hint_row(settings.bindings, device);
                }
                catch (...)
                {
                    session.hint_row.clear();
                }
                session.hinted_device = device;
                session.hinted_bindings = settings.bindings;
                session.hints_written = true;
                session.hints_sent = false;
            }
            if (!session.hints_sent && session.movie != nullptr)
            {
                session.hints_sent = game::set_lock_movie_controls(session.movie, session.hint_row.c_str());
            }
        }

        /** @brief Sounds the step's tumbler events, in the order they happen within a frame. */
        void play_cues(void *self, const sim::StepResult &result) noexcept
        {
            if (result.landed)
            {
                game::play_cue(self, game::Cue::Land);
            }
            if (result.dropped && result.drop_speed == sim::Speed::Slow)
            {
                game::play_cue(self, game::Cue::Fall);
            }
            if (result.pushed)
            {
                game::play_cue(self, game::Cue::Push);
            }
            if (result.reached_top)
            {
                game::play_cue(self, result.top_speed == sim::Speed::Slow ? game::Cue::TopSlow : game::Cue::TopFast);
            }
            if (result.set)
            {
                game::play_cue(self, game::Cue::Set);
            }
        }

        /**
         * @brief Hides what the vanilla lock draws while @p hide asks, or shows it again.
         * @details The call hides the content of the vanilla movie, not its element. The game unloads a hidden element
         *          and shows it again, freshly loaded, for its fade-out when the player leaves the lock. The content
         *          is hidden on every frame, because the game reloads the movie after any unload.
         */
        void sync_vanilla_view(LockSession &session, void *self, bool hide) noexcept
        {
            if (hide)
            {
                game::show_movie_content(game::vanilla_view_element(self), false);
                session.view_hidden = true;
            }
            else if (session.view_hidden)
            {
                game::show_movie_content(game::vanilla_view_element(self), true);
                session.view_hidden = false;
            }
        }

        /**
         * @brief The lift of the pick tip this frame. The tip bumps up with a tumbler it kicks up and drops back while
         *        the tumbler flies on alone.
         */
        [[nodiscard]] float pick_kick(const sim::TumblerSim &lock) noexcept
        {
            const sim::PinView under = lock.pin(lock.selected());
            if (under.phase != sim::PinPhase::Rising)
            {
                return 0.0f;
            }
            return PICK_KICK * std::sin(std::numbers::pi_v<float> * std::clamp(under.height, 0.0f, 1.0f));
        }

        /** @brief This frame of the lock for the movie. */
        [[nodiscard]] game::MovieFrame
        movie_frame(const LockSession &session, bool breaking, const Settings &settings) noexcept
        {
            game::MovieFrame frame{
                .pick = session.lock.pick_position(),
                .kick = pick_kick(session.lock),
                .opening = session.lock.solved(),
                .breaking = breaking,
                .scale = settings.lock_scale,
                .fancy = session.fancy,
                .set_count = session.pins_to_set - session.lock.remaining(),
                .total = session.pins_to_set,
                .show_header = settings.show_header,
                .show_footer = settings.show_footer,
            };
            for (int i = 0; i < sim::MAX_TUMBLERS; ++i)
            {
                frame.heights[static_cast<std::size_t>(i)] = session.lock.pin(i).height;
            }
            return frame;
        }

        /**
         * @brief Keeps the session's movie shown or hidden as @p visible asks, checked against the element itself on
         *        every frame.
         * @details The game can hide the element while the lock is up and then unloads its movie. A cached flag cannot
         *          see that, and a call into a hidden element reloads its movie and textures on every call. Each
         *          show loads the movie once. Past MOVIE_RESHOW_LIMIT shows, the lock goes back to the vanilla
         *          minigame.
         */
        void sync_movie(LockSession &session, bool visible) noexcept
        {
            if (session.movie == nullptr)
            {
                return;
            }
            if (game::lock_movie_visible(session.movie) == visible)
            {
                session.movie_shown = visible;
                return;
            }
            if (visible && session.movie_shown)
            {
                if (++session.movie_reshows > MOVIE_RESHOW_LIMIT)
                {
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Warning,
                        "Lock: the game keeps the lock movie hidden, so the rest of this lock is vanilla"
                    );
                    session.movie = nullptr;
                    session.vanilla = true;
                    return;
                }
                (void)DMK::log().try_log(
                    DMK::LogLevel::Warning,
                    "Lock: the game hid the lock movie, shown again ({} of {})",
                    session.movie_reshows,
                    MOVIE_RESHOW_LIMIT
                );
                // A movie shown again starts over without its hint row.
                session.hints_sent = false;
            }
            game::show_lock_movie(session.movie, visible);
            session.movie_shown = visible;
            if (visible)
            {
                session.movie_begun = false;
            }
        }

        /**
         * @brief Shows the movie while the lock is up and hands it this frame. A movie that cannot draw hands the rest
         *        of the session back to the vanilla minigame.
         */
        void update_movie(LockSession &session, void *self, LockState state, const Settings &settings) noexcept
        {
            if (session.movie == nullptr)
            {
                return;
            }
            const bool active =
                session.started && (game::is_play_state(state) || state == LockState::UnlockingAnim ||
                                    state == LockState::UnlockingAnimEnd || state == LockState::IsFailing);
            if (active && settings.show_footer)
            {
                sync_hints(session, settings);
            }
            sync_movie(session, active);
            if (session.movie == nullptr || !active)
            {
                return;
            }
            if (!session.movie_begun)
            {
                game::begin_lock_movie(session.movie);
                session.movie_begun = true;
            }
            const bool breaking =
                (state == LockState::IsFailing || session.lockout > 0.0f) &&
                game::read<std::int32_t>(self, constants::LP_BROKEN_COUNT_OFFSET) > session.broken_before_fail;
            if (game::update_lock_movie(session.movie, movie_frame(session, breaking, settings)))
            {
                session.movie_failures = 0;
                return;
            }
            if (++session.movie_failures >= MOVIE_FAILURE_LIMIT)
            {
                // The element exists but its player never loaded the movie. The player gets a lock they can see.
                (void)DMK::log().try_log(
                    DMK::LogLevel::Warning,
                    "Lock: the game UI did not load the lock movie, so the rest of this lock is vanilla"
                );
                sync_movie(session, false);
                session.movie = nullptr;
                session.vanilla = true;
            }
        }

        /**
         * @brief Detects an upward mouse flick: enough recent upward travel, mostly vertical, once per flick.
         * @param up Upward mouse travel this frame, in the game's lockpicking mouse units.
         * @param side Sideways mouse travel this frame, in the same units.
         * @details The travel is raw, so MousePickSpeed scales only the pick slide and PushFlick alone sets the push.
         */
        [[nodiscard]] bool
        detect_flick(LockSession &session, float up, float side, float dt, const Settings &settings) noexcept
        {
            const float decay = std::exp(-dt / FLICK_DECAY_SECONDS);
            session.flick_up = up < 0.0f ? 0.0f : session.flick_up * decay + up;
            session.flick_side = session.flick_side * decay + std::fabs(side);
            if (session.flick_latched)
            {
                session.flick_latched = session.flick_up > settings.push_flick * FLICK_REARM_SHARE;
                return false;
            }
            if (session.flick_up >= settings.push_flick && session.flick_up > session.flick_side * FLICK_VERTICAL_RATIO)
            {
                session.flick_latched = true;
                return true;
            }
            return false;
        }

        /** @brief Tumblers the move keys step this frame: one per press, then one per repeat while a key stays down. */
        [[nodiscard]] int key_steps(LockSession &session, const controls::FrameInputs &pad, float dt) noexcept
        {
            const int pressed = (pad.move_right ? 1 : 0) - (pad.move_left ? 1 : 0);
            const int held = (pad.move_right_held ? 1 : 0) - (pad.move_left_held ? 1 : 0);
            if (pressed != 0)
            {
                session.repeat_direction = pressed;
                session.repeat_timer = KEY_REPEAT_DELAY;
                return pressed;
            }
            if (held == 0 || held != session.repeat_direction)
            {
                session.repeat_direction = 0;
                return 0;
            }
            session.repeat_timer -= dt;
            if (session.repeat_timer > 0.0f)
            {
                return 0;
            }
            session.repeat_timer += KEY_REPEAT_INTERVAL;
            return held;
        }

        /** @brief The pick movement that puts it @p steps tumblers from the tumbler it is nearest to. */
        [[nodiscard]] float step_delta(float pick, int steps) noexcept
        {
            if (steps == 0)
            {
                return 0.0f;
            }
            const float target = std::clamp(
                std::round(pick) + static_cast<float>(steps),
                0.0f,
                static_cast<float>(sim::MAX_TUMBLERS - 1)
            );
            return target - pick;
        }

        /** @brief The log word of @p speed. */
        [[nodiscard]] const char *speed_name(sim::Speed speed) noexcept
        {
            switch (speed)
            {
            case sim::Speed::Slow:
                return "slow";
            case sim::Speed::Fast:
                return "fast";
            case sim::Speed::Fastest:
                return "fastest";
            }
            return "fast";
        }

        /** @brief Logs the input keys and, once per second, the mouse and stick values, at the TRACE log level. */
        void log_inputs(
            LockSession &session,
            const controls::FrameInputs &pad,
            float mouse_x,
            float mouse_y,
            int steps,
            float dt
        ) noexcept
        {
            session.logged_mouse_x += mouse_x;
            session.logged_mouse_y += mouse_y;
            session.log_timer += dt;
            if (pad.push || pad.set || steps != 0)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Trace,
                    "Inputs: {} key",
                    pad.set    ? "set"
                    : pad.push ? "push"
                               : "move"
                );
            }
            if (session.log_timer >= 1.0f)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Trace,
                    "Inputs: mouse {:.3f},{:.3f}/s, pad stick {:.2f},{:.2f}",
                    session.logged_mouse_x / session.log_timer,
                    session.logged_mouse_y / session.log_timer,
                    pad.stick_x,
                    pad.stick_y
                );
                session.log_timer = 0.0f;
                session.logged_mouse_x = 0.0f;
                session.logged_mouse_y = 0.0f;
            }
        }

        /**
         * @brief Ends the game's failure on the frame of the wrong set, before its animation starts, and pauses the
         *        controls for FailLockoutSeconds in its place.
         * @details game::finish_failure states what the failure still runs.
         */
        void skip_failure_animation(LockSession &session, void *self, const Settings &settings) noexcept
        {
            if (!game::finish_failure(self))
            {
                return;
            }
            game::play_break_sound(self);
            session.lockout = settings.fail_lockout_seconds;
            (void)DMK::log().try_log(
                DMK::LogLevel::Debug,
                "Lock: failure ended before its animation (state {}), controls paused {:.2f} s",
                static_cast<int>(game::state(self)),
                session.lockout
            );
        }

        /**
         * @brief Steps the tumbler lock around one original Update of a search-and-turn state.
         * @return The original Update's result.
         */
        [[nodiscard]] char play_frame(LockSession &session, void *self, float dt, const Settings &settings) noexcept
        {
            // The mouse deltas are per frame, and vanilla zeroes them after the read, so they stay zero. The stick and
            // the rotate action are held values, restored after the original.
            const float mouse_x = game::read<float>(self, constants::LP_MOUSE_X_OFFSET);
            const float mouse_y = game::read<float>(self, constants::LP_MOUSE_Y_OFFSET);
            game::write<float>(self, constants::LP_MOUSE_X_OFFSET, 0.0f);
            game::write<float>(self, constants::LP_MOUSE_Y_OFFSET, 0.0f);
            const float stick_x = game::read<float>(self, constants::LP_STICK_X_OFFSET);
            const float stick_y = game::read<float>(self, constants::LP_STICK_Y_OFFSET);
            game::write<float>(self, constants::LP_STICK_X_OFFSET, 0.0f);
            game::write<float>(self, constants::LP_STICK_Y_OFFSET, 0.0f);
            const float rotate = game::read<float>(self, constants::LP_ROTATE_INPUT_OFFSET);
            game::write<float>(self, constants::LP_ROTATE_INPUT_OFFSET, 0.0f);

            const controls::FrameInputs pad =
                session.main_thread ? controls::take_frame(settings.bindings) : controls::FrameInputs{};
            const bool stick_up = pad.stick_y >= STICK_PUSH;
            const bool flick =
                settings.bindings.push_with_mouse_up && detect_flick(session, mouse_y, mouse_x, dt, settings);
            const int steps = key_steps(session, pad, dt);
            // The inputs of a paused frame are read and dropped. A move key pressed in the pause needs a new press
            // after it, so its held repeat stops here.
            const bool paused = session.lockout > 0.0f;
            session.lockout = std::max(session.lockout - dt, 0.0f);
            if (paused)
            {
                session.repeat_direction = 0;
            }
            const sim::Inputs inputs =
                paused ? sim::Inputs{}
                       : sim::Inputs{
                             .pick_delta = mouse_x * settings.mouse_pick_speed +
                                           pad.stick_x * settings.pad_pick_speed * dt +
                                           step_delta(session.lock.pick_position(), steps),
                             .push = flick || (stick_up && !session.stick_was_up) || pad.push,
                             // A flick stays latched while the mouse keeps its upward move, which holds the push as a
                             // key does.
                             .push_held = pad.push_held || stick_up ||
                                          (settings.bindings.push_with_mouse_up && session.flick_latched),
                             .set = pad.set,
                         };
            session.stick_was_up = stick_up;

            const sim::StepResult result = session.lock.step(dt, inputs);

            if (DMK::log().is_enabled(DMK::LogLevel::Trace))
            {
                log_inputs(session, pad, mouse_x, mouse_y, steps, dt);
            }
            if (result.set && result.set_late >= 0.0f)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "Lock: set {:.3f} s after the pause, inside the grace",
                    result.set_late
                );
            }
            if (result.wrong_set)
            {
                // UpdateLock fails the pick of a still lock once durability is spent, so the original below runs the
                // vanilla Fail with its own gate, break roll and consequences.
                game::write<float>(self, constants::LP_DURABILITY_OFFSET, 0.0f);
                session.broken_before_fail = game::read<std::int32_t>(self, constants::LP_BROKEN_COUNT_OFFSET);
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "Lock: wrong set on tumbler {} ({}): {} for {:.3f} s, height {:.3f}",
                    session.lock.selected(),
                    speed_name(session.lock.speed(session.lock.selected())),
                    result.wrong_phase == sim::PinPhase::Rising ? "rising" : "falling",
                    result.wrong_time,
                    result.wrong_height
                );
            }

            s_hide_fail_message.store(result.wrong_set, std::memory_order_relaxed);
            const char original = s_update_original(self, dt);
            s_hide_fail_message.store(false, std::memory_order_relaxed);
            if (result.wrong_set && game::state(self) == LockState::IsFailing && settings.fail_lockout_seconds > 0.0f)
            {
                skip_failure_animation(session, self, settings);
            }

            game::write<float>(self, constants::LP_ROTATE_INPUT_OFFSET, rotate);
            game::write<float>(self, constants::LP_STICK_X_OFFSET, stick_x);
            game::write<float>(self, constants::LP_STICK_Y_OFFSET, stick_y);

            const LockState after = game::state(self);
            if (!game::is_play_state(after))
            {
                return original;
            }
            if (result.wrong_set)
            {
                // The failure ended on this frame, or the vanilla gate kept the pick. The durability goes back up, so
                // the zero cannot fail the pick on a later frame.
                const float max_durability = game::read<float>(self, constants::LP_DURABILITY_MAX_OFFSET);
                game::write<float>(
                    self,
                    constants::LP_DURABILITY_OFFSET,
                    max_durability > 0.0f ? max_durability : FALLBACK_DURABILITY
                );
                return original;
            }
            if (settings.tumbler_sounds)
            {
                play_cues(self, result);
            }
            if (result.solved && !session.committed)
            {
                game::write<std::uint8_t>(
                    self,
                    constants::LP_STATE_OFFSET,
                    static_cast<std::uint8_t>(LockState::UnlockingAnim)
                );
                game::write<float>(self, constants::LP_SUCCESS_HOLD_OFFSET, constants::SUCCESS_HOLD_SECONDS);
                session.committed = true;
                (void)DMK::log().try_log(DMK::LogLevel::Info, "Lock: all tumblers set, success handed to the game");
                return original;
            }
            if (settings.noise_enabled && result.moving)
            {
                const float level = game::lockpick_noise_level(game::player_soul(self), settings.noise_distance);
                game::emit_noise(self, level, dt);
            }
            return original;
        }

        /** @brief HUD message detour: drops the failure text of a tumbler lock's wrong set, passes every other text. */
        void __fastcall hud_message_detour(void *hud, std::uint8_t code) noexcept
        {
            if (code == constants::HUD_MESSAGE_LOCKPICK_FAIL && s_hide_fail_message.load(std::memory_order_relaxed))
            {
                return;
            }
            s_hud_message_original(hud, code);
        }

        /**
         * @brief Whether @p session belongs to the lock @p self runs now.
         * @details The game frees C_LockPicking at the end of each lock, and a later lock can get the same address. A
         *          lock passes Init, ExactPositioning and TransitioningIn before play, and keeps its lock entity in
         *          play. A session that sees an earlier state, or another entity in play, belongs to an earlier lock.
         *          A retry of the same lock can pass its first states between two detour calls, so a session whose
         *          lock ended owns no later state before the end either.
         */
        [[nodiscard]] bool owns_lock(const LockSession &session, void *self) noexcept
        {
            if (session.instance != self)
            {
                return false;
            }
            const LockState state = game::state(self);
            if (session.ended && state < LockState::TransitioningOut)
            {
                return false;
            }
            if (state < LockState::SpotSearching)
            {
                return !session.started && !session.vanilla;
            }
            if (game::is_play_state(state) && (session.started || session.vanilla))
            {
                return game::read<std::uint32_t>(self, constants::LP_LOCK_ENTITY_OFFSET) == session.entity;
            }
            return true;
        }

        /**
         * @brief Update detour: runs the tumbler lock for the player's session and passes every other call through.
         * @param self The C_LockPicking session.
         * @param dt Frame time in seconds.
         * @note No-throw: an exception cannot leave the detour, because the game frame above it has no unwind for it.
         */
        char __fastcall update_detour(void *self, float dt) noexcept
        {
            // An NPC's session can update on another thread, so it never touches the player's session or settings.
            if (!game::is_player_session(self))
            {
                return s_update_original(self, dt);
            }
            const Settings &settings = s_settings.refresh();
            const bool owned = owns_lock(s_lock, self);
            if (owned && !s_lock.ended && game::state(self) >= LockState::TransitioningOut)
            {
                s_lock.ended = true;
                (void)DMK::log()
                    .try_log(DMK::LogLevel::Debug, "Lock: left (state {})", static_cast<int>(game::state(self)));
            }
            // A lock without a movie that draws stays vanilla to its end. The next lock looks for the movie again.
            const bool vanilla_lock = owned && s_lock.vanilla;
            if (!settings.enabled || vanilla_lock)
            {
                if (s_lock.view_hidden)
                {
                    // The mod is off, or the lock went back to vanilla. The vanilla lock draws again.
                    sync_vanilla_view(s_lock, self, false);
                }
                if (owned && !s_lock.vanilla)
                {
                    sync_movie(s_lock, false);
                    s_lock = LockSession{};
                }
                return s_update_original(self, dt);
            }

            if (!owned)
            {
                s_lock = LockSession{};
                s_lock.instance = self;
            }

            const LockState state = game::state(self);
            char original = 0;
            if (game::is_play_state(state))
            {
                if (!s_lock.started)
                {
                    start_lock(s_lock, self, settings);
                }
                if (s_lock.vanilla)
                {
                    return s_update_original(self, dt);
                }
                check_thread(s_lock);
                original = play_frame(s_lock, self, dt, settings);
            }
            else
            {
                original = s_update_original(self, dt);
            }

            sync_vanilla_view(s_lock, self, settings.hide_vanilla_view && s_lock.started);
            update_movie(s_lock, self, game::state(self), settings);
            if (s_lock.started && !s_lock.vanilla)
            {
                // The vanilla prompts show the vanilla minigame's controls and keys. The hint row shows the mod's.
                game::hide_lock_prompts();
            }
            return original;
        }
    } // namespace

    DMK::Result<void> install_lockpicking_hook(DMK::hook::HookStack &hooks, std::uintptr_t update)
    {
        if (update == 0)
        {
            return std::unexpected(DMK::Error{DMK::ErrorCode::InvalidArg, "lockpicking_hook/anchor"});
        }
        DMK_TRY(
            created,
            DMK::hook::inline_at<&update_detour>(DMK::hook::InlineRequest{
                .name = "LockPickingUpdate",
                .target = DMK::Address{update},
            })
        );
        s_update_original = created.original<UpdateFn>();
        // The stack owns the hook before the arm, so teardown covers an arm that fails half-way.
        DMK::hook::Hook &stored = hooks.push(std::move(created));
        DMK_TRY_VOID(stored.enable());
        DMK::log().info("LockPicking: Update hooked at {}", DMK::format::format_address(update));
        return {};
    }

    DMK::Result<void> install_hud_message_hook(DMK::hook::HookStack &hooks, std::uintptr_t handler)
    {
        if (handler == 0)
        {
            return std::unexpected(DMK::Error{DMK::ErrorCode::InvalidArg, "lockpicking_hook/hud_anchor"});
        }
        DMK_TRY(
            created,
            DMK::hook::inline_at<&hud_message_detour>(DMK::hook::InlineRequest{
                .name = "LockpickingHudMessage",
                .target = DMK::Address{handler},
            })
        );
        s_hud_message_original = created.original<HudMessageFn>();
        DMK::hook::Hook &stored = hooks.push(std::move(created));
        DMK_TRY_VOID(stored.enable());
        DMK::log().info("LockPicking: HUD messages hooked at {}", DMK::format::format_address(handler));
        return {};
    }

    void reset_lockpicking_session() noexcept
    {
        s_lock = LockSession{};
    }
} // namespace OblivionLockpicking
