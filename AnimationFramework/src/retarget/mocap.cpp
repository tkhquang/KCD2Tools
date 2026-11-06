/**
 * @file retarget/mocap.cpp
 * @brief BVH and FBX animations read as world-space joint poses.
 */

#include "retarget/mocap.hpp"

#include "core/text_util.hpp"

#include <ufbx.h>

#include <charconv>
#include <cmath>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <numbers>
#include <optional>

namespace AnimationFramework
{
    namespace
    {
        constexpr double FBX_SAMPLE_RATE = 30.0;
        /// A BVH joint has at most one channel of each kind: three positions and three rotations.
        constexpr double MAX_BVH_CHANNELS = 6.0;

        [[nodiscard]] Quat axis_angle(int axis, double degrees) noexcept
        {
            const double half = degrees * std::numbers::pi / 360.0;
            Quat q{.x = 0.0, .y = 0.0, .z = 0.0, .w = std::cos(half)};
            (axis == 0 ? q.x : axis == 1 ? q.y : q.z) = std::sin(half);
            return q;
        }

        [[nodiscard]] Vec3 add(const Vec3 &a, const Vec3 &b) noexcept
        {
            return {a.x + b.x, a.y + b.y, a.z + b.z};
        }

        enum class Channel
        {
            PositionX,
            PositionY,
            PositionZ,
            RotationX,
            RotationY,
            RotationZ,
        };

        struct BvhJoint
        {
            std::string name;
            int parent = -1;
            Vec3 offset;
            std::vector<Channel> channels;
        };

        class Tokens
        {
        public:
            explicit Tokens(std::string_view text) : m_text{text} {}

            [[nodiscard]] std::string_view next() noexcept
            {
                while (m_at < m_text.size() && is_space(m_text[m_at]))
                {
                    ++m_at;
                }
                const std::size_t start = m_at;
                while (m_at < m_text.size() && !is_space(m_text[m_at]))
                {
                    ++m_at;
                }
                return m_text.substr(start, m_at - start);
            }

            [[nodiscard]] std::optional<double> number() noexcept
            {
                const std::string_view word = next();
                double value = 0.0;
                const auto [end, error] = std::from_chars(word.data(), word.data() + word.size(), value);
                if (error != std::errc{} || end != word.data() + word.size())
                {
                    return std::nullopt;
                }
                return value;
            }

        private:
            [[nodiscard]] static bool is_space(char c) noexcept
            {
                return c == ' ' || c == '\t' || c == '\r' || c == '\n';
            }

            std::string_view m_text;
            std::size_t m_at = 0;
        };

        [[nodiscard]] std::optional<Channel> channel_of(std::string_view word)
        {
            const std::string w = lowercase(word);
            if (w == "xposition")
                return Channel::PositionX;
            if (w == "yposition")
                return Channel::PositionY;
            if (w == "zposition")
                return Channel::PositionZ;
            if (w == "xrotation")
                return Channel::RotationX;
            if (w == "yrotation")
                return Channel::RotationY;
            if (w == "zrotation")
                return Channel::RotationZ;
            return std::nullopt;
        }

        /** @brief Joint world poses from each joint's local translation and rotation, parents first. */
        void to_world(std::vector<Pose> &poses, const std::vector<MocapJoint> &joints)
        {
            for (std::size_t j = 0; j < joints.size(); ++j)
            {
                const int p = joints[j].parent;
                if (p < 0)
                {
                    continue;
                }
                const Pose &parent = poses[static_cast<std::size_t>(p)];
                Pose &pose = poses[j];
                pose.position = add(parent.position, rotate(parent.rotation, pose.position));
                pose.rotation = multiply(parent.rotation, pose.rotation);
            }
        }

        struct SceneDeleter
        {
            void operator()(ufbx_scene *scene) const noexcept { ufbx_free_scene(scene); }
        };
        using ScenePtr = std::unique_ptr<ufbx_scene, SceneDeleter>;

        [[nodiscard]] std::string text_of(const ufbx_string &s)
        {
            return std::string{s.data, s.length};
        }

        [[nodiscard]] Pose pose_of(const ufbx_matrix &m) noexcept
        {
            return {
                .rotation = frame_rotation({m.m00, m.m10, m.m20}, {m.m01, m.m11, m.m21}),
                .position = {m.m03, m.m13, m.m23},
            };
        }
    } // namespace

