/**
 * @file test_fence_coverage.cpp
 * @brief The camera-wall policy (fence_coverage.hpp): panel seams, low panels, tall posts, open rails and incomplete
 *        reads.
 */

#include "fence_coverage.hpp"

#include <gtest/gtest.h>

#include <cmath>

namespace
{
    using namespace TPVCamera::FenceCoverage;

    /** @brief Measures a fence whose drawn surface is @p surface(width fraction, height), probe by probe. */
    template <typename Surface> [[nodiscard]] Measurement measure(Surface surface)
    {
        Measurement result;
        for (int row = 0; row < k_rows; ++row)
        {
            const float height = k_first_height + k_height_step * static_cast<float>(row);
            int hits = 0;
            for (int column = 0; column < k_columns; ++column)
            {
                const float x = width_fraction(row, column);
                EXPECT_GT(x, 0.0f);
                EXPECT_LT(x, 1.0f);
                hits += surface(x, height) ? 1 : 0;
            }
            result.observe(hits);
        }
        return result;
    }
} // namespace

TEST(FenceCoverage, SolidWallIsAWall)
{
    EXPECT_EQ(measure([](float, float) { return true; }).verdict(), Verdict::Wall);
}

TEST(FenceCoverage, HorizontalSeamKeepsTwoPanelsAWall)
{
    EXPECT_EQ(measure([](float, float h) { return h < 1.5f || h > 1.7f; }).verdict(), Verdict::Wall);
}

TEST(FenceCoverage, SmallEndOpeningKeepsAWall)
{
    EXPECT_EQ(measure([](float x, float) { return x > 0.08f; }).verdict(), Verdict::Wall);
}

TEST(FenceCoverage, LowPanelsWithTallPostsAreOpen)
{
    EXPECT_EQ(measure([](float x, float h) { return h < 1.5f || x < 0.04f || x > 0.96f; }).verdict(), Verdict::Open);
}

TEST(FenceCoverage, DenseLowerRowsAloneAreOpen)
{
    EXPECT_EQ(measure([](float, float h) { return h < 1.65f; }).verdict(), Verdict::Open);
}

TEST(FenceCoverage, OpenRailsAreOpen)
{
    const auto rails = [](float x, float h)
    { return x < 0.04f || x > 0.96f || std::fabs(h - 1.35f) < 0.04f || std::fabs(h - 1.75f) < 0.04f; };
    EXPECT_EQ(measure(rails).verdict(), Verdict::Open);
}

TEST(FenceCoverage, PostsOnTheFirstRowProbesDoNotAliasOtherRows)
{
    const auto posts = [](float x, float)
    {
        for (int c = 0; c < k_columns; ++c)
        {
            if (std::fabs(x - width_fraction(0, c)) < 0.01f)
            {
                return true;
            }
        }
        return false;
    };
    EXPECT_EQ(measure(posts).verdict(), Verdict::Open);
}

TEST(FenceCoverage, IncompleteRowsStayPendingAndACompletedVerdictSticks)
{
    Measurement missing;
    EXPECT_EQ(missing.rows, 0);
    EXPECT_EQ(missing.verdict(), Verdict::Pending);
    missing.observe(-1);
    missing.observe(k_columns + 1);
    EXPECT_EQ(missing.rows, 0);
    for (int i = 0; i < k_rows; ++i)
    {
        missing.observe(k_columns);
    }
    EXPECT_EQ(missing.verdict(), Verdict::Wall);
    const int completed = missing.rows;
    missing.observe(0);
    EXPECT_EQ(missing.rows, completed);
    EXPECT_EQ(missing.verdict(), Verdict::Wall);
}

TEST(FenceCoverage, SliceEndingMidRowNeverPublishesAPartialRow)
{
    Measurement resumed;
    for (int column = 0; column < k_columns - 1; ++column)
    {
        resumed.observe_probe(false);
        EXPECT_EQ(resumed.rows, 0);
        EXPECT_EQ(resumed.verdict(), Verdict::Pending);
        EXPECT_EQ(resumed.columns, column + 1);
        EXPECT_EQ(resumed.row_hits, 0);
    }
    resumed.observe_probe(true);
    EXPECT_EQ(resumed.rows, 1);
    EXPECT_EQ(resumed.hits[0], 1);
    EXPECT_EQ(resumed.columns, 0);
    EXPECT_EQ(resumed.row_hits, 0);
}

TEST(FenceCoverage, ProbeByProbeRecordingMatchesWholeRows)
{
    for (unsigned int pattern = 0; pattern < (1u << k_rows); ++pattern)
    {
        Measurement whole;
        Measurement sliced;
        for (int row = 0; row < k_rows; ++row)
        {
            const bool hit = (pattern & (1u << row)) != 0;
            whole.observe(hit ? k_columns : 0);
            for (int column = 0; column < k_columns; ++column)
            {
                sliced.observe_probe(hit);
            }
        }
        EXPECT_EQ(sliced.verdict(), whole.verdict()) << "pattern " << pattern;
        EXPECT_EQ(sliced.rows, whole.rows) << "pattern " << pattern;
        EXPECT_EQ(sliced.hits, whole.hits) << "pattern " << pattern;
        EXPECT_EQ(sliced.columns, 0) << "pattern " << pattern;
        EXPECT_EQ(sliced.row_hits, 0) << "pattern " << pattern;
    }
}

TEST(FenceCoverage, EveryCompleteRowPatternTerminates)
{
    for (unsigned int pattern = 0; pattern < (1u << k_rows); ++pattern)
    {
        Measurement m;
        for (int row = 0; row < k_rows; ++row)
        {
            m.observe((pattern & (1u << row)) != 0 ? k_columns : 0);
        }
        EXPECT_NE(m.verdict(), Verdict::Pending) << "pattern " << pattern;
    }
}
