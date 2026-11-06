/**
 * @file core/clip_check.cpp
 * @brief Clip checks (see clip_check.hpp).
 */

#include "core/clip_check.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <unordered_map>

namespace AnimationFramework
{
    namespace
    {
        // Above these a loop visibly jumps from its last pose to its first.
        constexpr double LOOP_SEAM_DEGREES = 3.0;
        constexpr double LOOP_SEAM_METRES = 0.01;
        // A toe within this of its lowest point on two frames in a row is planted.
        constexpr double FOOT_CONTACT_METRES = 0.03;
        // A planted toe moving faster than this (median) visibly skates. The game's own walks, runs and sprints measure
        // 1 to 13 cm/s.
        constexpr double FOOT_SLIDE_METRES_PER_SECOND = 0.25;
        // Root travel below this counts as an in-place clip.
        constexpr double ROOT_STILL_METRES = 0.01;
        constexpr std::size_t MAX_LISTED_BONES = 8;
        // The longest clip whose feet are measured: the measure poses every frame, and a damaged clip's frame range of
        // billions holds the caller for minutes.
        constexpr auto MAX_MEASURED_FRAMES = static_cast<std::int64_t>(MAX_CLIP_SECONDS * CLIP_FRAME_RATE);
        // The points a planted foot rests on: the toes. The ankle lifts while the toes stay down (a run's ankle moves
        // 35 to 54 cm/s through a stance its toes hold within 13).
        constexpr std::array<std::string_view, 2> FEET{
            "LeftToeBase",
            "RightToeBase",
        };
        // Twist bones: the game's own looping cycles do not loop them (its medium run's leg rolls end 14 to 21 degrees
        // from where they start while every other bone closes within 0.1), so a loop's seam leaves them out.
        constexpr std::string_view TWIST_BONE_MARK = "Roll";
        // Joints the engine reads as values rather than shows as bones (IK targets and weights, face pose weights, eye
        // and alignment helpers): the game's aim and look poses swing them 150 to 180 degrees between their first and
        // last frame.
        constexpr std::array<std::string_view, 7> ENGINE_JOINT_MARKS{
            "_IKTarget",
            "_IKBlend",
            "FA_POSE_",
            "blend_control_",
            "blend_physics_",
            "GroundAlign",
            "blendWeightVertex",
        };
        constexpr std::array<std::string_view, 4> ENGINE_JOINTS{
            "eye_left_bone",
            "eye_right_bone",
            "Align",
            "Align_Secondary",
        };

        /// Whether @p bone is one a loop's seam leaves out: a twist bone or a joint the engine reads as a value.
        [[nodiscard]] bool loops_without_seam(std::string_view bone) noexcept
        {
            return bone.find(TWIST_BONE_MARK) != std::string_view::npos ||
                   std::ranges::find(ENGINE_JOINTS, bone) != ENGINE_JOINTS.end() ||
                   std::ranges::any_of(
                       ENGINE_JOINT_MARKS,
                       [bone](std::string_view mark) { return bone.find(mark) != std::string_view::npos; }
                   );
        }

        /// The skeleton's bone names by controller id (views into @p skeleton).
        [[nodiscard]] std::unordered_map<std::uint32_t, std::string_view>
        names_by_controller(std::span<const SkeletonBone> skeleton)
        {
            std::unordered_map<std::uint32_t, std::string_view> names;
            for (const SkeletonBone &bone : skeleton)
            {
                names.emplace(bone.controller, bone.name);
            }
            return names;
        }

        /// @p names comma-joined, the first MAX_LISTED_BONES of them and a count of the rest.
        [[nodiscard]] std::string list(std::span<const std::string> names)
        {
            std::string text;
            for (std::size_t i = 0; i < names.size() && i < MAX_LISTED_BONES; ++i)
            {
                text += (i == 0 ? "" : ", ") + names[i];
            }
            if (names.size() > MAX_LISTED_BONES)
            {
                text += std::format(" and {} more", names.size() - MAX_LISTED_BONES);
            }
            return text;
        }