    Quat frame_rotation(const Vec3 &x_axis, const Vec3 &in_plane) noexcept
    {
        const auto normalize = [](const Vec3 &v) -> Vec3
        {
            const double n = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
            return n > 0.0 ? Vec3{v.x / n, v.y / n, v.z / n} : Vec3{};
        };
        const Vec3 x = normalize(x_axis);
        const double d = x.x * in_plane.x + x.y * in_plane.y + x.z * in_plane.z;
        const Vec3 y = normalize({in_plane.x - d * x.x, in_plane.y - d * x.y, in_plane.z - d * x.z});
        const Vec3 z{x.y * y.z - x.z * y.y, x.z * y.x - x.x * y.z, x.x * y.y - x.y * y.x};
        // Rotation matrix columns x, y, z to a quaternion (Shepperd: the branch of the largest diagonal term).
        const double m00 = x.x, m10 = x.y, m20 = x.z, m01 = y.x, m11 = y.y, m21 = y.z, m02 = z.x, m12 = z.y, m22 = z.z;
        const double trace = m00 + m11 + m22;
        Quat q;
        if (trace > 0.0)
        {
            const double s = std::sqrt(trace + 1.0) * 2.0;
            q = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25 * s};
        }
        else if (m00 > m11 && m00 > m22)
        {
            const double s = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
            q = {0.25 * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
        }
        else if (m11 > m22)
        {
            const double s = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
            q = {(m01 + m10) / s, 0.25 * s, (m12 + m21) / s, (m02 - m20) / s};
        }
        else
        {
            const double s = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
            q = {(m02 + m20) / s, (m12 + m21) / s, 0.25 * s, (m10 - m01) / s};
        }
        const double n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        return n > 0.0 ? Quat{q.x / n, q.y / n, q.z / n, q.w / n} : Quat{};
    }

    int MocapClip::find(std::string_view name) const noexcept
    {
        for (std::size_t j = 0; j < joints.size(); ++j)
        {
            if (joints[j].name == name)
            {
                return static_cast<int>(j);
            }
        }
        return -1;
    }

