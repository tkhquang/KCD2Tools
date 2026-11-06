/**
 * @file constants.hpp
 * @brief File names, RTTI names and the engine layout the framework reads and writes.
 *
 * Every offset and slot here belongs to WHGame.dll and is the same on the Steam 1.5.6, GOG 1.5 and Game Pass 1.4
 * builds the anchors are checked against. The console's slots are called only after its vtable matches the resolved
 * one. The other interfaces are the engine's own gEnv members, which the quorum-resolved gEnv holds at fixed offsets.
 */
#ifndef ANIMFW_CONSTANTS_HPP
#define ANIMFW_CONSTANTS_HPP

#include "version.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace AnimationFramework::constants
{
    inline constexpr const char *MOD_NAME = AnimationFramework::version::MOD_NAME;
    inline constexpr const char *INI_FILE_EXTENSION = ".ini";

    /** @brief The INI file name, derived from the module name like every other file the mod owns. */
    [[nodiscard]] inline std::string get_config_filename()
    {
        return std::string(MOD_NAME) + INI_FILE_EXTENSION;
    }

    /// The log next to the game executable for the production builds (dllmain.cpp), and next to the dev loader.
    inline constexpr const char *LOG_FILE_NAME = "KCD2_AnimationFramework.log";
    inline constexpr const char *INSTANCE_MUTEX_PREFIX = "KCD2_AnimationFramework_";
    inline constexpr const char *DEFAULT_LOG_LEVEL = "INFO";
    inline constexpr const char *GAME_MODULE = "WHGame.dll";
    inline constexpr std::chrono::milliseconds HOOK_RETIREMENT_DEADLINE{250};
    /// How long a teardown waits for the main thread's next frame to remove the console commands.
    inline constexpr std::chrono::milliseconds CONSOLE_TEARDOWN_DEADLINE{1000};
    /// The DEV folder of loose test paks next to the game executable, mounted as mod overlays.
    inline constexpr const wchar_t *DEV_PAK_DIR = L"KCD2_AnimationFramework_Dev";
    /// The mod clip summary cache next to the game executable (core/clip_cache.hpp), which is safe to delete.
    inline constexpr const char *CLIP_CACHE_FILE = "KCD2_AnimationFramework.clipcache";

    /// MSVC-mangled RTTI type names, the source of the vtables the anchors resolve.
    inline constexpr const char *RTTI_CHARACTER_MANAGER = ".?AVCharacterManager@@";
    inline constexpr const char *RTTI_ANIMATION_DATABASE = ".?AVCAnimationDatabase@@";
    inline constexpr const char *RTTI_CONSOLE = ".?AVCXConsole@@";
    inline constexpr const char *RTTI_READ_ONLY_CHUNK_FILE = ".?AVCReadOnlyChunkFile@@";
    inline constexpr const char *RTTI_ACTION_SCOPE = ".?AVCActionScope@@";

    /// SSystemGlobalEnvironment (gEnv) member offsets: I3DEngine, ICryPak, IConsole and the CharacterManager.
    inline constexpr std::ptrdiff_t GENV_3D_ENGINE_OFFSET = 0x08;
    inline constexpr std::ptrdiff_t GENV_CRYPAK_OFFSET = 0x50;
    inline constexpr std::ptrdiff_t GENV_CONSOLE_OFFSET = 0xA8;
    inline constexpr std::ptrdiff_t GENV_CHARACTER_MANAGER_OFFSET = 0xD0;
    /**
     * @brief The game's logger and the gEnv member checked before it. Its write slot takes (file, line, type, format,
     *        va_list), and type 3 writes always, to the console and kcd.log: the game's own log helper (Steam
     *        0x1804D42B0, GOG 0x180C66DAC, Game Pass 0x18087FAD0) calls it so after checking gEnv+0xC0, on all three
     *        builds.
     */
    inline constexpr std::ptrdiff_t GENV_LOG_OFFSET = 0xE0;
    inline constexpr std::ptrdiff_t GENV_LOG_GUARD_OFFSET = 0xC0;
    inline constexpr std::size_t LOG_WRITE_SLOT = 2;
    inline constexpr std::int32_t LOG_TYPE_ALWAYS = 3;

    /**
     * @brief I3DEngine slot 202 is CreateChunkFile(bool read_only), the chunk files the clip index loads read through.
     * @details The engine itself loads gEnv->p3DEngine and calls `[rax+650h]` (Steam 0x18051E148), and the facial
     *          index load calls the same slot on its module's copy of the pointer. CSystem's own chunk-file getter is
     *          not used: its slot moves between the Steam and the GOG and Game Pass builds.
     */
    inline constexpr std::size_t THREE_D_ENGINE_CREATE_CHUNK_FILE_SLOT = 202;

    /**
     * @brief CharacterManager vtable slot 18 is Update(bool paused).
     * @details CSystem::Update calls it once per frame on the main thread (`mov rcx, [env+0D0h]`, `setnz dl`,
     *          `call [rax+90h]`), after the stream engine and before the game and entity updates that start the
     *          animation jobs, so the previous frame's jobs are done when it runs.
     */
    inline constexpr std::size_t CHARACTER_MANAGER_UPDATE_SLOT = 18;

    /**
     * @brief CReadOnlyChunkFile slot 4 is Read(path), slot 5 ReadFromMemory(data, int size), the same on the Steam,
     *        GOG and Game Pass builds.
     * @details Every chunk file the engine loads from disk goes through Read: I3DEngine::LoadChunkFileContent (slot
     *          200, which the skeleton loader calls with the .chr path) makes one and its CGF loader calls
     *          `[rax+20h]`. ReadFromMemory keeps the caller's buffer (no copy) until the chunk file goes.
     */
    inline constexpr std::size_t CHUNK_FILE_READ_SLOT = 4;
    inline constexpr std::size_t CHUNK_FILE_READ_FROM_MEMORY_SLOT = 5;

    /**
     * @brief CActionScope slot 21 is IsDifferent(fragment, fragment tags, sub context), which an AutoReinstall action
     *        asks every frame (MotionIdle, MotionMovement, HoldItem*).
     * @details It answers from a cache keyed by the fragment, its tags and the scope context's tag mask, and queries
     *          the database again only when one of them changed. The scope's context (SAnimationContext) is at +0x18.
     *          The context's CTagState puts the tag definition at context +0x08 and the 20-byte mask at context
     *          +0x10. The cached mask is at scope +0x90. Same on the Steam, GOG and Game Pass builds.
     */
    inline constexpr std::size_t ACTION_SCOPE_IS_DIFFERENT_SLOT = 21;
    inline constexpr std::ptrdiff_t ACTION_SCOPE_CONTEXT_OFFSET = 0x18;
    inline constexpr std::ptrdiff_t ACTION_SCOPE_CACHED_CONTEXT_MASK_OFFSET = 0x90;
    inline constexpr std::ptrdiff_t ANIMATION_CONTEXT_TAG_DEFINITION_OFFSET = 0x08;
    inline constexpr std::ptrdiff_t ANIMATION_CONTEXT_TAG_STATE_OFFSET = 0x10;

    /**
     * @brief The shared camera-view console variable (see camera_view.hpp) and the console slots that find, register
     *        and read it.
     * @details CXConsole slots: 3 RegisterInt(name, value, flags, help, change function), 23 GetCVar(name). ICVar slot
     *          2 is GetIVal. Retail's flag bits differ from CryEngine 5.1.1's, so the variable is registered with
     *          none. Without a flag that asks for a copy, the console keeps the name and help pointers as given (the
     *          CXConsoleVariableBase constructor).
     */
    inline constexpr const char *CAMERA_VIEW_CVAR = "mod_camera_view";
    inline constexpr const char *CAMERA_VIEW_CVAR_HELP =
        "Camera view the player sees, published by the active camera mod: 0 first person (game default), 1 third "
        "person. Values from 2 are reserved. Read it, never write it unless you are the camera mod.";
    inline constexpr std::size_t CONSOLE_REGISTER_INT_SLOT = 3;
    inline constexpr std::size_t CONSOLE_GET_CVAR_SLOT = 23;
    inline constexpr std::size_t CVAR_GET_IVAL_SLOT = 2;
    /**
     * @brief Text variables and console commands, the same on the Steam, GOG and Game Pass builds.
     * @details CXConsole slots: 2 RegisterString(name, value, flags, help, change function), 33 AddCommand(name,
     *          function, flags, help), 34 RemoveCommand(name). ICVar slots: 5 GetString, 9 Set(text).
     *          IConsoleCmdArgs slots: 1 GetArgCount, 2 GetArg. AddCommand copies the name and the help text.
     */
    inline constexpr std::size_t CONSOLE_REGISTER_STRING_SLOT = 2;
    inline constexpr std::size_t CONSOLE_ADD_COMMAND_SLOT = 33;
    inline constexpr std::size_t CONSOLE_REMOVE_COMMAND_SLOT = 34;
    /**
     * @brief CXConsole slot 35 is ExecuteString(command, silent, defer).
     * @details Lua's System.ExecuteCommand calls it with (command, false, false) on all three builds. Deferred, the
     *          console copies the command and runs it on its next update.
     */
    inline constexpr std::size_t CONSOLE_EXECUTE_STRING_SLOT = 35;
    inline constexpr std::size_t CVAR_GET_STRING_SLOT = 5;
    inline constexpr std::size_t CVAR_SET_STRING_SLOT = 9;
    inline constexpr std::size_t COMMAND_ARGS_COUNT_SLOT = 1;
    inline constexpr std::size_t COMMAND_ARGS_GET_SLOT = 2;

    /**
     * @brief CryEngine's DynArray is one pointer to its elements, with the element count in the int32 before them
     *        (CSkeletonAnim::GetNumAnimsInFIFO, Steam 0x18083A8B0, reads `[rax-4]`).
     */
    inline constexpr std::ptrdiff_t DYN_ARRAY_COUNT_OFFSET = -4;

    /**
     * @brief The CAnimationDatabase option queries and the CTagDefinition layout behind custom-tag variants
     *        (hooks/view_variants.cpp).
     * @details CAnimationDatabase answers every option query through two virtuals that share one tag-set matcher:
     *          slot 12 GetBestEntry(query, selection) (the option a scope queues) and slot 6
     *          FindBestMatchingTag(query, matched, tagSetIdx) (the option CActionScope::IsDifferent compares with the
     *          queued one). Slot 7 GetTagDefs is `mov rax, [rcx+28h]; ret`, the database's CTagDefinition. A tag
     *          state is a 20-byte bit field. SFragmentQuery is {fragment id +0, global tags +4, fragment tags +0x18,
     *          required tags +0x2C, option index +0x40}, 0x44 bytes, and both queries pass the matcher exactly that
     *          (anchors.cpp QUERY_LAYOUT proves it).
     *
     *          CTagDefinition keeps its tags in a DynArray of 0x20-byte records at +0x08. Each record holds its group
     *          id at +0x04 (-1 for a plain tag) and the CRC-32 of its lowercase name at +0x10. A tag's bits in a tag
     *          state are the {byte, mask} pair at index*2 of the array at +0x18. Same on the Steam 1.5.6, GOG 1.5 and
     *          Game Pass 1.4 builds.
     */
    inline constexpr std::size_t ANIMATION_DATABASE_FIND_BEST_MATCHING_TAG_SLOT = 6;
    inline constexpr std::size_t ANIMATION_DATABASE_GET_TAG_DEFS_SLOT = 7;
    inline constexpr std::size_t ANIMATION_DATABASE_GET_BEST_ENTRY_SLOT = 12;
    inline constexpr std::ptrdiff_t TAG_DEFINITION_TAGS_OFFSET = 0x08;
    inline constexpr std::ptrdiff_t TAG_DEFINITION_TAG_MASKS_OFFSET = 0x18;
    inline constexpr std::size_t TAG_DEFINITION_TAG_STRIDE = 0x20;
    inline constexpr std::ptrdiff_t TAG_DEFINITION_TAG_GROUP_OFFSET = 0x04;
    inline constexpr std::ptrdiff_t TAG_DEFINITION_TAG_CRC_OFFSET = 0x10;
    inline constexpr std::size_t MANNEQUIN_TAG_STATE_BYTES = 20;
    /**
     * @brief Slot 16 FindBestBlends(query, result1, result2) picks the transition between two fragments.
     * @details SBlendQuery holds the source and target fragment ids at +0/+4, the source and target SFragTagState at
     *          +0x08/+0x30 (global tags first), the additional tags at +0x58 and the flags at +0x78. The result filler
     *          reads it out to +0x7C (anchors.cpp BLEND_QUERY_LAYOUT proves the three tag-state offsets). Same on the
     *          Steam 1.5.6, GOG 1.5 and Game Pass 1.4 builds.
     */
    inline constexpr std::size_t ANIMATION_DATABASE_FIND_BEST_BLENDS_SLOT = 16;
    inline constexpr std::ptrdiff_t BLEND_QUERY_TAG_STATE_FROM_OFFSET = 0x08;
    inline constexpr std::ptrdiff_t BLEND_QUERY_TAG_STATE_TO_OFFSET = 0x30;
    /**
     * @brief SBlendQueryResult as the result filler (Steam 0x1810458DC) writes it.
     * @details The filler copies the chosen variant's source and target SFragTagState to +0x08 and +0x30 and its
     *          SFragmentBlend to +0x58. It writes the blend index and two more fields out to +0x6C. With no variant,
     *          or a variant with no blend, +0x58 keeps the caller's value. The layout is from the Steam 1.5.6 build.
     */
    inline constexpr std::ptrdiff_t BLEND_RESULT_TAG_STATE_FROM_OFFSET = 0x08;
    inline constexpr std::ptrdiff_t BLEND_RESULT_TAG_STATE_TO_OFFSET = 0x30;
    inline constexpr std::ptrdiff_t BLEND_RESULT_BLEND_OFFSET = 0x58;
    /// The tag the game sets on the player's tags (from IsPlayer) at every tag update, in a database male NPCs share.
    inline constexpr const char *PLAYER_TAG = "player";
    /**
     * @brief The built-in custom tag (custom_tags.hpp): the framework adds it to Henry's tag definition and it is on
     *        while the view is third person, so an option tagged `...+player+thirdPerson` plays only then.
     */
    inline constexpr const char *THIRD_PERSON_TAG = "thirdPerson";
    /// The attributes a mod's patch to Henry's tag definition declares a custom tag with (custom_tags.hpp).
    inline constexpr const char *CUSTOM_TAG_CVAR_ATTR = "animfw_cvar";
    inline constexpr const char *CUSTOM_TAG_MIN_ATTR = "animfw_min";
    inline constexpr const char *CUSTOM_TAG_PRIORITY_ATTR = "animfw_priority";
    /// The tag definition Henry's database (kcd_male_database.adb) uses, which gets the tag.
    inline constexpr const char *PLAYER_TAG_DEFINITION = "animations/mannequin/adb/kcd_male_tags.xml";
} // namespace AnimationFramework::constants

#endif // ANIMFW_CONSTANTS_HPP
