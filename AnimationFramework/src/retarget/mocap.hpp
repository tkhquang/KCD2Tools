/**
 * @file retarget/mocap.hpp
 * @brief Motion capture and DCC animations (BVH, FBX) read as world-space joint poses, the retargeter's input.
 */

#ifndef ANIMFW_RETARGET_MOCAP_HPP
#define ANIMFW_RETARGET_MOCAP_HPP

#include "core/caf_reader.hpp"

#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace AnimationFramework
{
    /**
     * @brief The rotation of the frame whose x axis points along @p x_axis and whose x-y plane holds @p in_plane
     *        (Gram-Schmidt, so a matrix's scale and shear drop out).
     */
    [[nodiscard]] Quat frame_rotation(const Vec3 &x_axis, const Vec3 &in_plane) noexcept;

    /** @brief A rotation and a position (no scale). */
    struct Pose
    {
        Quat rotation;
        Vec3 position;
    };

    /** @brief A joint of a MocapClip's skeleton. */
    struct MocapJoint
    {
        std::string name;
        /// Index of the parent joint, -1 for a root. Parents come before their children.
        int parent = -1;
    };

    /**
     * @brief An animation as every joint's world pose at every frame. Units and axes are the source's own: the
     *        retargeter measures the skeleton to find its scale, up and facing.
     */
    struct MocapClip
    {
        std::vector<MocapJoint> joints;
        /// The skeleton's rest (bind) pose, world space.
        std::vector<Pose> rest;
        /// frames[f][j]: joint j's world pose at frame f.
        std::vector<std::vector<Pose>> frames;
        double fps = 30.0;
        /// The take (animation) read, for files that hold several.
        std::string take;
        /// Every take the file holds.
        std::vector<std::string> takes;

        /// The index of the joint named @p name, or -1.
        [[nodiscard]] int find(std::string_view name) const noexcept;
    };

    /**
     * @brief A Biovision hierarchy (`.bvh`): the rest pose from the offsets, rotations in the order each joint
     *        lists.
     * @details The axes are read as right-handed, in the file's own units. Position channels replace a joint's
     *          offset. The frame count is bounded by the numbers the motion section can hold.
     */
    [[nodiscard]] std::expected<MocapClip, std::string> read_bvh(std::string_view text);

    /**
     * @brief An FBX file's skeleton and one of its takes (by name, or the first that moves when @p take is empty),
     *        sampled at 30 frames a second, converted to meters and Z up. A take longer than MAX_CLIP_SECONDS fails.
     */
    [[nodiscard]] std::expected<MocapClip, std::string>
    read_fbx(std::span<const std::byte> file, std::string_view take = {});

    /** @brief A `.bvh` or `.fbx` file by its extension. The error leaves the file's name to the caller. */
    [[nodiscard]] std::expected<MocapClip, std::string>
    read_mocap_file(const std::filesystem::path &file, std::string_view take = {});
} // namespace AnimationFramework

#endif // ANIMFW_RETARGET_MOCAP_HPP
