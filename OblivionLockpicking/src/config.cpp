/**
 * @file config.cpp
 * @brief Configuration registration through DMK::config.
 */

#include "config.hpp"
#include "constants.hpp"
#include "strings.hpp"

#include <DetourModKit.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace OblivionLockpicking
{
    namespace
    {
        // Oblivion Remastered's layout. A and D move, W pushes and Space sets. A controller's left stick moves and
        // pushes, and A (Cross) sets. The mouse moves and pushes as in the original Oblivion, and its left click sets.
        constexpr std::string_view DEFAULT_MOVE_LEFT_KEYS = "a";
        constexpr std::string_view DEFAULT_MOVE_RIGHT_KEYS = "d";
        constexpr std::string_view DEFAULT_SET_KEYS = "space, mouse1, xi_a";
        constexpr std::string_view DEFAULT_PUSH_KEYS = "w";
        constexpr controls::PadStick DEFAULT_PAD_STICK = controls::PadStick::Left;
        constexpr KeepOnFail DEFAULT_KEEP_ON_FAIL = KeepOnFail::Thievery;

        /// Guards s_bindings, which the INI watcher thread stores and the caches copy, because it is not atomic.
        std::mutex s_bindings_mutex;
        controls::Bindings s_bindings{
            .move_left_keys = controls::parse_key_list(DEFAULT_MOVE_LEFT_KEYS),
            .move_right_keys = controls::parse_key_list(DEFAULT_MOVE_RIGHT_KEYS),
            .set_keys = controls::parse_key_list(DEFAULT_SET_KEYS),
            .push_keys = controls::parse_key_list(DEFAULT_PUSH_KEYS),
            .pad_stick = DEFAULT_PAD_STICK,
        };
        /**
         * @brief Counts the stores into s_bindings, so a SettingsCache copies them only after a change.
         * @details Each store bumps it under s_bindings_mutex. A cache reads it once without the mutex as a hint, and
         *          again under the mutex for the generation it copied.
         */
        std::atomic<std::uint32_t> s_bindings_generation{1};

        // Atomics, so the INI watcher thread can store while the game thread reads a snapshot. Each one holds its
        // key's default, which registration reads.
        std::atomic<bool> s_enabled{true};
        std::atomic<std::uint32_t> s_keep_set{static_cast<std::uint32_t>(DEFAULT_KEEP_ON_FAIL)};
        std::atomic<float> s_fail_lockout{0.8f};
        std::atomic<bool> s_tumbler_sounds{true};
        std::atomic<bool> s_noise_enabled{true};
        std::atomic<float> s_noise_distance{1.0f};
        std::atomic<bool> s_set_pushes_resting{false};
        std::atomic<bool> s_push_with_mouse_up{true};
        std::atomic<float> s_push_flick{0.25f};
        std::atomic<float> s_mouse_pick_speed{1.0f};
        std::atomic<float> s_pad_pick_speed{6.0f};
        std::atomic<float> s_time_scale{1.0f};
        std::atomic<float> s_tolerance_min{0.05f};
        std::atomic<float> s_tolerance_max{0.45f};
        std::atomic<float> s_speed_no_skill{1.0f};
        std::atomic<float> s_speed_full_skill{0.1f};
        std::atomic<bool> s_one_at_a_time{false};
        std::atomic<float> s_set_grace{0.017f};
        std::atomic<bool> s_hide_vanilla_view{true};
        std::atomic<float> s_lock_scale{1.0f};
        std::atomic<bool> s_show_header{true};
        std::atomic<bool> s_show_footer{true};

        template <class T> [[nodiscard]] T load(const std::atomic<T> &value) noexcept
        {
            return value.load(std::memory_order_relaxed);
        }

        /** @brief Applies @p store to s_bindings under its mutex, and publishes the change to the caches. */
        template <class Store> void store_bindings(Store store)
        {
            const std::lock_guard lock(s_bindings_mutex);
            store(s_bindings);
            s_bindings_generation.fetch_add(1, std::memory_order_relaxed);
        }

        /** @brief Binds the key list @p key to the s_bindings list @p list, with a warning for each dropped name. */
        void bind_key_list(
            const DMK::config::SectionBinder &section,
            std::string_view key,
            std::string_view default_value,
            controls::KeyList controls::Bindings::*list
        )
        {
            section.bind_string(
                key,
                key,
                [key, list](std::string_view value) -> void
                {
                    const controls::KeyList parsed = controls::parse_key_list(value);
                    if (parsed.dropped != 0)
                    {
                        (void)DMK::log().try_log(
                            DMK::LogLevel::Warning,
                            "INI: {} drops {} key name(s). A name has at most {} characters, and a list holds {}.",
                            key,
                            parsed.dropped,
                            controls::KEY_NAME_CAPACITY - 1,
                            controls::KEY_LIST_CAPACITY
                        );
                    }
                    store_bindings([&](controls::Bindings &bindings) -> void { bindings.*list = parsed; });
                },
                default_value
            );
        }

        /** @brief The settings as last loaded with @p bindings as the key lists and the stick, each value clamped. */
        [[nodiscard]] Settings make_settings(const controls::Bindings &bindings) noexcept
        {
            const float tolerance_min = std::clamp(load(s_tolerance_min), 0.0f, 0.95f);
            Settings settings{
                .enabled = load(s_enabled),
                .keep_set_tumblers_on_fail = static_cast<KeepOnFail>(load(s_keep_set)),
                .fail_lockout_seconds = std::clamp(load(s_fail_lockout), 0.0f, 10.0f),
                .tumbler_sounds = load(s_tumbler_sounds),
                .noise_enabled = load(s_noise_enabled),
                .noise_distance = std::clamp(load(s_noise_distance), 0.0f, 2.0f),
                .bindings = bindings,
                .set_pushes_resting = load(s_set_pushes_resting),
                .push_flick = std::clamp(load(s_push_flick), 0.02f, 5.0f),
                .mouse_pick_speed = std::clamp(load(s_mouse_pick_speed), 0.0f, 20.0f),
                .pad_pick_speed = std::clamp(load(s_pad_pick_speed), 0.0f, 50.0f),
                .time_scale = std::clamp(load(s_time_scale), 0.1f, 10.0f),
                .tolerance_min = tolerance_min,
                .tolerance_max = std::clamp(load(s_tolerance_max), tolerance_min + 0.01f, 1.0f),
                .speed_no_skill = std::clamp(load(s_speed_no_skill), 0.05f, 1.0f),
                .speed_full_skill = std::clamp(load(s_speed_full_skill), 0.05f, 1.0f),
                .one_tumbler_at_a_time = load(s_one_at_a_time),
                .set_grace_seconds = std::clamp(load(s_set_grace), 0.0f, 0.2f),
                .hide_vanilla_view = load(s_hide_vanilla_view),
                .lock_scale = std::clamp(load(s_lock_scale), 0.25f, 4.0f),
                .show_header = load(s_show_header),
                .show_footer = load(s_show_footer),
            };
            // PushWithMouseUp is a plain INI flag, stored apart from the key lists.
            settings.bindings.push_with_mouse_up = load(s_push_with_mouse_up);
            return settings;
        }
    } // namespace

    std::string_view keep_on_fail_name(KeepOnFail keep) noexcept
    {
        switch (keep)
        {
        case KeepOnFail::None:
            return "None";
        case KeepOnFail::All:
            return "All";
        case KeepOnFail::Thievery:
            return "Thievery";
        }
        return "Thievery";
    }

    std::optional<KeepOnFail> parse_keep_on_fail(std::string_view text) noexcept
    {
        text = strings::trim(text);
        for (const KeepOnFail keep : {KeepOnFail::None, KeepOnFail::All, KeepOnFail::Thievery})
        {
            if (strings::equals_ignoring_case(text, keep_on_fail_name(keep)))
            {
                return keep;
            }
        }
        return std::nullopt;
    }

    void register_config_items()
    {
        const DMK::config::SectionBinder settings_section = DMK::config::section("Settings");
        settings_section.bind_log_level("LogLevel", constants::DEFAULT_LOG_LEVEL);
        settings_section.bind("Enabled", s_enabled);
        settings_section.bind_string(
            "KeepSetTumblersOnFail",
            "KeepSetTumblersOnFail",
            [](std::string_view value) -> void
            {
                const std::optional<KeepOnFail> parsed = parse_keep_on_fail(value);
                if (!parsed)
                {
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Warning,
                        "INI: KeepSetTumblersOnFail={} is not None, All or Thievery, so {} applies",
                        value,
                        keep_on_fail_name(DEFAULT_KEEP_ON_FAIL)
                    );
                }
                s_keep_set.store(
                    static_cast<std::uint32_t>(parsed.value_or(DEFAULT_KEEP_ON_FAIL)),
                    std::memory_order_relaxed
                );
            },
            keep_on_fail_name(DEFAULT_KEEP_ON_FAIL)
        );
        settings_section.bind("FailLockoutSeconds", s_fail_lockout);
        settings_section.bind("TumblerSounds", s_tumbler_sounds);
        settings_section.bind("NoiseEnabled", s_noise_enabled);
        settings_section.bind("NoiseDistance", s_noise_distance);

        const DMK::config::SectionBinder controls_section = DMK::config::section("Controls");
        bind_key_list(controls_section, "MoveLeftKeys", DEFAULT_MOVE_LEFT_KEYS, &controls::Bindings::move_left_keys);
        bind_key_list(controls_section, "MoveRightKeys", DEFAULT_MOVE_RIGHT_KEYS, &controls::Bindings::move_right_keys);
        bind_key_list(controls_section, "SetKeys", DEFAULT_SET_KEYS, &controls::Bindings::set_keys);
        bind_key_list(controls_section, "PushKeys", DEFAULT_PUSH_KEYS, &controls::Bindings::push_keys);
        controls_section.bind_string(
            "PadStick",
            "PadStick",
            [](std::string_view value) -> void
            {
                const std::optional<controls::PadStick> parsed = controls::parse_pad_stick(value);
                if (!parsed)
                {
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Warning,
                        "INI: PadStick={} is not Left, Right or None, so {} applies",
                        value,
                        controls::pad_stick_name(DEFAULT_PAD_STICK)
                    );
                }
                store_bindings(
                    [&](controls::Bindings &bindings) -> void
                    { bindings.pad_stick = parsed.value_or(DEFAULT_PAD_STICK); }
                );
            },
            controls::pad_stick_name(DEFAULT_PAD_STICK)
        );
        controls_section.bind("SetPushesRestingTumbler", s_set_pushes_resting);
        controls_section.bind("PushWithMouseUp", s_push_with_mouse_up);
        controls_section.bind("PushFlick", s_push_flick);
        controls_section.bind("MousePickSpeed", s_mouse_pick_speed);
        controls_section.bind("PadPickSpeed", s_pad_pick_speed);

        const DMK::config::SectionBinder difficulty_section = DMK::config::section("Difficulty");
        difficulty_section.bind("TimeScale", s_time_scale);
        difficulty_section.bind("ToleranceMin", s_tolerance_min);
        difficulty_section.bind("ToleranceMax", s_tolerance_max);
        difficulty_section.bind("SpeedNoSkill", s_speed_no_skill);
        difficulty_section.bind("SpeedFullSkill", s_speed_full_skill);
        difficulty_section.bind("OneTumblerAtATime", s_one_at_a_time);
        difficulty_section.bind("SetGraceSeconds", s_set_grace);

        const DMK::config::SectionBinder view_section = DMK::config::section("View");
        view_section.bind("HideVanillaView", s_hide_vanilla_view);
        view_section.bind("LockScale", s_lock_scale);
        view_section.bind("ShowHeader", s_show_header);
        view_section.bind("ShowFooter", s_show_footer);
    }

    const Settings &SettingsCache::refresh() noexcept
    {
        controls::Bindings bindings = m_settings.bindings;
        if (s_bindings_generation.load(std::memory_order_relaxed) != m_bindings_generation)
        {
            // A store in progress keeps the old bindings for this call. The game thread never waits for the watcher.
            const std::unique_lock lock(s_bindings_mutex, std::try_to_lock);
            if (lock.owns_lock())
            {
                bindings = s_bindings;
                m_bindings_generation = s_bindings_generation.load(std::memory_order_relaxed);
            }
        }
        m_settings = make_settings(bindings);
        return m_settings;
    }
} // namespace OblivionLockpicking
