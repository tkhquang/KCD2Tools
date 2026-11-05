/**
 * @file core/occlusion/scan_guard.hpp
 * @brief The occluder scan's corruption guard on a single-walk octree query, and the rate limit of its truncation
 *        report.
 * @details A single-walk query fills the engine's own growable vector of render node pointers, so its count is read
 *          from the vector's bounds and no node is lost to a buffer of ours; the guard only refuses a vector whose
 *          bounds cannot be real. The truncation report fires on the first scan a guard cut short and then at most
 *          once per interval, carrying the count since the previous report, so a repeating cause stays visible
 *          without flooding the log.
 */
#ifndef TPVCAMERA_CORE_OCCLUSION_SCAN_GUARD_HPP
#define TPVCAMERA_CORE_OCCLUSION_SCAN_GUARD_HPP

#include <cstdint>
#include <utility>

namespace TPVCamera::ScanGuard
{
    /** @brief What one single-walk query returned, read from its vector's bounds. */
    struct Collected
    {
        /// The node pointers between the vector's first and last addresses.
        std::uint64_t count = 0;
        /// More nodes than the guard allows: the vector is taken for corrupt and the query for empty.
        bool overflow = false;
    };

    /**
     * @brief Counts the node pointers of a vector from its first and last addresses.
     * @details Unsigned arithmetic: a vector whose last address lies before its first reads as a huge count and fails
     *          the guard, so a corrupt vector is never walked.
     * @param guard The most nodes a real query can return.
     */
    [[nodiscard]] constexpr Collected collected(std::uintptr_t first, std::uintptr_t last, std::uint64_t guard) noexcept
    {
        const std::uint64_t count = static_cast<std::uint64_t>(last - first) / sizeof(void *);
        return {.count = count, .overflow = count > guard};
    }

    /**
     * @brief Rate limit of a repeating report: the first event reports at once, later ones at most once per interval,
     *        and each report carries the events since the previous one.
     */
    class ReportLimiter
    {
    public:
        explicit constexpr ReportLimiter(std::uint64_t interval_ms) noexcept : m_interval_ms(interval_ms) {}

        /**
         * @brief Counts one event at @p now_ms (a monotonic millisecond clock).
         * @return The events to report now (this one included), or 0 while the interval since the last report runs.
         */
        [[nodiscard]] constexpr std::uint64_t note(std::uint64_t now_ms) noexcept
        {
            ++m_pending;
            if (m_reported && now_ms - m_last_ms < m_interval_ms)
            {
                return 0;
            }
            m_reported = true;
            m_last_ms = now_ms;
            return std::exchange(m_pending, 0);
        }

    private:
        std::uint64_t m_interval_ms;
        std::uint64_t m_last_ms = 0;
        std::uint64_t m_pending = 0;
        bool m_reported = false;
    };
} // namespace TPVCamera::ScanGuard

#endif // TPVCAMERA_CORE_OCCLUSION_SCAN_GUARD_HPP
