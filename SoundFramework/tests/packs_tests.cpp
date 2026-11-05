/**
 * @file tests/packs_tests.cpp
 * @brief Unit tests of pack parsing and the catalog. Plain asserts, no framework: the exit code is the verdict.
 */

#include "packs/catalog.hpp"
#include "packs/cutscene_table.hpp"
#include "packs/ini_document.hpp"
#include "packs/sound_def.hpp"
#include "packs/voice_lines.hpp"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

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

    void test_adjusts()
    {
        const PackParse parsed = parse_pack(
            "[quiet_smithing]\nAdjusts = event:/Foleys/Professions/Blacksmith/*, event:/world/x\nVolume = 0.4\n"
            "[loud]\nAdjusts = event:/a\nVolume = 9\n"
            "[novolume]\nAdjusts = event:/a\n"
            "[notevent]\nAdjusts = foleys/x\nVolume = 0.5\n"
            "[empty]\nAdjusts =\nVolume = 0.5\n"
            "[extra]\nAdjusts = event:/a\nVolume = 0.5\nFiles = x.ogg\n",
            fs::path("C:/Mods/m/SoundFramework/p.ini")
        );
        // quiet_smithing, loud, novolume and extra; notevent and empty fail.
        CHECK(parsed.adjusts.size() == 4);
        CHECK(parsed.sounds.empty());
        const AdjustDef &smithing = parsed.adjusts[0];
        CHECK(smithing.events.size() == 2 && smithing.events[0] == "event:/foleys/professions/blacksmith/*");
        CHECK(smithing.volume == 0.4f);
        CHECK(parsed.adjusts[1].volume == 4.0f && mentions(parsed.problems, "clamped"));
        CHECK(parsed.adjusts[2].volume == 1.0f && mentions(parsed.problems, "[novolume] Adjusts without Volume"));
        CHECK(mentions(parsed.problems, "Adjusts item 'foleys/x' is not an event:/ path"));
        CHECK(mentions(parsed.problems, "[empty] Adjusts names no event"));
        CHECK(mentions(parsed.problems, "unknown key 'Files' ignored (adjustment)"));
        CHECK(wildcard_match(smithing.events[0], "event:/foleys/professions/blacksmith/bl_hammering"));
        CHECK(!wildcard_match(smithing.events[0], "event:/foleys/professions/lumberjack/saw_pull"));

        wchar_t temp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, temp);
        const fs::path root = fs::path(temp) / L"sndfw_adjusts_test";
        fs::remove_all(root);
        write_file(root / "mod_order.txt", "first\nsecond\n");
        write_file(
            root / "first/SoundFramework/a.ini",
            "[Quiet]\nAdjusts = event:/a\nVolume = 0.5\n[keep]\nAdjusts = event:/b\nVolume = 0.2\n"
        );
        write_file(root / "second/SoundFramework/b.ini", "[quiet]\nAdjusts = event:/a\nVolume = 0.8\n");
        const CatalogLoad load = load_catalog(root);
        // second's quiet replaces first's Quiet and moves to the end, so it wins events both name.
        CHECK(load.catalog.adjusts.size() == 2);
        CHECK(
            load.catalog.adjusts.size() == 2 && load.catalog.adjusts[0].name == "keep" &&
            load.catalog.adjusts[1].volume == 0.8f
        );
        CHECK(mentions(load.notes, "overrides the adjustment"));
        fs::remove_all(root);
    }

    void test_events()
    {
        const PackParse parsed = parse_pack(
            "[tick]\nEvents = event:/ui/skiptime/ui_skiptime_turn\nPitch = 0.85\n"
            "[mixed]\nFiles = a.ogg\nEvent = event:/ui/x, event:/ui/y\n"
            "[looped]\nEvents = event:/ui/x\nLoop = true\n"
            "[mixedloop]\nFiles = a.ogg\nEvents = event:/ui/x\nLoop = true\n"
            "[notevent]\nEvents = ui/skiptime\n"
            "[bareprefix]\nEvents = event:/\n",
            fs::path("C:/Mods/m/SoundFramework/p.ini")
        );
        CHECK(parsed.sounds.size() == 4); // tick, mixed, looped (Loop with Events only warns), mixedloop
        CHECK(parsed.sounds[0].files.empty() && parsed.sounds[0].events.size() == 1);
        CHECK(parsed.sounds[0].events[0] == "event:/ui/skiptime/ui_skiptime_turn" && parsed.sounds[0].pitch == 0.85f);
        CHECK(parsed.sounds[1].files.size() == 1 && parsed.sounds[1].events.size() == 2);
        CHECK(mentions(parsed.problems, "[looped] Loop and Stream apply to Files only"));
        CHECK(!mentions(parsed.problems, "[mixedloop]"));
        CHECK(mentions(parsed.problems, "Events item 'ui/skiptime' is not an event:/ path"));
        CHECK(mentions(parsed.problems, "Events item 'event:/' is not an event:/ path"));
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

        std::ifstream shipped(SNDFW_CUTSCENE_TABLE, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(shipped)), std::istreambuf_iterator<char>());
        const CutsceneTable built_in = parse_cutscene_table(text, rejected);
        CHECK(rejected == 0);
        CHECK(!built_in.empty());
    }
    void test_voice_lines()
    {
        const std::string texts = R"(<Table>
<Row><Cell>hello_AbCd</Cell><Cell>Hi</Cell><Cell>  Well&apos;   met &amp; good
day  </Cell></Row>
<Row><Cell>hm_XyZ1</Cell><Cell>Hm</Cell><Cell>Hm...</Cell></Row>
<Row><Cell>long_key</Cell><Cell>L</Cell><Cell>A longer line of text</Cell></Row>
<Row><Cell>guard_line</Cell><Cell>G</Cell><Cell>Halt, you!</Cell></Row>
<Row><Cell>empty_key</Cell><Cell>E</Cell><Cell></Cell></Row>
<Row><Cell>broken_key</Cell><Cell>only two</Cell></Row>
</Table>)";
        const std::string dialogue = R"(<Dialog>
