/**
 * @file config.hpp
 * @brief KCD2_HotPak.ini: which built paks to watch and how the reload behaves.
 */
#ifndef HOTPAK_CONFIG_HPP
#define HOTPAK_CONFIG_HPP

#include <chrono>
#include <string>
#include <vector>

namespace HotPak
{
    /** @brief What the pak watcher runs on. */
    struct WatchSettings
    {
        /// Built paks to watch ([Settings] Paks, separated by ';').
        std::vector<std::wstring> paks;
        /// Poll interval of the watcher.
        std::chrono::milliseconds poll{0};
    };

    /** @brief Registers every INI item with DMK::config. Call before DMK::config::load(). */
    void register_config_items();

    /** @brief The watcher settings as last loaded. */
    [[nodiscard]] WatchSettings watch_settings();

    /** @brief How long manager passes look for proxies to reset after a reload, as last loaded. */
    [[nodiscard]] std::chrono::milliseconds reset_window() noexcept;

} // namespace HotPak

#endif // HOTPAK_CONFIG_HPP
