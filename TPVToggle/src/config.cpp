/**
 * @file config.cpp
 * @brief Implementation of configuration loading function.
 *
 * Loads configuration settings from an INI file into the global configuration struct.
 */
#include <DetourModKit.hpp>

#include "config.hpp"
#include "global_state.hpp"

LocalConfig loadConfig()
{
    DMKConfig::registerKeyList("Settings", "ToggleKey", "Toggle View Keys", GlobalState::g_config.toggle_keys, "0x72");
    DMKConfig::registerKeyList("Settings", "FPVKey", "Force First-Person View Keys", GlobalState::g_config.fpv_keys, "");
    DMKConfig::registerKeyList("Settings", "TPVKey", "Force Third-Person View Keys", GlobalState::g_config.tpv_keys, "");
    DMKConfig::registerFloat("Settings", "tpv_offset_x", "tpv_offset_x", GlobalState::g_config.tpv_offset_x, 0.5);
    DMKConfig::registerFloat("Settings", "tpv_offset_y", "tpv_offset_y", GlobalState::g_config.tpv_offset_y, -3.0);
    DMKConfig::registerFloat("Settings", "tpv_offset_z", "tpv_offset_z", GlobalState::g_config.tpv_offset_z, 0.0);

    DMK::Config::registerBool("TPVCamera", "EnableTPV", "Enable Third-Person View", GlobalState::g_config.enableTpv, true);
    DMK::Config::registerFloat("TPVCamera", "Distance", "Camera Distance from Player", GlobalState::g_config.cameraDistance, 4.0f);
    DMK::Config::registerFloat("TPVCamera", "HeightOffset", "Camera Height Offset", GlobalState::g_config.cameraHeightOffset, 1.5f);
    DMK::Config::registerFloat("TPVCamera", "RightOffset", "Camera Right Offset", GlobalState::g_config.cameraRightOffset, 0.5f);
    DMK::Config::registerInt("TPVCamera", "ToggleKey", "TPV Toggle Hotkey (VK Code Hex)", GlobalState::g_config.toggleTpvKey, 0x72); // VK_F9

    // TPV Camera Positional Offsets
    DMKConfig::registerFloat("TPV", "OffsetBehind", "Distance camera is moved BACK from anchor (along aim dir)",
                             GlobalState::g_config.tpv_offset_behind, 2.5f); // Positive value = further behind

    DMKConfig::registerFloat("TPV", "OffsetUp", "Distance camera is moved UP from anchor (along aim's up)",
                             GlobalState::g_config.tpv_offset_up, 0.2f); // Positive value = further up

    DMKConfig::registerFloat("TPV", "OffsetRight", "Distance camera is moved RIGHT from anchor (along aim's right)",
                             GlobalState::g_config.tpv_offset_right, 0.0f); // Positive for right shoulder, negative for left

    // TPV Anchor Adjustment
    DMKConfig::registerFloat("TPV", "AnchorHeightAdjust", "Height to add to entity's root position for TPV anchor",
                             GlobalState::g_config.tpv_anchor_height_adjust, 1.6f); // Approx meters for shoulder height

    // TPV Aiming
    DMKConfig::registerFloat("TPV", "AimConvergenceDistance", "Distance (m) at which TPV aim converges with FPV aim",
                             GlobalState::g_config.tpv_aim_convergence_distance, 30.0f);

    std::string ini_path = DMKFilesystem::getRuntimeDirectory() + "\\" + Constants::getConfigFilename();
    DMKConfig::load(ini_path);

    return GlobalState::g_config;
}
