/**
 * @file fixes/diag.cpp
 * @brief The counters and the summary line (see diag.hpp).
 */

#include "fixes/diag.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string_view>

namespace EngineTweaks::fixes::diag
{
    namespace
    {
        /// The shortest time between two summary lines.
        constexpr std::uint64_t SUMMARY_INTERVAL_MS = 60000;

        Counters s_counters;
        std::atomic<std::uint64_t> s_next_summary{0};
        /// The counter total at the last summary, so an idle interval writes nothing.
        std::atomic<std::uint64_t> s_last_total{0};

        [[nodiscard]] std::uint64_t total(const Counters &tally) noexcept
        {
            return tally.menu_enters.get() + tally.link_unlinks.get() + tally.link_scans.get() +
                   tally.link_rebuilds.get() + tally.link_invalidations.get() + tally.clears.get() +
                   tally.clear_overflows.get() + tally.perk_copies.get() + tally.sleep_requests.get() +
                   tally.sleep_passthroughs.get();
        }

        void write_summary(std::string_view when) noexcept
        {
            const Counters &tally = s_counters;
            (void)DMK::log().try_log(
                DMK::LogLevel::Info,
                "Summary ({}): menus {} entered, {} view distance restore(s) | links {} unlink(s) by index over {} "
                "node(s), {} by the game's scan ({} fallback(s)), {} index rebuild(s) ({} failed), {} invalidation(s), "
                "{} insert(s) and {} erase(s) on the indexed map, {} verify mismatch(es) | "
                "clears {} with {} refresh(es) deferred, {} pending overflow(s), {} on another thread | perks {} "
                "cop(ies), {} unlocking in {} pass(es), {} pass limit(s) | sleep {} request(s), {} build(s), {} hour "
                "step(s) skipped, {} item clone(s) skipped, {} passed through, {} verify mismatch(es) | log drops {}",
                when,
                tally.menu_enters.get(),
                tally.menu_restores.get(),
                tally.link_unlinks.get(),
                tally.link_nodes.get(),
                tally.link_scans.get(),
                tally.link_fallbacks.get(),
                tally.link_rebuilds.get(),
                tally.link_rebuild_failures.get(),
                tally.link_invalidations.get(),
                tally.link_inserts.get(),
                tally.link_erases.get(),
                tally.link_verify_mismatches.get(),
                tally.clears.get(),
                tally.clear_refreshes_deferred.get(),
                tally.clear_overflows.get(),
                tally.clear_contended.get(),
                tally.perk_copies.get(),
                tally.perk_copies_unlocking.get(),
                tally.perk_passes.get(),
                tally.perk_pass_limits.get(),
                tally.sleep_requests.get(),
                tally.sleep_builds.get(),
                tally.sleep_steps_skipped.get(),
                tally.sleep_clones_skipped.get(),
                tally.sleep_passthroughs.get(),
                tally.sleep_verify_mismatches.get(),
                DMK::log().dropped_count()
            );
        }
    } // namespace

    Counters &counters() noexcept
    {
        return s_counters;
    }

    void maybe_log_summary() noexcept
    {
        const std::uint64_t now = GetTickCount64();
        std::uint64_t due = s_next_summary.load(std::memory_order_relaxed);
        if (now < due || !s_next_summary.compare_exchange_strong(due, now + SUMMARY_INTERVAL_MS))
        {
            return;
        }
        const std::uint64_t sum = total(s_counters);
        if (s_last_total.exchange(sum, std::memory_order_relaxed) == sum)
        {
            return;
        }
        write_summary("totals since the mod loaded");
    }

    void log_summary_now() noexcept
    {
        write_summary("at shutdown, totals since the mod loaded");
    }
} // namespace EngineTweaks::fixes::diag
