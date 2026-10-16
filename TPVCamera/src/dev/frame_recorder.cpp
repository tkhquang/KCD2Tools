/**
 * @file dev/frame_recorder.cpp
 * @brief DEV recording of the camera frames (see frame_recorder.hpp).
 */

#include "dev/frame_recorder.hpp"

#include "config.hpp"
#include "core/collision/probe_record.hpp"
#include "core/rig/rig_record.hpp"

#include <DetourModKit.hpp>

#include <cstddef>
#include <cstdio>
#include <deque>
#include <vector>

namespace TPVCamera::FrameRecorder
{
    namespace
    {
        // The ring keeps the newest frames up to this many bytes: about a minute of play at 60 frames a second with
        // the rig, the arm and the shoulder recorded each frame.
        constexpr std::size_t k_ring_bytes = std::size_t{48} << 20;

        // Main thread only (the camera frame); the export runs after every hook retired.
        std::deque<std::vector<std::byte>> s_frames;
        std::size_t s_bytes = 0;
        bool s_announced = false;

        /** @brief Appends a recorded frame and drops the oldest ones past k_ring_bytes. */
        void keep(std::vector<std::byte> &&frame)
        {
            if (!s_announced)
            {
                s_announced = true;
                (void)DMK::log().try_log(DMK::LogLevel::Info,
                                         "[DEV] Recording the camera frames (RecordFrames); they are written at the "
                                         "next reload");
            }
            s_bytes += frame.size();
            s_frames.push_back(std::move(frame));
            while (s_bytes > k_ring_bytes && s_frames.size() > 1)
            {
                s_bytes -= s_frames.front().size();
                s_frames.pop_front();
            }
        }
    } // namespace

    bool recording() noexcept
    {
        return settings().record_frames.load(std::memory_order_relaxed);
    }

    bool rig(RigState &state, const RigParams &params, const RigInput &in, RigPort &port, RigFrame &frame)
    {
        std::vector<std::byte> frame_bytes;
        frame_bytes.reserve(2048);
        const bool built = ProbeRecord::record_rig(frame_bytes, state, params, in, port, frame);
        keep(std::move(frame_bytes));
        return built;
    }

    Vector3 arm(CollisionState &state, const CollisionInput &in, const CollisionParams &params, WorldProbe &probe,
                CollisionTrace &trace)
    {
        std::vector<std::byte> frame;
        frame.reserve(8192);
        const Vector3 camera = ProbeRecord::record_arm(frame, state, in, params, probe, trace);
        keep(std::move(frame));
        return camera;
    }

    float shoulder(CollisionState &state, const ShoulderInput &in, const CollisionParams &params, WorldProbe &probe,
                   CollisionTrace &trace)
    {
        std::vector<std::byte> frame;
        frame.reserve(1024);
        const float allowed = ProbeRecord::record_shoulder(frame, state, in, params, probe, trace);
        keep(std::move(frame));
        return allowed;
    }

    std::optional<float> lens(LensState &state, const LensInput &in, WorldProbe &probe)
    {
        std::vector<std::byte> frame;
        frame.reserve(2048);
        const std::optional<float> near = ProbeRecord::record_lens(frame, state, in, probe);
        keep(std::move(frame));
        return near;
    }

    bool export_after_rundown(const char *path) noexcept
    {
        if (s_frames.empty())
        {
            return true;
        }
        std::FILE *file = nullptr;
        if (fopen_s(&file, path, "wb") != 0 || file == nullptr)
        {
            return false;
        }
        const ProbeRecord::Header header{};
        bool ok = std::fwrite(&header, sizeof(header), 1, file) == 1;
        for (const std::vector<std::byte> &frame : s_frames)
        {
            ok = ok && std::fwrite(frame.data(), 1, frame.size(), file) == frame.size();
        }
        ok = std::fclose(file) == 0 && ok;
        (void)DMK::log().try_log(ok ? DMK::LogLevel::Info : DMK::LogLevel::Warning,
                                 "[DEV] {} recorded camera frame(s), {} bytes, to {}", s_frames.size(), s_bytes, path);
        s_frames.clear();
        s_bytes = 0;
        return ok;
    }
} // namespace TPVCamera::FrameRecorder
