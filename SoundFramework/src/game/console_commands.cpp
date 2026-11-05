/**
 * @file game/console_commands.cpp
 * @brief The console commands, added and removed by the main-thread hook.
 */

#include "game/console_commands.hpp"
#include "audio/engine.hpp"
#include "constants.hpp"
#include "game/speech.hpp"
#include "game/voice_catalog.hpp"
#include "packs/sound_def.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace SoundFramework::game::commands
{
    namespace
    {
        using CommandFn = void (*)(void *args);
        using AddCommandFn =
            void(__fastcall *)(void *console, const char *name, CommandFn fn, int flags, const char *help);
        using RemoveCommandFn = bool(__fastcall *)(void *console, const char *name);
        using ArgCountFn = int(__fastcall *)(const void *args);
        using ArgFn = const char *(__fastcall *)(const void *args, int index);
        using GetCVarFn = void *(__fastcall *)(void *console, const char *name);
        using GetStringFn = const char *(__fastcall *)(const void *cvar);
        using SetStringFn = void(__fastcall *)(void *cvar, const char *value);
        using RegisterStringFn = void *(__fastcall *)(void *console,
                                                      const char *name,
                                                      const char *value,
                                                      int flags,
                                                      const char *help,
                                                      void *change_function);

        constexpr const char *LINES_CVAR = "sfw_lines_text";
        constexpr const char *LINES_CVAR_HELP = "The voice line catalog's search text, then its answer.";
        // A page goes over a console variable and a script shows a few dozen rows at a time.
        constexpr std::size_t MAX_LINES_PAGE = 200;

        Anchors s_anchors{};
        /// The variable the catalog's commands talk over. Main thread only.
        void *s_lines_cvar = nullptr;

        // Both flags change only under s_command_mutex; on_frame reads them first without it so that a frame with
        // nothing to do costs two loads.
        std::mutex s_command_mutex;
        std::atomic<bool> s_wanted{false};
        std::atomic<bool> s_registered{false};
        /// Set when gEnv does not hold the objects the anchors describe; the commands then stay off for good.
        bool s_refused = false;

        [[nodiscard]] void *const *vtable_of(const void *object) noexcept
        {
            return *static_cast<void *const *const *>(object);
        }

        [[nodiscard]] int arg_count(const void *args) noexcept
        {
            return reinterpret_cast<ArgCountFn>(vtable_of(args)[constants::CMD_ARGS_COUNT_SLOT])(args);
        }

        [[nodiscard]] std::string_view arg(const void *args, int index) noexcept
        {
            const char *text = reinterpret_cast<ArgFn>(vtable_of(args)[constants::CMD_ARGS_GET_SLOT])(args, index);
            return text == nullptr ? std::string_view{} : std::string_view{text};
        }

        [[nodiscard]] std::optional<float> arg_float(const void *args, int index) noexcept
        {
            const std::string_view text = arg(args, index);
            float value = 0.0f;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size())
            {
                return std::nullopt;
            }
            return value;
        }

        [[nodiscard]] std::optional<std::size_t> arg_size(const void *args, int index) noexcept
        {
            const std::string_view text = arg(args, index);
            std::size_t value = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size())
            {
                return std::nullopt;
            }
            return value;
        }

        /** @brief A NUL-terminated copy of @p text on the process heap, never freed: the console keeps the pointer. */
        [[nodiscard]] const char *leaked_copy(std::string_view text) noexcept
        {
            auto *block = static_cast<char *>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, text.size() + 1));
            if (block != nullptr)
            {
                std::memcpy(block, text.data(), text.size());
            }
            return block;
        }

        [[nodiscard]] bool is_string_variable(const void *cvar) noexcept
        {
            return cvar != nullptr && reinterpret_cast<std::uintptr_t>(vtable_of(cvar)) == s_anchors.cvar_string_vtable;
        }

        /**
         * @brief The variable the catalog's commands talk over, or null when it is not a string variable. The first
         *        loaded copy of the framework registers it; it outlives that copy, so a later copy finds it.
         */
        [[nodiscard]] void *find_or_register_lines_cvar(void *console) noexcept
        {
            void *const *slots = vtable_of(console);
            void *cvar = reinterpret_cast<GetCVarFn>(slots[constants::CONSOLE_GET_CVAR_SLOT])(console, LINES_CVAR);
            if (cvar == nullptr)
            {
                // The console keeps the name and help pointers, so they must outlive this module.
                const char *const name = leaked_copy(LINES_CVAR);
                const char *const help = leaked_copy(LINES_CVAR_HELP);
                if (name == nullptr || help == nullptr)
                {
                    return nullptr;
                }
                cvar = reinterpret_cast<RegisterStringFn>(slots[constants::CONSOLE_REGISTER_STRING_SLOT])(
                    console,
                    name,
                    "",
                    0,
                    help,
                    nullptr
                );
            }
            return is_string_variable(cvar) ? cvar : nullptr;
        }

        [[nodiscard]] std::string_view lines_text() noexcept
        {
            const char *const text =
                reinterpret_cast<GetStringFn>(vtable_of(s_lines_cvar)[constants::CVAR_GET_STRING_SLOT])(s_lines_cvar);
            return text != nullptr ? std::string_view{text} : std::string_view{};
        }

        void set_lines_text(const std::string &text) noexcept
        {
            const auto set = reinterpret_cast<SetStringFn>(vtable_of(s_lines_cvar)[constants::CVAR_SET_STRING_SLOT]);
            set(s_lines_cvar, text.c_str());
        }

        [[nodiscard]] voice_catalog::Languages languages()
        {
            return voice_catalog::Languages{
                .text = string_variable_lowered(constants::TEXT_LANGUAGE_CVAR),
                .voice = string_variable_lowered(constants::VOICE_LANGUAGE_CVAR),
            };
        }

        /** @brief Runs a command body, keeping exceptions out of the game's console. */
        template <typename Body> void guarded(std::string_view command, Body &&body) noexcept
        {
            try
            {
                body();
            }
            catch (const std::exception &error)
            {
                (void)DMK::log().try_log(DMK::LogLevel::Error, "{}: {}", command, error.what());
            }
            catch (...)
            {
                (void)DMK::log().try_log(DMK::LogLevel::Error, "{}: unknown error", command);
            }
        }

        void cmd_lines_find(void *args)
        {
            guarded(
                "sfw_lines_find",
                [args]
                {
                    if (s_lines_cvar == nullptr)
                    {
                        DMK::log().warning("sfw_lines_find: {} is unavailable", LINES_CVAR);
                        return;
                    }
                    const std::optional<std::size_t> first = arg_size(args, 1);
                    const std::optional<std::size_t> count = arg_size(args, 2);
                    if (!first || !count)
                    {
                        DMK::log().warning("sfw_lines_find: usage sfw_lines_find <first> <count>");
                        return;
                    }
                    // A copy: the answer overwrites the variable this text lives in.
                    const std::string wanted{lines_text()};
                    set_lines_text(voice_catalog::find(wanted, *first, std::min(*count, MAX_LINES_PAGE), languages()));
                }
            );
        }

        void cmd_lines_line(void *args)
        {
            guarded(
                "sfw_lines_line",
                [args]
                {
                    if (s_lines_cvar == nullptr)
                    {
                        DMK::log().warning("sfw_lines_line: {} is unavailable", LINES_CVAR);
                        return;
                    }
                    set_lines_text(voice_catalog::line(arg(args, 1), languages()));
                }
            );
        }

        void cmd_play(void *args)
        {
            guarded(
                "sfw_play",
                [args]
                {
                    const int count = arg_count(args);
                    if (count != 2 && count != 5)
                    {
                        DMK::log().warning("sfw_play: usage sfw_play <sound> [x y z]");
                        return;
                    }
                    std::optional<audio::WorldPosition> at;
                    if (count == 5)
                    {
                        const std::optional<float> x = arg_float(args, 2);
                        const std::optional<float> y = arg_float(args, 3);
                        const std::optional<float> z = arg_float(args, 4);
                        if (!x || !y || !z)
                        {
                            DMK::log().warning("sfw_play: x y z must be numbers");
                            return;
                        }
                        at = audio::WorldPosition{*x, *y, *z};
                    }
                    audio::request_play(std::string(arg(args, 1)), at);
                }
            );
        }

        void cmd_say(void *args)
        {
            guarded(
                "sfw_say",
                [args]
                {
                    const int count = arg_count(args);
                    if (count < 3)
                    {
                        DMK::log().warning(
                            "sfw_say: usage sfw_say <entity name> <line name> | <voice path> [speaker:<name key>] "
                            "[bubble:<text key>] [mood:<mood>] [strength:<0..2>] [subtitle text]"
                        );
                        return;
                    }
                    const std::string_view voice = arg(args, 2);
                    if (voice.find('/') == std::string_view::npos)
                    {
                        // A bare name is a pack voice line; everything about it comes from the pack.
                        (void)speech::say_line(std::string(arg(args, 1)), std::string(voice));
                        return;
                    }
                    constexpr std::string_view speaker_prefix = "speaker:";
                    constexpr std::string_view bubble_prefix = "bubble:";
                    constexpr std::string_view mood_prefix = "mood:";
                    constexpr std::string_view strength_prefix = "strength:";
                    std::string speaker;
                    std::string bubble;
                    std::vector<face::PoseWeight> mood;
                    float strength = 1.0f;
                    float max_strength = face::MAX_MOOD_STRENGTH;
                    int first_text = 3;
                    for (; first_text < count; ++first_text)
                    {
                        const std::string_view option = arg(args, first_text);
                        if (option.starts_with(speaker_prefix))
                        {
                            speaker = std::string(option.substr(speaker_prefix.size()));
                        }
                        else if (option.starts_with(bubble_prefix))
                        {
                            bubble = std::string(option.substr(bubble_prefix.size()));
                        }
                        else if (option.starts_with(mood_prefix))
                        {
                            const std::optional<packs::MoodDef> found =
                                speech::find_mood(option.substr(mood_prefix.size()));
                            if (!found.has_value())
                            {
                                DMK::log().warning("sfw_say: unknown mood '{}'", option.substr(mood_prefix.size()));
                                return;
                            }
                            mood = found->poses;
                            max_strength = found->max_strength;
                        }
                        else if (option.starts_with(strength_prefix))
                        {
                            const std::string_view number = option.substr(strength_prefix.size());
                            float value = 1.0f;
                            const auto [end, error] =
                                std::from_chars(number.data(), number.data() + number.size(), value);
                            if (error != std::errc{} || end != number.data() + number.size())
                            {
                                DMK::log().warning("sfw_say: strength must be a number");
                                return;
                            }
                            strength = std::clamp(value, 0.0f, face::MAX_MOOD_STRENGTH);
                        }
                        else
                        {
                            break;
                        }
                    }
                    // The mood's MaxStrength caps the strength, whichever of the two options came first.
                    strength = std::min(strength, max_strength);
                    std::string subtitle;
                    for (int i = first_text; i < count; ++i)
                    {
                        if (!subtitle.empty())
                        {
                            subtitle.push_back(' ');
                        }
                        subtitle.append(arg(args, i));
                    }
                    (void)speech::say(
                        std::string(arg(args, 1)),
                        std::string(arg(args, 2)),
                        speaker,
                        bubble,
                        subtitle,
                        false,
                        mood,
                        strength
                    );
                }
            );
        }

        void cmd_lipsync_offset(void *args)
        {
            guarded(
                "sfw_lipsync_offset",
                [args]
                {
                    const std::optional<float> value = arg_count(args) == 2 ? arg_float(args, 1) : std::nullopt;
                    if (!value.has_value())
                    {
                        DMK::log().warning(
                            "sfw_lipsync_offset: usage sfw_lipsync_offset <ms> (positive = mouth earlier)"
                        );
                        return;
                    }
                    speech::set_lip_offset_ms(static_cast<int>(*value));
                    DMK::log().info("sfw_lipsync_offset: {} ms", static_cast<int>(*value));
                }
            );
        }

        void cmd_mood_override(void *args)
        {
            guarded(
                "sfw_mood_override",
                [args]
                {
                    const int count = arg_count(args);
                    if (count < 2)
                    {
                        DMK::log().warning(
                            "sfw_mood_override: usage sfw_mood_override <mood> [pose:weight ...] (none = reset)"
                        );
                        return;
                    }
                    const std::string name(arg(args, 1));
                    if (!packs::valid_sound_name(name))
                    {
                        DMK::log().warning("sfw_mood_override: '{}' is not a valid mood name", name);
                        return;
                    }
                    std::string list;
                    for (int i = 2; i < count; ++i)
                    {
                        list += list.empty() ? "" : ",";
                        list += arg(args, i);
                    }
                    std::string error;
                    std::optional<std::vector<face::PoseWeight>> poses = face::parse_poses(list, error);
                    if (!poses.has_value())
                    {
                        DMK::log().warning("sfw_mood_override: {}", error);
                        return;
                    }
                    DMK::log().info("sfw_mood_override: {} = {} pose(s)", name, poses->size());
                    speech::override_mood(name, std::move(*poses));
                }
            );
        }

        void cmd_mood_dump(void *args)
        {
            guarded(
                "sfw_mood_dump",
                [args]
                {
                    if (arg_count(args) != 2)
                    {
                        DMK::log().warning("sfw_mood_dump: usage sfw_mood_dump <mood>");
                        return;
                    }
                    const std::string name(arg(args, 1));
                    const std::optional<packs::MoodDef> mood = speech::find_mood(name);
                    if (!mood.has_value())
                    {
                        DMK::log().warning("sfw_mood_dump: no mood named '{}'", name);
                        return;
                    }
                    std::string list;
                    for (const face::PoseWeight &pose : mood->poses)
                    {
                        list += std::format("{}{}:{}", list.empty() ? "" : ", ", pose.pose, pose.weight);
                    }
                    const std::string limit = mood->max_strength < face::MAX_MOOD_STRENGTH
                                                  ? std::format("\nMaxStrength = {}", mood->max_strength)
                                                  : std::string{};
                    DMK::log().info("sfw_mood_dump: paste into a pack INI:\n[{}]\nPoses = {}{}", name, list, limit);
                }
            );
        }

        void cmd_stop(void *args)
        {
            guarded(
                "sfw_stop",
                [args]
                {
                    if (arg_count(args) != 2)
                    {
                        DMK::log().warning("sfw_stop: usage sfw_stop <sound>");
                        return;
                    }
                    audio::request_stop(std::string(arg(args, 1)));
                }
            );
        }

        void cmd_stop_all(void *)
        {
            guarded("sfw_stop_all", [] { audio::request_stop_all(); });
        }

        void cmd_reload(void *)
        {
            guarded("sfw_reload", [] { audio::request_reload(); });
        }

        void cmd_list(void *)
        {
            guarded("sfw_list", [] { audio::request_list(); });
        }

        struct Command
        {
            const char *name;
            CommandFn fn;
            const char *help;
        };

        constexpr std::array COMMANDS = {
            Command{
                "sfw_lines_find",
                &cmd_lines_find,
                "sfw_lines_find <first> <count>: puts a page of the voice lines holding sfw_lines_text in that variable"
            },
            Command{
                "sfw_lines_line",
                &cmd_lines_line,
                "sfw_lines_line <voice path>: puts that voice file's line in sfw_lines_text"
            },
            Command{
                "sfw_play",
                &cmd_play,
                "sfw_play <sound> [x y z]: plays a SoundFramework sound at a world position, "
                "or at the camera without one"
            },
            Command{
                "sfw_say",
                &cmd_say,
                "sfw_say <entity name> <line name> | <voice path> [speaker:<name key>] "
                "[bubble:<text key>] [mood:<mood>] [strength:<0..2>] [subtitle text]: the entity says a pack "
                "voice line, or speaks a localization voice file (no extension) with optional mood and text"
            },
            Command{
                "sfw_lipsync_offset",
                &cmd_lipsync_offset,
                "sfw_lipsync_offset <ms>: shifts voice-driven mouth "
                "movement, positive earlier than the sound, negative later"
            },
            Command{
                "sfw_mood_override",
                &cmd_mood_override,
                "sfw_mood_override <mood> [pose:weight ...]: replaces or "
                "creates a mood until reload (none = reset)"
            },
            Command{
                "sfw_mood_dump",
                &cmd_mood_dump,
                "sfw_mood_dump <mood>: writes the mood's poses to the log, ready to "
                "paste into a pack INI"
            },
            Command{"sfw_stop", &cmd_stop, "sfw_stop <sound>: stops every playing instance of a SoundFramework sound"},
            Command{"sfw_stop_all", &cmd_stop_all, "sfw_stop_all: stops every SoundFramework sound"},
            Command{
                "sfw_reload",
                &cmd_reload,
                "sfw_reload: stops every SoundFramework sound and reads the packs again"
            },
            Command{"sfw_list", &cmd_list, "sfw_list: writes the SoundFramework catalog to KCD2_SoundFramework.log"},
        };

        /** @brief What reconcile() did; on_frame() logs it once the lock is released. */
        struct Reconciled
        {
            enum class Change : std::uint8_t
            {
                None,
                Added,
                Removed,
                Refused,
            };

            Change change = Change::None;
            /// For Refused: the system gEnv holds, the system the hook sees, and the console's vtable.
            std::uintptr_t live_system = 0;
            std::uintptr_t hooked_system = 0;
            std::uintptr_t console_vtable = 0;
        };

        /**
         * @brief Adds or removes the commands as wanted, through the live console once gEnv holds one.
         * @note Main thread only, under s_command_mutex. Sets s_refused when gEnv does not hold the CSystem the hook
         *       sees and the CXConsole the anchors describe.
         */
        [[nodiscard]] Reconciled reconcile(void *system) noexcept
        {
            const bool wanted = s_wanted.load(std::memory_order_relaxed);
            const bool registered = s_registered.load(std::memory_order_relaxed);
            if (wanted == registered || (wanted && s_refused))
            {
                return {};
            }
            const auto *genv = reinterpret_cast<const std::uint8_t *>(s_anchors.genv);
            void *const console = *reinterpret_cast<void *const *>(genv + constants::GENV_CONSOLE);
            void *const live_system = *reinterpret_cast<void *const *>(genv + constants::GENV_SYSTEM);
            if (console == nullptr || live_system == nullptr)
            {
                return {};
            }
            const auto console_vtable = reinterpret_cast<std::uintptr_t>(vtable_of(console));
            const auto system_vtable = reinterpret_cast<std::uintptr_t>(vtable_of(live_system));
            if (live_system != system || system_vtable != s_anchors.system_vtable ||
                console_vtable != s_anchors.console_vtable)
            {
                s_refused = true;
                return Reconciled{
                    .change = Reconciled::Change::Refused,
                    .live_system = reinterpret_cast<std::uintptr_t>(live_system),
                    .hooked_system = reinterpret_cast<std::uintptr_t>(system),
                    .console_vtable = console_vtable,
                };
            }
            void *const *slots = vtable_of(console);
            if (wanted)
            {
                const auto add = reinterpret_cast<AddCommandFn>(slots[constants::CONSOLE_ADD_COMMAND_SLOT]);
                for (const Command &command : COMMANDS)
                {
                    add(console, command.name, command.fn, 0, command.help);
                }
                s_lines_cvar = find_or_register_lines_cvar(console);
                s_registered.store(true, std::memory_order_release);
                return Reconciled{
                    .change = Reconciled::Change::Added,
                };
            }
            const auto remove = reinterpret_cast<RemoveCommandFn>(slots[constants::CONSOLE_REMOVE_COMMAND_SLOT]);
            for (const Command &command : COMMANDS)
            {
                (void)remove(console, command.name);
            }
            s_registered.store(false, std::memory_order_release);
            return Reconciled{
                .change = Reconciled::Change::Removed,
            };
        }

        void log_reconciled(const Reconciled &done) noexcept
        {
            switch (done.change)
            {
            case Reconciled::Change::None:
                break;
            case Reconciled::Change::Added:
                (void)DMK::log().try_log(
                    DMK::LogLevel::Info,
                    "Commands: sfw_play, sfw_say, sfw_lipsync_offset, sfw_mood_override, sfw_mood_dump, sfw_stop, "
                    "sfw_stop_all, sfw_reload, sfw_list, sfw_lines_find and sfw_lines_line added (main thread {})",
                    GetCurrentThreadId()
                );
                break;
            case Reconciled::Change::Removed:
                (void)DMK::log()
                    .try_log(DMK::LogLevel::Info, "Commands: removed (main thread {})", GetCurrentThreadId());
                break;
            case Reconciled::Change::Refused:
                (void)DMK::log().try_log(
                    DMK::LogLevel::Error,
                    "Commands: gEnv does not match the anchors (system {:#x} vs hooked {:#x}, console vtable {:#x}); "
                    "commands stay off",
                    done.live_system,
                    done.hooked_system,
                    done.console_vtable
                );
                break;
            }
        }
    } // namespace

    std::string string_variable_lowered(const char *name)
    {
        if (!s_anchors.voice_language)
        {
            return {};
        }
        const auto *const genv = reinterpret_cast<const std::uint8_t *>(s_anchors.genv);
        void *const console = *reinterpret_cast<void *const *>(genv + constants::GENV_CONSOLE);
        if (console == nullptr || reinterpret_cast<std::uintptr_t>(vtable_of(console)) != s_anchors.console_vtable)
        {
            return {};
        }
        const void *const cvar =
            reinterpret_cast<GetCVarFn>(vtable_of(console)[constants::CONSOLE_GET_CVAR_SLOT])(console, name);
        if (!is_string_variable(cvar))
        {
            return {};
        }
        const char *const value = reinterpret_cast<GetStringFn>(vtable_of(cvar)[constants::CVAR_GET_STRING_SLOT])(cvar);
        std::string lowered{value != nullptr ? value : ""};
        std::ranges::transform(
            lowered,
            lowered.begin(),
            [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
        );
        return lowered;
    }

    void configure(const Anchors &anchors) noexcept
    {
        s_anchors = anchors;
        const std::lock_guard lock(s_command_mutex);
        s_refused = false;
        s_wanted.store(true, std::memory_order_release);
    }

    void on_frame(void *system) noexcept
    {
        if (s_wanted.load(std::memory_order_acquire) == s_registered.load(std::memory_order_acquire))
        {
            return;
        }
        Reconciled done;
        {
            // Never block the frame: if shutdown holds the lock, the next frame tries again.
            const std::unique_lock lock(s_command_mutex, std::try_to_lock);
            if (!lock.owns_lock())
            {
                return;
            }
            done = reconcile(system);
        }
        log_reconciled(done);
    }

    bool remove_commands(std::chrono::milliseconds deadline) noexcept
    {
        {
            const std::lock_guard lock(s_command_mutex);
            s_wanted.store(false, std::memory_order_release);
            if (!s_registered.load(std::memory_order_acquire))
            {
                return true;
            }
        }
        const auto until = std::chrono::steady_clock::now() + deadline;
        while (std::chrono::steady_clock::now() < until)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
            if (!s_registered.load(std::memory_order_acquire))
            {
                return true;
            }
        }
        return false;
    }
} // namespace SoundFramework::game::commands
