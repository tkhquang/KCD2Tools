/**
 * @file character_pose.cpp
 * @brief The player's animated neck joint (see character_pose.hpp).
 */

#include "character_pose.hpp"
#include "aob_resolver.hpp"
#include "character_attachments.hpp"
#include "constants.hpp"
#include "hooks/vtable_slots.hpp"
#include "rtti_types.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string_view>

namespace TPVCamera
{
    namespace
    {
        constexpr std::uintptr_t k_min_address = 0x10000;
        // A neck read farther than this from the character's origin is taken for a layout drift, not a pose.
        constexpr float k_max_joint_reach = 3.0f;

        /** @brief CRC32 (reflected 0xEDB88320) of an ASCII name in lower case, the key of the skeleton's name table. */
        [[nodiscard]] constexpr std::uint32_t lower_name_crc32(std::string_view name) noexcept
        {
            std::uint32_t crc = 0xFFFFFFFFu;
            for (const char c : name)
            {
                const char lower = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
                crc ^= static_cast<std::uint8_t>(lower);
                for (int bit = 0; bit < 8; ++bit)
                {
                    crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
                }
            }
            return ~crc;
        }
        static_assert(lower_name_crc32("Bip01 Head") == 0x4A3B6D8Au, "the skeleton keys joints by the zlib CRC32");
        constexpr std::uint32_t k_neck_crc = lower_name_crc32("Neck");

        using GetterFn = std::uintptr_t(__fastcall *)(std::uintptr_t object);
        using AbsJointFn = std::uintptr_t(__fastcall *)(std::uintptr_t pose, int joint);
        using JointIdFn = int(__fastcall *)(std::uintptr_t skeleton, std::uint32_t crc);

        /** @brief Why a read failed, one bit each in s_reported. */
        enum class PoseError : std::uint8_t
        {
            None,
            NoCharacter,
            CharacterGetters,
            SkeletonPose,
            DefaultSkeleton,
            NoJoint,
            Fault,
            Implausible,
        };

        // Set once at setup, before the first camera frame.
        std::uintptr_t s_module_base = 0;
        std::size_t s_module_size = 0;
        // Main-thread state (the camera frame): each getter is checked once per vtable it is read from, and the
        // neck's index once per skeleton.
        std::uintptr_t s_character_vtable = 0;
        GetterFn s_get_pose = nullptr;
        GetterFn s_get_skeleton = nullptr;
        std::uintptr_t s_pose_vtable = 0;
        AbsJointFn s_abs_joint = nullptr;
        std::uintptr_t s_skeleton_vtable = 0;
        JointIdFn s_joint_id = nullptr;
        std::uintptr_t s_skeleton = 0;
        int s_neck_joint = -1;
        std::uint32_t s_reported = 0;
        bool s_failing = false;

        [[nodiscard]] std::uintptr_t read_vtable(std::uintptr_t object) noexcept
        {
            if (object < k_min_address)
            {
                return 0;
            }
            const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{object});
            return vtable.has_value() ? *vtable : 0;
        }

        /** @brief One code shape a vtable slot's target must match within its first `window` bytes. */
        struct SlotShape
        {
            const DMK::scan::Pattern *signature = nullptr;
            std::size_t window = 0;
        };

        /**
         * @brief The target of @p slot when it matches @p first or, when given, @p second, else 0
         *        (read_checked_vtable_slot()).
         */
        [[nodiscard]] std::uintptr_t checked_slot(std::uintptr_t vtable, std::size_t slot, SlotShape first,
                                                  SlotShape second = {}) noexcept
        {
            for (const SlotShape &shape : {first, second})
            {
                if (shape.signature == nullptr)
                {
                    continue;
                }
                const std::uintptr_t target = read_checked_vtable_slot(vtable, slot, *shape.signature, shape.window,
                                                                       s_module_base, s_module_size);
                if (target != 0)
                {
                    return target;
                }
            }
            return 0;
        }

