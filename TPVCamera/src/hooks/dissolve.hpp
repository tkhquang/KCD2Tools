/**
 * @file hooks/dissolve.hpp
 * @brief The engine's CLodValue dissolve byte and the main-view pass test the render detours share.
 * @details Every helper here runs inside a render detour on an engine job thread: no allocation, no logging, and it
 *          reads only engine data that is valid for the call.
 */
#ifndef TPVCAMERA_HOOKS_DISSOLVE_HPP
#define TPVCAMERA_HOOKS_DISSOLVE_HPP

#include "constants.hpp"

#include <algorithm>
#include <cstdint>

namespace TPVCamera
{

    /** @brief True for the main view: not a recursive (mirror), shadow-map or aux-window pass. */
    [[nodiscard]] inline bool main_view_pass(const std::uint8_t *pass) noexcept
    {
        return pass[Constants::PASS_INFO_RECURSION_OFFSET] == 0 && pass[Constants::PASS_INFO_SHADOW_OFFSET] == 0 &&
               pass[Constants::PASS_INFO_AUX_WINDOW_OFFSET] == 0;
    }

    /**
     * @brief Combines @p ours with the engine's own LOD-crossfade value in the CLodValue at @p lod.
     * @details With LodA set (the "out" half, FOB_DISSOLVE_OUT) a higher byte hides more, so the larger wins; for the
     *          LOD fading in (LodA -1, LodB set, no OUT) a lower byte hides more, so the byte is lowered.
     */
    inline void raise_dissolve(std::uint8_t *lod, std::uint8_t ours) noexcept
    {
        const auto lod_a = *reinterpret_cast<const std::int16_t *>(lod + Constants::CLODVALUE_LOD_A_OFFSET);
        const auto lod_b = *reinterpret_cast<const std::int16_t *>(lod + Constants::CLODVALUE_LOD_B_OFFSET);
        std::uint8_t &ref = lod[Constants::CLODVALUE_DISSOLVE_OFFSET];
        if (lod_a >= 0)
        {
            ref = std::max(ref, ours);
        }
        else if (lod_b >= 0 && ref != 0)
        {
            ref = std::max<std::uint8_t>(1, std::min<std::uint8_t>(ref, static_cast<std::uint8_t>(255 - ours)));
        }
    }

} // namespace TPVCamera

#endif // TPVCAMERA_HOOKS_DISSOLVE_HPP
