/**
 * @file hooks/frame_publication.hpp
 * @brief The lapse of a value the camera publishes for a game hook every frame it builds the third-person view.
 */
#ifndef TPVCAMERA_HOOKS_FRAME_PUBLICATION_HPP
#define TPVCAMERA_HOOKS_FRAME_PUBLICATION_HPP

#include <atomic>
#include <chrono>
#include <cstdint>

namespace TPVCamera
{
    /**
     * @brief Whether a value the camera published still stands.
     * @details A publication stands for a quarter second, so a hook falls back to the game's own value soon after the
     *          camera stops publishing (a load, a rig that fails to build, first person). The publisher stores the
     *          value relaxed and then calls renew(), whose release store pairs with the acquire load in live(). A
     *          hook therefore reads the value relaxed after live() returns true. A read across a later publication
     *          can mix two frames' values.
     * @note Any thread. No-throw, no allocation.
     */
    class FramePublication
    {
    public:
        /// Starts a quarter second in which live() returns true.
        void renew() noexcept { m_valid_until.store(now_ns() + k_lifetime_ns, std::memory_order_release); }

        /// Ends the current publication at once.
        void withdraw() noexcept { m_valid_until.store(0, std::memory_order_release); }

        /// Returns whether the last renew() still stands and no withdraw() followed it.
        [[nodiscard]] bool live() const noexcept { return now_ns() < m_valid_until.load(std::memory_order_acquire); }

    private:
        static constexpr std::int64_t k_lifetime_ns = 250'000'000;

        [[nodiscard]] static std::int64_t now_ns() noexcept
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }

        /// The steady clock (ns) the publication lapses at, 0 for none.
        std::atomic<std::int64_t> m_valid_until{0};
    };
} // namespace TPVCamera

#endif // TPVCAMERA_HOOKS_FRAME_PUBLICATION_HPP
