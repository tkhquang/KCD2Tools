/**
 * @file tool/studio/studio_main.cpp
 * @brief animfw-studio.exe (AnimFW Studio): the clip check and the mod maker in a window. The clip plays on a
 *        mannequin of the game's own skeleton, the reference clip beside it, and the bones a check flags show in
 *        red.
 */

#include "core/clip_check.hpp"
#include "core/text_util.hpp"
#include "tool/offline_tools.hpp"
#include "tool/studio/file_dialogs.hpp"
#include "tool/studio/mannequin.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <raylib.h>
#include <raymath.h>
#include <rlImGui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    using namespace AnimationFramework;
    using namespace AnimationFramework::studio;
    namespace fs = std::filesystem;

    constexpr int WINDOW_WIDTH = 1440;
    constexpr int WINDOW_HEIGHT = 900;
    constexpr float SIDEBAR_WIDTH = 470.0F;
    constexpr float TRANSPORT_HEIGHT = 92.0F;
    constexpr float REFERENCE_OFFSET = -1.1F;
    constexpr std::size_t SEARCH_LIMIT = 300;
    constexpr const char *SETTINGS_FILE = "animfw-studio.ini";

    constexpr Color BODY{226, 196, 156, 255};
    constexpr Color REFERENCE{120, 160, 220, 150};
    constexpr Color FLAGGED{225, 70, 60, 255};
    constexpr Color SLOT{255, 160, 40, 255};
    constexpr Color BACKGROUND{38, 41, 48, 255};
    constexpr ImVec4 WARNING_TEXT{1.0F, 0.45F, 0.35F, 1.0F};
    constexpr ImVec4 DIM_TEXT{0.65F, 0.68F, 0.72F, 1.0F};

    /** @brief The folders the window remembers, beside the exe. */
    struct Settings
    {
        std::string game;
        std::string mod;
        std::string install;
    };

    [[nodiscard]] fs::path settings_path()
    {
        return fs::path{GetApplicationDirectory()} / SETTINGS_FILE;
    }

    [[nodiscard]] Settings load_settings()
    {
        Settings settings;
        std::ifstream in{settings_path()};
        std::string line;
        while (std::getline(in, line))
        {
            const std::size_t equals = line.find('=');
            if (equals == std::string::npos)
            {
                continue;
            }
            const std::string key = line.substr(0, equals);
            std::string value = line.substr(equals + 1);
            if (key == "game")
            {
                settings.game = std::move(value);
            }
            else if (key == "mod")
            {
                settings.mod = std::move(value);
            }
            else if (key == "install")
            {
                settings.install = std::move(value);
            }
        }
        return settings;
    }

    void save_settings(const Settings &settings)
    {
        std::ofstream out{settings_path(), std::ios::trunc};
        out << "game=" << settings.game << "\nmod=" << settings.mod << "\ninstall=" << settings.install << '\n';
    }

    /** @brief A clip on the mannequin: its keys, how it poses the skeleton, and the bones to flag. */
    struct PreviewClip
    {
        FoundClip found;
        std::unique_ptr<ClipPoser> poser;
        std::vector<std::uint8_t> flagged;
        std::vector<std::size_t> slot_bones;
    };

    /** @brief A line of a tool's output, shown in the warning color when it is a warning. */
    struct ReportLine
    {
        bool warning = false;
        std::string text;
    };

    /** @brief @p line of a tool's output as a ReportLine: a warning when it says WARNING. */
    [[nodiscard]] ReportLine report_line(std::string_view line)
    {
        return ReportLine{.warning = line.find("WARNING") != std::string_view::npos, .text = std::string{line}};
    }

    /** @brief Draws @p lines in the bordered child @p id, which fills the rest of the panel. */
    void draw_log(const char *id, const std::vector<ReportLine> &lines)
    {
        ImGui::BeginChild(id, ImVec2{0, 0}, ImGuiChildFlags_Borders);
        for (const ReportLine &line : lines)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, line.warning ? WARNING_TEXT : ImGui::GetStyleColorVec4(ImGuiCol_Text));
            ImGui::TextWrapped("%s", line.text.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();
    }

    /** @brief `--render`: a clip drawn on the mannequin, saved as one PNG per frame instead of shown in a window. */
    struct RenderRequest
    {
        std::string clip;
        fs::path out;
        int width = 480;
        int height = 640;
        double from = 0.0;    ///< Seconds into the clip.
        double to = -1.0;     ///< Seconds into the clip; below zero: its end.
        int every = 1;        ///< Saves every n-th frame.
        float yaw = 1.25F;    ///< The camera's turn around Henry, in radians.
        float pitch = 0.18F;  ///< The camera's height angle, in radians.
        float distance = 3.0F;
        /// A clip played over it on a layer, as a HoldItem overlay plays: the bones it keys follow it instead.
        std::string overlay;
    };

    /** @brief The Studio window: the game, the mod, the tabs and the mannequin viewport. */
    class Studio
    {
    public:
        /**
         * @brief Renders @p request's clip on the mannequin in a hidden window, one PNG per frame
         *        (`frame_0000.png`, ...) in its out folder. Returns the process exit code.
         */
        int render(const RenderRequest &request);
        /**
         * @brief Lays the clip @p name over the loaded clip, as a layer in the game would: every bone it keys takes
         *        its keys, repeated over the loaded clip's length when it is shorter. False when it is not found.
         */
        bool apply_overlay(const std::string &name);

        /**
         * @brief A clip (and a reference) to check and play once the game has loaded, from "Open with" or the command
         *        line. A `.bvh` or `.fbx` opens the Retarget tab instead.
         */
        void open_on_start(std::string clip, std::string reference)
        {
            const std::string kind = lowercase(clip);
            if (kind.ends_with(".bvh") || kind.ends_with(".fbx"))
            {
                // An animation to retarget ("Open with"): the Retarget tab, ready for its settings.
                m_mocap_source = std::move(clip);
                suggest_retarget_target();
                m_open_retarget_tab = true;
                return;
            }
            m_clip_text = std::move(clip);
            m_reference_text = std::move(reference);
            m_check_on_load = !m_clip_text.empty();
        }

        /** @brief Opens the window and runs it until it closes; the game loads once the first frame shows. */
        void run();

    private:
        void load_game();
        void load_preview(const std::string &name, bool reference);
        void flag_bones();
        void run_check();
        void run_make();
        void run_pack();
        void run_retarget();
        void suggest_retarget_target();
        void refresh_actions();
        void handle_drops();

        void draw_ui();
        void draw_setup();
        void draw_check_tab();
        void draw_make_tab();
        void draw_pack_tab();
        void draw_retarget_tab();
        void draw_clips_tab();
        void draw_viewport();
        void render_scene(int width, int height);
        void pose(const PreviewClip &clip, double frame, std::vector<Vec3> &out) const;

        [[nodiscard]] std::optional<fs::path> mod_folder() const;

        // Read before open_on_start, which places a retarget's output in the remembered mod folder.
        Settings m_settings{load_settings()};
        std::unique_ptr<OfflineGame> m_game;
        std::unique_ptr<Mannequin> m_mannequin;
        std::string m_game_status = "Looking for the game...";
        bool m_load_pending = true;
        bool m_check_on_load = false;

        std::unique_ptr<PreviewClip> m_clip;
        std::unique_ptr<PreviewClip> m_reference;
        std::string m_clip_text;
        std::string m_reference_text;
        bool m_loop_check = false;
        std::vector<ReportLine> m_report;

        std::string m_fragment;
        std::string m_like_search;
        std::string m_like;
        std::vector<std::string> m_like_results;
        bool m_make_loop = false;
        bool m_make_upper = false;
        std::vector<ReportLine> m_make_log;
        std::vector<ModActionInfo> m_actions;

        std::string m_pack_log;
        bool m_pack_failed = false;

        std::string m_mocap_source;
        std::string m_mocap_target;
        std::string m_mocap_take;
        std::string m_mocap_hands = "relaxed_idle_both_player";
        int m_mocap_root = 0;
        bool m_mocap_turns = false;
        bool m_mocap_keep_facing = false;
        bool m_mocap_lock_feet = false;
        bool m_mocap_twist = true;
        float m_mocap_from = 0.0F;
        float m_mocap_to = 0.0F;
        int m_mocap_calibrate = -1;
        std::vector<ReportLine> m_mocap_log;
        bool m_open_retarget_tab = false;

        std::string m_clip_search;
        std::vector<std::string> m_clip_results;

        double m_time = 0.0;
        bool m_playing = true;
        float m_speed = 1.0F;
        bool m_loop_play = true;
        bool m_in_place = true;
        bool m_show_reference = true;

        RenderTexture2D m_view{};
        // From in front and a little to the side (the skeleton faces +Y): the clip on the left, the reference on the
        // right.
        float m_yaw = 1.25F;
        float m_pitch = 0.18F;
        float m_distance = 4.2F;
        Vector3 m_target{0.0F, 0.0F, 1.0F};
        std::vector<Vec3> m_positions;
        std::vector<Vec3> m_reference_positions;
    };

    std::optional<fs::path> Studio::mod_folder() const
    {
        if (m_settings.mod.empty())
        {
            return std::nullopt;
        }
        return fs::path{m_settings.mod};
    }

    void Studio::load_game()
    {
        m_load_pending = false;
        // The previews' posers hold the old game's skeleton: they go before it does.
        m_clip.reset();
        m_reference.reset();
        m_mannequin.reset();
        m_game.reset();
        std::optional<fs::path> folder =
            m_settings.game.empty() ? find_steam_game() : std::optional{fs::path{m_settings.game}};
        if (!folder)
        {
            m_game_status = "The game was not found in Steam's libraries: pick its folder (the one holding Data).";
            return;
        }
        auto paks = GamePaks::open(*folder);
        if (!paks)
        {
            m_game_status = paks.error();
            return;
        }
        m_settings.game = folder->string();
        save_settings(m_settings);
        m_game = std::make_unique<OfflineGame>(std::move(*paks));
        const std::vector<SkeletonBone> *skeleton = m_game->skeleton();
        if (skeleton == nullptr)
        {
            m_game_status = "Henry's skeleton could not be read from the game's paks.";
            return;
        }
        m_mannequin = std::make_unique<Mannequin>(*skeleton);
        m_game_status = std::format("{} clips, {} bones", m_game->paks().clip_paths().size(), skeleton->size());
        refresh_actions();
        if (m_check_on_load)
        {
            m_check_on_load = false;
            run_check();
        }
    }

    void Studio::flag_bones()
    {
        const std::vector<SkeletonBone> *skeleton = m_game ? m_game->skeleton() : nullptr;
        if (!m_clip || skeleton == nullptr)
        {
            return;
        }
        std::unordered_map<std::uint32_t, std::size_t> bone_of;
        for (std::size_t i = 0; i < skeleton->size(); ++i)
        {
            bone_of.emplace((*skeleton)[i].controller, i);
        }
        m_clip->flagged.assign(skeleton->size(), 0);
        m_clip->slot_bones.clear();
        if (m_reference)
        {
            for (const ClipTrack &track : m_reference->found.clip.tracks)
            {
                const auto bone = bone_of.find(track.controller);
                if (bone != bone_of.end() && m_clip->found.clip.track(track.controller) == nullptr)
                {
                    m_clip->flagged[bone->second] = 1;
                }
            }
        }
        for (const ClipTrack &track : m_clip->found.clip.tracks)
        {
            const auto bone = bone_of.find(track.controller);
            if (bone != bone_of.end() &&
                std::ranges::find(SKELETON_MOD_BONES, (*skeleton)[bone->second].name) != SKELETON_MOD_BONES.end())
            {
                m_clip->slot_bones.push_back(bone->second);
            }
        }
    }

    void Studio::load_preview(const std::string &name, bool reference)
    {
        std::unique_ptr<PreviewClip> &slot = reference ? m_reference : m_clip;
        slot.reset();
        const std::vector<SkeletonBone> *skeleton = m_game ? m_game->skeleton() : nullptr;
        if (name.empty() || skeleton == nullptr)
        {
            flag_bones();
            return;
        }
        auto found = m_game->find_clip(name, mod_folder());
        if (!found)
        {
            m_report.push_back(ReportLine{.warning = true, .text = found.error()});
            flag_bones();
            return;
        }
        auto loaded = std::make_unique<PreviewClip>();
        loaded->found = std::move(*found);
        loaded->poser = std::make_unique<ClipPoser>(*skeleton, loaded->found.clip);
        slot = std::move(loaded);
        if (!reference)
        {
            m_time = 0.0;
        }
        flag_bones();
    }

    void Studio::run_check()
    {
        m_report.clear();
        if (!m_game)
        {
            m_report.push_back(ReportLine{.warning = true, .text = "Load the game first."});
            return;
        }
        const ClipCheckRequest request{.clip = m_clip_text, .reference = m_reference_text, .loops = m_loop_check};
        report_clip_check(
            m_game->tools(mod_folder()),
            request,
            [this](std::string_view line) { m_report.push_back(report_line(line)); }
        );
        load_preview(m_clip_text, false);
        load_preview(m_reference_text, true);
    }

    void Studio::run_make()
    {
        m_make_log.clear();
        if (!m_game || !mod_folder())
        {
            m_make_log.push_back(ReportLine{.warning = true, .text = "Load the game and pick the mod's folder first."});
            return;
        }
        const MakeRequest request{
            .mod_folder = *mod_folder(),
            .fragment = m_fragment,
            .clip = m_clip_text,
            .like = m_like,
            .loop = m_make_loop,
            .upper = m_make_upper,
        };
        auto made = make_action_in_folder(
            *m_game,
            request,
            [this](std::string_view line) { m_make_log.push_back(report_line(line)); }
        );
        m_make_log.push_back(ReportLine{.warning = !made, .text = made ? *made : made.error()});
        refresh_actions();
    }

    void Studio::run_pack()
    {
        if (!mod_folder() || m_settings.install.empty())
        {
            m_pack_log = "Pick the mod's folder and where to install it first.";
            m_pack_failed = true;
            return;
        }
        auto packed = pack_mod_folder(*mod_folder(), fs::path{m_settings.install});
        m_pack_failed = !packed;
        m_pack_log = packed ? *packed : packed.error();
    }

    void Studio::suggest_retarget_target()
    {
        // <mod>\Data\Animations\humans\male\<modid>\<modid>_<source>.caf, or beside the source without a mod.
        const fs::path source{m_mocap_source};
        std::string stem;
        for (const char c : lowercase(source.stem().string()))
        {
            stem += (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ? c : '_';
        }
        if (const std::optional<fs::path> mod = mod_folder())
        {
            const std::string modid = mod_id_of(*mod);
            m_mocap_target =
                (*mod / "Data" / "Animations" / "humans" / "male" / modid / std::format("{}_{}.caf", modid, stem))
                    .string();
        }
        else
        {
            m_mocap_target = (source.parent_path() / (stem + ".caf")).string();
        }
    }

    void Studio::run_retarget()
    {
        m_mocap_log.clear();
        if (!m_game)
        {
            m_mocap_log.push_back(ReportLine{.warning = true, .text = "Load the game first."});
            return;
        }
        if (m_mocap_source.empty() || m_mocap_target.empty())
        {
            m_mocap_log.push_back(
                ReportLine{.warning = true, .text = "Pick the animation to retarget and the .caf to write."}
            );
            return;
        }
        RetargetRequest request;
        request.source = fs::path{m_mocap_source};
        request.target = fs::path{m_mocap_target};
        request.take = m_mocap_take;
        request.hands = m_mocap_hands;
        request.options.root = m_mocap_root == 1   ? RootMotion::Line
                               : m_mocap_root == 2 ? RootMotion::InPlace
                                                   : RootMotion::Path;
        request.options.root_turns = m_mocap_turns;
        request.options.keep_facing = m_mocap_keep_facing;
        request.options.lock_feet = m_mocap_lock_feet;
        request.options.share_twist = m_mocap_twist;
        request.options.from_seconds = m_mocap_from;
        if (m_mocap_to > 0.0F)
        {
            request.options.to_seconds = m_mocap_to;
        }
        if (m_mocap_calibrate >= 0)
        {
            request.options.calibration_frame = static_cast<std::size_t>(m_mocap_calibrate);
        }
        int code = 1;
        // A malformed source can size allocations past memory: its failure goes to the log, the window stays.
        try
        {
            code = retarget_clip_file(
                *m_game,
                request,
                [this](std::string_view line) { m_mocap_log.push_back(report_line(line)); }
            );
        }
        catch (const std::exception &error)
        {
            m_mocap_log.push_back(ReportLine{.warning = true, .text = error.what()});
        }
        if (code != 1)
        {
            // Play it on the mannequin, and let Check and Make pick it up.
            m_clip_text = m_mocap_target;
            load_preview(m_mocap_target, false);
        }
    }

    void Studio::refresh_actions()
    {
        m_actions = mod_folder() ? list_mod_actions(*mod_folder()) : std::vector<ModActionInfo>{};
    }

    void Studio::handle_drops()
    {
        if (!IsFileDropped())
        {
            return;
        }
        FilePathList dropped = LoadDroppedFiles();
        for (unsigned i = 0; i < dropped.count; ++i)
        {
            const std::string path = dropped.paths[i];
            std::error_code error;
            if (fs::is_directory(path, error))
            {
                m_settings.mod = path;
                save_settings(m_settings);
                refresh_actions();
            }
            else if (lowercase(path).ends_with(".caf"))
            {
                m_clip_text = path;
                run_check();
            }
            else if (lowercase(path).ends_with(".bvh") || lowercase(path).ends_with(".fbx"))
            {
                m_mocap_source = path;
                suggest_retarget_target();
                m_open_retarget_tab = true;
            }
        }
        UnloadDroppedFiles(dropped);
    }

    void Studio::pose(const PreviewClip &clip, double frame, std::vector<Vec3> &out) const
    {
        std::vector<Quat> rotations;
        clip.poser->pose(frame, rotations, out);
        if (m_in_place && !out.empty())
        {
            const Vec3 root = out.front();
            for (Vec3 &p : out)
            {
                p.x -= root.x;
                p.y -= root.y;
            }
        }
    }

    void Studio::render_scene(int width, int height)
    {
        if (m_view.id == 0 || m_view.texture.width != width || m_view.texture.height != height)
        {
            if (m_view.id != 0)
            {
                UnloadRenderTexture(m_view);
            }
            m_view = LoadRenderTexture(width, height);
        }
        m_positions.clear();
        m_reference_positions.clear();
        if (m_clip)
        {
            pose(*m_clip, m_clip->found.clip.start_frame + m_time, m_positions);
        }
        if (m_reference && m_show_reference)
        {
            const CafClip &reference = m_reference->found.clip;
            const double length = std::max(reference.end_frame - reference.start_frame, 1);
            pose(*m_reference, reference.start_frame + std::fmod(m_time, length), m_reference_positions);
        }
        if (m_mannequin && m_mannequin->center() && *m_mannequin->center() < m_positions.size())
        {
            const Vec3 &hips = m_positions[*m_mannequin->center()];
            Vector3 wanted{static_cast<float>(hips.x), static_cast<float>(hips.y), static_cast<float>(hips.z)};
            if (!m_reference_positions.empty())
            {
                wanted.x += REFERENCE_OFFSET * 0.5F;
            }
            m_target = Vector3Lerp(m_target, wanted, 0.2F);
        }
        Camera3D camera{};
        camera.target = m_target;
        camera.position = Vector3Add(
            m_target,
            Vector3{
                m_distance * std::cos(m_pitch) * std::cos(m_yaw),
                m_distance * std::cos(m_pitch) * std::sin(m_yaw),
                m_distance * std::sin(m_pitch),
            }
        );
        camera.up = Vector3{0.0F, 0.0F, 1.0F};
        camera.fovy = 40.0F;
        camera.projection = CAMERA_PERSPECTIVE;

        BeginTextureMode(m_view);
        ClearBackground(BACKGROUND);
        BeginMode3D(camera);
        const float grid = 0.5F;
        for (int i = -10; i <= 10; ++i)
        {
            const Color line = i == 0 ? Color{110, 115, 125, 255} : Color{70, 74, 82, 255};
            DrawLine3D(Vector3{i * grid, -10 * grid, 0}, Vector3{i * grid, 10 * grid, 0}, line);
            DrawLine3D(Vector3{-10 * grid, i * grid, 0}, Vector3{10 * grid, i * grid, 0}, line);
        }
        if (m_mannequin)
        {
            if (!m_positions.empty())
            {
                m_mannequin->draw(m_positions, Vector3{0, 0, 0}, BODY, m_clip->flagged, FLAGGED);
                m_mannequin->draw_markers(m_positions, Vector3{0, 0, 0}, m_clip->slot_bones, SLOT);
            }
            if (!m_reference_positions.empty())
            {
                m_mannequin->draw(m_reference_positions, Vector3{REFERENCE_OFFSET, 0, 0}, REFERENCE, {}, REFERENCE);
            }
        }
        EndMode3D();
        EndTextureMode();
    }

    void Studio::draw_setup()
    {
        ImGui::SeparatorText("Game");
        ImGui::SetNextItemWidth(-90.0F);
        ImGui::InputTextWithHint("##game", "Steam install", &m_settings.game);
        ImGui::SameLine();
        if (ImGui::Button("...##game"))
        {
            if (auto picked = pick_folder("The game's folder (holding Data)"))
            {
                m_settings.game = *picked;
                m_load_pending = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Load"))
        {
            m_load_pending = true;
        }
        ImGui::TextColored(m_game ? DIM_TEXT : WARNING_TEXT, "%s", m_game_status.c_str());

        ImGui::SeparatorText("Mod");
        ImGui::SetNextItemWidth(-40.0F);
        if (ImGui::InputTextWithHint(
                "##mod",
                "the mod's source folder, named after its mod id",
                &m_settings.mod,
                ImGuiInputTextFlags_EnterReturnsTrue
            ))
        {
            save_settings(m_settings);
            refresh_actions();
        }
        ImGui::SameLine();
        if (ImGui::Button("...##mod"))
        {
            if (auto picked = pick_folder("The mod's source folder"))
            {
                m_settings.mod = *picked;
                save_settings(m_settings);
                refresh_actions();
            }
        }
        if (mod_folder())
        {
            ImGui::TextColored(
                DIM_TEXT,
                "mod id %s, %zu action(s)",
                mod_id_of(*mod_folder()).c_str(),
                m_actions.size()
            );
            for (const ModActionInfo &action : m_actions)
            {
                const std::string label =
                    std::format("{}  ->  {}{}", action.fragment, action.clip, action.loop ? "  (loops)" : "");
                if (ImGui::Selectable(label.c_str()))
                {
                    m_clip_text = action.clip;
                    load_preview(action.clip, false);
                }
            }
        }
    }

    void Studio::draw_check_tab()
    {
        ImGui::TextColored(DIM_TEXT, "A .caf file (or drop one on the window), or a game clip by name.");
        ImGui::SetNextItemWidth(-40.0F);
        ImGui::InputTextWithHint("##clip", "clip", &m_clip_text);
        ImGui::SameLine();
        if (ImGui::Button("...##clip"))
        {
            if (auto picked = pick_clip_file())
            {
                m_clip_text = *picked;
            }
        }
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::InputTextWithHint("##reference", "reference: the game clip it replaces (optional)", &m_reference_text);
        ImGui::Checkbox("It loops", &m_loop_check);
        ImGui::SameLine();
        if (ImGui::Button("Check", ImVec2{-1.0F, 0.0F}))
        {
            run_check();
        }
        draw_log("report", m_report);
    }

    void Studio::draw_make_tab()
    {
        ImGui::TextColored(DIM_TEXT, "A new fragment that plays the clip, set up like a game fragment.");
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::InputTextWithHint("##fragment", "new fragment, e.g. MyMod_Wave", &m_fragment);
        ImGui::TextColored(DIM_TEXT, "Clip: %s", m_clip_text.empty() ? "(pick one in Check)" : m_clip_text.c_str());
        ImGui::SetNextItemWidth(-1.0F);
        if (ImGui::InputTextWithHint("##like", "search the game's fragments (wave, sit, pray...)", &m_like_search) &&
            m_game)
        {
            m_like_results = m_game->find_fragments(m_like_search, SEARCH_LIMIT);
        }
        if (ImGui::BeginListBox("##likes", ImVec2{-1.0F, 170.0F}))
        {
            for (const std::string &line : m_like_results)
            {
                const std::string name = line.substr(0, line.find('\t'));
                std::string shown = line;
                std::ranges::replace(shown, '\t', ' ');
                if (ImGui::Selectable(shown.c_str(), name == m_like))
                {
                    m_like = name;
                }
            }
            ImGui::EndListBox();
        }
        ImGui::Text("Set up like: %s", m_like.empty() ? "(pick one above)" : m_like.c_str());
        ImGui::Checkbox("The clip loops until stopped", &m_make_loop);
        ImGui::Checkbox("Upper body only, over what Henry is doing (gestures)", &m_make_upper);
        if (ImGui::Button("Make action", ImVec2{-1.0F, 0.0F}))
        {
            run_make();
        }
        draw_log("make_log", m_make_log);
    }

    void Studio::draw_pack_tab()
    {
        ImGui::TextColored(
            DIM_TEXT,
            "Writes mod.manifest and Data\\<modid>.pak: point it at your mod manager's folder"
        );
        ImGui::TextColored(DIM_TEXT, "for the mod (MO2: mods\\My Mod\\my_mod) or the game's Mods\\my_mod.");
        ImGui::SetNextItemWidth(-40.0F);
        if (ImGui::InputTextWithHint("##install", "install folder", &m_settings.install))
        {
            save_settings(m_settings);
        }
        ImGui::SameLine();
        if (ImGui::Button("...##install"))
        {
            if (auto picked = pick_folder("Where the installed mod goes"))
            {
                m_settings.install = *picked;
                save_settings(m_settings);
            }
        }
        if (ImGui::Button("Pack", ImVec2{-1.0F, 0.0F}))
        {
            run_pack();
        }
        if (!m_pack_log.empty())
        {
            ImGui::PushStyleColor(
                ImGuiCol_Text,
                m_pack_failed ? WARNING_TEXT : ImGui::GetStyleColorVec4(ImGuiCol_Text)
            );
            ImGui::TextWrapped("%s", m_pack_log.c_str());
            ImGui::PopStyleColor();
        }
    }

    void Studio::draw_retarget_tab()
    {
        ImGui::TextColored(DIM_TEXT, "A .bvh or .fbx made on another skeleton (mocap, Mixamo, SMPL, Unreal),");
        ImGui::TextColored(DIM_TEXT, "put on Henry's and compiled. Drop one on the window.");
        ImGui::SetNextItemWidth(-40.0F);
        ImGui::InputTextWithHint("##mocap", "animation (.bvh, .fbx)", &m_mocap_source);
        ImGui::SameLine();
        if (ImGui::Button("...##mocap"))
        {
            if (auto picked = pick_mocap_file())
            {
                m_mocap_source = *picked;
                suggest_retarget_target();
            }
        }
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::InputTextWithHint("##mocap_target", "the .caf to write", &m_mocap_target);
        constexpr const char *roots[] =
            {"Root follows the pelvis's path", "Root in a straight line (loops)", "In place (cycle)"};
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::Combo("##root", &m_mocap_root, roots, static_cast<int>(std::size(roots)));
        ImGui::Checkbox("Root turns with the body", &m_mocap_turns);
        ImGui::SameLine();
        ImGui::Checkbox("Keep the source's facing", &m_mocap_keep_facing);
        ImGui::Checkbox("Lock planted feet (leg IK)", &m_mocap_lock_feet);
        ImGui::SameLine();
        ImGui::Checkbox("Spread wrist twist", &m_mocap_twist);
        ImGui::SetNextItemWidth(150.0F);
        ImGui::DragFloat("from s", &m_mocap_from, 0.05F, 0.0F, 3600.0F, "%.2f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150.0F);
        ImGui::DragFloat("to s (0: end)", &m_mocap_to, 0.05F, 0.0F, 3600.0F, "%.2f");
        ImGui::SetNextItemWidth(150.0F);
        ImGui::InputInt("reference frame (-1: rest)", &m_mocap_calibrate);
        m_mocap_calibrate = std::max(m_mocap_calibrate, -1);
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::InputTextWithHint("##take", "take (FBX with several; empty: the first)", &m_mocap_take);
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::InputTextWithHint("##hands", "hands pose clip (empty: default pose)", &m_mocap_hands);
        if (ImGui::Button("Retarget", ImVec2{-1.0F, 0.0F}))
        {
            run_retarget();
        }
        draw_log("mocap_log", m_mocap_log);
    }

    void Studio::draw_clips_tab()
    {
        ImGui::TextColored(DIM_TEXT, "The game's clips: click to play, or use one as the reference.");
        ImGui::SetNextItemWidth(-1.0F);
        if (ImGui::InputTextWithHint("##clip_search", "search (walk, idle, sword...)", &m_clip_search) && m_game)
        {
            m_clip_results = m_clip_search.size() >= 2 ? m_game->find_clips(m_clip_search, SEARCH_LIMIT)
                                                       : std::vector<std::string>{};
        }
        ImGui::BeginChild("clips", ImVec2{0, 0}, ImGuiChildFlags_Borders);
        for (const std::string &path : m_clip_results)
        {
            const std::string stem = clip_stem(path);
            ImGui::PushID(path.c_str());
            if (ImGui::SmallButton("ref"))
            {
                m_reference_text = stem;
                load_preview(stem, true);
            }
            ImGui::SameLine();
            if (ImGui::Selectable(path.c_str(), m_clip && m_clip->found.path == path))
            {
                m_clip_text = stem;
                load_preview(stem, false);
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    void Studio::draw_viewport()
    {
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const int width = std::max(64, static_cast<int>(available.x));
        const int height = std::max(64, static_cast<int>(available.y - TRANSPORT_HEIGHT));
        render_scene(width, height);
        const ImVec2 corner = ImGui::GetCursorScreenPos();
        rlImGuiImageRenderTexture(&m_view);
        if (ImGui::IsItemHovered())
        {
            const ImGuiIO &io = ImGui::GetIO();
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Right))
            {
                m_yaw -= io.MouseDelta.x * 0.01F;
                m_pitch = std::clamp(m_pitch + io.MouseDelta.y * 0.01F, -0.2F, 1.4F);
            }
            m_distance = std::clamp(m_distance * (1.0F - io.MouseWheel * 0.1F), 0.6F, 15.0F);
        }
        ImDrawList *draw = ImGui::GetWindowDrawList();
        const std::string caption = m_clip
                                        ? std::format("{}  ({})", clip_stem(m_clip->found.path), m_clip->found.source)
                                        : std::string{"Drop a .caf here, or pick a clip in Check or Game clips."};
        draw->AddText(ImVec2{corner.x + 10.0F, corner.y + 8.0F}, IM_COL32(220, 222, 228, 255), caption.c_str());
        if (m_clip && m_reference && m_show_reference)
        {
            draw->AddText(
                ImVec2{corner.x + 10.0F, corner.y + 8.0F + ImGui::GetTextLineHeightWithSpacing()},
                IM_COL32(150, 180, 230, 255),
                std::format("blue, right: {}", clip_stem(m_reference->found.path)).c_str()
            );
            draw->AddText(
                ImVec2{corner.x + 10.0F, corner.y + 8.0F + 2.0F * ImGui::GetTextLineHeightWithSpacing()},
                IM_COL32(230, 120, 110, 255),
                "red: bones the reference keys that yours leaves out    orange: back slots skeleton mods move"
            );
        }

        double length = 0.0;
        if (m_clip)
        {
            length = std::max(m_clip->found.clip.end_frame - m_clip->found.clip.start_frame, 0);
        }
        if (ImGui::Button(m_playing ? "Pause" : "Play", ImVec2{70.0F, 0.0F}))
        {
            m_playing = !m_playing;
        }
        ImGui::SameLine();
        float frame = static_cast<float>(m_time);
        ImGui::SetNextItemWidth(-1.0F);
        if (ImGui::SliderFloat("##frame", &frame, 0.0F, static_cast<float>(std::max(length, 1.0)), "frame %.0f"))
        {
            m_time = frame;
            m_playing = false;
        }
        ImGui::SetNextItemWidth(160.0F);
        ImGui::SliderFloat("speed", &m_speed, 0.1F, 2.0F, "%.2fx");
        ImGui::SameLine();
        ImGui::Checkbox("loop", &m_loop_play);
        ImGui::SameLine();
        ImGui::Checkbox("in place", &m_in_place);
        ImGui::SameLine();
        ImGui::Checkbox("reference", &m_show_reference);
        if (m_clip)
        {
            ImGui::TextColored(
                DIM_TEXT,
                "%.3f s, %zu bones keyed. Drag to orbit, wheel to zoom.",
                m_clip->found.clip.seconds(),
                m_clip->found.clip.tracks.size()
            );
        }
        if (m_playing && length > 0.0)
        {
            m_time += GetFrameTime() * CLIP_FRAME_RATE * m_speed;
            if (m_time > length)
            {
                m_time = m_loop_play ? std::fmod(m_time, length) : length;
                m_playing = m_loop_play;
            }
        }
    }

    void Studio::draw_ui()
    {
        ImGui::SetNextWindowPos(ImVec2{0, 0});
        ImGui::SetNextWindowSize(ImVec2{static_cast<float>(GetScreenWidth()), static_cast<float>(GetScreenHeight())});
        ImGui::Begin(
            "Studio",
            nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus
        );
        ImGui::BeginChild("side", ImVec2{SIDEBAR_WIDTH, 0}, ImGuiChildFlags_Borders);
        draw_setup();
        ImGui::Spacing();
        if (ImGui::BeginTabBar("tabs"))
        {
            if (ImGui::BeginTabItem("Check"))
            {
                draw_check_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Make"))
            {
                draw_make_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Pack"))
            {
                draw_pack_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(
                    "Retarget",
                    nullptr,
                    std::exchange(m_open_retarget_tab, false) ? ImGuiTabItemFlags_SetSelected : 0
                ))
            {
                draw_retarget_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Game clips"))
            {
                draw_clips_tab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("view", ImVec2{0, 0});
        draw_viewport();
        ImGui::EndChild();
        ImGui::End();
    }

    void Studio::run()
    {
        SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT | FLAG_WINDOW_HIGHDPI);
        InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "AnimFW Studio - KCD2 Animation Framework");
        SetTargetFPS(60);
        rlImGuiBeginInitImGui();
        ImGui::StyleColorsDark();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        // Windows' own UI font reads far better than the built-in bitmap font, which stays when Segoe UI is missing.
        const std::optional<std::string> segoe = windows_font("segoeui.ttf");
        if (ImFont *font = segoe ? io.Fonts->AddFontFromFileTTF(segoe->c_str(), 17.0F) : nullptr; font != nullptr)
        {
            // rlImGui adds its built-in font first, which otherwise stays the default.
            io.FontDefault = font;
        }
        rlImGuiEndInitImGui();
        ImGuiStyle &style = ImGui::GetStyle();
        style.WindowPadding = ImVec2{10.0F, 10.0F};
        style.FramePadding = ImVec2{8.0F, 5.0F};
        style.ItemSpacing = ImVec2{8.0F, 7.0F};
        style.FrameRounding = 4.0F;
        style.ChildRounding = 4.0F;
        style.TabRounding = 4.0F;
        style.GrabRounding = 4.0F;
        int frames = 0;
        while (!WindowShouldClose())
        {
            // The game loads after the first frame shows, so the window is up while its paks are read.
            if (m_load_pending && ++frames > 1)
            {
                load_game();
            }
            handle_drops();
            BeginDrawing();
            ClearBackground(BACKGROUND);
            rlImGuiBegin();
            draw_ui();
            rlImGuiEnd();
            EndDrawing();
        }
        m_mannequin.reset();
        if (m_view.id != 0)
        {
            UnloadRenderTexture(m_view);
        }
        rlImGuiShutdown();
        CloseWindow();
    }

    bool Studio::apply_overlay(const std::string &name)
    {
        auto found = m_game->find_clip(name, mod_folder());
        if (!found)
        {
            std::fprintf(stderr, "%s\n", found.error().c_str());
            return false;
        }
        CafClip &base = m_clip->found.clip;
        const CafClip &overlay = found->clip;
        const int base_length = std::max(base.end_frame - base.start_frame, 1);
        const int overlay_length = std::max(overlay.end_frame - overlay.start_frame, 1);
        for (const ClipTrack &track : overlay.tracks)
        {
            ClipTrack tiled{.controller = track.controller};
            for (int start = 0; start <= base_length; start += overlay_length)
            {
                const double shift = base.start_frame + start - overlay.start_frame;
                for (std::size_t k = 0; k < track.rotation_frames.size(); ++k)
                {
                    tiled.rotation_frames.push_back(track.rotation_frames[k] + shift);
                    tiled.rotations.push_back(track.rotations[k]);
                }
                for (std::size_t k = 0; k < track.position_frames.size(); ++k)
                {
                    tiled.position_frames.push_back(track.position_frames[k] + shift);
                    tiled.positions.push_back(track.positions[k]);
                }
            }
            std::erase_if(base.tracks, [&](const ClipTrack &t) { return t.controller == track.controller; });
            base.tracks.push_back(std::move(tiled));
        }
        m_clip->poser = std::make_unique<ClipPoser>(*m_game->skeleton(), base);
        return true;
    }

    int Studio::render(const RenderRequest &request)
    {
        SetTraceLogLevel(LOG_WARNING);
        SetConfigFlags(FLAG_WINDOW_HIDDEN);
        InitWindow(request.width, request.height, "AnimFW Studio render");
        m_yaw = request.yaw;
        m_pitch = request.pitch;
        m_distance = request.distance;
        m_show_reference = false;
        load_game();
        int code = 0;
        if (!m_mannequin)
        {
            std::fprintf(stderr, "%s\n", m_game_status.c_str());
            code = 1;
        }
        else if (load_preview(request.clip, false); !m_clip)
        {
            for (const ReportLine &line : m_report)
            {
                std::fprintf(stderr, "%s\n", line.text.c_str());
            }
            code = 1;
        }
        else if (!request.overlay.empty() && !apply_overlay(request.overlay))
        {
            code = 1;
        }
        else
        {
            const CafClip &clip = m_clip->found.clip;
            const double frames = std::max(clip.end_frame - clip.start_frame, 1);
            const double rate = frames / std::max(clip.seconds(), 1e-6);
            const double first = std::clamp(request.from * rate, 0.0, frames);
            const double last = request.to < 0.0 ? frames : std::clamp(request.to * rate, first, frames);
            std::error_code error;
            fs::create_directories(request.out, error);
            // The camera eases toward the hips: settle it on the first frame before saving any.
            m_time = first;
            for (int i = 0; i < 60; ++i)
            {
                render_scene(request.width, request.height);
            }
            int saved = 0;
            for (double frame = first; frame <= last; frame += std::max(request.every, 1))
            {
                m_time = frame;
                render_scene(request.width, request.height);
                Image image = LoadImageFromTexture(m_view.texture);
                ImageFlipVertical(&image);
                const fs::path path = request.out / std::format("frame_{:04}.png", saved++);
                ExportImage(image, path.string().c_str());
                UnloadImage(image);
            }
            std::printf("%d frames at %.3f fps from %s\n", saved, rate / std::max(request.every, 1), clip_stem(m_clip->found.path).c_str());
        }
        m_clip.reset();
        m_mannequin.reset();
        if (m_view.id != 0)
        {
            UnloadRenderTexture(m_view);
        }
        CloseWindow();
        return code;
    }

    /** @brief Parses `--render <clip> <out folder> [--size WxH] [--from s] [--to s] [--every n] [--yaw r]
     *         [--pitch r] [--distance m]`. */
    [[nodiscard]] std::optional<RenderRequest> parse_render(int argc, char **argv)
    {
        if (argc < 4)
        {
            return std::nullopt;
        }
        RenderRequest request{.clip = argv[2], .out = fs::path{argv[3]}};
        for (int i = 4; i + 1 < argc; i += 2)
        {
            const std::string_view key = argv[i];
            const std::string value = argv[i + 1];
            if (key == "--size")
            {
                const std::size_t x = value.find('x');
                if (x == std::string::npos)
                {
                    return std::nullopt;
                }
                request.width = std::stoi(value.substr(0, x));
                request.height = std::stoi(value.substr(x + 1));
            }
            else if (key == "--from")
            {
                request.from = std::stod(value);
            }
            else if (key == "--to")
            {
                request.to = std::stod(value);
            }
            else if (key == "--every")
            {
                request.every = std::stoi(value);
            }
            else if (key == "--yaw")
            {
                request.yaw = std::stof(value);
            }
            else if (key == "--pitch")
            {
                request.pitch = std::stof(value);
            }
            else if (key == "--overlay")
            {
                request.overlay = value;
            }
            else if (key == "--distance")
            {
                request.distance = std::stof(value);
            }
            else
            {
                return std::nullopt;
            }
        }
        return request;
    }
} // namespace

int main(int argc, char **argv)
{
    if (argc > 1 && std::string_view{argv[1]} == "--render")
    {
        try
        {
            const std::optional<RenderRequest> request = parse_render(argc, argv);
            if (!request)
            {
                std::fprintf(
                    stderr,
                    "usage: animfw-studio --render <clip> <out folder> [--size WxH] [--from s] [--to s] [--every n] "
                    "[--yaw r] [--pitch r] [--distance m]\n"
                );
                return 2;
            }
            Studio studio;
            return studio.render(*request);
        }
        catch (const std::exception &error)
        {
            std::fprintf(stderr, "%s\n", error.what());
            return 1;
        }
    }
    Studio studio;
    studio.open_on_start(argc > 1 ? argv[1] : "", argc > 2 ? argv[2] : "");
    studio.run();
    return 0;
}
