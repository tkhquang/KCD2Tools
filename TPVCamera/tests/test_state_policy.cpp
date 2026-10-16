/**
 * @file test_state_policy.cpp
 * @brief core/policy/state_policy.hpp: the rows built from the five [StateBehavior] lists, first person winning over
 * third, and
 *        EnableStateBehavior gating only the edge-triggered lists.
 */

#include "core/policy/state_policy.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace
{
    using TPVCamera::StatePolicyLists;
    using TPVCamera::StatePolicyTable;
    using TPVCamera::ViewForce;

    constexpr std::uint32_t k_overlay = 1u << 1;
    constexpr std::uint32_t k_combat = 1u << 2;
    constexpr std::uint32_t k_mount = 1u << 3;
    constexpr std::uint32_t k_aiming = 1u << 6;
    constexpr std::uint32_t k_cart = 1u << 13;

    StatePolicyLists shipped_like()
    {
        return StatePolicyLists{
            .forced_fpv = k_aiming | k_cart,
            .forced_tpv = k_mount | k_cart,
            .orbit_exclude = k_overlay | k_aiming | k_combat | k_mount,
            .native_turn_exclude = k_combat | k_aiming | k_mount,
            .suppress = k_overlay,
            .edge_rules = true,
        };
    }

    TEST(StatePolicy, RowsFollowTheLists)
    {
        const StatePolicyTable table{shipped_like()};
        EXPECT_EQ(table.row(k_aiming).view, ViewForce::FirstPerson);
        EXPECT_TRUE(table.row(k_aiming).orbit_exclude);
        EXPECT_TRUE(table.row(k_aiming).native_turn_exclude);
        EXPECT_FALSE(table.row(k_aiming).suppress);
        EXPECT_EQ(table.row(k_mount).view, ViewForce::ThirdPerson);
        EXPECT_TRUE(table.row(k_overlay).suppress);
        EXPECT_EQ(table.row(k_overlay).view, ViewForce::None);
    }

    TEST(StatePolicy, FirstPersonWinsInARowAndAcrossSituations)
    {
        const StatePolicyTable table{shipped_like()};
        EXPECT_EQ(table.row(k_cart).view, ViewForce::FirstPerson); // listed in both
        EXPECT_EQ(table.view(k_mount | k_aiming), ViewForce::FirstPerson);
        EXPECT_EQ(table.view(k_mount), ViewForce::ThirdPerson);
        EXPECT_EQ(table.view(k_combat), ViewForce::None);
        EXPECT_EQ(table.forcing(ViewForce::FirstPerson), k_aiming | k_cart);
        EXPECT_EQ(table.forcing(ViewForce::ThirdPerson), k_mount);
        EXPECT_TRUE(table.forces_any_view());
    }

    TEST(StatePolicy, EnableStateBehaviorGatesOnlyTheEdgeRules)
    {
        StatePolicyLists lists = shipped_like();
        lists.edge_rules = false;
        const StatePolicyTable table{lists};
        EXPECT_EQ(table.view(k_aiming | k_mount), ViewForce::None);
        EXPECT_FALSE(table.forces_any_view());
        EXPECT_EQ(table.orbit_exclude(), 0u);
        EXPECT_EQ(table.native_turn_exclude(), k_combat | k_aiming | k_mount);
        EXPECT_EQ(table.suppress(), k_overlay);
    }

    TEST(StatePolicy, MasksEqualTheListsWithTheEdgeRulesOn)
    {
        const StatePolicyLists lists = shipped_like();
        const StatePolicyTable table{lists};
        EXPECT_EQ(table.orbit_exclude(), lists.orbit_exclude);
        EXPECT_EQ(table.native_turn_exclude(), lists.native_turn_exclude);
        EXPECT_EQ(table.suppress(), lists.suppress);
    }

    TEST(StatePolicy, NoneOrSeveralBitsReadAPlainRow)
    {
        const StatePolicyTable table{shipped_like()};
        EXPECT_EQ(table.row(0).view, ViewForce::None);
        EXPECT_FALSE(table.row(k_overlay | k_aiming).suppress);
    }
} // namespace