<SelectedSoul Role="HENRY" Voice="tomMcKay"/>
<SelectedSoul Role="STRAZ_NA_BRANE" Voice="johnSmith"/>
<SelectedSoul Role="FAKE" Voice="fejk"/>
<Response Role="HENRY" Skip="1" />
<Response Role="HENRY"><Parenthetical><Text StringName="not_this"/></Parenthetical><Text StringName="HELLO_abcd"/></Response>
<Response Role="STRAZ_NA_BRANE"><Text StringName="guard_line"/></Response>
<Response Role="FAKE"><Text StringName="long_key"/></Response>
<Response Role="OTHER"><Text StringName="guard_line"/></Response>
</Dialog>)";
        VoiceLineBuilder builder;
        builder.add_texts(texts);
        builder.add_dialogue(dialogue);
        builder.add_dialogue("<Dialog><NoResponses/></Dialog>");
        builder.add_voice_file("dialog/town/gate/jsmi_guard_line.ogg");
        builder.add_voice_file("dialog/town/gate/tmck_hello_AbCd.ogg");
        builder.add_voice_file("dialog/town/tmck_hm_XyZ1.ogg");
        builder.add_voice_file("dialog/town/long_key.OGG");
        builder.add_voice_file("dialog/town/tmck_empty_key.ogg");
        builder.add_voice_file("dialog/town/tmck_broken_key.ogg");
        builder.add_voice_file("dialog/town/tmck_no_such_key.ogg");
        builder.add_voice_file("dialog/town/readme.txt");
        builder.add_voice_file("music/town/tmck_hm_XyZ1.ogg");
        const std::string text = builder.build();
        CHECK(
            text == "town/gate/tmck_hello_AbCd\tHenry (Tom McKay)\tWell' met & good day\n"
                    "town/tmck_hm_XyZ1\tTom McKay\tHm...\n"
                    "town/gate/jsmi_guard_line\tStraz Na Brane (John Smith)\tHalt, you!\n"
                    "town/long_key\tFake\tA longer line of text\n"
        );

        const VoiceLines lines{text};
        CHECK(lines.size() == 4);
        CHECK(lines.find("", 0, 1).total == 4);
        CHECK(
            lines.find("", 1, 2).lines ==
            "town/tmck_hm_XyZ1\tTom McKay\tHm...\ntown/gate/jsmi_guard_line\tStraz Na Brane (John Smith)\tHalt, you!\n"
        );
        CHECK(lines.find("", 3, 9).lines == "town/long_key\tFake\tA longer line of text\n");
        CHECK(lines.find("", 9, 9).lines.empty());
        CHECK(lines.find("MCKAY", 0, 9).total == 2);
        CHECK(lines.find("halt", 0, 9).total == 1);
        // A path is no part of the search, and a line with two matches counts once.
        CHECK(lines.find("town", 0, 9).total == 0);
        CHECK(lines.find("e", 0, 9).total == 3);
        CHECK(lines.find("e", 1, 1).lines == "town/gate/jsmi_guard_line\tStraz Na Brane (John Smith)\tHalt, you!\n");
        CHECK(lines.find("no such words", 0, 9).total == 0);
        CHECK(lines.line("TOWN/long_KEY") == "town/long_key\tFake\tA longer line of text");
        CHECK(!lines.line("town/long").has_value());

        const VoiceLines nothing{std::string{}};
        CHECK(nothing.size() == 0);
        CHECK(nothing.find("a", 0, 9).total == 0);
    }

    /** @brief Builds the catalog from the real game paks and writes it: <out> <text pak> <scripts pak> <voice paks...>.
     */
    int run_voice_lines(int argc, char **argv)
    {
        VoiceLineSources sources{.text_pak = argv[3], .dialogue_pak = argv[4], .voice_paks = {}};
        for (int i = 5; i < argc; ++i)
        {
            sources.voice_paks.emplace_back(argv[i]);
        }
        using Clock = std::chrono::steady_clock;
        const Clock::time_point started = Clock::now();
        std::optional<std::string> text = build_voice_lines(sources, [] { return false; });
        const Clock::duration elapsed = Clock::now() - started;
        if (!text.has_value())
        {
            std::printf("voice lines: a pak could not be read\n");
            return 1;
        }
        std::ofstream{argv[2], std::ios::binary} << *text;
        const std::size_t bytes = text->size();
        const Clock::time_point indexed = Clock::now();
        const VoiceLines lines{std::move(*text)};
        const Clock::duration index_elapsed = Clock::now() - indexed;
        std::printf(
            "voice lines: %zu lines, %zu bytes, built in %lld ms, indexed in %lld ms\n",
            lines.size(),
            bytes,
            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()),
            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(index_elapsed).count())
        );
        for (const std::string_view wanted :
             {std::string_view{"sword"}, std::string_view{"e"}, std::string_view{"zzzzqq"}})
        {
            const Clock::time_point searched = Clock::now();
            const VoiceLinePage page = lines.find(wanted, 0, 40);
            const Clock::duration search_elapsed = Clock::now() - searched;
            std::printf(
                "voice lines: '%.*s' matches %zu lines in %lld us\n",
                static_cast<int>(wanted.size()),
                wanted.data(),
                page.total,
                static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(search_elapsed).count())
            );
        }
        return 0;
    }
} // namespace

int main(int argc, char **argv)
{
    if (argc >= 6 && std::string_view{argv[1]} == "voice-lines")
    {
        return run_voice_lines(argc, argv);
    }
    test_ini_reader();
    test_pack_values();
    test_play_at_and_keep_original();
    test_adjusts();
    test_events();
    test_replaces_voice();
    test_lines();
    test_moods();
    test_catalog();
    test_cutscene_table();
    test_voice_lines();
    if (s_failures != 0)
    {
        std::printf("%d check(s) failed\n", s_failures);
        return 1;
    }
    std::printf("all pack tests passed\n");
    return 0;
}
