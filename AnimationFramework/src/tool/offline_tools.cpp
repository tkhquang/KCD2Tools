/**
 * @file tool/offline_tools.cpp
 * @brief The tools that run without the game (see offline_tools.hpp).
 */

#include "tool/offline_tools.hpp"

#include "core/clip_compiler.hpp"
#include "core/mod_maker.hpp"
#include "core/text_util.hpp"
#include "core/zip_writer.hpp"
#include "retarget/retarget.hpp"

#include <windows.h>

#include <array>
#include <chrono>
#include <format>
#include <fstream>
#include <iterator>
#include <numbers>
#include <regex>
#include <span>

namespace AnimationFramework
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr std::string_view PATCH_FOLDER = "Data/Animations/Mannequin/ADB";
        constexpr std::string_view CLIP_FOLDER = "Data/Animations/humans/male";

        [[nodiscard]] std::optional<std::string> read_text(const fs::path &file)
        {
            std::ifstream in{file, std::ios::binary};
            if (!in)
            {
                return std::nullopt;
            }
            return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        }

        [[nodiscard]] bool write_text(const fs::path &file, std::string_view text)
        {
            std::error_code error;
            fs::create_directories(file.parent_path(), error);
            std::ofstream out{file, std::ios::binary | std::ios::trunc};
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            return out.good();
        }

        [[nodiscard]] std::string today()
        {
            return std::format("{:%Y-%m-%d}", std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now()));
        }

        [[nodiscard]] fs::path
        patch_file(const fs::path &mod, std::string_view modid, std::string_view stem, std::string_view extension)
        {
            return mod / PATCH_FOLDER / std::format("{}__{}.{}", stem, modid, extension);
        }

        [[nodiscard]] bool contains_ignoring_case(std::string_view text, std::string_view wanted)
        {
            return wanted.empty() || lowercase(text).find(lowercase(wanted)) != std::string::npos;
        }

        [[nodiscard]] std::unexpected<std::string> bad_mod_id(std::string_view modid)
        {
            // The game skips a mod whose manifest mod id holds anything else.
            return std::unexpected{
                std::format("{}: name the mod folder after its mod id (lowercase letters and underscores)", modid)
            };
        }

        /** @brief A clip file's path as the game names it: from its `animations/` folder on, else its file name. */
        [[nodiscard]] std::string clip_game_path(const fs::path &file)
        {
            const std::string generic = lowercase(file.generic_string());
            const std::size_t at = generic.rfind("/animations/");
            return at != std::string::npos ? generic.substr(at + 1) : lowercase(file.filename().string());
        }

        /** @brief Whether the relative path @p relative stays inside the folder it is joined to. */
        [[nodiscard]] bool stays_inside(const fs::path &relative)
        {
            // A root directory alone ("\x") also escapes: Windows joins it onto the folder's drive.
            const fs::path normal = relative.lexically_normal();
            return !normal.empty() && !normal.has_root_path() && *normal.begin() != fs::path{".."};
        }

        /**
         * @brief Whether @p target names a `.caf` file, else prints why it is refused.
         * @details The compile overwrites its target, so a mistyped path never replaces a file of another kind.
         */
        [[nodiscard]] bool clip_target_valid(const fs::path &target, const ToolPrint &print)
        {
            if (lowercase(target.extension().string()) == ".caf")
            {
                return true;
            }
            print(std::format("{}: the clip to write must be a .caf file", target.string()));
            return false;
        }
    } // namespace

    std::optional<fs::path> find_steam_game()
    {
        std::array<wchar_t, MAX_PATH> buffer{};
        DWORD size = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
        if (RegGetValueW(
                HKEY_CURRENT_USER,
                L"Software\\Valve\\Steam",
                L"SteamPath",
                RRF_RT_REG_SZ,
                nullptr,
                buffer.data(),
                &size
            ) != ERROR_SUCCESS)
        {
            return std::nullopt;
        }
        const fs::path steam{buffer.data()};
        std::vector<fs::path> libraries{steam};
        if (const std::optional<std::string> vdf = read_text(steam / "steamapps" / "libraryfolders.vdf"))
        {
            static const std::regex PATH_LINE{R"re("path"\s+"([^"]+)")re"};
            for (std::sregex_iterator it{vdf->begin(), vdf->end(), PATH_LINE}, end; it != end; ++it)
            {
                const std::string path = (*it)[1].str();
                std::string unescaped;
                for (std::size_t i = 0; i < path.size(); ++i)
                {
                    unescaped += path[i];
                    if (path[i] == '\\' && i + 1 < path.size() && path[i + 1] == '\\')
                    {
                        ++i;
                    }
                }
                libraries.emplace_back(unescaped);
            }
        }
        for (const fs::path &library : libraries)
        {
            const fs::path game = library / "steamapps" / "common" / "KingdomComeDeliverance2";
            std::error_code error;
            if (fs::exists(game / "Data" / "Animations.pak", error))
            {
                return game;
            }
        }
        return std::nullopt;
    }

    std::expected<FoundClip, std::string> read_clip_file(const fs::path &file)
    {
        const std::optional<std::string> data = read_text(file);
        if (!data)
        {
            return std::unexpected{std::format("{} does not read", file.string())};
        }
        auto clip = read_caf_file(std::as_bytes(std::span{*data}));
        if (!clip)
        {
            return std::unexpected{std::format("{}: {}", file.string(), clip.error())};
        }
        std::string path = clip_game_path(file);
        clip->name = path;
        return FoundClip{.clip = std::move(*clip), .path = std::move(path), .source = file.string()};
    }

    bool is_clip_file(std::string_view text)
    {
        std::error_code error;
        return lowercase(text).ends_with(".caf") && fs::is_regular_file(fs::path{text}, error);
    }

    std::string mod_id_of(const fs::path &mod)
    {
        if (const std::optional<std::string> manifest = read_text(mod / "mod.manifest"))
        {
            static const std::regex MODID{R"(<modid>\s*([^<\s]+)\s*</modid>)"};
            std::smatch match;
            if (std::regex_search(*manifest, match, MODID))
            {
                return match[1].str();
            }
        }
        // The full path names the folder also when it is given as "." or with a trailing separator.
        std::error_code error;
        fs::path full = fs::absolute(mod, error).lexically_normal();
        if (!full.has_filename())
        {
            full = full.parent_path();
        }
        return full.filename().string();
    }

    std::optional<RootMotion> root_motion_named(std::string_view name)
    {
        const std::string lower = lowercase(name);
        if (lower == "path")
        {
            return RootMotion::Path;
        }
        if (lower == "line")
        {
            return RootMotion::Line;
        }
        if (lower == "in-place")
        {
            return RootMotion::InPlace;
        }
        return std::nullopt;
    }

    OfflineGame::OfflineGame(GamePaks paks) : m_paks(std::move(paks)) {}

    ModToolGame OfflineGame::tools(std::optional<fs::path> mod_folder)
    {
        return ModToolGame{
            .files =
                AnimCatalogFiles{
                    .read = [this](const std::string &path) { return m_paks.read(path); },
                    .patches = [](const std::string &) { return std::vector<std::string>{}; },
                },
            .find_clip = [this, mod_folder](std::string_view name) { return find_clip(name, mod_folder); },
            .skeleton = [this]() { return skeleton(); },
        };
    }

    std::expected<FoundClip, std::string>
    OfflineGame::find_clip(std::string_view name, const std::optional<fs::path> &mod_folder)
    {
        if (is_clip_file(name))
        {
            return read_clip_file(fs::path{name});
        }
        if (mod_folder)
        {
            const fs::path clips = *mod_folder / "Data" / "Animations";
            std::error_code error;
            using Walk = fs::recursive_directory_iterator;
            for (Walk it{clips, fs::directory_options::skip_permission_denied, error}; !error && it != Walk{};
                 it.increment(error))
            {
                if (lowercase(it->path().extension().string()) == ".caf" &&
                    clip_stem(it->path().filename().string()) == lowercase(name))
                {
                    return read_clip_file(it->path());
                }
            }
        }
        return m_paks.find_clip(name);
    }

    const std::vector<SkeletonBone> *OfflineGame::skeleton()
    {
        if (!m_skeleton_read)
        {
            m_skeleton_read = true;
            if (auto bones = m_paks.male_skeleton())
            {
                m_skeleton = std::move(*bones);
            }
        }
        return m_skeleton ? &*m_skeleton : nullptr;
    }

    std::vector<std::string> OfflineGame::find_fragments(std::string_view text, std::size_t limit)
    {
        if (!m_catalog)
        {
            const AnimCatalogFiles files{
                .read = [this](const std::string &path) { return m_paks.read(path); },
                .patches = [](const std::string &) { return std::vector<std::string>{}; },
            };
            m_catalog = std::make_unique<AnimCatalog>(files);
        }
        const AnimCatalogPage page = m_catalog->find(AnimCatalogQuery{.text = text, .count = limit}, {});
        std::vector<std::string> lines;
        std::string_view rest = page.lines;
        while (!rest.empty())
        {
            const std::size_t end = rest.find('\n');
            lines.emplace_back(rest.substr(0, end));
            rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
        }
        return lines;
    }

    std::vector<std::string> OfflineGame::find_clips(std::string_view text, std::size_t limit) const
    {
        std::vector<std::string> found;
        for (const std::string &path : m_paks.clip_paths())
        {
            if (found.size() >= limit)
            {
                break;
            }
            if (contains_ignoring_case(path, text))
            {
                found.push_back(path);
            }
        }
        return found;
    }

    std::expected<std::string, std::string>
    make_action_in_folder(OfflineGame &game, const MakeRequest &request, const ToolPrint &print)
    {
        const fs::path &mod = request.mod_folder;
        const std::string modid = mod_id_of(mod);
        if (!is_mod_id(modid))
        {
            return bad_mod_id(modid);
        }
        std::string clip = request.clip;
        ModToolGame tools = game.tools(mod);
        // A clip file from outside the mod is copied in only once the action is accepted, so a refused action leaves
        // the mod folder as it was. Until then the action reads it where it is, under the path the copy gets.
        std::optional<fs::path> copy_source;
        fs::path copy_target;
        if (is_clip_file(clip))
        {
            const fs::path source{clip};
            auto decoded = read_clip_file(source);
            if (!decoded)
            {
                return std::unexpected{decoded.error()};
            }
            copy_target = mod / CLIP_FOLDER / modid / lowercase(source.filename().string());
            clip = clip_stem(source.filename().string());
            std::error_code error;
            if (!fs::equivalent(source, copy_target, error))
            {
                copy_source = source;
                decoded->path = clip_game_path(copy_target);
                decoded->clip.name = decoded->path;
                tools.find_clip = [inner = std::move(tools.find_clip), stem = clip, found = std::move(*decoded)](
                                      std::string_view name
                                  ) -> std::expected<FoundClip, std::string>
                { return lowercase(name) == stem ? std::expected<FoundClip, std::string>{found} : inner(name); };
            }
        }

        const ModAction action{
            .modid = modid,
            .fragment = request.fragment,
            .clip = clip,
            .loop = request.loop,
            .upper = request.upper,
        };
        const ModPatches current{
            .fragment_ids = read_text(patch_file(mod, modid, FRAGMENT_IDS_STEM, "xml")).value_or(""),
            .controller_defs = read_text(patch_file(mod, modid, CONTROLLER_DEFS_STEM, "xml")).value_or(""),
            .database = read_text(patch_file(mod, modid, DATABASE_STEM, "adb")).value_or(""),
        };
        auto made = make_mod_action(tools, action, request.like, current, print);
        if (!made)
        {
            return std::unexpected{made.error()};
        }
        // The file names derive from the clip's name. One that climbs out of Data refuses the action before any write.
        for (const ZipSource &file : made->patches.files)
        {
            if (!stays_inside(fs::path{file.name}))
            {
                return std::unexpected{std::format("{} leaves the mod's Data folder", file.name)};
            }
        }
        std::error_code error;
        if (copy_source)
        {
            fs::create_directories(copy_target.parent_path(), error);
            fs::copy_file(*copy_source, copy_target, fs::copy_options::overwrite_existing, error);
            if (error)
            {
                return std::unexpected{std::format(
                    "could not copy {} to {}: {}",
                    copy_source->string(),
                    copy_target.string(),
                    error.message()
                )};
            }
        }
        const bool manifest_ok =
            fs::exists(mod / "mod.manifest", error) || write_text(mod / "mod.manifest", mod_manifest(modid, today()));
        if (!manifest_ok || !write_text(patch_file(mod, modid, FRAGMENT_IDS_STEM, "xml"), made->patches.fragment_ids) ||
            !write_text(patch_file(mod, modid, CONTROLLER_DEFS_STEM, "xml"), made->patches.controller_defs) ||
            !write_text(patch_file(mod, modid, DATABASE_STEM, "adb"), made->patches.database))
        {
            return std::unexpected{std::format("could not write the mod to {}", mod.string())};
        }
        for (const ZipSource &file : made->patches.files)
        {
            if (!write_text(mod / "Data" / fs::path{file.name}, file.data))
            {
                return std::unexpected{std::format("could not write {} into {}", file.name, mod.string())};
            }
        }
        return std::format(
            "{} ({}) plays {}{}, {}. In game: player.human:PlayAnim(\"{}\", \"{}\")",
            action.fragment,
            modid,
            action.clip,
            action.loop ? " in a loop" : "",
            action.upper ? std::string{"on the upper body"}
                         : std::format("set up like {} ({})", request.like, made->scopes),
            action.fragment,
            action.upper ? OWN_SCOPES_TAG : std::string_view{}
        );
    }

    std::expected<std::string, std::string> pack_mod_folder(const fs::path &mod, const fs::path &install)
    {
        const std::string modid = mod_id_of(mod);
        if (!is_mod_id(modid))
        {
            return bad_mod_id(modid);
        }
        const fs::path data = mod / "Data";
        std::error_code error;
        // The trailing slashes keep a sibling such as Data2 from matching Data. A path that does not resolve fails the
        // pack: its empty result passes the check.
        const std::string install_full = lowercase(fs::weakly_canonical(install, error).generic_string()) + '/';
        if (error)
        {
            return std::unexpected{std::format("{}: {}", install.string(), error.message())};
        }
        const std::string data_full = lowercase(fs::weakly_canonical(data, error).generic_string()) + '/';
        if (error)
        {
            return std::unexpected{std::format("{}: {}", data.string(), error.message())};
        }
        if (install_full.starts_with(data_full))
        {
            return std::unexpected{std::string{"the install folder cannot be inside the mod's Data folder"}};
        }
        std::vector<ZipSource> files;
        using Walk = fs::recursive_directory_iterator;
        for (Walk it{data, fs::directory_options::skip_permission_denied, error}; !error && it != Walk{};
             it.increment(error))
        {
            std::error_code file_error;
            if (!it->is_regular_file(file_error))
            {
                continue;
            }
            const std::string relative = it->path().lexically_relative(data).generic_string();
            // As kcd-pak-action packs a mod: level data goes into the level's own paks, and paks are not nested.
            if (lowercase(relative).starts_with("levels/") || lowercase(it->path().extension().string()) == ".pak")
            {
                continue;
            }
            const std::optional<std::string> bytes = read_text(it->path());
            if (!bytes)
            {
                return std::unexpected{std::format("could not read {}", it->path().string())};
            }
            files.push_back(ZipSource{.name = relative, .data = *bytes});
        }
        if (error)
        {
            // A pack of the files the walk reached before its failure ships a mod with files missing.
            return std::unexpected{std::format("could not list {}: {}", data.string(), error.message())};
        }
        if (files.empty())
        {
            return std::unexpected{std::format("{} holds no files to pack", data.string())};
        }
        const std::optional<std::string> pak = write_stored_zip(files);
        const std::string manifest = read_text(mod / "mod.manifest").value_or(mod_manifest(modid, today()));
        const fs::path pak_file = install / "Data" / (modid + ".pak");
        if (!pak || !write_text(pak_file, *pak) || !write_text(install / "mod.manifest", manifest))
        {
            return std::unexpected{std::format("could not write the mod to {}", install.string())};
        }
        return std::format("{}: {} file(s) packed into {}", modid, files.size(), pak_file.string());
    }

    std::vector<ModActionInfo> list_mod_actions(const fs::path &mod)
    {
        std::vector<ModActionInfo> actions;
        const std::string modid = mod_id_of(mod);
        const std::optional<std::string> text = read_text(patch_file(mod, modid, DATABASE_STEM, "adb"));
        const std::optional<XmlElement> database = text ? read_xml_elements(*text) : std::nullopt;
        const XmlElement *list = database ? database->child("FragmentList") : nullptr;
        if (list == nullptr)
        {
            return actions;
        }
        for (const XmlElement &fragment : list->children)
        {
            const XmlElement *option = fragment.children.empty() ? nullptr : &fragment.children.front();
            ModActionInfo info{.fragment = fragment.tag};
            if (option != nullptr)
            {
                info.clip = first_clip(*option);
                if (const XmlElement *layer = option->child("AnimLayer"))
                {
                    for (const XmlElement &step : layer->children)
                    {
                        info.loop = info.loop ||
                                    (step.tag == "Animation" && step.get("flags").find("Loop") != std::string::npos);
                    }
                }
            }
            actions.push_back(std::move(info));
        }
        return actions;
    }

    int compile_clip_file(
        OfflineGame &game,
        const fs::path &source,
        const fs::path &target,
        const CompileOptions &options,
        const ToolPrint &print
    )
    {
        if (!clip_target_valid(target, print))
        {
            return 1;
        }
        const std::vector<SkeletonBone> *skeleton = game.skeleton();
        if (skeleton == nullptr)
        {
            print("Henry's skeleton could not be read from the game's paks");
            return 1;
        }
        const std::optional<std::string> text = read_text(source);
        if (!text)
        {
            print(std::format("{} could not be read", source.string()));
            return 1;
        }
        const auto bytes = std::as_bytes(std::span{text->data(), text->size()});
        auto sampled = read_intermediate_clip(bytes, *skeleton);
        if (!sampled)
        {
            print(std::format("{}: {}", source.string(), sampled.error()));
            return 1;
        }
        return compile_sampled_clip(game, *sampled, source, target, options, print);
    }

    int retarget_clip_file(OfflineGame &game, const RetargetRequest &request, const ToolPrint &print)
    {
        if (!clip_target_valid(request.target, print))
        {
            return 1;
        }
        const std::vector<SkeletonBone> *skeleton = game.skeleton();
        if (skeleton == nullptr)
        {
            print("Henry's skeleton could not be read from the game's paks");
            return 1;
        }
        auto source = read_mocap_file(request.source, request.take);
        if (!source)
        {
            print(std::format("{}: {}", request.source.string(), source.error()));
            return 1;
        }
        std::optional<FoundClip> hands;
        if (!request.hands.empty())
        {
            auto found = game.find_clip(request.hands);
            if (!found)
            {
                print(std::format("the hands clip {}: {}", request.hands, found.error()));
                return 1;
            }
            hands = std::move(*found);
        }
        auto result = retarget(*source, *skeleton, request.options, hands ? &hands->clip : nullptr);
        if (!result)
        {
            print(std::format("{}: {}", request.source.string(), result.error()));
            print(
                std::format(
                    "  its joints: {}",
                    [&]() -> std::string
                    {
                        std::string names;
                        for (const MocapJoint &joint : source->joints)
                        {
                            names += (names.empty() ? "" : ", ") + joint.name;
                        }
                        return names;
                    }()
                )
            );
            return 1;
        }
        const RetargetReport &report = result->report;
        print(
            std::format(
                "{} (take {}, {:.0f} frames a second, up {}): {:.3f} s retargeted, scale {:.3f}",
                request.source.string(),
                source->take,
                report.source_fps,
                report.up_axis,
                report.seconds,
                report.scale
            )
        );
        if (source->takes.size() > 1)
        {
            std::string takes;
            for (const std::string &take : source->takes)
            {
                takes += (takes.empty() ? "" : ", ") + take;
            }
            print(std::format("  takes in the file (--take <name>): {}", takes));
        }
        for (const auto &[bone, from] : report.mapping)
        {
            print(std::format("  {} <- {}", bone, from));
        }
        for (const std::string &note : report.notes)
        {
            print(std::format("  note: {}", note));
        }
        print(
            std::format(
                "  feet: left slides {:.2f} m/s planted and sits {:+.1f} cm at its lowest, right {:.2f} m/s and "
                "{:+.1f} cm{}",
                report.left_foot.planted_slide,
                report.left_foot.lowest * 100.0,
                report.right_foot.planted_slide,
                report.right_foot.lowest * 100.0,
                report.locked_frames > 0 ? std::format(" (planted feet held in {} leg frame(s))", report.locked_frames)
                                         : std::string{}
            )
        );
        return compile_sampled_clip(game, result->clip, request.source, request.target, request.compile, print);
    }

    int compile_sampled_clip(
        OfflineGame &game,
        const SampledClip &sampled,
        const fs::path &source,
        const fs::path &target,
        const CompileOptions &options,
        const ToolPrint &print
    )
    {
        if (!clip_target_valid(target, print))
        {
            return 1;
        }
        const std::vector<SkeletonBone> *skeleton = game.skeleton();
        if (skeleton == nullptr)
        {
            print("Henry's skeleton could not be read from the game's paks");
            return 1;
        }
        auto compiled = compile_clip(sampled, *skeleton, options);
        if (!compiled)
        {
            print(std::format("{}: {}", source.string(), compiled.error()));
            return 1;
        }
        const CompileReport &report = compiled->report;
        if (!write_text(
                target,
                std::string_view{reinterpret_cast<const char *>(compiled->file.data()), compiled->file.size()}
            ))
        {
            print(std::format("{} could not be written", target.string()));
            return 1;
        }
        print(
            std::format(
                "{}: {} frames ({:.3f} s){}{}, {} track(s), {} left out (unchanged at the default pose){}, keys {} -> "
                "{}, {} bytes",
                target.string(),
                report.frames,
                static_cast<double>(report.frames > 0 ? report.frames - 1 : 0) / CLIP_FRAME_RATE,
                report.loop ? ", loops" : "",
                report.additive ? ", additive" : "",
                report.tracks,
                report.dropped_tracks,
                sampled.unknown_tracks + report.unknown_bones > 0
                    ? std::format(", {} not bones of Henry's skeleton", sampled.unknown_tracks + report.unknown_bones)
                    : std::string{},
                report.keys_in,
                report.keys_out,
                report.bytes
            )
        );
        if (report.distance > 0.0 || report.turn != 0.0)
        {
            print(
                std::format(
                    "  the root travels {:.2f} m ({:.2f} m/s) and turns {:.1f} degrees",
                    report.distance,
                    report.speed,
                    report.turn * 180.0 / std::numbers::pi
                )
            );
        }
        // A check that cannot read the written clip (-1) counts as a warning: the clip is written but unchecked.
        const int warnings =
            report_clip_check(game.tools(), ClipCheckRequest{.clip = target.string(), .loops = report.loop}, print);
        return warnings != 0 ? 2 : 0;
    }
} // namespace AnimationFramework
