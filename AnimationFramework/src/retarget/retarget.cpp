/**
 * @file retarget/retarget.cpp
 * @brief The retarget of a humanoid skeleton's animation onto Henry's skeleton (see retarget.hpp).
 */

#include "retarget/retarget.hpp"

#include "core/text_util.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <map>
#include <numbers>
#include <ranges>

namespace AnimationFramework
{
    namespace
    {
        constexpr double SMOOTHING_SECONDS = 0.35;
        constexpr double PLANTED_HEIGHT = 0.03;
        constexpr double GROUND_PERCENTILE = 0.02;
        // Foot lock: a heel or ball of a foot within LOCK_HEIGHT of its lowest and slower than LOCK_SPEED is planted.
        // Plants shorter than LOCK_MIN_FRAMES are left alone, and the lock eases in and out over LOCK_EASE frames.
        constexpr double LOCK_HEIGHT = 0.025;
        constexpr double LOCK_SPEED = 0.3;
        constexpr std::size_t LOCK_MIN_FRAMES = 3;
        constexpr std::size_t LOCK_EASE = 4;
        constexpr std::size_t LOCK_SMOOTHING = 7;

        [[nodiscard]] Vec3 operator+(const Vec3 &a, const Vec3 &b) noexcept
        {
            return {a.x + b.x, a.y + b.y, a.z + b.z};
        }
        [[nodiscard]] Vec3 operator-(const Vec3 &a, const Vec3 &b) noexcept
        {
            return {a.x - b.x, a.y - b.y, a.z - b.z};
        }
        [[nodiscard]] Vec3 operator*(const Vec3 &a, double s) noexcept
        {
            return {a.x * s, a.y * s, a.z * s};
        }
        [[nodiscard]] double dot(const Vec3 &a, const Vec3 &b) noexcept
        {
            return a.x * b.x + a.y * b.y + a.z * b.z;
        }
        [[nodiscard]] Vec3 cross(const Vec3 &a, const Vec3 &b) noexcept
        {
            return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
        }
        [[nodiscard]] double length(const Vec3 &v) noexcept
        {
            return std::sqrt(dot(v, v));
        }
        [[nodiscard]] Vec3 normalized(const Vec3 &v) noexcept
        {
            const double n = length(v);
            return n > 0.0 ? v * (1.0 / n) : Vec3{};
        }
        [[nodiscard]] Vec3 lerp(const Vec3 &a, const Vec3 &b, double t) noexcept
        {
            return a + (b - a) * t;
        }

        [[nodiscard]] Quat operator*(const Quat &a, const Quat &b) noexcept
        {
            return multiply(a, b);
        }
        [[nodiscard]] double qdot(const Quat &a, const Quat &b) noexcept
        {
            return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        }
        [[nodiscard]] Quat qnormalized(const Quat &q) noexcept
        {
            const double n = std::sqrt(qdot(q, q));
            return n > 0.0 ? Quat{q.x / n, q.y / n, q.z / n, q.w / n} : Quat{};
        }
        [[nodiscard]] Quat negated(const Quat &q) noexcept
        {
            return {-q.x, -q.y, -q.z, -q.w};
        }

