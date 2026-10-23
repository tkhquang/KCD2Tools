/**
 * @file controls/bindings.hpp
 * @brief The lock's key bindings: lists of the game's key names from the INI, matched against input events, and the
 *        control hints written from them for the player's controller.
 *
 * Key names are the engine's own: "mouse1" to "mouse8", the keyboard keys as the game names them ("d", "space"), and
 * the Xbox controller's "xi_" names. Each Xbox name also matches its PlayStation twins ("xi_a" is "pad_cross"), so one
 * list serves both controllers. The "pad_" names work as well. The code is plain C++ without a game or Windows
 * dependency.
 */
#ifndef OBLIVIONLP_CONTROLS_BINDINGS_HPP
#define OBLIVIONLP_CONTROLS_BINDINGS_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace OblivionLockpicking::controls
{
    /// Longest key name kept, with the terminator.
    inline constexpr std::size_t KEY_NAME_CAPACITY = 24;
    /// Keys per list.
    inline constexpr std::size_t KEY_LIST_CAPACITY = 8;

    /** @brief A list of key names. Trivially copyable, so it travels inside the settings snapshot. */
    struct KeyList
    {
        /// The names, each terminated.
        std::array<std::array<char, KEY_NAME_CAPACITY>, KEY_LIST_CAPACITY> names{};
        /// Names in use.
        std::size_t count{0};
        /// Names the parse dropped: longer than KEY_NAME_CAPACITY - 1 characters, or past KEY_LIST_CAPACITY.
        std::size_t dropped{0};

        /** @brief The name at @p index. */
        [[nodiscard]] std::string_view name(std::size_t index) const noexcept
        {
            return std::string_view(names[index].data());
        }
    };

    /**
     * @brief Parses a comma-separated list of key names.
     * @details The parse trims and lowercases each name. It skips empty items, and counts in KeyList::dropped the names
     *          too long to keep and the items past the capacity.
     */
    [[nodiscard]] KeyList parse_key_list(std::string_view text) noexcept;

    /** @brief Where an event's key name sits in a list: the entry, and which of the entry's names it is. */
    struct KeyMatch
    {
        /// The entry, or -1 when the name is not in the list.
        int entry{-1};
        /// 0 for the entry's own name, 1 and up for its twins.
        int source{0};
    };

    /** @brief Finds @p name (an event's key name) in @p keys, through each entry's own name and its twins. */
    [[nodiscard]] KeyMatch find_key(const KeyList &keys, std::string_view name) noexcept;

    /** @brief True when @p name is @p key or one of its twins. */
    [[nodiscard]] bool same_key(std::string_view key, std::string_view name) noexcept;

    /** @brief The controller stick that moves the pick, and pushes when held up. */
    enum class PadStick : std::uint8_t
    {
        None,
        Left,
        Right,
    };

    /** @brief The PadStick that @p text names ("Left", "Right" or "None", in any case), or std::nullopt. */
    [[nodiscard]] std::optional<PadStick> parse_pad_stick(std::string_view text) noexcept;

    /** @brief The INI name of @p stick: "Left", "Right" or "None". */
    [[nodiscard]] std::string_view pad_stick_name(PadStick stick) noexcept;

    /** @brief The stick's axes as Xbox names, whose twins match too. Both are empty for None. */
    struct StickAxes
    {
        std::string_view x;
        std::string_view y;
    };

    /** @brief The axes of @p stick. */
    [[nodiscard]] StickAxes stick_axes(PadStick stick) noexcept;

    /** @brief Everything the player can bind. Trivially copyable. */
    struct Bindings
    {
        /// Keys that step the pick one tumbler left. A held key repeats the step.
        KeyList move_left_keys;
        /// Keys that step the pick one tumbler right. A held key repeats the step.
        KeyList move_right_keys;
        /// Keys that set the tumbler at the top.
        KeyList set_keys;
        /// Keys that push the tumbler under the pick.
        KeyList push_keys;
        /// The stick that moves the pick and pushes.
        PadStick pad_stick{PadStick::Left};
        /// True when an upward mouse move pushes, as in Oblivion.
        bool push_with_mouse_up{true};
    };

    /** @brief True when both lists hold the same names in the same order. KeyList::dropped does not count. */
    [[nodiscard]] bool operator==(const KeyList &lhs, const KeyList &rhs) noexcept;
    /** @brief True when every list, the stick and the mouse push match. */
    [[nodiscard]] bool operator==(const Bindings &lhs, const Bindings &rhs) noexcept;

    /** @brief The controller the hints are written for. */
    enum class HintDevice : std::uint8_t
    {
        KeyboardMouse,
        Xbox,
        PlayStation,
    };

    /**
     * @brief The device the game's own button hints show, from its wh_ui_InputDevice console variable.
     * @return Xbox for 2, PlayStation for 16, the keyboard and mouse for any other code.
     */
    [[nodiscard]] HintDevice hint_device_from_code(int code) noexcept;

    /**
     * @brief The control hints under the lock, for the controls of @p device only, as a row for the lock movie.
     * @details Tabs separate the sections. Each section is a run of words and button icons separated by spaces. An
     *          icon is "#<frame>" or "#<frame>:<label>". The frame is a frame of the game's button movie
     *          (Libs/UI/buttons.gfx, whose frames are named after keys). The label is the localization key of the
     *          letter on a keycap. A word is a localization key that the movie shows in the player's language, for
     *          example "@ui_oblivion_lockpicking_hint_move #maxis_x".
     * @throws std::bad_alloc when the row cannot grow.
     */
    [[nodiscard]] std::string control_hint_row(const Bindings &bindings, HintDevice device);
} // namespace OblivionLockpicking::controls

#endif // OBLIVIONLP_CONTROLS_BINDINGS_HPP
