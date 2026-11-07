/**
 * @file tests/core_tests.cpp
 * @brief Unit tests of the game-independent code: text folds, clip path hashes, clip headers, the clip summary cache,
 *        zip entries, clip and skeleton readers, the clip compiler, the retargeter, the mod maker, the database deltas,
 *        the custom tag rules, the skeleton merge and the animation catalog.
 *
 * With arguments it runs one tool on real game files instead (see main for the forms):
 *   - `<adb|controllerdefs|animevents|tags|rows> <base file> <modded file> [out delta]` runs one file pair through its
 *     delta maker and prints what it found and how long it took (the merge runs at game load);
 *   - `clips`, `caf-dump`, `catalog` and `chr` measure clips, dump a clip's keys, build the catalog and merge
 *     skeletons.
 */

#include "check.hpp"
#include "core/anim_catalog.hpp"
#include "core/bridge_protocol.hpp"
#include "core/caf_reader.hpp"
#include "core/clip_check.hpp"
#include "core/clip_compiler.hpp"
#include "core/chr_merge.hpp"
#include "core/clip_cache.hpp"
#include "core/clip_header.hpp"
#include "core/game_paks.hpp"
#include "core/mod_tools.hpp"
#include "core/legacy_merge.hpp"
#include "core/mod_maker.hpp"
#include "core/tag_rules.hpp"
#include "core/text_util.hpp"
#include "core/zip_reader.hpp"
#include "core/zip_writer.hpp"
#include "retarget/mocap.hpp"
#include "retarget/retarget.hpp"

#include <process.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <utility>
#include <numbers>
#include <cstdio>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <vector>

namespace
{
    using namespace AnimationFramework;

    /// A zip that Python's zipfile wrote: a deflated (dynamic Huffman) .adb and a stored text file.
    constexpr std::uint8_t TEST_ZIP[] = {
        0x50, 0x4B, 0x03, 0x04, 0x14, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x21, 0x00, 0x05, 0x0A, 0x6A, 0x9A,
        0xA9, 0x01, 0x00, 0x00, 0x59, 0x19, 0x00, 0x00, 0x21, 0x00, 0x00, 0x00, 0x41, 0x6E, 0x69, 0x6D, 0x61, 0x74,
        0x69, 0x6F, 0x6E, 0x73, 0x2F, 0x4D, 0x61, 0x6E, 0x6E, 0x65, 0x71, 0x75, 0x69, 0x6E, 0x2F, 0x41, 0x44, 0x42,
        0x2F, 0x54, 0x65, 0x73, 0x74, 0x2E, 0x61, 0x64, 0x62, 0xBD, 0xD8, 0x3B, 0x6B, 0xC2, 0x50, 0x00, 0x86, 0xE1,
        0xDD, 0x5F, 0x11, 0xB2, 0x0A, 0x8D, 0xE7, 0x66, 0x5B, 0x88, 0x01, 0x25, 0x38, 0xB5, 0x5B, 0x77, 0x39, 0x68,
        0x94, 0x60, 0x72, 0x6A, 0xD5, 0xD2, 0xF6, 0xDF, 0xB7, 0x6A, 0x0B, 0x9D, 0x5A, 0xDE, 0x6F, 0x70, 0xCB, 0x8D,
        0x7C, 0xCB, 0xCB, 0x43, 0x48, 0x39, 0x4D, 0x6D, 0x5F, 0xCF, 0xB2, 0xF9, 0x3E, 0x6E, 0xEA, 0x66, 0x3D, 0xC9,
        0x4F, 0xE7, 0xF1, 0xD8, 0x3E, 0xA7, 0x43, 0xF1, 0x18, 0x53, 0x6A, 0x5E, 0x5E, 0xDB, 0x54, 0x4C, 0xEB, 0x59,
        0xB1, 0x5D, 0xAE, 0x16, 0x7D, 0xEC, 0x9A, 0xC5, 0xFA, 0xEB, 0xD1, 0xBE, 0x49, 0xC7, 0x76, 0x75, 0xB8, 0x79,
        0xEF, 0xBB, 0xBC, 0x1A, 0x64, 0xE5, 0xFC, 0xFB, 0x5A, 0xF6, 0x14, 0x37, 0x87, 0x49, 0xFE, 0x16, 0xBB, 0xED,
        0x70, 0xD7, 0xC5, 0x8F, 0x66, 0x3F, 0x3C, 0x8E, 0xF2, 0xF3, 0xDB, 0x2F, 0x77, 0xF2, 0xAA, 0x3C, 0x2D, 0x3C,
        0x9C, 0x6E, 0x5D, 0x0E, 0xCF, 0x63, 0x59, 0x8A, 0x7D, 0x33, 0xC9, 0x97, 0x5D, 0xBB, 0x5B, 0x8C, 0xF2, 0xA2,
        0x2A, 0x8B, 0x5F, 0x4F, 0x15, 0x3F, 0x6F, 0xFF, 0x6F, 0xC9, 0xC0, 0x25, 0x23, 0x2F, 0x59, 0xB8, 0x64, 0xE5,
        0x25, 0x07, 0x97, 0x9C, 0xBC, 0xE4, 0xE1, 0x92, 0x97, 0x97, 0x02, 0x5C, 0x0A, 0xF2, 0xD2, 0x18, 0x2E, 0x8D,
        0xE5, 0x25, 0x5A, 0xF9, 0xED, 0xD5, 0x2A, 0xBF, 0xBB, 0x5A, 0xE5, 0xF7, 0x57, 0xAB, 0xDC, 0xE8, 0x48, 0xD0,
        0xCC, 0x8D, 0xAE, 0x04, 0xED, 0xDC, 0xE8, 0x4C, 0xD0, 0xD0, 0x8D, 0xEE, 0x04, 0x2D, 0xDD, 0xE8, 0x50, 0x60,
        0xD0, 0x75, 0x29, 0x68, 0xEB, 0x46, 0xA7, 0x02, 0xC7, 0xAE, 0x5B, 0x81, 0x63, 0xD7, 0xB1, 0xC0, 0xB1, 0xEB,
        0x5A, 0xD0, 0xD8, 0xAD, 0xAE, 0x05, 0x8D, 0xDD, 0xEA, 0x5A, 0xD0, 0xD8, 0xAD, 0xAE, 0x05, 0xFE, 0x7C, 0xD1,
        0xB5, 0xA0, 0xB1, 0x5B, 0x5D, 0x0B, 0x1A, 0xBB, 0xD5, 0xB5, 0xA0, 0xB1, 0x5B, 0x5D, 0x0B, 0x1C, 0xBB, 0xAE,
        0x05, 0x8E, 0x5D, 0xD7, 0x02, 0xC7, 0xAE, 0x6B, 0x41, 0x63, 0x77, 0xBA, 0x16, 0xF8, 0x63, 0x5D, 0xD7, 0x82,
        0xC6, 0xEE, 0x74, 0x2D, 0x68, 0xEC, 0x4E, 0xD7, 0x82, 0xC6, 0xEE, 0x74, 0x2D, 0x68, 0xEC, 0x4E, 0xD7, 0x82,
        0xC6, 0xEE, 0x74, 0x2D, 0x70, 0xEC, 0xBA, 0x16, 0x38, 0x76, 0x5D, 0x0B, 0x1C, 0xBB, 0xAE, 0x05, 0x8D, 0xDD,
        0xEB, 0x5A, 0xD0, 0xD8, 0xBD, 0xAE, 0x05, 0x8D, 0xDD, 0xEB, 0x5A, 0xD0, 0xD8, 0xBD, 0xAE, 0x05, 0x8D, 0xDD,
        0xEB, 0x5A, 0xD0, 0xD8, 0xBD, 0xAE, 0x05, 0xFE, 0x0F, 0xA3, 0x6B, 0x81, 0x63, 0xD7, 0xB5, 0xC0, 0xB1, 0xEB,
        0x5A, 0xE0, 0xD8, 0x75, 0x2D, 0x68, 0xEC, 0x41, 0xD7, 0x82, 0xC6, 0x1E, 0x74, 0x2D, 0x68, 0xEC, 0x41, 0xD7,
        0x82, 0xC6, 0x1E, 0x74, 0x2D, 0xF0, 0x5F, 0x47, 0x5D, 0x0B, 0x1A, 0x7B, 0xD0, 0xB5, 0xA0, 0xB1, 0x07, 0x5D,
        0x0B, 0x1C, 0xBB, 0xAE, 0x05, 0x8E, 0x5D, 0xD7, 0x02, 0xC7, 0xFE, 0x97, 0x16, 0x97, 0xEB, 0xF5, 0xAC, 0x1A,
        0x7C, 0x02, 0x50, 0x4B, 0x03, 0x04, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0x00, 0x78, 0x77,
        0xA6, 0x0F, 0x0C, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x15, 0x00, 0x00, 0x00, 0x61, 0x6E, 0x69, 0x6D,
        0x61, 0x74, 0x69, 0x6F, 0x6E, 0x73, 0x2F, 0x73, 0x74, 0x6F, 0x72, 0x65, 0x64, 0x2E, 0x74, 0x78, 0x74, 0x73,
        0x74, 0x6F, 0x72, 0x65, 0x64, 0x20, 0x62, 0x79, 0x74, 0x65, 0x73, 0x50, 0x4B, 0x01, 0x02, 0x14, 0x00, 0x14,
        0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x21, 0x00, 0x05, 0x0A, 0x6A, 0x9A, 0xA9, 0x01, 0x00, 0x00, 0x59,
        0x19, 0x00, 0x00, 0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x41, 0x6E, 0x69, 0x6D, 0x61, 0x74, 0x69, 0x6F, 0x6E, 0x73, 0x2F, 0x4D, 0x61, 0x6E, 0x6E,
        0x65, 0x71, 0x75, 0x69, 0x6E, 0x2F, 0x41, 0x44, 0x42, 0x2F, 0x54, 0x65, 0x73, 0x74, 0x2E, 0x61, 0x64, 0x62,
        0x50, 0x4B, 0x01, 0x02, 0x14, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0x00, 0x78, 0x77,
        0xA6, 0x0F, 0x0C, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x80, 0x01, 0xE8, 0x01, 0x00, 0x00, 0x61, 0x6E, 0x69, 0x6D, 0x61, 0x74, 0x69, 0x6F,
        0x6E, 0x73, 0x2F, 0x73, 0x74, 0x6F, 0x72, 0x65, 0x64, 0x2E, 0x74, 0x78, 0x74, 0x50, 0x4B, 0x05, 0x06, 0x00,
        0x00, 0x00, 0x00, 0x02, 0x00, 0x02, 0x00, 0x92, 0x00, 0x00, 0x00, 0x27, 0x02, 0x00, 0x00, 0x00, 0x00,
    };

    /** @brief The deflated entry's text, built the same way the zip's writer built it. */
    [[nodiscard]] std::string deflated_text()
    {
        std::string text = "<AnimDB FragDef=\"Animations/Mannequin/ADB/kcd_male_fragmentids.xml\">\n";
        for (int i = 0; i < 60; ++i)
        {
            text += " <Fragment Tags=\"walk+player+t" + std::to_string(i % 7) +
                    "\" FragTags=\"\"><AnimLayer><Animation name=\"clip_" + std::to_string(i) +
                    "\"/></AnimLayer></Fragment>\n";
        }
        text += "</AnimDB>\n";
        return text;
    }

    /** @brief A file path unique to this process and call, removed by the caller. */
    [[nodiscard]] std::filesystem::path temp_file(std::string_view stem)
    {
        static std::atomic<int> counter{0};
        std::error_code error;
        return std::filesystem::temp_directory_path(error) /
               (std::string{stem} + "_" + std::to_string(_getpid()) + "_" + std::to_string(++counter) + ".zip");
    }

    void test_text_util()
    {
        CHECK(is_mod_id("gesture_wheel"));
        CHECK(is_mod_id("my_mod"));
        CHECK(!is_mod_id("mod2"));
        CHECK(!is_mod_id(""));
        CHECK(!is_mod_id("Gesture"));
        CHECK(!is_mod_id("../other"));
        CHECK(!is_mod_id("a/b"));
        CHECK(!is_mod_id(std::string(65, 'a')));
        CHECK(tag_set("B+a++") == "a+b");
        CHECK(tag_set("+Walk+player+") == tag_set("PLAYER+walk"));
        CHECK(tag_set("").empty());
        CHECK((split_tags("+Walk++player+") == std::vector<std::string>{"Walk", "player"}));
        CHECK(split_tags("+").empty());
        CHECK(fold_path("Animations\\Human\\Male\\A.CAF") == "animations/human/male/a.caf");
        CHECK(lowercase("Animations/MALE.ADB") == "animations/male.adb");
        CHECK(path_ends_with("Animations\\Mannequin\\ADB\\Kcd_Male.ADB", ".adb"));
        CHECK(
            path_ends_with("x/animations/mannequin/adb/kcd_male_tags.xml", "animations/mannequin/adb/kcd_male_tags.xml")
        );
        CHECK(path_ends_with(
            "Animations\\Mannequin\\ADB\\KCD_male_tags.xml",
            "animations/mannequin/adb/kcd_male_tags.xml"
        ));
        CHECK(!path_ends_with("adb", ".adb"));
        CHECK(path_contains("Animations\\MANNEQUIN\\adb\\x.xml", "mannequin/adb/"));
        CHECK(!path_contains("Animations/Mannequin/x.xml", "mannequin/adb/"));
        CHECK(path_equals("A\\b", "a/B"));
        CHECK(!path_equals("ab", "abc"));

        // A mod pak mounts relative to the game root, or absolute under it; only the part after the root counts.
        CHECK(pak_mod_id("mods\\my_mod\\data\\my_mod.pak", "D:\\Games\\KCD2") == "my_mod");
        CHECK(pak_mod_id("Mods\\My_Mod\\Data\\x.pak", "") == "my_mod");
        CHECK(pak_mod_id("d:\\games\\kcd2\\mods\\my_mod\\data\\x.pak", "D:\\Games\\KCD2\\") == "my_mod");
        CHECK(pak_mod_id("D:/Games/KCD2/Mods/my_mod/Data/x.pak", "d:\\games\\kcd2") == "my_mod");
        CHECK(pak_mod_id("d:\\mods\\kcd2\\data\\ipl_gamedata.pak", "D:\\mods\\KCD2").empty());
        CHECK(pak_mod_id("data\\animations.pak", "D:\\mods\\KCD2").empty());
        CHECK(pak_mod_id("d:\\other\\mods\\my_mod\\data\\x.pak", "D:\\Games\\KCD2").empty());
        CHECK(pak_mod_id("d:\\games\\kcd2x\\mods\\my_mod\\data\\x.pak", "D:\\Games\\KCD2").empty());
        CHECK(pak_mod_id("mods\\my_mod", "").empty());
        CHECK(pak_mod_id("mods\\\\data\\x.pak", "").empty());
    }

