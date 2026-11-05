/**
 * @file constants.hpp
 * @brief File names, RTTI names, the game layouts the mod reads and the movie's interface.
 *
 * Every layout here was checked on the Steam 1.5.6, GOG 1.5 and Game Pass 1.4 builds.
 */
#ifndef HUDCLOCK_CONSTANTS_HPP
#define HUDCLOCK_CONSTANTS_HPP

#include "version.hpp"

#include <DetourModKit.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace HUDClock::constants
{
    inline constexpr const char *MOD_NAME = HUDClock::version::MOD_NAME;
    inline constexpr const char *INI_FILE_EXTENSION = ".ini";

    /** @brief The INI file name: the mod name plus the extension. */
    [[nodiscard]] inline std::string get_config_filename()
    {
        return std::string{MOD_NAME} + INI_FILE_EXTENSION;
    }

    inline constexpr const char *LOG_FILE_NAME = "KCD2_HUDClock.log";
    inline constexpr const char *INSTANCE_MUTEX_PREFIX = "KCD2_HUDClock_";
    inline constexpr const char *DEFAULT_LOG_LEVEL = "INFO";
    inline constexpr const char *GAME_MODULE = "WHGame.dll";
    inline constexpr std::chrono::milliseconds HOOK_RETIREMENT_DEADLINE{250};

    // RTTI type names of the objects the mod reads or calls.
    inline constexpr const char *RTTI_FLASH_UI = ".?AVCFlashUI@@";
    inline constexpr const char *RTTI_FLASH_UI_ELEMENT = ".?AVCFlashUIElement@@";
    inline constexpr const char *RTTI_FLASH_PLAYER = ".?AVCFlashPlayer@@";
    inline constexpr const char *RTTI_HUD_MASK = ".?AVC_UIHudMask@guimodule@wh@@";
    inline constexpr const char *RTTI_RPG_MODULE = ".?AVC_RPGModule@rpgmodule@wh@@";
    inline constexpr const char *RTTI_CALENDAR = ".?AVC_Calendar@rpgmodule@wh@@";
    inline constexpr const char *RTTI_CHARACTER_MANAGER = ".?AVCharacterManager@@";
    inline constexpr const char *RTTI_PLAYER_MODULE = ".?AVC_PlayerModule@playermodule@wh@@";
    inline constexpr const char *RTTI_PLAYER = ".?AVC_Player@entitymodule@wh@@";
    inline constexpr const char *RTTI_PHOTOMODE_MANAGER = ".?AVC_PhotomodeManager@game@wh@@";
    /// The class of each core::MINIGAMES entry, in that order. Each derives from wh::playermodule::C_Minigame.
    inline constexpr const char *RTTI_ALCHEMY = ".?AVC_Alchemy@playermodule@wh@@";
    inline constexpr const char *RTTI_BLACKSMITHING = ".?AVC_Blacksmithing@playermodule@wh@@";
    inline constexpr const char *RTTI_DICE = ".?AVC_Dice@playermodule@wh@@";
    inline constexpr const char *RTTI_READING = ".?AVC_Reading@playermodule@wh@@";
    inline constexpr const char *RTTI_LOCK_PICKING = ".?AVC_LockPicking@playermodule@wh@@";
    inline constexpr const char *RTTI_PICKPOCKETING = ".?AVC_Pickpocketing@playermodule@wh@@";
    inline constexpr const char *RTTI_HERB_GATHERING = ".?AVC_HerbGathering@playermodule@wh@@";
    inline constexpr const char *RTTI_HOLE_DIGGING = ".?AVC_HoleDigging@playermodule@wh@@";

    /// CharacterManager::Update(bool paused), the frame hook, and the slot it must be.
    inline constexpr std::size_t CHARACTER_MANAGER_UPDATE_SLOT = 18;

    /// gEnv->pFlashUI.
    inline constexpr std::ptrdiff_t GENV_FLASH_UI_OFFSET = 0x138;
    /// CFlashUI's element array (an int32 count sits right before its first entry), and the lock that guards it.
    inline constexpr std::ptrdiff_t FLASH_UI_ELEMENTS_OFFSET = 0x60;
    inline constexpr std::ptrdiff_t FLASH_UI_LOCK_OFFSET = 0x78;
    /// CFlashUIElement::SetVisible, IsVisible and GetFlashPlayer, and the element's name.
    inline constexpr std::size_t ELEMENT_SET_VISIBLE_SLOT = 28;
    inline constexpr std::size_t ELEMENT_IS_VISIBLE_SLOT = 29;
    inline constexpr std::size_t ELEMENT_GET_PLAYER_SLOT = 42;
    inline constexpr std::ptrdiff_t ELEMENT_NAME_OFFSET = 0x30;
    /// CFlashPlayer::Invoke.
    inline constexpr std::size_t PLAYER_INVOKE_SLOT = 46;

    /**
     * @brief C_UIHudMask's bitset of the HUD parts that show, one bit per wh::framework::E_HudElements value.
     * @details The mask sets each bit from the part's own rules, then shows or hides the part in hud.gfx. In play it
     *          holds 0x12FFCFF, in the pause menu 0x40 (GameLog only).
     */
    inline constexpr std::ptrdiff_t HUD_MASK_BITS_OFFSET = 0x150;

    /// Game context to C_RPGModule to C_Calendar to the world time, an int64 in milliseconds.
    inline constexpr std::ptrdiff_t CONTEXT_RPG_MODULE_OFFSET = 0x130;
    inline constexpr std::ptrdiff_t RPG_MODULE_CALENDAR_OFFSET = 0x88;
    inline constexpr std::ptrdiff_t CALENDAR_WORLD_TIME_OFFSET = 0x48;

    // Game context to C_PlayerModule to its minigame manager, whose std::multimap holds every running minigame by the
    // entity id of its actor. C_PlayerModule's vtable slot 7 is the manager getter (mov rax,[rcx+18h]), and the
    // manager's lookup walks the map at +0x20 and reads each minigame from its node at +0x28.
    inline constexpr std::ptrdiff_t CONTEXT_PLAYER_MODULE_OFFSET = 0x128;
    inline constexpr std::ptrdiff_t PLAYER_MODULE_MINIGAMES_OFFSET = 0x18;
    inline constexpr std::ptrdiff_t MINIGAMES_MAP_OFFSET = 0x20;
    /// MSVC std::multimap (a _Tree): the head node, then the element count.
    inline constexpr std::ptrdiff_t MAP_HEAD_OFFSET = 0x0;
    inline constexpr std::ptrdiff_t MAP_SIZE_OFFSET = 0x8;
    /// MSVC _Tree node: its links, its nil flag (set on the head only) and the mapped value of its pair.
    inline constexpr std::ptrdiff_t MAP_NODE_LEFT_OFFSET = 0x0;
    inline constexpr std::ptrdiff_t MAP_NODE_PARENT_OFFSET = 0x8;
    inline constexpr std::ptrdiff_t MAP_NODE_RIGHT_OFFSET = 0x10;
    inline constexpr std::ptrdiff_t MAP_NODE_IS_NIL_OFFSET = 0x19;
    inline constexpr std::ptrdiff_t MAP_NODE_MAPPED_OFFSET = 0x28;
    /// The actor that plays a C_Minigame, stored by its constructor.
    inline constexpr std::ptrdiff_t MINIGAME_OWNER_OFFSET = 0x18;
    /// More running minigames than this means the map is not what the mod expects.
    inline constexpr std::size_t MAX_MINIGAMES = 32;

    // Game context to C_PhotomodeManager, which the context init stores right after its constructor. The manager's
    // update sets the byte at +0x231 once photo mode runs (mov byte ptr [rcx+231h], 1), and the exit that turns off
    // the photomode_base and photomode_camera action maps clears it (mov [rdi+231h], sil).
    inline constexpr std::ptrdiff_t CONTEXT_PHOTOMODE_MANAGER_OFFSET = 0xB0;
    inline constexpr std::ptrdiff_t PHOTOMODE_ACTIVE_OFFSET = 0x231;

    /// The mod's element, from pack/Data/Libs/UI/UIElements/KCD2_HUDClock.xml.
    inline constexpr const char *CLOCK_ELEMENT = "KCD2_HUDClock";
    /// The movie's functions (ui/KCD2_HUDClock.as).
    inline constexpr const char *MOVIE_SET_LINE_STYLE = "setLineStyle";
    inline constexpr const char *MOVIE_SET_DIAL_STYLE = "setDialStyle";
    inline constexpr const char *MOVIE_SET_PLACE = "setPlace";
    inline constexpr const char *MOVIE_SET_TEXTS = "setTexts";
    inline constexpr const char *MOVIE_SET_HOUR = "setHour";
} // namespace HUDClock::constants

#endif // HUDCLOCK_CONSTANTS_HPP
