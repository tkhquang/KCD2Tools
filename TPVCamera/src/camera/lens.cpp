/**
 * @file camera/lens.cpp
 * @brief The third-person lens near plane on the live camera (see lens.hpp).
 */

#include "camera/lens.hpp"
#include "camera/live_probe.hpp"
#include "config.hpp"
#include "core/lens/lens_plan.hpp"
#include "render_occlusion.hpp"
#ifdef TPVCAMERA_DEV_BUILD
#include "dev/frame_recorder.hpp"
#endif

namespace TPVCamera
{
    namespace
    {
        // Main thread only (the camera frame).
        LensState s_lens{};
    } // namespace

    std::optional<float> plan_live_lens_near(const ViewWrite &view, const std::array<float, 3> &near_corner, float arm,
                                             float view_blend, float delta_time, bool repeat_build)
    {
        const LensInput in{
            .position = view.position,
            .right = view.right,
            .forward = view.forward,
            .up = view.up,
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
        if (FrameRecorder::recording())
        {
            return FrameRecorder::lens(s_lens, in, live_probe());
        }
#endif
        return plan_lens_near(s_lens, in, live_probe());
    }
} // namespace TPVCamera