    void test_bridge_protocol()
    {
        using namespace AnimationFramework::bridge;
        CHECK(is_fragment_name("MyMod_Wave2"));
        CHECK(!is_fragment_name("2Wave"));
        CHECK(!is_fragment_name("Wave\")"));
        CHECK(!is_fragment_name(""));
        CHECK(!is_fragment_name(std::string(MAX_NAME_LENGTH + 1, 'a')));
        CHECK(is_clip_name("wave_01"));
        CHECK(!is_clip_name("Wave"));
        CHECK(!is_clip_name("wave;x"));

        const auto hello = parse_request("hello");
        CHECK(hello && hello->operation == Operation::Hello);
        CHECK(!parse_request("hello\tx"));
        CHECK(!parse_request(""));
        CHECK(!parse_request("run\tprint(1)"));
        CHECK(!parse_request(std::string(MAX_REQUEST_BYTES + 1, 'a')));

        const auto make = parse_request("make\tmy_mod\tMyMod_Wave\tmy_mod_wave\tADLG_Wave\t1\t0");
        CHECK(make && make->operation == Operation::Make && make->modid == "my_mod" && make->fragment == "MyMod_Wave");
        CHECK(make && make->clip == "my_mod_wave" && make->like == "ADLG_Wave" && make->loop && !make->upper);
        CHECK(!parse_request("make\tmy_mod2\tMyMod_Wave\tmy_mod_wave\tADLG_Wave\t1\t0"));
        CHECK(!parse_request("make\tmy_mod\tMyMod_Wave\tmy_mod_wave\tADLG_Wave\t1\t2"));
        CHECK(!parse_request("make\tmy_mod\tMyMod_Wave; x\tmy_mod_wave\tADLG_Wave\t1\t0"));
        CHECK(!parse_request("make\tmy_mod\tMyMod_Wave\tmy_mod_wave\tADLG_Wave\t1"));
        CHECK(!parse_request("make\tmy_mod\tMyMod_Wave\tmy_mod_wave\tADLG_Wave\t1\t0\t0"));

        const auto play = parse_request("play\tMyMod_Wave\t1");
        CHECK(play && play->operation == Operation::Play && play->fragment == "MyMod_Wave" && play->upper);
        CHECK(!parse_request("play\tMyMod_Wave\")\t0"));
        const auto stop = parse_request("stop");
        CHECK(stop && stop->operation == Operation::Stop);

        // The Lua matches the add-on's Lua runner fallback, inside the world check and the error hand-off.
        const std::string played = lua_for(*play);
        CHECK(played.contains("player.human:PlayAnim(\"MyMod_Wave\", \"animfw_own_scopes\")"));
        CHECK(played.contains("pcall(function()"));
        CHECK(played.contains("System.SetCVar(\"animfw_bridge_error\", tostring(message))"));
        CHECK(played.contains("error(\"no game world: load a save first\", 0)"));
        CHECK(lua_for(*parse_request("play\tMyMod_Wave\t0")).contains("PlayAnim(\"MyMod_Wave\", \"\")"));
        CHECK(lua_for(*stop).contains("player.human:StopAnim()"));
        CHECK(lua_for(*make).contains(
            "System.ExecuteCommand(\"animfw_make_mod my_mod MyMod_Wave my_mod_wave ADLG_Wave loop\")"
        ));
        CHECK(lua_for(*parse_request("make\tmy_mod\tA\tb\tC\t0\t1")).contains("animfw_make_mod my_mod A b C upper\")"));
        CHECK(lua_for(*parse_request("reload_clips")).contains("System.ExecuteCommand(\"animfw_reload_clips\")"));

        CHECK(answer(true, "done") == "ok\tdone");
        CHECK(answer(false, "a\tb\nc") == "error\ta b c");
    }

    void test_clip_path_hash()
    {
        // CityHash64 v1.1 of the lowercased path, from the reference implementation: the keys the game's headers hold.
        struct Vector
        {
            std::string_view path;
            std::uint64_t hash;
        };
        constexpr Vector VECTORS[] = {
            {"", 0x9AE16A3B2F90404FULL},
            {"a", 0xB3454265B6DF75E3ULL},
            {"abc", 0x24A5B3A074E7F369ULL},
            {"Anim", 0xB2F6182811A3B581ULL},
            {"animations/a.caf", 0x51F50EBAE236F0B6ULL},
            {"Animations/Human/Male/relaxed_walk.caf", 0xE8E90D24F3CB5CD8ULL},
            {"animations/animals/cat/relaxed_idle_gallop.caf", 0xD63CD6E5F82332D6ULL},
            {"animations/human/male/locomotion/relaxed/relaxed_walk_medium_player_forward_01.caf",
             0x1BEE02E5AFC0D77DULL},
            {"Animations/Human/Male/Combat/Sword/Attacks/"
             "combat_sword_attack_left_to_right_strong_variant_02_player.caf",
             0xE564D42F582E4AF2ULL},
        };
        for (const Vector &vector : VECTORS)
        {
            CHECK(clip_path_hash(vector.path) == vector.hash);
        }
    }

    void test_clip_header()
    {
        std::array<std::byte, CAF_MOTION_CHUNK_SIZE> motion{};
        const std::uint32_t flags = 0x10;
        const std::int32_t start_key = 15;
        const std::int32_t end_key = 45;
        std::memcpy(motion.data(), &flags, sizeof(flags));
        std::memcpy(motion.data() + 16, &start_key, sizeof(start_key));
        std::memcpy(motion.data() + 20, &end_key, sizeof(end_key));

        const std::optional<ClipHeader> header = build_clip_header(motion, 3, "Animations\\Test\\Clip.caf");
        CHECK(header.has_value());
        if (!header)
        {
            return;
        }
        std::uint32_t stored_flags = 0;
        float duration = 0.0F;
        std::uint32_t controllers = 0;
        std::memcpy(&stored_flags, header->data(), sizeof(stored_flags));
        std::memcpy(&duration, header->data() + CLIP_HEADER_DURATION_OFFSET, sizeof(duration));
        std::memcpy(&controllers, header->data() + CLIP_HEADER_CONTROLLERS_OFFSET, sizeof(controllers));
        CHECK(stored_flags == (flags | 0x2000));
        CHECK(duration == static_cast<float>(end_key - start_key) * (1.0F / 30.0F));
        CHECK(controllers == 3);
        CHECK(clip_header_path(*header) == "animations/test/clip.caf");

        CHECK(!build_clip_header(std::span{motion}.first(CAF_MOTION_CHUNK_SIZE - 1), 3, "a.caf").has_value());
        CHECK(!build_clip_header(motion, 3, std::string(CLIP_HEADER_PATH_SIZE, 'a')).has_value());
        CHECK(!build_clip_header(motion, 3, "").has_value());

        // Keys at the ends of the int32 range: the key span is taken in 64 bits instead of overflowing.
        std::array<std::byte, CAF_MOTION_CHUNK_SIZE> wide = motion;
        const std::int32_t lowest = std::numeric_limits<std::int32_t>::min();
        const std::int32_t highest = std::numeric_limits<std::int32_t>::max();
        std::memcpy(wide.data() + 16, &lowest, sizeof(lowest));
        std::memcpy(wide.data() + 20, &highest, sizeof(highest));
        const std::optional<ClipHeader> wide_header = build_clip_header(wide, 3, "a.caf");
        float wide_duration = 0.0F;
        if (wide_header)
        {
            std::memcpy(&wide_duration, wide_header->data() + CLIP_HEADER_DURATION_OFFSET, sizeof(wide_duration));
        }
        CHECK(wide_header.has_value() && wide_duration == static_cast<float>(4294967295LL) * (1.0F / 30.0F));

        // A one-entry CrCh 0x746 image: header, chunk table, then the entry.
        std::vector<std::byte> image(16 + 16 + CLIP_HEADER_SIZE);
        const auto put = [&image](std::size_t at, auto value)
        { std::memcpy(image.data() + at, &value, sizeof(value)); };
        std::memcpy(image.data(), "CrCh", 4);
        put(4, std::uint32_t{0x746});
        put(8, std::uint32_t{1});
        put(12, std::uint32_t{16});
        put(16, static_cast<std::uint16_t>(CLIP_HEADER_CHUNK_TYPE));
        put(18, static_cast<std::uint16_t>(CLIP_HEADER_CHUNK_VERSION));
        put(24, static_cast<std::uint32_t>(CLIP_HEADER_SIZE));
        put(28, std::uint32_t{32});
        std::memcpy(image.data() + 32, header->data(), CLIP_HEADER_SIZE);
        const std::vector<ClipHeader> entries = read_clip_image(image);
        CHECK(entries.size() == 1);
        CHECK(!entries.empty() && entries.front() == *header);

        image[0] = std::byte{'X'};
        CHECK(read_clip_image(image).empty());
    }

    void test_zip_reader()
    {
        const std::filesystem::path path = temp_file("animfw_core_tests");
        {
            std::ofstream out{path, std::ios::binary};
            out.write(reinterpret_cast<const char *>(TEST_ZIP), static_cast<std::streamsize>(sizeof(TEST_ZIP)));
        }
        const std::optional<std::vector<ZipEntry>> index = read_zip_index(path);
        CHECK(index.has_value() && index->size() == 2);
        if (index && index->size() == 2)
        {
            const ZipEntry &deflated = (*index)[0];
            const ZipEntry &stored = (*index)[1];
            CHECK(deflated.name == "animations/mannequin/adb/test.adb");
            CHECK(deflated.method == 8);
            CHECK(stored.method == 0);
            CHECK(read_zip_file(path, deflated) == deflated_text());
            CHECK(read_zip_file(path, stored) == std::string{"stored bytes"});

            ZipEntry corrupt = deflated;
            corrupt.crc32 ^= 1;
            CHECK(!read_zip_file(path, corrupt).has_value());
            ZipEntry short_entry = deflated;
            short_entry.size -= 1;
            CHECK(!read_zip_file(path, short_entry).has_value());
            // A directory entry whose data runs past the end of the file fails before its buffer is allocated.
            ZipEntry past_end = deflated;
            past_end.compressed_size = 0x10000000U;
            past_end.size = 0x10000000U;
            CHECK(!read_zip_file(path, past_end).has_value());
        }
        std::error_code error;
        std::filesystem::remove(path, error);
        CHECK(!read_zip_index(path).has_value());

        // The CRC-32 check value of "123456789".
        constexpr std::string_view CHECK_TEXT = "123456789";
        CHECK(zip_crc32({reinterpret_cast<const std::uint8_t *>(CHECK_TEXT.data()), CHECK_TEXT.size()}) == 0xCBF43926U);
    }

    void test_zip_writer()
    {
        const std::vector<ZipSource> files{
            {.name = "animations/humans/male/my_mod/wave.caf", .data = std::string{"CrCh\0\x46\x07", 7}},
            {.name = "animations/humans/male/my_mod/empty.caf", .data = {}},
        };
        const std::optional<std::string> zip = write_stored_zip(files);
        CHECK(zip.has_value());
        const std::filesystem::path path = temp_file("animfw_core_tests_zip");
        {
            std::ofstream out{path, std::ios::binary};
            out.write(zip->data(), static_cast<std::streamsize>(zip->size()));
        }
        const std::optional<std::vector<ZipEntry>> index = read_zip_index(path);
        CHECK(index.has_value() && index->size() == files.size());
        if (index && index->size() == files.size())
        {
            for (std::size_t i = 0; i < files.size(); ++i)
            {
                const ZipEntry &entry = (*index)[i];
                CHECK(entry.name == files[i].name);
                CHECK(entry.method == 0);
                CHECK(entry.size == files[i].data.size());
                CHECK(read_zip_file(path, entry) == files[i].data);
            }
        }
        CHECK(write_stored_zip({}).has_value());
        std::error_code error;
        std::filesystem::remove(path, error);

        // The general-purpose flags sit at +6 of a local header and +8 of a central record. Only a well-formed UTF-8
        // name with a byte past ASCII gets bit 11.
        using Flags = std::array<std::uint16_t, 2>;
        const auto name_flags = [](const std::string &name) -> Flags
        {
            const std::vector<ZipSource> one{
                {.name = name, .data = "x"},
            };
            const std::optional<std::string> written = write_stored_zip(one);
            if (!written)
            {
                return Flags{0xFFFF, 0xFFFF};
            }
            const auto at = [&written](std::size_t offset) -> std::uint16_t
            {
                return static_cast<std::uint16_t>(
                    static_cast<std::uint8_t>((*written)[offset]) |
                    (static_cast<std::uint8_t>((*written)[offset + 1]) << 8U)
                );
            };
            const std::size_t central = 30 + name.size() + 1;
            return Flags{at(6), at(central + 8)};
        };
        const Flags utf8{0x0800, 0x0800};
        const Flags none{0, 0};
        CHECK(name_flags("animations/a.caf") == none);
        CHECK(name_flags("animations/\xC3\xA9.caf") == utf8);
        CHECK(name_flags("animations/\xF0\x9F\x98\x80.caf") == utf8);
        CHECK(name_flags("animations/\xE9.caf") == none);
        CHECK(name_flags("animations/\xC0\xAF.caf") == none);
        CHECK(name_flags("animations/\xED\xA0\x80.caf") == none);
        CHECK(name_flags("animations/\xE2\x82") == none);
    }

    void test_adb_delta()
    {
        const std::string base =
            "<AnimDB>\n"
            " <FragmentList>\n"
            "  <Idle><Fragment Tags=\"Idle+Player\" FragTags=\"\"><Animation name=\"a\"/></Fragment>"
            "<Fragment Tags=\"Crouch\"><Animation name=\"c\"/></Fragment></Idle>\n"
            " </FragmentList>\n"
            " <FragmentBlendList><Blend from=\"Idle\" to=\"Walk\"><Variant from=\"\" to=\"\">"
            "<Fragment/></Variant></Blend></FragmentBlendList>\n"
            " <SubADBs><SubADB File=\"Animations/Mannequin/ADB/Combat.adb\"/></SubADBs>\n"
            "</AnimDB>\n";

        // The same document with attribute order and case changes only.
        const std::string same =
            "<?xml version=\"1.0\"?>\n<AnimDB>\n"
            " <FragmentList>\n"
            "  <Idle><Fragment FragTags=\"\" Tags=\"Idle+Player\"><Animation name=\"a\"/></Fragment>"
            "<Fragment Tags=\"Crouch\"><Animation name=\"c\"/></Fragment></Idle>\n"
            " </FragmentList>\n"
            " <FragmentBlendList><Blend from=\"idle\" to=\"WALK\"><Variant from=\"\" to=\"\">"
            "<Fragment/></Variant></Blend></FragmentBlendList>\n"
            " <SubADBs><SubADB File=\"animations/mannequin/adb/combat.adb\"/></SubADBs>\n"
            "</AnimDB>\n";
        const std::optional<AdbDelta> unchanged = make_adb_delta(base, same);
        CHECK(unchanged.has_value() && unchanged->empty());

        // The game's merge matches fragment names without case, so a copy that spells the fragment `<idle>` holds
        // the same options.
        std::string respelled_fragment = same;
        respelled_fragment.replace(respelled_fragment.find("<Idle>"), 6, "<idle>");
        respelled_fragment.replace(respelled_fragment.find("</Idle>"), 7, "</idle>");
        const std::optional<AdbDelta> case_only = make_adb_delta(base, respelled_fragment);
        CHECK(case_only.has_value() && case_only->empty());

        // One option changed, one dropped (the PI turns it off), a blend variant removed, a sub-ADB added.
        const std::string modded =
            "<AnimDB>\n"
            " <FragmentList>\n"
            "  <Idle><Fragment Tags=\"Idle+Player\" FragTags=\"\"><Animation name=\"b\"/></Fragment>"
            "<?Fragment Tags=\"Crouch\"><Animation name=\"c\"/></Fragment?></Idle>\n"
            " </FragmentList>\n"
            " <FragmentBlendList><Blend from=\"Idle\" to=\"Walk\"/></FragmentBlendList>\n"
            " <SubADBs><SubADB File=\"Animations/Mannequin/ADB/Combat.adb\"/>"
            "<SubADB File=\"Animations/Mannequin/ADB/Mod.adb\"/></SubADBs>\n"
            "</AnimDB>\n";
        const std::optional<AdbDelta> delta = make_adb_delta(base, modded);
        CHECK(delta.has_value());
        if (delta)
        {
            CHECK(delta->options == 1);
            CHECK(delta->removed_options == 1);
            CHECK(delta->variants == 0);
            CHECK(delta->removed_variants == 1);
            CHECK(delta->sub_adbs == 1);
            CHECK(delta->text.find("<Animation name=\"b\"/>") != std::string::npos);
            CHECK(delta->text.find("Tags=\"Crouch\" Mode=\"remove\"") != std::string::npos);
            CHECK(delta->text.find("Mod.adb") != std::string::npos);
            CHECK(delta->text.find("Combat.adb") == std::string::npos);
        }

        CHECK(!make_adb_delta(base, "<NotAnimDB/>").has_value());
        CHECK(!make_adb_delta("<AnimDB><Broken", modded).has_value());

        // An attribute value that spells a child element is not that child: the option changed.
        const std::string spelled =
            "<AnimDB><FragmentList><Idle><Fragment Tags=\"t\" a=\"v(B)\"/></Idle></FragmentList></AnimDB>";
        const std::string nested =
            "<AnimDB><FragmentList><Idle><Fragment Tags=\"t\" a=\"v\"><B/></Fragment></Idle></FragmentList></AnimDB>";
        const std::optional<AdbDelta> respelled = make_adb_delta(spelled, nested);
        CHECK(respelled.has_value() && respelled->options == 1);
    }

