/**
 * @file camera/view_box.hpp
 * @brief How near the lens a world box shows in the view: the least view depth of the part of an axis-aligned box that
 *        lies inside the view frustum.
 *
 * @details Engine-free (no Windows or DetourModKit header), so the local tests compile it as is.
 */
#ifndef TPVCAMERA_CAMERA_VIEW_BOX_HPP
#define TPVCAMERA_CAMERA_VIEW_BOX_HPP

#include <algorithm>
#include <array>
#include <cstddef>

namespace TPVCamera
{
    /** @brief A view: the lens position, its unit right, forward and up axes, and the frustum's half-angle tangents. */
    struct ViewAxes
    {
        std::array<float, 3> position{};
        std::array<float, 3> right{1.0f, 0.0f, 0.0f};
        std::array<float, 3> forward{0.0f, 1.0f, 0.0f};
        std::array<float, 3> up{0.0f, 0.0f, 1.0f};
        float tan_half_width = 1.0f;
        float tan_half_height = 1.0f;
    };

    namespace view_box_detail
    {
        /** @brief A point in view space: x along right, y the view depth, z along up. */
        struct ViewPoint
        {
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
        };

        /** @brief A clip plane a * x + b * y + c * z + d >= 0 in view space. */
        struct Plane
        {
            float a = 0.0f;
            float b = 0.0f;
            float c = 0.0f;
            float d = 0.0f;

            [[nodiscard]] float at(const ViewPoint &p) const noexcept { return a * p.x + b * p.y + c * p.z + d; }
        };

        // A box face (4 corners) clipped by the six planes gains at most one corner per plane.
        constexpr std::size_t k_max_poly = 10;

        /** @brief A convex polygon of at most k_max_poly view-space corners. */
        struct Polygon
        {
            std::array<ViewPoint, k_max_poly> points{};
            std::size_t count = 0;
        };

        /** @brief Keeps the part of @p in on the non-negative side of @p plane (Sutherland-Hodgman, one plane). */
        [[nodiscard]] inline Polygon clip(const Polygon &in, const Plane &plane) noexcept
        {
            Polygon out;
            for (std::size_t i = 0; i < in.count; ++i)
            {
                const ViewPoint &a = in.points[i];
                const ViewPoint &b = in.points[(i + 1) % in.count];
                const float da = plane.at(a);
                const float db = plane.at(b);
                if (da >= 0.0f && out.count < k_max_poly)
                {
                    out.points[out.count++] = a;
                }
                if ((da >= 0.0f) != (db >= 0.0f) && out.count < k_max_poly)
                {
                    const float t = da / (da - db);
                    out.points[out.count++] =
                        ViewPoint{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
                }
            }
            return out;
        }

        [[nodiscard]] inline float dot3(const std::array<float, 3> &a, float x, float y, float z) noexcept
        {
            return a[0] * x + a[1] * y + a[2] * z;
        }
    } // namespace view_box_detail

    /**
     * @brief The least view depth of the part of @p box that lies inside @p view's frustum, from the lens out to
     *        @p reach meters of depth, or a negative value when no part of it does.
     * @details The least of a linear function over the convex intersection of a box and a frustum lies at one of the
     *          intersection's corners. Each such corner lies on a face of the box, unless it is a corner of the
     *          frustum itself, and the only frustum corner nearer than its far end is the lens: so each of the box's
     *          six faces is clipped by the frustum's four sides and its near and far ends, the least depth of what is
     *          left is taken, and the lens inside the box gives 0. Exact, no sampling.
     * @param box World AABB {min.x, min.y, min.z, max.x, max.y, max.z}.
     */
    [[nodiscard]] inline float box_depth_in_view(const float (&box)[6], const ViewAxes &view, float reach) noexcept
    {
        using view_box_detail::Plane;
        using view_box_detail::Polygon;
        using view_box_detail::ViewPoint;

        const auto &p = view.position;
        if (p[0] >= box[0] && p[0] <= box[3] && p[1] >= box[1] && p[1] <= box[4] && p[2] >= box[2] && p[2] <= box[5])
        {
            return 0.0f;
        }
        const auto to_view = [&](float wx, float wy, float wz) noexcept
        {
            const float dx = wx - p[0];
            const float dy = wy - p[1];
            const float dz = wz - p[2];
            return ViewPoint{view_box_detail::dot3(view.right, dx, dy, dz),
                             view_box_detail::dot3(view.forward, dx, dy, dz),
                             view_box_detail::dot3(view.up, dx, dy, dz)};
        };
        // Corner i has x from bit 0, y from bit 1 and z from bit 2 (0 = min, 1 = max).
        std::array<ViewPoint, 8> corners{};
        for (std::size_t i = 0; i < 8; ++i)
        {
            corners[i] = to_view(box[(i & 1) != 0 ? 3 : 0], box[(i & 2) != 0 ? 4 : 1], box[(i & 4) != 0 ? 5 : 2]);
        }
        // The faces as corner indices, each listed around its edge: x min, x max, y min, y max, z min, z max.
        constexpr std::array<std::array<std::size_t, 4>, 6> k_faces{{
            {0, 2, 6, 4},
            {1, 5, 7, 3},
            {0, 4, 5, 1},
            {2, 3, 7, 6},
            {0, 1, 3, 2},
            {4, 6, 7, 5},
        }};
        const float tw = view.tan_half_width;
        const float th = view.tan_half_height;
        const std::array<Plane, 6> planes{{
            {0.0f, 1.0f, 0.0f, 0.0f},   // in front of the lens
            {0.0f, -1.0f, 0.0f, reach}, // no deeper than reach
            {-1.0f, tw, 0.0f, 0.0f},    // x <= tw * y
            {1.0f, tw, 0.0f, 0.0f},     // -x <= tw * y
            {0.0f, th, -1.0f, 0.0f},    // z <= th * y
            {0.0f, th, 1.0f, 0.0f},     // -z <= th * y
        }};
        float nearest = -1.0f;
        for (const auto &face : k_faces)
        {
            Polygon poly;
            for (const std::size_t corner : face)
            {
                poly.points[poly.count++] = corners[corner];
            }
            for (const Plane &plane : planes)
            {
                poly = view_box_detail::clip(poly, plane);
                if (poly.count == 0)
                {
                    break;
                }
            }
            for (std::size_t i = 0; i < poly.count; ++i)
            {
                const float depth = std::max(0.0f, poly.points[i].y);
                nearest = nearest < 0.0f ? depth : std::min(nearest, depth);
            }
        }
        return nearest;
    }
} // namespace TPVCamera

#endif // TPVCAMERA_CAMERA_VIEW_BOX_HPP
