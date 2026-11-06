/**
 * @file core/chr_merge.hpp
 * @brief Merges whole copies of one character skeleton (`.chr`) that several mods ship, bone by bone.
 *
 * Mods that move attachment slot bones (back-slot weapons, shields) each ship the whole skeleton with a few bones'
 * default transforms changed, so the last mod wins and the others' bones snap back. A skeleton is a CryEngine chunk
 * file (`CrCh` 0x746: a 16-byte header, then 16-byte table entries {u16 type, u16 version, u32 id, u32 size,
 * u32 offset}). Its CompiledBones chunk (0x2000 v0x800) is a 32-byte header and one 584-byte record per bone, the
 * bone's name at +312 and its parent offset at +572.
 *
 * The merge starts from the game's copy and, mod by mod in load order, takes every bone record and every other chunk
 * the mod changed. A later mod that changes the same record or chunk differently wins it, and the change it overrides
 * is reported. A copy with another chunk layout or bone list cannot be merged record by record: then nothing merges
 * and the newest copy loads as it is.
 */
#ifndef ANIMFW_CORE_CHR_MERGE_HPP
#define ANIMFW_CORE_CHR_MERGE_HPP

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace AnimationFramework
{
    /** @brief One mod's whole copy of a skeleton. */
    struct SkeletonCopy
    {
        std::string_view source;
        std::string_view bytes;
    };

    /** @brief What one mod's copy contributed. */
    struct SkeletonContribution
    {
        std::string source;
        /// Bones whose record it changed from the game's.
        std::vector<std::string> bones;
        /// Of those, the ones an earlier mod had already changed the same way.
        std::size_t same_as_earlier = 0;
        /// The other parts it changed: "bones header", or a chunk by its type ("chunk 0x2001").
        std::vector<std::string> chunks;
    };

    /** @brief A record or chunk a later mod changed differently from an earlier one: the later one wins. */
    struct SkeletonConflict
    {
        std::string winner;
        std::string loser;
        /// A bone name, "bones header" or "chunk 0x...".
        std::string what;
    };

    /** @brief The merged skeleton file, what each copy contributed and the conflicts the later copies won. */
    struct SkeletonMerge
    {
        std::string bytes;
        std::vector<SkeletonContribution> contributions;
        std::vector<SkeletonConflict> conflicts;
    };

    /** @brief Why the copies cannot merge: the copy that differs in structure, and how. */
    struct SkeletonMergeRefusal
    {
        std::string source;
        std::string reason;
    };

    /**
     * @brief Merges @p copies (load order, the newest last) onto the game's skeleton @p base.
     * @return The merged file, or why it cannot merge (the game's file or a copy is not a skeleton this reads, or a
     *         copy's chunk layout or bone list differs from the game's).
     */
    [[nodiscard]] std::expected<SkeletonMerge, SkeletonMergeRefusal>
    merge_skeletons(std::string_view base, std::span<const SkeletonCopy> copies);

} // namespace AnimationFramework

#endif // ANIMFW_CORE_CHR_MERGE_HPP
