/**
 * @file tool/animfw_tool.cpp
 * @brief animfw.exe: the clip check and the mod maker without the game running, on the command
 *        line or as an MCP server for AI assistants. It reads the stock game from its paks and works on a mod's source
 *        folder in the standard KCD2 layout (`mod.manifest`, `Data\...`), which `pack` turns into an installable mod.
 */

#include "core/text_util.hpp"
#include "tool/cutscene_clone.hpp"
#include "tool/mcp_server.hpp"
#include "tool/offline_tools.hpp"
#include "tool/sandbox_layer.hpp"
#include "tool/sandbox_setup.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <exception>
#include <expected>
#include <format>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using namespace AnimationFramework;
    namespace fs = std::filesystem;

    constexpr std::string_view USAGE =
        R"(animfw: checks clips and makes KCD2 Animation Framework mods without the game running.

  animfw [--game <folder>] check <clip> [reference] [--loop]
      <clip>: a .caf file, or a game clip by name (relaxed_walk_medium_player) or path.
      [reference]: a game clip a full-body clip in its place should cover the bones of.

  animfw [--game <folder>] fragments [text]
      Lists the game's fragments whose line holds <text>, to pick one for make.

  animfw [--game <folder>] clips <text>
      Lists the game's clips whose path holds <text>.

  animfw [--game <folder>] make <mod folder> <new fragment> <clip> <game fragment>
                               [--loop] [--upper]
      Adds an action to the mod in <mod folder> (named after its mod id, made when missing): a new fragment that plays
      <clip> (a .caf file, copied into the mod, or a clip the mod or the game has), set up like <game fragment>, or with
      --upper on the upper body only, over what Henry is doing (for clips that animate the arms, such as gestures).

  animfw [--game <folder>] clone-cutscene <mod folder> <vanilla sequence> <new name> [--row <row>] [--at <x>,<y>,<z>]
      Copies one of the game's in-game cutscenes (its sequence name, such as cin_s9921k_crime__branding_kutnahora)
      into the mod under <new name>: a new level layer with new entity GUIDs and a new row of the cutscene table, so
      AnimationFramework.Cutscene plays (or you change) the copy and the original stays as it is. The row is found
      through the level's CutsceneHolder; --row names it when none does. --at moves the copy to another spot of the
      same level: the point its characters stand at lands on <x>,<y>,<z> (on the ground), cameras, lights and props
      move along, and the scene keeps its heading.

  animfw [--game <folder>] sandbox-layer <mod folder> <file.lyr> [--as <name>] [--row <row>]
      Turns a layer saved in the game's Sandbox editor (Data/Levels/<level>/Layers/...lyr in the editor's folder) into
      a scene in the mod, without exporting the level: a level layer named <name> (the file's name by default) with
      its sequence and sequence object renamed to match, and a row of the cutscene table (the vanilla cinematic's own
      when the layer is one, --row names another, else a plain row). Run it again after each save in Sandbox.

  animfw sandbox-setup [--editor <folder>] [--mod <mod folder>] [--remove]
      Adds an "AnimFW export" button to the Sandbox editor (its KCD2Mod folder, found in Steam's libraries unless
      --editor names it): select an object of your scene and click it to save the layers and run sandbox-layer for
      the scene's layer into <mod folder> (asked on the first click when not given). --remove takes it out again.
      The editor's skins stop the button (every program the editor starts crashes): use View > No Skinning.

  animfw [--game <folder>] compile <file.i_caf> <file.caf> [--loop | --no-loop] [--additive]
                               [--overlay] [--compression <n>]
      Compiles an animation exported for the Resource Compiler (.i_caf, as Blender exporters write it) into a clip
      the game plays, for Henry's skeleton, without rc.exe, then checks it. The export's own timing decides the
      length at any frame rate. Every bone the export keys is kept (a full-body clip needs them all); --overlay
      leaves out bones that never move from their default pose, as rc.exe does. Loops are detected unless given.
      --compression is rc.exe's value (10: drop keys the others give within 0.32 mm and 0.036 degrees; 0: exact only).

  animfw [--game <folder>] retarget <file.bvh|file.fbx> <file.caf> [--take <name>]
                               [--calibrate rest|<frame>] [--root path|line|in-place] [--root-turns] [--keep-facing]
                               [--no-twist] [--feet lock|free] [--from <seconds>] [--to <seconds>]
                               [--map <Henry bone>=<joint>]... [--hands <clip>|none] [--loop | --no-loop]
                               [--compression <n>] [--skeleton male|female]
      Puts an animation made on another skeleton (motion capture, Mixamo, SMPL, other games) on Henry's and compiles
      it, then checks it. Bones are matched by their names; the output lists the matches, and --map fixes one
      (--map Head=head_jnt; --map LeftShoulder= leaves Henry's bone out). The reference pose is the source's rest pose,
      or the frame --calibrate names where it stands neutral. The root follows the pelvis's smoothed path; line keeps
      one steady speed (looping locomotion), in-place keeps it still; --root-turns turns it with the pelvis's facing.
      The motion is turned to start facing Henry's forward; --keep-facing keeps the source's facing. The forearm roll
      bones take their share of the hand's twist so a turned wrist does not pinch (--no-twist leaves them still).
      --feet lock holds each planted heel and ball of the foot in place with leg IK, for sources whose feet skate
      (not for in-place clips; it costs some smoothness at the knees).
      --hands poses the fingers from a game clip when the source has none (relaxed_idle_both_player by default).
      --skeleton female makes the clip for the women's skeleton (a female NPC or double); it plays from
      Animations\humans\female\..., where the women's animation set loads clips.

  animfw actions <mod folder>
      Lists the mod's actions.

  animfw pack <mod folder> <install folder>
      Writes the mod as installed: <install folder>\mod.manifest and Data\<modid>.pak (a mod manager's mod folder,
      such as MO2's mods\My Mod\my_mod, or the game's Mods\my_mod), Data\Levels\<level>\<modid>.pak for each level
      folder of the mod's Data\Levels, and Localization\<folder>.pak for each folder of the mod's Localization.

  animfw [--game <folder>] mcp
      Serves the tools to an AI assistant over MCP (stdio).

--game is the game's folder (holding Data); the default is the Steam install.
)";

    /// Writes one line of the tool's output.
    void print(std::string_view line)
    {
        std::cout << line << '\n';
    }

    /** @brief @p folder, else the Steam install of the game, or nothing after printing why not. */
    [[nodiscard]] std::optional<fs::path> game_folder(const std::optional<fs::path> &folder)
    {
        std::optional<fs::path> game = folder ? folder : find_steam_game();
        if (!game)
        {
            print("the game's folder was not found in Steam's libraries: pass it with --game <folder>");
        }
        return game;
    }

    /** @brief The game from @p folder, else from the Steam install, or nothing after printing why not. */
    [[nodiscard]] std::optional<OfflineGame> open_game(const std::optional<fs::path> &folder)
    {
        const std::optional<fs::path> game = game_folder(folder);
        if (!game)
        {
            return std::nullopt;
        }
        auto paks = GamePaks::open(*game);
        if (!paks)
        {
            print(paks.error());
            return std::nullopt;
        }
        return std::optional<OfflineGame>{std::in_place, std::move(*paks)};
    }

    /**
     * @brief Prints the line that registers a scene clone-cutscene or sandbox-layer wrote, or why it was not written.
     * @return The command's exit code.
     */
    [[nodiscard]] int print_scene(const std::expected<std::string, std::string> &scene)
    {
        if (!scene)
        {
            print(scene.error());
            return 1;
        }
        print("Register it in your mod's Lua:");
        print(std::format("  {}", *scene));
        return 0;
    }

    /// Prints each of @p lines indented, its tabs as spaces.
    void print_lines(const std::vector<std::string> &lines)
    {
        for (std::string line : lines)
        {
            std::ranges::replace(line, '\t', ' ');
            print(std::format("  {}", line));
        }
    }

    /** @brief The command line: its options and its words (the command, then its arguments). */
    struct CommandLine
    {
        std::vector<std::string> words;
        std::optional<fs::path> game_folder;
        bool help = false;
        bool loop = false;
        bool upper = false;
        CompileOptions compile;
        RetargetRequest retarget;
        /// clone-cutscene's and sandbox-layer's --row.
        std::string row;
        /// clone-cutscene's --at.
        std::optional<ScenePoint> at;
        /// sandbox-layer's --as.
        std::string as;
        /// sandbox-setup's --editor, --mod and --remove.
        SandboxSetupRequest setup;
    };

    /// The options that take the next argument as their value.
    constexpr std::array<std::string_view, 16> VALUE_OPTIONS{
        "--game",
        "--row",
        "--at",
        "--as",
        "--editor",
        "--mod",
        "--take",
        "--calibrate",
        "--root",
        "--feet",
        "--from",
        "--to",
        "--map",
        "--hands",
        "--compression",
        "--skeleton",
    };

    /** @brief @p text as a whole number or a decimal of type T, all of it, independent of the C locale. */
    template <typename T>
        requires std::integral<T> || std::floating_point<T>
    [[nodiscard]] std::optional<T> parse_number(std::string_view text) noexcept
    {
        T value{};
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size())
        {
            return std::nullopt;
        }
        return value;
    }

    /**
     * @brief Sets the option @p option (one of VALUE_OPTIONS) to @p value in @p line.
     * @return Nothing, or why @p value does not fit the option.
     */
    [[nodiscard]] std::expected<void, std::string>
    set_value_option(std::string_view option, std::string_view value, CommandLine &line)
    {
        const auto invalid = [&](std::string_view expected) -> std::unexpected<std::string>
        { return std::unexpected{std::format("{} {}: write {}", option, value, expected)}; };
        RetargetOptions &retarget = line.retarget.options;
        if (option == "--game")
        {
            line.game_folder = fs::path{value};
        }
        else if (option == "--row")
        {
            line.row = value;
        }
        else if (option == "--at")
        {
            line.at = parse_scene_point(value);
            if (!line.at)
            {
                return invalid("three numbers with commas, such as 3150.5,612.25,41.8");
            }
        }
        else if (option == "--as")
        {
            line.as = value;
        }
        else if (option == "--editor")
        {
            line.setup.editor_root = fs::path{value};
        }
        else if (option == "--mod")
        {
            line.setup.mod_folder = fs::path{value};
        }
        else if (option == "--take")
        {
            line.retarget.take = value;
        }
        else if (option == "--calibrate")
        {
            const std::optional<std::size_t> frame = parse_number<std::size_t>(value);
            if (value != "rest" && !frame)
            {
                return invalid("rest or a frame number, 0 or more");
            }
            retarget.calibration_frame = frame;
        }
        else if (option == "--root")
        {
            const std::optional<RootMotion> root = root_motion_named(value);
            if (!root)
            {
                return invalid("path, line or in-place");
            }
            retarget.root = *root;
        }
        else if (option == "--feet")
        {
            const std::string feet = lowercase(value);
            if (feet != "lock" && feet != "free")
            {
                return invalid("lock or free");
            }
            retarget.lock_feet = feet == "lock";
        }
        else if (option == "--from" || option == "--to")
        {
            const std::optional<double> seconds = parse_number<double>(value);
            if (!seconds || !std::isfinite(*seconds) || *seconds < 0.0)
            {
                return invalid("seconds, 0 or more");
            }
            if (option == "--from")
            {
                retarget.from_seconds = *seconds;
            }
            else
            {
                retarget.to_seconds = *seconds;
            }
        }
        else if (option == "--map")
        {
            const std::size_t equals = value.find('=');
            if (equals == std::string_view::npos)
            {
                return invalid("<Henry bone>=<source joint>");
            }
            retarget.map.emplace_back(std::string{value.substr(0, equals)}, std::string{value.substr(equals + 1)});
        }
        else if (option == "--hands")
        {
            line.retarget.hands = value == "none" ? std::string{} : std::string{value};
        }
        else if (option == "--skeleton")
        {
            if (value != "male" && value != "female")
            {
                return invalid("male or female");
            }
            line.retarget.female = value == "female";
        }
        else if (option == "--compression")
        {
            const std::optional<int> compression = parse_number<int>(value);
            if (!compression || *compression < 0)
            {
                return invalid("a whole number, 0 or more");
            }
            line.compile.compression = *compression;
        }
        return {};
    }

    /** @brief Reads the command line (without the program's name), or tells what is wrong with it. */
    [[nodiscard]] std::expected<CommandLine, std::string> parse_command_line(std::span<char *const> arguments)
    {
        CommandLine line;
        for (std::size_t i = 0; i < arguments.size(); ++i)
        {
            const std::string_view argument{arguments[i]};
            if (std::ranges::find(VALUE_OPTIONS, argument) != VALUE_OPTIONS.end())
            {
                if (i + 1 == arguments.size())
                {
                    return std::unexpected{std::format("{} needs a value (--help lists the options)", argument)};
                }
                if (auto set = set_value_option(argument, arguments[++i], line); !set)
                {
                    return std::unexpected{std::move(set.error())};
                }
            }
            else if (argument == "--help" || argument == "-h" || argument == "/?")
            {
                line.help = true;
                return line;
            }
            else if (argument == "--root-turns")
            {
                line.retarget.options.root_turns = true;
            }
            else if (argument == "--keep-facing")
            {
                line.retarget.options.keep_facing = true;
            }
            else if (argument == "--no-twist")
            {
                line.retarget.options.share_twist = false;
            }
            else if (argument == "--remove")
            {
                line.setup.remove = true;
            }
            else if (argument == "--loop")
            {
                line.loop = true;
                line.compile.loop = true;
            }
            else if (argument == "--no-loop")
            {
                line.compile.loop = false;
            }
            else if (argument == "--additive")
            {
                line.compile.additive = true;
            }
            else if (argument == "--overlay")
            {
                line.compile.tracks = TrackPolicy::DropUnchangedAtDefault;
            }
            else if (argument == "--upper")
            {
                line.upper = true;
            }
            else if (argument.starts_with("--"))
            {
                return std::unexpected{std::format("unknown option {} (--help lists the options)", argument)};
            }
            else
            {
                line.words.emplace_back(argument);
            }
        }
        return line;
    }

    /**
     * @brief Runs the command in @p arguments (without the program's name).
     * @return The process exit code: 0 on success, 2 for a clip written or checked with warnings, 1 on failure.
     */
    [[nodiscard]] int run(std::span<char *const> arguments)
    {
        auto parsed = parse_command_line(arguments);
        if (!parsed)
        {
            print(parsed.error());
            return 1;
        }
        CommandLine &line = *parsed;
        if (line.help)
        {
            print(USAGE);
            return 0;
        }
        if (line.words.empty())
        {
            print(USAGE);
            return 1;
        }
        const std::string command = lowercase(line.words.front());
        const std::span<const std::string> rest = std::span{line.words}.subspan(1);

        if (command == "mcp")
        {
            return run_mcp_server(line.game_folder);
        }
        if (command == "compile" && rest.size() == 2)
        {
            std::optional<OfflineGame> game = open_game(line.game_folder);
            if (!game)
            {
                return 1;
            }
            return compile_clip_file(*game, fs::path{rest[0]}, fs::path{rest[1]}, line.compile, print);
        }
        if (command == "retarget" && rest.size() == 2)
        {
            std::optional<OfflineGame> game = open_game(line.game_folder);
            if (!game)
            {
                return 1;
            }
            line.retarget.source = fs::path{rest[0]};
            line.retarget.target = fs::path{rest[1]};
            line.retarget.compile = line.compile;
            return retarget_clip_file(*game, line.retarget, print);
        }
        if (command == "clone-cutscene" && rest.size() == 3)
        {
            const std::optional<fs::path> root = game_folder(line.game_folder);
            if (!root)
            {
                return 1;
            }
            const CutsceneCloneRequest request{
                .game_root = *root,
                .mod_folder = fs::path{rest[0]},
                .sequence = rest[1],
                .new_name = rest[2],
                .row = line.row,
                .at = line.at,
            };
            return print_scene(clone_cutscene(request, print));
        }
        if (command == "sandbox-layer" && rest.size() == 2)
        {
            const std::optional<fs::path> root = game_folder(line.game_folder);
            if (!root)
            {
                return 1;
            }
            const SandboxLayerRequest request{
                .game_root = *root,
                .mod_folder = fs::path{rest[0]},
                .layer_file = fs::path{rest[1]},
                .name = line.as,
                .row = line.row,
            };
            return print_scene(convert_sandbox_layer(request, print));
        }
        if (command == "sandbox-setup" && rest.empty())
        {
            const auto set_up = setup_sandbox(line.setup, print);
            if (!set_up)
            {
                print(set_up.error());
                return 1;
            }
            return 0;
        }
        // The editor button's helper: closes the editor's "No modified layers found" note (see sandbox_setup.hpp).
        if (command == "sandbox-note" && rest.size() == 1)
        {
            const std::optional<unsigned long> pid = parse_number<unsigned long>(rest[0]);
            return pid && close_sandbox_note(*pid) ? 0 : 1;
        }
        if (command == "pack" && rest.size() == 2)
        {
            auto packed = pack_mod_folder(fs::path{rest[0]}, fs::path{rest[1]});
            print(packed ? *packed : packed.error());
            return packed ? 0 : 1;
        }
        if (command == "actions" && rest.size() == 1)
        {
            const std::vector<ModActionInfo> actions = list_mod_actions(fs::path{rest[0]});
            for (const ModActionInfo &action : actions)
            {
                print(std::format("  {} plays {}{}", action.fragment, action.clip, action.loop ? " (loops)" : ""));
            }
            print(std::format("{} action(s)", actions.size()));
            return 0;
        }

        const bool check = command == "check" && !rest.empty() && rest.size() <= 2;
        const bool fragments = command == "fragments" && rest.size() <= 1;
        const bool clips = command == "clips" && rest.size() == 1;
        const bool make = command == "make" && rest.size() == 4;
        if (!check && !fragments && !clips && !make)
        {
            print(USAGE);
            return 1;
        }
        std::optional<OfflineGame> game = open_game(line.game_folder);
        if (!game)
        {
            return 1;
        }
        if (check)
        {
            const ClipCheckRequest request{
                .clip = rest[0],
                .reference = rest.size() > 1 ? rest[1] : std::string{},
                .loops = line.loop,
            };
            const int warnings = report_clip_check(game->tools(), request, print);
            return warnings < 0 ? 1 : warnings > 0 ? 2 : 0;
        }
        if (fragments || clips)
        {
            const std::string text = rest.empty() ? std::string{} : rest[0];
            const std::vector<std::string> lines =
                fragments ? game->find_fragments(text, SIZE_MAX) : game->find_clips(text, SIZE_MAX);
            print_lines(lines);
            print(std::format("{} {}", lines.size(), fragments ? "fragment(s)" : "clip(s)"));
            return 0;
        }
        const MakeRequest request{
            .mod_folder = fs::path{rest[0]},
            .fragment = rest[1],
            .clip = rest[2],
            .like = rest[3],
            .loop = line.loop,
            .upper = line.upper,
        };
        auto made = make_action_in_folder(*game, request, print);
        print(made ? *made : made.error());
        if (made)
        {
            print(std::format("  install: animfw pack \"{}\" <install folder>", rest[0]));
        }
        return made ? 0 : 1;
    }
} // namespace

int main(int argc, char **argv)
{
    // An escaped exception (an allocation failure, a filesystem error) ends the tool with exit code 1 instead of a
    // crash. Its message goes to stderr, so the MCP server's stdout holds only protocol messages.
    try
    {
        return run(std::span<char *const>{argv, static_cast<std::size_t>(argc)}.subspan(1));
    }
    catch (const std::exception &error)
    {
        std::cerr << "animfw: " << error.what() << '\n';
        return 1;
    }
}
