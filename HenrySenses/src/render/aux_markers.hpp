/**
 * @file render/aux_markers.hpp
 * @brief Aux-geom markers (corner brackets) around loot the engine silhouette cannot draw.
 *
 * The aux-geom command buffer is fetched every frame through CD3D9Renderer::GetIRenderAuxGeom (gEnv + 0x108,
 * vtable slot 199), which returns the calling thread's own CAuxGeomCB; it is never cached because the collector swaps
 * buffers at the end of every frame. Markers are drawn without depth write and, unless [Render] SeeThrough is off,
 * without the depth test, so they stay visible through walls; they back up materials without a custom-render technique
 * (glass, invisible StashCorpse meshes) and serve as the whole highlight when Backend=Markers. Every slot is validated
 * against its AOB-resolved function and every call runs under SEH.
 */
#ifndef HENRYSENSES_AUX_MARKERS_HPP
#define HENRYSENSES_AUX_MARKERS_HPP

#include "game_structures.hpp"

#include <DetourModKit/error.hpp>

#include <cstdint>
#include <span>

namespace HenrySenses
{
    /**
     * @struct MarkerRequest
     * @brief One marker to draw this frame.
     */
    struct MarkerRequest
    {
        /// World-space bounds of the highlighted entity.
        game_structures::Aabb bounds{};
        /// Packed 0xRRGGBBAA colour (the alpha byte is replaced by the marker opacity).
        std::uint32_t color_word{0};
    };

    /**
     * @brief Checks that the aux-geom validators resolved.
     * @return An empty value when markers can be drawn, or NoMatch naming the missing anchor.
     */
    [[nodiscard]] DMK::Result<void> initialize_aux_markers();

    /** @brief Marks markers unavailable. */
    void shutdown_aux_markers() noexcept;

    /**
     * @brief Reports whether markers can be drawn.
     * @return True once initialize_aux_markers() succeeded and no draw has faulted.
     */
    [[nodiscard]] bool aux_markers_available() noexcept;

    /**
     * @brief Queues corner brackets for this frame.
     * @param markers The markers to draw.
     * @param opacity Marker opacity in [0, 1] (pulse fade).
     * @return The number of markers queued.
     * @note Main thread only (the aux-geom buffer is per thread).
     */
    [[nodiscard]] std::size_t draw_markers(std::span<const MarkerRequest> markers, float opacity) noexcept;

} // namespace HenrySenses

#endif // HENRYSENSES_AUX_MARKERS_HPP
