/**
 * @file camera/commit.hpp
 * @brief The single writer of the game view camera's per-build values: the matrix, the FOV with the cull edges it
 *        scales, and the near corner.
 *
 * @details The builder computes the cull planes and the render projection from the FOV, the near, projection and
 *          far corners and the matrix right after the frustum detour returns. The frame's stages therefore work on
 *          values: the frustum as this build found it (CameraFrustum, read once: SetFrustum's values on the frame's
 *          first build, what the first build committed on its second) and what each stage decided (ViewWrite), and
 *          commit_view() writes them once, at the end of the build. No stage reads the camera's memory back after
 *          another stage changed it. Main thread only (the camera detour).
 */
#ifndef TPVCAMERA_CAMERA_COMMIT_HPP
#define TPVCAMERA_CAMERA_COMMIT_HPP

#include "math_utils.hpp"

#include <array>
#include <cstdint>

namespace TPVCamera
{
    /** @brief The game view camera's FOV, near corner and basis as this build found them. */
    struct CameraFrustum
    {
        float fov = 0.0f;                   // radians
        std::array<float, 3> near_corner{}; // {x, near distance, z} in camera space (see CCAMERA_NEAR_CORNER_OFFSET)
        Vector3 right{};                    // the engine's eye basis: matrix columns 0, 1 and 2
        Vector3 forward{};
        Vector3 up{};
    };

    /** @brief Reads @p camera's frustum as this build found it. */
    [[nodiscard]] CameraFrustum read_camera_frustum(std::uintptr_t camera) noexcept;

    /** @brief What this build writes into the game view camera. */
    struct ViewWrite
    {
        Vector3 position{}; // the matrix translation
        // The matrix basis, written only when rebuild_basis; otherwise the engine's eye basis stays (these hold it).
        Vector3 right{};
        Vector3 forward{};
        Vector3 up{};
        bool rebuild_basis = false;
        // The FOV (radians) and the ratio tan(fov / 2) / tan(game / 2) the cull edges scale by.
        float fov = 0.0f;
        float fov_ratio = 1.0f;
        bool fov_override = false;
        // The near plane's distance over the game's: the near corner scales by it as a whole.
        float near_scale = 1.0f;
        bool near_override = false;
    };

    /**
     * @brief The near corner the build ends with: the one it found, its x and z scaled by the FOV ratio, then all of
     *        it by the near plane's scale, in the order commit_view() applies them, so the values match bit for bit.
     */
    [[nodiscard]] std::array<float, 3> committed_near_corner(const CameraFrustum &frustum,
                                                             const ViewWrite &write) noexcept;

    /** @brief Writes @p write into @p camera: the matrix, then the FOV and the cull edges, then the near corner. */
    void commit_view(std::uintptr_t camera, const ViewWrite &write) noexcept;
} // namespace TPVCamera

#endif // TPVCAMERA_CAMERA_COMMIT_HPP