        /// The distance between @p a and @p b.
        [[nodiscard]] double distance(const Vec3 &a, const Vec3 &b) noexcept
        {
            return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
        }

        /// The median of @p values (the upper one of an even count), 0 for none.
        [[nodiscard]] double median(std::vector<double> values) noexcept
        {
            if (values.empty())
            {
                return 0.0;
            }
            const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
            std::ranges::nth_element(values, middle);
            return *middle;
        }

        /// Warns about the bones @p reference keys and @p clip leaves out, the skeleton-placed ones aside.
        void check_reference(
            const CafClip &clip,
            const CafClip &reference,
            const std::unordered_map<std::uint32_t, std::string_view> &names,
            std::vector<ClipFinding> &findings
        )
        {
            std::vector<std::string> missing;
            for (const ClipTrack &track : reference.tracks)
            {
                if (clip.track(track.controller) == nullptr)
                {
                    const auto it = names.find(track.controller);
                    if (it != names.end() &&
                        std::ranges::find(SKELETON_PLACED_BONES, it->second) != SKELETON_PLACED_BONES.end())
                    {
                        continue;
                    }
                    missing.push_back(
                        it != names.end() ? std::string{it->second} : std::format("{:#010x}", track.controller)
                    );
                }
            }
            if (!missing.empty())
            {
                findings.push_back(
                    ClipFinding{
                        .level = ClipFindingLevel::Warning,
                        .text = std::format(
                            "leaves out {} bone(s) {} keys: {}. On a full-body option they fall to the bind pose "
                            "(exporters drop the bones a clip never moves): key the whole body, or play it on the "
                            "upper body",
                            missing.size(),
                            reference.name.empty() ? std::string{"the reference"} : reference.name,
                            list(missing)
                        ),
                    }
                );
            }
        }

        /** @brief Horizontal root travel over the clip, or nothing when the root has no position keys. */
        [[nodiscard]] std::optional<double>
        root_travel(const CafClip &clip, std::span<const SkeletonBone> skeleton) noexcept
        {
            const ClipTrack *track = skeleton.empty() ? nullptr : clip.track(skeleton.front().controller);
            if (track == nullptr)
            {
                return std::nullopt;
            }
            const std::optional<Vec3> a = sample_position(*track, clip.start_frame);
            const std::optional<Vec3> b = sample_position(*track, clip.end_frame);
            if (!a || !b)
            {
                return std::nullopt;
            }
            return std::hypot(b->x - a->x, b->y - a->y);
        }

        /// The largest first-to-last-frame rotation and position change of a bone other than the root.
        void measure_seam(
            const CafClip &clip,
            std::span<const SkeletonBone> skeleton,
            const std::unordered_map<std::uint32_t, std::string_view> &names,
            ClipMotionMeasure &measure
        )
        {
            const std::uint32_t root = skeleton.empty() ? 0 : skeleton.front().controller;
            for (const ClipTrack &track : clip.tracks)
            {
                if (track.controller == root)
                {
                    continue;
                }
                const auto name = names.find(track.controller);
                const std::string_view bone = name != names.end() ? name->second : std::string_view{"an unknown bone"};
                if (loops_without_seam(bone))
                {
                    continue;
                }
                const std::optional<Quat> a = sample_rotation(track, clip.start_frame);
                const std::optional<Quat> b = sample_rotation(track, clip.end_frame);
                if (a && b && angle_between(*a, *b) > measure.seam_degrees)
                {
                    measure.seam_degrees = angle_between(*a, *b);
                    measure.seam_rotation_bone = bone;
                }
                const std::optional<Vec3> p = sample_position(track, clip.start_frame);
                const std::optional<Vec3> q = sample_position(track, clip.end_frame);
                if (p && q && distance(*p, *q) > measure.seam_metres)
                {
                    measure.seam_metres = distance(*p, *q);
                    measure.seam_position_bone = bone;
                }
            }
        }

