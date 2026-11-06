/**
 * @file anchors.hpp
 * @brief Every WHGame.dll address the framework uses, resolved from RTTI and byte signatures at init.
 *
 * The functions resolve through the DMK anchor registry: a ladder of two byte signatures each, a validator that checks
 * the result against the game image and its unwind data, and a feature gate (anchor::evaluate_gate) per part of the
 * framework. The database's virtuals come from its RTTI vtable, each slot checked against the code shapes it must hold.
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
    };

    /**
     * @brief Resolves every anchor inside @p game, logs each one and gates each feature on its anchors.
     * @return The anchors. Each feature checks the ones it needs and stays off without them.
     * @note Setup/control-plane only.
     */
    [[nodiscard]] Anchors resolve_anchors(DMK::Region game);
} // namespace AnimationFramework

#endif // ANIMFW_ANCHORS_HPP
