/**
 * @file indoor_state.hpp
 * @brief Whether a point is inside a building: the level's clip volumes, the interiors the engine lights as indoors.
 */
#ifndef TPVCAMERA_INDOOR_STATE_HPP
#define TPVCAMERA_INDOOR_STATE_HPP

#include "math_utils.hpp"

namespace TPVCamera
{
    /**
     * @brief Resolves the clip-volume manager global and its point query (Feature::IndoorState).
     * @details Best-effort: on a miss point_indoors() answers false, so a preset bound to `indoors` never applies.
     * @note Setup only, after resolve_all_anchors() and before the camera hook is armed.
     */
    void initialize_indoor_state();

    /**
     * @brief True when @p point is inside one of the level's active clip volumes (a building interior).
     * @details One call of the engine's own point query (CClipVolumeManager::GetClipVolumeByPos: a grid lookup and the
     *          volume's mesh test) on a manager whose vtable RTTI proves, under an SEH guard, so a level unload or a
     *          layout drift answers false. Logs at Trace each time the volume that holds the point changes, with its
     *          name, flags and bounds; the volume's address is kept for that comparison only, never read again.
     * @note Main thread only (the engine changes its clip volumes there).
     */
    [[nodiscard]] bool point_indoors(const Vector3 &point) noexcept;
} // namespace TPVCamera

#endif // TPVCAMERA_INDOOR_STATE_HPP
