/**
 * @file dev/frame_recorder.hpp
 * @brief DEV recording of the camera frames for the offline replay (core/collision/probe_record.hpp): while
 *        [Debug] RecordFrames is on, each camera frame's rig, collision and lens are recorded into a ring of the last
 *        k_ring_bytes, written beside the log when the generation shuts down.
 */
#ifndef TPVCAMERA_DEV_FRAME_RECORDER_HPP
#define TPVCAMERA_DEV_FRAME_RECORDER_HPP

#include "core/collision/arm.hpp"
#include "core/lens/lens_plan.hpp"
#include "core/rig/rig_stage.hpp"

#include <optional>

namespace TPVCamera::FrameRecorder
{
    /** @brief Whether [Debug] RecordFrames is on. */
    [[nodiscard]] bool recording() noexcept;

    /** @brief run_rig() on @p port, recorded into the ring. Main thread only. */
    [[nodiscard]] bool rig(RigState &state, const RigParams &params, const RigInput &in, RigPort &port,
                           RigFrame &frame);

    /** @brief resolve_camera_collision() on @p probe, recorded into the ring. Main thread only. */
    [[nodiscard]] Vector3 arm(CollisionState &state, const CollisionInput &in, const CollisionParams &params,
                              WorldProbe &probe, CollisionTrace &trace);

    /** @brief resolve_shoulder_collision() on @p probe, recorded into the ring. Main thread only. */
    [[nodiscard]] float shoulder(CollisionState &state, const ShoulderInput &in, const CollisionParams &params,
                                 WorldProbe &probe, CollisionTrace &trace);

    /** @brief plan_lens_near() on @p probe, recorded into the ring. Main thread only. */
    [[nodiscard]] std::optional<float> lens(LensState &state, const LensInput &in, WorldProbe &probe);

    /**
     * @brief Writes the recorded frames to @p path (KCD2_TPVCamera_frames_genNNNN.tpvrec), oldest first, and empties
     *        the ring. Nothing is written when nothing was recorded.
     * @note Call after every hook has retired, so no camera frame appends meanwhile.
     * @return False when the file could not be written; true otherwise, also when there was nothing to write.
     */
    [[nodiscard]] bool export_after_rundown(const char *path) noexcept;
} // namespace TPVCamera::FrameRecorder

#endif // TPVCAMERA_DEV_FRAME_RECORDER_HPP