    std::expected<MocapClip, std::string> read_bvh(std::string_view text)
    {
        Tokens tokens{text};
        if (lowercase(tokens.next()) != "hierarchy")
        {
            return std::unexpected{std::string{"not a BVH file (no HIERARCHY)"}};
        }
        std::vector<BvhJoint> joints;
        std::vector<int> stack;
        int pending = -1; // the joint whose block opens next
        bool end_site = false;
        int depth_end_site = 0;
        for (;;)
        {
            const std::string word = lowercase(tokens.next());
            if (word.empty())
            {
                return std::unexpected{std::string{"the BVH hierarchy ends early"}};
            }
            if (word == "motion")
            {
                break;
            }
            if (word == "root" || word == "joint")
            {
                BvhJoint joint;
                joint.name = std::string{tokens.next()};
                joint.parent = stack.empty() ? -1 : stack.back();
                joints.push_back(std::move(joint));
                pending = static_cast<int>(joints.size()) - 1;
            }
            else if (word == "end")
            {
                (void)tokens.next(); // "Site"
                end_site = true;
            }
            else if (word == "{")
            {
                if (end_site)
                {
                    ++depth_end_site;
                }
                else if (pending < 0)
                {
                    return std::unexpected{std::string{"a BVH block opens before its ROOT or JOINT"}};
                }
                else
                {
                    stack.push_back(pending);
                }
            }
            else if (word == "}")
            {
                if (depth_end_site > 0)
                {
                    --depth_end_site;
                    end_site = depth_end_site > 0;
                }
                else if (!stack.empty())
                {
                    stack.pop_back();
                }
            }
            else if (word == "offset")
            {
                const auto x = tokens.number();
                const auto y = tokens.number();
                const auto z = tokens.number();
                if (!x || !y || !z)
                {
                    return std::unexpected{std::string{"a BVH OFFSET is not three numbers"}};
                }
                if (!end_site && !stack.empty())
                {
                    joints[static_cast<std::size_t>(stack.back())].offset = {*x, *y, *z};
                }
            }
            else if (word == "channels")
            {
                const auto count = tokens.number();
                if (!count || !(*count >= 0.0 && *count <= MAX_BVH_CHANNELS) || *count != std::floor(*count) ||
                    stack.empty())
                {
                    return std::unexpected{std::string{"a BVH CHANNELS line is malformed"}};
                }
                for (int c = 0; c < static_cast<int>(*count); ++c)
                {
                    const auto channel = channel_of(tokens.next());
                    if (!channel)
                    {
                        return std::unexpected{std::string{"a BVH channel is unknown"}};
                    }
                    joints[static_cast<std::size_t>(stack.back())].channels.push_back(*channel);
                }
            }
        }
        if (joints.empty())
        {
            return std::unexpected{std::string{"the BVH file has no joints"}};
        }
        if (lowercase(tokens.next()) != "frames:")
        {
            return std::unexpected{std::string{"the BVH motion has no frame count"}};
        }
        const auto frame_count = tokens.number();
        if (lowercase(tokens.next()) != "frame" || lowercase(tokens.next()) != "time:")
        {
            return std::unexpected{std::string{"the BVH motion has no frame time"}};
        }
        const auto frame_time = tokens.number();
        std::size_t channel_count = 0;
        for (const BvhJoint &joint : joints)
        {
            channel_count += joint.channels.size();
        }
        if (channel_count == 0)
        {
            return std::unexpected{std::string{"the BVH file has no channels"}};
        }
        // from_chars reads "nan" and "inf", so both fail here before the cast below. Each frame holds one number of at
        // least one character per channel, so the text bounds the count before any frame is allocated.
        if (!frame_count || !frame_time || !std::isfinite(*frame_count) || !std::isfinite(*frame_time) ||
            *frame_count < 1.0 ||
            *frame_count * static_cast<double>(channel_count) > static_cast<double>(text.size()) || *frame_time <= 0.0)
        {
            return std::unexpected{std::string{"the BVH frame count or time is invalid"}};
        }

        MocapClip clip;
        clip.fps = 1.0 / *frame_time;
        clip.take = "BVH";
        clip.takes = {clip.take};
        for (const BvhJoint &joint : joints)
        {
            clip.joints.push_back({.name = joint.name, .parent = joint.parent});
        }
        clip.rest.resize(joints.size());
        for (std::size_t j = 0; j < joints.size(); ++j)
        {
            clip.rest[j].position = joints[j].offset;
        }
        to_world(clip.rest, clip.joints);

        const auto frames = static_cast<std::size_t>(*frame_count);
        clip.frames.reserve(frames);
        for (std::size_t f = 0; f < frames; ++f)
        {
            std::vector<Pose> poses(joints.size());
            for (std::size_t j = 0; j < joints.size(); ++j)
            {
                Vec3 position = joints[j].offset;
                Quat rotation;
                for (const Channel channel : joints[j].channels)
                {
                    const auto value = tokens.number();
                    if (!value)
                    {
                        return std::unexpected{std::format("the BVH motion ends at frame {} of {}", f, frames)};
                    }
                    switch (channel)
                    {
                    case Channel::PositionX:
                        position.x = *value;
                        break;
                    case Channel::PositionY:
                        position.y = *value;
                        break;
                    case Channel::PositionZ:
                        position.z = *value;
                        break;
                    // Rotations apply in the listed order, each about the axes the previous ones turned.
                    case Channel::RotationX:
                        rotation = multiply(rotation, axis_angle(0, *value));
                        break;
                    case Channel::RotationY:
                        rotation = multiply(rotation, axis_angle(1, *value));
                        break;
                    case Channel::RotationZ:
                        rotation = multiply(rotation, axis_angle(2, *value));
                        break;
                    }
                }
                // Position channels give the joint's position (BVH writers put the root's here) and replace the offset.
                poses[j] = {.rotation = rotation, .position = position};
            }
            to_world(poses, clip.joints);
            clip.frames.push_back(std::move(poses));
        }
        return clip;
    }

