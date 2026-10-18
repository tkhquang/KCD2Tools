/**
 * @file hooks/occluder_fade.hpp
 * @brief Dithers out the render nodes between the third-person camera and the character.
 *
 * Instead of pulling the camera in front of a post, a fence, a cart or a tree, the camera keeps its distance and the
 * object fades with the engine's own LOD-crossfade dither. The camera code names the nodes that hide the character
 * each frame; this module ramps each node's fade in and out and hands the set to a detour on
 * CObjManager::AddOrCreatePersistentRenderObject, which raises the node's CLodValue dissolve byte in the main view.
 * The engine turns that byte into FOB_DISSOLVE on the node's persistent render objects, so nothing is written to a
 * render object directly and the shadow stays solid.
 */
#ifndef TPVCAMERA_HOOKS_OCCLUDER_FADE_HPP
#define TPVCAMERA_HOOKS_OCCLUDER_FADE_HPP

#include <DetourModKit/error.hpp>
#include <DetourModKit/hook.hpp>

#include <span>

namespace TPVCamera
{

    /**
     * @brief Installs the AddOrCreatePersistentRenderObject detour from the OccluderFade anchor.
     * @details Best-effort: on a miss the camera collides with every object and never fades one.
     * @param hooks The mod's hook stack. The handle is stored before its arm.
     * @return An empty value when the hook was installed, else the library Error (ErrorCode::NoMatch when the anchor
     *         did not resolve).
     * @note Call after resolve_all_anchors().
     */
    [[nodiscard]] DMK::Result<void> initialize_occluder_fade(DMK::hook::HookStack &hooks);

    /** @brief True once the dissolve detour is armed. */
    [[nodiscard]] bool occluder_fade_available() noexcept;

    /** @brief One render node the camera wants faded this frame. */
    struct FadeRequest
    {
        // The IRenderNode.
        void *node = nullptr;
        // How far the node dithers away once fully faded, 0 (not at all) to 1 (almost gone).
        float strength = 0.0f;
        // Start fading without waiting OCCLUDER_FADE_DELAY_SECONDS: for an object at the lens, which should not stay
        // solid while the delay runs. It still fades in over OCCLUDER_FADE_IN_SECONDS.
        bool immediate = false;
        // What the node is beyond its address (render_node_identity()): another object at the address of a node that
        // was freed starts solid instead of taking over that node's fade.
        std::uint64_t identity = 0;
    };

    /**
     * @brief Advances the fade by one camera frame and publishes it to the detour.
     * @details @p requests are the render nodes that hide the character this frame. A node starts to fade only once
     *          it has been listed for OCCLUDER_FADE_DELAY_SECONDS (unless it is immediate), so an object that crosses
     *          a sightline for an instant never blinks. It then fades in toward its strength, is held for a moment
     *          after it stops being listed, and fades back out. An empty span fades everything out. Main thread only
     *          (the camera frame).
     * @param requests The nodes to fade; a node listed twice takes the stronger request, and extras past
     *        OCCLUDER_FADE_MAX_NODES are dropped. A node's strength eases toward its request rather than jumping, and a
     *        node fading back out keeps the strength it last had.
     * @param delta_time Seconds this frame advances.
     */
    void update_occluder_fade(std::span<const FadeRequest> requests, float delta_time) noexcept;

    /**
     * @brief Drops every fade track and unpublishes the set at once.
     * @details For the camera leaving third person: the faded objects are behind the first-person eye, and a later
     *          return must not republish node addresses from before the gap. Main thread only.
     */
    void reset_occluder_fade() noexcept;

} // namespace TPVCamera

#endif // TPVCAMERA_HOOKS_OCCLUDER_FADE_HPP