    void test_controller_defs_delta()
    {
        using AnimationFramework::ControllerDefsDelta;
        using AnimationFramework::make_controller_defs_delta;
        const std::string base = "<ControllerDef>\n"
                                 " <Tags filename=\"t.xml\"/>\n"
                                 " <FragmentDefs>\n"
                                 "  <FreeAttack scopes=\"UpperBody+LookPose\" flags=\"AutoReinstall\">\n"
                                 "   <Override tags=\"l_shield\" fragTags=\"slash+r_swords\" scopes=\"UpperBody\"/>\n"
                                 "   <Override tags=\"\" fragTags=\"kick\" scopes=\"FullBody\"/>\n"
                                 "  </FreeAttack>\n"
                                 "  <MotionIdle scopes=\"FullBody\"/>\n"
                                 " </FragmentDefs>\n"
                                 " <ScopeContextDefs><Char3P/></ScopeContextDefs>\n"
                                 " <ScopeDefs><FullBody layer=\"0\" numLayers=\"3\" context=\"Char3P\"/></ScopeDefs>\n"
                                 "</ControllerDef>\n";
        const std::optional<ControllerDefsDelta> unchanged = make_controller_defs_delta(base, base);
        CHECK(unchanged.has_value() && unchanged->empty());
        std::string respelled = base;
        respelled.replace(respelled.find("<FreeAttack"), 11, "<freeattack");
        respelled.replace(respelled.find("</FreeAttack>"), 13, "</freeattack>");
        const std::optional<ControllerDefsDelta> case_only = make_controller_defs_delta(base, respelled);
        CHECK(case_only.has_value() && case_only->empty());

        // An override changed (tag order differs but the set is the same key), one dropped, one added; MotionIdle's
        // flags changed; a new fragment definition, scope and context.
        const std::string modded =
            "<ControllerDef>\n"
            " <FragmentDefs>\n"
            "  <FreeAttack scopes=\"UpperBody+LookPose\" flags=\"AutoReinstall\">\n"
            "   <Override tags=\"l_shield\" fragTags=\"r_swords+slash\" scopes=\"FullBody+UpperBody\"/>\n"
            "   <Override tags=\"thirdPerson\" fragTags=\"slash\" scopes=\"FullBody+UpperBody\"/>\n"
            "  </FreeAttack>\n"
            "  <MotionIdle scopes=\"FullBody\" flags=\"Persistent\"/>\n"
            "  <ModEmote scopes=\"FullBody\"/>\n"
            " </FragmentDefs>\n"
            " <ScopeContextDefs><Char3P/><ModProp/></ScopeContextDefs>\n"
            " <ScopeDefs><FullBody layer=\"0\" numLayers=\"3\" context=\"Char3P\"/>"
            "<ModScope layer=\"16\" numLayers=\"1\" context=\"ModProp\"/></ScopeDefs>\n"
            "</ControllerDef>\n";
        const std::optional<ControllerDefsDelta> delta = make_controller_defs_delta(base, modded);
        CHECK(delta.has_value());
        if (delta)
        {
            CHECK(delta->overrides == 2);         // FreeAttack: the changed l_shield one and the new thirdPerson one
            CHECK(delta->removed_overrides == 1); // the kick override
            CHECK(delta->fragment_defs == 2);     // MotionIdle flags, ModEmote added
            CHECK(delta->scopes == 1);
            CHECK(delta->scope_contexts == 1);
            CHECK(delta->text.find("fragTags=\"kick\" Mode=\"remove\"") != std::string::npos);
            CHECK(delta->text.find("flags=\"Persistent\" Mode=\"replace\"") != std::string::npos);
            CHECK(delta->text.find("<ModScope") != std::string::npos);
            CHECK(delta->text.find("<FullBody") == std::string::npos);
        }
        CHECK(!make_controller_defs_delta(base, "<AnimDB/>").has_value());
    }

    void test_anim_events_delta()
    {
        using AnimationFramework::anim_event_key;
        using AnimationFramework::AnimEventsDelta;
        using AnimationFramework::make_anim_events_delta;
        CHECK(anim_event_key("./Animations\\Humans/Male/A.caf") == "animations/humans/male/a.caf");
        const std::string base =
            "<?xml version=\"1.0\"?>\n<anim_event_list>\n"
            " <animation name=\"animations/a.caf\"><event name=\"foley\" time=\"0.1\"/></animation>\n"
            " <animation name=\"animations/b.caf\"><event name=\"foley\" time=\"0.2\"/></animation>\n"
            " <animation name=\"animations/c.caf\"><event name=\"sound\" time=\"0.3\"/></animation>\n"
            "</anim_event_list>\n";
        const std::optional<AnimEventsDelta> unchanged = make_anim_events_delta(base, base);
        CHECK(unchanged.has_value() && unchanged->empty());
        // b changed (case-insensitive path keeps the key), c dropped, d added; an empty duplicate of a is ignored.
        const std::string modded =
            "<anim_event_list>\n"
            " <animation name=\"animations/a.caf\"/>\n"
            " <animation name=\"Animations/A.caf\"><event name=\"foley\" time=\"0.1\"/></animation>\n"
            " <animation name=\"Animations\\B.caf\"><event name=\"foley\" time=\"0.25\"/></animation>\n"
            " <animation name=\"animations/d.caf\"><event name=\"swish\" time=\"0.4\"/></animation>\n"
            "</anim_event_list>\n";
        const std::optional<AnimEventsDelta> delta = make_anim_events_delta(base, modded);
        CHECK(delta.has_value());
        if (delta)
        {
            CHECK(delta->animations == 3); // A.caf (its name differs in case), B.caf, d.caf
            CHECK(delta->removed == 1);
            CHECK(delta->text.find("time=\"0.25\"") != std::string::npos);
            CHECK(delta->text.find("name=\"animations/c.caf\" Mode=\"remove\"") != std::string::npos);
        }
        CHECK(!make_anim_events_delta(base, "<AnimDB/>").has_value());
    }

    void test_tag_definition_delta()
    {
        using AnimationFramework::make_tag_definition_delta;
        using AnimationFramework::TagDefinitionDelta;
        const std::string base =
            "<TagDefinition version=\"2\">\n"
            " <Imports><Import filename=\"Animations/Mannequin/ADB/tests_male_fragmentids.xml\"/>"
            "</Imports>\n"
            " <Tags>\n"
            "  <Tag name=\"MotionIdle\"/>\n"
            "  <Group name=\"Stance\"><Tag name=\"horse\" priority=\"5\"/><Tag name=\"sitting\"/></Group>\n"
            "  <Tag name=\"player\"/>\n"
            " </Tags>\n"
            "</TagDefinition>\n";
        const std::optional<TagDefinitionDelta> unchanged = make_tag_definition_delta(base, base);
        CHECK(unchanged.has_value() && unchanged->empty());

        // Two new fragment IDs (one differing only in case from nothing stock), a tag added to the Stance group, a new
        // group, a new import; horse's priority changed and player dropped (neither carried).
        const std::string modded =
            "<TagDefinition version=\"2\">\n"
            " <Imports><Import filename=\"animations/mannequin/adb/TESTS_male_fragmentids.xml\"/>"
            "<Import filename=\"Animations/Mannequin/ADB/mod_fragmentids.xml\"/></Imports>\n"
            " <Tags>\n"
            "  <Tag name=\"motionidle\"/>\n"
            "  <Group name=\"stance\"><Tag name=\"horse\" priority=\"6\"/><Tag name=\"sitting\"/>"
            "<Tag name=\"kneeling\"/></Group>\n"
            "  <Tag name=\"VIA_hug\"/>\n"
            "  <Tag name=\"VIA_pray\"/>\n"
            "  <Group name=\"ModMood\"><Tag name=\"tired\"/><Tag name=\"wet\"/></Group>\n"
            " </Tags>\n"
            "</TagDefinition>\n";
        const std::optional<TagDefinitionDelta> delta = make_tag_definition_delta(base, modded);
        CHECK(delta.has_value());
        if (delta)
        {
            CHECK(delta->tags == 5);        // kneeling, VIA_hug, VIA_pray, tired, wet
            CHECK(delta->imports == 1);     // mod_fragmentids.xml (the tests import differs only in case)
            CHECK(delta->not_carried == 2); // horse's priority, the dropped player tag
            CHECK(delta->text.find("<Group name=\"stance\">\n<Tag name=\"kneeling\"/>") != std::string::npos);
            CHECK(delta->text.find("<Tag name=\"VIA_hug\"/>") != std::string::npos);
            CHECK(delta->text.find("<Group name=\"ModMood\">") != std::string::npos);
            CHECK(delta->text.find("horse") == std::string::npos);
            CHECK(delta->text.find("motionidle") == std::string::npos);
        }
        CHECK(!make_tag_definition_delta(base, "<ControllerDef/>").has_value());
    }

    void test_table_rows_delta()
    {
        using AnimationFramework::make_table_rows_delta;
        using AnimationFramework::TableRowsDelta;
        const std::string base =
            "<?xml version=\"1.0\" encoding=\"us-ascii\"?>\n<database name=\"barbora\">\n <anim_fragments "
            "version=\"14\">\n"
            "  <anim_fragment actor_class_hash=\"1\" fragment_guid=\"A\" mn_fragment_id=\"Wave\" mn_option_index=\"0\" "
            "total_duration=\"2\"/>\n"
            "  <anim_fragment actor_class_hash=\"1\" fragment_guid=\"B\" mn_fragment_id=\"Bow\" mn_option_index=\"0\" "
            "total_duration=\"3\"/>\n"
            " </anim_fragments>\n</database>\n";
        const std::optional<TableRowsDelta> unchanged = make_table_rows_delta(base, base);
        CHECK(unchanged.has_value() && unchanged->empty());
        // Wave changed (fragment id case differs, same key), Bow dropped, a new fragment's row added.
        const std::string modded =
            "<database name=\"barbora\"><anim_fragments version=\"14\"><anim_fragment actor_class_hash=\"1\" "
            "fragment_guid=\"a\" mn_fragment_id=\"WAVE\" mn_option_index=\"0\" total_duration=\"2.5\"/><anim_fragment "
            "actor_class_hash=\"1\" fragment_guid=\"C\" mn_fragment_id=\"Pray\" mn_option_index=\"0\" "
            "total_duration=\"6\"/></anim_fragments></database>";
        const std::optional<TableRowsDelta> delta = make_table_rows_delta(base, modded);
        CHECK(delta.has_value());
        if (delta)
        {
            CHECK(delta->added == 1);
            CHECK(delta->changed == 1);
            CHECK(delta->dropped == 1);
            CHECK(delta->text.find("<anim_fragments version=\"14\">") != std::string::npos);
            CHECK(delta->text.find("mn_fragment_id=\"Pray\"") != std::string::npos);
            CHECK(delta->text.find("total_duration=\"2.5\"") != std::string::npos);
            CHECK(delta->text.find("Bow") == std::string::npos);
        }
        CHECK(!make_table_rows_delta(base, "<AnimDB/>").has_value());
    }

    [[nodiscard]] std::string read_file(const char *path)
    {
        std::ifstream in{path, std::ios::binary};
        return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    }

    /** @brief The real-file mode (see the file comment). */
    int run_delta(std::string_view kind, const char *base_path, const char *mod_path, const char *out_path)
    {
        using namespace AnimationFramework;
        const std::string base = read_file(base_path);
        const std::string mod = read_file(mod_path);
        const auto started = std::chrono::steady_clock::now();
        std::optional<std::string> text;
        std::string summary;
        if (kind == "animevents")
        {
            if (const auto d = make_anim_events_delta(base, mod))
            {
                text = d->text;
                summary = std::to_string(d->animations) + " event list(s) added or changed, " +
                          std::to_string(d->removed) + " removed";
            }
        }
        else if (kind == "controllerdefs")
        {
            if (const auto d = make_controller_defs_delta(base, mod))
            {
                text = d->text;
                summary = std::to_string(d->fragment_defs) + " fragment definition(s), " +
                          std::to_string(d->overrides) + " override(s), " + std::to_string(d->removed_overrides) +
                          " removed, " + std::to_string(d->scopes) + " scope(s)";
            }
        }
        else if (kind == "rows")
        {
            if (const auto d = make_table_rows_delta(base, mod))
            {
                text = d->text;
                summary = std::to_string(d->added) + " row(s) added, " + std::to_string(d->changed) + " changed, " +
                          std::to_string(d->dropped) + " dropped";
            }
        }
        else if (kind == "tags")
        {
            if (const auto d = make_tag_definition_delta(base, mod))
            {
                text = d->text;
                summary = std::to_string(d->tags) + " tag(s), " + std::to_string(d->imports) + " import(s) added, " +
                          std::to_string(d->not_carried) + " not carried";
            }
        }
        else if (const auto d = make_adb_delta(base, mod))
        {
            text = d->text;
            summary = std::to_string(d->options) + " option(s), " + std::to_string(d->removed_options) + " removed";
        }
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        if (!text)
        {
            std::printf("not a well-formed %.*s pair\n", static_cast<int>(kind.size()), kind.data());
            return 1;
        }
        std::printf(
            "%s (%lld ms, %zu + %zu bytes in, %zu delta bytes)\n",
            summary.c_str(),
            static_cast<long long>(ms),
            base.size(),
            mod.size(),
            text->size()
        );
        if (out_path != nullptr)
        {
            std::ofstream{out_path, std::ios::binary} << *text;
        }
        return 0;
    }

    void test_clip_cache()
    {
        const std::filesystem::path path = temp_file("animfw_clip_cache");
        const std::string a = ClipCache::key("mods/a.pak", "animations/x.caf", 0x1234, 10, 20);
        const std::string b = ClipCache::key("mods/a.pak", "animations/y.caf", 0x5678, 30, 40);
        CHECK(a != ClipCache::key("mods/a.pak", "animations/x.caf", 0x1235, 10, 20));
        CHECK(a != ClipCache::key("mods/b.pak", "animations/x.caf", 0x1234, 10, 20));

        ClipCache cache;
        CHECK(!cache.dirty());
        cache.put(
            a,
            CachedClip{
                .motion = {std::byte{1}, std::byte{2}, std::byte{3}},
                .controllers = 7,
                .bones = {0xAABBCCDDU, 7U}
            }
        );
        cache.put(b, CachedClip{.motion = {std::byte{9}}, .controllers = 1});
        CHECK(cache.dirty() && cache.size() == 2);
        CHECK(cache.save(path, {a}));
        CHECK(!cache.dirty());

        ClipCache loaded;
        CHECK(loaded.load(path));
        CHECK(loaded.size() == 1 && !loaded.dirty());
        const CachedClip *hit = loaded.find(a);
        CHECK(hit != nullptr && hit->controllers == 7 && hit->motion.size() == 3 && hit->motion[2] == std::byte{3});
        CHECK((hit != nullptr && hit->bones == std::vector<std::uint32_t>{0xAABBCCDDU, 7U}));
        CHECK(loaded.find(b) == nullptr);

        // A cut-off file is not a cache.
        const std::uintmax_t full = std::filesystem::file_size(path);
        std::filesystem::resize_file(path, full - 1);
        CHECK(!loaded.load(path) && loaded.size() == 0);
        {
            std::ofstream out{path, std::ios::binary | std::ios::trunc};
            out << "not a cache";
        }
        CHECK(!loaded.load(path));
        std::error_code error;
        std::filesystem::remove(path, error);
        CHECK(!loaded.load(path));

        // A save that cannot write reports it and leaves the cache dirty.
        ClipCache unsaved;
        unsaved.put(a, CachedClip{.controllers = 1});
        CHECK(!unsaved.save(temp_file("animfw_missing_folder") / "cache.bin"));
        CHECK(unsaved.dirty());
    }

    template <typename T> void put_value(std::string &out, T value)
    {
        out.append(reinterpret_cast<const char *>(&value), sizeof(T));
    }

    /** @brief A chunk file of (type, version, payload) chunks, as the game's tools write them. */
    [[nodiscard]] std::string
    chunk_file(const std::vector<std::tuple<std::uint16_t, std::uint16_t, std::string>> &chunks)
    {
        std::string out{"CrCh"};
        put_value<std::uint32_t>(out, 0x746);
        put_value<std::uint32_t>(out, static_cast<std::uint32_t>(chunks.size()));
        put_value<std::uint32_t>(out, 16);
        std::uint32_t offset = 16 + 16 * static_cast<std::uint32_t>(chunks.size());
        std::string body;
        for (std::size_t i = 0; i < chunks.size(); ++i)
        {
            const auto &[type, version, payload] = chunks[i];
            put_value<std::uint16_t>(out, type);
            put_value<std::uint16_t>(out, version);
            put_value<std::uint32_t>(out, static_cast<std::uint32_t>(i + 1));
            put_value<std::uint32_t>(out, static_cast<std::uint32_t>(payload.size()));
            put_value<std::uint32_t>(out, offset + static_cast<std::uint32_t>(body.size()));
            body += payload;
            body.append((4 - body.size() % 4) % 4, '\0');
        }
        return out + body;
    }

    /** @brief A CompiledBones record: controller id, bone-to-world translation, name, parent offset. */
    [[nodiscard]] std::string bone_record(std::string_view name, float x, float y, float z, std::int32_t parent_offset)
    {
        std::string record(584, '\0');
        const std::uint32_t id = bone_controller_id(name);
        std::memcpy(record.data(), &id, 4);
        const std::array<float, 12> b2w{1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z};
        std::memcpy(record.data() + 264, b2w.data(), sizeof(b2w));
        std::memcpy(record.data() + 312, name.data(), name.size());
        std::memcpy(record.data() + 572, &parent_offset, 4);
        return record;
    }

