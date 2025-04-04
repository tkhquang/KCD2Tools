/**
 * @file config.h
 * @brief Defines configuration structure and loading function prototype.
 *
 * Contains the `Config` struct used to hold settings loaded from the INI file.
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <vector>
#include <string>

/**
 * @struct Config
 * @brief Holds settings parsed from the INI file.
 *
 * Includes keybindings, log level, and the AOB pattern string. Defaults
 * are applied during loading if settings are missing or invalid.
 */
struct Config
{
    // Key binding lists (populated from INI)
    std::vector<int> toggle_keys; /**< VK codes that toggle FPV/TPV. */
    std::vector<int> fpv_keys;    /**< VK codes that force FPV. */
    std::vector<int> tpv_keys;    /**< VK codes that force TPV. */

    // Other configurable settings
    std::string log_level;   /**< Logging level string (e.g., "INFO"). */
    std::string aob_pattern; /**< AOB pattern string for memory scan. */

    // Default constructor is sufficient.
    Config() = default;
};

/**
 * @brief Loads configuration settings from an INI file.
 * @details Parses the file, applies defaults from `Constants.h` for missing
 * or invalid values, and performs validation.
 * @param ini_filename Base filename of the config file (e.g.,
 * "KCD2_TPVToggle.ini").
 * @return Config Structure containing loaded or default settings.
 */
Config loadConfig(const std::string &ini_filename);

#endif // CONFIG_H
