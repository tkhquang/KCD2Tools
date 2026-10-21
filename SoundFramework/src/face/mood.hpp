/**
 * @file face/mood.hpp
 * @brief Facial moods as weights on the face rig's pose bones (`FA_POSE_<name>`), for voice lines to hold while they
 *        play.
 *
 * KCD2 faces have a FACS-style rig: each pose bone (brow lowerer, cheek raiser, corner depressor, nose wrinkler, ...)
 * deforms the face by its weight. A mood is a small set of pose weights, defined in INI files (the framework's
 * defaults file and mod packs), never in code. Poses are tagged upper face or mouth, so a line that already has an
 * authored lip-sync animation can take only the upper half of a mood.
 */
#ifndef SNDFW_FACE_MOOD_HPP
#define SNDFW_FACE_MOOD_HPP

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace SoundFramework::face
{
    enum class Region : std::uint8_t
    {
        Upper,
        Mouth,
    };

    struct PoseWeight
    {
        /// Pose name without the `FA_POSE_` prefix, as the rig spells it (for example `L_browsLowerer`).
        std::string pose;
        float weight = 0.0f;
    };

    /// The one mood that needs no definition: no poses.
    inline constexpr std::string_view NEUTRAL = "neutral";

    /**
     * @brief The largest weight of a mood pose, both as written and as applied after MoodStrength. The game's own
     *        facial clips reach about 1.6 (mouth corners, jaw drop).
     */
    inline constexpr float MAX_POSE_WEIGHT = 2.0f;

    /// The largest MoodStrength: below 1 it tones a mood down, above 1 it exaggerates it.
    inline constexpr float MAX_MOOD_STRENGTH = 2.0f;

    /**
     * @brief The rig's spelling of pose @p name (case-insensitive, with or without the `FA_POSE_` prefix), or nothing
     *        when the rig has no such pose.
     */
    [[nodiscard]] std::optional<std::string> canonical_pose(std::string_view name);

    /** @brief Whether pose @p canonical_name moves the mouth (jaw, lips, corners, cheeks' lower part) or the upper
     * face. */
    [[nodiscard]] Region region_of(std::string_view canonical_name) noexcept;

    /**
     * @brief The mouth poses speech moves syllable by syllable (jaw, lip rounding and pressing, the sticky lips), as
     *        the rig spells them. The other mouth poses (corners, smile, dimples, sneer, frown) hold the line's
     *        expression and change slowly.
     */
    [[nodiscard]] std::span<const std::string_view> articulation_poses() noexcept;

    /**
     * @brief Parses `pose:weight, pose:weight` (weights 0 to MAX_POSE_WEIGHT).
     * @return The poses, or nothing with @p error set when an item cannot be read or names no pose of the rig.
     */
    [[nodiscard]] std::optional<std::vector<PoseWeight>> parse_poses(std::string_view text, std::string &error);

    /** @brief @p base with every pose of @p extra set to the extra weight (added when @p base lacks it). */
    [[nodiscard]] std::vector<PoseWeight> merge_poses(std::vector<PoseWeight> base, std::span<const PoseWeight> extra);
} // namespace SoundFramework::face

#endif // SNDFW_FACE_MOOD_HPP
