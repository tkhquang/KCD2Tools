/**
 * @file camera/lens.cpp
 * @brief The third-person lens near plane on the live camera (see lens.hpp).
 */

#include "camera/lens.hpp"
#include "camera/live_probe.hpp"
#include "config.hpp"
#include "constants.hpp"
#include "core/lens/lens_plan.hpp"
#include "render_occlusion.hpp"
#ifdef TPVCAMERA_DEV_BUILD
#include "dev/frame_recorder.hpp"
#endif

#include <optional>

namespace TPVCamera
{
    namespace
    {
        // Main thread only (the camera frame).
        LensState s_lens{};
    } // namespace

    void apply_lens_near_plane(std::uintptr_t camera, const GameStructures::Matrix34f &view, float arm,
                               float view_blend, float delta_time, bool repeat_build)
    {
        auto *near_corner = reinterpret_cast<float *>(camera + Constants::CCAMERA_NEAR_CORNER_OFFSET);
        const LensInput in{
            .position = Vector3{view.m[0][3], view.m[1][3], view.m[2][3]},
            .right = Vector3{view.m[0][0], view.m[1][0], view.m[2][0]},
            .forward = Vector3{view.m[0][1], view.m[1][1], view.m[2][1]},
            .up = Vector3{view.m[0][2], view.m[1][2], view.m[2][2]},
            .corner_x = near_corner[0],
            .game_near = near_corner[1],
            .corner_z = near_corner[2],
            .arm = arm,
            .view_blend = view_blend,
            .delta_time = delta_time,
            .rules_generation = occluder_rules_generation(),
            .repeat_build = repeat_build,
            .use_lens_cut = settings().use_lens_cut.load(std::memory_order_relaxed),
        };
#ifdef TPVCAMERA_DEV_BUILD
        const std::optional<float> near_plane = FrameRecorder::recording()
                                                    ? FrameRecorder::lens(s_lens, in, live_probe())
                                                    : plan_lens_near(s_lens, in, live_probe());
#else
        const std::optional<float> near_plane = plan_lens_near(s_lens, in, live_probe());
#endif
        if (!near_plane.has_value())
        {
            return;
        }
        // The builder rebuilds the cull planes and the render projection from the near corner, which SetFrustum writes
        // fresh before every build, so scaling it moves the near plane for this build only.
        const float scale = *near_plane / in.game_near;
        near_corner[0] *= scale;
        near_corner[1] *= scale;
        near_corner[2] *= scale;
    }
} // namespace TPVCamera
