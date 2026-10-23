/**
 * @file kcse_plugin.cpp
 * @brief The two exports that make the production DLL a Kingdom Come Script Extender (KCSE) plugin.
 *
 * KCSE loads every DLL in `Mods/<mod>/KCSE/Plugins/` with LoadLibrary and reads its `KCSEPlugin_Version` data export.
 * It keeps a DLL whose data passes its checks, and later calls `KCSEPlugin_Load` on its own loader thread. The mod
 * starts from DllMain exactly as the ASI does (dllmain.cpp), so the load call only accepts. The single-instance mutex
 * in DllMain lets only one of the ASI and the plugin run when a user installs both.
 *
 * The data layout is KCSE's published plugin ABI (data version 1), declared here from its documented fields.
 */

#include "version.hpp"

#include <cstdint>

namespace OblivionLockpicking::kcse
{
    /** @brief KCSE plugin version data, data version 1. */
    struct PluginVersionData
    {
        /// The layout version of this data.
        std::uint32_t data_version;
        /// The plugin's own version, packed as major << 16 | minor << 8 | patch.
        std::uint32_t plugin_version;
        /// The plugin name.
        char name[256];
        /// The plugin author.
        char author[256];
        /// Packed game versions the plugin supports. All zero accepts any.
        std::uint32_t compatible_game_versions[16];
        /// The lowest KCSE version the plugin needs, 0 for any.
        std::uint32_t kcse_version_required;
        /// Bit 0: the plugin finds the game code by signature, so the game version gate does not apply.
        std::uint32_t version_independence;
    };
    static_assert(sizeof(PluginVersionData) == 592);

    inline constexpr std::uint32_t DATA_VERSION = 1;
    inline constexpr std::uint32_t VERSION_INDEPENDENT_SIGNATURES = 1;
} // namespace OblivionLockpicking::kcse

extern "C" __declspec(dllexport) OblivionLockpicking::kcse::PluginVersionData KCSEPlugin_Version = {
    .data_version = OblivionLockpicking::kcse::DATA_VERSION,
    .plugin_version = (VERSION_MAJOR << 16) | (VERSION_MINOR << 8) | VERSION_PATCH,
    .name = "KCD2_OblivionLockpicking",
    .author = "tkhquang",
    .compatible_game_versions = {},
    .kcse_version_required = 0,
    .version_independence = OblivionLockpicking::kcse::VERSION_INDEPENDENT_SIGNATURES,
};

/**
 * @brief Called by KCSE once every plugin is registered.
 * @return Always true, because the mod already starts from DllMain.
 * @note The KCSE root interface goes unused, because the mod needs none of its services.
 */
extern "C" __declspec(dllexport) bool KCSEPlugin_Load(const void *) noexcept
{
    return true;
}
