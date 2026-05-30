/**
 * @file camera_offset_state.h
 * @brief Sequence-locked holder for the live third-person camera offset.
 */
#ifndef CAMERA_OFFSET_STATE_HPP
#define CAMERA_OFFSET_STATE_HPP

#include <atomic>
#include <cstdint>

#include "math_utils.hpp"

/**
 * @brief Lock-free, tear-free holder for the live camera offset shared between
 *        the per-frame render hook and the camera-profile system.
 * @details The third-person camera detour reads this every rendered frame, a hot
 *          path that must never block, while the camera-profile poll thread (the
 *          continuous offset adjustment) and the profile-management methods write
 *          it. A non-atomic Vector3 shared this way is a data race; taking the
 *          profile mutex on the render path is forbidden by the hot-path rules.
 *
 *          A seqlock resolves both: the reader takes a wait-free, tear-free
 *          snapshot (it retries while a write is in flight rather than locking),
 *          and writers serialize among themselves. The per-component stores are
 *          relaxed atomics so the concurrent reader is never a data race, and the
 *          odd/even sequence counter lets the reader detect and discard a torn
 *          read. This mirrors the sequence-counter publish that DetourModKit's
 *          Profiler uses to expose sample slots without a reader lock.
 *
 * @warning Writers must be mutually exclusive. An overlap would break the
 *          odd/even sequence invariant (the reader could spin or observe a torn
 *          value). In this mod every writer runs under
 *          CameraProfileManager::m_profileMutex; the one-time startup seed in
 *          DllMain happens before that system's writer threads exist.
 */
class CameraOffsetState
{
public:
    /**
     * @brief Returns a coherent snapshot of the offset without blocking.
     * @details Reads the opening sequence, snapshots the components, then re-reads
     *          the sequence. A change between the two reads, or an odd value, means
     *          a write was in flight, so the snapshot is discarded and retried.
     * @return The current offset as a coherent Vector3.
     */
    [[nodiscard]] Vector3 load() const noexcept
    {
        for (;;)
        {
            const uint32_t seq_before = m_seq.load(std::memory_order_acquire);
            const Vector3 snapshot{
                m_x.load(std::memory_order_relaxed),
                m_y.load(std::memory_order_relaxed),
                m_z.load(std::memory_order_relaxed)};
            // Pairs with the writer's release stores of the sequence so the
            // component loads above are ordered before the re-read below.
            std::atomic_thread_fence(std::memory_order_acquire);
            const uint32_t seq_after = m_seq.load(std::memory_order_relaxed);
            if (seq_before == seq_after && (seq_before & 1u) == 0u)
            {
                return snapshot;
            }
        }
    }

    /**
     * @brief Publishes a new offset. The caller must hold the writer mutex.
     * @details Bumps the sequence to odd (write in progress), stores the
     *          components, then bumps it to even (published). A reader that
     *          observes the odd or changed sequence retries.
     * @param offset The new offset to publish.
     */
    void store(const Vector3 &offset) noexcept
    {
        m_seq.fetch_add(1, std::memory_order_relaxed); // enter write (odd)
        // The release fence keeps the component stores below from being reordered
        // ahead of the odd sequence, so a reader cannot pair an even sequence with
        // half-written data.
        std::atomic_thread_fence(std::memory_order_release);
        m_x.store(offset.x, std::memory_order_relaxed);
        m_y.store(offset.y, std::memory_order_relaxed);
        m_z.store(offset.z, std::memory_order_relaxed);
        m_seq.fetch_add(1, std::memory_order_release); // leave write (even), publish
    }

    /**
     * @brief Adds a delta to the current offset. The caller must hold the writer
     *        mutex so the load and the publish are not interleaved with another
     *        writer.
     * @param delta The per-axis delta to add.
     */
    void add(const Vector3 &delta) noexcept
    {
        store(load() + delta);
    }

private:
    std::atomic<uint32_t> m_seq{0};
    std::atomic<float> m_x{0.0f};
    std::atomic<float> m_y{0.0f};
    std::atomic<float> m_z{0.0f};
};

#endif // CAMERA_OFFSET_STATE_HPP
