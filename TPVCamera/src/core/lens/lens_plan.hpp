/**
 * @file core/lens/lens_plan.hpp
 * @brief The third-person lens near plane: how far out the near clip plane goes so what touches the lens is cut away.
 *
 * @details Engine-free (no Windows or DetourModKit header): the world is asked through a WorldProbe, so a recorded
 *          frame replays exactly (core/collision/probe_record.hpp). camera/lens.hpp reads the camera and scales its
 *          near corner by the plane this plans.
 */
#ifndef TPVCAMERA_CORE_LENS_LENS_PLAN_HPP
#define TPVCAMERA_CORE_LENS_LENS_PLAN_HPP

#include "core/collision/world_probe.hpp"
#include "core/math/vec.hpp"

#include <cstdint>
#include <optional>

namespace TPVCamera
{
    /** @brief The last measured clearance and the pose, view and rule set it was measured for. */
    struct LensSample
    {
        Vector3 position{};
        Vector3 forward{};
        Vector3 up{};
        float tan_half_width = 0.0f;
        float tan_half_height = 0.0f;
        std::uint64_t rules_generation = 0;
        std::uint64_t stamp_ms = 0;
        float solid = 0.0f;
        bool valid = false;
    };

    /** @brief The lens near plane's carry-over between camera frames. Main thread only. */
    struct LensState
    {
        // The near distance the last measured game-view frame eased to; 0 while the plane is the game's own.
        float near = 0.0f;
        LensSample sample{};
    };

    /** @brief One build's input to the lens near plane. */
    struct LensInput
    {
        // The final view: its position and its right, forward and up axes (the matrix columns).
        Vector3 position{};
        Vector3 right{};
        Vector3 forward{};
        Vector3 up{};
        // The near corner as SetFrustum wrote it for this build (+x half width, y the near distance, +z half height).
        float corner_x = 0.0f;
        float game_near = 0.0f;
        float corner_z = 0.0f;
        float arm = 0.0f; // the arm after collision, from the head
        float view_blend = 0.0f;
        float delta_time = 0.0f;
        std::uint64_t rules_generation = 0; // occluder_rules_generation()
        bool repeat_build = false;
        bool use_lens_cut = true; // UseLensCut
    };

    /**
     * @brief The near plane this build of the camera uses, or std::nullopt to leave the game's own.
     * @details In third person the near clip plane is pushed out to k_lens_near_max, so a leaf, a branch or the corner
     *          of a prop right at the lens is cut away pixel by pixel instead of filling the view. It is held
     *          k_lens_near_margin short of the ground or a building, so the camera never sees through a wall, and
     *          k_lens_near_character_margin short of the character's head. The clearance is measured once per frame
     *          from the final pose and the same plane is used for the frame's second build; it eases with the view
     *          blend, so first person keeps the game's plane.
     */
    [[nodiscard]] std::optional<float> plan_lens_near(LensState &state, const LensInput &in, WorldProbe &probe);
} // namespace TPVCamera

#endif // TPVCAMERA_CORE_LENS_LENS_PLAN_HPP
