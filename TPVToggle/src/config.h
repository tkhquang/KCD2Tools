#ifndef CONFIG_H
#define CONFIG_H

#include <vector>
#include <string>

struct Config
{
    std::vector<int> toggle_keys; // Keys that toggle between FPV and TPV
    std::vector<int> fpv_keys;    // Keys that explicitly switch to first-person view
    std::vector<int> tpv_keys;    // Keys that explicitly switch to third-person view
    std::string log_level;        // Logging level
    std::string aob_pattern;      // Pattern for memory scanning
};

Config loadConfig(const std::string &ini_path);

#endif // CONFIG_H