        /// The larger of the two toes' median planted slide, when the skeleton has both and the clip is not too long.
        void measure_feet(const CafClip &clip, const std::vector<SkeletonBone> &skeleton, ClipMotionMeasure &measure)
        {
            const std::int64_t frames = std::max<std::int64_t>(std::int64_t{clip.end_frame} - clip.start_frame, 0) + 1;
            if (frames > MAX_MEASURED_FRAMES)
            {
                return;
            }
            const ClipPoser poser{skeleton, clip};
            std::array<std::size_t, FEET.size()> feet{};
            for (std::size_t f = 0; f < FEET.size(); ++f)
            {
                const std::optional<std::size_t> bone = poser.bone(FEET[f]);
                if (!bone)
                {
                    return;
                }
                feet[f] = *bone;
            }
            std::array<std::vector<Vec3>, FEET.size()> path;
            std::vector<Quat> rotations;
            std::vector<Vec3> positions;
            for (std::int64_t i = 0; i < frames; ++i)
            {
                poser.pose(static_cast<double>(clip.start_frame) + static_cast<double>(i), rotations, positions);
                for (std::size_t f = 0; f < FEET.size(); ++f)
                {
                    path[f].push_back(positions[feet[f]]);
                }
            }
            measure.foot_slide = 0.0;
            for (std::size_t f = 0; f < FEET.size(); ++f)
            {
                const double lowest = std::ranges::min(path[f], {}, &Vec3::z).z;
                std::vector<double> slides;
                for (std::size_t i = 0; i + 1 < path[f].size(); ++i)
                {
                    if (path[f][i].z - lowest < FOOT_CONTACT_METRES && path[f][i + 1].z - lowest < FOOT_CONTACT_METRES)
                    {
                        const double step =
                            std::hypot(path[f][i + 1].x - path[f][i].x, path[f][i + 1].y - path[f][i].y);
                        slides.push_back(step * CLIP_FRAME_RATE);
                    }
                }
                const double slide = median(std::move(slides));
                if (slide > *measure.foot_slide)
                {
                    measure.foot_slide = slide;
                    measure.slide_foot = FEET[f];
                }
            }
        }

        /// The seam finding: a warning for a loop that jumps, a note otherwise.
        void add_seam_findings(const ClipMotionMeasure &measure, bool loops, std::vector<ClipFinding> &findings)
        {
            const bool jumps = measure.seam_degrees > LOOP_SEAM_DEGREES || measure.seam_metres > LOOP_SEAM_METRES;
            if (!jumps)
            {
                if (loops)
                {
                    findings.push_back(
                        ClipFinding{
                            .level = ClipFindingLevel::Note,
                            .text = std::format(
                                "loops cleanly (last pose within {:.1f} degrees of the first)",
                                measure.seam_degrees
                            ),
                        }
                    );
                }
                return;
            }
            std::string what =
                measure.seam_degrees > LOOP_SEAM_DEGREES
                    ? std::format("{:.1f} degrees at {}", measure.seam_degrees, measure.seam_rotation_bone)
                    : std::string{};
            if (measure.seam_metres > LOOP_SEAM_METRES)
            {
                what += std::format(
                    "{}{:.1f} cm at {}",
                    what.empty() ? "" : " and ",
                    measure.seam_metres * 100.0,
                    measure.seam_position_bone
                );
            }
            if (loops)
            {
                findings.push_back(
                    ClipFinding{
                        .level = ClipFindingLevel::Warning,
                        .text = std::format("jumps {} from its last pose to its first when it loops", what),
                    }
                );
            }
            else
            {
                findings.push_back(
                    ClipFinding{
                        .level = ClipFindingLevel::Note,
                        .text = std::format(
                            "ends {} away from its first pose: fine for a one-shot, a jump if it loops",
                            what
                        ),
                    }
                );
            }
        }
    } // namespace

