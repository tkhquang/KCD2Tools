/**
 * @file game/lip_drive.cpp
 * @brief The pose-morph Execute hook and the per-frame weight publisher.
 *
 * Execute runs on animation worker threads, so it only reads an immutable snapshot published by the main thread. The
 * snapshot maps a character instance to its jaw and lip weights; characters not in it pass through untouched.
 */

#include "game/lip_drive.hpp"
#include "game/game_clock.hpp"
#include "constants.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <string_view>

namespace SoundFramework::game::lip_drive
{
    namespace
    {
        using ExecuteFn = bool(__fastcall *)(void *self, void *const *params);
        using GetEntityFn = void *(__fastcall *)(void *system, std::uint32_t id);
        using GetCharacterFn = void *(__fastcall *)(void *entity, int slot);
        using GetSkeletonFn = void *(__fastcall *)(void *character);
        using JointIdFn = std::int16_t(__fastcall *)(void *skeleton, std::uint32_t crc);
        using JointTranslationFn = float *(__fastcall *)(void *pose, std::int32_t joint);

        /**
         * @brief CRC32 (reflected 0xEDB88320) of a joint name in lower case: the skeleton's lookup by CRC compares
         *        lower-case CRCs (animation clips use the case-sensitive one).
         */
        [[nodiscard]] constexpr std::uint32_t joint_crc(std::string_view name) noexcept
        {
            std::uint32_t crc = 0xFFFFFFFFu;
            for (const char raw : name)
            {
                const char c = raw >= 'A' && raw <= 'Z' ? static_cast<char>(raw - 'A' + 'a') : raw;
                crc ^= static_cast<std::uint8_t>(c);
                for (int bit = 0; bit < 8; ++bit)
                {
                    crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
                }
            }
            return ~crc;
        }
        static_assert(joint_crc("FA_POSE_C_jawDrop") == 0xB6875729u);

        struct PoseBone
        {
            std::uint32_t crc;
            float audio::MouthPose::*channel;
            /// Weight at a channel value of 1.
            float scale;
        };

        constexpr std::array POSE_BONES = {
            PoseBone{joint_crc("FA_POSE_C_jawDrop"), &audio::MouthPose::jaw, 0.9f},
            PoseBone{joint_crc("FA_POSE_C_lipPart"), &audio::MouthPose::lips, 0.7f},
            PoseBone{joint_crc("FA_POSE_C_O"), &audio::MouthPose::round_open, 0.8f},
            PoseBone{joint_crc("FA_POSE_C_U"), &audio::MouthPose::round_closed, 0.8f},
            PoseBone{joint_crc("FA_POSE_L_cornerPull"), &audio::MouthPose::spread, 0.6f},
            PoseBone{joint_crc("FA_POSE_R_cornerPull"), &audio::MouthPose::spread, 0.6f},
            PoseBone{joint_crc("FA_POSE_L_showTeeth"), &audio::MouthPose::teeth, 0.5f},
            PoseBone{joint_crc("FA_POSE_R_showTeeth"), &audio::MouthPose::teeth, 0.5f},
        };

        constexpr std::uint64_t MOOD_FADE_IN_MS = 250;
        constexpr std::uint64_t MOOD_FADE_OUT_MS = 450;

        struct MoodBone
        {
            std::uint32_t crc;
            float weight;
        };

        struct Line
        {
            std::uint64_t ticket = 0;
            std::uint32_t entity = 0;
            /// Time into the line on the voice clock (game_clock), negative during the performance's delay.
            double clock_ms = 0.0;
            std::uint32_t step_ms = 20;
            std::uint32_t duration_ms = 0;
            std::vector<audio::MouthPose> track;
            audio::PoseTrack poses;
            std::vector<MoodBone> mood;
            /// The sound stopped: no more mouth parts.
            bool ended = false;
            /// Held for its sound: its clock does not run until sound_started().
            bool waiting = false;
            std::uint32_t delay_ms = 0;
            /// Performance::replace_mouth.
            bool replace_mouth = false;
            /// Performance::mouth_pending until the first part of the mouth arrives.
            bool awaiting_mouth = false;
        };

        struct Target
        {
            const void *character = nullptr;
            /// Clear the articulation poses before writing this frame's weights.
            bool replace_mouth = false;
            bool has_mouth = false;
            audio::MouthPose pose;
            /// Learned channel weights for this frame; empty without a learned track.
            std::vector<float> channels;
            /// Mood weights for this frame, faded and scaled.
            std::vector<MoodBone> mood;
        };

