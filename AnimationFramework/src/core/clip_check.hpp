/**
 * @file core/clip_check.hpp
 * @brief Checks a mod's clip for the mistakes that break it or other mods in game, the rules the modding guide's
 *        "What a clip should key" gives: tracks for bones the skeleton lacks, tracks for the bones skeleton mods move,
 *        bones a full-body clip leaves to the bind pose, loops that jump, feet that slide.
 */

#ifndef ANIMFW_CORE_CLIP_CHECK_HPP
#define ANIMFW_CORE_CLIP_CHECK_HPP

#include "core/caf_reader.hpp"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace AnimationFramework
{
    /**
     * @brief The back slots skeleton mods (Swords on the Back, Realistic Bow and Crossbow Position) move in `male.chr`.
     * @details A clip track for one decides that bone while the clip plays, over the mod's skeleton. 52 of the game's
     *          13,808 male clips key them (its run and sprint among them), so a mod clip that does was most likely
     *          copied from one.
     */
    inline constexpr std::array<std::string_view, 4> SKELETON_MOD_BONES{
        "BackSlot_bow",
        "BackSlot_crossbow",
        "BackSlot_shield",
        "BackSlot_cannon",
    };

    /**
     * @brief Bones the skeleton places and a clip should leave to it (docs/MODDING.md, "What a clip should key"): the
     *        back slots and the right weapon root. A clip that leaves them out where a game clip keys them follows that
     *        rule.
     */
    inline constexpr std::array<std::string_view, 5> SKELETON_PLACED_BONES{
        "BackSlot_bow",
        "BackSlot_crossbow",
        "BackSlot_shield",
        "BackSlot_cannon",
        "RightWeaponRoot",
    };

    /// How much a clip check finding matters.
    enum class ClipFindingLevel
    {
        /// Breaks the clip or another mod.
        Warning,
        /// Worth knowing, depends on how the clip is used.
        Note,
    };

    /// One finding of a clip check, as a line of text.
    struct ClipFinding
    {
        ClipFindingLevel level = ClipFindingLevel::Note;
        std::string text;
    };

    /**
     * @brief The checks the bone list alone answers, cheap enough for every mod clip at load: tracks for bones the
     *        skeleton lacks and tracks for the bones skeleton mods move.
     */
    [[nodiscard]] std::vector<ClipFinding>
    check_clip_bones(std::span<const std::uint32_t> controllers, std::span<const SkeletonBone> skeleton);

    /** @brief What playing a clip on the skeleton measures. */
    struct ClipMotionMeasure
    {
        /// The largest change between the first and the last pose of a bone other than the root.
        double seam_degrees = 0.0;
        std::string seam_rotation_bone;
        double seam_metres = 0.0;
        std::string seam_position_bone;
        /// Horizontal root travel from the first frame to the last, or nothing when the root has no position keys.
        std::optional<double> root_travel;
        /**
         * @brief The larger of the two feet's median horizontal speed while planted, in metres per second, or nothing
         *        when the skeleton has no feet or the clip runs past an hour.
         */
        std::optional<double> foot_slide;
        std::string slide_foot;
    };

    /** @brief Plays @p clip on @p skeleton (bone 0 the root) and measures its loop seam, root travel and foot slide. */
    [[nodiscard]] ClipMotionMeasure measure_clip_motion(const CafClip &clip, const std::vector<SkeletonBone> &skeleton);

    /// What check_clip_motion checks a clip against.
    struct ClipMotionCheck
    {
        /// The clip loops (an idle, a locomotion cycle): a jump between its last and first pose is a warning.
        bool loops = false;
        /// A clip whose bones a full-body clip should cover too (the game clip the option replaces), or null.
        const CafClip *reference = nullptr;
    };

    /**
     * @brief The checks that play the clip: its length and keyed bones, bones @p check's reference keys that it leaves
     *        out, the jump between its last and first pose, its root motion, and how far planted feet slide.
     */
    [[nodiscard]] std::vector<ClipFinding>
    check_clip_motion(const CafClip &clip, const std::vector<SkeletonBone> &skeleton, const ClipMotionCheck &check);
} // namespace AnimationFramework

#endif // ANIMFW_CORE_CLIP_CHECK_HPP
