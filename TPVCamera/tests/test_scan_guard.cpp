/**
 * @file test_scan_guard.cpp
 * @brief The occluder scan's single-walk guard and truncation report limit (core/occlusion/scan_guard.hpp).
 */

#include "core/occlusion/scan_guard.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace
{
    using namespace TPVCamera::ScanGuard;

    constexpr std::uint64_t k_guard = 65536;

    /** @brief The bounds of the first @p n entries of @p nodes, as the engine vector holds them. */
    template <std::size_t N>
    [[nodiscard]] Collected collect(const std::array<void *, N> &nodes, std::size_t n, std::uint64_t guard = k_guard)
    {
        const auto first = reinterpret_cast<std::uintptr_t>(nodes.data());
        return collected(first, first + n * sizeof(void *), guard);
    }
} // namespace

TEST(ScanGuard, EmptyVectorHoldsNothing)
{
    const Collected got = collected(0, 0, k_guard);
    EXPECT_EQ(got.count, 0u);
    EXPECT_FALSE(got.overflow);
}

TEST(ScanGuard, CountsThePointersBetweenTheBounds)
{
    std::array<void *, 8> nodes{};
    const Collected got = collect(nodes, 3);
    EXPECT_EQ(got.count, 3u);
    EXPECT_FALSE(got.overflow);
}

TEST(ScanGuard, MeasuredDenseInteriorPassesWhole)
{
    // The densest typed query measured in game: 1695 brushes in a furnished Kuttenberg interior.
    const Collected got = collected(0x10000, 0x10000 + 1695 * sizeof(void *), k_guard);
    EXPECT_EQ(got.count, 1695u);
    EXPECT_FALSE(got.overflow);
}

TEST(ScanGuard, GuardItselfStillFits)
{
    const Collected got = collected(0x10000, 0x10000 + k_guard * sizeof(void *), k_guard);
    EXPECT_EQ(got.count, k_guard);
    EXPECT_FALSE(got.overflow);
}

TEST(ScanGuard, OnePastTheGuardOverflows)
{
    const Collected got = collected(0x10000, 0x10000 + (k_guard + 1) * sizeof(void *), k_guard);
    EXPECT_EQ(got.count, k_guard + 1);
    EXPECT_TRUE(got.overflow);
}

TEST(ScanGuard, LastBeforeFirstOverflows)
{
    const Collected got = collected(0x20000, 0x10000, k_guard);
    EXPECT_TRUE(got.overflow);
}

TEST(ReportLimiter, FirstEventReportsAtOnce)
{
    ReportLimiter limiter{60000};
    EXPECT_EQ(limiter.note(5), 1u);
}

TEST(ReportLimiter, FirstEventAtClockZeroReports)
{
    ReportLimiter limiter{60000};
    EXPECT_EQ(limiter.note(0), 1u);
    EXPECT_EQ(limiter.note(1), 0u);
}

TEST(ReportLimiter, EventsWithinTheIntervalWaitAndAreCarried)
{
    ReportLimiter limiter{60000};
    EXPECT_EQ(limiter.note(1000), 1u);
    for (std::uint64_t t = 2000; t < 61000; t += 10000)
    {
        EXPECT_EQ(limiter.note(t), 0u);
    }
    // Six silent events, then this one at the interval's end.
    EXPECT_EQ(limiter.note(61000), 7u);
    EXPECT_EQ(limiter.note(61001), 0u);
}

TEST(ReportLimiter, QuietStretchReportsTheNextEventAtOnce)
{
    ReportLimiter limiter{60000};
    EXPECT_EQ(limiter.note(1000), 1u);
    EXPECT_EQ(limiter.note(500000), 1u);
}
