/**
 * @file tests/packs_tests.cpp
 * @brief Unit tests of pack parsing and the catalog. Plain asserts, no framework: the exit code is the verdict.
 */

#include "packs/catalog.hpp"
#include "packs/cutscene_table.hpp"
#include "packs/ini_document.hpp"
#include "packs/sound_def.hpp"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace
{
    using namespace SoundFramework::packs;
    namespace fs = std::filesystem;

    int s_failures = 0;

    void check(bool condition, const char *what, int line)
    {
        if (!condition)
        {
            std::printf("FAIL line %d: %s\n", line, what);
            ++s_failures;
        }
    }

#define CHECK(cond) check((cond), #cond, __LINE__)

    [[nodiscard]] bool mentions(const std::vector<std::string> &lines, std::string_view needle)
    {
        for (const std::string &line : lines)
        {
            if (line.find(needle) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

    void write_file(const fs::path &path, std::string_view text)
    {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
    }

    void test_ini_reader()
    {
        const IniDocument doc = parse_ini(
            "\xEF\xBB\xBF; header\norphan = 1\n[a]\n key = value ; note\nx=a;b\n"
            "#c\n[ ]\nnoequals\n[b]\nk =\n"
        );
        CHECK(doc.sections.size() == 2);
        CHECK(doc.sections[0].name == "a");
        CHECK(doc.sections[0].entries.size() == 2);
        CHECK(doc.sections[0].entries[0].value == "value");
        CHECK(doc.sections[0].entries[1].value == "a;b");
        CHECK(doc.sections[1].entries.size() == 1 && doc.sections[1].entries[0].value.empty());
        CHECK(doc.problems.size() == 3);
        CHECK(mentions(doc.problems, "line 2:"));
        CHECK(iequals("Files", "FILES") && !iequals("File", "Files"));
    }

    void test_pack_values()
    {
        const fs::path ini = fs::path("C:/Mods/m/SoundFramework/p.ini");
        const PackParse parsed = parse_pack(
            "[bell]\nFiles = a.ogg, sub/b.wav ,\nBus = voice\nSpace = 2d\nVolume = 9\nPitch = 1.5\nLoop = yes\n"
            "MaxInstances = 3\nMinDistance = 5\nMaxDistance = 1\nColour = red\n"
            "Replaces = event:/Animals/Dog/dog_bark, event:/ui/x\n"
            "ReplacesFor = Tvez_Vorech\n"
            "[badreplace]\nFiles = x.ogg\nReplaces = bus:/dieg\n"
            "[orphanfor]\nFiles = x.ogg\nReplacesFor = someone\n"
            "[bad name]\nFiles = x.ogg\n[nofiles]\nVolume = 1\n[badbus]\nFiles = x.ogg\nBus = nowhere\n"
            "[raw]\nFiles = C:/abs/x.ogg\nBus = bus:/dieg/w_beds\nVolume = loud\n",
            ini
        );
        CHECK(parsed.sounds.size() == 2); // bell, and orphanfor (ReplacesFor alone only warns)
        const SoundDef &bell = parsed.sounds.front();
        CHECK(bell.name == "bell");
        CHECK(bell.files.size() == 2);
        CHECK(bell.files[1] == fs::path("C:/Mods/m/SoundFramework/sub/b.wav").lexically_normal());
        CHECK(bell.bus == "bus:/dieg/dialog");
        CHECK(bell.space == Space::Flat);
        CHECK(bell.volume == 4.0f);
        CHECK(bell.pitch == 1.5f);
        CHECK(bell.loop);
        CHECK(bell.max_instances == 3);
        CHECK(bell.replaces.size() == 2 && bell.replaces[0] == "event:/animals/dog/dog_bark");
        CHECK(mentions(parsed.problems, "'bus:/dieg' is not an event:/ path"));
        CHECK(bell.replaces_for.size() == 1 && bell.replaces_for[0] == "tvez_vorech");
        CHECK(mentions(parsed.problems, "[orphanfor] ReplacesFor without Replaces"));
        CHECK(bell.max_distance == bell.min_distance && bell.min_distance == 5.0f);
        CHECK(mentions(parsed.problems, "clamped"));
        CHECK(mentions(parsed.problems, "unknown key 'Colour'"));
        CHECK(mentions(parsed.problems, "[bad name]"));
        CHECK(mentions(parsed.problems, "[nofiles] no Files"));
        CHECK(mentions(parsed.problems, "nowhere"));
        CHECK(mentions(parsed.problems, "'loud' is not a number"));
        CHECK(bus_path("WORLD") == std::optional<std::string>("bus:/dieg/w_obj/obj"));
        CHECK(!bus_path("bus").has_value());
        CHECK(valid_sound_name("my_mod.bell-2") && !valid_sound_name("a b") && !valid_sound_name(""));
    }

    void test_play_at_and_keep_original()
    {
        const PackParse parsed = parse_pack(
            "[shot]\nFiles = x.ogg\nReplaces = event:/combat/weapons/bow/bow_shot\nPlayAt = Release : 1\n"
            "[anyvalue]\nFiles = x.ogg\nReplaces = event:/a\nPlayAt = Stage\n"
            "[orphanat]\nFiles = x.ogg\nPlayAt = release\n"
            "[noname]\nFiles = x.ogg\nReplaces = event:/a\nPlayAt = :1\n"
            "[badvalue]\nFiles = x.ogg\nReplaces = event:/a\nPlayAt = release:high\n"
            "[nanvalue]\nFiles = x.ogg\nReplaces = event:/a\nPlayAt = release:nan\n"
            "[layered]\nFiles = x.ogg\nReplaces = event:/a\nKeepOriginal = yes\n"
            "[orphankeep]\nFiles = x.ogg\nKeepOriginal = true\n",
            fs::path("C:/Mods/m/SoundFramework/p.ini")
        );
        CHECK(parsed.sounds.size() == 5); // shot, anyvalue, orphanat, layered, orphankeep (alone they only warn)
        CHECK(parsed.sounds[0].play_at == "release" && parsed.sounds[0].play_at_value == std::optional(1.0f));
        CHECK(parsed.sounds[1].play_at == "stage" && !parsed.sounds[1].play_at_value.has_value());
        CHECK(mentions(parsed.problems, "[orphanat] PlayAt without Replaces"));
        CHECK(mentions(parsed.problems, "[noname] PlayAt needs a parameter name"));
        CHECK(mentions(parsed.problems, "PlayAt value 'high' is not a number"));
        CHECK(mentions(parsed.problems, "PlayAt value 'nan' is not a number"));
        CHECK(!parsed.sounds[0].keep_original && parsed.sounds[3].keep_original);
        CHECK(mentions(parsed.problems, "[orphankeep] KeepOriginal without Replaces"));
    }

    void test_replaces_voice()
    {
        const PackParse parsed = parse_pack(
            "[scum]\nVoice = v\nReplacesVoice = Stra_Strazny_Z_*_Surx, zaty_strazny_z_stat_HFVa\n",
            fs::path("C:/Mods/m/SoundFramework/p.ini")
        );
        CHECK(parsed.lines.size() == 1 && parsed.problems.empty());
        CHECK(
            parsed.lines[0].replaces_voice.size() == 2 && parsed.lines[0].replaces_voice[0] == "stra_strazny_z_*_surx"
        );
        CHECK(parsed.lines[0].replaces_voice[1] == "zaty_strazny_z_stat_hfva");
        CHECK(wildcard_match("stra_strazny_z_*", "stra_strazny_z_se_zlodeje_xnnl"));
        CHECK(wildcard_match("*", "") && wildcard_match("*", "anything"));
        CHECK(wildcard_match("a*b*c", "axxbyyc") && !wildcard_match("a*b*c", "axxbyy"));
        CHECK(wildcard_match("*_hfva", "zaty_strazny_z_stat_hfva") && !wildcard_match("*_hfva", "zaty_hfvax"));
        CHECK(!wildcard_match("zaty_strazny_z_stat_hfva", "zaty_strazny_z_stat_hfvax"));
        CHECK(wildcard_match("a**b", "ab") && !wildcard_match("", "a"));
    }

    void test_lines()
    {
        const fs::path ini = fs::path("C:/Mods/m/SoundFramework/p.ini");
        const PackParse parsed = parse_pack(
            "[hello]\nVoice = Localization/dialog/m/a_01, Localization/dialog/m/a_02.ogg\nTextKey = k1, k2\n"
            "Speaker = soul_ui_name_guard\nCooldown = 5\n"
            "[quiet]\nVoice = Localization/dialog/m/b\n"
            "[keyed]\nVoice = Localization/dialog/m/c\nTextKey = kc\n"
            "[both]\nVoice = x\nTextKey = kx\nDisplay = both\n"
            "[badcount]\nVoice = x, y, z\nTextKey = a, b\n"
            "[baddisplay]\nVoice = x\nDisplay = loud\n"
            "[orphan]\nVoice = x\nDisplay = subtitle\n"
            "[sound]\nFiles = s.ogg\n",
            ini
        );
        CHECK(parsed.lines.size() == 5);
        CHECK(parsed.sounds.size() == 1);
        const LineDef &hello = parsed.lines[0];
        CHECK(hello.voices.size() == 2 && hello.voices[1] == "Localization/dialog/m/a_02");
        CHECK(hello.text_keys.size() == 2 && hello.display == Display::Bubble);
        CHECK(hello.speaker == "soul_ui_name_guard" && hello.cooldown == 5.0f);
        CHECK(parsed.lines[1].display == Display::None);
        CHECK(parsed.lines[2].display == Display::Bubble);
        CHECK(parsed.lines[3].display == Display::Both);
        CHECK(parsed.lines[4].name == "orphan" && parsed.lines[4].display == Display::Subtitle);
        CHECK(mentions(parsed.problems, "2 TextKey(s) for 3 Voice take(s)"));
        CHECK(mentions(parsed.problems, "'loud' is not subtitle"));
        CHECK(mentions(parsed.problems, "[orphan] Display needs a TextKey"));

        const PackParse moods = parse_pack(
            "[grumpy]\nVoice = v\nMood = Angry\nMoodStrength = 0.5\nMoodPoses = L_browsLowerer:0.9, "
            "fa_pose_c_kiss:0.2\n"
            "[custom]\nVoice = v\nMoodPoses = R_cornerPull:0.4\n"
            "[badpose]\nVoice = v\nMoodPoses = L_browsFlying:0.5\n"
            "[badweight]\nVoice = v\nMoodPoses = C_kiss:2.5\n"
            "[crying]\nPoses = L_browsUp_in:0.9, R_browsUp_in:0.9\nMaxStrength = 0.4\n"
            "[brokenmood]\nPoses = L_nope:0.5\n",
            ini
        );
        CHECK(moods.lines.size() == 2);
        CHECK(moods.lines[0].mood_name == "Angry" && moods.lines[0].mood_strength == 0.5f);
        CHECK(moods.lines[0].mood_poses.size() == 2 && moods.lines[0].mood.empty());
        CHECK(moods.moods.size() == 1 && moods.moods[0].name == "crying" && moods.moods[0].poses.size() == 2);
        CHECK(moods.moods[0].max_strength == 0.4f);
        CHECK(mentions(moods.problems, "'L_browsFlying' is not a pose"));
        CHECK(mentions(moods.problems, "'2.5' is not a weight"));
        CHECK(mentions(moods.problems, "[brokenmood] Poses: 'L_nope' is not a pose"));
    }

    void test_moods()
    {
        wchar_t temp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, temp);
        const fs::path root = fs::path(temp) / L"sndfw_moods_test";
        fs::remove_all(root);
        const fs::path defaults = root / "KCD2_SoundFramework_moods.ini";
        write_file(
            defaults,
            "[angry]\nPoses = L_browsLowerer:0.7, R_browsLowerer:0.7\n[happy]\nPoses = L_cornerPull:0.8\n"
        );
        write_file(
            root / "Mods/m/SoundFramework/p.ini",
            "[happy]\nPoses = L_cornerPull:0.4\n"
            "[grumpy]\nVoice = v\nMood = Angry\nMoodPoses = L_browsLowerer:0.9, C_kiss:0.2\n"
            "[cheery]\nVoice = v\nMood = happy\n"
            "[calm]\nVoice = v\nMood = neutral\n"
            "[typo]\nVoice = v\nMood = angery\nMoodPoses = C_press:0.3\n"
        );
        const CatalogLoad load = load_catalog(root / "Mods", defaults);
        const auto weight_of = [&](std::string_view line, std::string_view pose)
        {
            for (const auto &p : load.catalog.lines.at(std::string(line)).mood)
            {
                if (p.pose == pose)
                {
                    return p.weight;
                }
            }
            return -1.0f;
        };
        CHECK(load.catalog.moods.size() == 2);
        CHECK(weight_of("grumpy", "L_browsLowerer") == 0.9f);
        CHECK(weight_of("grumpy", "R_browsLowerer") == 0.7f);
        CHECK(weight_of("grumpy", "C_kiss") == 0.2f);
        // The pack's happy overrides the defaults file's.
        CHECK(weight_of("cheery", "L_cornerPull") == 0.4f);
        CHECK(mentions(load.notes, "mood [happy]"));
        CHECK(load.catalog.lines.at("calm").mood.empty());
        CHECK(mentions(load.problems, "Mood 'angery' is not defined"));
        CHECK(load.catalog.lines.at("typo").mood.size() == 1);
        // The defaults file is watched like a pack file.
        CHECK(load.catalog.files.front().mod_id == "(defaults)");
        fs::remove_all(root);
    }

    void test_catalog()
    {
        wchar_t temp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, temp);
        const fs::path root = fs::path(temp) / L"sndfw_packs_test";
        fs::remove_all(root);
        write_file(root / "mod_order.txt", "zeta\n\nalpha\n");
        write_file(root / "alpha/SoundFramework/a.ini", "[bell]\nFiles = bell.ogg\n[ghost]\nFiles = none.ogg\n");
        write_file(root / "alpha/SoundFramework/bell.ogg", "x");
        write_file(root / "zeta/SoundFramework/z.ini", "[Bell]\nFiles = z.ogg\n[mixed]\nFiles = z.ogg, gone.ogg\n");
        write_file(root / "zeta/SoundFramework/z.ogg", "x");
        write_file(root / "beta/SoundFramework/b.INI", "[beta_only]\nFiles = z.ogg\n[greet]\nVoice = v_beta\n");
        write_file(root / "zeta/SoundFramework/lines.ini", "[Greet]\nVoice = v_zeta\n");
        write_file(root / "nopack/readme.txt", "x");

        const std::vector<PackFile> files = discover_packs(root);
        CHECK(files.size() == 4);
        CHECK(
            files.size() == 4 && files[0].mod_id == "zeta" && files[1].mod_id == "zeta" && files[2].mod_id == "alpha" &&
            files[3].mod_id == "beta"
        );

        const CatalogLoad load = load_catalog(root);
        // zeta loads first, so alpha's "bell" overrides zeta's "Bell".
        CHECK(load.catalog.sounds.size() == 2);
        CHECK(
            load.catalog.sounds.contains("bell") &&
            load.catalog.sounds.at("bell").files.front().filename() == "bell.ogg"
        );
        CHECK(load.catalog.sounds.contains("mixed") && load.catalog.sounds.at("mixed").files.size() == 1);
        CHECK(!load.catalog.sounds.contains("ghost"));
        CHECK(!load.catalog.sounds.contains("beta_only"));
        CHECK(mentions(load.notes, "overrides"));
        // beta loads after zeta (unlisted, alphabetical after the listed ones), so its greet wins.
        CHECK(load.catalog.lines.size() == 1 && load.catalog.lines.at("greet").voices.front() == "v_beta");
        CHECK(mentions(load.notes, "overrides the voice line"));
        CHECK(mentions(load.problems, "gone.ogg"));
        CHECK(mentions(load.problems, "[ghost] skipped"));
        CHECK(!catalog_changed(load.catalog));
        fs::last_write_time(
            root / "alpha/SoundFramework/a.ini",
            fs::file_time_type::clock::now() + std::chrono::seconds(5)
        );
        CHECK(catalog_changed(load.catalog));
        fs::remove_all(root);
        CHECK(catalog_changed(load.catalog));
    }

    void test_cutscene_table()
    {
        std::size_t rejected = 99;
        const CutsceneTable table = parse_cutscene_table(
            "# comment\n"
            "cin__Henry__ABcd\tspeaker\tcin_m01_henry, cin_m01_henry_clean\r\n"
            "cin__henry__abcd\tfaces\tcin_m02_guard\n"
            "puni_henry__dwij\tplayer\t\n"
            "\n"
            "broken_row\n"
            "no_actors\tspeaker\t\n"
            "odd_mode\tsinger\tsomeone\n",
            rejected
        );
        CHECK(rejected == 3);
        CHECK(table.size() == 2);
        const auto henry = table.find("cin__henry__abcd");
        CHECK(henry != table.end() && henry->second.size() == 2);
        if (henry != table.end() && henry->second.size() == 2)
        {
            CHECK(henry->second[0].mode == CutsceneMode::Speaker);
            CHECK(henry->second[0].actors.size() == 2 && henry->second[0].actors[1] == "cin_m01_henry_clean");
            CHECK(henry->second[1].mode == CutsceneMode::Faces && henry->second[1].actors.size() == 1);
        }
        const auto player = table.find("puni_henry__dwij");
        CHECK(player != table.end() && player->second[0].mode == CutsceneMode::Player);
        CHECK(player != table.end() && player->second[0].actors.empty());
        CHECK(cutscene_line_id("Localization\\dialog\\trosecko\\Cin__X_H7G1.ogg") == "cin__x_h7g1");
        CHECK(cutscene_line_id("Localization/dialog/x/setk_ptacek_Y1mG") == "setk_ptacek_y1mg");
    }
} // namespace

int main()
{
    test_ini_reader();
    test_pack_values();
    test_play_at_and_keep_original();
    test_replaces_voice();
    test_lines();
    test_moods();
    test_catalog();
    test_cutscene_table();
    if (s_failures != 0)
    {
        std::printf("%d check(s) failed\n", s_failures);
        return 1;
    }
    std::printf("all pack tests passed\n");
    return 0;
}
