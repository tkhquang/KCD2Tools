/**
 * @file config.hpp
 * @brief Configuration model and registration for the TPV Toggle mod.
 *
 * @details Settings split by access pattern:
 *          - LiveSettings holds every value read on a hot path (per-frame detours,
 *            poll threads) or across threads. They are std::atomic so INI
 *            hot-reload (which runs the setters on the ConfigWatcher thread) never
 *            races the game thread. They are bound with DMK::Config::register_atomic.
 *          - Config holds init-only values applied once while the mod sets itself
 *            up, plus the key-combo lists for hold bindings, which are parsed by
 *            DMK::Config and consumed once when the InputManager hold bindings are
 *            registered. Press bindings are registered separately via
 *            DMK::Config::register_press_combo (see input registration).
 */
#ifndef CONFIG_HPP
#define CONFIG_HPP

#include <DetourModKit.hpp>

#include <atomic>
#include <string>

/**
 * @struct Config
 * @brief Init-only settings and the key lists for hold bindings.
 */
struct Config
{
    // Optional features (read once during initialization).
    bool enable_overlay_feature{true}; /**< Enable overlay detection and handling. */

    // Camera profile system (init-only portion).
    std::string profile_directory{}; /**< Directory holding saved camera profiles. */

    // Transition settings (snapshotted into TransitionManager at init and on reload).
    float transition_duration{0.3f};
    bool use_spring_physics{false};
    float spring_strength{10.0f};
    float spring_damping{0.8f};

    // Hold-binding key lists (parsed by DMK::Config; consumed once when the
    // InputManager hold bindings are registered).
    DMK::Config::KeyComboList hold_scroll_keys;
    DMK::Config::KeyComboList offset_x_inc_keys;
    DMK::Config::KeyComboList offset_x_dec_keys;
    DMK::Config::KeyComboList offset_y_inc_keys;
    DMK::Config::KeyComboList offset_y_dec_keys;
    DMK::Config::KeyComboList offset_z_inc_keys;
    DMK::Config::KeyComboList offset_z_dec_keys;
};

/** @brief Process-wide init-only configuration (defined in config.cpp). */
extern Config g_config;

namespace TPVToggle
{
    /**
     * @struct LiveSettings
     * @brief Hot-path and cross-thread settings as atomics so INI hot-reload is
     *        race-free against the game thread and the poll threads.
     */
    struct LiveSettings
    {
        std::atomic<bool> enableCameraProfiles{false};

        // Static (profiles-disabled) third-person offset, read per frame.
        std::atomic<float> tpvOffsetX{0.0f};
        std::atomic<float> tpvOffsetY{0.0f};
        std::atomic<float> tpvOffsetZ{0.0f};

        // Custom third-person FOV in degrees, read per frame by Detour_TpvCameraUpdate
        // (which writes the radian-converted value into the output pose). Hot-
        // reloadable: edit the INI and the next frame picks the new value up.
        // Sentinel <= 0 disables the override.
        std::atomic<float> tpvFovDegrees{-1.0f};

        // Camera input sensitivities and pitch limits, read per input event.
        std::atomic<float> yawSensitivity{1.0f};
        std::atomic<float> pitchSensitivity{1.0f};
        std::atomic<bool> pitchLimitsEnabled{false};
        std::atomic<float> pitchMin{-180.0f};
        std::atomic<float> pitchMax{180.0f};

        // Continuous offset-adjustment step, read by the profile poll thread.
        std::atomic<float> offsetAdjustmentStep{0.01f};

        // Delay before restoring TPV after an overlay closes, read by the monitor thread.
        std::atomic<int> overlayRestoreDelayMs{200};
    };

    /** @brief Returns the process-wide live (atomic) settings. */
    [[nodiscard]] LiveSettings &settings() noexcept;

    /**
     * @brief Registers every non-key configuration item with DMK::Config.
     * @details Registers the log level, the LiveSettings atomics, and the Config
     *          init-only values plus hold-binding key lists. Must be called before
     *          DMK::Config::load(). Press bindings are registered separately.
     */
    void register_config_items();

} // namespace TPVToggle

#endif // CONFIG_HPP
