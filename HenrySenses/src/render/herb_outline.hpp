/**
 * @file render/herb_outline.hpp
 * @brief Engine silhouettes for pickable herbs.
 *
 * A herb is one instance inside a merged-mesh vegetation cell, drawn in one batch with every other plant sharing its
 * material, so it has no render object of its own to carry a silhouette word; a pickable mushroom is a CVegetation
 * node, whose render never copies the node's silhouette word into its render object. Once per frame, from the
 * static-mesh render hook, each nearby unpicked plant the camera can see, and the engine still draws at that distance,
 * is submitted again through CStatObj::Render at the exact transform the engine draws it with: a temporary, all but
 * transparent render object that carries the silhouette word, marker and camera distance, so the plant shows its
 * outline while the scene keeps the swaying original. Nothing is created in the engine, so nothing needs to be
 * released.
 */
#ifndef HENRYSENSES_HERB_OUTLINE_HPP
#define HENRYSENSES_HERB_OUTLINE_HPP

#include "game_structures.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace HenrySenses
{
    /**
     * @struct HerbOutlineItem
     * @brief One herb to outline.
     */
    struct HerbOutlineItem
    {
        game_structures::Matrix34f world{};
        std::uintptr_t stat_obj{0};
        /// The silhouette word (styled and faded); 0 skips the plant.
        std::uint32_t word{0};
    };

    /**
     * @brief Resolves CStatObj::Render.
     * @return True when herbs can be outlined (the static-mesh render hook must be installed as well).
     */
    bool initialize_herb_outline() noexcept;

    /**
     * @brief Drops the published set and releases the submission TLS slot.
     * @note Call only after the render hooks have been disabled and drained.
     */
    void shutdown_herb_outline() noexcept;

    /**
     * @brief Publishes the herbs to outline from the next frame on; an empty span stops outlining.
     * @details A set equal to the published one (same plants and words) is not published again.
     * @note Main thread.
     */
    void publish_herb_outline(std::span<const HerbOutlineItem> items);

    /** @brief Number of herbs in the published set. */
    [[nodiscard]] std::size_t herb_outline_count() noexcept;

    /**
     * @brief The silhouette word of the herb this thread is submitting, or 0 outside a submission.
     * @details Read by the static-mesh render hook, which writes it into the temporary render object.
     */
    [[nodiscard]] std::uint32_t herb_outline_thread_word() noexcept;

    /**
     * @brief Submits the published herbs once per frame, on the first general-pass static-mesh render.
     * @param pass_info The SRenderingPassInfo of that render.
     * @note Called by the static-mesh render hook on 3D-engine job threads.
     */
    void herb_outline_on_render(const void *pass_info) noexcept;

} // namespace HenrySenses

#endif // HENRYSENSES_HERB_OUTLINE_HPP
