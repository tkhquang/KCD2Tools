/**
 * @file config.hpp
 * @brief KCD2_OblivionLockpicking.ini: the switches and tuning of the tumbler minigame.
 */
#ifndef OBLIVIONLP_CONFIG_HPP
#define OBLIVIONLP_CONFIG_HPP

#include "controls/bindings.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace OblivionLockpicking
{
    /** @brief The set tumblers a wrong set leaves up, KeepSetTumblersOnFail in the INI. */
    enum class KeepOnFail : std::uint8_t
    {
        /// Drop every set tumbler, as Oblivion's Novice rank did.
        None,
        /// Keep every set tumbler.
        All,
        /// Keep as many as the Thievery level earns, as Oblivion's Security ranks did.
        Thievery,
    };

    /** @brief The INI name of @p keep: None, All or Thievery. */
    [[nodiscard]] std::string_view keep_on_fail_name(KeepOnFail keep) noexcept;

    /** @brief The KeepOnFail that @p text names, in any case, or std::nullopt for any other text. */
    [[nodiscard]] std::optional<KeepOnFail> parse_keep_on_fail(std::string_view text) noexcept;

    /**
     * @brief The loaded settings. Every value is clamped to a usable range.
     * @details The INI defaults live in config.cpp, in the atomic each key binds to.
     */
    struct Settings
    {
        /// Master switch. Off leaves the vanilla minigame untouched.
        bool enabled{};
        /// The set tumblers a wrong set leaves up.
        KeepOnFail keep_set_tumblers_on_fail{};
        /// Seconds the controls pause after a wrong set, in place of the game's failure animation. 0 plays it.
        float fail_lockout_seconds{};
        /// True when the tumbler events sound, through the SoundFramework pack or the vanilla click at the top.
        bool tumbler_sounds{};
        /// True when a tumbler in motion makes lockpicking noise for nearby NPCs.
        bool noise_enabled{};
        /// The cursor distance fed to the game's own noise formula while a tumbler moves, 0 quietest to 2 loudest.
        float noise_distance{};

        /// The move, set and push keys, the pad stick, and whether an upward mouse move pushes.
        controls::Bindings bindings;
        /// True when a set on a tumbler at rest pushes it. False does nothing, as in Oblivion.
        bool set_pushes_resting{};
        /// Upward mouse travel, in the game's lockpicking mouse units, that counts as a push.
        float push_flick{};
        /// Tumblers per unit of the game's lockpicking mouse movement, which already carries the game's sensitivity.
        float mouse_pick_speed{};
        /// Tumblers per second at full deflection of the pad stick.
        float pad_pick_speed{};

        /// Multiplier on every tumbler duration. Above 1 is slower and easier.
        float time_scale{};
        /// The game's sweet-spot size that maps to skill 0.
        float tolerance_min{};
        /// The game's sweet-spot size that maps to skill 1.
        float tolerance_max{};
        /// Oblivion's lock speed at skill 0. 1 is a Very Hard lock at low Security.
        float speed_no_skill{};
        /// Oblivion's lock speed at skill 1. 0.1 is a Very Easy lock.
        float speed_full_skill{};
        /// True when the pick stays under its tumbler until no tumbler moves, as in Oblivion.
        bool one_tumbler_at_a_time{};
        /// Seconds after a tumbler leaves the top during which a set still counts. 0 is Oblivion's strict rule.
        float set_grace_seconds{};

        /// True when the game's lockpicking lock hides while the tumbler lock is up.
        bool hide_vanilla_view{};
        /// Size of the tumbler lock, 1 for its 1080p layout.
        float lock_scale{};
        /// True when the bar above the lock (tumblers set, difficulty) shows.
        bool show_header{};
        /// True when the control hints below the lock show.
        bool show_footer{};
    };

    /** @brief Registers every INI item with DMK::config. Call before DMK::config::load(). */
    void register_config_items();

    /** @brief One thread's copy of the settings, refreshed without a wait. */
    class SettingsCache
    {
    public:
        /**
         * @brief Refreshes the copy to the settings as last loaded and returns it.
         * @details The key bindings follow only when the INI watcher does not hold them at that moment. Otherwise the
         *          copy keeps its previous bindings, and a later call takes the new ones.
         * @note Callback-safe. Use each cache from one thread only.
         */
        [[nodiscard]] const Settings &refresh() noexcept;

    private:
        Settings m_settings{};
        /// The bindings generation the copy holds. 0 is never published, so the first call takes the bindings.
        std::uint32_t m_bindings_generation{0};
    };
} // namespace OblivionLockpicking

#endif // OBLIVIONLP_CONFIG_HPP
