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

struct Config
{
    std::vector<int> toggle_keys;
    std::vector<int> fpv_keys;
    std::vector<int> tpv_keys;
    std::string log_level_str = Constants::DEFAULT_LOG_LEVEL;
} g_config;

inline Config loadConfig()
{
    DMKConfig::registerKeyList("Settings", "ToggleKey", "Toggle View Keys", g_config.toggle_keys, "0x72");
    DMKConfig::registerKeyList("Settings", "FPVKey", "Force First-Person View Keys", g_config.fpv_keys, "");
    DMKConfig::registerKeyList("Settings", "TPVKey", "Force Third-Person View Keys", g_config.tpv_keys, "");
    DMKConfig::registerString("Settings", "LogLevel", "Log Level (TRACE, DEBUG, INFO, WARNING, ERROR)", g_config.log_level_str, "INFO");

    std::string ini_path = DMKFilesystem::getRuntimeDirectory() + "\\" + Constants::getConfigFilename();
    DMKConfig::load(ini_path);

    return g_config;
}

#endif // CONFIG_H
