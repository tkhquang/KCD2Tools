/**
 * @file render/hdr_output.hpp
 * @brief Highlights under the game's HDR output: where the frame goes, the scene the composite draws into, the scene
 *        copy it reads to add highlights as SDR adds them, and SDR white.
 *
 * Under HDR output the game's HDR Output Transform redraws the whole back buffer from its HDR scene and the UI layer
 * after every after-LDR pass, so engine_silhouette.cpp composites into that scene instead. It reads a copy of the
 * scene so it can add the highlight to each pixel's SDR value, and it places SDR white at the UI brightness, where
 * the transform puts the SDR UI.
 *
 * Threads: the render thread decides, copies and releases; the main thread reads the UI brightness. Shutdown releases
 * once the render path is idle.
 */
#ifndef HENRYSENSES_HDR_OUTPUT_HPP
#define HENRYSENSES_HDR_OUTPUT_HPP

#include <cstdint>

namespace HenrySenses
{
    /**
     * @brief Resolves the HDR calls, names the scene copy for this load and resets the HDR state.
     * @note Init, after the anchors resolve, before the render hooks arm.
     */
    void initialize_hdr_output() noexcept;

    /**
     * @brief Reports whether the frame of the view @p stage draws goes to the screen through the HDR Output Transform:
     *        the render output's HDR flag, the condition the display context itself tests.
     * @param stage The published CSceneCustomStage.
     * @return True under HDR output; false under SDR output or when a read fails.
     * @note Render thread.
     */
    [[nodiscard]] bool hdr_output_active(std::uintptr_t stage) noexcept;

    /**
     * @brief Records whether the composite of this view goes into the HDR scene, and logs a change.
     * @param hdr The composite goes into the HDR scene.
     * @param from_render_output The decision came from the render output ([Render] HdrOutput = Auto).
     * @note Render thread.
     */
    void note_hdr_output(bool hdr, bool from_render_output) noexcept;

    /**
     * @brief Returns the scene the HDR Output Transform draws into the back buffer, for the view @p stage draws.
     * @details The game's own lookup picks it: the upscaler's output when an upscaler runs.
     * @param stage The published CSceneCustomStage.
     * @return The texture, or 0 when the lookup is unavailable or returns no created texture.
     * @note Render thread.
     */
    [[nodiscard]] std::uintptr_t hdr_scene_target(std::uintptr_t stage) noexcept;

    /**
     * @brief Copies @p scene into the mod's scene copy for the composite to read, and logs a change of the composite's
     *        mode.
     * @param pipeline The executing pipeline, which the copy pass is built for.
     * @param scene The HDR Output Transform's scene.
     * @param binding_generation The composite's binding generation (a new pipeline or stage rebinds the pass).
     * @return The copy holding this frame's scene, or 0 when there is none yet or the copy is off for the session.
     * @note Render thread.
     */
    [[nodiscard]] std::uintptr_t
    copy_hdr_scene(std::uintptr_t pipeline, std::uintptr_t scene, std::uint32_t binding_generation) noexcept;

    /**
     * @brief Frees the scene copy when no HDR composite has used it for a while.
     * @note Render thread, right after a composite that bound its own texture where the copy was.
     */
    void release_idle_hdr_scene_copy() noexcept;

    /**
     * @brief Returns SDR white in HDR scene units: the UI brightness over 80 nits.
     * @note Any thread.
     */
    [[nodiscard]] float hdr_sdr_white() noexcept;

    /**
     * @brief Reads the UI brightness while HDR output is on, and logs a change once it settles.
     * @note Main thread, every tick.
     */
    void update_hdr_sdr_white() noexcept;

    /** @brief Scene copies drawn since load (diagnostics). */
    [[nodiscard]] std::uint64_t hdr_scene_copies() noexcept;

    /**
     * @brief Destroys the scene copy's pass and target.
     * @note Shutdown, once the render path is idle.
     */
    void release_hdr_output() noexcept;

} // namespace HenrySenses

#endif // HENRYSENSES_HDR_OUTPUT_HPP
