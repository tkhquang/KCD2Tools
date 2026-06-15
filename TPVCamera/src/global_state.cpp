/**
 * @file global_state.cpp
 * @brief Storage backing the cross-module shared-state accessors.
 */

#include "global_state.hpp"

// Stable unmangled symbol for the resolved game context pointer (see header).
extern "C"
{
    std::atomic<std::byte *> g_global_context_ptr_address{nullptr};
}

namespace TPVCamera
{
    ModuleInfo &module_info() noexcept
    {
        static ModuleInfo state;
        return state;
    }

    OverlayState &overlay_state() noexcept
    {
        static OverlayState state;
        return state;
    }

    CameraState &camera_state() noexcept
    {
        static CameraState state;
        return state;
    }

    void InteractionAimPose::store(float px, float py, float pz, float dx, float dy, float dz) noexcept
    {
        const std::uint32_t seq = m_seq.load(std::memory_order_relaxed);
        m_seq.store(seq + 1, std::memory_order_relaxed); // odd: write in progress
        // Release fence: the odd-marker store cannot be reordered after the payload stores below it, so the
        // "write in progress" sequence is established before any field changes. A reader that catches a
        // half-written payload then sees an odd (or, after the even publish, a changed) sequence on its
        // re-read and retries.
        std::atomic_thread_fence(std::memory_order_release);
        m_pos_x.store(px, std::memory_order_relaxed);
        m_pos_y.store(py, std::memory_order_relaxed);
        m_pos_z.store(pz, std::memory_order_relaxed);
        m_dir_x.store(dx, std::memory_order_relaxed);
        m_dir_y.store(dy, std::memory_order_relaxed);
        m_dir_z.store(dz, std::memory_order_relaxed);
        m_valid.store(true, std::memory_order_relaxed);
        // Release fence: orders the payload stores before the even-marker publish; it pairs with the
        // reader's acquire fence so a reader that sees the even sequence sees the whole payload.
        std::atomic_thread_fence(std::memory_order_release);
        m_seq.store(seq + 2, std::memory_order_relaxed); // even: published
    }

    bool InteractionAimPose::load(float &px, float &py, float &pz, float &dx, float &dy, float &dz) const noexcept
    {
        // Bounded seqlock read: a hook callback must not spin unboundedly, so cap the retries and fail
        // closed if the producer is mid-publish across the cap. The producer is a single thread making
        // forward progress, so in practice the first attempt succeeds.
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            const std::uint32_t before = m_seq.load(std::memory_order_acquire);
            if (before & 1u)
                continue; // write in progress
            const bool valid = m_valid.load(std::memory_order_relaxed);
            const float lpx = m_pos_x.load(std::memory_order_relaxed);
            const float lpy = m_pos_y.load(std::memory_order_relaxed);
            const float lpz = m_pos_z.load(std::memory_order_relaxed);
            const float ldx = m_dir_x.load(std::memory_order_relaxed);
            const float ldy = m_dir_y.load(std::memory_order_relaxed);
            const float ldz = m_dir_z.load(std::memory_order_relaxed);
            // Acquire fence pairs with the producer's release fences; the relaxed re-read then detects a
            // publish that overlapped the payload reads.
            std::atomic_thread_fence(std::memory_order_acquire);
            if (m_seq.load(std::memory_order_relaxed) != before)
                continue; // torn across the read; retry
            if (!valid)
                return false;
            px = lpx;
            py = lpy;
            pz = lpz;
            dx = ldx;
            dy = ldy;
            dz = ldz;
            return true;
        }
        return false;
    }

    void InteractionAimPose::invalidate() noexcept
    {
        const std::uint32_t seq = m_seq.load(std::memory_order_relaxed);
        m_seq.store(seq + 1, std::memory_order_relaxed); // odd
        std::atomic_thread_fence(std::memory_order_release);
        m_valid.store(false, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        m_seq.store(seq + 2, std::memory_order_relaxed); // even
    }

    bool InteractionAimPose::is_valid() const noexcept
    {
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            const std::uint32_t before = m_seq.load(std::memory_order_acquire);
            if (before & 1u)
                continue;
            const bool valid = m_valid.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (m_seq.load(std::memory_order_relaxed) != before)
                continue;
            return valid;
        }
        return false;
    }

    InteractionAimPose &interaction_aim_pose() noexcept
    {
        static InteractionAimPose pose;
        return pose;
    }

    std::atomic<uint32_t> &game_state_mask() noexcept
    {
        static std::atomic<uint32_t> mask{0};
        return mask;
    }

    std::atomic<bool> &game_world_ready() noexcept
    {
        static std::atomic<bool> ready{false};
        return ready;
    }

} // namespace TPVCamera