        using Snapshot = std::vector<Target>;

        Anchors s_anchors{};
        /// Joint CRCs per learned channel; written before the hook is armed, read-only afterwards.
        std::vector<std::vector<std::uint32_t>> s_channel_joints;
        /// Joint CRCs of the articulation poses; written before the hook is armed, read-only afterwards.
        std::vector<std::uint32_t> s_articulation_joints;
        ExecuteFn s_original = nullptr;
        std::atomic<std::shared_ptr<const Snapshot>> s_snapshot;
        /// Main-thread state.
        std::vector<Line> s_lines;
        std::uint64_t s_next_ticket = 1;
        bool s_published_any = false;

        [[nodiscard]] void *const *vtable_of(const void *object) noexcept
        {
            return *static_cast<void *const *const *>(object);
        }

        [[nodiscard]] void *character_of(std::uint32_t entity_id) noexcept
        {
            const auto *genv = reinterpret_cast<const std::uint8_t *>(s_anchors.genv);
            void *const system = *reinterpret_cast<void *const *>(genv + constants::GENV_ENTITY_SYSTEM);
            if (system == nullptr ||
                reinterpret_cast<std::uintptr_t>(vtable_of(system)) != s_anchors.entity_system_vtable)
            {
                return nullptr;
            }
            void *const entity = reinterpret_cast<GetEntityFn>(vtable_of(
                system
            )[constants::ENTITY_SYSTEM_GET_ENTITY_SLOT])(system, entity_id);
            if (entity == nullptr || reinterpret_cast<std::uintptr_t>(vtable_of(entity)) != s_anchors.entity_vtable)
            {
                return nullptr;
            }
            void *const character =
                reinterpret_cast<GetCharacterFn>(vtable_of(entity)[constants::ENTITY_GET_CHARACTER_SLOT])(entity, 0);
            if (character == nullptr ||
                reinterpret_cast<std::uintptr_t>(vtable_of(character)) != s_anchors.character_vtable)
            {
                return nullptr;
            }
            return character;
        }

        /** @brief Logs the first object of an unexpected type the morph hook meets (worker thread, once). */
        void report_unexpected(const char *what, const void *object) noexcept
        {
            static std::atomic_flag reported;
            if (reported.test_and_set(std::memory_order_relaxed))
            {
                return;
            }
            (void)DMK::log().try_log(
                DMK::LogLevel::Warning,
                "LipDrive: {} vtable {:#x} is not the anchored one; that face is left alone",
                what,
                reinterpret_cast<std::uintptr_t>(vtable_of(object))
            );
        }

        bool __fastcall execute_detour(void *self, void *const *params) noexcept
        {
            const std::shared_ptr<const Snapshot> snapshot = s_snapshot.load(std::memory_order_acquire);
            if (snapshot != nullptr && params != nullptr && params[0] != nullptr && params[1] != nullptr)
            {
                const auto target = std::ranges::find(*snapshot, params[0], &Target::character);
                if (target != snapshot->end())
                {
                    void *const character = params[0];
                    void *const pose = params[1];
                    void *const skeleton =
                        reinterpret_cast<GetSkeletonFn>(vtable_of(character)[constants::CHARACTER_GET_SKELETON_SLOT])(
                            character
                        );
                    const auto weight_of = [&](std::uint32_t crc) -> float *
                    {
                        const std::int16_t id = reinterpret_cast<JointIdFn>(vtable_of(
                            skeleton
                        )[constants::SKELETON_JOINT_BY_CRC_SLOT])(skeleton, crc);
                        return id < 0 ? nullptr
                                      : reinterpret_cast<JointTranslationFn>(vtable_of(
                                            pose
                                        )[constants::POSE_JOINT_TRANSLATION_SLOT])(pose, id);
                    };
                    const auto apply = [&](std::uint32_t crc, float value)
                    {
                        if (float *const weight = weight_of(crc); weight != nullptr)
                        {
                            *weight = std::max(*weight, value);
                        }
                    };
                    if (reinterpret_cast<std::uintptr_t>(vtable_of(pose)) != s_anchors.pose_data_vtable)
                    {
                        report_unexpected("pose data", pose);
                    }
                    else if (skeleton != nullptr &&
                             reinterpret_cast<std::uintptr_t>(vtable_of(skeleton)) != s_anchors.skeleton_vtable)
                    {
                        report_unexpected("skeleton", skeleton);
                    }
                    else if (skeleton != nullptr)
                    {
                        if (target->replace_mouth)
                        {
                            for (const std::uint32_t crc : s_articulation_joints)
                            {
                                if (float *const weight = weight_of(crc); weight != nullptr)
                                {
                                    *weight = 0.0f;
                                }
                            }
                        }
                        if (target->has_mouth)
                        {
                            for (const PoseBone &bone : POSE_BONES)
                            {
                                apply(bone.crc, bone.scale * (target->pose.*bone.channel));
                            }
                        }
                        const std::size_t channels = std::min(target->channels.size(), s_channel_joints.size());
                        for (std::size_t c = 0; c < channels; ++c)
                        {
                            for (const std::uint32_t crc : s_channel_joints[c])
                            {
                                apply(crc, target->channels[c]);
                            }
                        }
                        for (const MoodBone &bone : target->mood)
                        {
                            apply(bone.crc, bone.weight);
                        }
                    }
                }
            }
            return s_original(self, params);
        }
    } // namespace