    /**
     * @brief A clip pack chunk (0x905) of one clip, `animations/x/packed_clip.caf` (frames 0 to 10), whose one track
     *        turns Hips from the identity to 90 degrees about z: float rotations (format 1) at f32 frames 0 and 10,
     *        the track storage after the tables. @p rotation_track is the index its track record names.
     */
    [[nodiscard]] std::string clip_pack_chunk(std::uint32_t rotation_track = 0)
    {
        // Counts: no position track, one rotation track, one time track, one clip.
        std::string chunk;
        for (const std::uint32_t count : {0U, 1U, 1U, 1U})
        {
            put_value<std::uint32_t>(chunk, count);
        }
        // The time track's 2 keys, and the track count of each time format: one f32 track.
        put_value<std::uint16_t>(chunk, 2);
        for (const std::uint32_t tracks : {1U, 0U, 0U, 0U, 0U, 0U, 0U})
        {
            put_value<std::uint32_t>(chunk, tracks);
        }
        for (int i = 0; i < 9; ++i)
        {
            put_value<std::uint32_t>(chunk, 0);
        }
        // The rotation track's 2 keys, and the track count of each value format: one float quaternion track.
        put_value<std::uint16_t>(chunk, 2);
        for (const std::uint32_t tracks : {0U, 1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U})
        {
            put_value<std::uint32_t>(chunk, tracks);
        }
        // The offsets of the time track and the rotation track, then the storage end.
        for (const std::int32_t offset : {0, 8, 40})
        {
            put_value<std::int32_t>(chunk, offset);
        }
        const float s = std::sqrt(0.5F);
        for (const float v : {0.0F, 10.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, s, s})
        {
            put_value<float>(chunk, v);
        }
        const std::string name = "animations/x/packed_clip.caf";
        put_value<std::uint16_t>(chunk, static_cast<std::uint16_t>(name.size()));
        chunk += name;
        std::string motion(132, '\0');
        const std::int32_t end = 10;
        std::memcpy(motion.data() + 20, &end, 4);
        chunk += motion;
        // No foot plant bytes, one track record: the id, the position time and track (none), the rotation time and
        // track.
        put_value<std::uint16_t>(chunk, 0);
        put_value<std::uint16_t>(chunk, 1);
        for (const std::uint32_t field : {bone_controller_id("Hips"), 0xFFFFFFFFU, 0xFFFFFFFFU, 0U, rotation_track})
        {
            put_value<std::uint32_t>(chunk, field);
        }
        return chunk;
    }

    void test_caf_reader()
    {
        // A clip: one track on Hips, two rotation keys (identity, then 90 degrees about z) at frames 0 and 10, and two
        // position keys moving it 1 m along x.
        std::string motion(132, '\0');
        const std::int32_t end = 10;
        std::memcpy(motion.data() + 20, &end, 4);
        std::string controller;
        put_value<std::uint32_t>(controller, bone_controller_id("Hips"));
        put_value<std::uint16_t>(controller, 2);
        put_value<std::uint16_t>(controller, 2);
        for (const std::uint8_t b :
             std::array<std::uint8_t, 8>{1, 0, 2, 1, 0, 1, 0, 0}) // rot f32x4, f32 times, pos f32x3, own times, f32,
                                                                  // aligned
        {
            put_value<std::uint8_t>(controller, b);
        }
        const float s = std::sqrt(0.5F);
        for (const float v :
             {0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, s, s, 0.0F, 10.0F, 0.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F, 0.0F, 10.0F})
        {
            put_value<float>(controller, v);
        }
        const std::string caf = chunk_file(
            {{std::uint16_t{0x3002}, std::uint16_t{0x925}, motion},
             {std::uint16_t{0x100D}, std::uint16_t{0x829}, controller}}
        );
        auto clip = read_caf_file(std::as_bytes(std::span{caf}));
        CHECK(clip.has_value());
        if (!clip)
        {
            return;
        }
        CHECK(clip->tracks.size() == 1 && clip->end_frame == 10);
        CHECK(std::abs(clip->seconds() - 10.0 / 30.0) < 1e-9);
        const ClipTrack &track = clip->tracks.front();
        CHECK(track.rotations.size() == 2 && track.position_frames.size() == 2);
        const auto half = sample_rotation(track, 5.0);
        CHECK(half.has_value() && std::abs(angle_between(*half, Quat{}) - 45.0) < 1e-6);
        const auto moved = sample_position(track, 5.0);
        CHECK(moved.has_value() && std::abs(moved->x - 0.5) < 1e-6);
        const std::string not_a_clip{"not a clip"};
        CHECK(!read_caf_file(std::as_bytes(std::span{not_a_clip})).has_value());
        CHECK(
            (CafClip{
                 .start_frame = std::numeric_limits<std::int32_t>::min(),
                 .end_frame = std::numeric_limits<std::int32_t>::max()
             }
                 .seconds() > 1.0e8)
        );

        // Start/stop key times (format 3) key every frame from start to stop, the engine's (u32)(stop - start) + 1,
        // also on a range too far from 0 for a step of 1 to move it.
        const auto ranged_clip = [&motion](float start, float stop)
        {
            std::string ranged;
            put_value<std::uint32_t>(ranged, bone_controller_id("Hips"));
            put_value<std::uint16_t>(ranged, 1);
            put_value<std::uint16_t>(ranged, 0);
            for (const std::uint8_t b : std::array<std::uint8_t, 8>{1, 3, 0, 0, 0, 1, 0, 0})
            {
                put_value<std::uint8_t>(ranged, b);
            }
            for (const float v : {0.0F, 0.0F, 0.0F, 1.0F, start, stop})
            {
                put_value<float>(ranged, v);
            }
            const std::string file = chunk_file(
                {{std::uint16_t{0x3002}, std::uint16_t{0x925}, motion},
                 {std::uint16_t{0x100D}, std::uint16_t{0x829}, ranged}}
            );
            return read_caf_file(std::as_bytes(std::span{file}));
        };
        const auto stepped = ranged_clip(2.0F, 5.0F);
        CHECK(
            (stepped.has_value() && stepped->tracks.front().rotation_frames == std::vector<double>{2.0, 3.0, 4.0, 5.0})
        );
        const auto far = ranged_clip(1.0e20F, 1.0e20F);
        CHECK(far.has_value() && far->tracks.front().rotation_frames.size() == 1);
        CHECK(!ranged_clip(5.0F, 2.0F).has_value());
        // A span past an hour of frames is refused instead of growing a key array from six bytes of file.
        CHECK(!ranged_clip(0.0F, 1.0e6F).has_value());

        // A skeleton: a root, Hips 1 m above it, and a back slot under Hips.
        std::string bones(32, '\0');
        bones += bone_record("Human_Male", 0, 0, 0, 0);
        bones += bone_record("Hips", 0, 0, 1, -1);
        bones += bone_record("BackSlot_bow", 0, 0, 1.5F, -1);
        auto skeleton = read_skeleton_bones(std::as_bytes(std::span{bones}));
        CHECK(skeleton.has_value() && skeleton->size() == 3);
        if (!skeleton)
        {
            return;
        }
        CHECK((*skeleton)[1].parent == 0 && (*skeleton)[2].parent == 1 && (*skeleton)[1].name == "Hips");
        // A parent offset that leads before the first bone is refused, not read as a second root.
        std::string orphan(32, '\0');
        orphan += bone_record("Human_Male", 0, 0, 0, 0);
        orphan += bone_record("Hips", 0, 0, 1, -2);
        CHECK(!read_skeleton_bones(std::as_bytes(std::span{orphan})).has_value());

        // Played on it, Hips sits where its keys put it and the slot follows its parent.
        const ClipPoser poser{*skeleton, *clip};
        std::vector<Quat> rotations;
        std::vector<Vec3> positions;
        poser.pose(10.0, rotations, positions);
        CHECK(std::abs(positions[1].x - 1.0) < 1e-6 && std::abs(positions[1].z - 1.0) < 1e-6);
        CHECK(std::abs(angle_between(rotations[2], rotations[1])) < 1e-6);
        CHECK(std::abs(positions[2].z - 1.5) < 1e-6);

        const std::vector<std::uint32_t> keyed{bone_controller_id("Hips"), bone_controller_id("BackSlot_bow"), 0x1234U};
        const std::vector<ClipFinding> findings = check_clip_bones(keyed, *skeleton);
        CHECK(findings.size() == 2);
        CHECK(findings.size() == 2 && findings[0].text.find("lacks") != std::string::npos);
        CHECK(findings.size() == 2 && findings[1].text.find("BackSlot_bow") != std::string::npos);

        // Hips turns 90 degrees from the first frame to the last: a jump when the clip loops, a note when it does not.
        const ClipMotionMeasure measure = measure_clip_motion(*clip, *skeleton);
        CHECK(std::abs(measure.seam_degrees - 90.0) < 1e-3 && measure.seam_rotation_bone == "Hips");
        CHECK(!measure.root_travel.has_value());
        const auto looped = check_clip_motion(*clip, *skeleton, ClipMotionCheck{.loops = true});
        CHECK(
            std::ranges::any_of(
                looped,
                [](const ClipFinding &f)
                { return f.level == ClipFindingLevel::Warning && f.text.find("jumps") != std::string::npos; }
            )
        );
        const auto once = check_clip_motion(*clip, *skeleton, ClipMotionCheck{});
        CHECK(std::ranges::none_of(once, [](const ClipFinding &f) { return f.level == ClipFindingLevel::Warning; }));

        // Chunk views carry each table entry's id.
        const auto views = read_chunk_views(std::as_bytes(std::span{caf}));
        CHECK(views.has_value() && views->size() == 2 && (*views)[1].id == 2 && (*views)[1].type == 0x100D);

        // A clip pack: the clip and its track resolve through the track tables, and a stem picks clips.
        const std::string pack = chunk_file({{std::uint16_t{0x100D}, std::uint16_t{0x905}, clip_pack_chunk()}});
        const auto packed = read_dba_clips(std::as_bytes(std::span{pack}));
        CHECK(packed.has_value() && packed->size() == 1);
        if (packed && packed->size() == 1)
        {
            const CafClip &one = packed->front();
            CHECK(one.name == "animations/x/packed_clip.caf" && one.end_frame == 10 && one.tracks.size() == 1);
            const ClipTrack *hips = one.track(bone_controller_id("Hips"));
            CHECK((hips != nullptr && hips->rotation_frames == std::vector<double>{0.0, 10.0}));
            const auto packed_half = hips != nullptr ? sample_rotation(*hips, 5.0) : std::nullopt;
            CHECK(packed_half.has_value() && std::abs(angle_between(*packed_half, Quat{}) - 45.0) < 1e-6);
        }
        const std::vector<std::string> picked{"packed_clip"};
        const std::vector<std::string> other{"other_clip"};
        const auto by_stem = read_dba_clips(std::as_bytes(std::span{pack}), picked);
        const auto none_picked = read_dba_clips(std::as_bytes(std::span{pack}), other);
        CHECK(by_stem.has_value() && by_stem->size() == 1);
        CHECK(none_picked.has_value() && none_picked->empty());
        // A track record naming the storage end marker's slot does not resolve to a track.
        const std::string marker = chunk_file({{std::uint16_t{0x100D}, std::uint16_t{0x905}, clip_pack_chunk(1)}});
        CHECK(!read_dba_clips(std::as_bytes(std::span{marker})).has_value());
        // Track counts whose tables cannot fit the chunk are refused before anything is sized from them.
        std::string oversized = clip_pack_chunk();
        const std::uint32_t many_times = 0x10000000U;
        std::memcpy(oversized.data() + 8, &many_times, 4);
        const std::string oversized_pack = chunk_file({{std::uint16_t{0x100D}, std::uint16_t{0x905}, oversized}});
        CHECK(!read_dba_clips(std::as_bytes(std::span{oversized_pack})).has_value());

        // Toes are measured on a clip of ordinary length; a damaged frame range of billions is not posed frame by
        // frame.
        std::string footed(32, '\0');
        footed += bone_record("Human_Male", 0, 0, 0, 0);
        footed += bone_record("LeftToeBase", 0.1F, 0, 0, -1);
        footed += bone_record("RightToeBase", -0.1F, 0, 0, -2);
        const auto feet = read_skeleton_bones(std::as_bytes(std::span{footed}));
        CHECK(feet.has_value() && feet->size() == 3);
        if (feet)
        {
            CHECK(measure_clip_motion(CafClip{.end_frame = 10}, *feet).foot_slide.has_value());
            const CafClip endless{.end_frame = std::numeric_limits<std::int32_t>::max()};
            CHECK(!measure_clip_motion(endless, *feet).foot_slide.has_value());
        }
    }

    /** @brief The motion parameters' flags and speed of a compiled clip. */
    [[nodiscard]] std::pair<std::uint32_t, float> compiled_motion(const std::vector<std::byte> &file)
    {
        const auto chunks = read_chunk_views(file);
        if (chunks)
        {
            for (const ChunkView &chunk : *chunks)
            {
                if (chunk.type == 0x3002 && chunk.data.size() >= 132)
                {
                    std::uint32_t flags = 0;
                    float speed = 0.0F;
                    std::memcpy(&flags, chunk.data.data(), 4);
                    std::memcpy(&speed, chunk.data.data() + 24, 4);
                    return {flags, speed};
                }
            }
        }
        return {0xFFFFFFFFU, -1.0F};
    }

    /** @brief The worst rotation error of random rotations on a non-root bone compiled at @p compression. */
    [[nodiscard]] double small_tree_error(const std::vector<SkeletonBone> &skeleton, int compression)
    {
        std::uint32_t seed = 12345U;
        const auto next = [&seed]()
        {
            seed = seed * 1664525U + 1013904223U;
            return static_cast<double>(seed >> 8U) / 16777216.0 * 2.0 - 1.0;
        };
        double worst = 0.0;
        for (int i = 0; i < 2000; ++i)
        {
            Quat q{.x = next(), .y = next(), .z = next(), .w = next()};
            const double n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
            q = Quat{.x = q.x / n, .y = q.y / n, .z = q.z / n, .w = q.w / n};
            const SampledClip one{
                .frames = 1,
                .tracks = {SampledTrack{.controller = skeleton[1].controller, .rotations = {q}}}
            };
            auto file = compile_clip(one, skeleton, CompileOptions{.compression = compression});
            if (!file)
            {
                return 180.0;
            }
            auto back = read_caf_file(file->file);
            const ClipTrack *s = back ? back->track(skeleton[1].controller) : nullptr;
            worst =
                std::max(worst, s != nullptr && !s->rotations.empty() ? angle_between(s->rotations.front(), q) : 180.0);
        }
        return worst;
    }

