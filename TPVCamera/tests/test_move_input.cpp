/**
 * @file test_move_input.cpp
 * @brief core/input/move_input.hpp step_game_move_stand_in(): the game's own move input stands in for a latch reset
 *        under a held key or stick until that input stays idle, and never for a stranded latch or after a release.
 */

#include "core/input/move_input.hpp"

#include <gtest/gtest.h>

#include <optional>

namespace
{
    using TPVCamera::GameMoveStandIn;
    using TPVCamera::GameMoveStandInStep;
    using TPVCamera::k_game_move_stop_delay;
    using TPVCamera::step_game_move_stand_in;

    constexpr float k_frame = 1.0f / 60.0f;
} // namespace

TEST(GameMoveStandIn, StartsForAnIdleLatchWhileTheGameShowsAMove)
{
    GameMoveStandIn stand_in{};
    const GameMoveStandInStep step = step_game_move_stand_in(stand_in, false, 0.0f, 1.0f, k_frame);
    EXPECT_TRUE(step.started);
    EXPECT_TRUE(stand_in.active);
    EXPECT_FLOAT_EQ(step.magnitude, 1.0f);
}

TEST(GameMoveStandIn, CoversASlightDeflectionFromTheSticksOtherAxis)
{
    GameMoveStandIn stand_in{};
    const GameMoveStandInStep step = step_game_move_stand_in(stand_in, false, 0.1f, 0.99f, k_frame);
    EXPECT_TRUE(step.started);
    EXPECT_FLOAT_EQ(step.magnitude, 0.99f);
}

TEST(GameMoveStandIn, HoldsWhenTheLatchFallsBackToZeroWithTheStickPushed)
{
    GameMoveStandIn stand_in{};
    (void)step_game_move_stand_in(stand_in, false, 0.3f, 1.0f, k_frame);
    const GameMoveStandInStep step = step_game_move_stand_in(stand_in, true, 0.0f, 1.0f, k_frame);
    EXPECT_FALSE(step.started);
    EXPECT_TRUE(stand_in.active);
    EXPECT_FLOAT_EQ(step.magnitude, 1.0f);
}

TEST(GameMoveStandIn, NeverStartsOnceAReleaseArmedTheMove)
{
    GameMoveStandIn stand_in{};
    const GameMoveStandInStep step = step_game_move_stand_in(stand_in, true, 0.0f, 1.0f, k_frame);
    EXPECT_FALSE(step.started);
    EXPECT_FALSE(stand_in.active);
    EXPECT_FLOAT_EQ(step.magnitude, 0.0f);
}

TEST(GameMoveStandIn, LeavesAStrandedLatchToTheReArmGuard)
{
    GameMoveStandIn stand_in{};
    const GameMoveStandInStep step = step_game_move_stand_in(stand_in, false, 1.0f, 0.0f, k_frame);
    EXPECT_FALSE(step.started);
    EXPECT_FALSE(stand_in.active);
    EXPECT_FLOAT_EQ(step.magnitude, 1.0f);
}

TEST(GameMoveStandIn, StartsNothingWhenTheGamesInputCannotBeRead)
{
    GameMoveStandIn stand_in{};
    const GameMoveStandInStep step = step_game_move_stand_in(stand_in, false, 0.0f, std::nullopt, k_frame);
    EXPECT_FALSE(step.started);
    EXPECT_FALSE(stand_in.active);
    EXPECT_FLOAT_EQ(step.magnitude, 0.0f);
}

TEST(GameMoveStandIn, EndsOnceTheGamesInputStaysIdleForTheStopDelay)
{
    GameMoveStandIn stand_in{};
    (void)step_game_move_stand_in(stand_in, false, 0.0f, 1.0f, k_frame);
    const float half_delay = k_game_move_stop_delay * 0.5f;
    const GameMoveStandInStep waiting = step_game_move_stand_in(stand_in, true, 0.0f, 0.0f, half_delay);
    EXPECT_TRUE(stand_in.active);
    EXPECT_FLOAT_EQ(waiting.magnitude, 0.0f);
    (void)step_game_move_stand_in(stand_in, true, 0.0f, 0.0f, half_delay);
    EXPECT_FALSE(stand_in.active);
    EXPECT_FLOAT_EQ(stand_in.idle_seconds, 0.0f);
}

TEST(GameMoveStandIn, AMoveReadingRestartsTheIdleWait)
{
    GameMoveStandIn stand_in{};
    (void)step_game_move_stand_in(stand_in, false, 0.0f, 1.0f, k_frame);
    const float most_of_delay = k_game_move_stop_delay * 0.75f;
    (void)step_game_move_stand_in(stand_in, true, 0.0f, 0.0f, most_of_delay);
    (void)step_game_move_stand_in(stand_in, true, 0.0f, 1.0f, k_frame);
    EXPECT_FLOAT_EQ(stand_in.idle_seconds, 0.0f);
    (void)step_game_move_stand_in(stand_in, true, 0.0f, 0.0f, most_of_delay);
    EXPECT_TRUE(stand_in.active);
}

TEST(GameMoveStandIn, AnUnreadableGameInputCountsAsIdle)
{
    GameMoveStandIn stand_in{};
    (void)step_game_move_stand_in(stand_in, false, 0.0f, 1.0f, k_frame);
    (void)step_game_move_stand_in(stand_in, true, 0.0f, std::nullopt, k_game_move_stop_delay);
    EXPECT_FALSE(stand_in.active);
}
