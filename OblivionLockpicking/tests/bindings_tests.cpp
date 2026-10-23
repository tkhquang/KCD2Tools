/**
 * @file tests/bindings_tests.cpp
 * @brief Unit tests of the key bindings: list parsing, matching through the controller twins, and the hints.
 *        Plain asserts, no framework: the exit code is the verdict.
 */

#include "controls/bindings.hpp"

#include <cstdio>
#include <string>

namespace
{
    using namespace OblivionLockpicking::controls;

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

    void test_parse()
    {
        const KeyList keys = parse_key_list("  Mouse1, D ,,xi_TriggerL,  ");
        CHECK(keys.count == 3);
        // An empty item is no name, so the parse does not count it as dropped.
        CHECK(keys.dropped == 0);
        CHECK(keys.name(0) == "mouse1");
        CHECK(keys.name(1) == "d");
        CHECK(keys.name(2) == "xi_triggerl");

        CHECK(parse_key_list("").count == 0);
        CHECK(parse_key_list(" , ").count == 0);
        // Too long to be a key name.
        const KeyList long_name = parse_key_list("a_name_far_too_long_for_any_key");
        CHECK(long_name.count == 0 && long_name.dropped == 1);
        // Past the capacity.
        const KeyList full = parse_key_list("a,b,c,d,e,f,g,h,i,j");
        CHECK(full.count == KEY_LIST_CAPACITY && full.dropped == 2);
    }

    void test_find_key()
    {
        const KeyList keys = parse_key_list("mouse1, xi_a, pad_l1, xi_triggerl");
        CHECK(find_key(keys, "mouse1").entry == 0);
        CHECK(find_key(keys, "mouse2").entry == -1);
        CHECK(find_key(keys, "xi_a").entry == 1);
        CHECK(find_key(keys, "xi_a").source == 0);
        // A PlayStation controller posts its own names for the same buttons.
        CHECK(find_key(keys, "pad_cross").entry == 1);
        CHECK(find_key(keys, "pad_cross").source != 0);
        // A PlayStation name in the list matches the Xbox name too.
        CHECK(find_key(keys, "xi_shoulderl").entry == 2);
        // A trigger matches its analog and digital names on both controllers, each as its own source.
        const KeyMatch analog = find_key(keys, "xi_triggerl");
        const KeyMatch digital = find_key(keys, "xi_triggerl_btn");
        CHECK(analog.entry == 3 && digital.entry == 3 && analog.source != digital.source);
        CHECK(find_key(keys, "pad_l2").entry == 3);
        CHECK(find_key(keys, "pad_ltrigger").entry == 3);
        CHECK(find_key(keys, "xi_triggerr").entry == -1);
        CHECK(find_key(keys, "").entry == -1);
    }

    void test_sticks()
    {
        CHECK(parse_pad_stick("left") == PadStick::Left);
        CHECK(parse_pad_stick(" NONE ") == PadStick::None);
        CHECK(parse_pad_stick("Right") == PadStick::Right);
        CHECK(!parse_pad_stick("sideways").has_value());
        CHECK(parse_pad_stick(pad_stick_name(PadStick::None)) == PadStick::None);
        CHECK(stick_axes(PadStick::Right).x == "xi_thumbrx");
        CHECK(stick_axes(PadStick::Left).y == "xi_thumbly");
        CHECK(stick_axes(PadStick::None).x.empty());
        CHECK(same_key("xi_thumbrx", "pad_stickrx"));
        CHECK(!same_key("xi_thumbrx", "xi_thumbry"));
    }

    void test_hint_device()
    {
        CHECK(hint_device_from_code(1) == HintDevice::KeyboardMouse);
        CHECK(hint_device_from_code(2) == HintDevice::Xbox);
        CHECK(hint_device_from_code(16) == HintDevice::PlayStation);
        CHECK(hint_device_from_code(0) == HintDevice::KeyboardMouse);
    }