        /** @brief Calls a getter. SEH-guarded, POD body: a fault answers 0. */
        [[nodiscard]] std::uintptr_t call_getter_guarded(GetterFn getter, std::uintptr_t object) noexcept
        {
            __try
            {
                return getter(object);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }

        /** @brief GetJointIDByCRC32. SEH-guarded, POD body: a fault answers -1. */
        [[nodiscard]] int joint_id_guarded(JointIdFn lookup, std::uintptr_t skeleton, std::uint32_t crc) noexcept
        {
            __try
            {
                return lookup(skeleton, crc);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return -1;
            }
        }

        /** @brief GetAbsJointByID's translation into @p out. SEH-guarded, POD body: false on a fault or no joint. */
        [[nodiscard]] bool abs_joint_guarded(AbsJointFn joint_of, std::uintptr_t pose, int joint, Vector3 &out) noexcept
        {
            __try
            {
                const std::uintptr_t quatt = joint_of(pose, joint);
                if (quatt < k_min_address)
                {
                    return false;
                }
                const auto *translation = reinterpret_cast<const float *>(quatt + Constants::QUATT_TRANSLATION_OFFSET);
                out = Vector3{translation[0], translation[1], translation[2]};
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        [[nodiscard]] std::string_view error_text(PoseError error) noexcept
        {
            switch (error)
            {
            case PoseError::NoCharacter:
                return "the player's render proxy holds no CCharInstance";
            case PoseError::CharacterGetters:
                return "CCharInstance's skeleton getters do not match their code";
            case PoseError::SkeletonPose:
                return "the ISkeletonPose is not a CSkeletonPose, or its GetAbsJointByID does not match its code";
            case PoseError::DefaultSkeleton:
                return "the IDefaultSkeleton is not a CDefaultSkeleton, or its GetJointIDByCRC32 does not match its "
                       "code";
            case PoseError::NoJoint:
                return "the skeleton has no Neck joint";
            case PoseError::Fault:
                return "a skeleton read faulted";
            case PoseError::Implausible:
                return "the Neck joint read is not a position on the body";
            case PoseError::None:
                break;
            }
            return "no error";
        }

        /** @brief Logs a failure once per kind until the next success, and fails the read. */
        [[nodiscard]] std::optional<Vector3> fail(PoseError error) noexcept
        {
            const std::uint32_t bit = 1u << static_cast<unsigned>(error);
            if ((s_reported & bit) == 0)
            {
                s_reported |= bit;
                (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                         "BodyAnchor: {}; the camera anchors to the first-person eye height instead",
                                         error_text(error));
            }
            s_failing = true;
            return std::nullopt;
        }
    } // namespace

    void initialize_character_pose(std::uintptr_t module_base, std::size_t module_size) noexcept
    {
        s_module_base = module_base;
        s_module_size = module_size;
    }

    std::optional<Vector3> neck_model_position(std::uintptr_t entity) noexcept
    {
        const std::uintptr_t character = character_instance_of(entity);
        if (character == 0)
        {
            return fail(PoseError::NoCharacter);
        }
        const std::uintptr_t character_vtable = read_vtable(character);
        if (character_vtable != s_character_vtable)
        {
            s_character_vtable = character_vtable;
            s_get_pose = reinterpret_cast<GetterFn>(
                checked_slot(character_vtable, Constants::CHAR_INSTANCE_VTABLE_GET_SKELETON_POSE_SLOT,
                             {&Aob::k_charGetSkeletonPoseBody, Aob::k_charGetterBodyWindow}));
            s_get_skeleton = reinterpret_cast<GetterFn>(
                checked_slot(character_vtable, Constants::CHAR_INSTANCE_VTABLE_GET_DEFAULT_SKELETON_SLOT,
                             {&Aob::k_charGetDefaultSkeletonBody, Aob::k_charGetterBodyWindow}));
        }
        if (s_get_pose == nullptr || s_get_skeleton == nullptr)
        {
            return fail(PoseError::CharacterGetters);
        }

        const std::uintptr_t pose = call_getter_guarded(s_get_pose, character);
        const std::uintptr_t pose_vtable = read_vtable(pose);
        if (pose_vtable == 0 || !vtable_is(GameClass::SkeletonPose, pose_vtable))
        {
            return fail(PoseError::SkeletonPose);
        }
        if (pose_vtable != s_pose_vtable)
        {
            s_pose_vtable = pose_vtable;
            s_abs_joint = reinterpret_cast<AbsJointFn>(
                checked_slot(pose_vtable, Constants::SKELETON_POSE_VTABLE_GET_ABS_JOINT_SLOT,
                             {&Aob::k_skeletonPoseAbsJointBody, Aob::k_skeletonPoseAbsJointBodyWindow},
                             {&Aob::k_skeletonPoseAbsJointTail, Aob::k_skeletonPoseAbsJointTailWindow}));
        }
        if (s_abs_joint == nullptr)
        {
            return fail(PoseError::SkeletonPose);
        }

        const std::uintptr_t skeleton = call_getter_guarded(s_get_skeleton, character);
        if (skeleton != s_skeleton)
        {
            const std::uintptr_t skeleton_vtable = read_vtable(skeleton);
            if (skeleton_vtable == 0 || !vtable_is(GameClass::DefaultSkeleton, skeleton_vtable))
            {
                return fail(PoseError::DefaultSkeleton);
            }
            if (skeleton_vtable != s_skeleton_vtable)
            {
                s_skeleton_vtable = skeleton_vtable;
                s_joint_id = reinterpret_cast<JointIdFn>(
                    checked_slot(skeleton_vtable, Constants::DEFAULT_SKELETON_VTABLE_JOINT_ID_BY_CRC_SLOT,
                                 {&Aob::k_jointIdByCrcBody, Aob::k_jointIdByCrcBodyWindow},
                                 {&Aob::k_jointIdByCrcProbe, Aob::k_jointIdByCrcProbeWindow}));
            }
            if (s_joint_id == nullptr)
            {
                return fail(PoseError::DefaultSkeleton);
            }
            s_skeleton = skeleton;
            s_neck_joint = joint_id_guarded(s_joint_id, skeleton, k_neck_crc);
            (void)DMK::log().try_log(DMK::LogLevel::Debug, "BodyAnchor: skeleton {:#x}, Neck joint {}", skeleton,
                                     s_neck_joint);
        }
        if (s_neck_joint < 0)
        {
            return fail(PoseError::NoJoint);
        }

        Vector3 neck{};
        if (!abs_joint_guarded(s_abs_joint, pose, s_neck_joint, neck))
        {
            return fail(PoseError::Fault);
        }
        // An index out of range reads the engine's static identity, whose translation is the origin.
        const float reach2 = neck.magnitude_squared();
        if (!std::isfinite(reach2) || reach2 < 1e-8f || reach2 > k_max_joint_reach * k_max_joint_reach)
        {
            return fail(PoseError::Implausible);
        }
        if (s_failing)
        {
            s_failing = false;
            s_reported = 0;
            (void)DMK::log().try_log(DMK::LogLevel::Info, "BodyAnchor: the Neck joint reads again (joint {})",
                                     s_neck_joint);
        }
        return neck;
    }
} // namespace TPVCamera