        [[nodiscard]] Quat slerp(const Quat &a, Quat b, double t) noexcept
        {
            double c = qdot(a, b);
            if (c < 0.0)
            {
                b = negated(b);
                c = -c;
            }
            if (c > 0.9995)
            {
                return qnormalized(
                    {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t}
                );
            }
            const double angle = std::acos(std::clamp(c, -1.0, 1.0));
            const double s = std::sin(angle);
            const double wa = std::sin((1.0 - t) * angle) / s;
            const double wb = std::sin(t * angle) / s;
            return {a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
        }

        [[nodiscard]] Quat about_axis(const Vec3 &axis, double radians) noexcept
        {
            const Vec3 a = normalized(axis) * std::sin(radians / 2.0);
            return {a.x, a.y, a.z, std::cos(radians / 2.0)};
        }

        /** @brief The smallest rotation that turns direction @p from onto @p to. */
        [[nodiscard]] Quat from_to(const Vec3 &from, const Vec3 &to) noexcept
        {
            const Vec3 a = normalized(from);
            const Vec3 b = normalized(to);
            const double c = dot(a, b);
            if (c < -0.999999)
            {
                Vec3 axis = cross({1.0, 0.0, 0.0}, a);
                if (length(axis) < 1e-6)
                {
                    axis = cross({0.0, 1.0, 0.0}, a);
                }
                return about_axis(axis, std::numbers::pi);
            }
            const Vec3 v = cross(a, b);
            return qnormalized({v.x, v.y, v.z, 1.0 + c});
        }

        /**
         * @brief The rotation that turns direction @p from onto @p to and, about it, the side direction @p from_side
         * onto
         *        @p to_side: a bone's direction and its roll (a palm's facing, a sole's tilt) both line up.
         */
        [[nodiscard]] Quat
        from_to_rolled(const Vec3 &from, const Vec3 &from_side, const Vec3 &to, const Vec3 &to_side) noexcept
        {
            if (length(cross(normalized(from), from_side)) < 1e-6 || length(cross(normalized(to), to_side)) < 1e-6)
            {
                return from_to(from, to);
            }
            return frame_rotation(to, to_side) * conjugate(frame_rotation(from, from_side));
        }

        [[nodiscard]] Pose aligned(const Quat &g, const Pose &pose) noexcept
        {
            return {.rotation = g * pose.rotation, .position = rotate(g, pose.position)};
        }

        /**
         * @brief A box filter over @p window samples (odd). Past the ends the values continue mirrored about the end
         *        value, so a steady trend stays straight to the last frame: a held end value flattens the path there
         *        and makes the pelvis jump at a loop's seam.
         */
        [[nodiscard]] std::vector<double> smoothed(const std::vector<double> &values, std::size_t window)
        {
            const auto n = static_cast<std::ptrdiff_t>(values.size());
            std::vector<double> out(values.size());
            if (n == 0)
            {
                return out;
            }
            const auto value = [&](std::ptrdiff_t i) -> double
            {
                if (i < 0)
                {
                    return 2.0 * values.front() - values[static_cast<std::size_t>(std::min(-i, n - 1))];
                }
                if (i >= n)
                {
                    return 2.0 * values.back() -
                           values[static_cast<std::size_t>(std::max<std::ptrdiff_t>(2 * (n - 1) - i, 0))];
                }
                return values[static_cast<std::size_t>(i)];
            };
            const auto half = static_cast<std::ptrdiff_t>(window / 2);
            for (std::ptrdiff_t i = 0; i < n; ++i)
            {
                double sum = 0.0;
                for (std::ptrdiff_t d = -half; d <= half; ++d)
                {
                    sum += value(i + d);
                }
                out[static_cast<std::size_t>(i)] = sum / static_cast<double>(window);
            }
            return out;
        }

        /** @brief The least-squares line through @p values (one per frame). */
        [[nodiscard]] std::vector<double> line_fit(const std::vector<double> &values)
        {
            const std::size_t n = values.size();
            if (n < 2)
            {
                return values;
            }
            double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
            for (std::size_t i = 0; i < n; ++i)
            {
                const auto x = static_cast<double>(i);
                sx += x;
                sy += values[i];
                sxx += x * x;
                sxy += x * values[i];
            }
            const double count = static_cast<double>(n);
            const double slope = (count * sxy - sx * sy) / (count * sxx - sx * sx);
            const double offset = (sy - slope * sx) / count;
            std::vector<double> out(n);
            for (std::size_t i = 0; i < n; ++i)
            {
                out[i] = offset + slope * static_cast<double>(i);
            }
            return out;
        }

        [[nodiscard]] double median(std::vector<double> values)
        {
            if (values.empty())
            {
                return 0.0;
            }
            const auto mid = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
            std::nth_element(values.begin(), mid, values.end());
            return *mid;
        }

        // Sides index SideRoles arrays and count in loops, so they stay plain ints.
        constexpr int LEFT = 0;
        constexpr int RIGHT = 1;

        constexpr std::array<std::string_view, 5> FINGERS = {"Thumb", "Index", "Middle", "Ring", "Pinky"};

        struct NameInfo
        {
            int side = -1;
            std::string stem;
            int number = -1;
            bool helper = false;
        };

        [[nodiscard]] bool starts_with_any(std::string_view text, std::initializer_list<std::string_view> words)
        {
            return std::ranges::any_of(words, [&](std::string_view w) { return text.starts_with(w); });
        }

        constexpr std::initializer_list<std::string_view> SIDED_STEMS = {
            "hip",    "upleg",  "thigh",    "knee",  "leg",    "foot",  "ankle",   "toe",
            "collar", "clav",   "shoulder", "uparm", "arm",    "elbow", "forearm", "hand",
            "wrist",  "finger", "thumb",    "index", "middle", "ring",  "pinky",
        };

        /** @brief A joint name's side, role word and number: "mixamorig:LeftHandIndex2" -> left, handindex, 2. */
        [[nodiscard]] NameInfo analyze(std::string_view raw)
        {
            NameInfo info;
            std::string name = lowercase(raw);
            if (const auto cut = name.find_last_of(":|"); cut != std::string::npos)
            {
                name = name.substr(cut + 1);
            }
            for (const std::string_view word :
                 {"end",
                  "nub",
                  "twist",
                  "roll",
                  "share",
                  "helper",
                  "target",
                  "pole",
                  "tip",
                  "top",
                  "dummy",
                  "ctrl",
                  "socket",
                  "slot",
                  "attach",
                  "weapon",
                  "prop"})
            {
                if (name.find(word) != std::string::npos)
                {
                    info.helper = true;
                }
            }
            std::vector<std::string> tokens;
            std::string token;
            for (const char c : name)
            {
                if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
                {
                    token += c;
                }
                else if (!token.empty())
                {
                    tokens.push_back(std::exchange(token, {}));
                }
            }
            if (!token.empty())
            {
                tokens.push_back(token);
            }
            std::erase_if(
                tokens,
                [](const std::string &t)
                {
                    return t == "mixamorig" || t == "mixamorig1" || t == "bip01" || t == "bip001" || t == "bip" ||
                           t == "def" || t == "jnt" || t == "j" || t == "cc" || t == "base" || t == "b";
                }
            );
            for (auto it = tokens.begin(); it != tokens.end() && info.side < 0; ++it)
            {
                if (*it == "l" || *it == "left")
                {
                    info.side = LEFT;
                    tokens.erase(it);
                    break;
                }
                if (*it == "r" || *it == "right")
                {
                    info.side = RIGHT;
                    tokens.erase(it);
                    break;
                }
            }
            for (std::string &t : tokens)
            {
                if (info.side >= 0)
                {
                    break;
                }
                if (t.starts_with("left") && t.size() > 4)
                {
                    info.side = LEFT;
                    t = t.substr(4);
                }
                else if (t.starts_with("right") && t.size() > 5)
                {
                    info.side = RIGHT;
                    t = t.substr(5);
                }
                else if (
                    (t[0] == 'l' || t[0] == 'r') && t.size() > 1 &&
                    starts_with_any(std::string_view{t}.substr(1), SIDED_STEMS)
                )
                {
                    info.side = t[0] == 'l' ? LEFT : RIGHT; // CMU: LHipJoint, RThumb
                    t = t.substr(1);
                }
            }
            std::string base;
            for (const std::string &t : tokens)
            {
                base += t;
            }
            std::size_t digits = base.size();
            while (digits > 0 && base[digits - 1] >= '0' && base[digits - 1] <= '9')
            {
                --digits;
            }
            if (digits < base.size())
            {
                // A number too long for an int leaves the joint unnumbered.
                int number = -1;
                if (std::from_chars(base.data() + digits, base.data() + base.size(), number).ec == std::errc{})
                {
                    info.number = number;
                }
            }
            info.stem = base.substr(0, digits);
            return info;
        }

        struct SideRoles
        {
            int collar = -1;
            int upper_arm = -1;
            int fore_arm = -1;
            int hand = -1;
            int thigh = -1;
            int shin = -1;
            int foot = -1;
            int toe = -1;
            std::array<std::array<int, 3>, 5> fingers{
                {{-1, -1, -1}, {-1, -1, -1}, {-1, -1, -1}, {-1, -1, -1}, {-1, -1, -1}}
            };
        };

        struct Roles
        {
            int hips = -1;
            int head = -1;
            std::vector<int> spine;
            std::vector<int> neck;
            std::array<SideRoles, 2> sides;
        };

        [[nodiscard]] bool is_any(std::string_view stem, std::initializer_list<std::string_view> words)
        {
            return std::ranges::find(words, stem) != words.end();
        }

        [[nodiscard]] int finger_of(std::string_view stem)
        {
            for (std::size_t f = 0; f < FINGERS.size(); ++f)
            {
                const std::string word = lowercase(FINGERS[f]);
                if (stem == word || stem == "hand" + word || stem == "finger" + word)
                {
                    return static_cast<int>(f);
                }
            }
            return stem == "little" || stem == "handlittle" || stem == "pinkie" ? 4 : -1;
        }

        [[nodiscard]] std::expected<Roles, std::string> detect_roles(const MocapClip &source)
        {
            const std::size_t count = source.joints.size();
            std::vector<NameInfo> info(count);
            std::vector<int> depth(count, 0);
            for (std::size_t j = 0; j < count; ++j)
            {
                info[j] = analyze(source.joints[j].name);
                const int p = source.joints[j].parent;
                depth[j] = p < 0 ? 0 : depth[static_cast<std::size_t>(p)] + 1;
            }
            Roles roles;
            const auto keep = [&](int &slot, std::size_t j) -> void
            {
                if (slot < 0 || depth[j] < depth[static_cast<std::size_t>(slot)])
                {
                    slot = static_cast<int>(j);
                }
            };
            for (int side = LEFT; side <= RIGHT; ++side)
            {
                bool has_arm = false;
                bool has_ankle = false;
                for (const NameInfo &n : info)
                {
                    if (n.side == side && !n.helper)
                    {
                        has_arm = has_arm || is_any(n.stem, {"arm", "upperarm", "uparm", "humerus"});
                        has_ankle = has_ankle || n.stem == "ankle";
                    }
                }
                SideRoles &r = roles.sides[static_cast<std::size_t>(side)];
                for (std::size_t j = 0; j < count; ++j)
                {
                    const NameInfo &n = info[j];
                    if (n.side != side || n.helper)
                    {
                        continue;
                    }
                    const std::string_view s = n.stem;
                    if (is_any(s, {"collar", "clavicle", "clav"}) || (s == "shoulder" && has_arm))
                    {
                        keep(r.collar, j);
                    }
                    else if (is_any(s, {"arm", "upperarm", "uparm", "humerus"}) || s == "shoulder")
                    {
                        keep(r.upper_arm, j);
                    }
                    else if (is_any(s, {"forearm", "lowerarm", "elbow", "lowarm"}))
                    {
                        keep(r.fore_arm, j);
                    }
                    else if (is_any(s, {"hand", "wrist"}))
                    {
                        keep(r.hand, j);
                    }
                    else if (is_any(s, {"upleg", "upperleg", "thigh", "hip", "femur"}))
                    {
                        keep(r.thigh, j);
                    }
                    else if (is_any(s, {"leg", "lowerleg", "knee", "calf", "shin", "lowleg"}))
                    {
                        keep(r.shin, j);
                    }
                    else if (s == "ankle" || (s == "foot" && !has_ankle))
                    {
                        keep(r.foot, j);
                    }
                    else if (is_any(s, {"toe", "toebase", "toes", "ball"}) || s == "foot")
                    {
                        keep(r.toe, j);
                    }
                    else if (const int f = finger_of(s); f >= 0 && n.number >= 1 && n.number <= 3)
                    {
                        keep(r.fingers[static_cast<std::size_t>(f)][static_cast<std::size_t>(n.number - 1)], j);
                    }
                }
            }
            const SideRoles &left = roles.sides[LEFT];
            const SideRoles &right = roles.sides[RIGHT];
            for (const auto &[slot, what] :
                 {std::pair{left.thigh, "left thigh"},
                  {right.thigh, "right thigh"},
                  {left.shin, "left shin"},
                  {right.shin, "right shin"},
                  {left.foot, "left foot"},
                  {right.foot, "right foot"},
                  {left.upper_arm, "left upper arm"},
                  {right.upper_arm, "right upper arm"},
                  {left.fore_arm, "left forearm"},
                  {right.fore_arm, "right forearm"},
                  {left.hand, "left hand"},
                  {right.hand, "right hand"}})
            {
                if (slot < 0)
                {
                    return std::unexpected{std::format("no {} joint was recognized: map it with --map", what)};
                }
            }

            // The pelvis: where both legs meet.
            const auto ancestors = [&](int j) -> std::vector<int>
            {
                std::vector<int> chain;
                for (; j >= 0; j = source.joints[static_cast<std::size_t>(j)].parent)
                {
                    chain.push_back(j);
                }
                return chain;
            };
            const std::vector<int> left_up = ancestors(left.thigh);
            for (int j = source.joints[static_cast<std::size_t>(right.thigh)].parent; j >= 0 && roles.hips < 0;
                 j = source.joints[static_cast<std::size_t>(j)].parent)
            {
                if (std::ranges::find(left_up, j) != left_up.end())
                {
                    roles.hips = j;
                }
            }
            if (roles.hips < 0)
            {
                return std::unexpected{std::string{"the legs share no pelvis joint"}};
            }
            for (std::size_t j = 0; j < count; ++j)
            {
                if (info[j].side < 0 && !info[j].helper && info[j].stem == "head")
                {
                    keep(roles.head, j);
                }
            }
            if (roles.head < 0)
            {
                return std::unexpected{std::string{"no head joint was recognized: map it with --map Head=<joint>"}};
            }
            std::vector<int> path = ancestors(roles.head);
            const auto at_hips = std::ranges::find(path, roles.hips);
            if (at_hips == path.end())
            {
                return std::unexpected{std::string{"the head does not hang from the pelvis"}};
            }
            path.erase(at_hips, path.end());
            path.erase(path.begin()); // the head itself
            std::ranges::reverse(path);
            for (const int j : path)
            {
                const bool neck = info[static_cast<std::size_t>(j)].stem.starts_with("neck");
                (neck || !roles.neck.empty() ? roles.neck : roles.spine).push_back(j);
            }
            return roles;
        }

        constexpr std::array<std::string_view, 5> HENRY_SPINE = {"Spine", "Spine1", "Spine2", "Spine3", "Spine4"};
        constexpr std::array<std::string_view, 2> HENRY_NECK = {"Neck", "Neck1"};
        constexpr std::array<std::string_view, 3> HENRY_NECK_HEAD = {"Neck", "Neck1", "Head"};

        [[nodiscard]] std::string sided(int side, std::string_view bone)
        {
            return std::format("{}{}", side == LEFT ? "Left" : "Right", bone);
        }

        /** @brief Where one Henry bone's world rotation change comes from at each frame. */
        struct Drive
        {
            /// A source joint, or -1.
            int joint = -1;
            /// Or a blend of two drives (chains): their indices into the drive table and the weight of the second.
            int a = -1;
            int b = -1;
            double t = 0.0;
        };
    } // namespace

