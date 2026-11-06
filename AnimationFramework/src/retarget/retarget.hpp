/**
 * @file retarget/retarget.hpp
 * @brief Puts an animation made on another skeleton (motion capture, Mixamo, SMPL, other games) on Henry's.
 * @details Each mapped bone of Henry's takes the source bone's world-space rotation change from a reference pose, so
 *          the motion transfers whatever the two skeletons' bone axes are. The reference is the source's rest pose (or
 *          a chosen frame) with every limb turned onto the direction the matching bone of Henry's has in its default
 *          pose: a T-pose source and Henry's A-pose then line up, and a source arm hanging along Henry's default arm
 *          gives Henry's default arm. Chains of different lengths (spine, neck) spread the rotation along Henry's
 *          bones. The pelvis height scales by leg length. The root follows the pelvis on the ground. Bones the source
 *          lacks keep their default pose relative to their parent, the fingers take a hands pose (a game clip's) when
 *          the source has none, and the IK helper bones follow the bones the game reads them for.
 */

#ifndef ANIMFW_RETARGET_RETARGET_HPP
#define ANIMFW_RETARGET_RETARGET_HPP

#include "core/caf_reader.hpp"
#include "core/clip_compiler.hpp"
#include "retarget/mocap.hpp"

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace AnimationFramework
{
    /** @brief How the root (`Human_Male`, the character's place in the world) moves. */
    enum class RootMotion
    {
        /// Along the pelvis's path on the ground, smoothed over a third of a second (a walk or a run moves Henry).
        Path,
        /// In a straight line at one steady speed, from the pelvis's path (looping locomotion: no jump at the seam).
        Line,
        /// Not at all: the pelvis moves under a still root.
        InPlace,
    };

    /** @brief How retarget() maps, aligns and cuts the source. */
    struct RetargetOptions
    {
        /// The source frame that stands for the reference pose, or nothing for the source's rest pose.
        std::optional<std::size_t> calibration_frame;
        RootMotion root = RootMotion::Path;
        /// The root turns with the pelvis's facing (smoothed), for clips that turn the body.
        bool root_turns = false;
        /// Keep the source's facing instead of turning the motion to start facing Henry's forward.
        bool keep_facing = false;
        /**
         * @brief The forearm roll bones take their share of the hand's twist (by where they sit along the forearm), so
         *        the skin weighted to them turns gradually. The game's own clips leave them still, which pinches a
         *        twisted wrist.
         */
        bool share_twist = true;
        /**
         * @brief Hold each planted heel or ball of the foot in one place (two-bone IK on the leg), for sources whose
         *        feet skate. Never for an in-place root, whose feet must slide.
         * @note Off by default: it roughens the knees' path, and retargeted feet slide about as little as the game's
         *       own clips (2 to 7 cm/s).
         */
        bool lock_feet = false;
        /// Where the kept part of the source starts, in seconds from its start (clamped to the source).
        double from_seconds = 0.0;
        /// Where the kept part ends, in seconds from the source's start, or nothing for the source's end.
        std::optional<double> to_seconds;
        /**
         * @brief Henry bone = source joint pairs that replace the automatic mapping (an empty joint leaves the bone
         *        out). A chain bone named here turns that chain's automatic mapping off, and its other bones follow
         *        the named ones.
         */
        std::vector<std::pair<std::string, std::string>> map;
    };

    /** @brief How one retargeted foot sits on the ground. */
    struct RetargetFoot
    {
        /// Median ground speed of the foot while it is planted (within 3 cm of its lowest), m/s: foot slide.
        double planted_slide = 0.0;
        /// The ankle's lowest height above Henry's default ankle height, m (negative: through the ground).
        double lowest = 0.0;
    };

    /** @brief What a retarget did, for the user to check. */
    struct RetargetReport
    {
        /// Henry bone and the source joint it follows, in Henry's bone order.
        std::vector<std::pair<std::string, std::string>> mapping;
        /// Things worth checking (bones the source lacks, joints left unmapped).
        std::vector<std::string> notes;
        double source_fps = 0.0;
        /// The retargeted clip's length.
        double seconds = 0.0;
        /// Henry's leg length over the source's.
        double scale = 0.0;
        /// The source's up axis in its reference pose, such as "+Y".
        std::string up_axis;
        RetargetFoot left_foot;
        RetargetFoot right_foot;
        /// Frames a foot lock adjusted a leg in (both legs counted).
        std::size_t locked_frames = 0;
    };

    /** @brief A retargeted clip, ready to compile, and its report. */
    struct RetargetResult
    {
        SampledClip clip;
        RetargetReport report;
    };

    /**
     * @brief Retargets @p source onto @p skeleton (Henry's, bones parents first, bone 0 the root).
     * @details Quaternions are Hamilton products that rotate column vectors (core's multiply() and rotate()). The
     *          source keeps its own units, up axis and facing: leg length gives the scale, the head over the feet
     *          gives the up axis, and the hip line gives the facing. The source must be right-handed, as Henry's
     *          skeleton is, because no rotation turns one handedness into the other.
     * @param hands A clip whose first frame gives the fingers' pose when the source has no fingers (nullptr: the
     *        skeleton's default pose).
     * @return The clip and its report, or why the source does not retarget (a pose that is not finite, a limb or the
     *         head left unrecognized, a skeleton without Henry's bones).
     */
    [[nodiscard]] std::expected<RetargetResult, std::string> retarget(
        const MocapClip &source,
        std::span<const SkeletonBone> skeleton,
        const RetargetOptions &options,
        const CafClip *hands = nullptr
    );
} // namespace AnimationFramework

#endif // ANIMFW_RETARGET_RETARGET_HPP
