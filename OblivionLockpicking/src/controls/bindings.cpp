/**
 * @file controls/bindings.cpp
 * @brief Key list parsing, key matching with the controller twins, and the control hints.
 */

#include "controls/bindings.hpp"
#include "strings.hpp"

#include <algorithm>
#include <vector>

namespace OblivionLockpicking::controls
{
    namespace
    {
        /** @brief One controller control: its Xbox name, the other names it posts and its icons. */
        struct PadKey
        {
            /// The Xbox name.
            std::string_view name;
            /// The PlayStation names of the same control and, for a trigger, the digital button the engine also posts.
            std::array<std::string_view, 3> twins;
            /// The control's Xbox frame in the game's button movie.
            std::string_view xbox_icon;
            /// The control's PlayStation frame in the game's button movie.
            std::string_view playstation_icon;
        };

        constexpr PadKey PAD_KEYS[] = {
            {"xi_a", {"pad_cross"}, "xi_a", "pad_cross"},
            {"xi_b", {"pad_circle"}, "xi_b", "pad_circle"},
            {"xi_x", {"pad_square"}, "xi_x", "pad_square"},
            {"xi_y", {"pad_triangle"}, "xi_y", "pad_triangle"},
            {"xi_shoulderl", {"pad_l1"}, "xi_shoulderl", "pad_l1"},
            {"xi_shoulderr", {"pad_r1"}, "xi_shoulderr", "pad_r1"},
            {"xi_triggerl", {"xi_triggerl_btn", "pad_ltrigger", "pad_l2"}, "xi_triggerl", "pad_l2"},
            {"xi_triggerr", {"xi_triggerr_btn", "pad_rtrigger", "pad_r2"}, "xi_triggerr", "pad_r2"},
            {"xi_thumbl", {"pad_l3"}, "xi_thumbl", "pad_l3"},
            {"xi_thumbr", {"pad_r3"}, "xi_thumbr", "pad_r3"},
            {"xi_dpad_up", {"pad_up"}, "xi_dpad_up", "pad_up"},
            {"xi_dpad_down", {"pad_down"}, "xi_dpad_down", "pad_down"},
            {"xi_dpad_left", {"pad_left"}, "xi_dpad_left", "pad_left"},
            {"xi_dpad_right", {"pad_right"}, "xi_dpad_right", "pad_right"},
            {"xi_start", {"pad_start"}, "xi_start", "pad_start"},
            {"xi_back", {"pad_touch"}, "xi_back", "pad_touch"},
            {"xi_thumblx", {"pad_sticklx"}, "xi_thumbl_move", "pad_thumbl_move"},
            {"xi_thumbly", {"pad_stickly"}, "xi_thumbl_move", "pad_thumbl_move"},
            {"xi_thumbrx", {"pad_stickrx"}, "xi_thumbr_move", "pad_thumbr_move"},
            {"xi_thumbry", {"pad_stickry"}, "xi_thumbr_move", "pad_thumbr_move"},
        };

        /** @brief A keyboard key whose letter follows the player's keyboard layout, and its engine key id. */
        struct LayoutKey
        {
            /// The engine's key name.
            std::string_view name;
            /// The engine's key id.
            int id;
        };

        /// The game localizes these keycaps as @ui_control_uc_<id>, from the active Windows keyboard layout.
        constexpr LayoutKey LAYOUT_KEYS[] = {
            {"minus", 11},      {"equals", 12},   {"q", 15},         {"w", 16}, {"e", 17},     {"r", 18},
            {"t", 19},          {"y", 20},        {"u", 21},         {"i", 22}, {"o", 23},     {"p", 24},
            {"lbracket", 25},   {"rbracket", 26}, {"a", 29},         {"s", 30}, {"d", 31},     {"f", 32},
            {"g", 33},          {"h", 34},        {"j", 35},         {"k", 36}, {"l", 37},     {"semicolon", 38},
            {"apostrophe", 39}, {"tilde", 40},    {"backslash", 42}, {"z", 43}, {"x", 44},     {"c", 45},
            {"v", 46},          {"b", 47},        {"n", 48},         {"m", 49}, {"comma", 50}, {"period", 51},
            {"slash", 52},      {"oem_102", 109},
        };

