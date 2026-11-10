/**
 * @file anchors.hpp
 * @brief Every WHGame.dll address the framework uses, resolved from RTTI and byte signatures at init.
 *
 * The functions resolve through the DMK anchor registry: a ladder of two byte signatures each, a validator that checks
 * the result against the game image and its unwind data, and a feature gate (anchor::evaluate_gate) per part of the
 * framework. Engine virtuals (the animation database's, the script system's) come from their RTTI vtables, each slot
 * checked against the code shapes it must hold.
 */
#ifndef ANIMFW_ANCHORS_HPP
#define ANIMFW_ANCHORS_HPP

#include <DetourModKit.hpp>

#include <cstddef>
#include <cstdint>

namespace AnimationFramework
{
    /** @brief The resolved addresses. Zero means unresolved, or resolved inside a feature whose gate failed. */
    struct Anchors
    {
        /// SSystemGlobalEnvironment (gEnv), a static struct inside the game image.
        std::uintptr_t genv = 0;
        /// Primary vtable of CXConsole, checked before its slots are called.
        std::uintptr_t console_vtable = 0;
        /// CharacterManager::Update(bool paused), once per frame on the main thread (the frame tick).
        std::uintptr_t character_manager_update = 0;
        /// CXmlUtils::LoadXmlFromFile(out, name, patch, clean, read-only, reuse): every Mannequin database load.
        std::uintptr_t xml_load_file = 0;
        /// CharacterManager::LoadAnimationImageFileCAF(this, path, chunk file, count&): a clip index load.
        std::uintptr_t clip_image_load = 0;
        /// An animation set's CHRPARAMS reload(set, path, load).
        std::uintptr_t chrparams_reload = 0;
        /// The facial clip index load, which the clip index init runs right after animations.img.
        std::uintptr_t facial_index_load = 0;
        /// The `je` over the .dba loader's fatal "DBA/CAF mismatch" error.
        std::uintptr_t dba_mismatch_check = 0;
        /// CAnimationDatabase::GetBestEntry(query, selection), slot 12: the option a scope queues.
        std::uintptr_t get_best_entry = 0;
        /**
         * @brief CAnimationDatabase::FindBestMatchingTag(query, matched, tag set index), slot 6: the option
         *        IsDifferent compares with the queued one.
         */
        std::uintptr_t find_best_matching_tag = 0;
        /// The database's CTagDefinition field, decoded from GetTagDefs (slot 7, `mov rax, [rcx+disp8]; ret`).
        std::ptrdiff_t database_tag_defs_offset = 0;
        /**
         * @brief CAnimationDatabase::FindBestBlends(query, result1, result2), slot 16: the transition between two
         *        fragments. Resolved only with the option queries.
         */
        std::uintptr_t find_best_blends = 0;
        /// CReadOnlyChunkFile::Read(this, path), slot 4: every chunk file read from disk (skeletons included).
        std::uintptr_t chunk_file_read = 0;
        /// CReadOnlyChunkFile::ReadFromMemory(this, data, size), slot 5. Resolved only with chunk_file_read.
        std::uintptr_t chunk_file_read_from_memory = 0;
        /**
         * @brief CActionScope::IsDifferent(this, fragment, fragment tags, sub context), slot 21: whether an
         *        AutoReinstall fragment's best option changed, answered from a cache (see constants.hpp).
         */
        std::uintptr_t action_scope_is_different = 0;
        /// C_ScriptBindHuman PlayAnim: a Henry-only bind the framework opens to NPCs.
        std::uintptr_t play_anim_bind = 0;
        /// C_ScriptBindHuman StopAnim: a Henry-only bind the framework opens to NPCs.
        std::uintptr_t stop_anim_bind = 0;
        /// Primary vtable of CScriptSystem, with its ExecuteBuffer slot checked. The bridge's Lua runs through it.
        std::uintptr_t script_system_vtable = 0;
        /// Primary vtable of CMovieSystem, with its Load and LoadSequence slots checked.
        std::uintptr_t movie_system_vtable = 0;
        /// Primary vtable of CSystem, whose ISystem::LoadXmlFromFile slot CMovieSystem::Load calls. Resolved with it.
        std::uintptr_t system_vtable = 0;
        /// The movie system's ISystem field, decoded from CMovieSystem::Load (`mov rcx, [rcx+disp8]`).
        std::ptrdiff_t movie_system_system_offset = 0;
        /// The CSystem field that holds CXmlUtils, decoded from the ISystem::LoadXmlFromFile slot Load calls.
        std::ptrdiff_t system_xml_utils_offset = 0;
        /// Primary vtable of C_CutsceneHolder, the game-object extension of a CutsceneHolder entity.
        std::uintptr_t cutscene_holder_vtable = 0;
        /// Makes a C_CutsceneConfiguration from a holder: what the quest CutsceneHandler node calls to enqueue.
        std::uintptr_t cutscene_create = 0;
        /// C_CutscenePlayer's enqueue of a configuration, the handler node's next call.
        std::uintptr_t cutscene_enqueue = 0;
        /// The getter of the game object the handler node reaches the cutscene player through.
        std::uintptr_t cutscene_game_getter = 0;
        /// The first field of the chain from that object to C_CutscenePlayer, decoded from the handler node.
        std::ptrdiff_t cutscene_player_outer_offset = 0;
        /// The second field of that chain, which holds C_CutscenePlayer.
        std::ptrdiff_t cutscene_player_inner_offset = 0;
        /// The static holding the game's linkable-object manager, decoded from the spawn listener.
        std::uintptr_t linkable_manager_static = 0;
        /// Whether the manager takes an entity, the listener's check before it makes one linkable.
        std::uintptr_t linkable_filter = 0;
        /// Makes one entity a linkable object with a WUID: (manager, entity, ownership flag).
        std::uintptr_t linkable_make = 0;
        /// CEntitySystem's vtable, to check the object gEnv holds before its entity and layer slots are called.
        std::uintptr_t entity_system_vtable = 0;
        /// C_GameProfileManager's vtable, to check the object the game state holds.
        std::uintptr_t profile_manager_vtable = 0;
        /// The game state getter the cutscene's profile step reaches the profile manager through.
        std::uintptr_t profile_game_getter = 0;
        /// The first field of the chain from the game state to C_GameProfileManager, decoded from the same step.
        std::ptrdiff_t profile_manager_outer_offset = 0;
        /// The second field of that chain, which holds C_GameProfileManager.
        std::ptrdiff_t profile_manager_inner_offset = 0;
        /// CryString from text, as the cutscene's profile step makes its key name.
        std::uintptr_t string_from_text = 0;
        /// CryString's release, as the same step drops its key name.
        std::uintptr_t string_release = 0;
        /// The level loader's profile record constructor.
        std::uintptr_t profile_record_create = 0;
        /// The level loader's insert of a record into the profile manager's record vector.
        std::uintptr_t profile_records_insert = 0;
        /// The level loader's push of one layer name into a record's layer list.
        std::uintptr_t profile_layer_push = 0;
        /**
         * @brief CEntityLoadManager::ReserveEntityIds(XmlNodeRef &entities): the id reservation of one layer as it
         *        loads, reached through CEntitySystem slot 42 (`mov rcx, [rcx+disp32]; jmp`).
         */
        std::uintptr_t reserve_layer_ids = 0;
        /**
         * @brief The spin lock of CEntitySystem's entity id allocator, decoded from ReserveEntityId (slot 17).
         *        Resolved only with reserve_layer_ids, as are the two offsets below.
         */
        std::ptrdiff_t entity_id_lock_offset = 0;
        /// The allocator's slot array (24-byte slots: salt +0, prev +4, next +8, entity +16), from the same function.
        std::ptrdiff_t entity_id_slots_offset = 0;
        /// The free list head relative to the slot array, decoded from the unlink ReserveEntityId reaches.
        std::ptrdiff_t entity_id_free_head_offset = 0;
        /// Primary vtable of the RTTR method wrapper of the quest function ExecuteConsoleString.
        std::uintptr_t console_string_method_vtable = 0;
        /// The wrapper's single-argument invoke (slot 15).
        std::uintptr_t console_string_invoke = 0;
        /// The wrapper's argument-list invoke (slot 17).
        std::uintptr_t console_string_invoke_list = 0;
        /// CAnimSequence::TriggerTrackEvent: an Event node's key reaching its sequence.
        std::uintptr_t trigger_track_event = 0;
    };

    /**
     * @brief Resolves every anchor inside @p game, logs each one and gates each feature on its anchors.
     * @return The anchors. Each feature checks the ones it needs and stays off without them.
     * @note Setup/control-plane only.
     */
    [[nodiscard]] Anchors resolve_anchors(DMK::Region game);
} // namespace AnimationFramework

#endif // ANIMFW_ANCHORS_HPP
