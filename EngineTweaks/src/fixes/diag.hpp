/**
 * @file fixes/diag.hpp
 * @brief Diagnostics shared by the fixes: event counters, the periodic summary line and a stopwatch.
 *
 * Every detour counts what it did. maybe_log_summary() writes one Info line with every counter at most once a minute,
 * and only when a counter moved, so an Info log still shows which fixes ran and how often. Per-call detail goes to
 * Trace and notable events to Debug, in the fixes themselves.
 */
#ifndef ENGINETWEAKS_FIXES_DIAG_HPP
#define ENGINETWEAKS_FIXES_DIAG_HPP

#include <windows.h>

#include <atomic>
#include <cstdint>

namespace EngineTweaks::fixes::diag
{
    /// One event counter, relaxed because the counts feed only the log.
    class Counter
    {
    public:
        /// Adds @p amount to the count.
        void add(std::uint64_t amount = 1) noexcept { m_value.fetch_add(amount, std::memory_order_relaxed); }
        /// The count so far.
        [[nodiscard]] std::uint64_t get() const noexcept { return m_value.load(std::memory_order_relaxed); }

    private:
        std::atomic<std::uint64_t> m_value{0};
    };

    /// The counters of every fix.
    struct Counters
    {
        Counter menu_enters;
        Counter menu_restores;
        Counter link_unlinks;
        Counter link_nodes;
        Counter link_scans;
        Counter link_fallbacks;
        Counter link_rebuilds;
        Counter link_rebuild_failures;
        Counter link_invalidations;
        Counter link_inserts;
        Counter link_erases;
        Counter link_verify_mismatches;
        Counter clears;
        Counter clear_refreshes_deferred;
        Counter clear_overflows;
        Counter clear_contended;
        Counter perk_copies;
        Counter perk_copies_unlocking;
        Counter perk_passes;
        Counter perk_pass_limits;
        Counter sleep_requests;
        Counter sleep_builds;
        Counter sleep_steps_skipped;
        Counter sleep_clones_skipped;
        Counter sleep_passthroughs;
        Counter sleep_verify_mismatches;
    };

    /** @brief The counters of this generation. */
    [[nodiscard]] Counters &counters() noexcept;

    /**
     * @brief Writes the summary line when a minute passed since the last one and a counter moved.
     * @note Callback-safe: one caller wins each interval. No-throw.
     */
    void maybe_log_summary() noexcept;

    /** @brief Writes the summary line now. No-throw. */
    void log_summary_now() noexcept;

    /**
     * @brief A timer that starts at construction.
     * @note Callback-safe: QueryPerformanceCounter and QueryPerformanceFrequency reads only.
     */
    class Stopwatch
    {
    public:
        Stopwatch() noexcept { QueryPerformanceCounter(&m_start); }

        /** @brief Milliseconds since construction. */
        [[nodiscard]] double elapsed_ms() const noexcept
        {
            LARGE_INTEGER now{};
            LARGE_INTEGER frequency{};
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&frequency);
            return static_cast<double>(now.QuadPart - m_start.QuadPart) * 1000.0 /
                   static_cast<double>(frequency.QuadPart);
        }

    private:
        LARGE_INTEGER m_start{};
    };
} // namespace EngineTweaks::fixes::diag

#endif // ENGINETWEAKS_FIXES_DIAG_HPP
