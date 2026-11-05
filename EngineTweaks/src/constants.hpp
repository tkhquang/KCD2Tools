/**
 * @file constants.hpp
 * @brief File names, RTTI names and the game layouts the fixes read.
 *
 * A signature rung (anchors.cpp) spells out every game layout here, so a build that moves one fails its anchor and
 * leaves the fix off. The std::map node and try_emplace result layouts are MSVC's STL ABI, which no game build moves.
 */
#ifndef ENGINETWEAKS_CONSTANTS_HPP
#define ENGINETWEAKS_CONSTANTS_HPP

#include "version.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace EngineTweaks::constants
{
    inline constexpr const char *MOD_NAME = EngineTweaks::version::MOD_NAME;
    inline constexpr const char *INI_FILE_EXTENSION = ".ini";

    /** @brief The INI file name: the mod name plus the extension. */
    [[nodiscard]] inline std::string get_config_filename()
    {
        return std::string{MOD_NAME} + INI_FILE_EXTENSION;
    }

    inline constexpr const char *LOG_FILE_NAME = "KCD2_EngineTweaks.log";
    inline constexpr const char *INSTANCE_MUTEX_PREFIX = "KCD2_EngineTweaks_";
    inline constexpr const char *DEFAULT_LOG_LEVEL = "INFO";
    /// Async log queue slots (a power of two, about 560 bytes each), so a Trace burst of a few thousand lines fits.
    inline constexpr std::size_t LOG_QUEUE_CAPACITY = 32768;
    inline constexpr const char *GAME_MODULE = "WHGame.dll";
    /// An ASI can attach before WHGame.dll maps, so init polls for it this often, for at most 3 seconds.
    inline constexpr std::chrono::milliseconds GAME_MODULE_POLL_INTERVAL{10};
    inline constexpr int GAME_MODULE_POLL_ATTEMPTS = 300;
    /// How often the INI watcher checks the file for a saved change.
    inline constexpr std::chrono::milliseconds INI_RELOAD_INTERVAL{250};
    inline constexpr std::chrono::milliseconds HOOK_RETIREMENT_DEADLINE{250};

    /// RTTI type name of C_InventorySoul, whose vtable slot corroborates a signature.
    inline constexpr const char *RTTI_INVENTORY_SOUL = ".?AVC_InventorySoul@rpgmodule@wh@@";
    /// RTTI type name of C_RPGUtils, whose vtable slot corroborates a signature.
    inline constexpr const char *RTTI_RPG_UTILS = ".?AVC_RPGUtils@rpgmodule@wh@@";

    /// C_InventorySoul's refresh of the soul's inventory-wide item flags.
    inline constexpr std::size_t INVENTORY_SOUL_REFRESH_SLOT = 38;
    /// C_RPGUtils's cached skip-time prediction.
    inline constexpr std::size_t RPG_UTILS_SLEEP_PREDICTION_SLOT = 51;

    /// The full-screen UI mode switch's argument: a flag byte (on/off) and, at this offset, the cvar helper.
    inline constexpr std::ptrdiff_t FULL_UI_MODE_HELPER_OFFSET = 0x08;
    /// The helper's ICVar of e_ViewDistRatio.
    inline constexpr std::ptrdiff_t FULL_UI_HELPER_VIEW_DIST_CVAR_OFFSET = 0x60;
    /// ICVar::GetFVal and ICVar::Set(float).
    inline constexpr std::size_t CVAR_GET_FLOAT_SLOT = 4;
    inline constexpr std::size_t CVAR_SET_FLOAT_SLOT = 8;

    /// The link map: an MSVC std::map, so its head node and size, then each node's links and key fields.
    inline constexpr std::ptrdiff_t MAP_HEAD_OFFSET = 0x00;
    inline constexpr std::ptrdiff_t MAP_SIZE_OFFSET = 0x08;
    inline constexpr std::ptrdiff_t NODE_LEFT_OFFSET = 0x00;
    inline constexpr std::ptrdiff_t NODE_PARENT_OFFSET = 0x08;
    inline constexpr std::ptrdiff_t NODE_RIGHT_OFFSET = 0x10;
    inline constexpr std::ptrdiff_t NODE_IS_NIL_OFFSET = 0x19;
    inline constexpr std::ptrdiff_t NODE_KEY_FROM_OFFSET = 0x20;
    inline constexpr std::ptrdiff_t NODE_KEY_TO_OFFSET = 0x28;
    /// The try_emplace result: the node, then the inserted flag.
    inline constexpr std::ptrdiff_t INSERT_RESULT_NODE_OFFSET = 0x00;
    inline constexpr std::ptrdiff_t INSERT_RESULT_INSERTED_OFFSET = 0x08;

    /**
     * @brief The perk container: begin/end of its extra definition list and owned perk list (pointer vectors), its
     *        progression block (an unlock pass needs one) and its flag byte.
     * @details Flag bit 0 suspends the auto-unlock pass.
     */
    inline constexpr std::ptrdiff_t PERKS_EXTRA_DEFS_BEGIN_OFFSET = 0x10;
    inline constexpr std::ptrdiff_t PERKS_EXTRA_DEFS_END_OFFSET = 0x18;
    inline constexpr std::ptrdiff_t PERKS_OWNED_BEGIN_OFFSET = 0x40;
    inline constexpr std::ptrdiff_t PERKS_OWNED_END_OFFSET = 0x48;
    inline constexpr std::ptrdiff_t PERKS_PROGRESSION_OFFSET = 0x58;
    inline constexpr std::ptrdiff_t PERKS_FLAGS_OFFSET = 0x60;
    inline constexpr std::uint8_t PERKS_SUSPEND_AUTO_UNLOCK = 0x01;

    /// The RPG parameter block: the game time ratio and the hours one prediction table covers.
    inline constexpr std::ptrdiff_t RPG_PARAMS_TIME_RATIO_OFFSET = 0x3A8;
    inline constexpr std::ptrdiff_t RPG_PARAMS_PREDICTION_HOURS_OFFSET = 0x59C;
    /// C_RPGUtils's prediction cache stamp, where 0 makes the next request rebuild the table.
    inline constexpr std::ptrdiff_t RPG_UTILS_CACHE_STAMP_OFFSET = 0x10;
    /// C_RPGUtils's cached prediction table: one 16-byte result per hour.
    inline constexpr std::ptrdiff_t RPG_UTILS_CACHE_TABLE_OFFSET = 0x18;
    /// C_SoulSimulator's copy of the simulated soul, which each hour step returns.
    inline constexpr std::ptrdiff_t SOUL_SIMULATOR_COPY_OFFSET = 0x10;
} // namespace EngineTweaks::constants

#endif // ENGINETWEAKS_CONSTANTS_HPP