        // Every key not listed below shows on the small keycap.
        /// Keys drawn on the widest keycap.
        constexpr std::string_view WIDE_KEYS[] = {
            "backspace", "enter",   "lctrl",      "rctrl",    "lshift", "rshift", "lalt",   "ralt",   "space",
            "capslock",  "numlock", "scrolllock", "np_enter", "print",  "pause",  "home",   "pgup",   "end",
            "pgdn",      "insert",  "delete",     "mouse4",   "mouse5", "mouse6", "mouse7", "mouse8",
        };
        /// Keys drawn on the middle keycap.
        constexpr std::string_view MEDIUM_KEYS[] = {
            "escape", "tab",         "up",          "down",   "left",      "right",     "lwin", "rwin",
            "apps",   "np_multiply", "np_subtract", "np_add", "np_divide", "np_period", "np_0", "np_1",
            "np_2",   "np_3",        "np_4",        "np_5",   "np_6",      "np_7",      "np_8", "np_9",
        };

        // The words of the movie's hint row. They are localization keys (text_ui_menus__oblivion_lockpicking.xml) that
        // the movie's text fields show in the player's language.
        constexpr std::string_view ROW_MOVE = "@ui_oblivion_lockpicking_hint_move";
        constexpr std::string_view ROW_PUSH = "@ui_oblivion_lockpicking_hint_push";
        constexpr std::string_view ROW_SET = "@ui_oblivion_lockpicking_hint_set";
        constexpr std::string_view ROW_UP = "@ui_oblivion_lockpicking_hint_up";
        constexpr std::string_view ROW_OR = "@ui_oblivion_lockpicking_hint_or";

        /** @brief The controller control @p name belongs to (as its Xbox name or a twin), or nullptr. */
        [[nodiscard]] const PadKey *pad_key(std::string_view name) noexcept
        {
            if (name.empty())
            {
                return nullptr;
            }
            for (const PadKey &key : PAD_KEYS)
            {
                if (key.name == name || std::find(key.twins.begin(), key.twins.end(), name) != key.twins.end())
                {
                    return &key;
                }
            }
            return nullptr;
        }

        /** @brief @p name's position among @p key's names: 0 for the Xbox name, 1 and up for the twins, or -1. */
        [[nodiscard]] int source_of(const PadKey &key, std::string_view name) noexcept
        {
            if (key.name == name)
            {
                return 0;
            }
            for (std::size_t i = 0; i < key.twins.size(); ++i)
            {
                if (!key.twins[i].empty() && key.twins[i] == name)
                {
                    return static_cast<int>(i) + 1;
                }
            }
            return -1;
        }

        template <std::size_t N>
        [[nodiscard]] bool contains(const std::string_view (&names)[N], std::string_view name) noexcept
        {
            return std::find(std::begin(names), std::end(names), name) != std::end(names);
        }

        [[nodiscard]] bool is_pad_name(std::string_view name) noexcept
        {
            return name.starts_with("xi_") || name.starts_with("pad_");
        }

        /** @brief The icon a key shows in the movie row: its button frame, or a keycap with its localized letter. */
        [[nodiscard]] std::string key_icon(std::string_view name, HintDevice device)
        {
            if (const PadKey *key = pad_key(name))
            {
                return "#" + std::string(device == HintDevice::PlayStation ? key->playstation_icon : key->xbox_icon);
            }
            if (name == "mouse1" || name == "mouse2" || name == "mouse3")
            {
                return "#" + std::string(name);
            }
            for (const LayoutKey &key : LAYOUT_KEYS)
            {
                if (key.name == name)
                {
                    return "#key:@ui_control_uc_" + std::to_string(key.id);
                }
            }
            const std::string_view keycap = contains(WIDE_KEYS, name)     ? "enter"
                                            : contains(MEDIUM_KEYS, name) ? "escape"
                                                                          : "key";
            return "#" + std::string(keycap) + ":@ui_key_" + std::string(name);
        }

        /** @brief One way to do an action: a key, or a move of the mouse or the stick. */
        struct Way
        {
            /** @brief What the player uses. */
            enum class Kind : std::uint8_t
            {
                Key,
                /// A move-left key and a move-right key, shown side by side. Either one can be absent.
                KeyPair,
                MouseMove,
                MouseUp,
                StickMove,
                StickUp,
            };
            Kind kind{Kind::Key};
            /// The key of a Key, or the move-left key of a KeyPair.
            std::string_view key;
            /// The move-right key of a KeyPair.
            std::string_view right_key;
        };

        /** @brief The ways to do each action, for the controls of the hinted device only. */
        struct HintPlan
        {
            /// The ways to move the pick.
            std::vector<Way> move;
            /// The ways to push a tumbler.
            std::vector<Way> push;
            /// The ways to set a tumbler.
            std::vector<Way> set;
        };