    void test_clip_compiler()
    {
        // A three-bone skeleton: Human_Male (root), Spine above it, Head above Spine, all unrotated.
        const std::vector<SkeletonBone> skeleton{
            {.name = "Human_Male", .controller = bone_controller_id("Human_Male"), .parent = -1},
            {.name = "Spine", .controller = bone_controller_id("Spine"), .parent = 0, .position = {.z = 1.0}},
            {.name = "Head", .controller = bone_controller_id("Head"), .parent = 1, .position = {.z = 1.5}},
        };
        // 31 frames: the root starts turned 90 degrees about z at (1, 2, 0) and walks 1 m/s along its own y; Spine
        // swings 40 degrees about z and back (so the clip loops); Head holds its default pose.
        constexpr std::size_t FRAMES = 31;
        const double half = std::sqrt(0.5);
        const Quat turned{.z = half, .w = half};
        SampledClip clip{.frames = FRAMES};
        SampledTrack root{.controller = skeleton[0].controller};
        SampledTrack spine{.controller = skeleton[1].controller};
        SampledTrack head{.controller = skeleton[2].controller};
        for (std::size_t f = 0; f < FRAMES; ++f)
        {
            const double t = static_cast<double>(f) / 30.0;
            const Vec3 walked = rotate(turned, Vec3{.y = t});
            root.rotations.push_back(turned);
            root.positions.push_back(Vec3{.x = walked.x + 1.0, .y = walked.y + 2.0, .z = walked.z});
            const double angle =
                0.5 * (40.0 * std::numbers::pi / 180.0) * std::sin(std::numbers::pi * static_cast<double>(f) / 30.0);
            spine.rotations.push_back(Quat{.z = std::sin(angle), .w = std::cos(angle)});
            spine.positions.push_back(Vec3{.z = 1.0});
            head.rotations.push_back(Quat{});
            head.positions.push_back(Vec3{.z = 0.5});
        }
        clip.tracks = {root, spine, head};

        // Two tracks of one bone would write two controller chunks with one id.
        SampledClip doubled = clip;
        doubled.tracks.push_back(spine);
        CHECK(!compile_clip(doubled, skeleton, CompileOptions{}).has_value());

        auto compiled = compile_clip(clip, skeleton, CompileOptions{});
        CHECK(compiled.has_value());
        if (compiled)
        {
            const auto [flags, speed] = compiled_motion(compiled->file);
            CHECK(flags == 0x2); // loops, not additive
            CHECK(std::abs(speed - 1.0F) < 0.01F);
            CHECK(compiled->report.tracks == 3);
            auto back = read_caf_file(compiled->file);
            CHECK(back.has_value() && back->end_frame == 30);
            if (back)
            {
                // The root starts at the identity and walks along +y (rebased); Spine is within the tolerance.
                const ClipTrack *r = back->track(skeleton[0].controller);
                CHECK(r != nullptr && angle_between(r->rotations.front(), Quat{}) < 0.01);
                CHECK(
                    r != nullptr && std::abs(r->positions.front().x) < 1.0e-4 &&
                    std::abs(r->positions.back().y - 1.0) < 1.0e-3
                );
                const ClipTrack *s = back->track(skeleton[1].controller);
                double worst = 0.0;
                for (std::size_t f = 0; s != nullptr && f < FRAMES; ++f)
                {
                    const auto q = sample_rotation(*s, static_cast<double>(f));
                    worst = std::max(worst, q ? angle_between(*q, spine.rotations[f]) : 180.0);
                }
                CHECK(s != nullptr && worst < 0.04);
            }
        }

        // As the Resource Compiler: Head never leaves its default pose and goes.
        auto overlay = compile_clip(clip, skeleton, CompileOptions{.tracks = TrackPolicy::DropUnchangedAtDefault});
        CHECK(overlay.has_value() && overlay->report.tracks == 2);

        // Additive: flags 0x3, the root left to the base layer, Head's zero change left out, keys from frame 1.
        auto additive = compile_clip(clip, skeleton, CompileOptions{.additive = true});
        CHECK(additive.has_value());
        if (additive)
        {
            CHECK(compiled_motion(additive->file).first == 0x3);
            CHECK(additive->report.tracks == 1);
            auto back = read_caf_file(additive->file);
            const ClipTrack *s = back ? back->track(skeleton[1].controller) : nullptr;
            CHECK(s != nullptr && !s->rotation_frames.empty() && s->rotation_frames.front() == 1.0);
        }

        // Random rotations read back within the tolerance: the 48-bit small tree at compression 10, the 64-bit one
        // at compression 0 (whose tolerance the 48-bit format cannot meet).
        CHECK(small_tree_error(skeleton, 10) < 0.02);
        CHECK(small_tree_error(skeleton, 0) < 0.001);

        // The small-tree codecs: the dropped (largest) component's index sits in the top 2 bits, and it decodes as
        // positive, so a rotation whose largest component is negative reads back as its negation.
        CHECK(encode_small_tree_48(Quat{})[2] >> 14U == 3U);
        CHECK(encode_small_tree_64(Quat{})[1] >> 30U == 3U);
        const double w = -std::sqrt(1.0 - 0.14);
        const Quat flipped{.x = 0.1, .y = -0.2, .z = 0.3, .w = w};
        const Quat back_48 = decode_small_tree_48(encode_small_tree_48(flipped));
        const Quat back_64 = decode_small_tree_64(encode_small_tree_64(flipped));
        CHECK(back_48.w > 0.0 && angle_between(back_48, flipped) < 0.01);
        CHECK(back_64.w > 0.0 && angle_between(back_64, flipped) < 0.0005);
        CHECK(std::abs(back_64.x + 0.1) < 1.0e-5 && std::abs(back_64.y - 0.2) < 1.0e-5);

        // A rotation with no finite length compiles to the identity instead of reaching the quantizer as NaN.
        const SampledClip broken{
            .frames = 1,
            .tracks = {SampledTrack{
                .controller = skeleton[1].controller,
                .rotations = {Quat{.x = std::numeric_limits<double>::infinity()}}
            }},
        };
        const auto repaired = compile_clip(broken, skeleton, CompileOptions{});
        const auto repaired_back = repaired ? read_caf_file(repaired->file)
                                            : std::expected<CafClip, std::string>{std::unexpected{std::string{}}};
        const ClipTrack *repaired_track = repaired_back ? repaired_back->track(skeleton[1].controller) : nullptr;
        CHECK(
            repaired_track != nullptr && !repaired_track->rotations.empty() &&
            angle_between(repaired_track->rotations.front(), Quat{}) < 0.01
        );

        // An intermediate file keyed at 24 frames a second (200 ticks a key) plays at its own length, resampled to 30.
        std::string controller;
        put_value<std::uint32_t>(controller, 25);
        put_value<std::uint32_t>(controller, skeleton[1].controller);
        for (int k = 0; k < 25; ++k)
        {
            const double angle =
                0.5 * (std::numbers::pi / 2.0) * static_cast<double>(k) / 24.0; // 0 to 90 degrees in 1 s
            put_value<std::int32_t>(controller, 200 * k);
            for (const float v : {0.0F, 0.0F, 100.0F})
            {
                put_value<float>(controller, v); // 1 m up, in cm
            }
            for (const float v : {0.0F, 0.0F, static_cast<float>(-angle)})
            {
                put_value<float>(controller, v); // the log of the conjugate rotation: minus half the angle about z
            }
        }
        const std::string intermediate = chunk_file({{std::uint16_t{0x100D}, std::uint16_t{0x827}, controller}});
        auto read =
            read_intermediate_clip(std::as_bytes(std::span{intermediate.data(), intermediate.size()}), skeleton);
        CHECK(read.has_value() && read->frames == 31 && read->tracks.size() == 1);
        if (read && read->frames == 31 && !read->tracks.empty())
        {
            CHECK(angle_between(read->tracks.front().rotations.back(), Quat{.z = half, .w = half}) < 0.01);
            CHECK(std::abs(read->tracks.front().positions.front().z - 1.0) < 1.0e-6);
        }

        // Two keys more than an hour apart (4800 ticks a second) mark a damaged file, refused before any frame is
        // sampled.
        const auto two_keys = [&skeleton](std::int32_t last_tick) -> std::string
        {
            std::string keys;
            put_value<std::uint32_t>(keys, 2);
            put_value<std::uint32_t>(keys, skeleton[1].controller);
            for (const std::int32_t tick : {0, last_tick})
            {
                put_value<std::int32_t>(keys, tick);
                for (int i = 0; i < 6; ++i)
                {
                    put_value<float>(keys, 0.0F);
                }
            }
            return chunk_file({{std::uint16_t{0x100D}, std::uint16_t{0x827}, keys}});
        };
        const std::string hour = two_keys(4800 * 3600);
        const std::string damaged = two_keys(4800 * 3600 + 4800);
        const auto hour_read = read_intermediate_clip(std::as_bytes(std::span{hour.data(), hour.size()}), skeleton);
        CHECK(hour_read.has_value() && hour_read->frames == 30 * 3600 + 1);
        CHECK(!read_intermediate_clip(std::as_bytes(std::span{damaged.data(), damaged.size()}), skeleton).has_value());
    }

    struct RigBone
    {
        std::string name;
        int parent;
        Vec3 at;
    };

    /** @brief A Henry-like skeleton in an A-pose, every bone unrotated (model space), left at +x, toes toward -y. */
    [[nodiscard]] std::vector<RigBone> henry_like_rig()
    {
        std::vector<RigBone> rig{
            {"Human_Male", -1, {}},
            {"Hips", 0, {.z = 0.95}},
            {"Spine", 1, {.z = 1.05}},
            {"Spine1", 2, {.z = 1.12}},
            {"Spine2", 3, {.z = 1.2}},
            {"Spine3", 4, {.z = 1.28}},
            {"Spine4", 5, {.z = 1.36}},
            {"Neck", 6, {.z = 1.5}},
            {"Neck1", 7, {.z = 1.55}},
            {"Head", 8, {.z = 1.62}},
            {"Spine_IKTarget", 0, {}},
        };
        for (const auto &[side, x] : {std::pair{std::string_view{"Left"}, 1.0}, {std::string_view{"Right"}, -1.0}})
        {
            const auto add = [&](std::string_view bone, std::string_view parent, Vec3 at)
            {
                const std::string parent_name = parent.starts_with("Spine") || parent == "Hips"
                                                    ? std::string{parent}
                                                    : std::format("{}{}", side, parent);
                int p = -1;
                for (std::size_t i = 0; i < rig.size(); ++i)
                {
                    p = rig[i].name == parent_name ? static_cast<int>(i) : p;
                }
                rig.push_back({std::format("{}{}", side, bone), p, at});
            };
            add("Shoulder", "Spine4", {.x = 0.05 * x, .z = 1.45});
            add("Arm", "Shoulder", {.x = 0.18 * x, .z = 1.43});
            add("ForeArm", "Arm", {.x = 0.3 * x, .z = 1.2});
            add("Hand", "ForeArm", {.x = 0.4 * x, .z = 0.98});
            add("HandMiddle1", "Hand", {.x = 0.43 * x, .z = 0.9});
            add("Hand_IKBlend", "Hand", {.x = 0.4 * x, .z = 0.98});
            add("UpLeg", "Hips", {.x = 0.1 * x, .z = 0.95});
            add("Leg", "UpLeg", {.x = 0.1 * x, .z = 0.5});
            add("Foot", "Leg", {.x = 0.1 * x, .z = 0.08});
            add("ToeBase", "Foot", {.x = 0.1 * x, .y = -0.12, .z = 0.02});
        }
        return rig;
    }

    [[nodiscard]] std::vector<std::pair<std::string, std::string>>
    retarget_mapping(const std::vector<std::string> &names)
    {
        const std::vector<RigBone> rig = henry_like_rig();
        std::vector<SkeletonBone> skeleton;
        MocapClip source;
        for (std::size_t i = 0; i < rig.size(); ++i)
        {
            skeleton.push_back(
                {.name = rig[i].name,
                 .controller = bone_controller_id(rig[i].name),
                 .parent = rig[i].parent,
                 .position = rig[i].at}
            );
            source.joints.push_back({.name = names[i], .parent = rig[i].parent});
            source.rest.push_back({.position = rig[i].at});
        }
        source.frames = {source.rest};
        auto result = retarget(source, skeleton, RetargetOptions{});
        CHECK(result.has_value());
        return result ? result->report.mapping : std::vector<std::pair<std::string, std::string>>{};
    }

    [[nodiscard]] bool maps(
        const std::vector<std::pair<std::string, std::string>> &mapping,
        std::string_view henry,
        std::string_view joint
    )
    {
        return std::ranges::any_of(mapping, [&](const auto &m) { return m.first == henry && m.second == joint; });
    }

    void test_retarget()
    {
        // BVH: the root's position channels replace its offset; rotations apply in the listed order.
        {
            const auto clip = read_bvh(R"(HIERARCHY
ROOT Hips
{
  OFFSET 0 0 0
  CHANNELS 6 Xposition Yposition Zposition Zrotation Xrotation Yrotation
  JOINT Chest
  {
    OFFSET 0 10 0
    CHANNELS 3 Zrotation Xrotation Yrotation
    End Site
    {
      OFFSET 0 5 0
    }
  }
}
MOTION
Frames: 2
Frame Time: 0.033333
0 90 0 0 0 0 0 0 0
1 90 0 90 0 0 0 0 0
)");
            CHECK(clip.has_value());
            if (clip)
            {
                CHECK(clip->joints.size() == 2 && clip->joints[1].parent == 0 && clip->frames.size() == 2);
                CHECK(std::abs(clip->fps - 30.0) < 0.01);
                CHECK(std::abs(clip->rest[1].position.y - 10.0) < 1e-9);
                const Vec3 chest0 = clip->frames[0][1].position;
                const Vec3 chest1 = clip->frames[1][1].position;
                CHECK(std::abs(chest0.x) < 1e-9 && std::abs(chest0.y - 100.0) < 1e-9);
                CHECK(std::abs(chest1.x + 9.0) < 1e-9 && std::abs(chest1.y - 90.0) < 1e-9);
            }
            CHECK(!read_bvh("not a bvh").has_value());
        }

        // Bone names of the common rigs map onto Henry's bones.
        const std::vector<RigBone> rig = henry_like_rig();
        const auto renamed = [&](auto rename)
        {
            std::vector<std::string> names;
            for (const RigBone &bone : rig)
            {
                names.push_back(rename(bone.name));
            }
            return names;
        };
        const auto replace_all =
            [](std::string text, std::initializer_list<std::pair<std::string_view, std::string_view>> pairs)
        {
            for (const auto &[from, to] : pairs)
            {
                if (const auto at = text.find(from); at != std::string::npos)
                {
                    text.replace(at, from.size(), to);
                }
            }
            return text;
        };
        {
            const auto mapping = retarget_mapping(renamed([](std::string n) { return "mixamorig:" + n; }));
            CHECK(maps(mapping, "LeftArm", "mixamorig:LeftArm"));
            CHECK(maps(mapping, "LeftShoulder", "mixamorig:LeftShoulder"));
            CHECK(maps(mapping, "RightToeBase", "mixamorig:RightToeBase"));
            CHECK(maps(mapping, "Hips", "mixamorig:Hips"));
        }
        {
            // SMPL: Collar is the clavicle, Shoulder the upper arm, Foot the toes below the Ankle.
            const auto mapping = retarget_mapping(renamed(
                [&](std::string n)
                {
                    for (const auto &[side, s] :
                         {std::pair{std::string_view{"Left"}, std::string_view{"L_"}},
                          {std::string_view{"Right"}, std::string_view{"R_"}}})
                    {
                        if (n.starts_with(side))
                        {
                            n = std::string{s} + n.substr(side.size());
                            return replace_all(
                                n,
                                {{"Shoulder", "Collar"},
                                 {"ForeArm", "Elbow"},
                                 {"Arm", "Shoulder"},
                                 {"HandMiddle1", "Middle1"},
                                 {"Hand", "Wrist"},
                                 {"UpLeg", "Hip"},
                                 {"Leg", "Knee"},
                                 {"Foot", "Ankle"},
                                 {"ToeBase", "Foot"}}
                            );
                        }
                    }
                    return n == "Hips" ? std::string{"Pelvis"} : n;
                }
            ));
            CHECK(maps(mapping, "LeftShoulder", "L_Collar"));
            CHECK(maps(mapping, "LeftArm", "L_Shoulder"));
            CHECK(maps(mapping, "LeftForeArm", "L_Elbow"));
            CHECK(maps(mapping, "LeftFoot", "R_Ankle") == false);
            CHECK(maps(mapping, "RightFoot", "R_Ankle"));
            CHECK(maps(mapping, "RightToeBase", "R_Foot"));
            CHECK(maps(mapping, "Hips", "Pelvis"));
        }
        {
            // Unreal: side suffixes, clavicle, upperarm, lowerarm, thigh, calf, ball.
            const auto mapping = retarget_mapping(renamed(
                [&](std::string n)
                {
                    for (const auto &[side, s] :
                         {std::pair{std::string_view{"Left"}, std::string_view{"_l"}},
                          {std::string_view{"Right"}, std::string_view{"_r"}}})
                    {
                        if (n.starts_with(side))
                        {
                            return replace_all(
                                       n.substr(side.size()),
                                       {{"Shoulder", "clavicle"},
                                        {"ForeArm", "lowerarm"},
                                        {"Arm", "upperarm"},
                                        {"HandMiddle1", "middle_01"},
                                        {"Hand", "hand"},
                                        {"UpLeg", "thigh"},
                                        {"Leg", "calf"},
                                        {"ToeBase", "ball"},
                                        {"Foot", "foot"}}
                                   ) +
                                   std::string{s};
                        }
                    }
                    return n == "Hips" ? std::string{"pelvis"} : n;
                }
            ));
            CHECK(maps(mapping, "LeftShoulder", "clavicle_l"));
            CHECK(maps(mapping, "LeftArm", "upperarm_l"));
            CHECK(maps(mapping, "RightForeArm", "lowerarm_r"));
            CHECK(maps(mapping, "RightLeg", "calf_r"));
            CHECK(maps(mapping, "LeftToeBase", "ball_l"));
        }

        // Onto the same skeleton the motion comes back as it went in: every mapped bone's world rotation, and the
        // pelvis.
        {
            std::vector<SkeletonBone> skeleton;
            MocapClip source;
            source.fps = 30.0;
            for (const RigBone &bone : rig)
            {
                skeleton.push_back(
                    {.name = bone.name,
                     .controller = bone_controller_id(bone.name),
                     .parent = bone.parent,
                     .position = bone.at}
                );
                source.joints.push_back({.name = bone.name, .parent = bone.parent});
                source.rest.push_back({.position = bone.at});
            }
            const auto index = [&](std::string_view name)
            {
                for (std::size_t i = 0; i < rig.size(); ++i)
                {
                    if (rig[i].name == name)
                    {
                        return i;
                    }
                }
                return std::size_t{0};
            };
            constexpr std::size_t FRAMES = 12;
            for (std::size_t f = 0; f < FRAMES; ++f)
            {
                // Local turns on the upper body; the legs stay in the rest pose (they set the ground).
                std::vector<Quat> local(rig.size());
                const double t = static_cast<double>(f) / static_cast<double>(FRAMES - 1);
                local[index("Spine2")] = {.x = std::sin(0.2 * t), .w = std::cos(0.2 * t)};
                local[index("LeftArm")] = {.y = std::sin(0.6 * t), .w = std::cos(0.6 * t)};
                local[index("LeftForeArm")] = {.x = std::sin(-0.5 * t), .w = std::cos(-0.5 * t)};
                local[index("Head")] = {.z = std::sin(0.4 * t), .w = std::cos(0.4 * t)};
                std::vector<Pose> poses(rig.size());
                for (std::size_t i = 0; i < rig.size(); ++i)
                {
                    const int p = rig[i].parent;
                    if (p < 0)
                    {
                        poses[i] = {.rotation = local[i], .position = rig[i].at};
                        continue;
                    }
                    const Pose &up = poses[static_cast<std::size_t>(p)];
                    const Vec3 offset{
                        rig[i].at.x - rig[static_cast<std::size_t>(p)].at.x,
                        rig[i].at.y - rig[static_cast<std::size_t>(p)].at.y,
                        rig[i].at.z - rig[static_cast<std::size_t>(p)].at.z
                    };
                    const Vec3 turned = rotate(up.rotation, offset);
                    poses[i] = {
                        .rotation = multiply(up.rotation, local[i]),
                        .position = {up.position.x + turned.x, up.position.y + turned.y, up.position.z + turned.z}
                    };
                }
                source.frames.push_back(std::move(poses));
            }
            auto result = retarget(source, skeleton, RetargetOptions{});
            CHECK(result.has_value());
            if (result)
            {
                const SampledClip &clip = result->clip;
                CHECK(clip.frames == FRAMES);
                double worst = 0.0;
                for (std::size_t f = 0; f < FRAMES; ++f)
                {
                    std::vector<Quat> world(rig.size());
                    for (std::size_t i = 0; i < rig.size(); ++i)
                    {
                        const int p = rig[i].parent;
                        world[i] = p < 0 ? clip.tracks[i].rotations[f]
                                         : multiply(world[static_cast<std::size_t>(p)], clip.tracks[i].rotations[f]);
                    }
                    for (const std::string_view name :
                         {"Hips", "Spine2", "Spine4", "LeftArm", "LeftForeArm", "LeftHand", "Head", "RightUpLeg"})
                    {
                        worst =
                            std::max(worst, angle_between(world[index(name)], source.frames[f][index(name)].rotation));
                    }
                    const Vec3 hips = clip.tracks[index("Hips")].positions[f];
                    CHECK(std::abs(hips.z - 0.95) < 1e-6 && std::abs(hips.x) < 1e-6 && std::abs(hips.y) < 1e-6);
                }
                CHECK(worst < 1e-4);
                // The spine IK target follows the chest; an IK blend sits at its bone.
                const Quat chest_last = source.frames[FRAMES - 1][index("Spine4")].rotation;
                CHECK(angle_between(clip.tracks[index("Spine_IKTarget")].rotations[FRAMES - 1], chest_last) < 1e-4);
                const Vec3 blend = clip.tracks[index("LeftHand_IKBlend")].positions[FRAMES - 1];
                CHECK(std::abs(blend.x) + std::abs(blend.y) + std::abs(blend.z) < 1e-9);
                CHECK(std::abs(result->report.scale - 1.0) < 1e-9);
            }

            // A non-finite source pose (BVH reads "nan" and "inf") never reaches the compiled clip.
            MocapClip broken = source;
            broken.frames[FRAMES / 2][index("Head")].rotation.x = std::numeric_limits<double>::quiet_NaN();
            CHECK(!retarget(broken, skeleton, RetargetOptions{}).has_value());
            broken = source;
            broken.frames[0][index("Hips")].position.z = std::numeric_limits<double>::infinity();
            CHECK(!retarget(broken, skeleton, RetargetOptions{}).has_value());
        }
    }