    std::vector<ClipFinding>
    check_clip_bones(std::span<const std::uint32_t> controllers, std::span<const SkeletonBone> skeleton)
    {
        const auto names = names_by_controller(skeleton);
        std::vector<std::string> unknown;
        std::vector<std::string> slots;
        for (const std::uint32_t controller : controllers)
        {
            const auto it = names.find(controller);
            if (it == names.end())
            {
                unknown.push_back(std::format("{:#010x}", controller));
            }
            else if (std::ranges::find(SKELETON_MOD_BONES, it->second) != SKELETON_MOD_BONES.end())
            {
                slots.emplace_back(it->second);
            }
        }
        std::vector<ClipFinding> findings;
        if (!unknown.empty())
        {
            findings.push_back(
                ClipFinding{
                    .level = ClipFindingLevel::Warning,
                    .text = std::format(
                        "keys {} bone(s) the skeleton lacks ({}): exported for another skeleton or with renamed bones",
                        unknown.size(),
                        list(unknown)
                    ),
                }
            );
        }
        if (!slots.empty())
        {
            findings.push_back(
                ClipFinding{
                    .level = ClipFindingLevel::Warning,
                    .text = std::format(
                        "keys {}, which skeleton mods move (Swords on the Back, Realistic Bow and Crossbow Position): "
                        "drop these tracks unless your mod moves them",
                        list(slots)
                    ),
                }
            );
        }
        return findings;
    }

    ClipMotionMeasure measure_clip_motion(const CafClip &clip, const std::vector<SkeletonBone> &skeleton)
    {
        ClipMotionMeasure measure;
        measure_seam(clip, skeleton, names_by_controller(skeleton), measure);
        measure.root_travel = root_travel(clip, skeleton);
        measure_feet(clip, skeleton, measure);
        return measure;
    }

    std::vector<ClipFinding>
    check_clip_motion(const CafClip &clip, const std::vector<SkeletonBone> &skeleton, const ClipMotionCheck &check)
    {
        const auto names = names_by_controller(skeleton);
        std::vector<ClipFinding> findings;
        std::vector<std::uint32_t> controllers;
        controllers.reserve(clip.tracks.size());
        for (const ClipTrack &track : clip.tracks)
        {
            controllers.push_back(track.controller);
        }
        findings.push_back(
            ClipFinding{
                .level = ClipFindingLevel::Note,
                .text = std::format(
                    "{:.3f} s, {} of the skeleton's {} bones keyed",
                    clip.seconds(),
                    clip.tracks.size(),
                    skeleton.size()
                ),
            }
        );
        for (ClipFinding &finding : check_clip_bones(controllers, skeleton))
        {
            findings.push_back(std::move(finding));
        }
        if (check.reference != nullptr)
        {
            check_reference(clip, *check.reference, names, findings);
        }

        const ClipMotionMeasure measure = measure_clip_motion(clip, skeleton);
        add_seam_findings(measure, check.loops, findings);
        const bool in_place = !measure.root_travel || *measure.root_travel < ROOT_STILL_METRES;
        if (!in_place)
        {
            findings.push_back(
                ClipFinding{
                    .level = ClipFindingLevel::Note,
                    .text = std::format(
                        "moves the root {:.2f} m ({:.2f} m/s; Henry walks 1.40, runs 3.57 and sprints 6.23 m/s)",
                        *measure.root_travel,
                        *measure.root_travel / clip.seconds()
                    ),
                }
            );
        }
        // A note, not a warning: a clip whose feet are off the ground (riding, climbing, sitting) has no planted feet
        // to measure, and a quarter of the game's moving clips read over the limit for that reason.
        if (measure.foot_slide && *measure.foot_slide > FOOT_SLIDE_METRES_PER_SECOND)
        {
            findings.push_back(
                ClipFinding{
                    .level = ClipFindingLevel::Note,
                    .text = in_place
                                ? std::format(
                                      "planted toes move {:.0f} cm/s ({}) while the root stays: an in-place cycle",
                                      *measure.foot_slide * 100.0,
                                      measure.slide_foot
                                  )
                                : std::format(
                                      "planted toes slide {:.0f} cm/s ({}, median while down; the game's walks and "
                                      "runs: 1 to 13): if the feet are on the ground, they skate",
                                      *measure.foot_slide * 100.0,
                                      measure.slide_foot
                                  ),
                }
            );
        }
        return findings;
    }
} // namespace AnimationFramework
