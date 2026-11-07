/**
 * @file core/bridge_protocol.cpp
 * @brief Blender add-on requests (see bridge_protocol.hpp).
 */

#include "core/bridge_protocol.hpp"

#include "core/mod_maker.hpp"
#include "core/text_util.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <optional>

namespace AnimationFramework::bridge
{
    namespace
    {
        constexpr char FIELD_SEPARATOR = '\t';
        // The most fields a request has: make's operation and six fields.
        constexpr std::size_t MAX_FIELDS = 7;

        struct OperationSpec
        {
            std::string_view name;
            Operation operation;
            /// Fields after the operation's name.
            std::size_t fields;
        };

        constexpr std::array<OperationSpec, 5> OPERATIONS{{
            {"hello", Operation::Hello, 0},
            {"reload_clips", Operation::ReloadClips, 0},
            {"make", Operation::Make, 6},
            {"play", Operation::Play, 2},
            {"stop", Operation::Stop, 0},
        }};

        [[nodiscard]] constexpr bool is_lower(char c) noexcept
        {
            return c >= 'a' && c <= 'z';
        }

        [[nodiscard]] constexpr bool is_digit(char c) noexcept
        {
            return c >= '0' && c <= '9';
        }

        /// A 0 or 1 field.
        [[nodiscard]] std::optional<bool> read_flag(std::string_view field) noexcept
        {
            if (field == "1")
            {
                return true;
            }
            if (field == "0")
            {
                return false;
            }
            return std::nullopt;
        }
    } // namespace

    bool is_fragment_name(std::string_view text) noexcept
    {
        if (text.empty() || text.size() > MAX_NAME_LENGTH)
        {
            return false;
        }
        const auto letter = [](char c) { return is_lower(c) || (c >= 'A' && c <= 'Z'); };
        return letter(text.front()) &&
               std::ranges::all_of(text, [&letter](char c) { return letter(c) || is_digit(c) || c == '_'; });
    }

    bool is_clip_name(std::string_view text) noexcept
    {
        return !text.empty() && text.size() <= MAX_NAME_LENGTH &&
               std::ranges::all_of(text, [](char c) { return is_lower(c) || is_digit(c) || c == '_'; });
    }

    std::expected<Request, std::string> parse_request(std::string_view message)
    {
        if (message.size() > MAX_REQUEST_BYTES)
        {
            return std::unexpected(std::format("request longer than {} bytes", MAX_REQUEST_BYTES));
        }
        std::array<std::string_view, MAX_FIELDS> fields{};
        std::size_t count = 0;
        std::size_t start = 0;
        while (true)
        {
            const std::size_t end = message.find(FIELD_SEPARATOR, start);
            if (count == MAX_FIELDS)
            {
                return std::unexpected(std::string{"too many fields"});
            }
            fields[count++] = message.substr(start, end == std::string_view::npos ? end : end - start);
            if (end == std::string_view::npos)
            {
                break;
            }
            start = end + 1;
        }

        const auto spec = std::ranges::find(OPERATIONS, fields[0], &OperationSpec::name);
        if (spec == OPERATIONS.end())
        {
            return std::unexpected(std::format("unknown operation '{}'", fields[0]));
        }
        if (count != spec->fields + 1)
        {
            return std::unexpected(std::format("{} takes {} field(s), got {}", spec->name, spec->fields, count - 1));
        }

        Request request{.operation = spec->operation};
        if (request.operation == Operation::Make)
        {
            const std::optional<bool> loop = read_flag(fields[5]);
            const std::optional<bool> upper = read_flag(fields[6]);
            if (!is_mod_id(fields[1]))
            {
                return std::unexpected(std::string{"make: the mod id takes lowercase letters and underscores only"});
            }
            if (!is_fragment_name(fields[2]) || !is_fragment_name(fields[4]))
            {
                return std::unexpected(std::string{"make: the action and the game fragment need fragment names"});
            }
            if (!is_clip_name(fields[3]))
            {
                return std::unexpected(
                    std::string{"make: the clip name takes lowercase letters, digits and underscores only"}
                );
            }
            if (!loop || !upper)
            {
                return std::unexpected(std::string{"make: loop and upper are 0 or 1"});
            }
            request.modid = fields[1];
            request.fragment = fields[2];
            request.clip = fields[3];
            request.like = fields[4];
            request.loop = *loop;
            request.upper = *upper;
        }
        else if (request.operation == Operation::Play)
        {
            const std::optional<bool> upper = read_flag(fields[2]);
            if (!is_fragment_name(fields[1]))
            {
                return std::unexpected(std::string{"play: the action needs a fragment name"});
            }
            if (!upper)
            {
                return std::unexpected(std::string{"play: upper is 0 or 1"});
            }
            request.fragment = fields[1];
            request.upper = *upper;
        }
        return request;
    }

    std::string lua_for(const Request &request)
    {
        std::string body;
        switch (request.operation)
        {
        case Operation::ReloadClips:
            body = std::format("System.ExecuteCommand(\"{}\")", RELOAD_CLIPS_COMMAND);
            break;
        case Operation::Make:
            body = std::format(
                "System.ExecuteCommand(\"{} {} {} {} {}{}{}\")",
                MAKE_MOD_COMMAND,
                request.modid,
                request.fragment,
                request.clip,
                request.like,
                request.loop ? " loop" : "",
                request.upper ? " upper" : ""
            );
            break;
        case Operation::Play:
            body = std::format(
                "player.human:PlayAnim(\"{}\", \"{}\")",
                request.fragment,
                request.upper ? OWN_SCOPES_TAG : std::string_view{}
            );
            break;
        case Operation::Stop:
            body = "player.human:StopAnim()";
            break;
        case Operation::Hello:
            break;
        }
        // Level 0 keeps the chunk position out of the message, so the add-on shows NO_WORLD_ERROR as written.
        return std::format(
            "local ok, message = pcall(function()\n"
            "  if player == nil or player.human == nil then error(\"{}\", 0) end\n"
            "  {}\n"
            "end)\n"
            "if not ok then System.SetCVar(\"{}\", tostring(message)) end\n",
            NO_WORLD_ERROR,
            body,
            ERROR_CVAR
        );
    }

    std::string answer(bool ok, std::string_view text)
    {
        std::string out{ok ? "ok" : "error"};
        out += FIELD_SEPARATOR;
        for (const char c : text)
        {
            out += c == FIELD_SEPARATOR || c == '\n' || c == '\r' ? ' ' : c;
        }
        return out;
    }
} // namespace AnimationFramework::bridge
