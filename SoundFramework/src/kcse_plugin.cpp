/**
 * @file kcse_plugin.cpp
 * @brief The two exports that make the production DLL a Kingdom Come Script Extender (KCSE) plugin.
 *
 * KCSE loads every DLL in `Mods/<mod>/KCSE/Plugins/` with LoadLibrary, reads its `KCSEPlugin_Version` data export,
 * keeps it when the data passes its checks, and later calls `KCSEPlugin_Load` on its own loader thread. The framework
 * starts from DllMain exactly as the ASI does (dllmain.cpp), so the load call only has to accept. The single-instance
 * mutex in DllMain keeps the ASI and the plugin from both running when a user installs both.
 *
 * The data layout is KCSE's published plugin ABI (data version 1), declared here from its documented fields.
 */

#include "version.hpp"

#include <cstdint>

namespace SoundFramework::kcse
{
    /** @brief KCSE plugin version data, data version 1. */
    struct PluginVersionData
    {
        std::uint32_t data_version;
        std::uint32_t plugin_version;
        char name[256];
        char author[256];
        /// Packed game versions the plugin supports; all zero accepts any.
        std::uint32_t compatible_game_versions[16];
        std::uint32_t kcse_version_required;
        /// Bit 0: the plugin finds the game code by signature, so the game version gate does not apply.
        std::uint32_t version_independence;
    };
    static_assert(sizeof(PluginVersionData) == 592);

    inline constexpr std::uint32_t DATA_VERSION = 1;
    inline constexpr std::uint32_t VERSION_INDEPENDENT_SIGNATURES = 1;
} // namespace SoundFramework::kcse

extern "C" __declspec(dllexport) SoundFramework::kcse::PluginVersionData KCSEPlugin_Version = {
    .data_version = SoundFramework::kcse::DATA_VERSION,
    .plugin_version = (VERSION_MAJOR << 16) | (VERSION_MINOR << 8) | VERSION_PATCH,
    .name = "KCD2_SoundFramework",
    .author = "tkhquang",
    .compatible_game_versions = {},
    .kcse_version_required = 0,
    .version_independence = SoundFramework::kcse::VERSION_INDEPENDENT_SIGNATURES,
};

/**
 * @brief Called by KCSE once every plugin is registered.
 * @param kcse KCSE's root interface (unused: the framework needs none of its services).
 * @return Always true; the framework is already starting from DllMain.
 */
extern "C" __declspec(dllexport) bool KCSEPlugin_Load(const void *kcse)
{
    (void)kcse;
    return true;
}