    void test_mod_maker()
    {
        // The game's wave (male_adlg.adb): a window-only option, a Looking scope helper, then the plain options.
        const std::string like_options_text = R"(<ADLG_Wave>
            <Fragment BlendOutDuration="0.2" GUID="1d7f7328-ea37-4c33-a9af-a2bbb0c550c1" Tags="at_window">
                <AnimLayer><Blend ExitTime="0" StartTime="0" Duration="0.5"/><Animation name="window_wave"/></AnimLayer>
            </Fragment>
            <Fragment BlendOutDuration="0.2" GUID="938d3df6-e9c2-411b-b093-54b9e87c0275" Tags="Scope_Looking">
                <ProcLayer><Procedural type="LookingBlendBone"/></ProcLayer>
            </Fragment>
            <Fragment BlendOutDuration="0.2" GUID="bb8f8813-dfdb-4192-bbe7-c72124f3c991" Tags="" FragTags="x">
                <AnimLayer>
                    <Blend ExitTime="0" StartTime="0" Duration="0.68"/>
                    <Animation name="wave_goodbye" flags="Loop"/>
                    <Blend ExitTime="1" StartTime="0" Duration="0.2"/>
                    <Animation name="wave_more"/>
                </AnimLayer>
                <AnimLayer><Animation name="face_wave"/></AnimLayer>
                <ProcLayer><Procedural type="AnimationOverride"/></ProcLayer>
            </Fragment>
        </ADLG_Wave>)";
        const std::string like_definition_text =
            R"(<ADLG_Wave scopes="FullBody+Looking">)"
            R"(<Override tags="at_window" scopes="FullBody+Looking"/></ADLG_Wave>)";
        const auto like_options = read_xml_elements(like_options_text);
        const auto like_definition = read_xml_elements(like_definition_text);
        CHECK(like_options.has_value() && like_definition.has_value());
        if (!like_options || !like_definition)
        {
            return;
        }
        const XmlElement *model = template_option(*like_options);
        CHECK(model != nullptr && first_clip(*model) == "wave_goodbye");

        const ModAction wave{.modid = "my_mod", .fragment = "MyMod_Wave", .clip = "my_mod_wave"};
        auto made = add_mod_action(ModPatches{}, wave, *like_definition, *like_options);
        CHECK(made.has_value());
        if (!made)
        {
            return;
        }
        const auto ids = read_xml_elements(made->fragment_ids);
        CHECK(ids.has_value() && lists_fragment(*ids, "mymod_wave"));
        const auto defs = read_xml_elements(made->controller_defs);
        const XmlElement *definition = defs ? find_fragment_definition(*defs, "MyMod_Wave") : nullptr;
        CHECK(definition != nullptr && definition->get("scopes") == "FullBody+Looking" && definition->children.empty());
        const auto database = read_xml_elements(made->database);
        const XmlElement *options = database ? find_fragment_options(*database, "MyMod_Wave") : nullptr;
        CHECK(options != nullptr && options->children.size() == 2);
        if (options == nullptr || options->children.size() != 2)
        {
            return;
        }
        const XmlElement &option = options->children[0];
        CHECK(option.get("GUID") == option_guid("my_mod", "MyMod_Wave", 0));
        CHECK(option.get("Tags").empty() && option.attr("FragTags") == nullptr);
        CHECK(first_clip(option) == "my_mod_wave");
        const XmlElement *layer = option.child("AnimLayer");
        CHECK(layer != nullptr && layer->children.size() == 2);
        CHECK(layer != nullptr && layer->children.size() == 2 && layer->children[1].attr("flags") == nullptr);
        CHECK(std::ranges::count_if(option.children, [](const XmlElement &c) { return c.tag == "AnimLayer"; }) == 1);
        CHECK(option.child("ProcLayer") != nullptr);
        CHECK(options->children[1].get("Tags") == "Scope_Looking");
        CHECK(options->children[1].get("GUID") == option_guid("my_mod", "MyMod_Wave", 1));
        CHECK(option_guid("my_mod", "MyMod_Wave", 0) != option_guid("my_mod", "MyMod_Wave", 1));

        // Made again as a loop, the fragment is replaced, not repeated; a second action joins it.
        ModAction looped = wave;
        looped.loop = true;
        auto again = add_mod_action(*made, looped, *like_definition, *like_options);
        const ModAction other{.modid = "my_mod", .fragment = "MyMod_Bow", .clip = "my_mod_bow"};
        auto both = again ? add_mod_action(*again, other, *like_definition, *like_options) : again;
        CHECK(both.has_value());
        if (both)
        {
            const auto both_db = read_xml_elements(both->database);
            const XmlElement *list = both_db ? both_db->child("FragmentList") : nullptr;
            CHECK(list != nullptr && list->children.size() == 2);
            const XmlElement *again_options = both_db ? find_fragment_options(*both_db, "MyMod_Wave") : nullptr;
            const XmlElement *again_layer = again_options ? again_options->children[0].child("AnimLayer") : nullptr;
            CHECK(again_layer != nullptr && again_layer->children.back().get("flags") == "Loop");
            const auto both_ids = read_xml_elements(both->fragment_ids);
            const XmlElement *tags = both_ids ? both_ids->child("Tags") : nullptr;
            CHECK(tags != nullptr && tags->children.size() == 2);
        }

        // On the upper body: its own UpperBody definition, the clip through a blend space that keeps the chest up, no
        // procedural layers and no scope helpers.
        const ModAction upper{.modid = "my_mod", .fragment = "MyMod_Gesture", .clip = "my_mod_wave", .upper = true};
        auto gesture = add_mod_action(ModPatches{}, upper, *like_definition, *like_options);
        CHECK(gesture.has_value());
        if (gesture)
        {
            const auto upper_defs = read_xml_elements(gesture->controller_defs);
            const XmlElement *upper_definition =
                upper_defs ? find_fragment_definition(*upper_defs, "MyMod_Gesture") : nullptr;
            CHECK(upper_definition != nullptr && upper_definition->get("scopes") == "UpperBody");
            const auto upper_db = read_xml_elements(gesture->database);
            const XmlElement *upper_options = upper_db ? find_fragment_options(*upper_db, "MyMod_Gesture") : nullptr;
            CHECK(upper_options != nullptr && upper_options->children.size() == 1);
            if (upper_options != nullptr && !upper_options->children.empty())
            {
                const XmlElement &upper_option = upper_options->children.front();
                CHECK(first_clip(upper_option) == "my_mod_u_my_mod_wave");
                CHECK(upper_option.child("ProcLayer") == nullptr);
            }
            CHECK(gesture->files.size() == 1);
            if (gesture->files.size() == 1)
            {
                const ZipSource &blend_space = gesture->files.front();
                CHECK(blend_space.name == "animations/humans/male/blendspace/my_mod/my_mod_u_my_mod_wave.bspace");
                const auto parsed = read_xml_elements(blend_space.data);
                const XmlElement *joints = parsed ? parsed->child("JointList") : nullptr;
                // The Gesture Wheel's joint list: chest, neck, head and eyes, each arm's 13 parts, each hand's 20
                // finger joints.
                CHECK(joints != nullptr && joints->children.size() == 74);
                CHECK(blend_space.data.find("AName=\"my_mod_wave\"") != std::string::npos);
            }
        }

