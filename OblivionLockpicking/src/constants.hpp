/**
 * @file constants.hpp
 * @brief File names, RTTI names and the engine layout the mod reads and writes.
 *
 * Every offset and slot here belongs to WHGame.dll and is the same on the Steam, GOG and Game Pass builds that the
 * anchors are checked against. game/lockpicking.hpp states which objects are RTTI-checked before a field or slot use.
 */
#ifndef OBLIVIONLP_CONSTANTS_HPP
#define OBLIVIONLP_CONSTANTS_HPP

#include "version.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace OblivionLockpicking::constants
{
    inline constexpr const char *MOD_NAME = OblivionLockpicking::version::MOD_NAME;
    inline constexpr const char *INI_FILE_EXTENSION = ".ini";

    /** @brief The INI file name, derived from the module name as every other file the mod owns. */
    [[nodiscard]] inline std::string get_config_filename()
    {
        return std::string(MOD_NAME) + INI_FILE_EXTENSION;
    }

    inline constexpr const char *LOG_FILE_NAME = "KCD2_OblivionLockpicking.log";
    inline constexpr const char *INSTANCE_MUTEX_PREFIX = "KCD2_OblivionLockpicking_";
    inline constexpr const char *DEFAULT_LOG_LEVEL = "INFO";
    inline constexpr const char *GAME_MODULE = "WHGame.dll";
    inline constexpr std::chrono::milliseconds HOOK_RETIREMENT_DEADLINE{250};

    // RTTI type names (MSVC mangled).
    inline constexpr const char *RTTI_LOCKPICKING = ".?AVC_LockPicking@playermodule@wh@@";
    inline constexpr const char *RTTI_PLAYER = ".?AVC_Player@entitymodule@wh@@";
    inline constexpr const char *RTTI_SOUL = ".?AVC_Soul@rpgmodule@wh@@";
    inline constexpr const char *RTTI_FLASH_UI_ELEMENT = ".?AVCFlashUIElement@@";
    inline constexpr const char *RTTI_RPG_MINIGAMES = ".?AVC_RPGMinigames@rpgmodule@wh@@";
    inline constexpr const char *RTTI_ENTITY_SYSTEM = ".?AVCEntitySystem@@";
    inline constexpr const char *RTTI_CONSOLE = ".?AVCXConsole@@";
    inline constexpr const char *RTTI_FLASH_UI = ".?AVCFlashUI@@";
    inline constexpr const char *RTTI_FLASH_PLAYER = ".?AVCFlashPlayer@@";
    inline constexpr const char *RTTI_INPUT = ".?AVCDXInput@@";
    inline constexpr const char *RTTI_TURN_FAIL_ACTION = ".?AVC_LockPickingTurnFailAction@playermodule@wh@@";

    // C_LockPicking primary vtable slots.
    inline constexpr std::size_t LOCKPICKING_UPDATE_SLOT = 19;
    inline constexpr std::size_t LOCKPICKING_SLOT_COUNT = 47;
    /**
     * @brief C_Minigame::PlaySound(const CryString &trigger, IEntity *at).
     * @details It looks the FMOD trigger up by name and executes it on the minigame's audio proxy for that entity. It
     *          takes the string by value and releases it.
     */
    inline constexpr std::size_t MINIGAME_PLAY_SOUND_SLOT = 42;

    // SSystemGlobalEnvironment (gEnv) members.
    inline constexpr std::ptrdiff_t GENV_INPUT_OFFSET = 0x40;
    inline constexpr std::ptrdiff_t GENV_ENTITY_SYSTEM_OFFSET = 0xA0;
    inline constexpr std::ptrdiff_t GENV_CONSOLE_OFFSET = 0xA8;
    inline constexpr std::ptrdiff_t GENV_FLASH_UI_OFFSET = 0x138;
    /// CEntitySystem::GetEntity(EntityId), which returns the IEntity.
    inline constexpr std::size_t ENTITY_SYSTEM_GET_ENTITY_SLOT = 14;

    /// Slots of the CXConsole primary vtable. The last one pins the layout.
    inline constexpr std::size_t CONSOLE_SLOT_COUNT = 76;
    /// IConsole::ExecuteString(const char *command, bool silent, bool defer).
    inline constexpr std::size_t CONSOLE_EXECUTE_SLOT = 35;
    /**
     * @brief IConsole::GetSortedVars(const char **names, size_t capacity, const char *prefix).
     * @details It lists the variables and commands whose name starts with the prefix. Without a names array, it only
     *          counts them.
     */
    inline constexpr std::size_t CONSOLE_SORTED_VARS_SLOT = 39;
    /// IConsole::GetCVar(const char *name), which returns the console variable or nullptr.
    inline constexpr std::size_t CONSOLE_GET_CVAR_SLOT = 23;
    /// ICVar::GetIVal().
    inline constexpr std::size_t CVAR_GET_INT_SLOT = 2;
    /**
     * @brief The device the game's button hints show.
     * @details C_UIActionHintManager sets it from each input event: 1 keyboard and mouse, 2 Xbox controller, 16
     *          PlayStation controller.
     */
    inline constexpr const char *HINT_DEVICE_CVAR = "wh_ui_InputDevice";

    /// Slots of the CDXInput primary vtable. The last one pins the layout.
    inline constexpr std::size_t INPUT_SLOT_COUNT = 72;
    /// AddConsoleEventListener(listener, name, flag), for the listener set the engine notifies first.
    inline constexpr std::size_t INPUT_ADD_CONSOLE_LISTENER_SLOT = 3;
    /// RemoveConsoleEventListener(listener).
    inline constexpr std::size_t INPUT_REMOVE_CONSOLE_LISTENER_SLOT = 4;
    // SInputEvent fields.
    inline constexpr std::ptrdiff_t INPUT_EVENT_STATE_OFFSET = 0x04;
    inline constexpr std::ptrdiff_t INPUT_EVENT_NAME_OFFSET = 0x08;
    inline constexpr std::ptrdiff_t INPUT_EVENT_VALUE_OFFSET = 0x18;

    /// The vanilla click of the sweet spot, which the mod plays for a tumbler at the top.
    inline constexpr const char *SOUND_TUMBLER_TOP = "special_lockpicking_onplace";
    /// The game's pick break, which its failure animation plays and the mod plays at the lock in its place.
    inline constexpr const char *SOUND_PICK_BREAK = "special_lockpicking_fail";
    /// SoundFramework's console command that plays a pack sound.
    inline constexpr const char *SOUND_FRAMEWORK_PLAY = "sfw_play";
    /// The prefix of this mod's sounds in its SoundFramework pack.
    inline constexpr const char *SOUND_PACK_PREFIX = "kcd2_oblivionlockpicking_";

    // C_LockPicking fields.
    inline constexpr std::ptrdiff_t LP_OWNER_OFFSET = 0x18;
    inline constexpr std::ptrdiff_t LP_LOCK_ENTITY_OFFSET = 0x28;
    inline constexpr std::ptrdiff_t LP_STATE_OFFSET = 0x88;
    inline constexpr std::ptrdiff_t LP_TOLERANCE_OFFSET = 0x90;
    inline constexpr std::ptrdiff_t LP_DURABILITY_OFFSET = 0xAC;
    inline constexpr std::ptrdiff_t LP_DURABILITY_MAX_OFFSET = 0xB0;
    /**
     * @brief 100.0f while the rotate action (keyboard "right" superaction, pad LT) is held, 0.0f otherwise.
     * @details The value is held, because the action handler writes it only when the input changes.
     */
    inline constexpr std::ptrdiff_t LP_ROTATE_INPUT_OFFSET = 0xB4;
    /// Right stick X, which the xi_lock_cursor_x handler writes on change and Update reads every frame.
    inline constexpr std::ptrdiff_t LP_STICK_X_OFFSET = 0xDC;
    /// Right stick Y, which the xi_lock_cursor_y handler writes on change and Update reads every frame.
    inline constexpr std::ptrdiff_t LP_STICK_Y_OFFSET = 0xE0;
    inline constexpr std::ptrdiff_t LP_LOCK_DIFFICULTY_OFFSET = 0x128;
    /**
     * @brief esLockFanciness, one byte: 0 Common, 1 Fancy.
     * @details The bytes after it are other lock properties (bLockReversed, bSendMessage) and padding.
     */
    inline constexpr std::ptrdiff_t LP_LOCK_FANCINESS_OFFSET = 0x12C;
    inline constexpr std::ptrdiff_t LP_VIEW_OFFSET = 0x130;
    /**
     * @brief Mouse X delta, positive to the right.
     * @details The lock_cursor_x handler accumulates it, and UpdateCursor consumes it each frame.
     */
    inline constexpr std::ptrdiff_t LP_MOUSE_X_OFFSET = 0x138;
    /**
     * @brief Mouse Y delta, positive up.
     * @details The lock_cursor_y handler negates the raw Y and accumulates it, and UpdateCursor consumes it each frame.
     */
    inline constexpr std::ptrdiff_t LP_MOUSE_Y_OFFSET = 0x13C;
    /// Success hold, which Update counts down in state 6 before it commits the success (Finish) at zero.
    inline constexpr std::ptrdiff_t LP_SUCCESS_HOLD_OFFSET = 0x184;
    /// AI hearing emitter, which UpdateLock and Update drive.
    inline constexpr std::ptrdiff_t LP_EMITTER_OFFSET = 0x190;
    inline constexpr std::ptrdiff_t LP_BROKEN_COUNT_OFFSET = 0x1A4;
    /**
     * @brief The hand animation of the failure, a C_LockPickingTurnFailAction IAction, while it plays, or null.
     * @details The failure state has no timer. It ends when this action exits.
     */
    inline constexpr std::ptrdiff_t LP_TURN_FAIL_ACTION_OFFSET = 0x168;
    /// IAction: its status.
    inline constexpr std::ptrdiff_t ACTION_STATUS_OFFSET = 0x28;
    /// The IAction status of a finished action.
    inline constexpr std::int32_t ACTION_STATUS_FINISHED = 4;
    /// The code Fail sends to the HUD messages: "Lockpicking attempt failed" (ui_hud_lp_fail).
    inline constexpr std::uint8_t HUD_MESSAGE_LOCKPICK_FAIL = 4;

    /// C_Human: its C_Soul.
    inline constexpr std::ptrdiff_t HUMAN_SOUL_OFFSET = 0x668;
    /**
     * @brief C_Soul: float GetSkillLevel(soul, skill, flag).
     * @details The lockpicking XP code calls it with (4, 0) for the level with its buffs, clamped to the maximum.
     */
    inline constexpr std::size_t SOUL_SKILL_LEVEL_SLOT = 102;
    /// The Thievery skill id.
    inline constexpr unsigned SKILL_THIEVERY = 4;
    /// The highest skill level.
    inline constexpr float SKILL_MAX_LEVEL = 30.0f;

    /// C_LockPickingView: its CFlashUIElement.
    inline constexpr std::ptrdiff_t VIEW_ELEMENT_OFFSET = 0x10;
    // CFlashUIElement primary vtable slots.
    inline constexpr std::size_t ELEMENT_SET_VISIBLE_SLOT = 28;
    inline constexpr std::size_t ELEMENT_IS_VISIBLE_SLOT = 29;
    /// GetFlashPlayer(), which creates the element's player if it has none and returns it as a std::shared_ptr (sret).
    inline constexpr std::size_t ELEMENT_GET_PLAYER_SLOT = 42;
    /// CFlashUIElement: its name, a CryString.
    inline constexpr std::ptrdiff_t ELEMENT_NAME_OFFSET = 0x30;

    /// CFlashUI: the loaded elements, a DynArray of element pointers with its count in the int before the data.
    inline constexpr std::ptrdiff_t FLASH_UI_ELEMENTS_OFFSET = 0x60;
    /// CFlashUI: the SRW lock that guards the element list.
    inline constexpr std::ptrdiff_t FLASH_UI_LOCK_OFFSET = 0x78;

    /// CFlashPlayer::Invoke(const char *method, const SFlashVarValue *args, unsigned count, SFlashVarValue *result).
    inline constexpr std::size_t PLAYER_INVOKE_SLOT = 46;
    /// CFlashPlayer::SetVariable(const char *path, const SFlashVarValue &value).
    inline constexpr std::size_t PLAYER_SET_VARIABLE_SLOT = 39;
    /// Everything a movie draws.
    inline constexpr const char *MOVIE_ROOT_VISIBLE = "_root._visible";
    /**
     * @brief The game's HUD element (Libs/UI/UIElements/HUD.xml).
     * @details Its control prompts, bottom right, are notifications of g_ActionHintNotifMngr (hud.gfx), each with its
     *          action's name as id. Remove(id, E_REMOVE_HIDE = 0) takes one away at once, until the game sends its
     *          prompts again.
     */
    inline constexpr const char *HUD_ELEMENT = "hud";
    /// The HUD function that removes one control prompt.
    inline constexpr const char *HUD_REMOVE_PROMPT = "_root.g_ActionHintNotifMngr.Remove";
    /// E_REMOVE_HIDE, which removes a prompt without its fade.
    inline constexpr double HUD_REMOVE_AT_ONCE = 0.0;
    /**
     * @brief The vanilla lockpicking prompts (defaultActionHelp.xml, action map "lockpicking").
     * @details They rotate the lock and move the cursor with the mouse or the stick. The game's Help and Stop prompts
     *          stay.
     */
    inline constexpr const char *HUD_LOCK_PROMPTS[] = {
        "lock_dir_fwd",
        "lock_cursor_x",
        "xi_lock_cursor_x",
    };

    /// The mod's own Scaleform element, Libs/UI/UIElements/KCD2_OblivionLockpicking.xml in its pak.
    inline constexpr const char *LOCK_MOVIE_ELEMENT = "KCD2_OblivionLockpicking";
    /// The movie's per-frame function (ui/KCD2_OblivionLockpicking.as).
    inline constexpr const char *LOCK_MOVIE_UPDATE = "update";
    /// The movie's function that takes the control hint row.
    inline constexpr const char *LOCK_MOVIE_CONTROLS = "controls";
    /// The movie's function that starts a lock: its next update() reads the header texts again.
    inline constexpr const char *LOCK_MOVIE_BEGIN = "begin";

    /// Hearing emitter: the noise level, emitted again each frame as rate * level * dt while it is not zero.
    inline constexpr std::ptrdiff_t EMITTER_LEVEL_OFFSET = 0x08;

    // Game context to C_RPGModule to C_RPGMinigames, the chain UpdateLock follows to its noise query.
    inline constexpr std::ptrdiff_t CONTEXT_RPG_MODULE_OFFSET = 0x130;
    inline constexpr std::ptrdiff_t RPG_MODULE_MINIGAMES_OFFSET = 0x98;
    /// C_RPGMinigames: the lockpicking noise level from the cursor distance, buffs applied.
    inline constexpr std::size_t RPG_LOCKPICK_NOISE_SLOT = 29;

    /** @brief C_LockPickingModel::State, the byte at LP_STATE_OFFSET, with every value of the game's enum. */
    enum class LockState : std::uint8_t
    {
        Init = 0,
        ExactPositioning = 1,
        TransitioningIn = 2,
        /// Cursor outside the sweet spot.
        SpotSearching = 3,
        /// Cursor inside the sweet spot.
        LockRotating = 4,
        /// A turn outside the spot, which drains durability.
        DryRun = 5,
        UnlockingAnim = 6,
        UnlockingAnimEnd = 7,
        IsFailing = 8,
        TransitioningOut = 9,
        CanLeave = 10,
        Ended = 11,
    };

    /// The success hold vanilla UpdateLock arms when the lock turns fully.
    inline constexpr float SUCCESS_HOLD_SECONDS = 0.5f;
} // namespace OblivionLockpicking::constants

#endif // OBLIVIONLP_CONSTANTS_HPP