    DMK::Result<void> start(const Anchors &anchors, DMK::hook::HookStack &hooks)
    {
        s_anchors = anchors;
        if (!anchors.lip_drive || anchors.pose_morph_execute == 0)
        {
            DMK::log().warning("LipDrive: the face anchors did not resolve; lines without lip sync keep a still face");
            return {};
        }
        s_articulation_joints.clear();
        for (const std::string_view pose : face::articulation_poses())
        {
            s_articulation_joints.push_back(joint_crc(std::string("FA_POSE_").append(pose)));
        }
        DMK_TRY(
            created,
            DMK::hook::inline_at<&execute_detour>(DMK::hook::InlineRequest{
                .name = "JointPoseMorphExecute",
                .target = DMK::Address{anchors.pose_morph_execute},
            })
        );
        s_original = created.original<ExecuteFn>();
        DMK::hook::Hook &stored = hooks.push(std::move(created));
        DMK_TRY_VOID(stored.enable());
        DMK::log().info("LipDrive: pose-morph Execute hooked");
        return {};
    }

    void set_pose_channels(const std::vector<std::vector<std::string>> &channels)
    {
        s_channel_joints.clear();
        for (const std::vector<std::string> &names : channels)
        {
            std::vector<std::uint32_t> &joints = s_channel_joints.emplace_back();
            for (const std::string &name : names)
            {
                joints.push_back(joint_crc(name));
            }
        }
    }

    std::uint64_t perform(std::uint32_t entity_id, Performance performance)
    {
        if (s_original == nullptr || (performance.mood.empty() && !performance.mouth_pending))
        {
            return 0;
        }
        Line line{
            .ticket = s_next_ticket++,
            .entity = entity_id,
            .step_ms = std::max<std::uint32_t>(1, performance.step_ms),
            .duration_ms = performance.duration_ms,
            .waiting = true,
            .delay_ms = performance.delay_ms,
            .replace_mouth = performance.replace_mouth,
            .awaiting_mouth = performance.mouth_pending,
        };
        for (const face::PoseWeight &pose : performance.mood)
        {
            line.mood.push_back(
                MoodBone{
                    joint_crc(std::string("FA_POSE_") + pose.pose),
                    std::min(pose.weight * performance.mood_strength, face::MAX_POSE_WEIGHT),
                }
            );
        }
        const std::uint64_t ticket = line.ticket;
        s_lines.push_back(std::move(line));
        return ticket;
    }

    void attach_poses(std::uint64_t ticket, std::size_t first_step, std::size_t total_steps, audio::PoseTrack poses)
    {
        const auto line = std::ranges::find(s_lines, ticket, &Line::ticket);
        if (line == s_lines.end() || line->ended || first_step != line->poses.frames())
        {
            return;
        }
        line->awaiting_mouth = false;
        line->duration_ms =
            std::max<std::uint32_t>(line->duration_ms, static_cast<std::uint32_t>(total_steps) * line->step_ms);
        if (first_step == 0)
        {
            line->poses = std::move(poses);
        }
        else if (poses.channels == line->poses.channels)
        {
            line->poses.weights.insert(line->poses.weights.end(), poses.weights.begin(), poses.weights.end());
        }
    }

