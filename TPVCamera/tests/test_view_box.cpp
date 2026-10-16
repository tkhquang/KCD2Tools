/**
 * @file test_view_box.cpp
 * @brief camera/view_box.hpp: the least view depth of the part of a box inside the view frustum, in front of the lens,
 *        beside it, behind it, past the reach, entered obliquely, and with the lens inside the box.
 */

#include "camera/view_box.hpp"

#include <gtest/gtest.h>

namespace
{
    using TPVCamera::box_depth_in_view;
    using TPVCamera::ViewAxes;

    // A view from the origin along +y, 90 degrees wide and tall (both half-angle tangents 1).
    ViewAxes straight_view()
    {
        return ViewAxes{};
    }

    TEST(ViewBox, AheadIsItsNearFace)
    {
        const float box[6] = {-0.5f, 2.0f, -0.5f, 0.5f, 3.0f, 0.5f};
        EXPECT_FLOAT_EQ(box_depth_in_view(box, straight_view(), 5.0f), 2.0f);
    }

    TEST(ViewBox, BehindIsOut)
    {
        const float box[6] = {-0.5f, -3.0f, -0.5f, 0.5f, -1.0f, 0.5f};
        EXPECT_LT(box_depth_in_view(box, straight_view(), 5.0f), 0.0f);
    }

    TEST(ViewBox, BesideTheFrustumIsOut)
    {
        // At depth 1 the view reaches x = 1; the box starts at x = 3.
        const float box[6] = {3.0f, 0.5f, -0.5f, 4.0f, 1.0f, 0.5f};
        EXPECT_LT(box_depth_in_view(box, straight_view(), 5.0f), 0.0f);
    }

    TEST(ViewBox, PastTheReachIsOut)
    {
        const float box[6] = {-0.5f, 2.0f, -0.5f, 0.5f, 3.0f, 0.5f};
        EXPECT_LT(box_depth_in_view(box, straight_view(), 1.2f), 0.0f);
    }

    TEST(ViewBox, ObliqueBoxEntersWhereTheSideMeetsIt)
    {
        // Its nearest corner (x 0.5 at depth 0.2) lies outside the view; the side x = y first meets it at depth 0.5.
        const float box[6] = {0.5f, 0.2f, -0.5f, 2.0f, 3.0f, 0.5f};
        EXPECT_NEAR(box_depth_in_view(box, straight_view(), 5.0f), 0.5f, 1e-5f);
    }

    TEST(ViewBox, LensInsideIsZero)
    {
        const float box[6] = {-0.3f, -0.3f, -1.0f, 0.3f, 0.3f, 1.0f};
        EXPECT_FLOAT_EQ(box_depth_in_view(box, straight_view(), 5.0f), 0.0f);
    }

    TEST(ViewBox, BoxAtTheLensSideButReachingIntoViewIsAtItsEntry)
    {
        // Beside the lens, from just behind it to 2 m ahead, x 0.3..1: the side x = y meets it at depth 0.3.
        const float box[6] = {0.3f, -0.2f, -0.5f, 1.0f, 2.0f, 0.5f};
        EXPECT_NEAR(box_depth_in_view(box, straight_view(), 5.0f), 0.3f, 1e-5f);
    }

    TEST(ViewBox, TurnedViewMeasuresAlongItsForward)
    {
        ViewAxes view;
        view.position = {10.0f, 20.0f, 5.0f};
        view.right = {0.0f, -1.0f, 0.0f};
        view.forward = {1.0f, 0.0f, 0.0f};
        view.up = {0.0f, 0.0f, 1.0f};
        view.tan_half_width = 0.5f;
        view.tan_half_height = 0.3f;
        const float box[6] = {11.0f, 19.9f, 4.9f, 12.0f, 20.1f, 5.1f};
        EXPECT_FLOAT_EQ(box_depth_in_view(box, view, 5.0f), 1.0f);
        // Above the view's top at that depth (z 5.5 > 5 + 0.3 * 1.2) until it is far enough out.
        const float high[6] = {11.0f, 19.9f, 5.5f, 12.0f, 20.1f, 6.0f};
        EXPECT_LT(box_depth_in_view(high, view, 1.5f), 0.0f);
        EXPECT_NEAR(box_depth_in_view(high, view, 5.0f), 0.5f / 0.3f, 1e-4f);
    }
} // namespace