        const XmlElement no_clip{.tag = "Empty"};
        CHECK(!add_mod_action(ModPatches{}, wave, *like_definition, no_clip).has_value());
        CHECK(mod_manifest("my_mod", "2026-11-02").find("<modid>my_mod</modid>") != std::string::npos);
    }

    /** @brief A one-track clip (Hips turning 90 degrees over 10 frames) as a compiled .caf, and its motion chunk. */
    [[nodiscard]] std::pair<std::string, std::string> small_clip()
    {
        std::string motion(132, '\0');
        const std::int32_t end = 10;
        std::memcpy(motion.data() + 20, &end, 4);
        std::string controller;
        put_value<std::uint32_t>(controller, bone_controller_id("Hips"));
        put_value<std::uint16_t>(controller, 2);
        put_value<std::uint16_t>(controller, 0);
        for (const std::uint8_t b : std::array<std::uint8_t, 8>{1, 0, 0, 0, 0, 1, 0, 0})
        {
            put_value<std::uint8_t>(controller, b);
        }
        const float s = std::sqrt(0.5F);
        for (const float v : {0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, s, s, 0.0F, 10.0F})
        {
            put_value<float>(controller, v);
        }
        return {
            chunk_file(
                {{std::uint16_t{0x3002}, std::uint16_t{0x925}, motion},
                 {std::uint16_t{0x100D}, std::uint16_t{0x829}, controller}}
            ),
            motion,
        };
    }

    void test_offline_game()
    {
        // A game folder whose Animations.pak holds a clip index with one loose clip, the clip, Henry's skeleton and the
        // three Mannequin files of a game fragment to copy.
        const auto [clip, motion] = small_clip();
        const std::string clip_path = "animations/humans/male/test/test_clip.caf";
        const auto header = build_clip_header(std::as_bytes(std::span{motion}), 1, clip_path);
        CHECK(header.has_value());
        if (!header)
        {
            return;
        }
        const std::string index = chunk_file(
            {{std::uint16_t{0x3007},
              std::uint16_t{0x971},
              std::string{reinterpret_cast<const char *>(header->data()), header->size()}}}
        );
        std::string bones(32, '\0');
        bones += bone_record("Human_Male", 0, 0, 0, 0);
        bones += bone_record("Hips", 0, 0, 1, -1);
        const std::string chr = chunk_file({{std::uint16_t{0x2000}, std::uint16_t{0x800}, bones}});
        const std::vector<ZipSource> files{
            {.name = "animations/animations.img", .data = index},
            {.name = clip_path, .data = clip},
            {.name = std::string{MALE_SKELETON_PATH}, .data = chr},
            {.name = std::string{MALE_FRAGMENT_IDS},
             .data = R"(<TagDefinition version="2"><Tags><Tag name="ADLG_Test"/></Tags></TagDefinition>)"},
            {.name = std::string{MALE_CONTROLLER_DEFS},
             .data = R"(<ControllerDef><FragmentDefs><ADLG_Test scopes="FullBody"/></FragmentDefs></ControllerDef>)"},
            {.name = std::string{MALE_DATABASE},
             .data = R"(<AnimDB><SubADBs><SubADB File="Animations/Mannequin/ADB/sub.adb"/></SubADBs></AnimDB>)"},
            {.name = "animations/mannequin/adb/sub.adb",
             .data = R"(<AnimDB><FragmentList><ADLG_Test><Fragment GUID="1" Tags="">)"
                     R"(<AnimLayer><Animation name="test_clip"/></AnimLayer>)"
                     R"(</Fragment></ADLG_Test></FragmentList></AnimDB>)"},
        };
        const std::filesystem::path root = temp_file("animfw_game");
        std::error_code error;
        std::filesystem::create_directories(root / "Data", error);
        {
            const std::optional<std::string> pak = write_stored_zip(files);
            std::ofstream out{root / "Data" / "Animations.pak", std::ios::binary};
            out.write(pak->data(), static_cast<std::streamsize>(pak->size()));
        }
        auto paks = GamePaks::open(root);
        CHECK(paks.has_value());
        if (paks)
        {
            const auto found = paks->find_clip("TEST_CLIP");
            CHECK(found.has_value() && found->path == clip_path && found->clip.tracks.size() == 1);
            CHECK(!paks->find_clip("no_clip").has_value());
            const auto skeleton = paks->male_skeleton();
            CHECK(skeleton.has_value() && skeleton->size() == 2);

            const std::vector<SkeletonBone> bones_read = skeleton.value_or(std::vector<SkeletonBone>{});
            const ModToolGame game{
                .files =
                    AnimCatalogFiles{
                        .read = [&paks](const std::string &path) { return paks->read(path); },
                        .patches = [](const std::string &) { return std::vector<std::string>{}; },
                    },
                .find_clip = [&paks](std::string_view name) { return paks->find_clip(name); },
                .skeleton = [&bones_read] { return &bones_read; },
            };
            std::vector<std::string> printed;
            const ToolPrint keep = [&printed](std::string_view line) { printed.emplace_back(line); };
            CHECK(report_clip_check(game, ClipCheckRequest{.clip = "test_clip"}, keep) == 0);
            CHECK(report_clip_check(game, ClipCheckRequest{.clip = "test_clip", .loops = true}, keep) == 1);
            CHECK(report_clip_check(game, ClipCheckRequest{.clip = "missing"}, keep) == -1);

            const ModAction action{.modid = "my_mod", .fragment = "MyMod_Test", .clip = "test_clip"};
            const auto made = make_mod_action(game, action, "ADLG_Test", ModPatches{}, keep);
            CHECK(made.has_value() && made->scopes == "FullBody");
            CHECK(made.has_value() && made->patches.database.find("MyMod_Test") != std::string::npos);
            CHECK(
                std::ranges::any_of(printed, [](const std::string &l) { return l.find("note:") != std::string::npos; })
            );
            ModAction game_name = action;
            game_name.fragment = "adlg_test";
            CHECK(!make_mod_action(game, game_name, "ADLG_Test", ModPatches{}, keep).has_value());
            CHECK(!make_mod_action(game, action, "NoSuchFragment", ModPatches{}, keep).has_value());
            ModAction markup_clip = action;
            markup_clip.clip = "test&clip";
            CHECK(!make_mod_action(game, markup_clip, "ADLG_Test", ModPatches{}, keep).has_value());
            // A clip name with a path in it would name an upper-body action's blend space outside its folder.
            for (const char *path_clip : {"../test_clip", "x\\test_clip", "c:test_clip", ""})
            {
                ModAction upper_path = action;
                upper_path.clip = path_clip;
                upper_path.upper = true;
                CHECK(!make_mod_action(game, upper_path, "ADLG_Test", ModPatches{}, keep).has_value());
            }
        }
        std::filesystem::remove_all(root, error);
        CHECK(!GamePaks::open(root).has_value());
    }

    void test_tag_rules()
    {
        using namespace AnimationFramework::tag_rules;
        // take_changed_bits: a play's bits go back, what the game changed since stays, a tag group's old value returns;
        // with the states swapped the play's bits come again.
        {
            const std::array<std::uint8_t, 2> before{0x05, 0x10};
            const std::array<std::uint8_t, 2> after{0x07, 0x20};
            std::array<std::uint8_t, 2> live{0x47, 0x20};
            take_changed_bits(live, before, after);
            CHECK((live == std::array<std::uint8_t, 2>{0x45, 0x10}));
            take_changed_bits(live, after, before);
            CHECK((live == std::array<std::uint8_t, 2>{0x47, 0x20}));
            std::array<std::uint8_t, 2> unchanged{0x47, 0x20};
            take_changed_bits(unchanged, after, after);
            CHECK((unchanged == std::array<std::uint8_t, 2>{0x47, 0x20}));
        }
        // requested_option: the first animfw_option_<n> among the play's tags; anything else asks for none.
        {
            CHECK(requested_option("animfw_option_2") == 2);
            CHECK(requested_option("sitting+animfw_option_0+animfw_temporary_tags") == 0);
            CHECK(requested_option("animfw_option_3+animfw_option_1") == 3);
            CHECK(requested_option("") == -1);
            CHECK(requested_option("animfw_own_scopes") == -1);
            CHECK(requested_option("animfw_option_") == -1);
            CHECK(requested_option("animfw_option_x1") == -1);
            CHECK(requested_option("animfw_option_99999999") == -1);
            static_assert(requested_option("a+animfw_option_12") == 12);
        }
        // covers: every bit of the game's answer, tag-group values included, nothing else required.
        const std::array<std::uint8_t, 3> plain{0x10, 0x00, 0x06};
        CHECK(covers(std::array<std::uint8_t, 3>{0x11, 0x02, 0x06}, plain));
        CHECK(!covers(std::array<std::uint8_t, 3>{0x10, 0x00, 0x02}, plain)); // a group value differs
        CHECK(!covers(std::array<std::uint8_t, 2>{0x10, 0x00}, plain));
        CHECK(covers(plain, std::array<std::uint8_t, 3>{}));

        // Combinations: the highest-ranked tag's sets first, most complete first.
        std::vector<std::uint32_t> order;
        for (std::uint32_t s = 0b111; s != 0; s = next_subset(0b111, s))
        {
            order.push_back(s);
        }
        CHECK((order == std::vector<std::uint32_t>{0b111, 0b110, 0b101, 0b100, 0b011, 0b010, 0b001}));
        order.clear();
        for (std::uint32_t s = 0b1010; s != 0; s = next_subset(0b1010, s))
        {
            order.push_back(s);
        }
        CHECK((order == std::vector<std::uint32_t>{0b1010, 0b1000, 0b0010}));
        CHECK(capped(0b111111) == 0b111100);
        CHECK(capped(0b1001) == 0b1001);

        CHECK(
            (parse_tag_list(" Tired, thirdPerson;wet  tired ,,") ==
             std::vector<std::string>{"tired", "thirdperson", "wet"})
        );
        CHECK(parse_tag_list("").empty());
        CHECK(mannequin_crc("thirdPerson") == mannequin_crc("thirdperson"));
        CHECK(mannequin_crc("") == 0u);
        CHECK(mannequin_crc("a") == 0xE8B7BE43u); // CRC-32 of "a"

        // Ranking: the INI list first (its first name highest), then declared priority, then declaration order.
        const std::vector<Declared> declared{{"thirdPerson", 0}, {"tired", 0}, {"wet", 5}, {"cold", 0}, {"Tired", 9}};
        const std::vector<std::size_t> plain_rank = rank(declared, {}, {});
        CHECK((plain_rank == std::vector<std::size_t>{3, 1, 0, 2})); // rising: cold < tired < thirdPerson < wet
        const std::vector<std::string> ini{"tired", "thirdperson"};
        const std::vector<std::string> off{"wet"};
        CHECK((rank(declared, ini, off) == std::vector<std::size_t>{3, 0, 1})); // cold < thirdPerson < tired
    }
} // namespace

namespace
{
    /**
     * @brief A minimal skeleton chunk file: a 4-byte chunk 0x1000 and a CompiledBones chunk of @p names, each record
     *        filled with @p fill (one byte per bone) before its name and parent offset.
     */
    [[nodiscard]] std::string make_skeleton(
        const std::vector<std::string> &names,
        const std::string &fill,
        std::string_view other = "abcd",
        bool extra_chunk = false
    )
    {
        constexpr std::size_t record = 584;
        std::string bones(32, '\0');
        for (std::size_t i = 0; i < names.size(); ++i)
        {
            std::string r(record, static_cast<char>(fill[i]));
            std::fill(r.begin() + 312, r.begin() + 576, '\0');
            std::memcpy(r.data() + 312, names[i].data(), names[i].size());
            const std::int32_t parent = i == 0 ? -1 : -1 * static_cast<std::int32_t>(i);
            std::memcpy(r.data() + 572, &parent, 4);
            bones += r;
        }
        struct Chunk
        {
            std::uint16_t type;
            std::uint16_t version;
            std::string data;
        };
        std::vector<Chunk> chunks{{0x1000, 1, std::string{other}}, {0x2000, 0x800, bones}};
        if (extra_chunk)
        {
            chunks.push_back({0x1001, 1, "zz"});
        }
        const auto u16 = [](std::string &out, std::uint16_t v) { out.append(reinterpret_cast<const char *>(&v), 2); };
        const auto u32 = [](std::string &out, std::uint32_t v) { out.append(reinterpret_cast<const char *>(&v), 4); };
        std::string file = "CrCh";
        u32(file, 0x746);
        u32(file, static_cast<std::uint32_t>(chunks.size()));
        u32(file, 16);
        std::uint32_t offset = static_cast<std::uint32_t>(16 + 16 * chunks.size());
        for (std::size_t i = 0; i < chunks.size(); ++i)
        {
            u16(file, chunks[i].type);
            u16(file, chunks[i].version);
            u32(file, static_cast<std::uint32_t>(i + 1));
            u32(file, static_cast<std::uint32_t>(chunks[i].data.size()));
            u32(file, offset);
            offset += static_cast<std::uint32_t>(chunks[i].data.size());
        }
        for (const Chunk &chunk : chunks)
        {
            file += chunk.data;
        }
        return file;
    }

    void test_chr_merge()
    {
        const std::vector<std::string> names{"Root", "Hips", "BackSlot_bow", "BackSlot_shield"};
        const std::string base = make_skeleton(names, "aaaa");
        const std::string a = make_skeleton(names, "aaba");         // moves BackSlot_bow
        const std::string b = make_skeleton(names, "aabc", "wxyz"); // same bow move, shield moved, chunk 0x1000
        const std::string c = make_skeleton(names, "aada");         // another bow move
        const std::array<SkeletonCopy, 2> ab{SkeletonCopy{"a", a}, SkeletonCopy{"b", b}};
        const auto merged = merge_skeletons(base, ab);
        CHECK(merged.has_value());
        if (merged)
        {
            CHECK(merged->bytes == make_skeleton(names, "aabc", "wxyz"));
            CHECK(merged->conflicts.empty());
            CHECK(merged->contributions.size() == 2);
            CHECK((merged->contributions[0].bones == std::vector<std::string>{"BackSlot_bow"}));
            CHECK((merged->contributions[1].bones == std::vector<std::string>{"BackSlot_bow", "BackSlot_shield"}));
            CHECK(merged->contributions[1].same_as_earlier == 1);
            CHECK((merged->contributions[1].chunks == std::vector<std::string>{"chunk 0x1000"}));
        }
        // Disjoint changes merge in either order; a different change to the same bone goes to the later mod.
        const std::array<SkeletonCopy, 2> ac{SkeletonCopy{"a", a}, SkeletonCopy{"c", c}};
        const auto conflicting = merge_skeletons(base, ac);
        CHECK(conflicting.has_value());
        if (conflicting)
        {
            CHECK(conflicting->bytes == c);
            CHECK(conflicting->conflicts.size() == 1);
            CHECK(
                !conflicting->conflicts.empty() && conflicting->conflicts[0].winner == "c" &&
                conflicting->conflicts[0].loser == "a" && conflicting->conflicts[0].what == "BackSlot_bow"
            );
        }
        // Another bone list or chunk layout refuses the merge, naming the copy.
        const std::string renamed = make_skeleton({"Root", "Hips", "BackSlot_bow", "BackSlot_axe"}, "aaaa");
        const std::array<SkeletonCopy, 2> ar{SkeletonCopy{"a", a}, SkeletonCopy{"r", renamed}};
        const auto refused = merge_skeletons(base, ar);
        CHECK(!refused.has_value() && refused.error().source == "r");
        const std::string extra = make_skeleton(names, "aaaa", "abcd", true);
        const std::array<SkeletonCopy, 1> ae{SkeletonCopy{"e", extra}};
        CHECK(!merge_skeletons(base, ae).has_value());
        // Float noise in a transform is no change: a re-exported copy neither moves a bone nor overrides one.
        const auto with_value = [&base](float value)
        {
            std::string file = base;
            const std::size_t transform = 16 + 2 * 16 + 4 + 32 + 2 * 584 + 216; // bone 2's first transform value
            std::memcpy(file.data() + transform, &value, sizeof(float));
            return file;
        };
        const std::string vanilla_one = with_value(1.0F);
        const std::string moved = with_value(1.5F);
        const std::string moved_noise = with_value(1.5000003F);
        const std::string noisy_vanilla = with_value(1.0000003F);
        const std::array<SkeletonCopy, 2> mn{SkeletonCopy{"m", moved}, SkeletonCopy{"n", moved_noise}};
        const auto same_move = merge_skeletons(vanilla_one, mn);
        CHECK(
            same_move.has_value() && same_move->conflicts.empty() && same_move->contributions[1].same_as_earlier == 1
        );
        const std::array<SkeletonCopy, 2> mv{SkeletonCopy{"m", moved}, SkeletonCopy{"v", noisy_vanilla}};
        const auto kept = merge_skeletons(vanilla_one, mv);
        CHECK(kept.has_value() && kept->bytes == moved && kept->contributions[1].bones.empty());

        // A chunk two copies change the same way counts in both copies' chunks, never as a bone the same as earlier.
        const std::string d = make_skeleton(names, "aaaa", "wxyz");
        const std::array<SkeletonCopy, 2> bd{SkeletonCopy{"b", b}, SkeletonCopy{"d", d}};
        const auto chunk_twice = merge_skeletons(base, bd);
        CHECK(
            chunk_twice.has_value() && chunk_twice->conflicts.empty() &&
            chunk_twice->contributions[1].same_as_earlier == 0
        );
        CHECK((
            chunk_twice.has_value() && chunk_twice->contributions[1].chunks == std::vector<std::string>{"chunk 0x1000"}
        ));

        const std::array<SkeletonCopy, 1> just_a{SkeletonCopy{"a", a}};
        CHECK(!merge_skeletons("not a chunk file", just_a).has_value());
        CHECK(!merge_skeletons(base.substr(0, base.size() - 8), just_a).has_value());
    }

    void test_anim_catalog()
    {
        const std::string male_database = R"(<AnimDB>
  <SubADBs><SubADB Tags="x" File="Animations/Mannequin/ADB/Sub.adb"/></SubADBs>
  <FragmentList>
    <Wave>
      <Fragment uid="1" Tags="sitting" FragTags=""><AnimLayer><Animation name="wave_seat"/></AnimLayer></Fragment>
      <Fragment uid="2" Tags="a+b" FragTags=""><AnimLayer><Animation name="wave_ab"/></AnimLayer></Fragment>
      <Fragment uid="3" Tags="c" FragTags="">
        <AnimLayer><Animation name="wave_c"/><Animation name="wave_c2"/></AnimLayer>
      </Fragment>
      <Fragment uid="4" Tags="c" FragTags=""><AnimLayer><Animation name="wave_c3"/></AnimLayer></Fragment>
      <Fragment uid="5" Tags="horse+Scope_Upper" FragTags="">
        <AnimLayer><Animation name="wave_scope"/></AnimLayer>
      </Fragment>
    </Wave>
    <Empty>
      <Fragment uid="6" Tags="" FragTags=""/>
    </Empty>
    <Ride>
      <Fragment uid="7" Tags="horse" FragTags="fast"><AnimLayer><Animation name="ride"/></AnimLayer></Fragment>
    </Ride>
    <NotAnId>
      <Fragment uid="8" Tags="" FragTags=""><AnimLayer><Animation name="stray"/></AnimLayer></Fragment>
    </NotAnId>
  </FragmentList>
  <FragmentBlendList>
    <Blend from="Wave" to="Ride"><Variant from="" to=""><Fragment uid="9" blendOutDuration="0.2" /></Variant></Blend>
  </FragmentBlendList>
</AnimDB>)";
        const std::string sub_database = R"(<AnimDB><FragmentList>
    <Face>
      <Fragment uid="1" Tags="" FragTags=""><AnimLayer><Animation name="face"/></AnimLayer></Fragment>
    </Face>
</FragmentList></AnimDB>)";
        const std::string fragment_ids = R"(<TagDefinition><Tags>
  <Tag name="Wave"/><Tag name="Empty"/><Tag name="Ride"/><Tag name="Face"/><Tag name="Dog"/>
</Tags></TagDefinition>)";
        const std::string controller_defs = R"(<ControllerDef><FragmentDefs>
  <Wave scopes="FullBody+Upper" flags=""><Override tags="sitting" scopes="Upper"/></Wave>
  <Face scopes="Upper"/>
  <Ride scopes=""/>
</FragmentDefs></ControllerDef>)";
        const std::string female_database = R"(<AnimDB><FragmentList>
    <Wave>
      <Fragment uid="1" Tags="" FragTags=""/>
    </Wave>