    void attach_track(std::uint64_t ticket, std::vector<audio::MouthPose> track)
    {
        const auto line = std::ranges::find(s_lines, ticket, &Line::ticket);
        if (line == s_lines.end() || line->ended)
        {
            return;
        }
        line->awaiting_mouth = false;
        line->duration_ms =
            std::max<std::uint32_t>(line->duration_ms, static_cast<std::uint32_t>(track.size()) * line->step_ms);
        line->track = std::move(track);
    }

    void sound_started(std::uint64_t ticket)
    {
        const auto line = std::ranges::find(s_lines, ticket, &Line::ticket);
        if (line == s_lines.end() || !line->waiting)
        {
            return;
        }
        line->clock_ms = -static_cast<double>(line->delay_ms);
        line->waiting = false;
        // The speaker plays one line at a time: this one replaces whatever ran before it.
        const std::uint32_t entity = line->entity;
        std::erase_if(s_lines, [&](const Line &other) { return other.entity == entity && other.ticket != ticket; });
    }

    void end_line(std::uint64_t ticket)
    {
        const auto line = std::ranges::find(s_lines, ticket, &Line::ticket);
        if (line == s_lines.end() || line->ended)
        {
            return;
        }
        if (line->waiting)
        {
            // Never started: the per-frame pass drops it once its mood (if any) has faded.
            line->clock_ms = 0.0;
            line->waiting = false;
        }
        line->duration_ms = line->clock_ms > 0.0 ? static_cast<std::uint32_t>(line->clock_ms) : 0;
        line->track.clear();
        line->poses.weights.clear();
        line->ended = true;
        line->awaiting_mouth = false;
    }

    bool active() noexcept
    {
        return s_original != nullptr;
    }

    void on_frame() noexcept
    {
        if (s_lines.empty())
        {
            if (s_published_any)
            {
                s_snapshot.store(nullptr, std::memory_order_release);
                s_published_any = false;
            }
            return;
        }
        try
        {
            const double step_ms = game_clock::frame_ms();
            auto snapshot = std::make_shared<Snapshot>();
            std::erase_if(
                s_lines,
                [&](Line &line)
                {
                    if (line.waiting)
                    {
                        return false;
                    }
                    line.clock_ms += step_ms;
                    if (line.clock_ms < 0.0)
                    {
                        return false;
                    }
                    const auto elapsed = static_cast<std::uint64_t>(line.clock_ms);
                    // A line whose length comes with its mouth has no length before the first part arrives. The end of
                    // its sound (end_line) bounds it until then.
                    if (!line.awaiting_mouth && elapsed >= line.duration_ms + MOOD_FADE_OUT_MS)
                    {
                        return true;
                    }
                    void *const character = character_of(line.entity);
                    if (character == nullptr)
                    {
                        return false;
                    }
                    Target target{
                        .character = character,
                        .replace_mouth = line.replace_mouth,
                    };
                    const std::size_t index = static_cast<std::size_t>(elapsed / line.step_ms);
                    if (index < line.track.size())
                    {
                        target.has_mouth = true;
                        target.pose = line.track[index];
                    }
                    if (index < line.poses.frames())
                    {
                        const auto row =
                            line.poses.weights.begin() + static_cast<std::ptrdiff_t>(index * line.poses.channels);
                        target.channels.assign(row, row + static_cast<std::ptrdiff_t>(line.poses.channels));
                    }
                    if (!line.mood.empty())
                    {
                        float fade = std::min(1.0f, static_cast<float>(elapsed) / MOOD_FADE_IN_MS);
                        if (elapsed > line.duration_ms)
                        {
                            fade *= 1.0f - static_cast<float>(elapsed - line.duration_ms) / MOOD_FADE_OUT_MS;
                        }
                        for (const MoodBone &bone : line.mood)
                        {
                            target.mood.push_back(MoodBone{bone.crc, bone.weight * std::max(0.0f, fade)});
                        }
                    }
                    snapshot->push_back(std::move(target));
                    return false;
                }
            );
            s_snapshot.store(std::move(snapshot), std::memory_order_release);
            s_published_any = true;
        }
        catch (...)
        {
            // The next frame publishes nothing, so no face keeps a stale pose.
            s_lines.clear();
            (void)DMK::log().log_noexcept(DMK::LogLevel::Error, "LipDrive: the face update failed; its lines stop");
        }
    }

    void clear() noexcept
    {
        s_lines.clear();
        s_snapshot.store(nullptr, std::memory_order_release);
        s_published_any = false;
    }
} // namespace SoundFramework::game::lip_drive
