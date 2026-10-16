/**
 * @file test_channels.cpp
 * @brief core/frame/channels.hpp: a reader gets the initial value before any publish and the last value after, and
 *        under a publisher on another thread every copy it gets comes from one publish; a triple buffer publishes the
 *        claimed slot, withdraws to none, and claims a slot again only after two others.
 */

#include "core/frame/channels.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>

namespace
{
    using TPVCamera::SeqlockSnapshot;
    using TPVCamera::TripleBuffer;

    /** @brief A value whose fields all derive from one counter, so a mix of two publishes shows. */
    struct Sample
    {
        std::uint64_t count = 0;
        float half = 0.0f;
        std::uint32_t inverted = ~0u;
        bool odd = false;
    };

    Sample sample_of(std::uint64_t count)
    {
        return Sample{
            .count = count,
            .half = static_cast<float>(count) * 0.5f,
            .inverted = ~static_cast<std::uint32_t>(count),
            .odd = (count & 1u) != 0u,
        };
    }

    bool coherent(const Sample &sample)
    {
        const Sample expected = sample_of(sample.count);
        return sample.half == expected.half && sample.inverted == expected.inverted && sample.odd == expected.odd;
    }
} // namespace

TEST(SeqlockSnapshot, ReadsTheInitialThenTheLastPublish)
{
    SeqlockSnapshot<Sample> channel{sample_of(7)};
    Sample read;
    ASSERT_TRUE(channel.try_load(read));
    EXPECT_EQ(read.count, 7u);
    channel.publish(sample_of(8));
    channel.publish(sample_of(9));
    ASSERT_TRUE(channel.try_load(read));
    EXPECT_EQ(read.count, 9u);
    EXPECT_TRUE(coherent(read));
}

TEST(SeqlockSnapshot, ReaderNeverSeesAMixedPublish)
{
    SeqlockSnapshot<Sample> channel{sample_of(0)};
    std::atomic<bool> stop{false};
    std::thread publisher(
        [&channel, &stop]
        {
            for (std::uint64_t count = 1; !stop.load(std::memory_order_relaxed); ++count)
            {
                channel.publish(sample_of(count));
            }
        });
    std::uint64_t reads = 0;
    std::uint64_t last = 0;
    bool mixed = false;
    bool backwards = false;
    for (int i = 0; i < 200000; ++i)
    {
        Sample read;
        if (!channel.try_load(read))
        {
            continue;
        }
        ++reads;
        mixed = mixed || !coherent(read);
        backwards = backwards || read.count < last;
        last = read.count;
    }
    stop.store(true, std::memory_order_relaxed);
    publisher.join();
    EXPECT_GT(reads, 0u);
    EXPECT_FALSE(mixed);
    EXPECT_FALSE(backwards);
}

TEST(TripleBuffer, PublishesTheClaimedSlotAndWithdraws)
{
    TripleBuffer<int> tables;
    EXPECT_EQ(tables.latest(), nullptr);
    int &first = tables.claim();
    first = 1;
    tables.publish(first);
    ASSERT_NE(tables.latest(), nullptr);
    EXPECT_EQ(*tables.latest(), 1);
    tables.withdraw();
    EXPECT_EQ(tables.latest(), nullptr);
}

TEST(TripleBuffer, ReclaimsASlotOnlyAfterTwoOthers)
{
    // A reader holding the table published two publishes ago still reads it unchanged.
    TripleBuffer<int> tables;
    int &first = tables.claim();
    first = 1;
    tables.publish(first);
    const int *held = tables.latest();
    int &second = tables.claim();
    second = 2;
    tables.publish(second);
    int &third = tables.claim();
    third = 3;
    tables.publish(third);
    EXPECT_EQ(*held, 1);
    EXPECT_NE(&second, held);
    EXPECT_NE(&third, held);
    EXPECT_EQ(&tables.claim(), held);
}
