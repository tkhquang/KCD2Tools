/**
 * @file character_pose.hpp
 * @brief The player's animated neck joint, which the camera's body anchor follows through the game's own actions.
 *
 * @details Read through the engine's skeleton interfaces: the player's CCharInstance (character_attachments.hpp), its
 *          ISkeletonPose and GetAbsJointByID, with the joint's index looked up once per skeleton by the lower-case
 *          name's CRC32 (IDefaultSkeleton::GetJointIDByCRC32). Every object is checked by its vtable and every getter
 *          by its code before it is called (layout_offsets.hpp, Aob::k_charGetSkeletonPoseBody and its siblings).
 *          Main thread only (the camera frame): the animation jobs have finished the pose by then.
 */
#ifndef TPVCAMERA_CHARACTER_POSE_HPP
#define TPVCAMERA_CHARACTER_POSE_HPP

#include "core/math/vec.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace TPVCamera
{
    /**
     * @brief Hands the module the game image the skeleton getters must lie in.
     * @note Setup only, before the first camera frame.
     */
    void initialize_character_pose(std::uintptr_t module_base, std::size_t module_size) noexcept;

    /**
     * @brief The neck joint of @p entity's character in the character's model space: x right, y forward, z up, meters
     *        from the character's origin (the entity's, whose world matrix places it).
     * @return Nothing when a getter or object does not check out, the joint is missing, or the read faults. A failure
     *         is logged once until the next success.
     */
    [[nodiscard]] std::optional<Vector3> neck_model_position(std::uintptr_t entity) noexcept;
} // namespace TPVCamera

#endif // TPVCAMERA_CHARACTER_POSE_HPP