    void test_hint_row()
    {
        Bindings bindings{
            .set_keys = parse_key_list("mouse1, d, xi_triggerl"),
            .pad_stick = PadStick::Right,
        };
        CHECK(
            control_hint_row(bindings, HintDevice::KeyboardMouse) ==
            "@ui_oblivion_lockpicking_hint_move #maxis_x\t@ui_oblivion_lockpicking_hint_push #maxis_y "
            "@ui_oblivion_lockpicking_hint_up\t@ui_oblivion_lockpicking_hint_set #mouse1 "
            "@ui_oblivion_lockpicking_hint_or #key:@ui_control_uc_31"
        );
        CHECK(
            control_hint_row(bindings, HintDevice::Xbox) ==
            "@ui_oblivion_lockpicking_hint_move #xi_thumbr_move\t@ui_oblivion_lockpicking_hint_push #xi_thumbr_move "
            "@ui_oblivion_lockpicking_hint_up\t@ui_oblivion_lockpicking_hint_set #xi_triggerl"
        );
        CHECK(
            control_hint_row(bindings, HintDevice::PlayStation) ==
            "@ui_oblivion_lockpicking_hint_move #pad_thumbr_move\t@ui_oblivion_lockpicking_hint_push #pad_thumbr_move "
            "@ui_oblivion_lockpicking_hint_up\t@ui_oblivion_lockpicking_hint_set #pad_l2"
        );

        // Keycaps by width, with the game's own key labels. A PlayStation name shows its Xbox button on Xbox.
        bindings.set_keys = parse_key_list("1, escape, space, mouse4, pad_cross");
        bindings.push_keys = parse_key_list("");
        bindings.push_with_mouse_up = false;
        CHECK(
            control_hint_row(bindings, HintDevice::KeyboardMouse) ==
            "@ui_oblivion_lockpicking_hint_move #maxis_x\t@ui_oblivion_lockpicking_hint_set #key:@ui_key_1 "
            "@ui_oblivion_lockpicking_hint_or #escape:@ui_key_escape @ui_oblivion_lockpicking_hint_or "
            "#enter:@ui_key_space @ui_oblivion_lockpicking_hint_or "
            "#enter:@ui_key_mouse4"
        );
        CHECK(
            control_hint_row(bindings, HintDevice::Xbox) ==
            "@ui_oblivion_lockpicking_hint_move #xi_thumbr_move\t@ui_oblivion_lockpicking_hint_push #xi_thumbr_move "
            "@ui_oblivion_lockpicking_hint_up\t@ui_oblivion_lockpicking_hint_set #xi_a"
        );

        bindings.pad_stick = PadStick::Left;
        CHECK(
            control_hint_row(bindings, HintDevice::PlayStation) ==
            "@ui_oblivion_lockpicking_hint_move #pad_thumbl_move\t@ui_oblivion_lockpicking_hint_push #pad_thumbl_move "
            "@ui_oblivion_lockpicking_hint_up\t@ui_oblivion_lockpicking_hint_set #pad_cross"
        );

        // A pad layout: the left stick, A to set, the right trigger to push.
        bindings.set_keys = parse_key_list("mouse1, xi_a");
        bindings.push_keys = parse_key_list("xi_triggerr, space");
        bindings.push_with_mouse_up = true;
        CHECK(
            control_hint_row(bindings, HintDevice::Xbox) ==
            "@ui_oblivion_lockpicking_hint_move #xi_thumbl_move\t@ui_oblivion_lockpicking_hint_push #xi_thumbl_move "
            "@ui_oblivion_lockpicking_hint_up @ui_oblivion_lockpicking_hint_or #xi_triggerr\t"
            "@ui_oblivion_lockpicking_hint_set #xi_a"
        );
        CHECK(
            control_hint_row(bindings, HintDevice::KeyboardMouse) ==
            "@ui_oblivion_lockpicking_hint_move #maxis_x\t@ui_oblivion_lockpicking_hint_push #maxis_y "
            "@ui_oblivion_lockpicking_hint_up @ui_oblivion_lockpicking_hint_or #enter:@ui_key_space\t"
            "@ui_oblivion_lockpicking_hint_set #mouse1"
        );

        // No stick, no mouse push, no pad keys: only what still works is listed.
        bindings.pad_stick = PadStick::None;
        bindings.push_with_mouse_up = false;
        bindings.set_keys = parse_key_list("mouse1, xi_a, pad_cross");
        bindings.push_keys = parse_key_list("");
        CHECK(control_hint_row(bindings, HintDevice::Xbox) == "@ui_oblivion_lockpicking_hint_set #xi_a");
        CHECK(
            control_hint_row(bindings, HintDevice::KeyboardMouse) ==
            "@ui_oblivion_lockpicking_hint_move #maxis_x\t@ui_oblivion_lockpicking_hint_set #mouse1"
        );
    }

