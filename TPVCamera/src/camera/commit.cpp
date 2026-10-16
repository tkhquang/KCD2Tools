/**
 * @file camera/commit.cpp
 * @brief The single writer of the game view camera's per-build values (see commit.hpp).
 */

#include "camera/commit.hpp"
#include "constants.hpp"
#include "game_structures.hpp"

namespace TPVCamera
{
    namespace
    {
        [[nodiscard]] float &camera_float(std::uintptr_t camera, std::ptrdiff_t offset) noexcept
        {
            return *reinterpret_cast<float *>(camera + offset);
        }
    } // namespace

    CameraFrustum read_camera_frustum(std::uintptr_t camera) noexcept
    {
        // The camera is the game view's live CCamera inside its CView, which the detour validated before the build;
        // the reads run under the detour's SEH frame like the writes in commit_view().
        const auto *matrix = reinterpret_cast<const GameStructures::Matrix34f *>(camera);
        const auto *corner = reinterpret_cast<const float *>(camera + Constants::CCAMERA_NEAR_CORNER_OFFSET);
        CameraFrustum frustum;
        frustum.fov = camera_float(camera, Constants::CCAMERA_PROJECTION_FOV_OFFSET);
        frustum.near_corner = {corner[0], corner[1], corner[2]};
        frustum.right = Vector3{matrix->m[0][0], matrix->m[1][0], matrix->m[2][0]};
        frustum.forward = Vector3{matrix->m[0][1], matrix->m[1][1], matrix->m[2][1]};
        frustum.up = Vector3{matrix->m[0][2], matrix->m[1][2], matrix->m[2][2]};
        return frustum;
    }

    std::array<float, 3> committed_near_corner(const CameraFrustum &frustum, const ViewWrite &write) noexcept
    {
        std::array<float, 3> corner = frustum.near_corner;
        if (write.fov_override)
        {
            corner[0] *= write.fov_ratio;
            corner[2] *= write.fov_ratio;
        }
        if (write.near_override)
        {
            corner[0] *= write.near_scale;
            corner[1] *= write.near_scale;
            corner[2] *= write.near_scale;
        }
        return corner;
    }

    void commit_view(std::uintptr_t camera, const ViewWrite &write) noexcept
    {
        auto *matrix = reinterpret_cast<GameStructures::Matrix34f *>(camera);
        matrix->m[0][3] = write.position.x;
        matrix->m[1][3] = write.position.y;
        matrix->m[2][3] = write.position.z;
        if (write.rebuild_basis)
        {
            matrix->m[0][0] = write.right.x;
            matrix->m[1][0] = write.right.y;
            matrix->m[2][0] = write.right.z;
            matrix->m[0][1] = write.forward.x;
            matrix->m[1][1] = write.forward.y;
            matrix->m[2][1] = write.forward.z;
            matrix->m[0][2] = write.up.x;
            matrix->m[1][2] = write.up.y;
            matrix->m[2][2] = write.up.z;
        }
        // A new FOV rescales the x and z of the near and far corners by tan(new/2)/tan(game/2) and the projection
        // plane's distance by the inverse (see CCAMERA_PROJECTION_FOV_OFFSET), so culling matches the rendered view.
        if (write.fov_override)
        {
            camera_float(camera, Constants::CCAMERA_PROJECTION_FOV_OFFSET) = write.fov;
            camera_float(camera, Constants::CCAMERA_CULL_EDGE_0_OFFSET) *= write.fov_ratio;
            camera_float(camera, Constants::CCAMERA_CULL_EDGE_1_OFFSET) *= write.fov_ratio;
            camera_float(camera, Constants::CCAMERA_CULL_TAN_OFFSET) /= write.fov_ratio;
            camera_float(camera, Constants::CCAMERA_CULL_EDGE_3_OFFSET) *= write.fov_ratio;
            camera_float(camera, Constants::CCAMERA_CULL_EDGE_4_OFFSET) *= write.fov_ratio;
        }
        // The builder rebuilds the cull planes and the render projection from the near corner, so scaling it moves the
        // near plane for this build only.
        if (write.near_override)
        {
            auto *corner = reinterpret_cast<float *>(camera + Constants::CCAMERA_NEAR_CORNER_OFFSET);
            corner[0] *= write.near_scale;
            corner[1] *= write.near_scale;
            corner[2] *= write.near_scale;
        }
    }
} // namespace TPVCamera