    std::expected<MocapClip, std::string> read_fbx(std::span<const std::byte> file, std::string_view take)
    {
        ufbx_load_opts opts{};
        opts.target_axes = ufbx_axes_right_handed_z_up;
        opts.target_unit_meters = 1.0;
        opts.space_conversion = UFBX_SPACE_CONVERSION_TRANSFORM_ROOT;
        ufbx_error error{};
        ScenePtr scene{ufbx_load_memory(file.data(), file.size(), &opts, &error)};
        if (!scene)
        {
            return std::unexpected{std::format("the FBX file could not be read: {}", text_of(error.description))};
        }

        // Joints: the skeleton's bones (limb nodes), each parented to its nearest bone ancestor. ufbx sorts the nodes
        // by depth, so every ancestor is a joint already when its descendants come.
        std::vector<const ufbx_node *> nodes;
        std::vector<int> joint_of(scene->nodes.count, -1);
        MocapClip clip;
        for (std::size_t n = 0; n < scene->nodes.count; ++n)
        {
            const ufbx_node *node = scene->nodes.data[n];
            if (node->bone == nullptr)
            {
                continue;
            }
            int parent = -1;
            for (const ufbx_node *up = node->parent; up != nullptr; up = up->parent)
            {
                if (joint_of[up->typed_id] >= 0)
                {
                    parent = joint_of[up->typed_id];
                    break;
                }
            }
            joint_of[node->typed_id] = static_cast<int>(nodes.size());
            nodes.push_back(node);
            clip.joints.push_back({.name = text_of(node->name), .parent = parent});
        }
        if (nodes.empty())
        {
            return std::unexpected{std::string{"the FBX file has no skeleton (no bones)"}};
        }

        // Rest: the bind pose where the file has one, else the nodes' own transforms.
        clip.rest.resize(nodes.size());
        std::vector<bool> bound(nodes.size(), false);
        for (std::size_t j = 0; j < nodes.size(); ++j)
        {
            clip.rest[j] = pose_of(nodes[j]->node_to_world);
        }
        for (std::size_t p = 0; p < scene->poses.count; ++p)
        {
            const ufbx_pose *pose = scene->poses.data[p];
            if (!pose->is_bind_pose)
            {
                continue;
            }
            for (std::size_t b = 0; b < pose->bone_poses.count; ++b)
            {
                const ufbx_bone_pose &bone = pose->bone_poses.data[b];
                const int j = bone.bone_node != nullptr ? joint_of[bone.bone_node->typed_id] : -1;
                if (j >= 0 && !bound[static_cast<std::size_t>(j)])
                {
                    clip.rest[static_cast<std::size_t>(j)] = pose_of(bone.bone_to_world);
                    bound[static_cast<std::size_t>(j)] = true;
                }
            }
        }

        // The take: by name, else the first that lasts.
        const ufbx_anim_stack *stack = nullptr;
        for (std::size_t s = 0; s < scene->anim_stacks.count; ++s)
        {
            const ufbx_anim_stack *candidate = scene->anim_stacks.data[s];
            clip.takes.push_back(text_of(candidate->name));
            const bool named = !take.empty() && lowercase(text_of(candidate->name)) == lowercase(take);
            const bool first_lasting = take.empty() && stack == nullptr && candidate->time_end > candidate->time_begin;
            if (named || first_lasting)
            {
                stack = candidate;
            }
        }
        if (stack == nullptr)
        {
            return std::unexpected{
                take.empty() ? std::string{"the FBX file has no animation"}
                             : std::format("the FBX file has no take named {}", take)
            };
        }
        clip.take = text_of(stack->name);
        clip.fps = FBX_SAMPLE_RATE;
        const double length = stack->time_end - stack->time_begin;
        // The cap also keeps the frame count below the cast's range.
        if (!std::isfinite(length) || length < 0.0 || length > MAX_CLIP_SECONDS)
        {
            return std::unexpected{std::format("the FBX take {} has no valid time span (at most an hour)", clip.take)};
        }
        const auto count = static_cast<std::size_t>(std::floor(length * FBX_SAMPLE_RATE + 1e-6)) + 1;
        clip.frames.reserve(count);
        for (std::size_t f = 0; f < count; ++f)
        {
            const double time = stack->time_begin + static_cast<double>(f) / FBX_SAMPLE_RATE;
            ScenePtr state{ufbx_evaluate_scene(scene.get(), stack->anim, time, nullptr, &error)};
            if (!state)
            {
                return std::unexpected{
                    std::format("the FBX take could not be evaluated: {}", text_of(error.description))
                };
            }
            std::vector<Pose> poses(nodes.size());
            for (std::size_t j = 0; j < nodes.size(); ++j)
            {
                poses[j] = pose_of(state->nodes.data[nodes[j]->typed_id]->node_to_world);
            }
            clip.frames.push_back(std::move(poses));
        }
        return clip;
    }

    std::expected<MocapClip, std::string> read_mocap_file(const std::filesystem::path &file, std::string_view take)
    {
        std::ifstream in{file, std::ios::binary};
        if (!in)
        {
            return std::unexpected{std::string{"the file could not be read"}};
        }
        const std::string data{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        const std::string extension = lowercase(file.extension().string());
        if (extension == ".bvh")
        {
            return read_bvh(data);
        }
        if (extension == ".fbx")
        {
            return read_fbx(std::as_bytes(std::span{data.data(), data.size()}), take);
        }
        return std::unexpected{std::string{"only .bvh and .fbx animations can be read"}};
    }
} // namespace AnimationFramework
