/**
 * @file config.h
 * @brief Defines configuration structure and the loading function prototype.
 *
 * Contains the `Config` struct used to hold application settings loaded from
 * the INI configuration file (e.g., hotkeys, logging level, optional features).
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <vector>
#include <string>

#include <DetourModKit.hpp>

#include "constants.hpp"

struct LocalConfig
{
    std::vector<int> toggle_keys;
    std::vector<int> fpv_keys;
    std::vector<int> tpv_keys;

    float tpv_offset_x;
    float tpv_offset_y;
    float tpv_offset_z;

    bool enableTpv;

    float cameraDistance;
    float cameraHeightOffset;
    float cameraRightOffset;

    float tpv_offset_behind;            // How far back from the adjusted anchor
    float tpv_offset_up;                // How far up from the adjusted anchor (along camera's up)
    float tpv_offset_right;             // How far right from the adjusted anchor (along camera's right)
    float tpv_anchor_height_adjust;     // How much to raise anchor from entity root
    float tpv_aim_convergence_distance; // Distance for FPV target point projection

    int toggleTpvKey;

    std::string log_level_str = Constants::DEFAULT_LOG_LEVEL;
};

LocalConfig loadConfig();

#endif // CONFIG_H