</FragmentList></AnimDB>)";
        std::map<std::string, std::string> texts{
            {"animations/mannequin/adb/kcd_male_database.adb", male_database},
            {"animations/mannequin/adb/sub.adb", sub_database},
            {"animations/mannequin/adb/kcd_male_fragmentids.xml", fragment_ids},
            {"animations/mannequin/adb/kcd_male_controllerdefs.xml", controller_defs},
            {"animations/mannequin/adb/wh_female_database.adb", female_database},
        };
        std::map<std::string, std::vector<std::string>> patches;
        const AnimCatalogFiles files{
            .read = [&texts](const std::string &path) -> std::optional<std::string>
            {
                const auto it = texts.find(path);
                return it != texts.end() ? std::optional{it->second} : std::nullopt;
            },
            .patches = [&patches](const std::string &path) -> std::vector<std::string>
            {
                const auto it = patches.find(path);
                return it != patches.end() ? it->second : std::vector<std::string>{};
            },
        };

        {
            const AnimCatalog catalog{files};
            CHECK(catalog.fragment_count() == 4);
            CHECK(catalog.option_count() == 7);
            CHECK(
                catalog.text() == "Empty\t\t\t\t\t\n"
                                  "Face\t\tpartial\t\t\t\n"
                                  "Ride\thorse+fast\t\t\thorse:horse+fast\t\n"
                                  "Wave\tc\t\tseat:sitting\thorse:horse\twomen\n"
                                  "@Wave\twave_seat\tsitting\t0\tseat\t\t\n"
                                  "@Wave\twave_ab\ta+b\t0\t\t\t\n"
                                  "@Wave\twave_c\tc\t0\tstand\t\twave_c2\n"
                                  "@Wave\twave_c3\tc\t1\tstand\t\t\n"
                                  "@Ride\tride\thorse+fast\t0\thorse\t\t\n"
                                  "@NotAnId\tstray\t\t0\tstand\t\t\n"
                                  "@Face\tface\t\t0\tstand\tpartial\t\n"
            );

            const std::vector<std::string> none;
            const AnimCatalogPage everything = catalog.find(AnimCatalogQuery{.count = 2}, none);
            CHECK(everything.total == 4);
            CHECK(everything.lines == "Empty\t\t\t\t\t\nFace\t\tpartial\t\t\t\n");
            CHECK(
                catalog.find(AnimCatalogQuery{.first = 3, .count = 2}, none).lines ==
                "Wave\tc\t\tseat:sitting\thorse:horse\twomen\n"
            );
            CHECK(catalog.find(AnimCatalogQuery{.text = "WAVE_C", .options = false, .count = 9}, none).total == 0);
            CHECK(catalog.find(AnimCatalogQuery{.text = "WAVE_C", .options = true, .count = 9}, none).total == 2);
            CHECK(catalog.find(AnimCatalogQuery{.options = true, .posture = "seat", .count = 9}, none).total == 2);
            CHECK(catalog.find(AnimCatalogQuery{.options = true, .posture = "horse", .count = 9}, none).total == 3);
            const std::vector<std::string> hidden{"Wa"};
            CHECK(catalog.find(AnimCatalogQuery{.options = true, .count = 0}, hidden).total == 6);
            CHECK(catalog.fragment_line("Face") == "Face\t\tpartial\t\t\t");
            CHECK(!catalog.fragment_line("Fac").has_value());
            CHECK(!catalog.fragment_line("Dog").has_value());
        }

        // A patch replaces the options with its tags in place, whatever their order, keeps the numbering of the rest,
        // appends, removes, adds a fragment and a sub-database, and sets a fragment's scopes.
        patches["animations/mannequin/adb/kcd_male_database.adb"] = {
            R"(<AnimDB>
  <SubADBs><SubADB File="animations/mannequin/adb/mod.adb"/></SubADBs>
  <FragmentList>
    <Wave>
      <Fragment Tags="b+A" FragTags=""><AnimLayer><Animation name="mod_ab"/></AnimLayer></Fragment>
      <Fragment Tags="c" FragTags="" Mode="append"><AnimLayer><Animation name="mod_c"/></AnimLayer></Fragment>
      <Fragment Tags="sitting" FragTags="" Mode="remove"/>
      <Fragment Tags="new" FragTags=""><AnimLayer><Animation name="mod_new"/></AnimLayer></Fragment>
    </Wave>
    <Mod_Own>
      <Fragment Tags="" FragTags=""><AnimLayer><Animation name="own"/></AnimLayer></Fragment>
    </Mod_Own>
  </FragmentList>
</AnimDB>)",
        };
        patches["animations/mannequin/adb/kcd_male_fragmentids.xml"] = {
            R"(<TagDefinition><Tags><Tag name="Mod_Own"/></Tags></TagDefinition>)",
        };
        patches["animations/mannequin/adb/kcd_male_controllerdefs.xml"] = {
            R"(<ControllerDef><FragmentDefs><Mod_Own scopes="Upper"/>)"
            R"(<Wave scopes="Upper"/></FragmentDefs></ControllerDef>)",
        };
        texts["animations/mannequin/adb/mod.adb"] = R"(<AnimDB><FragmentList>
    <Mod_Sub>
      <Fragment Tags="" FragTags=""><AnimLayer><Animation name="mod_sub"/></AnimLayer></Fragment>
    </Mod_Sub>
</FragmentList></AnimDB>)";
        {
            const AnimCatalog catalog{files};
            const std::string_view text = catalog.text();
            CHECK(catalog.fragment_count() == 5);
            CHECK(catalog.fragment_line("Mod_Own") == "Mod_Own\t\tpartial\t\t\t");
            CHECK(catalog.fragment_line("Wave") == "Wave\tc\tpartial\t\thorse:horse\twomen");
            CHECK(!catalog.fragment_line("Mod_Sub").has_value());
            CHECK(!text.contains("wave_seat"));
            CHECK(!text.contains("wave_ab"));
            CHECK(text.contains("@Wave\tmod_ab\tb+A\t0\t\tpartial\t\n"));
            CHECK(text.contains("@Wave\twave_c3\tc\t1\tstand\tpartial\t\n@Wave\tmod_c\tc\t2\tstand\tpartial\t\n"));
            CHECK(text.contains("@Wave\tmod_new\tnew\t0\t\tpartial\t\n"));
            CHECK(text.find("mod_ab") < text.find("wave_c\t"));
            CHECK(text.contains("@Mod_Sub\tmod_sub\t\t0\tstand\t\t\n"));
        }

        patches["animations/mannequin/adb/kcd_male_controllerdefs.xml"] = {
            R"(<ControllerDef><FragmentDefs><Face scopes="" Mode="replace"/></FragmentDefs></ControllerDef>)",
        };
        {
            const AnimCatalog catalog{files};
            CHECK(catalog.fragment_line("Face") == "Face\t\t\t\t\t");
        }
    }

    /**
     * @brief Writes the catalog of the real game files to @p argv[2] and times it. The first pak is the game's, the
     *        rest are mods' in load order.
     */
    int run_catalog(int argc, char **argv)
    {
        struct Pak
        {
            std::filesystem::path path;
            std::vector<ZipEntry> index;
        };
        std::vector<Pak> paks;
        for (int i = 3; i < argc; ++i)
        {
            std::optional<std::vector<ZipEntry>> index = read_zip_index(argv[i]);
            if (!index.has_value())
            {
                std::printf("catalog: could not read %s\n", argv[i]);
                return 1;
            }
            paks.push_back(Pak{argv[i], std::move(*index)});
        }
        const AnimCatalogFiles files{
            .read = [&paks](const std::string &path) -> std::optional<std::string>
            {
                for (auto pak = paks.rbegin(); pak != paks.rend(); ++pak)
                {
                    const auto entry = std::ranges::find(pak->index, path, &ZipEntry::name);
                    if (entry != pak->index.end())
                    {
                        return read_zip_file(pak->path, *entry);
                    }
                }
                return std::nullopt;
            },
            .patches = [&paks](const std::string &path) -> std::vector<std::string>
            {
                const std::size_t dot = path.find_last_of('.');
                const std::string prefix = path.substr(0, dot) + "__";
                const std::string extension = path.substr(dot);
                std::vector<std::string> texts;
                for (const Pak &pak : paks)
                {
                    for (const ZipEntry &entry : pak.index)
                    {
                        if (entry.name.starts_with(prefix) && entry.name.ends_with(extension))
                        {
                            texts.push_back(read_zip_file(pak.path, entry).value_or(std::string{}));
                        }
                    }
                }
                return texts;
            },
        };
        using Clock = std::chrono::steady_clock;
        const Clock::time_point started = Clock::now();
        const AnimCatalog catalog{files};
        const Clock::duration elapsed = Clock::now() - started;
        std::ofstream{argv[2], std::ios::binary} << catalog.text();
        std::printf(
            "catalog: %zu fragments, %zu options, %zu bytes in %lld ms\n",
            catalog.fragment_count(),
            catalog.option_count(),
            catalog.text().size(),
            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count())
        );
        const std::vector<std::string> none;
        const Clock::time_point search_started = Clock::now();
        const AnimCatalogPage page =
            catalog.find(AnimCatalogQuery{.text = "sword", .options = true, .count = 40}, none);
        const Clock::duration search_elapsed = Clock::now() - search_started;
        std::printf(
            "catalog: 'sword' matches %zu lines in %lld us\n",
            page.total,
            static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(search_elapsed).count())
        );
        return 0;
    }

    /** @brief `chr <out> <game copy> <mod copy>...`: merges real skeleton copies offline. */
    int run_chr_merge(int argc, char **argv)
    {
        const std::string base = read_file(argv[3]);
        std::vector<std::string> files;
        std::vector<SkeletonCopy> copies;
        for (int i = 4; i < argc; ++i)
        {
            files.push_back(read_file(argv[i]));
        }
        for (int i = 4; i < argc; ++i)
        {
            copies.push_back(SkeletonCopy{argv[i], files[static_cast<std::size_t>(i - 4)]});
        }
        const auto merged = merge_skeletons(base, copies);
        if (!merged)
        {
            std::printf("refused: %s: %s\n", merged.error().source.c_str(), merged.error().reason.c_str());
            return 1;
        }
        for (const SkeletonContribution &c : merged->contributions)
        {
            std::string bones;
            for (const std::string &bone : c.bones)
            {
                bones += (bones.empty() ? "" : ", ") + bone;
            }
            std::printf(
                "%s: %zu bone(s) [%s], %zu same as earlier, %zu other chunk(s)\n",
                c.source.c_str(),
                c.bones.size(),
                bones.c_str(),
                c.same_as_earlier,
                c.chunks.size()
            );
        }
        for (const SkeletonConflict &c : merged->conflicts)
        {
            std::printf("conflict: %s overrides %s on %s\n", c.winner.c_str(), c.loser.c_str(), c.what.c_str());
        }
        std::ofstream{argv[2], std::ios::binary} << merged->bytes;
        return 0;
    }
} // namespace

namespace
{
    using namespace AnimationFramework;

    [[nodiscard]] std::span<const std::byte> bytes_of(const std::string &text)
    {
        return std::as_bytes(std::span{text});
    }

    /** @brief One line per clip: what the motion checks measure, for tuning their limits on the game's own clips. */
    void print_clip_measure(std::ostream &out, const CafClip &clip, const std::vector<SkeletonBone> &skeleton)
    {
        std::vector<std::uint32_t> controllers;
        for (const ClipTrack &track : clip.tracks)
        {
            controllers.push_back(track.controller);
        }
        const std::vector<ClipFinding> bones = check_clip_bones(controllers, skeleton);
        const ClipMotionMeasure m = measure_clip_motion(clip, skeleton);
        out << std::format(
            "{}\t{:.3f}\t{}\t{:.2f}\t{}\t{:.2f}\t{}\t{:.3f}\t{:.3f}\t{}\n",
            clip.name,
            clip.seconds(),
            clip.tracks.size(),
            m.seam_degrees,
            m.seam_rotation_bone,
            m.seam_metres * 100.0,
            m.seam_position_bone,
            m.root_travel.value_or(-1.0),
            m.foot_slide.value_or(-1.0),
            bones.size()
        );
        for (const ClipFinding &finding : bones)
        {
            out << "#\t" << clip.name << "\t" << finding.text << "\n";
        }
    }

    /** @brief `clips <male.chr> <pak> <out.tsv>`: measures every male clip of a pak, loose and packed in .dba files. */
    int run_clips([[maybe_unused]] int argc, char **argv)
    {
        const std::string chr = read_file(argv[2]);
        const auto skeleton = read_skeleton_file(bytes_of(chr));
        const auto index = read_zip_index(argv[3]);
        if (!skeleton || !index)
        {
            std::cerr << "cannot read the skeleton or the pak\n";
            return 1;
        }
        std::ofstream out{argv[4], std::ios::binary};
        out << "clip\tseconds\ttracks\tseam_deg\tseam_bone\tseam_cm\tseam_move_bone\troot_m\tslide_mps\t"
               "bone_findings\n";
        int clips = 0;
        int failed = 0;
        for (const ZipEntry &entry : *index)
        {
            if (!entry.name.starts_with("animations/humans/male/"))
            {
                continue;
            }
            const bool caf = entry.name.ends_with(".caf");
            if (!caf && !entry.name.ends_with(".dba"))
            {
                continue;
            }
            const std::optional<std::string> data = read_zip_file(argv[3], entry);
            if (!data)
            {
                ++failed;
                continue;
            }
            if (caf)
            {
                auto clip = read_caf_file(bytes_of(*data));
                if (!clip)
                {
                    std::cerr << entry.name << ": " << clip.error() << "\n";
                    ++failed;
                    continue;
                }
                clip->name = entry.name;
                print_clip_measure(out, *clip, *skeleton);
                ++clips;
                continue;
            }
            auto packed = read_dba_clips(bytes_of(*data));
            if (!packed)
            {
                std::cerr << entry.name << ": " << packed.error() << "\n";
                ++failed;
                continue;
            }
            for (const CafClip &clip : *packed)
            {
                print_clip_measure(out, clip, *skeleton);
                ++clips;
            }
        }
        std::cout << clips << " clip(s) measured, " << failed << " file(s) failed\n";
        return failed == 0 ? 0 : 1;
    }

    /** @brief `caf-dump <file.caf|file.dba> [stem]`: every track's key counts and first and last keys. */
    int run_caf_dump(int argc, char **argv)
    {
        const std::string data = read_file(argv[2]);
        std::vector<CafClip> clips;
        if (argc >= 4)
        {
            const std::vector<std::string> stems{argv[3]};
            auto packed = read_dba_clips(bytes_of(data), stems);
            if (!packed)
            {
                std::cerr << packed.error() << "\n";
                return 1;
            }
            clips = std::move(*packed);
        }
        else
        {
            auto clip = read_caf_file(bytes_of(data));
            if (!clip)
            {
                std::cerr << clip.error() << "\n";
                return 1;
            }
            clips.push_back(std::move(*clip));
        }
        for (const CafClip &clip : clips)
        {
            std::cout << std::format("{} {} {}\n", clip.name, clip.start_frame, clip.end_frame);
            for (const ClipTrack &t : clip.tracks)
            {
                std::cout << std::format("{:08x} {} {}", t.controller, t.rotations.size(), t.positions.size());
                if (!t.rotations.empty())
                {
                    const Quat &a = t.rotations.front();
                    const Quat &b = t.rotations.back();
                    std::cout << std::format(
                        " r {:.5f} {:.5f} {:.5f} {:.5f} | {:.5f} {:.5f} {:.5f} {:.5f} f {:.1f} {:.1f}",
                        a.x,
                        a.y,
                        a.z,
                        a.w,
                        b.x,
                        b.y,
                        b.z,
                        b.w,
                        t.rotation_frames.front(),
                        t.rotation_frames.back()
                    );
                }
                if (!t.positions.empty())
                {
                    const Vec3 &p = t.positions.front();
                    const Vec3 &q = t.positions.back();
                    std::cout << std::format(
                        " p {:.5f} {:.5f} {:.5f} | {:.5f} {:.5f} {:.5f} f {:.1f} {:.1f}",
                        p.x,
                        p.y,
                        p.z,
                        q.x,
                        q.y,
                        q.z,
                        t.position_frames.front(),
                        t.position_frames.back()
                    );
                }
                std::cout << "\n";
            }
        }
        return 0;
    }
} // namespace

int main(int argc, char **argv)
{
    if (argc >= 5 && std::string_view{argv[1]} == "clips")
    {
        return run_clips(argc, argv);
    }
    if (argc >= 3 && std::string_view{argv[1]} == "caf-dump")
    {
        return run_caf_dump(argc, argv);
    }
    if (argc >= 4 && std::string_view{argv[1]} == "catalog")
    {
        return run_catalog(argc, argv);
    }
    if (argc >= 5 && std::string_view{argv[1]} == "chr")
    {
        return run_chr_merge(argc, argv);
    }
    if (argc >= 4)
    {
        return run_delta(argv[1], argv[2], argv[3], argc >= 5 ? argv[4] : nullptr);
    }
    test_text_util();
    test_bridge_protocol();
    test_clip_path_hash();
    test_clip_header();
    test_clip_cache();
    test_zip_reader();
    test_zip_writer();
    test_caf_reader();
    test_clip_compiler();
    test_retarget();
    test_mod_maker();
    test_offline_game();
    test_adb_delta();
    test_controller_defs_delta();
    test_anim_events_delta();
    test_tag_definition_delta();
    test_table_rows_delta();
    test_tag_rules();
    test_chr_merge();
    test_anim_catalog();
    return AnimationFramework::tests::finish("core");
}