        void add_keys(std::vector<Way> &ways, const KeyList &keys, bool pad)
        {
            for (std::size_t i = 0; i < keys.count; ++i)
            {
                if (is_pad_name(keys.name(i)) == pad)
                {
                    ways.push_back(
                        Way{
                            .kind = Way::Kind::Key,
                            .key = keys.name(i),
                        }
                    );
                }
            }
        }

        /** @brief The move keys of the hinted device, a left key and a right key per way, paired in list order. */
        void add_move_keys(std::vector<Way> &ways, const Bindings &bindings, bool pad)
        {
            std::vector<std::string_view> left;
            std::vector<std::string_view> right;
            for (std::size_t i = 0; i < bindings.move_left_keys.count; ++i)
            {
                if (is_pad_name(bindings.move_left_keys.name(i)) == pad)
                {
                    left.push_back(bindings.move_left_keys.name(i));
                }
            }
            for (std::size_t i = 0; i < bindings.move_right_keys.count; ++i)
            {
                if (is_pad_name(bindings.move_right_keys.name(i)) == pad)
                {
                    right.push_back(bindings.move_right_keys.name(i));
                }
            }
            for (std::size_t i = 0; i < std::max(left.size(), right.size()); ++i)
            {
                ways.push_back(
                    Way{
                        .kind = Way::Kind::KeyPair,
                        .key = i < left.size() ? left[i] : std::string_view{},
                        .right_key = i < right.size() ? right[i] : std::string_view{},
                    }
                );
            }
        }

        [[nodiscard]] HintPlan plan_hints(const Bindings &bindings, HintDevice device)
        {
            const bool pad = device != HintDevice::KeyboardMouse;
            const bool stick = pad && bindings.pad_stick != PadStick::None;
            HintPlan plan;
            if (!pad)
            {
                add_move_keys(plan.move, bindings, pad);
                plan.move.push_back(
                    Way{
                        .kind = Way::Kind::MouseMove,
                    }
                );
            }
            else
            {
                if (stick)
                {
                    plan.move.push_back(
                        Way{
                            .kind = Way::Kind::StickMove,
                        }
                    );
                }
                add_move_keys(plan.move, bindings, pad);
            }
            if (!pad && bindings.push_with_mouse_up)
            {
                plan.push.push_back(
                    Way{
                        .kind = Way::Kind::MouseUp,
                    }
                );
            }
            if (stick)
            {
                plan.push.push_back(
                    Way{
                        .kind = Way::Kind::StickUp,
                    }
                );
            }
            add_keys(plan.push, bindings.push_keys, pad);
            add_keys(plan.set, bindings.set_keys, pad);
            return plan;
        }

        /** @brief The ways joined with @p conjunction ("or"), each one once. */
        template <class Render>
        [[nodiscard]] std::string join_ways(const std::vector<Way> &ways, std::string_view conjunction, Render render)
        {
            std::vector<std::string> parts;
            for (const Way &way : ways)
            {
                std::string part = render(way);
                if (!part.empty() && std::find(parts.begin(), parts.end(), part) == parts.end())
                {
                    parts.push_back(std::move(part));
                }
            }
            std::string joined;
            for (const std::string &part : parts)
            {
                if (!joined.empty())
                {
                    joined += " ";
                    joined += conjunction;
                    joined += " ";
                }
                joined += part;
            }
            return joined;
        }

        /** @brief The stick's axis name, for its icon. */
        [[nodiscard]] std::string_view stick_axis(const Bindings &bindings) noexcept
        {
            return stick_axes(bindings.pad_stick).x;
        }
    } // namespace

    KeyList parse_key_list(std::string_view text) noexcept
    {
        KeyList list;
        while (!text.empty())
        {
            const std::size_t comma = text.find(',');
            const std::string_view item = strings::trim(text.substr(0, comma));
            text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
            if (item.empty())
            {
                continue;
            }
            if (item.size() >= KEY_NAME_CAPACITY || list.count == KEY_LIST_CAPACITY)
            {
                ++list.dropped;
                continue;
            }
            std::array<char, KEY_NAME_CAPACITY> &name = list.names[list.count++];
            std::transform(item.begin(), item.end(), name.begin(), strings::lower);
            name[item.size()] = '\0';
        }
        return list;
    }

