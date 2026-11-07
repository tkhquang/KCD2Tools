/**
 * @file core/bridge_protocol.hpp
 * @brief The requests the Blender add-on sends the framework's local pipe, and the Lua each one runs in the game.
 *
 * One pipe message is one request: an operation and its fields, separated by tabs, in UTF-8. The answer is one
 * message too: `ok` or `error`, a tab, then a line of text.
 *
 *   hello                                         -> ok <tab> <protocol version> <space> <framework version tag>
 *   reload_clips                                  -> animfw_reload_clips
 *   make <modid> <fragment> <clip> <like> <loop> <upper>  (loop, upper: 0 or 1) -> animfw_make_mod
 *   play <fragment> <upper>                       -> player.human:PlayAnim
 *   stop                                          -> player.human:StopAnim
 *
 * Every operation but hello runs the same Lua as the add-on's Lua runner fallback, so both paths reach the game the
 * same way: the console commands through System.ExecuteCommand, the play and the stop through the game's own binds and
 * the framework's hooks on them. The names follow the add-on's own rules, so no request can carry Lua of its own, a
 * console separator or a quote.
 */
#ifndef ANIMFW_CORE_BRIDGE_PROTOCOL_HPP
#define ANIMFW_CORE_BRIDGE_PROTOCOL_HPP

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace AnimationFramework::bridge
{
    /// Raised when a request or an answer changes shape. The add-on reads it from the hello answer.
    inline constexpr int PROTOCOL_VERSION = 1;
    /// The longest request the pipe takes. Valid requests stay far below it.
    inline constexpr std::size_t MAX_REQUEST_BYTES = 1024;
    /// The longest name field (fragment, clip, game fragment). Mod ids follow is_mod_id.
    inline constexpr std::size_t MAX_NAME_LENGTH = 128;
    /**
     * @brief The text variable the Lua sets to its error message, which the framework reads back after the run.
     * @details The engine's ExecuteBuffer reports only success. The message itself goes to kcd.log with its call
     *          stack, so the wrapper's pcall catches it and hands it over here instead.
     */
    inline constexpr std::string_view ERROR_CVAR = "animfw_bridge_error";
    inline constexpr std::string_view ERROR_CVAR_HELP =
        "Animation Framework: the error of the last request from the Blender add-on. Written by the framework.";
    /// The error of a request while no game world is loaded: the Lua has no player script table then.
    inline constexpr std::string_view NO_WORLD_ERROR = "no game world: load a save first";

    enum class Operation : std::uint8_t
    {
        Hello,
        ReloadClips,
        Make,
        Play,
        Stop,
    };

    /** @brief A checked request. Fields an operation does not use stay empty or false. */
    struct Request
    {
        Operation operation{Operation::Hello};
        std::string modid;
        std::string fragment;
        std::string clip;
        std::string like;
        bool loop{false};
        bool upper{false};
    };

    /** @brief A fragment name as the add-on takes it: a letter, then letters, digits and underscores. */
    [[nodiscard]] bool is_fragment_name(std::string_view text) noexcept;

    /** @brief A clip name as the add-on takes it: lowercase letters, digits and underscores. */
    [[nodiscard]] bool is_clip_name(std::string_view text) noexcept;

    /**
     * @brief Reads one request message.
     * @return The request, or the reason it was refused, ready for the error answer.
     * @throws std::bad_alloc If a field cannot be copied.
     */
    [[nodiscard]] std::expected<Request, std::string> parse_request(std::string_view message);

    /**
     * @brief The Lua chunk the game runs for @p request (any operation but Hello).
     * @details The body runs inside pcall, after a check for the player's script table that fails with
     *          NO_WORLD_ERROR. An error sets ERROR_CVAR to its message through System.SetCVar.
     * @throws std::bad_alloc If the text cannot be built.
     */
    [[nodiscard]] std::string lua_for(const Request &request);

    /**
     * @brief One answer message: `ok` or `error`, a tab, then @p text with its tabs and line breaks made spaces.
     * @throws std::bad_alloc If the text cannot be built.
     */
    [[nodiscard]] std::string answer(bool ok, std::string_view text);
} // namespace AnimationFramework::bridge

#endif // ANIMFW_CORE_BRIDGE_PROTOCOL_HPP
