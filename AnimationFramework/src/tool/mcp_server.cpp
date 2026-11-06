/**
 * @file tool/mcp_server.cpp
 * @brief The tool's MCP server (see mcp_server.hpp).
 */

#include "tool/mcp_server.hpp"

#include "core/mod_tools.hpp"
#include "tool/offline_tools.hpp"
#include "version.hpp"

#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AnimationFramework
{
    namespace
    {
        using json = nlohmann::json;
        namespace fs = std::filesystem;

        constexpr std::string_view MODERN_VERSION = "2026-07-28";
        constexpr std::array<std::string_view, 4>
            LEGACY_VERSIONS{"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};
        constexpr std::string_view META_VERSION = "io.modelcontextprotocol/protocolVersion";
        constexpr std::string_view META_SERVER_INFO = "io.modelcontextprotocol/serverInfo";
        constexpr int PARSE_ERROR = -32700;
        constexpr int INVALID_REQUEST = -32600;
        constexpr int METHOD_NOT_FOUND = -32601;
        constexpr int INVALID_PARAMS = -32602;
        constexpr int INTERNAL_ERROR = -32603;
        constexpr int UNSUPPORTED_VERSION = -32022;
        constexpr std::int64_t LIST_TTL_MS = 3600000;
        constexpr std::size_t DEFAULT_LIMIT = 50;
        constexpr std::size_t MAX_LIMIT = 500;

        constexpr std::string_view INSTRUCTIONS =
            "Makes animation mods for Kingdom Come: Deliverance II with the Animation Framework, from the game's own "
            "files (no game running). Workflow: (1) find_game_fragments to pick a game action set up like the new one "
            "(body layers, blends, procedural layers: ADLG_Wave for an upper-body gesture, a full-body action for one "
            "that stops Henry); (2) check_clip on the user's exported .caf and fix its warnings (or use a game clip by "
            "name, find_game_clips); (3) make_action into the mod's source folder, named after its mod id (lowercase "
            "letters and underscores only); (4) pack_mod into the user's mod manager folder or the game's Mods folder. "
            "Animations from other skeletons (.bvh, .fbx) go through retarget_clip first, Blender .i_caf exports "
            "through compile_clip. Clips play by file name: start new clip names with the mod id. In game the action "
            "plays with player.human:PlayAnim(\"<fragment>\", \"\") on Henry, or on a male NPC's human table (the tags "
            "\"animfw_own_scopes\" instead of \"\" for an upper-body action).";

        [[nodiscard]] json server_info()
        {
            return json{
                {"name", "kcd2-animation-framework"},
                {"title", "KCD2 Animation Framework"},
                {"version", std::string{version::VERSION_TAG}.substr(1)}
            };
        }

        [[nodiscard]] json string_property(std::string_view description)
        {
            return json{{"type", "string"}, {"description", description}};
        }

        [[nodiscard]] json tool_list()
        {
            const json limit{
                {"type", "integer"},
                {"minimum", 1},
                {"maximum", MAX_LIMIT},
                {"description", "How many to return (default 50)."}
            };
            const json loop{
                {"type", "boolean"},
                {"description", "The clip loops until the action is stopped (idles, cycles)."}
            };
            return json::array({
                {{"name", "check_clip"},
                 {"title", "Check a clip"},
                 {"description",
                  "Plays a clip on Henry's skeleton and reports what would break it in game: bones the skeleton lacks, "
                  "back slots skeleton mods move, bones a reference clip covers that it leaves to the bind pose "
                  "(T-pose), a loop that jumps, root speed, sliding feet. Warnings need fixing; notes are "
                  "information."},
                 {"inputSchema",
                  {{"type", "object"},
                   {"properties",
                    {{"clip",
                      string_property(
                          "A .caf file's full path, or a game clip by name (relaxed_walk_medium_player) or path."
                      )},
                     {"reference",
                      string_property("Optional: a game clip whose bones a full-body clip in its place should cover.")},
                     {"loop", loop}}},
                   {"required", json::array({"clip"})},
                   {"additionalProperties", false}}}},
                {{"name", "find_game_fragments"},
                 {"title", "Find game fragments"},
                 {"description",
                  "Searches the game's fragments (Mannequin actions such as ADLG_Wave): one line each, its name, the "
                  "tags it plays on standing, 'partial' when it leaves out the full body, seat and horse versions, "
                  "'women' when women have it."},
                 {"inputSchema",
                  {{"type", "object"},
                   {"properties",
                    {{"text", string_property("Found in any field, ignoring case; empty lists all.")},
                     {"limit", limit}}},
                   {"additionalProperties", false}}}},
                {{"name", "find_game_clips"},
                 {"title", "Find game clips"},
                 {"description", "Searches the game's clips by path (animations/humans/male/...), ignoring case."},
                 {"inputSchema",
                  {{"type", "object"},
                   {"properties", {{"text", string_property("Part of the clip's path or name.")}, {"limit", limit}}},
                   {"required", json::array({"text"})},
                   {"additionalProperties", false}}}},
                {{"name", "make_action"},
                 {"title", "Make an action"},
                 {"description",
                  "Adds an action to a mod's source folder: a new fragment that plays the clip, set up like a game "
                  "fragment. Copies a .caf into the mod, checks it against the game fragment's own clip, writes the "
                  "three patches and a mod.manifest. Run again with the same fragment to change it."},
                 {"inputSchema",
                  {{"type", "object"},
                   {"properties",
                    {{"mod_folder",
                      string_property("The mod's source folder, named after its mod id; made when missing.")},
                     {"fragment",
                      string_property(
                          "The new fragment's name, letters, digits and underscores, best started with the mod's name "
                          "(MyMod_Wave)."
                      )},
                     {"clip",
                      string_property(
                          "A .caf file's full path (copied into the mod), or a clip the mod or the game has, by name."
                      )},
                     {"game_fragment",
                      string_property("The game fragment to set the action up like (find_game_fragments).")},
                     {"loop", loop},
                     {"upper",
                      {{"type", "boolean"},
                       {"description",
                        "Play on the upper body only, over what Henry is doing (played with the animfw_own_scopes "
                        "tag). Use it for clips that animate the arms or head only, such as gestures: a full-body "
                        "action puts every bone the clip leaves out in the bind pose."}}}}},
                   {"required", json::array({"mod_folder", "fragment", "clip", "game_fragment"})},
                   {"additionalProperties", false}}}},
                {{"name", "retarget_clip"},
                 {"title", "Retarget an animation"},
                 {"description",
                  "Puts an animation made on another skeleton (a .bvh or .fbx: motion capture, Mixamo, SMPL/Meshy, "
                  "Unreal, 100STYLE, CMU) on Henry's and compiles it into a .caf, then checks it. Bones are matched by "
                  "name; the result lists each Henry bone and the joint it follows, and how the feet came out (planted "
                  "slide, lowest height). Fix a wrong match with map. Make the action with make_action afterwards."},
                 {"inputSchema",
                  {{"type", "object"},
                   {"properties",
                    {{"source", string_property("The .bvh or .fbx file's full path.")},
                     {"target",
                      string_property(
                          "The .caf to write, best in the mod's Data/Animations/humans/male/<modid>/ folder, named "
                          "<modid>_<name>.caf."
                      )},
                     {"take",
                      string_property("Optional: the take (animation) to read from a file that holds several.")},
                     {"calibrate_frame",
                      {{"type", "integer"},
                       {"minimum", 0},
                       {"description",
                        "Optional: a source frame where it stands neutral, used as the reference pose instead of its "
                        "rest pose (when the rest pose is not a T-pose or an A-pose, or its head is tilted)."}}},
                     {"root",
                      {{"type", "string"},
                       {"enum", json::array({"path", "line", "in-place"})},
                       {"description",
                        "How the root moves: along the pelvis's smoothed path (default), in a straight line at a "
                        "steady speed (looping locomotion), or not at all (an in-place cycle)."}}},
                     {"root_turns",
                      {{"type", "boolean"}, {"description", "The root turns with the body's facing (turning clips)."}}},
                     {"lock_feet",
                      {{"type", "boolean"},
                       {"description",
                        "Hold planted heels and balls of the feet in place with leg IK, for sources whose feet skate "
                        "(not with root in-place)."}}},
                     {"from_seconds",
                      {{"type", "number"},
                       {"minimum", 0},
                       {"description",
                        "Optional: where to start in the source, in seconds (mocap takes often start with a "
                        "T-pose)."}}},
                     {"to_seconds",
                      {{"type", "number"},
                       {"minimum", 0},
                       {"description", "Optional: where to end in the source, in seconds."}}},
                     {"map",
                      {{"type", "object"},
                       {"additionalProperties", {{"type", "string"}}},
                       {"description",
                        "Optional: Henry bone -> source joint, replacing the automatic match (an empty joint leaves "
                        "Henry's bone out)."}}},
                     {"hands",
                      string_property(
                          "Optional: the game clip whose first frame poses the fingers when the source has none "
                          "(default relaxed_idle_both_player; 'none' for the default pose)."
                      )},
                     {"loop",
                      {{"type", "boolean"},
                       {"description", "Optional: force the clip's loop flag (detected otherwise)."}}}}},
                   {"required", json::array({"source", "target"})},
                   {"additionalProperties", false}}}},
                {{"name", "compile_clip"},
                 {"title", "Compile an intermediate animation"},
                 {"description",
                  "Compiles an .i_caf (what Blender exporters such as Bohemian Animator write for rc.exe) into a .caf "
                  "for Henry's skeleton without rc.exe, keeping every bone (overlay drops the ones that never move), "
                  "then checks it."},
                 {"inputSchema",
                  {{"type", "object"},
                   {"properties",
                    {{"source", string_property("The .i_caf file's full path.")},
                     {"target", string_property("The .caf to write.")},
                     {"overlay",
                      {{"type", "boolean"},
                       {"description", "Leave out bones that never leave their default pose (for overlay clips)."}}},
                     {"additive",
                      {{"type", "boolean"},
                       {"description", "Store each frame's change from the first (additive clips)."}}},
                     {"loop",
                      {{"type", "boolean"},
                       {"description", "Optional: force the clip's loop flag (detected otherwise)."}}}}},
                   {"required", json::array({"source", "target"})},
                   {"additionalProperties", false}}}},
                {{"name", "list_mod_actions"},
                 {"title", "List a mod's actions"},
                 {"description",
                  "The actions a mod's source folder has: each fragment, the clip it plays and whether it loops."},
                 {"inputSchema",
                  {{"type", "object"},
                   {"properties", {{"mod_folder", string_property("The mod's source folder.")}}},
                   {"required", json::array({"mod_folder"})},
                   {"additionalProperties", false}}}},
                {{"name", "pack_mod"},
                 {"title", "Pack a mod"},
                 {"description",
                  "Writes a mod's source folder as installed: <install_folder>/mod.manifest and Data/<modid>.pak. Give "
                  "a mod manager's folder for the mod (MO2: mods/<Mod Name>/<modid>) or the game's Mods/<modid>."},
                 {"inputSchema",
                  {{"type", "object"},
                   {"properties",
                    {{"mod_folder", string_property("The mod's source folder.")},
                     {"install_folder",
                      string_property("Where the installed mod goes; not inside the source's Data folder.")}}},
                   {"required", json::array({"mod_folder", "install_folder"})},
                   {"additionalProperties", false}}}},
            });
        }

        /** @brief The game, read the first time a tool needs it. */
        class LazyGame
        {
        public:
            explicit LazyGame(std::optional<fs::path> folder) : m_folder(std::move(folder)) {}

            [[nodiscard]] std::expected<OfflineGame *, std::string> get()
            {
                if (m_game)
                {
                    return m_game.get();
                }
                if (!m_folder)
                {
                    m_folder = find_steam_game();
                }
                if (!m_folder)
                {
                    return std::unexpected{std::string{
                        "the game's folder was not found in Steam's libraries: start the server with --game <folder>"
                    }};
                }
                auto paks = GamePaks::open(*m_folder);
                if (!paks)
                {
                    return std::unexpected{paks.error()};
                }
                m_game = std::make_unique<OfflineGame>(std::move(*paks));
                return m_game.get();
            }

        private:
            std::optional<fs::path> m_folder;
            std::unique_ptr<OfflineGame> m_game;
        };

        [[nodiscard]] json tool_result(std::string text, json structured, bool error)
        {
            json result{{"content", json::array({{{"type", "text"}, {"text", std::move(text)}}})}, {"isError", error}};
            if (!structured.is_null())
            {
                result["structuredContent"] = std::move(structured);
            }
            return result;
        }

        [[nodiscard]] std::string argument(const json &arguments, std::string_view name)
        {
            const auto it = arguments.find(name);
            return it != arguments.end() && it->is_string() ? it->get<std::string>() : std::string{};
        }

        [[nodiscard]] std::size_t limit_of(const json &arguments)
        {
            // A signed read clamps a negative limit to 1. An unsigned read wraps it to a huge count.
            const auto it = arguments.find("limit");
            const std::int64_t limit = it != arguments.end() && it->is_number_integer()
                                           ? it->get<std::int64_t>()
                                           : static_cast<std::int64_t>(DEFAULT_LIMIT);
            return static_cast<std::size_t>(std::clamp<std::int64_t>(limit, 1, static_cast<std::int64_t>(MAX_LIMIT)));
        }

        [[nodiscard]] std::string joined(const std::vector<std::string> &lines)
        {
            std::string text;
            for (const std::string &line : lines)
            {
                text += line;
                text += '\n';
            }
            return text;
        }

        /** @brief The schema of the tool named @p name in @p tools (tool_list()), or null when there is none. */
        [[nodiscard]] const json *find_tool(const json &tools, std::string_view name)
        {
            const auto it = std::ranges::find_if(
                tools,
                [name](const json &tool) { return tool.at("name").get_ref<const std::string &>() == name; }
            );
            return it != tools.end() ? &*it : nullptr;
        }

        /** @brief The first argument @p tool requires that @p arguments lacks or leaves empty, or nothing. */
        [[nodiscard]] std::optional<std::string> missing_argument(const json &tool, const json &arguments)
        {
            const json &schema = tool.at("inputSchema");
            const auto required = schema.find("required");
            if (required == schema.end())
            {
                return std::nullopt;
            }
            // Every required property is a string: an empty one names no file, folder or clip.
            for (const json &field : *required)
            {
                const std::string &name = field.get_ref<const std::string &>();
                if (argument(arguments, name).empty())
                {
                    return name;
                }
            }
            return std::nullopt;
        }

        /**
         * @brief Why @p arguments does not fit @p tool's input schema: an argument the schema does not list, or one of
         *        another JSON type than its property's. Nothing when every argument fits.
         * @details The schemas set additionalProperties to false. A misspelled or mistyped argument, ignored, changes
         *          the result without a word.
         */
        [[nodiscard]] std::optional<std::string> invalid_argument(const json &tool, const json &arguments)
        {
            const json &properties = tool.at("inputSchema").at("properties");
            for (const auto &[name, value] : arguments.items())
            {
                const auto property = properties.find(name);
                if (property == properties.end())
                {
                    return std::format("{} is not an argument of this tool", name);
                }
                const std::string &type = property->at("type").get_ref<const std::string &>();
                const bool fits = type == "string"    ? value.is_string()
                                  : type == "boolean" ? value.is_boolean()
                                  : type == "integer" ? value.is_number_integer()
                                  : type == "number"  ? value.is_number()
                                                      : value.is_object();
                if (!fits)
                {
                    return std::format("{} must be of type {}", name, type);
                }
            }
            return std::nullopt;
        }

        /** @brief Runs the tool @p name, which tool_list() holds, on @p arguments. */
        [[nodiscard]] json call_tool(LazyGame &lazy, const std::string &name, const json &arguments)
        {
            if (name == "pack_mod" || name == "list_mod_actions")
            {
                const fs::path mod{argument(arguments, "mod_folder")};
                if (name == "list_mod_actions")
                {
                    json actions = json::array();
                    std::vector<std::string> lines;
                    for (const ModActionInfo &action : list_mod_actions(mod))
                    {
                        actions.push_back(
                            {{"fragment", action.fragment}, {"clip", action.clip}, {"loop", action.loop}}
                        );
                        lines.push_back(
                            std::format("{} plays {}{}", action.fragment, action.clip, action.loop ? " (loops)" : "")
                        );
                    }
                    return tool_result(
                        lines.empty() ? "no actions yet" : joined(lines),
                        json{{"actions", actions}},
                        false
                    );
                }
                auto packed = pack_mod_folder(mod, fs::path{argument(arguments, "install_folder")});
                return packed ? tool_result(*packed, json{{"summary", *packed}}, false)
                              : tool_result(packed.error(), nullptr, true);
            }

            auto game = lazy.get();
            if (!game)
            {
                return tool_result(game.error(), nullptr, true);
            }
            std::vector<std::string> lines;
            const ToolPrint keep = [&lines](std::string_view line) { lines.emplace_back(line); };
            if (name == "check_clip")
            {
                const ClipCheckRequest request{
                    .clip = argument(arguments, "clip"),
                    .reference = argument(arguments, "reference"),
                    .loops = arguments.value("loop", false),
                };
                const int warnings = report_clip_check((*game)->tools(), request, keep);
                return tool_result(joined(lines), json{{"warnings", warnings}, {"lines", lines}}, warnings < 0);
            }
            if (name == "find_game_fragments")
            {
                const std::vector<std::string> found =
                    (*game)->find_fragments(argument(arguments, "text"), limit_of(arguments));
                json fragments = json::array();
                for (const std::string &line : found)
                {
                    fragments.push_back({{"name", line.substr(0, line.find('\t'))}, {"line", line}});
                }
                return tool_result(joined(found), json{{"fragments", fragments}}, false);
            }
            if (name == "find_game_clips")
            {
                const std::vector<std::string> found =
                    (*game)->find_clips(argument(arguments, "text"), limit_of(arguments));
                return tool_result(joined(found), json{{"clips", found}}, false);
            }
            if (name == "make_action")
            {
                const MakeRequest request{
                    .mod_folder = fs::path{argument(arguments, "mod_folder")},
                    .fragment = argument(arguments, "fragment"),
                    .clip = argument(arguments, "clip"),
                    .like = argument(arguments, "game_fragment"),
                    .loop = arguments.value("loop", false),
                    .upper = arguments.value("upper", false),
                };
                auto made = make_action_in_folder(**game, request, keep);
                if (!made)
                {
                    lines.push_back(made.error());
                    return tool_result(joined(lines), nullptr, true);
                }
                // The printed lines hold notes too: count only the warnings.
                const auto warnings = std::ranges::count_if(
                    lines,
                    [](const std::string &line) { return line.find("WARNING") != std::string::npos; }
                );
                lines.push_back(*made);
                return tool_result(joined(lines), json{{"summary", *made}, {"warnings", warnings}}, false);
            }
            // retarget_clip or compile_clip, the remaining tools.
            CompileOptions compile;
            if (const auto loop = arguments.find("loop"); loop != arguments.end() && loop->is_boolean())
            {
                compile.loop = loop->get<bool>();
            }
            int code = 1;
            if (name == "compile_clip")
            {
                compile.additive = arguments.value("additive", false);
                compile.tracks =
                    arguments.value("overlay", false) ? TrackPolicy::DropUnchangedAtDefault : TrackPolicy::KeepAll;
                code = compile_clip_file(
                    **game,
                    fs::path{argument(arguments, "source")},
                    fs::path{argument(arguments, "target")},
                    compile,
                    keep
                );
            }
            else
            {
                RetargetRequest request;
                request.source = fs::path{argument(arguments, "source")};
                request.target = fs::path{argument(arguments, "target")};
                request.take = argument(arguments, "take");
                request.compile = compile;
                if (const auto frame = arguments.find("calibrate_frame"); frame != arguments.end())
                {
                    if (!frame->is_number_unsigned())
                    {
                        return tool_result("calibrate_frame must be a frame number, 0 or more", nullptr, true);
                    }
                    request.options.calibration_frame = frame->get<std::size_t>();
                }
                if (const std::string root = argument(arguments, "root"); !root.empty())
                {
                    const std::optional<RootMotion> motion = root_motion_named(root);
                    if (!motion)
                    {
                        return tool_result("root must be path, line or in-place", nullptr, true);
                    }
                    request.options.root = *motion;
                }
                request.options.root_turns = arguments.value("root_turns", false);
                request.options.lock_feet = arguments.value("lock_feet", false);
                request.options.from_seconds = arguments.value("from_seconds", 0.0);
                if (const auto to = arguments.find("to_seconds"); to != arguments.end() && to->is_number())
                {
                    request.options.to_seconds = to->get<double>();
                }
                if (const auto map = arguments.find("map"); map != arguments.end() && map->is_object())
                {
                    for (const auto &[bone, joint] : map->items())
                    {
                        if (!joint.is_string())
                        {
                            return tool_result(
                                std::format("map: {} must name a source joint, or be empty", bone),
                                nullptr,
                                true
                            );
                        }
                        request.options.map.emplace_back(bone, joint.get<std::string>());
                    }
                }
                if (const std::string hands = argument(arguments, "hands"); !hands.empty())
                {
                    request.hands = hands == "none" ? std::string{} : hands;
                }
                code = retarget_clip_file(**game, request, keep);
            }
            return tool_result(
                joined(lines),
                json{{"written", code != 1}, {"warnings", code == 2}, {"lines", lines}},
                code == 1
            );
        }

        void send(const json &message)
        {
            std::cout << message.dump(-1, ' ', false, json::error_handler_t::replace) << '\n';
            std::cout.flush();
        }

        void send_error(const json &id, int code, std::string_view message, json data = nullptr)
        {
            json error{{"code", code}, {"message", message}};
            if (!data.is_null())
            {
                error["data"] = std::move(data);
            }
            send(json{{"jsonrpc", "2.0"}, {"id", id}, {"error", std::move(error)}});
        }

        /**
         * @brief The id of @p message that a reply echoes: a string or an integer, else null.
         * @details JSON-RPC answers an id it cannot read with null. The serializer recurses, so an echoed deeply nested
         *          id overflows the stack.
         */
        [[nodiscard]] json reply_id(const json &message)
        {
            const auto id = message.find("id");
            return id != message.end() && (id->is_string() || id->is_number_integer()) ? *id : json{};
        }

        void handle(LazyGame &lazy, const json &message)
        {
            if (!message.is_object())
            {
                send_error(nullptr, INVALID_REQUEST, "Invalid Request: a message must be a JSON object");
                return;
            }
            const auto method_field = message.find("method");
            const auto id_field = message.find("id");
            if (method_field == message.end())
            {
                // A response, which a server does not answer, or neither a request nor a response.
                if (!message.contains("result") && !message.contains("error"))
                {
                    send_error(reply_id(message), INVALID_REQUEST, "Invalid Request: no method");
                }
                return;
            }
            if (id_field == message.end())
            {
                return; // notifications/initialized, cancellations: nothing to answer.
            }
            // An id the reply cannot echo (null, a fraction, an object) is answered with a null id, as JSON-RPC asks.
            if (!id_field->is_string() && !id_field->is_number_integer())
            {
                send_error(nullptr, INVALID_REQUEST, "Invalid Request: the id must be a string or an integer");
                return;
            }
            const json &id = *id_field;
            const auto jsonrpc = message.find("jsonrpc");
            if (jsonrpc == message.end() || *jsonrpc != "2.0" || !method_field->is_string())
            {
                send_error(
                    id,
                    INVALID_REQUEST,
                    R"(Invalid Request: a request needs "jsonrpc": "2.0" and a method name)"
                );
                return;
            }
            const std::string method = method_field->get<std::string>();
            const json params = message.value("params", json::object());
            if (!params.is_object())
            {
                send_error(id, INVALID_PARAMS, "Invalid params: params must be an object");
                return;
            }

            // A 2026-07-28 request carries its version in _meta. A request without it follows initialize.
            const json meta = params.value("_meta", json::object());
            const auto version = meta.find(std::string{META_VERSION});
            const bool modern = version != meta.end();
            if (modern && (!version->is_string() || version->get<std::string>() != MODERN_VERSION))
            {
                // Only a string version is echoed: the serializer recurses into a nested one.
                send_error(
                    id,
                    UNSUPPORTED_VERSION,
                    "Unsupported protocol version",
                    json{
                        {"supported", json::array({MODERN_VERSION})},
                        {"requested", version->is_string() ? *version : json{}}
                    }
                );
                return;
            }
            json result;
            if (method == "initialize")
            {
                const std::string requested = params.value("protocolVersion", std::string{});
                const bool known = std::ranges::find(LEGACY_VERSIONS, requested) != LEGACY_VERSIONS.end();
                result = json{
                    {"protocolVersion", known ? requested : std::string{LEGACY_VERSIONS.front()}},
                    {"capabilities", {{"tools", {{"listChanged", false}}}}},
                    {"serverInfo", server_info()},
                    {"instructions", INSTRUCTIONS},
                };
            }
            else if (method == "server/discover")
            {
                result = json{
                    {"resultType", "complete"},
                    {"supportedVersions", json::array({MODERN_VERSION})},
                    {"capabilities", {{"tools", json::object()}}},
                    {"instructions", INSTRUCTIONS},
                    {"ttlMs", LIST_TTL_MS},
                    {"cacheScope", "public"},
                };
            }
            else if (method == "ping")
            {
                result = json::object();
            }
            else if (method == "tools/list")
            {
                result = json{{"tools", tool_list()}, {"ttlMs", LIST_TTL_MS}, {"cacheScope", "public"}};
            }
            else if (method == "tools/call")
            {
                const auto name = params.find("name");
                if (name == params.end() || !name->is_string())
                {
                    send_error(id, INVALID_PARAMS, "tools/call needs the tool's name");
                    return;
                }
                const json arguments = params.value("arguments", json::object());
                if (!arguments.is_object())
                {
                    send_error(id, INVALID_PARAMS, "Invalid params: the tool's arguments must be an object");
                    return;
                }
                const std::string &tool_name = name->get_ref<const std::string &>();
                const json tools = tool_list();
                const json *tool = find_tool(tools, tool_name);
                if (tool == nullptr)
                {
                    send_error(id, INVALID_PARAMS, std::format("Unknown tool: {}", tool_name));
                    return;
                }
                // A wrong or missing argument is the tool's error, which the assistant reads and can correct.
                std::optional<std::string> problem = invalid_argument(*tool, arguments);
                if (!problem)
                {
                    if (const std::optional<std::string> missing = missing_argument(*tool, arguments))
                    {
                        problem = std::format("{} is required", *missing);
                    }
                }
                result = problem ? tool_result(*problem, nullptr, true) : call_tool(lazy, tool_name, arguments);
            }
            else
            {
                send_error(id, METHOD_NOT_FOUND, std::format("Method not found: {}", method));
                return;
            }
            if (modern)
            {
                result["resultType"] = "complete";
                result["_meta"][META_SERVER_INFO] = server_info();
            }
            send(json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
        }
    } // namespace

    int run_mcp_server(std::optional<fs::path> game_folder)
    {
        // Messages are UTF-8 lines. Text mode turns "\n" into "\r\n" on the way out.
        _setmode(_fileno(stdout), _O_BINARY);
        _setmode(_fileno(stdin), _O_BINARY);
        LazyGame lazy{std::move(game_folder)};
        std::string line;
        while (std::getline(std::cin, line))
        {
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            if (line.empty())
            {
                continue;
            }
            json message;
            try
            {
                message = json::parse(line);
            }
            catch (const json::exception &error)
            {
                // Not only parse_error: a number past double's range (1e999) throws out_of_range.
                send_error(nullptr, PARSE_ERROR, std::format("Parse error: {}", error.what()));
                continue;
            }
            try
            {
                handle(lazy, message);
            }
            catch (const json::type_error &error)
            {
                // A parameter of the wrong type, which json::value() throws on.
                if (message.is_object() && message.contains("id"))
                {
                    send_error(reply_id(message), INVALID_PARAMS, std::format("Invalid params: {}", error.what()));
                }
            }
            catch (const std::exception &error)
            {
                std::cerr << "KCD2 Animation Framework MCP: " << error.what() << '\n';
                if (message.is_object() && message.contains("id"))
                {
                    send_error(reply_id(message), INTERNAL_ERROR, std::format("Internal error: {}", error.what()));
                }
            }
        }
        return 0;
    }
} // namespace AnimationFramework
