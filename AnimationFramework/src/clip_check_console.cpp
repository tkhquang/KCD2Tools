/**
 * @file clip_check_console.cpp
 * @brief The clip check command (see clip_check_console.hpp).
 */

#include "clip_check_console.hpp"

#include "console_vars.hpp"
#include "core/mod_tools.hpp"
#include "core/text_util.hpp"
#include "hooks/anim_patch.hpp"

#include <DetourModKit.hpp>

#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace AnimationFramework
{
    namespace
    {
        constexpr char COMMAND_FAILED[] = "ClipCheck: the clip check failed";
        constexpr const char *CHECK_COMMAND = "animfw_check_clip";
        constexpr const char *CHECK_HELP =
            "Animation Framework: checks a clip for what breaks it in game. animfw_check_clip <clip> [reference] "
            "[loop]: the clip's file name or path; a game clip whose bones a full-body clip should cover too; loop "
            "when the clip loops.";
        constexpr std::string_view LOOP_WORD = "loop";

        // Main thread only.
        bool s_added = false;

        /** @brief One line to the console and kcd.log, and to the framework's log. */
        void print(std::string_view line)
        {
            game_log_line(line);
            DMK::log().info("ClipCheck: {}", line);
        }

        /// The check command: checks the clip, against the reference clip when given, and prints the findings.
        void check(void *args)
        {
            std::vector<std::string> names;
            bool loops = false;
            for (int i = 1; i < command_argument_count(args); ++i)
            {
                const std::string_view argument = command_argument(args, i);
                if (lowercase(argument) == LOOP_WORD)
                {
                    loops = true;
                }
                else if (!argument.empty())
                {
                    names.emplace_back(argument);
                }
            }
            if (names.empty() || names.size() > 2)
            {
                print(std::format("usage: {} <clip> [reference clip] [loop]", CHECK_COMMAND));
                return;
            }
            const ClipCheckRequest request{
                .clip = names[0],
                .reference = names.size() > 1 ? names[1] : std::string{},
                .loops = loops,
            };
            report_clip_check(running_game_tools(), request, [](std::string_view line) { print(line); });
        }
    } // namespace

    void run_clip_check_frame_work() noexcept
    {
        if (s_added || !console_ready())
        {
            return;
        }
        add_console_command(CHECK_COMMAND, &guarded_command<&check, COMMAND_FAILED>, CHECK_HELP);
        s_added = true;
    }

    void remove_clip_check_console() noexcept
    {
        if (s_added)
        {
            remove_console_command(CHECK_COMMAND);
        }
        s_added = false;
    }
} // namespace AnimationFramework