    std::expected<RetargetResult, std::string> retarget(
        const MocapClip &source,
        std::span<const SkeletonBone> skeleton,
        const RetargetOptions &options,
        const CafClip *hands
    )
    {
        if (source.frames.empty() || source.joints.empty())
        {
            return std::unexpected{std::string{"the source has no frames"}};
        }
        // The walks up the source's joints index each parent, which must come before its joint. Every pose list holds
        // one pose per joint.
        const std::size_t joint_count = source.joints.size();
        const bool parents_first = std::ranges::all_of(
            std::views::iota(std::size_t{0}, joint_count),
            [&](std::size_t j) { return source.joints[j].parent < static_cast<int>(j); }
        );
        const bool posed = source.rest.size() == joint_count &&
                           std::ranges::all_of(
                               source.frames,
                               [&](const std::vector<Pose> &frame) { return frame.size() == joint_count; }
                           );
        if (!parents_first || !posed)
        {
            return std::unexpected{std::string{"the source's joints, rest pose and frames do not match"}};
        }
        // A NaN or infinite value in the file (a BVH number reads "nan" and "inf") spreads through every bone it
        // drives and into the compiled clip.
        const auto finite = [](const Pose &pose) -> bool
        {
            return std::isfinite(pose.position.x) && std::isfinite(pose.position.y) && std::isfinite(pose.position.z) &&
                   std::isfinite(pose.rotation.x) && std::isfinite(pose.rotation.y) && std::isfinite(pose.rotation.z) &&
                   std::isfinite(pose.rotation.w);
        };
        if (!std::ranges::all_of(source.rest, finite) ||
            !std::ranges::all_of(
                source.frames,
                [&](const std::vector<Pose> &frame) { return std::ranges::all_of(frame, finite); }
            ))
        {
            return std::unexpected{std::string{"the source holds a pose that is not a finite number"}};
        }
        if (!std::isfinite(source.fps) || source.fps <= 0.0 || !std::isfinite(options.from_seconds) ||
            (options.to_seconds && !std::isfinite(*options.to_seconds)))
        {
            return std::unexpected{std::string{"the source's frame rate or the span to keep is invalid"}};
        }
        if (skeleton.empty())
        {
            return std::unexpected{std::string{"the skeleton is empty"}};
        }
        // The forward kinematics below index each bone's parent, which must come before it under the one root.
        if (skeleton.front().parent >= 0)
        {
            return std::unexpected{std::format("the skeleton's first bone {} is not its root", skeleton.front().name)};
        }
        for (std::size_t b = 1; b < skeleton.size(); ++b)
        {
            if (skeleton[b].parent < 0 || static_cast<std::size_t>(skeleton[b].parent) >= b)
            {
                return std::unexpected{
                    std::format("the skeleton's bone {} does not follow its parent", skeleton[b].name)
                };
            }
        }
        auto detected = detect_roles(source);
        if (!detected)
        {
            return std::unexpected{detected.error()};
        }
        Roles roles = std::move(*detected);
        RetargetReport report;

        const std::size_t bone_count = skeleton.size();
        std::map<std::string, std::size_t, std::less<>> henry;
        for (std::size_t b = 0; b < bone_count; ++b)
        {
            henry.emplace(skeleton[b].name, b);
        }
        const auto bone = [&](std::string_view name) -> int
        {
            const auto it = henry.find(name);
            return it == henry.end() ? -1 : static_cast<int>(it->second);
        };
        for (const std::string_view needed :
             {"Hips",
              "Head",
              "LeftUpLeg",
              "LeftLeg",
              "LeftFoot",
              "LeftToeBase",
              "RightUpLeg",
              "RightLeg",
              "RightFoot",
              "RightToeBase",
              "LeftForeArm",
              "LeftHand",
              "RightForeArm",
              "RightHand",
              "Neck",
              "Spine4"})
        {
            if (bone(needed) < 0)
            {
                return std::unexpected{
                    std::format("the skeleton has no {} bone (Henry's skeleton is expected)", needed)
                };
            }
        }

        // Henry's default pose: model space, and each bone relative to its parent.
        std::vector<Quat> bind_rotation(bone_count);
        std::vector<Vec3> bind_position(bone_count);
        std::vector<Quat> local_rotation(bone_count);
        std::vector<Vec3> local_position(bone_count);
        for (std::size_t b = 0; b < bone_count; ++b)
        {
            bind_rotation[b] = qnormalized(skeleton[b].rotation);
            bind_position[b] = skeleton[b].position;
            const int p = skeleton[b].parent;
            if (p < 0)
            {
                local_rotation[b] = bind_rotation[b];
                local_position[b] = bind_position[b];
                continue;
            }
            const auto up = static_cast<std::size_t>(p);
            local_rotation[b] = conjugate(bind_rotation[up]) * bind_rotation[b];
            local_position[b] = rotate(conjugate(bind_rotation[up]), bind_position[b] - bind_position[up]);
        }
        const auto henry_at = [&](std::string_view name)
        { return bind_position[static_cast<std::size_t>(bone(name))]; };
        const Vec3 henry_up = normalized(henry_at("Head") - (henry_at("LeftFoot") + henry_at("RightFoot")) * 0.5);
        if (henry_up.z < 0.99)
        {
            return std::unexpected{std::string{"the skeleton's default pose does not stand up along Z"}};
        }
        const Vec3 henry_forward = normalized(cross(henry_up, henry_at("RightUpLeg") - henry_at("LeftUpLeg")));

        // The reference pose and the alignment of the source's up and facing onto Henry's.
        if (options.calibration_frame && *options.calibration_frame >= source.frames.size())
        {
            return std::unexpected{std::format(
                "the calibration frame {} is past the source's {} frames",
                *options.calibration_frame,
                source.frames.size()
            )};
        }
        const std::vector<Pose> &raw_reference =
            options.calibration_frame ? source.frames[*options.calibration_frame] : source.rest;
        const auto raw = [&](int j) { return raw_reference[static_cast<std::size_t>(j)].position; };
        const SideRoles &left = roles.sides[LEFT];
        const SideRoles &right = roles.sides[RIGHT];
        const Vec3 up_raw = raw(roles.head) - (raw(left.foot) + raw(right.foot)) * 0.5;
        Vec3 up_axis{};
        {
            const std::array<double, 3> c = {up_raw.x, up_raw.y, up_raw.z};
            const auto axis = static_cast<std::size_t>(
                std::ranges::max_element(c, {}, [](double v) { return std::abs(v); }) - c.begin()
            );
            (axis == 0 ? up_axis.x : axis == 1 ? up_axis.y : up_axis.z) = c[axis] > 0.0 ? 1.0 : -1.0;
            report.up_axis = std::format("{}{}", c[axis] > 0.0 ? "+" : "-", "XYZ"[axis]);
        }
        const Quat to_up = from_to(up_axis, henry_up);
        Vec3 source_forward = cross(henry_up, rotate(to_up, raw(right.thigh) - raw(left.thigh)));
        source_forward = normalized(source_forward - henry_up * dot(source_forward, henry_up));
        const double yaw =
            std::atan2(dot(cross(source_forward, henry_forward), henry_up), dot(source_forward, henry_forward));
        const Quat align = about_axis(henry_up, yaw) * to_up;

        std::vector<Pose> reference(source.joints.size());
        for (std::size_t j = 0; j < reference.size(); ++j)
        {
            reference[j] = aligned(align, raw_reference[j]);
        }

        // Frames at 30 a second over the kept span, aligned.
        const double source_seconds = static_cast<double>(source.frames.size() - 1) / source.fps;
        const double from = std::clamp(options.from_seconds, 0.0, source_seconds);
        const double to = std::min(options.to_seconds.value_or(source_seconds), source_seconds);
        if (to < from)
        {
            return std::unexpected{std::format(
                "the span {:.2f} s to {:.2f} s is empty (the source lasts {:.2f} s)",
                from,
                to,
                source_seconds
            )};
        }
        // The cap also keeps the frame count below the cast's range (a tiny frame rate stretches the source).
        if (to - from > MAX_CLIP_SECONDS)
        {
            return std::unexpected{std::format(
                "the span {:.2f} s to {:.2f} s is longer than an hour: keep a part of it with the from and to seconds",
                from,
                to
            )};
        }
        const auto frames = static_cast<std::size_t>(std::floor((to - from) * CLIP_FRAME_RATE + 1e-6)) + 1;

        // The motion turns about the vertical so the clip starts toward Henry's forward, wherever the source faced
        // then (a capture walks about its volume). The reference stays as it is, so the turn carries into Henry.
        Quat motion_align = align;
        if (!options.keep_facing)
        {
            const std::vector<Pose> &first =
                source.frames
                    [std::min(static_cast<std::size_t>(std::lround(from * source.fps)), source.frames.size() - 1)];
            Vec3 facing0 = cross(
                henry_up,
                rotate(
                    align,
                    first[static_cast<std::size_t>(right.thigh)].position -
                        first[static_cast<std::size_t>(left.thigh)].position
                )
            );
            facing0 = normalized(facing0 - henry_up * dot(facing0, henry_up));
            const double turn = std::atan2(dot(cross(facing0, henry_forward), henry_up), dot(facing0, henry_forward));
            motion_align = about_axis(henry_up, turn) * align;
        }
        std::vector<std::vector<Pose>> poses(frames, std::vector<Pose>(source.joints.size()));
        for (std::size_t k = 0; k < frames; ++k)
        {
            const double at = (from + static_cast<double>(k) / CLIP_FRAME_RATE) * source.fps;
            const auto i0 = std::min(static_cast<std::size_t>(std::floor(at)), source.frames.size() - 1);
            const std::size_t i1 = std::min(i0 + 1, source.frames.size() - 1);
            const double t = std::clamp(at - static_cast<double>(i0), 0.0, 1.0);
            for (std::size_t j = 0; j < source.joints.size(); ++j)
            {
                const Pose &a = source.frames[i0][j];
                const Pose &b = source.frames[i1][j];
                poses[k][j] = aligned(
                    motion_align,
                    {.rotation = slerp(a.rotation, b.rotation, t), .position = lerp(a.position, b.position, t)}
                );
            }
        }
        const auto posed_at = [&](std::size_t k, int j) { return poses[k][static_cast<std::size_t>(j)].position; };
        const auto reference_at = [&](int j) { return reference[static_cast<std::size_t>(j)].position; };

        const double source_leg = length(reference_at(left.thigh) - reference_at(left.shin)) +
                                  length(reference_at(left.shin) - reference_at(left.foot));
        const double henry_leg =
            length(henry_at("LeftUpLeg") - henry_at("LeftLeg")) + length(henry_at("LeftLeg") - henry_at("LeftFoot"));
        if (source_leg <= 0.0)
        {
            return std::unexpected{std::string{"the source's leg has no length in the reference pose"}};
        }
        const double scale = henry_leg / source_leg;

        // Reference corrections: each source limb's reference direction onto the matching Henry bone's.
        std::vector<Quat> correction(source.joints.size());
        const auto aim = [&](int joint, int child, std::string_view henry_bone, std::string_view henry_child) -> void
        {
            if (joint >= 0 && child >= 0 && bone(henry_bone) >= 0 && bone(henry_child) >= 0)
            {
                correction[static_cast<std::size_t>(joint)] =
                    from_to(reference_at(child) - reference_at(joint), henry_at(henry_child) - henry_at(henry_bone));
            }
        };
        const auto inherit = [&](int joint, int from_joint) -> void
        {
            if (joint >= 0 && from_joint >= 0)
            {
                correction[static_cast<std::size_t>(joint)] = correction[static_cast<std::size_t>(from_joint)];
            }
        };
        for (int side = LEFT; side <= RIGHT; ++side)
        {
            const SideRoles &r = roles.sides[static_cast<std::size_t>(side)];
            const auto side_bone = [&](std::string_view b) { return sided(side, b); };
            aim(r.collar, r.upper_arm, side_bone("Shoulder"), side_bone("Arm"));
            aim(r.upper_arm, r.fore_arm, side_bone("Arm"), side_bone("ForeArm"));
            aim(r.fore_arm, r.hand, side_bone("ForeArm"), side_bone("Hand"));
            // The palm's facing: across the knuckles, index to little finger (or ring). Without it the hand and
            // forearm keep the source rest pose's palm roll (Mixamo's hands turn 60 degrees).
            const int index = r.fingers[1][0];
            const int little = r.fingers[4][0] >= 0 ? r.fingers[4][0] : r.fingers[3][0];
            const std::string henry_little = r.fingers[4][0] >= 0 ? side_bone("HandPinky1") : side_bone("HandRing1");
            const bool palm =
                index >= 0 && little >= 0 && bone(side_bone("HandIndex1")) >= 0 && bone(henry_little) >= 0;
            const Vec3 source_palm = palm ? reference_at(index) - reference_at(little) : Vec3{};
            const Vec3 henry_palm = palm ? henry_at(side_bone("HandIndex1")) - henry_at(henry_little) : Vec3{};
            if (r.fingers[2][0] >= 0)
            {
                aim(r.hand, r.fingers[2][0], side_bone("Hand"), side_bone("HandMiddle1"));
                if (palm && bone(side_bone("HandMiddle1")) >= 0)
                {
                    correction[static_cast<std::size_t>(r.hand)] = from_to_rolled(
                        reference_at(r.fingers[2][0]) - reference_at(r.hand),
                        source_palm,
                        henry_at(side_bone("HandMiddle1")) - henry_at(side_bone("Hand")),
                        henry_palm
                    );
                }
            }
            else
            {
                inherit(r.hand, r.fore_arm);
            }
            if (palm && r.fore_arm >= 0 && r.hand >= 0)
            {
                correction[static_cast<std::size_t>(r.fore_arm)] = from_to_rolled(
                    reference_at(r.hand) - reference_at(r.fore_arm),
                    source_palm,
                    henry_at(side_bone("Hand")) - henry_at(side_bone("ForeArm")),
                    henry_palm
                );
            }
            aim(r.thigh, r.shin, side_bone("UpLeg"), side_bone("Leg"));
            aim(r.shin, r.foot, side_bone("Leg"), side_bone("Foot"));
            if (r.toe >= 0)
            {
                // The sole's tilt: up stays up, so a foot flat in the reference stands flat on Henry.
                correction[static_cast<std::size_t>(r.foot)] = from_to_rolled(
                    reference_at(r.toe) - reference_at(r.foot),
                    henry_up,
                    henry_at(side_bone("ToeBase")) - henry_at(side_bone("Foot")),
                    henry_up
                );
                inherit(r.toe, r.foot);
            }
            else
            {
                inherit(r.foot, r.shin);
            }
            for (std::size_t f = 0; f < FINGERS.size(); ++f)
            {
                const auto &chain = r.fingers[f];
                for (std::size_t s = 0; s < 2; ++s)
                {
                    aim(chain[s],
                        chain[s + 1],
                        side_bone(std::format("Hand{}{}", FINGERS[f], s + 1)),
                        side_bone(std::format("Hand{}{}", FINGERS[f], s + 2)));
                }
                inherit(chain[2], chain[1]);
            }
        }
        // The pelvis, spine, neck and head keep their reference as it is: their joints sit differently on every
        // skeleton, so a copy of their directions leans Henry off his own posture. A neutral reference stance
        // (--calibrate) does that job.

        // Which source joint (or blend) drives each Henry bone.
        std::vector<Drive> drives;
        std::vector<int> drive_of(bone_count, -1);
        const auto drive_joint = [&](std::string_view henry_bone, int joint) -> void
        {
            const int b = bone(henry_bone);
            if (b >= 0 && joint >= 0)
            {
                drive_of[static_cast<std::size_t>(b)] = static_cast<int>(drives.size());
                drives.push_back({.joint = joint});
            }
        };
        const auto drive_blend = [&](std::string_view henry_bone, int a, int b_drive, double t) -> void
        {
            const int b = bone(henry_bone);
            if (b >= 0 && a >= 0)
            {
                drive_of[static_cast<std::size_t>(b)] = static_cast<int>(drives.size());
                drives.push_back({.a = a, .b = b_drive < 0 ? a : b_drive, .t = b_drive < 0 ? 0.0 : t});
            }
        };
        // A Henry chain sampled along a source chain, first to first and last to last.
        const auto drive_chain = [&](std::span<const std::string_view> henry_chain,
                                     const std::vector<int> &source_chain) -> void
        {
            std::vector<int> joint_drives;
            for (const int j : source_chain)
            {
                joint_drives.push_back(static_cast<int>(drives.size()));
                drives.push_back({.joint = j});
            }
            const std::size_t m = henry_chain.size();
            const std::size_t n = joint_drives.size();
            for (std::size_t i = 0; i < m; ++i)
            {
                const double u = m > 1 && n > 1
                                     ? static_cast<double>(i) * static_cast<double>(n - 1) / static_cast<double>(m - 1)
                                     : 0.0;
                const auto lo = static_cast<std::size_t>(std::floor(u));
                const std::size_t hi = std::min(lo + 1, n - 1);
                drive_blend(henry_chain[i], joint_drives[lo], joint_drives[hi], u - static_cast<double>(lo));
            }
        };

        drive_joint("Hips", roles.hips);
        if (!roles.spine.empty())
        {
            drive_chain(HENRY_SPINE, roles.spine);
        }
        if (!roles.neck.empty())
        {
            // Neck to head as one chain, so Henry's upper neck turns between the source's neck and its head.
            std::vector<int> neck_to_head = roles.neck;
            neck_to_head.push_back(roles.head);
            drive_chain(HENRY_NECK_HEAD, neck_to_head);
        }
        else
        {
            drive_joint("Head", roles.head);
        }
        for (int side = LEFT; side <= RIGHT; ++side)
        {
            const SideRoles &r = roles.sides[static_cast<std::size_t>(side)];
            drive_joint(sided(side, "Shoulder"), r.collar);
            drive_joint(sided(side, "Arm"), r.upper_arm);
            drive_joint(sided(side, "ForeArm"), r.fore_arm);
            drive_joint(sided(side, "Hand"), r.hand);
            drive_joint(sided(side, "UpLeg"), r.thigh);
            drive_joint(sided(side, "Leg"), r.shin);
            drive_joint(sided(side, "Foot"), r.foot);
            drive_joint(sided(side, "ToeBase"), r.toe);
            for (std::size_t f = 0; f < FINGERS.size(); ++f)
            {
                for (std::size_t s = 0; s < 3; ++s)
                {
                    drive_joint(sided(side, std::format("Hand{}{}", FINGERS[f], s + 1)), r.fingers[f][s]);
                }
            }
        }

        // The hand-named pairs (options.map) replace the automatic mapping.
        bool spine_overridden = false;
        bool neck_overridden = false;
        for (const auto &[henry_bone, joint_name] : options.map)
        {
            const int b = bone(henry_bone);
            if (b < 0)
            {
                return std::unexpected{std::format("--map: Henry's skeleton has no bone {}", henry_bone)};
            }
            const int j = joint_name.empty() ? -1 : source.find(joint_name);
            if (!joint_name.empty() && j < 0)
            {
                return std::unexpected{std::format("--map: the source has no joint {}", joint_name)};
            }
            spine_overridden = spine_overridden || std::ranges::find(HENRY_SPINE, henry_bone) != HENRY_SPINE.end();
            neck_overridden = neck_overridden || std::ranges::find(HENRY_NECK, henry_bone) != HENRY_NECK.end();
            drive_of[static_cast<std::size_t>(b)] = -1;
            if (j >= 0)
            {
                // A joint named by hand keeps its reference as it is, unless the automatic pass corrected it.
                drive_joint(henry_bone, j);
            }
        }
        // A chain with a bone named by hand: its other bones blend between the named ones. A bone named with no
        // joint stays out.
        const auto refill_chain = [&](std::span<const std::string_view> chain) -> void
        {
            std::vector<std::pair<std::size_t, int>> anchors;
            std::vector<bool> named(chain.size(), false);
            for (std::size_t i = 0; i < chain.size(); ++i)
            {
                const int b = bone(chain[i]);
                named[i] = std::ranges::any_of(options.map, [&](const auto &entry) { return entry.first == chain[i]; });
                if (b >= 0 && named[i] && drive_of[static_cast<std::size_t>(b)] >= 0)
                {
                    anchors.emplace_back(i, drive_of[static_cast<std::size_t>(b)]);
                }
                else if (b >= 0)
                {
                    drive_of[static_cast<std::size_t>(b)] = -1;
                }
            }
            if (anchors.empty())
            {
                return;
            }
            for (std::size_t i = 0; i < chain.size(); ++i)
            {
                if (named[i] || bone(chain[i]) < 0 || drive_of[static_cast<std::size_t>(bone(chain[i]))] >= 0)
                {
                    continue;
                }
                auto after = std::ranges::find_if(anchors, [&](const auto &a) { return a.first > i; });
                if (after == anchors.begin())
                {
                    drive_blend(chain[i], after->second, -1, 0.0);
                }
                else if (after == anchors.end())
                {
                    drive_blend(chain[i], anchors.back().second, -1, 0.0);
                }
                else
                {
                    const auto before = std::prev(after);
                    const double t =
                        static_cast<double>(i - before->first) / static_cast<double>(after->first - before->first);
                    drive_blend(chain[i], before->second, after->second, t);
                }
            }
        };
        if (spine_overridden)
        {
            refill_chain(HENRY_SPINE);
        }
        if (neck_overridden)
        {
            refill_chain(HENRY_NECK);
        }

        // Henry's neck when the source has none: between the chest and the head.
        if (roles.neck.empty() && !neck_overridden)
        {
            const int chest = drive_of[static_cast<std::size_t>(bone("Spine4"))];
            const int head = drive_of[static_cast<std::size_t>(bone("Head"))];
            drive_blend("Neck", chest, head, 1.0 / 3.0);
            drive_blend("Neck1", chest, head, 2.0 / 3.0);
        }

        // The fingers' pose from the hands clip where the source does not drive them.
        std::vector<std::optional<Quat>> hands_rotation(bone_count);
        if (hands != nullptr)
        {
            for (std::size_t b = 0; b < bone_count; ++b)
            {
                const std::string &name = skeleton[b].name;
                const bool finger = (name.starts_with("LeftHand") || name.starts_with("RightHand") ||
                                     name.starts_with("LeftInHand") || name.starts_with("RightInHand")) &&
                                    name != "LeftHand" && name != "RightHand" && name.find("IK") == std::string::npos &&
                                    name.find("Slot") == std::string::npos;
                const ClipTrack *track = hands->track(skeleton[b].controller);
                if (finger && drive_of[b] < 0 && track != nullptr && !track->rotations.empty())
                {
                    hands_rotation[b] = qnormalized(track->rotations.front());
                }
            }
        }

        // Each drive's world rotation change at each frame.
        std::vector<std::vector<Quat>> delta(drives.size(), std::vector<Quat>(frames));
        for (std::size_t d = 0; d < drives.size(); ++d)
        {
            const Drive &drive = drives[d];
            if (drive.joint < 0)
            {
                continue;
            }
            const auto j = static_cast<std::size_t>(drive.joint);
            const Quat reference_rotation = conjugate(correction[j] * reference[j].rotation);
            for (std::size_t k = 0; k < frames; ++k)
            {
                delta[d][k] = qnormalized(poses[k][j].rotation * reference_rotation);
            }
        }
        for (std::size_t d = 0; d < drives.size(); ++d)
        {
            const Drive &drive = drives[d];
            if (drive.joint >= 0)
            {
                continue;
            }
            for (std::size_t k = 0; k < frames; ++k)
            {
                delta[d][k] = slerp(
                    delta[static_cast<std::size_t>(drive.a)][k],
                    delta[static_cast<std::size_t>(drive.b)][k],
                    drive.t
                );
            }
        }

        // The root's path and facing from the pelvis.
        const std::size_t window =
            std::max<std::size_t>(3, static_cast<std::size_t>(std::lround(CLIP_FRAME_RATE * SMOOTHING_SECONDS)) | 1U);
        std::vector<double> path_x(frames), path_y(frames), facing(frames);
        double unwrap = 0.0;
        double previous = 0.0;
        for (std::size_t k = 0; k < frames; ++k)
        {
            const Vec3 d = (posed_at(k, roles.hips) - posed_at(0, roles.hips)) * scale;
            path_x[k] = d.x;
            path_y[k] = d.y;
            const Vec3 f = cross(henry_up, posed_at(k, right.thigh) - posed_at(k, left.thigh));
            double angle = std::atan2(f.y, f.x);
            if (k > 0)
            {
                while (angle + unwrap - previous > std::numbers::pi)
                {
                    unwrap -= 2.0 * std::numbers::pi;
                }
                while (angle + unwrap - previous < -std::numbers::pi)
                {
                    unwrap += 2.0 * std::numbers::pi;
                }
            }
            facing[k] = angle + unwrap;
            previous = facing[k];
        }
        std::vector<double> root_x(frames, 0.0), root_y(frames, 0.0);
        if (options.root == RootMotion::Path)
        {
            root_x = smoothed(path_x, window);
            root_y = smoothed(path_y, window);
        }
        else if (options.root == RootMotion::Line)
        {
            root_x = line_fit(path_x);
            root_y = line_fit(path_y);
        }
        else
        {
            // In place: the pelvis keeps its sway about its smoothed travel, which neither it nor the root makes.
            const std::vector<double> travel_x = smoothed(path_x, window);
            const std::vector<double> travel_y = smoothed(path_y, window);
            for (std::size_t k = 0; k < frames; ++k)
            {
                path_x[k] -= travel_x[k];
                path_y[k] -= travel_y[k];
            }
        }
        std::vector<double> root_yaw(frames, 0.0);
        if (options.root_turns)
        {
            const std::vector<double> smooth = smoothed(facing, window);
            for (std::size_t k = 0; k < frames; ++k)
            {
                root_yaw[k] = smooth[k] - smooth[0];
            }
        }

        // The pelvis height: the source's hip joints above its ground, scaled, over Henry's toes.
        std::vector<double> lows(frames);
        const int left_low = left.toe >= 0 ? left.toe : left.foot;
        const int right_low = right.toe >= 0 ? right.toe : right.foot;
        for (std::size_t k = 0; k < frames; ++k)
        {
            lows[k] = std::min(posed_at(k, left_low).z, posed_at(k, right_low).z);
        }
        std::vector<double> sorted = lows;
        std::ranges::sort(sorted);
        const double ground = sorted[static_cast<std::size_t>(GROUND_PERCENTILE * static_cast<double>(frames - 1))];
        const double henry_low = left.toe >= 0 ? (henry_at("LeftToeBase").z + henry_at("RightToeBase").z) / 2.0
                                               : (henry_at("LeftFoot").z + henry_at("RightFoot").z) / 2.0;
        const double upleg_offset = henry_at("Hips").z - (henry_at("LeftUpLeg").z + henry_at("RightUpLeg").z) / 2.0;
        const Vec3 hips_offset{henry_at("Hips").x, henry_at("Hips").y, 0.0};

        // Forward kinematics of Henry's skeleton at every frame, then each bone relative to its parent.
        const auto hips_bone = static_cast<std::size_t>(bone("Hips"));
        const int spine_target = bone("Spine_IKTarget");
        const int spine_top = bone("Spine4");
        const std::array<std::pair<int, int>, 2> hand_targets = {
            std::pair{bone("LeftHand_IKTarget"), bone("LeftHand")},
            std::pair{bone("RightHand_IKTarget"), bone("RightHand")}
        };
        SampledClip clip;
        clip.frames = frames;
        clip.tracks.resize(bone_count);
        std::vector<bool> keyed_position(bone_count, false);
        for (std::size_t b = 0; b < bone_count; ++b)
        {
            clip.tracks[b].controller = skeleton[b].controller;
            clip.tracks[b].rotations.resize(frames);
            const std::string &name = skeleton[b].name;
            keyed_position[b] = b == 0 || b == hips_bone || name.ends_with("_IKTarget") || name.ends_with("_IKBlend");
            if (keyed_position[b])
            {
                clip.tracks[b].positions.resize(frames);
            }
        }
        // Forearm roll bones (children of a forearm, between it and the hand): their place along the forearm.
        struct TwistShare
        {
            int hand = -1;
            double fraction = 0.0;
        };
        std::vector<TwistShare> twist_share(bone_count);
        if (options.share_twist)
        {
            for (int side = LEFT; side <= RIGHT; ++side)
            {
                const int fore_arm = bone(sided(side, "ForeArm"));
                const int hand = bone(sided(side, "Hand"));
                if (fore_arm < 0 || hand < 0)
                {
                    continue;
                }
                const Vec3 elbow = bind_position[static_cast<std::size_t>(fore_arm)];
                const Vec3 span = bind_position[static_cast<std::size_t>(hand)] - elbow;
                for (std::size_t b = 0; b < bone_count; ++b)
                {
                    if (skeleton[b].parent == fore_arm && skeleton[b].name.find("ForeArmRoll") != std::string::npos)
                    {
                        const double t = dot(bind_position[b] - elbow, span) / dot(span, span);
                        twist_share[b] = {.hand = hand, .fraction = std::clamp(t, 0.0, 1.0)};
                    }
                }
            }
        }
        std::vector<Quat> world(bone_count);
        std::vector<Vec3> place(bone_count);
        const std::size_t left_ankle = static_cast<std::size_t>(bone("LeftFoot"));
        const std::size_t right_ankle = static_cast<std::size_t>(bone("RightFoot"));
        // Leg rotations the foot lock replaces, per frame: bone -> world rotation.
        std::vector<std::map<std::size_t, Quat>> leg_fix(frames);
        const auto pose_frame = [&](std::size_t k) -> void
        {
            const Quat root_rotation = about_axis(henry_up, root_yaw[k]) * bind_rotation[0];
            const Vec3 root_position{root_x[k], root_y[k], 0.0};
            world[0] = root_rotation;
            place[0] = root_position;
            const double hip_height =
                ((posed_at(k, left.thigh).z + posed_at(k, right.thigh).z) / 2.0 - ground) * scale + henry_low +
                upleg_offset;
            const Vec3 hips_place = Vec3{path_x[k], path_y[k], 0.0} +
                                    rotate(about_axis(henry_up, root_yaw[k]), hips_offset) + Vec3{0.0, 0.0, hip_height};
            for (std::size_t b = 1; b < bone_count; ++b)
            {
                const auto p = static_cast<std::size_t>(skeleton[b].parent);
                if (const auto fixed = leg_fix[k].find(b); fixed != leg_fix[k].end())
                {
                    world[b] = fixed->second;
                }
                else if (drive_of[b] >= 0)
                {
                    world[b] = qnormalized(delta[static_cast<std::size_t>(drive_of[b])][k] * bind_rotation[b]);
                }
                else
                {
                    world[b] = world[p] * hands_rotation[b].value_or(local_rotation[b]);
                }
                if (twist_share[b].fraction > 0.0)
                {
                    // A forearm roll bone takes its share of the hand's twist about the forearm, by where it sits along
                    // it, so the skin weighted to it turns gradually instead of all at the wrist.
                    const auto hand = static_cast<std::size_t>(twist_share[b].hand);
                    const Quat hand_world =
                        drive_of[hand] >= 0
                            ? qnormalized(delta[static_cast<std::size_t>(drive_of[hand])][k] * bind_rotation[hand])
                            : world[p] * local_rotation[hand];
                    const Quat held = world[p] * local_rotation[hand];
                    const Vec3 axis = normalized(rotate(world[p], local_position[hand]));
                    const Quat turn = conjugate(held) * hand_world;
                    const double along = dot(Vec3{turn.x, turn.y, turn.z}, rotate(conjugate(held), axis));
                    double angle = 2.0 * std::atan2(along, turn.w);
                    angle = std::remainder(angle, 2.0 * std::numbers::pi);
                    world[b] = about_axis(axis, twist_share[b].fraction * angle) * world[b];
                }
                place[b] = b == hips_bone ? hips_place : place[p] + rotate(world[p], local_position[b]);
            }
        };

        // Foot lock: while the heel or the ball of a foot is planted, that point holds one place on the ground and the
        // leg reaches for it (two-bone IK on the thigh and shin, the knee kept in its own bend plane, the foot's own
        // turn untouched), eased in and out. A hold on the ankle instead fights the foot's roll from heel to toe and
        // jerks the knees.
        std::size_t locked_frames = 0;
        if (options.lock_feet && options.root != RootMotion::InPlace && frames >= LOCK_MIN_FRAMES)
        {
            // Where the heels and balls of the feet go as retargeted, before the lock.
            std::array<std::array<std::vector<Vec3>, 2>, 2> contact_track; // [side][heel, ball][frame]
            std::array<std::array<int, 2>, 2> contact_bone{};
            for (int side = LEFT; side <= RIGHT; ++side)
            {
                contact_bone[static_cast<std::size_t>(side)] = {
                    bone(sided(side, "Heel")),
                    bone(sided(side, "ToeBase"))
                };
                for (auto &track : contact_track[static_cast<std::size_t>(side)])
                {
                    track.resize(frames);
                }
            }
            for (std::size_t k = 0; k < frames; ++k)
            {
                pose_frame(k);
                for (std::size_t side = 0; side < 2; ++side)
                {
                    for (std::size_t c = 0; c < 2; ++c)
                    {
                        const int b = contact_bone[side][c];
                        contact_track[side][c][k] =
                            b >= 0 ? place[static_cast<std::size_t>(b)] : place[side == 0 ? left_ankle : right_ankle];
                    }
                }
            }
            for (int side = LEFT; side <= RIGHT; ++side)
            {
                const auto s_index = static_cast<std::size_t>(side);
                const auto thigh = static_cast<std::size_t>(bone(sided(side, "UpLeg")));
                const auto shin = static_cast<std::size_t>(bone(sided(side, "Leg")));
                const auto ankle = static_cast<std::size_t>(bone(sided(side, "Foot")));
                // Per contact point: how firmly it is held at each frame, and where.
                std::array<std::vector<double>, 2> weight{
                    std::vector<double>(frames, 0.0),
                    std::vector<double>(frames, 0.0)
                };
                std::array<std::vector<Vec3>, 2> hold{std::vector<Vec3>(frames), std::vector<Vec3>(frames)};
                for (std::size_t c = 0; c < 2; ++c)
                {
                    const std::vector<Vec3> &track = contact_track[s_index][c];
                    double lowest = track.front().z;
                    for (const Vec3 &p : track)
                    {
                        lowest = std::min(lowest, p.z);
                    }
                    std::vector<bool> planted(frames, false);
                    for (std::size_t k = 0; k < frames; ++k)
                    {
                        const std::size_t a = k > 0 ? k - 1 : k;
                        const std::size_t b = k + 1 < frames ? k + 1 : k;
                        const Vec3 step = track[b] - track[a];
                        const double speed = std::hypot(step.x, step.y) * CLIP_FRAME_RATE /
                                             static_cast<double>(std::max<std::size_t>(b - a, 1));
                        planted[k] = track[k].z - lowest < LOCK_HEIGHT && speed < LOCK_SPEED;
                    }
                    for (std::size_t k = 0; k < frames;)
                    {
                        if (!planted[k])
                        {
                            ++k;
                            continue;
                        }
                        std::size_t last = k;
                        while (last + 1 < frames && planted[last + 1])
                        {
                            ++last;
                        }
                        if (last - k + 1 >= LOCK_MIN_FRAMES)
                        {
                            Vec3 mean{};
                            for (std::size_t i = k; i <= last; ++i)
                            {
                                mean = mean + track[i];
                            }
                            mean = mean * (1.0 / static_cast<double>(last - k + 1));
                            const std::size_t first_eased = k >= LOCK_EASE ? k - LOCK_EASE : 0;
                            const std::size_t last_eased = std::min(last + LOCK_EASE, frames - 1);
                            for (std::size_t i = first_eased; i <= last_eased; ++i)
                            {
                                const double outside = i < k      ? static_cast<double>(k - i)
                                                       : i > last ? static_cast<double>(i - last)
                                                                  : 0.0;
                                const double x = 1.0 - outside / static_cast<double>(LOCK_EASE + 1);
                                const double w = x * x * (3.0 - 2.0 * x);
                                if (w > weight[c][i])
                                {
                                    weight[c][i] = w;
                                    hold[c][i] = mean;
                                }
                            }
                        }
                        k = last + 1;
                    }
                }
                // The ankle's pull back to the held place at each frame (both points held: their weighted mean, scaled
                // to the stronger hold), smoothed so the switch from heel to ball and the ease in and out stay gentle.
                std::vector<double> pull_x(frames, 0.0), pull_y(frames, 0.0);
                for (std::size_t k = 0; k < frames; ++k)
                {
                    Vec3 pull{};
                    double total = 0.0;
                    for (std::size_t c = 0; c < 2; ++c)
                    {
                        const double w = weight[c][k];
                        if (w <= 0.0)
                        {
                            continue;
                        }
                        const Vec3 off = hold[c][k] - contact_track[s_index][c][k];
                        pull = pull + Vec3{off.x, off.y, 0.0} * w;
                        total += w;
                    }
                    if (total <= 0.0)
                    {
                        continue;
                    }
                    pull = pull * (std::max(weight[0][k], weight[1][k]) / total);
                    pull_x[k] = pull.x;
                    pull_y[k] = pull.y;
                }
                pull_x = smoothed(pull_x, LOCK_SMOOTHING);
                pull_y = smoothed(pull_y, LOCK_SMOOTHING);
                for (std::size_t k = 0; k < frames; ++k)
                {
                    const Vec3 pull{pull_x[k], pull_y[k], 0.0};
                    if (length(pull) < 1e-5)
                    {
                        continue; // already there (a straight leg otherwise bends at the reach limit)
                    }
                    ++locked_frames;
                    pose_frame(k);
                    const Vec3 hip = place[thigh];
                    const Vec3 knee = place[shin];
                    const Vec3 foot = place[ankle];
                    const Vec3 target = foot + pull;
                    const double upper = length(knee - hip);
                    const double lower = length(foot - knee);
                    const Vec3 reach = target - hip;
                    const double d = std::clamp(length(reach), std::abs(upper - lower) + 1e-4, upper + lower - 1e-4);
                    const Vec3 u = normalized(reach);
                    Vec3 bend = (knee - hip) - u * dot(knee - hip, u);
                    if (length(bend) < 1e-6)
                    {
                        bend = henry_forward;
                    }
                    bend = normalized(bend);
                    const double cos_hip =
                        std::clamp((upper * upper + d * d - lower * lower) / (2.0 * upper * d), -1.0, 1.0);
                    const Vec3 new_knee =
                        hip + u * (upper * cos_hip) + bend * (upper * std::sqrt(1.0 - cos_hip * cos_hip));
                    const Vec3 new_foot = hip + u * d;
                    const Quat turn_thigh = from_to(knee - hip, new_knee - hip);
                    const Quat turn_shin = from_to(rotate(turn_thigh, foot - knee), new_foot - new_knee);
                    leg_fix[k][thigh] = qnormalized(turn_thigh * world[thigh]);
                    leg_fix[k][shin] = qnormalized(turn_shin * turn_thigh * world[shin]);
                }
            }
        }
        report.locked_frames = locked_frames;

        // The final pose, each bone relative to its parent, and the ankles' paths for the report.
        std::vector<Vec3> left_track(frames), right_track(frames);
        for (std::size_t k = 0; k < frames; ++k)
        {
            pose_frame(k);
            left_track[k] = place[left_ankle];
            right_track[k] = place[right_ankle];
            for (std::size_t b = 0; b < bone_count; ++b)
            {
                SampledTrack &track = clip.tracks[b];
                const int parent = skeleton[b].parent;
                Quat rotation = parent < 0 ? world[b] : conjugate(world[static_cast<std::size_t>(parent)]) * world[b];
                Vec3 position = parent < 0 ? place[b]
                                           : rotate(
                                                 conjugate(world[static_cast<std::size_t>(parent)]),
                                                 place[b] - place[static_cast<std::size_t>(parent)]
                                             );
                const auto follow = [&](int target, int followed) -> void
                {
                    if (static_cast<int>(b) == target && followed >= 0)
                    {
                        const auto f = static_cast<std::size_t>(followed);
                        rotation = conjugate(world[0]) * world[f];
                        position = rotate(conjugate(world[0]), place[f] - place[0]);
                    }
                };
                follow(spine_target, spine_top);
                for (const auto &[target, hand] : hand_targets)
                {
                    follow(target, hand);
                }
                if (skeleton[b].name.ends_with("_IKBlend"))
                {
                    rotation = local_rotation[b];
                    position = {};
                }
                if (k > 0 && qdot(rotation, track.rotations[k - 1]) < 0.0)
                {
                    rotation = negated(rotation);
                }
                track.rotations[k] = qnormalized(rotation);
                if (keyed_position[b])
                {
                    track.positions[k] = position;
                }
            }
        }

        // The report.
        // A blend with one source (a chain's end, Spine4 under a neckless source's Neck) names that source's joint. A
        // blend only refers to drives made before it, so the walk ends.
        const auto joint_name = [&](int drive_index) -> std::string
        {
            const Drive *drive = &drives[static_cast<std::size_t>(drive_index)];
            while (drive->joint < 0 && (drive->a == drive->b || drive->t <= 0.0))
            {
                drive = &drives[static_cast<std::size_t>(drive->a)];
            }
            return drive->joint >= 0 ? source.joints[static_cast<std::size_t>(drive->joint)].name : std::string{"?"};
        };
        for (std::size_t b = 0; b < bone_count; ++b)
        {
            const int d = drive_of[b];
            if (d < 0)
            {
                continue;
            }
            const Drive &drive = drives[static_cast<std::size_t>(d)];
            const bool two_sources = drive.joint < 0 && drive.a != drive.b && drive.t > 0.0;
            std::string from_text = two_sources ? std::format(
                                                      "{} {:.0f}% / {} {:.0f}%",
                                                      joint_name(drive.a),
                                                      (1.0 - drive.t) * 100.0,
                                                      joint_name(drive.b),
                                                      drive.t * 100.0
                                                  )
                                                : joint_name(d);
            report.mapping.emplace_back(skeleton[b].name, std::move(from_text));
        }
        for (int side = LEFT; side <= RIGHT; ++side)
        {
            const SideRoles &r = roles.sides[static_cast<std::size_t>(side)];
            const std::string_view which = side == LEFT ? "left" : "right";
            if (r.collar < 0)
            {
                report.notes.push_back(std::format("no {} collar bone: Henry's keeps its default pose", which));
            }
            if (r.toe < 0)
            {
                report.notes.push_back(std::format("no {} toe: Henry's toes keep their default pose", which));
            }
            if (std::ranges::all_of(r.fingers, [](const auto &f) { return f[0] < 0; }))
            {
                report.notes.push_back(
                    std::format(
                        "no {} fingers: {}",
                        which,
                        hands != nullptr ? "the hands clip's pose" : "the default pose"
                    )
                );
            }
        }
        if (roles.neck.empty())
        {
            report.notes.push_back("no neck: Henry's neck turns between the chest and the head");
        }
        const auto foot_report = [&](const std::vector<Vec3> &track, std::size_t ankle) -> RetargetFoot
        {
            RetargetFoot foot;
            double lowest = track.front().z;
            for (const Vec3 &p : track)
            {
                lowest = std::min(lowest, p.z);
            }
            foot.lowest = lowest - bind_position[ankle].z;
            std::vector<double> speeds;
            for (std::size_t k = 0; k + 1 < frames; ++k)
            {
                if (track[k].z - lowest < PLANTED_HEIGHT && track[k + 1].z - lowest < PLANTED_HEIGHT)
                {
                    const Vec3 step = track[k + 1] - track[k];
                    speeds.push_back(std::hypot(step.x, step.y) * CLIP_FRAME_RATE);
                }
            }
            foot.planted_slide = median(std::move(speeds));
            return foot;
        };
        report.left_foot = foot_report(left_track, left_ankle);
        report.right_foot = foot_report(right_track, right_ankle);
        report.source_fps = source.fps;
        report.seconds = static_cast<double>(frames - 1) / CLIP_FRAME_RATE;
        report.scale = scale;
        return RetargetResult{.clip = std::move(clip), .report = std::move(report)};
    }
} // namespace AnimationFramework
