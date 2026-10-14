/**
 * @file dev/query_ledger.cpp
 * @brief Implementation of the query ledger (see query_ledger.hpp). Empty outside DEV profiling builds.
 */

#include "query_ledger.hpp"

#if defined(TPVCAMERA_DEV_BUILD) && defined(DMK_ENABLE_PROFILING)
#include <algorithm>
#include <atomic>
#include <cmath>
#include <windows.h>

namespace TPVCamera::QueryLedger
{
    namespace
    {
        // A camera that moved less than this (meters) and turned less than this (cosine of 0.05 degrees) since the
        // previous frame stood still on it.
        constexpr float k_still_move = 1e-3f;
        constexpr float k_still_turn_cos = 0.99999962f;

        struct AtomicStats
        {
            std::atomic<std::uint64_t> calls{0};
            std::atomic<std::int64_t> ticks{0};
            std::atomic<std::int64_t> max_ticks{0};
        };

        std::array<std::array<AtomicStats, KIND_COUNT>, TAG_COUNT> s_totals{};
        // The physics queries and their ticks since the last close_frame(). A launch trace on a job worker adds to
        // whichever frame is open; the frame boundary is only the main thread's.
        std::atomic<std::uint32_t> s_frame_physics{0};
        std::atomic<std::int64_t> s_frame_physics_ticks{0};

        // Main thread only: written by note_view() and close_frame().
        std::array<std::uint64_t, MOTION_COUNT> s_frames{};
        std::array<std::array<std::uint64_t, FRAME_QUERY_BINS>, MOTION_COUNT> s_physics_per_frame{};
        std::array<std::int64_t, MOTION_COUNT> s_max_physics_ticks{};
        ViewPose s_last_pose{};
        bool s_last_pose_valid = false;
        Motion s_open_motion = Motion::Idle;

        void raise_max(std::atomic<std::int64_t> &slot, std::int64_t value) noexcept
        {
            std::int64_t seen = slot.load(std::memory_order_relaxed);
            while (value > seen && !slot.compare_exchange_weak(seen, value, std::memory_order_relaxed))
            {
            }
        }
    } // namespace

    std::int64_t begin() noexcept
    {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return now.QuadPart;
    }

    void end(Tag tag, Kind kind, std::int64_t started, std::uint32_t calls) noexcept
    {
        const auto tag_index = static_cast<std::size_t>(tag);
        const auto kind_index = static_cast<std::size_t>(kind);
        if (tag_index >= TAG_COUNT || kind_index >= KIND_COUNT || calls == 0)
        {
            return;
        }
        const std::int64_t elapsed = std::max<std::int64_t>(0, begin() - started);
        AtomicStats &stats = s_totals[tag_index][kind_index];
        stats.calls.fetch_add(calls, std::memory_order_relaxed);
        stats.ticks.fetch_add(elapsed, std::memory_order_relaxed);
        raise_max(stats.max_ticks, elapsed);
        if (kind == Kind::Physics)
        {
            s_frame_physics.fetch_add(calls, std::memory_order_relaxed);
            s_frame_physics_ticks.fetch_add(elapsed, std::memory_order_relaxed);
        }
    }

    void note_view(const ViewPose &pose) noexcept
    {
        bool still = false;
        if (s_last_pose_valid)
        {
            float moved2 = 0.0f;
            float turn = 0.0f;
            float length2 = 0.0f;
            float last_length2 = 0.0f;
            for (std::size_t i = 0; i < 3; ++i)
            {
                const float d = pose.position[i] - s_last_pose.position[i];
                moved2 += d * d;
                turn += pose.forward[i] * s_last_pose.forward[i];
                length2 += pose.forward[i] * pose.forward[i];
                last_length2 += s_last_pose.forward[i] * s_last_pose.forward[i];
            }
            const float lengths = std::sqrt(length2 * last_length2);
            still = moved2 < k_still_move * k_still_move && lengths > 0.0f && turn >= k_still_turn_cos * lengths;
        }
        s_last_pose = pose;
        s_last_pose_valid = true;
        s_open_motion = still ? Motion::Still : Motion::Moving;
    }

    void close_frame() noexcept
    {
        const std::uint32_t physics = s_frame_physics.exchange(0, std::memory_order_relaxed);
        const std::int64_t ticks = s_frame_physics_ticks.exchange(0, std::memory_order_relaxed);
        const auto motion = static_cast<std::size_t>(s_open_motion);
        ++s_frames[motion];
        ++s_physics_per_frame[motion][std::min<std::size_t>(physics, FRAME_QUERY_BINS - 1)];
        s_max_physics_ticks[motion] = std::max(s_max_physics_ticks[motion], ticks);
        // A frame that renders no third-person camera never notes a pose, and the next one compares against the pose
        // before the gap only if it is again a third-person frame.
        if (s_open_motion == Motion::Idle)
        {
            s_last_pose_valid = false;
        }
        s_open_motion = Motion::Idle;
    }

    Snapshot snapshot() noexcept
    {
        Snapshot result{};
        for (std::size_t t = 0; t < TAG_COUNT; ++t)
        {
            for (std::size_t k = 0; k < KIND_COUNT; ++k)
            {
                const AtomicStats &stats = s_totals[t][k];
                result.totals[t][k] = TagStats{
                    .calls = stats.calls.load(std::memory_order_relaxed),
                    .ticks = stats.ticks.load(std::memory_order_relaxed),
                    .max_ticks = stats.max_ticks.load(std::memory_order_relaxed),
                };
            }
        }
        result.frames = s_frames;
        result.physics_per_frame = s_physics_per_frame;
        result.max_physics_ticks_per_frame = s_max_physics_ticks;
        return result;
    }
} // namespace TPVCamera::QueryLedger
#endif
