/**
 * @file anim_catalog_console.cpp
 * @brief The animation catalog's console commands (see anim_catalog_console.hpp).
 */

#include "anim_catalog_console.hpp"
#include "console_vars.hpp"
#include "core/anim_catalog.hpp"
#include "hooks/anim_patch.hpp"

#include <DetourModKit.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AnimationFramework
{
    namespace
    {
        constexpr std::string_view TEXT_CVAR = "animfw_catalog_text";
        constexpr std::string_view TEXT_CVAR_HELP = "The animation catalog's search text, then its answer.";
        constexpr const char *FIND_COMMAND = "animfw_catalog_find";
        constexpr const char *FIND_COMMAND_HELP =
            "animfw_catalog_find <fragments|all> <first> <count> [seat|horse]: puts "
            "a page of the lines holding animfw_catalog_text in that variable";
        constexpr const char *LINE_COMMAND = "animfw_catalog_line";
        constexpr const char *LINE_COMMAND_HELP =
            "animfw_catalog_line <fragment>: puts its line in animfw_catalog_text";
        constexpr const char *HIDE_COMMAND = "animfw_catalog_hide";
        constexpr const char *HIDE_COMMAND_HELP =
            "animfw_catalog_hide <prefix>: leaves fragments with this name prefix "
            "out of searches";
        constexpr char COMMAND_FAILED[] = "AnimCatalog: a catalog command failed";
        constexpr std::string_view LIST_ALL = "all";
        // A page goes over a console variable and a script shows a few dozen rows at a time.
        constexpr std::size_t MAX_PAGE_LINES = 200;
        constexpr std::size_t MAX_HIDDEN_PREFIXES = 64;

        // Main thread only. s_failed latches a failed registration, so the frame tick does not retry it every frame.
        void *s_text_cvar = nullptr;
        bool s_added = false;
        bool s_failed = false;
        std::unique_ptr<const AnimCatalog> s_catalog;
        std::vector<std::string> s_hidden;

        /** @brief The catalog, built on the first request of a session: nobody pays for it until a mod asks. */
        [[nodiscard]] const AnimCatalog &catalog()
        {
            if (s_catalog == nullptr)
            {
                const auto started = std::chrono::steady_clock::now();
                s_catalog = std::make_unique<const AnimCatalog>(animation_catalog_files());
                const auto elapsed = std::chrono::steady_clock::now() - started;
                DMK::log().info(
                    "AnimCatalog: {} fragments and {} options in {} ms",
                    s_catalog->fragment_count(),
                    s_catalog->option_count(),
                    std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
                );
            }
            return *s_catalog;
        }

        /** @brief The command's whole-number argument, or nothing when it is not one. */
        [[nodiscard]] std::optional<std::size_t> number_argument(void *args, int index)
        {
            const std::string_view text = command_argument(args, index);
            std::size_t value = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            return error == std::errc{} && end == text.data() + text.size() ? std::optional{value} : std::nullopt;
        }

        /// The find command: one page of the catalog lines that hold the text variable, into that variable.
        void find(void *args)
        {
            const std::optional<std::size_t> first = number_argument(args, 2);
            const std::optional<std::size_t> count = number_argument(args, 3);
            if (!first.has_value() || !count.has_value())
            {
                DMK::log().warning("AnimCatalog: {} needs a list, a first line and a count", FIND_COMMAND);
                set_cvar_text(s_text_cvar, std::string{});
                return;
            }
            // A copy: the answer overwrites the variable this text lives in.
            const std::string text{cvar_text(s_text_cvar)};
            const AnimCatalogPage page = catalog().find(
                AnimCatalogQuery{
                    .text = text,
                    .options = command_argument(args, 1) == LIST_ALL,
                    .posture = command_argument(args, 4),
                    .first = *first,
                    .count = std::min(*count, MAX_PAGE_LINES),
                },
                s_hidden
            );
            set_cvar_text(s_text_cvar, std::to_string(page.total) + "\n" + page.lines);
        }

        /// The line command: one fragment's catalog line into the text variable, empty for an unknown fragment.
        void line(void *args)
        {
            const std::optional<std::string_view> found = catalog().fragment_line(command_argument(args, 1));
            set_cvar_text(s_text_cvar, found.has_value() ? std::string{*found} : std::string{});
        }

        /// The hide command: leaves fragments with the given name prefix out of later searches.
        void hide(void *args)
        {
            std::string prefix{command_argument(args, 1)};
            if (prefix.empty() || std::ranges::find(s_hidden, prefix) != s_hidden.end())
            {
                return;
            }
            // Scripts call the command, so the list has a bound: every search tests each prefix against each line.
            if (s_hidden.size() >= MAX_HIDDEN_PREFIXES)
            {
                DMK::log().warning(
                    "AnimCatalog: {} already hides {} prefixes; '{}' is ignored",
                    HIDE_COMMAND,
                    MAX_HIDDEN_PREFIXES,
                    prefix
                );
                return;
            }
            s_hidden.push_back(std::move(prefix));
        }
    } // namespace

    void run_anim_catalog_frame_work() noexcept
    {
        if (s_added || s_failed || !console_ready())
        {
            return;
        }
        bool registered = false;
        s_text_cvar = find_or_register_string_cvar(TEXT_CVAR, TEXT_CVAR_HELP, registered);
        if (s_text_cvar == nullptr)
        {
            s_failed = true;
            (void)DMK::log().try_log(
                DMK::LogLevel::Warning,
                "AnimCatalog: {} could not be found or registered; the catalog commands are off",
                TEXT_CVAR
            );
            return;
        }
        add_console_command(FIND_COMMAND, &guarded_command<&find, COMMAND_FAILED>, FIND_COMMAND_HELP);
        add_console_command(LINE_COMMAND, &guarded_command<&line, COMMAND_FAILED>, LINE_COMMAND_HELP);
        add_console_command(HIDE_COMMAND, &guarded_command<&hide, COMMAND_FAILED>, HIDE_COMMAND_HELP);
        s_added = true;
    }

    void remove_anim_catalog() noexcept
    {
        if (s_added)
        {
            remove_console_command(FIND_COMMAND);
            remove_console_command(LINE_COMMAND);
            remove_console_command(HIDE_COMMAND);
        }
        s_added = false;
        s_failed = false;
        s_text_cvar = nullptr;
        s_catalog.reset();
        s_hidden.clear();
    }
} // namespace AnimationFramework
