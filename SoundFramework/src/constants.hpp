/**
 * @file constants.hpp
 * @brief File names, FMOD module and export names, and the WHGame.dll layouts the console commands use.
 */
#ifndef SNDFW_CONSTANTS_HPP
#define SNDFW_CONSTANTS_HPP

#include "version.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace SoundFramework::constants
{
    inline constexpr const char *MOD_NAME = SoundFramework::version::MOD_NAME;
    inline constexpr const char *INI_FILE_EXTENSION = ".ini";

    /** @brief The INI file name, derived from the module name like every other file the mod owns. */
    [[nodiscard]] inline std::string get_config_filename()
    {
        return std::string(MOD_NAME) + INI_FILE_EXTENSION;
    }

    inline constexpr const char *LOG_FILE_NAME = "KCD2_SoundFramework.log";
    /// The default moods, beside the framework module; packs override them by name.
    inline constexpr const wchar_t *MOODS_FILE_NAME = L"KCD2_SoundFramework_moods.ini";
    /// The learned lip model ([LipSync] Mode = ADVANCED), beside the framework module.
    inline constexpr const wchar_t *LIP_MODEL_FILE_NAME = L"KCD2_SoundFramework_lipmodel.bin";
    inline constexpr const char *INSTANCE_MUTEX_PREFIX = "KCD2_SoundFramework_";
    inline constexpr const char *DEFAULT_LOG_LEVEL = "INFO";
    inline constexpr const char *GAME_MODULE = "WHGame.dll";
    inline constexpr std::chrono::milliseconds HOOK_RETIREMENT_DEADLINE{250};

    // The game's FMOD 2.02 runtime, loaded by WHGame from Bin/Win64Shared.
    inline constexpr const wchar_t *FMOD_CORE_MODULE = L"fmod.dll";
    inline constexpr const wchar_t *FMOD_STUDIO_MODULE = L"fmodstudio.dll";

    /**
     * @brief FMOD::Studio::System::setListenerAttributes(int, const FMOD_3D_ATTRIBUTES*, const FMOD_VECTOR*): the game
     *        calls it on its Studio system whenever the listener moves, which makes it the place to learn the listener.
     */
    inline constexpr const char *STUDIO_SET_LISTENER_EXPORT =
        "?setListenerAttributes@System@Studio@FMOD@@QEAA?AW4FMOD_RESULT@@HPEBUFMOD_3D_ATTRIBUTES@@PEBUFMOD_VECTOR@@@Z";

    /// FMOD::Studio::System::update(): the game calls it on its Studio system every audio frame.
    inline constexpr const char *STUDIO_SYSTEM_UPDATE_EXPORT = "?update@System@Studio@FMOD@@QEAA?AW4FMOD_RESULT@@XZ";

    /// FMOD::Studio::EventInstance::start(): every sound the game plays starts here.
    inline constexpr const char *STUDIO_EVENT_START_EXPORT =
        "?start@EventInstance@Studio@FMOD@@QEAA?AW4FMOD_RESULT@@XZ";

    /**
     * @brief FMOD::Studio::EventInstance::setParameterByName(const char*, float, bool): the only parameter setter the
     *        game imports. It moves a running event on to its next stage, such as a bow shot from the draw to the
     *        release.
     */
    inline constexpr const char *STUDIO_EVENT_SET_PARAMETER_EXPORT =
        "?setParameterByName@EventInstance@Studio@FMOD@@QEAA?AW4FMOD_RESULT@@PEBDM_N@Z";

    // WHGame.dll RTTI names and layouts (verified on Steam 1.5.6, GOG 1.5 and Game Pass 1.4+).
    inline constexpr const char *RTTI_CONSOLE = ".?AVCXConsole@@";
    inline constexpr const char *RTTI_SYSTEM = ".?AVCSystem@@";
    /// CXConsole's vtable length; slot 75 is its last.
    inline constexpr std::size_t CONSOLE_SLOT_COUNT = 76;
    /// IConsole::AddCommand(const char* name, void(*)(IConsoleCmdArgs*), int flags, const char* help).
    inline constexpr std::size_t CONSOLE_ADD_COMMAND_SLOT = 33;
    /// IConsole::RemoveCommand(const char* name).
    inline constexpr std::size_t CONSOLE_REMOVE_COMMAND_SLOT = 34;
    /// IConsole::GetCVar(const char* name): the variable, or nullptr when none has that name (case-insensitive).
    inline constexpr std::size_t CONSOLE_GET_CVAR_SLOT = 23;
    /// The console's string variables, checked by vtable before their value is read.
    inline constexpr const char *RTTI_CVAR_STRING = ".?AVCXConsoleVariableString@@";
    /// ICVar::GetString() const: the value as text, owned by the variable.
    inline constexpr std::size_t CVAR_GET_STRING_SLOT = 5;
    /// IConsoleCmdArgs::GetArgCount() const; argument 0 is the command name.
    inline constexpr std::size_t CMD_ARGS_COUNT_SLOT = 1;
    /// IConsoleCmdArgs::GetArg(int) const.
    inline constexpr std::size_t CMD_ARGS_GET_SLOT = 2;
    /// ISystem::Update(int flags, int pause_mode), once per frame on the main thread.
    inline constexpr std::size_t SYSTEM_UPDATE_SLOT = 5;
    inline constexpr const char *RTTI_ENTITY_SYSTEM = ".?AVCEntitySystem@@";
    inline constexpr const char *RTTI_ENTITY = ".?AVCEntity@@";
    inline constexpr std::size_t ENTITY_SYSTEM_SLOT_COUNT = 98;
    /// IEntitySystem::GetEntity(EntityId): nullptr for a removed entity (the id carries a salt).
    inline constexpr std::size_t ENTITY_SYSTEM_GET_ENTITY_SLOT = 14;
    /// IEntitySystem::FindEntityByName(const char*), case-insensitive.
    inline constexpr std::size_t ENTITY_SYSTEM_FIND_BY_NAME_SLOT = 16;
    inline constexpr std::size_t ENTITY_SLOT_COUNT = 158;
    /// IEntity::GetId() const.
    inline constexpr std::size_t ENTITY_GET_ID_SLOT = 1;
    /// IEntity::GetWorldPos() const, returning a Vec3 through a hidden out pointer.
    inline constexpr std::size_t ENTITY_GET_WORLD_POS_SLOT = 46;
    /// IEntity::GetCharacter(int slot): the slot's ICharacterInstance, or nullptr.
    inline constexpr std::size_t ENTITY_GET_CHARACTER_SLOT = 101;

    // Face pose bones: wh::animationmodule::C_JointPoseMorphPoseModifier turns each FA_POSE_* joint's relative
    // translation X (its pose weight) into face joint motion.
    inline constexpr const char *RTTI_POSE_MORPH = ".?AVC_JointPoseMorphPoseModifier@animationmodule@wh@@";
    inline constexpr std::size_t POSE_MORPH_SLOT_COUNT = 8;
    /// bool Execute(const SAnimationPoseModifierParams&): params[0] = ICharacterInstance*, params[1] = pose data.
    inline constexpr std::size_t POSE_MORPH_EXECUTE_SLOT = 5;
    /// ICharacterInstance::GetIDefaultSkeleton().
    inline constexpr std::size_t CHARACTER_GET_SKELETON_SLOT = 11;
    /// IDefaultSkeleton::GetJointIDByCRC32(uint32): the joint index, or -1.
    inline constexpr std::size_t SKELETON_JOINT_BY_CRC_SLOT = 4;
    /// Pose data: pointer to a joint's relative translation (its first float is a pose bone's weight).
    inline constexpr std::size_t POSE_JOINT_TRANSLATION_SLOT = 3;
    // The objects the morph hook calls into, each checked by vtable before its slots are used.
    inline constexpr const char *RTTI_CHARACTER = ".?AVCCharInstance@@";
    inline constexpr std::size_t CHARACTER_SLOT_COUNT = 73;
    inline constexpr const char *RTTI_SKELETON = ".?AVCDefaultSkeleton@@";
    inline constexpr std::size_t SKELETON_SLOT_COUNT = 23;
    inline constexpr const char *RTTI_POSE_DATA = ".?AVCPoseData@Skeleton@@";
    inline constexpr std::size_t POSE_DATA_SLOT_COUNT = 18;
    inline constexpr std::uintptr_t GENV_ENTITY_SYSTEM = 0xA0;
    /**
     * @brief gEnv's ITimer (class CTimer), checked by vtable before use. Slot 9 GetFrameTime(which) returns the frame's
     *        game time (0 while paused, scaled by the game's time scales), slot 10 GetRealFrameTime() its real
     *        duration.
     */
    inline constexpr std::uintptr_t GENV_TIMER = 0x80;
    inline constexpr const char *RTTI_TIMER = ".?AVCTimer@@";
    inline constexpr std::size_t TIMER_GET_FRAME_TIME_SLOT = 9;
    inline constexpr std::size_t TIMER_GET_REAL_FRAME_TIME_SLOT = 10;
    inline constexpr int TIMER_GAME = 0;

    // Speech (actor speak manager) and subtitles.
    inline constexpr const char *RTTI_ACTOR_SYSTEM = ".?AVCActorSystem@@";
    inline constexpr const char *RTTI_ACTOR = ".?AVC_Actor@entitymodule@wh@@";
    inline constexpr const char *RTTI_UI_SUBTITLES = ".?AVC_UISubtitles@guimodule@wh@@";
    inline constexpr std::size_t ACTOR_SYSTEM_SLOT_COUNT = 19;
    /// IActorSystem::GetActor(EntityId).
    inline constexpr std::size_t ACTOR_SYSTEM_GET_ACTOR_SLOT = 3;
    inline constexpr std::size_t ACTOR_SLOT_COUNT = 275;
    /// C_Actor's speak-manager getter (creates it on first use); every actor class shares the same function.
    inline constexpr std::size_t ACTOR_GET_SPEAK_MAN_SLOT = 143;
    /// The game context's CActorSystem.
    inline constexpr std::uintptr_t CONTEXT_ACTOR_SYSTEM = 0x180;
    /// C_ActorSpeakMan's actor (Speak reads it there).
    inline constexpr std::uintptr_t SPEAK_MAN_ACTOR = 0x08;
    /// C_Actor's IEntity (Speak reads the speaker's position through it).
    inline constexpr std::uintptr_t ACTOR_ENTITY = 0x38;
    /**
     * @brief C_ActorSpeakMan's current line: its request's localization path (a game string, length at -8), emptied
     *        when the line finishes or is stopped.
     */
    inline constexpr std::uintptr_t SPEAK_MAN_LINE_PATH = 0x48;
    /// C_ActorSpeakMan's playing sound id (u32): set when the line's sound starts, 0 once it finishes or is stopped.
    inline constexpr std::uintptr_t SPEAK_MAN_SOUND_ID = 0x8C;
    /// C_ActorSpeakMan's start-pending flag: set when Speak asks the sound module to play, cleared once it starts.
    inline constexpr std::uintptr_t SPEAK_MAN_START_PENDING = 0x90;
    /**
     * @brief C_ActorSpeakMan's lip-sync flag: Speak sets it when the line has a lip-sync animation (the speech then
     *        waits while the animation streams in) and clears it when the line has none.
     */
    inline constexpr std::uintptr_t SPEAK_MAN_HAS_LIP_SYNC = 0x91;
    /**
     * @brief The voice language, a string console variable. The game opens that language's voice paks and its facial
     *        animation pak `Data/Facials/Facials_<language>*.pak`, or the English one when there is none.
     */
    inline constexpr const char *VOICE_LANGUAGE_CVAR = "g_languageAudio";
    /// The language whose facial animation the game falls back to.
    inline constexpr std::string_view FACIAL_FALLBACK_LANGUAGE = "english";
    /// The engine animates faces only this close to the camera (ca_FacialAnimationRadius, metres).
    inline constexpr float FACIAL_ANIMATION_RADIUS = 30.0f;
    /// The game context's UI chain to the I_UISubtitles interface: [[ctx+0x150]+0x10]+0x120.
    inline constexpr std::uintptr_t CONTEXT_UI = 0x150;
    inline constexpr std::uintptr_t UI_HUD = 0x10;
    inline constexpr std::uintptr_t HUD_SUBTITLES = 0x120;
    /// I_UISubtitles sits at this offset inside C_UISubtitles.
    inline constexpr std::uintptr_t SUBTITLES_INTERFACE_OFFSET = 0x58;
    /// I_UISubtitles::ShowSubtitle(const string& text, const string& speaker, bool, bool force, bool localize).
    inline constexpr std::size_t SUBTITLES_SHOW_SLOT = 1;
    /// I_UISubtitles::ClearSubtitles().
    inline constexpr std::size_t SUBTITLES_CLEAR_SLOT = 2;
    inline constexpr std::uintptr_t GENV_CONSOLE = 0xA8;
    inline constexpr std::uintptr_t GENV_SYSTEM = 0xC0;
    /// A ReplacesFor filter matches an event that starts within this many metres of a named entity.
    inline constexpr float REPLACES_FOR_RADIUS = 2.0f;
    /// How long shutdown waits for the main thread to remove the console commands.
    inline constexpr std::chrono::milliseconds COMMAND_REMOVAL_DEADLINE{1500};
} // namespace SoundFramework::constants

#endif // SNDFW_CONSTANTS_HPP
