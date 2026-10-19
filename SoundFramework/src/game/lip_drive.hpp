/**
 * @file game/lip_drive.hpp
 * @brief Voice-driven mouth movement for lines the game has no lip-sync animation for.
 *
 * KCD2 faces are driven by pose bones (`FA_POSE_*`): each pose bone's relative translation X is a pose weight, and the
 * game's C_JointPoseMorphPoseModifier turns those weights into face joint motion. While one of the framework's lines
 * plays, the main thread publishes the current mouth pose from the line's analysis; a hook on the modifier's
 * Execute writes them into the speaker's pose bones right before the game's own morph runs.
 */
#ifndef SNDFW_GAME_LIP_DRIVE_HPP
#define SNDFW_GAME_LIP_DRIVE_HPP

#include "anchors.hpp"
#include "audio/lip_model.hpp"
#include "audio/voice_analysis.hpp"
#include "face/mood.hpp"

#include <DetourModKit.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace SoundFramework::game::lip_drive
{
    /**
     * @brief Hooks the pose-morph modifier's Execute when its anchors resolved.
     * @return An empty value, or the install error (already logged). Missing anchors only switch lip drive off.
     * @note Setup/control-plane only.
     */
    [[nodiscard]] DMK::Result<void> start(const Anchors &anchors, DMK::hook::HookStack &hooks);

    /** @brief What one line does to its speaker's face while it plays. */
    struct Performance
    {
        /**
         * @brief The line's mouth comes from its voice: attach_poses() or attach_track() adds it to the running line
         *        once the worker computed it.
         */
        bool mouth_pending = false;
        std::uint32_t step_ms = 20;
        /// The face starts moving this long from now.
        std::uint32_t delay_ms = 0;
        /// How long the line lasts; the mood fades out after it.
        std::uint32_t duration_ms = 0;
        /// Mood pose weights held for the line, already filtered to the upper face when the mouth is the game's.
        std::vector<face::PoseWeight> mood;
        float mood_strength = 1.0f;
    };

    /**
     * @brief Sets the joints each learned pose channel drives (FA_POSE names, several when poses are mirrored).
     * @note Setup/control-plane only, before start().
     */
    void set_pose_channels(const std::vector<std::vector<std::string>> &channels);

    /**
     * @brief Queues @p performance on the entity with id @p entity_id. Its face holds until sound_started(), which
     *        also ends the entity's previous line, so the face starts and changes with the line's sound.
     * @return The line's ticket for attach_poses(), or 0 when nothing started.
     * @note Main thread only.
     */
    std::uint64_t perform(std::uint32_t entity_id, Performance performance);

    /**
     * @brief Adds a part of a learned track, its rows from @p first_step on, to the running line @p ticket; dropped
     *        when that line has ended or was replaced, or when the part does not continue the rows already there.
     *        The track keeps the line's start time, so steps computed late are skipped, not delayed.
     * @note Main thread only.
     */
    void attach_poses(std::uint64_t ticket, std::size_t first_step, audio::PoseTrack poses);

    /**
     * @brief Sets the basic mouth track of the running line @p ticket; dropped when that line has ended or was
     *        replaced. The track keeps the line's start time, as attach_poses() does.
     * @note Main thread only.
     */
    void attach_track(std::uint64_t ticket, std::vector<audio::MouthPose> track);

    /**
     * @brief Starts the clock of the line @p ticket, which perform() held for its sound: the face follows the line
     *        from now on, after the performance's delay.
     * @note Main thread only.
     */
    void sound_started(std::uint64_t ticket);

    /**
     * @brief Ends the running line @p ticket now, because its sound stopped: the mouth stops at once and the mood fades
     *        out. Mouth parts that arrive later are dropped.
     * @note Main thread only.
     */
    void end_line(std::uint64_t ticket);

    /** @brief True when the pose-morph hook is installed, so voice lines move mouths. */
    [[nodiscard]] bool active() noexcept;

    /** @brief Publishes the current weights of every speaking entity. Main thread only, from the main-thread hook. */
    void on_frame() noexcept;

    /** @brief Stops every line and publishes nothing; the hook then leaves every pose untouched. */
    void clear() noexcept;
} // namespace SoundFramework::game::lip_drive

#endif // SNDFW_GAME_LIP_DRIVE_HPP