    void check_row(const std::string &row, const char *expected, int line)
    {
        if (row != expected)
        {
            std::printf("FAIL line %d: hint row\n  got      %s\n  expected %s\n", line, row.c_str(), expected);
            ++s_failures;
        }
    }

    void test_move_keys()
    {
        // The defaults, Oblivion Remastered's layout: A and D step the pick, the mouse still moves it.
        Bindings bindings{
            .move_left_keys = parse_key_list("a"),
            .move_right_keys = parse_key_list("d"),
            .set_keys = parse_key_list("space, mouse1, xi_a"),
            .push_keys = parse_key_list("w"),
        };
        CHECK(bindings.pad_stick == PadStick::Left);
        check_row(
            control_hint_row(bindings, HintDevice::KeyboardMouse),
            "@ui_oblivion_lockpicking_hint_move #key:@ui_control_uc_29 #key:@ui_control_uc_31 "
            "@ui_oblivion_lockpicking_hint_or #maxis_x\t@ui_oblivion_lockpicking_hint_push #maxis_y "
            "@ui_oblivion_lockpicking_hint_up @ui_oblivion_lockpicking_hint_or #key:@ui_control_uc_16\t"
            "@ui_oblivion_lockpicking_hint_set #enter:@ui_key_space @ui_oblivion_lockpicking_hint_or #mouse1",
            __LINE__
        );
        check_row(
            control_hint_row(bindings, HintDevice::Xbox),
            "@ui_oblivion_lockpicking_hint_move #xi_thumbl_move\t@ui_oblivion_lockpicking_hint_push #xi_thumbl_move "
            "@ui_oblivion_lockpicking_hint_up\t@ui_oblivion_lockpicking_hint_set #xi_a",
            __LINE__
        );

        // Pad move keys pair up after the stick. A key without a partner shows alone.
        bindings.move_left_keys = parse_key_list("a, xi_dpad_left");
        bindings.move_right_keys = parse_key_list("d, xi_dpad_right, right");
        check_row(
            control_hint_row(bindings, HintDevice::KeyboardMouse),
            "@ui_oblivion_lockpicking_hint_move #key:@ui_control_uc_29 #key:@ui_control_uc_31 "
            "@ui_oblivion_lockpicking_hint_or #escape:@ui_key_right @ui_oblivion_lockpicking_hint_or #maxis_x	"
            "@ui_oblivion_lockpicking_hint_push #maxis_y @ui_oblivion_lockpicking_hint_up "
            "@ui_oblivion_lockpicking_hint_or #key:@ui_control_uc_16	"
            "@ui_oblivion_lockpicking_hint_set #enter:@ui_key_space @ui_oblivion_lockpicking_hint_or #mouse1",
            __LINE__
        );
        check_row(
            control_hint_row(bindings, HintDevice::Xbox),
            "@ui_oblivion_lockpicking_hint_move #xi_thumbl_move @ui_oblivion_lockpicking_hint_or #xi_dpad_left "
            "#xi_dpad_right\t@ui_oblivion_lockpicking_hint_push #xi_thumbl_move @ui_oblivion_lockpicking_hint_up\t"
            "@ui_oblivion_lockpicking_hint_set #xi_a",
            __LINE__
        );
    }

    void test_equality()
    {
        Bindings a{
            .set_keys = parse_key_list("mouse1, d"),
        };
        Bindings b{
            .set_keys = parse_key_list("mouse1,d"),
        };
        CHECK(a == b);
        b.pad_stick = PadStick::Right;
        CHECK(!(a == b));
        b.pad_stick = a.pad_stick;
        b.move_left_keys = parse_key_list("a");
        CHECK(!(a == b));
    }
} // namespace

int main()
{
    test_parse();
    test_find_key();
    test_sticks();
    test_hint_device();
    test_hint_row();
    test_move_keys();
    test_equality();
    if (s_failures != 0)
    {
        std::printf("%d failure(s)\n", s_failures);
        return 1;
    }
    std::printf("all bindings tests passed\n");
    return 0;
}
