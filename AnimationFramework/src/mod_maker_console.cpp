/**
 * @file mod_maker_console.cpp
 * @brief The mod maker's console commands (see mod_maker_console.hpp).
 */

#include "mod_maker_console.hpp"

#include "console_vars.hpp"
#include "core/mod_tools.hpp"
#include "core/text_util.hpp"
#include "core/zip_writer.hpp"
#include "hooks/anim_patch.hpp"

#include <DetourModKit.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AnimationFramework
{
    namespace
    {
        constexpr char COMMAND_FAILED[] = "ModMaker: a command failed";
        constexpr const char *MAKE_COMMAND = "animfw_make_mod";
        constexpr const char *MAKE_HELP =
            "Animation Framework: adds an action to a mod made in the DEV folder. animfw_make_mod <modid> <new "
            "fragment> <clip> <game fragment> [loop] [upper]: the new fragment plays the clip, set up like the game "
            "fragment (its body layers, blends and procedural layers), or on the upper body only with upper; the "
            "databases reload, so player.human:PlayAnim plays it at once.";
        constexpr const char *PACK_COMMAND = "animfw_pack_mod";
        constexpr const char *PACK_HELP =
            "Animation Framework: writes a mod made in the DEV folder as a finished mod, "
            "KCD2_AnimationFramework_Dev\\packed\\<modid>\\ (mod.manifest and Data\\<modid>.pak). animfw_pack_mod "
            "<modid>";

        // The framework's own folder for action table rows: the game cannot patch its tables.
        constexpr std::string_view FRAMEWORK_TABLES = "animations/mannequin/tables/";
        constexpr std::array<std::string_view, 2> TABLE_STEMS{"anim_fragment", "actor_anim_action"};
        constexpr std::string_view LOOSE_CLIP_ROOT = "animations";
        constexpr std::string_view PACKED_FOLDER = "packed";
        constexpr std::string_view LOOP_WORD = "loop";
        constexpr std::string_view UPPER_WORD = "upper";

        // Main thread only.
        bool s_added = false;

        /** @brief One line to the console and kcd.log, and to the framework's log. */
        void print(std::string_view line)
        {
            game_log_line(line);
            DMK::log().info("ModMaker: {}", line);
        }

        /// The bytes of @p file, or nothing when it cannot be opened.
        [[nodiscard]] std::optional<std::string> read_text(const std::filesystem::path &file)
        {
            std::ifstream in{file, std::ios::binary};
            if (!in)
            {
                return std::nullopt;
            }
            return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        }

        /// Replaces @p file with @p text, and reports whether every byte was written.
        [[nodiscard]] bool write_text(const std::filesystem::path &file, std::string_view text)
        {
            std::ofstream out{file, std::ios::binary | std::ios::trunc};
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            // Bytes left in the stream buffer reach the file at close, so a failed write can show only there.
            out.close();
            return !out.fail();
        }

        /** @brief The DEV folder's patch of game file @p stem.@p extension for @p modid. */
        [[nodiscard]] std::filesystem::path
        dev_patch(std::string_view stem, std::string_view modid, std::string_view extension)
        {
            return animation_dev_folder() / std::format("{}__{}.{}", stem, modid, extension);
        }

        /// The make command: adds the action's patches to the DEV folder and reloads the databases.
        void make(void *args)
        {
            std::vector<std::string> words;
            bool loop = false;
            bool upper = false;
            for (int i = 1; i < command_argument_count(args); ++i)
            {
                const std::string_view word = command_argument(args, i);
                if (lowercase(word) == LOOP_WORD)
                {
                    loop = true;
                }
                else if (lowercase(word) == UPPER_WORD)
                {
                    upper = true;
                }
                else if (!word.empty())
                {
                    words.emplace_back(word);
                }
            }
            if (words.size() != 4)
            {
                print(
                    std::format("usage: {} <modid> <new fragment> <clip> <game fragment> [loop] [upper]", MAKE_COMMAND)
                );
                return;
            }
            const ModAction action{
                .modid = words[0],
                .fragment = words[1],
                .clip = words[2],
                .loop = loop,
                .upper = upper,
            };
            const std::string &like = words[3];
            const ModPatches current{
                .fragment_ids = read_text(dev_patch(FRAGMENT_IDS_STEM, action.modid, "xml")).value_or(""),
                .controller_defs = read_text(dev_patch(CONTROLLER_DEFS_STEM, action.modid, "xml")).value_or(""),
                .database = read_text(dev_patch(DATABASE_STEM, action.modid, "adb")).value_or(""),
            };
            auto made = make_mod_action(
                running_game_tools(),
                action,
                like,
                current,
                [](std::string_view line) { print(line); }
            );
            if (!made)
            {
                print(made.error());
                if (made.error().starts_with(NO_CLIP_ERROR_PREFIX))
                {
                    print(
                        std::format(
                            "  a new clip goes under KCD2_AnimationFramework_Dev\\Animations\\humans\\male\\{}\\; run "
                            "animfw_reload_clips after adding it",
                            action.modid
                        )
                    );
                }
                return;
            }
            std::error_code error;
            std::filesystem::create_directories(animation_dev_folder(), error);
            if (!write_text(dev_patch(FRAGMENT_IDS_STEM, action.modid, "xml"), made->patches.fragment_ids) ||
                !write_text(dev_patch(CONTROLLER_DEFS_STEM, action.modid, "xml"), made->patches.controller_defs) ||
                !write_text(dev_patch(DATABASE_STEM, action.modid, "adb"), made->patches.database))
            {
                print(std::format("{}: could not write the patches to the DEV folder", action.fragment));
                return;
            }
            // An upper-body action's blend space goes in loose beside the clips, at its path in the game's data.
            for (const ZipSource &file : made->patches.files)
            {
                const std::filesystem::path target = animation_dev_folder() / std::filesystem::path{file.name};
                std::filesystem::create_directories(target.parent_path(), error);
                if (!write_text(target, file.data))
                {
                    print(std::format("{}: could not write {}", action.fragment, target.string()));
                    return;
                }
            }
            if (!made->patches.files.empty())
            {
                request_clip_reload();
            }
            print(
                std::format(
                    "{} ({}) plays {}{}, {}; its patches are in the DEV folder",
                    action.fragment,
                    action.modid,
                    action.clip,
                    action.loop ? " in a loop" : "",
                    action.upper ? std::string{"on the upper body"}
                                 : std::format("set up like {} ({})", like, made->scopes)
                )
            );
            print(
                std::format(
                    "  the databases reload now. Play it: player.human:PlayAnim(\"{}\", \"{}\"); finish the mod: {} {}",
                    action.fragment,
                    action.upper ? OWN_SCOPES_TAG : std::string_view{},
                    PACK_COMMAND,
                    action.modid
                )
            );
            run_console_command_later("mn_reload");
        }

        /** @brief The path a DEV folder patch takes in the mod's pak, or nothing when no game file has its name. */
        [[nodiscard]] std::optional<std::string> packed_patch_path(std::string_view file_name, std::string_view suffix)
        {
            const std::string name = lowercase(file_name);
            const std::size_t dot = name.rfind('.');
            const std::string stem = name.substr(0, dot);
            const std::string game_stem = stem.substr(0, stem.size() - suffix.size());
            const std::string game_name = game_stem + (dot == std::string::npos ? std::string{} : name.substr(dot));
            if (std::ranges::find(TABLE_STEMS, game_stem) != TABLE_STEMS.end())
            {
                return std::string{FRAMEWORK_TABLES} + name;
            }
            const std::optional<std::string> folder = game_file_folder(game_name);
            return folder ? std::optional{*folder + name} : std::nullopt;
        }

        /// The pack command: writes the mod's DEV folder patches and clips as a finished mod.
        void pack(void *args)
        {
            const std::string modid = command_argument_count(args) == 2 ? std::string{command_argument(args, 1)} : "";
            if (!is_mod_id(modid))
            {
                print(std::format("usage: {} <modid> (lowercase letters and underscores)", PACK_COMMAND));
                return;
            }
            const std::filesystem::path dev = animation_dev_folder();
            const std::string suffix = "__" + modid;
            std::vector<ZipSource> files;
            int patches = 0;
            int clips = 0;
            std::error_code error;
            for (const auto &entry : std::filesystem::directory_iterator{dev, error})
            {
                std::error_code file_error;
                if (!entry.is_regular_file(file_error) || !lowercase(entry.path().stem().string()).ends_with(suffix))
                {
                    continue;
                }
                const std::string file_name = entry.path().filename().string();
                const std::optional<std::string> path = packed_patch_path(file_name, suffix);
                const std::optional<std::string> text = read_text(entry.path());
                if (!path || !text)
                {
                    print(std::format("  skipped {}: no game file of that name to patch", file_name));
                    continue;
                }
                files.push_back(
                    ZipSource{
                        .name = *path,
                        .data = *text,
                    }
                );
                ++patches;
            }
            const std::filesystem::path clip_root = dev / LOOSE_CLIP_ROOT;
            using Walk = std::filesystem::recursive_directory_iterator;
            for (Walk it{clip_root, std::filesystem::directory_options::skip_permission_denied, error};
                 !error && it != Walk{};
                 it.increment(error))
            {
                std::error_code file_error;
                const std::string extension = lowercase(it->path().extension().string());
                if (!it->is_regular_file(file_error) ||
                    (extension != ".caf" && extension != ".bspace" && extension != ".comb"))
                {
                    continue;
                }
                const std::string relative = lowercase(it->path().lexically_relative(clip_root).generic_string());
                bool in_mod_folder = false;
                for (const std::filesystem::path &part : std::filesystem::path{relative}.parent_path())
                {
                    in_mod_folder = in_mod_folder || part.string() == modid;
                }
                const std::optional<std::string> data = in_mod_folder ? read_text(it->path()) : std::nullopt;
                if (data)
                {
                    files.push_back(
                        ZipSource{
                            .name = std::string{LOOSE_CLIP_ROOT} + "/" + relative,
                            .data = *data,
                        }
                    );
                    ++clips;
                }
            }
            if (files.empty())
            {
                print(
                    std::format(
                        "{}: the DEV folder holds no patch named *__{}.* and no clip in a {} folder",
                        modid,
                        modid,
                        modid
                    )
                );
                return;
            }
            const std::optional<std::string> pak = write_stored_zip(files);
            const std::filesystem::path out = dev / PACKED_FOLDER / modid;
            std::filesystem::create_directories(out / "Data", error);
            const auto today = std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now());
            if (!pak || !write_text(out / "Data" / (modid + ".pak"), *pak) ||
                !write_text(out / "mod.manifest", mod_manifest(modid, std::format("{:%Y-%m-%d}", today))))
            {
                print(std::format("{}: could not write the mod to {}", modid, out.string()));
                return;
            }
            print(std::format("{}: {} patch(es) and {} clip(s) written to {}", modid, patches, clips, out.string()));
            print(
                std::format(
                    "  install it: copy the {} folder into the game's Mods folder (or add it to your mod manager), "
                    "then delete its patches and clips from the DEV folder so they do not load twice",
                    modid
                )
            );
        }
    } // namespace

    void run_mod_maker_frame_work() noexcept
    {
        if (s_added || !console_ready())
        {
            return;
        }
        add_console_command(MAKE_COMMAND, &guarded_command<&make, COMMAND_FAILED>, MAKE_HELP);
        add_console_command(PACK_COMMAND, &guarded_command<&pack, COMMAND_FAILED>, PACK_HELP);
        s_added = true;
    }

    void remove_mod_maker_console() noexcept
    {
        if (s_added)
        {
            remove_console_command(MAKE_COMMAND);
            remove_console_command(PACK_COMMAND);
        }
        s_added = false;
    }
} // namespace AnimationFramework
