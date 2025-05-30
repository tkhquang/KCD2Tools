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

    std::string ini_path = DMKFilesystem::getRuntimeDirectory() + "\\" + Constants::getConfigFilename();
    DMKConfig::load(ini_path);

    return GlobalState::g_config;
}