    bool same_key(std::string_view key, std::string_view name) noexcept
    {
        if (key == name)
        {
            return true;
        }
        const PadKey *pad = pad_key(key);
        return pad != nullptr && source_of(*pad, name) >= 0;
    }

    KeyMatch find_key(const KeyList &keys, std::string_view name) noexcept
    {
        for (std::size_t i = 0; i < keys.count; ++i)
        {
            const std::string_view key = keys.name(i);
            if (key == name)
            {
                return KeyMatch{
                    .entry = static_cast<int>(i),
                    .source = 0,
                };
            }
            if (const PadKey *pad = pad_key(key))
            {
                // Source 0 is the exact name. The control's names follow it, its Xbox name first.
                const int source = source_of(*pad, name);
                if (source >= 0)
                {
                    return KeyMatch{
                        .entry = static_cast<int>(i),
                        .source = source + 1,
                    };
                }
            }
        }
        return KeyMatch{};
    }

    std::optional<PadStick> parse_pad_stick(std::string_view text) noexcept
    {
        text = strings::trim(text);
        for (const PadStick stick : {PadStick::Left, PadStick::Right, PadStick::None})
        {
            if (strings::equals_ignoring_case(text, pad_stick_name(stick)))
            {
                return stick;
            }
        }
        return std::nullopt;
    }

    std::string_view pad_stick_name(PadStick stick) noexcept
    {
        switch (stick)
        {
        case PadStick::Left:
            return "Left";
        case PadStick::Right:
            return "Right";
        case PadStick::None:
            return "None";
        }
        return "Left";
    }

    StickAxes stick_axes(PadStick stick) noexcept
    {
        switch (stick)
        {
        case PadStick::Left:
            return StickAxes{
                .x = "xi_thumblx",
                .y = "xi_thumbly",
            };
        case PadStick::Right:
            return StickAxes{
                .x = "xi_thumbrx",
                .y = "xi_thumbry",
            };
        case PadStick::None:
            break;
        }
        return StickAxes{};
    }

    bool operator==(const KeyList &lhs, const KeyList &rhs) noexcept
    {
        if (lhs.count != rhs.count)
        {
            return false;
        }
        for (std::size_t i = 0; i < lhs.count; ++i)
        {
            if (lhs.name(i) != rhs.name(i))
            {
                return false;
            }
        }
        return true;
    }

    bool operator==(const Bindings &lhs, const Bindings &rhs) noexcept
    {
        return lhs.move_left_keys == rhs.move_left_keys && lhs.move_right_keys == rhs.move_right_keys &&
               lhs.set_keys == rhs.set_keys && lhs.push_keys == rhs.push_keys && lhs.pad_stick == rhs.pad_stick &&
               lhs.push_with_mouse_up == rhs.push_with_mouse_up;
    }

    HintDevice hint_device_from_code(int code) noexcept
    {
        switch (code)
        {
        case 2:
            return HintDevice::Xbox;
        case 16:
            return HintDevice::PlayStation;
        default:
            return HintDevice::KeyboardMouse;
        }
    }

    std::string control_hint_row(const Bindings &bindings, HintDevice device)
    {
        const auto render = [&](const Way &way) -> std::string
        {
            switch (way.kind)
            {
            case Way::Kind::MouseMove:
                return "#maxis_x";
            case Way::Kind::MouseUp:
                return "#maxis_y " + std::string(ROW_UP);
            case Way::Kind::StickMove:
                return key_icon(stick_axis(bindings), device);
            case Way::Kind::StickUp:
                return key_icon(stick_axis(bindings), device) + " " + std::string(ROW_UP);
            case Way::Kind::KeyPair:
            {
                const std::string left = way.key.empty() ? std::string{} : key_icon(way.key, device);
                const std::string right = way.right_key.empty() ? std::string{} : key_icon(way.right_key, device);
                return left.empty() || right.empty() ? left + right : left + " " + right;
            }
            case Way::Kind::Key:
                break;
            }
            return key_icon(way.key, device);
        };
        const HintPlan plan = plan_hints(bindings, device);
        std::string row;
        const auto section = [&](const std::vector<Way> &ways, std::string_view action) -> void
        {
            const std::string icons = join_ways(ways, ROW_OR, render);
            if (icons.empty())
            {
                return;
            }
            row += row.empty() ? "" : "\t";
            row += action;
            row += " ";
            row += icons;
        };
        section(plan.move, ROW_MOVE);
        section(plan.push, ROW_PUSH);
        section(plan.set, ROW_SET);
        return row;
    }
} // namespace OblivionLockpicking::controls
