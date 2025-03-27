#ifndef CONFIG_H
#define CONFIG_H

#include <vector>
#include <string>

struct Config
{
    std::vector<int> toggle_keys; // List of virtual-key codes for toggling
    std::string log_level;        // Logging level (DEBUG, INFO, WARNING, ERROR)
    std::string aob_pattern;      // Array of Bytes pattern for memory scanning
    // Add other settings as needed
};

Config loadConfig(const std::string &ini_path);

#endif // CONFIG_H
