/**
 * @file render/engine_silhouette.hpp
 * @brief Revived engine silhouette pass: mesh-accurate, per-object coloured, see-through outlines.
 *
 * The retail standard pipeline never instantiates CSceneCustomStage, so the HUD silhouette word on a render node
 * draws nothing on its own. This backend restores the missing pieces from surviving retail building blocks:
 *   - P1: register the custom stage on the standard pipeline at the ENTRY of CStandardGraphicsPipeline::Init, so the
 *     base Init's stage loop initializes it with the others (registering after Init would leave it uninitialized and
 *     make every object fail to compile). When the hook arrives after the pipeline exists, the stage is registered
 *     and initialized from the render thread instead (late attach).
 *   - P2: CSceneCustomStage::CreatePipelineState returns success with a null PSO for the debug passes (0 to 2), so no
 *     debug shader is compiled, and optionally for a failed silhouette pass, so a failure can never hide an object.
 *   - P3: inject the node's word (IRenderNode + 0x3C) into SRendParams + 0x78 around CRenderProxy::Render.
 *   - P4: after CSceneForwardStage::ExecuteAfterPostProcessHDR, draw render list 27 into the silhouette mask
 *     ($SceneNormalsMap) and composite it into $DisplayTargetDst with the shipped DeferredSilhouettesOptimised
 *     technique (C1).
 * Every step logs its result (tagged A1 to C1 in the log lines), so the log alone shows which step works.
 *
 * The stage outlives the module that registered it: the pipeline owns it and compiled objects keep PSOs built from its
 * passes, so it is never freed. A module that unloads first unpublishes it (P+0x50 back to null, the retail state) and
 * records it in the dev loader's persistent slots, and the next generation adopts that same object instead of
 * registering and initializing a second one.
 */
#ifndef HENRYSENSES_ENGINE_SILHOUETTE_HPP
#define HENRYSENSES_ENGINE_SILHOUETTE_HPP

#include "hooks/hook_set.hpp"

#include <DetourModKit/error.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace HenrySenses
{
    /**
     * @struct SilhouetteFrameParams
     * @brief Per-frame values the main-thread tick publishes to the render-thread composite and the injection hook.
     */
    struct SilhouetteFrameParams
    {
        /// Global highlight intensity in [0, 1] (pulse fade); scales the composite strength.
        float intensity{1.0f};
        /// Opacity of the tint inside every outline (a Style = OutlineFill group shows), or 0 for edges only.
        float interior_opacity{0.0f};
    };

    /**
     * @brief Binds the loader-owned slots that carry an unpublished stage from one generation to the next.
     * @details Validates the layout tag the slots carry, discards contents written under another layout, and logs
     *          every stage a previous generation left for adoption. An empty span (the release ASI) disables the
     *          stash: the stage is still unpublished on shutdown, but nothing can adopt it.
     * @param slots The persistent slots, valid for the whole process, or an empty span.
     * @note Call once from init(), before install_engine_silhouette_early().
     */
    void bind_engine_stage_stash(std::span<std::uint64_t> slots) noexcept;

    /**
     * @brief Installs P1 (pipeline Init) and P2 (CreatePipelineState) from the early anchor phase.
     * @param hooks The mod's hook set; the handles are pushed first so they are removed last.
     * @return An empty value when both hooks are armed, or the Error that refused an install.
     * @note Runs before the rest of init so the stage can be registered before the renderer builds its pipeline.
     */
    [[nodiscard]] DMK::Result<void> install_engine_silhouette_early(HookSet &hooks);

    /**
     * @brief Binds the render-thread calls and installs P3 (word injection) and P4 (mask draw and composite).
     * @param hooks The mod's hook set.
     * @return An empty value when the backend is installed, or the Error that refused an install.
     */
    [[nodiscard]] DMK::Result<void> initialize_engine_silhouette(HookSet &hooks);

    /**
     * @brief Takes every live stage off its pipeline on the render thread and records it for the next generation.
     * @details P4 performs the unpublish inside CStandardGraphicsPipeline::Execute, the only point where no engine
     *          reader of P+0x50 is between its null test and its re-read, and stashes the stage before the store. The
     *          call waits (bounded) for every pipeline that still holds a stage this generation saw or published,
     *          however long ago it last rendered. A pipeline that renders no frame within @p timeout keeps its stage
     *          published, so the current generation must retain its guard hooks and session. P1 registers nothing, and
     *          P4
     *          neither adopts nor late-attaches.
     * @param timeout Upper bound of the wait for the render thread.
     * @return The number of stages left published.
     * @note Teardown thread, after the main thread restored the nodes and before any hook is disabled, so P2 still
     *       guards every stage that stays published.
     */
    [[nodiscard]] std::size_t unpublish_engine_stage(std::chrono::milliseconds timeout) noexcept;

    /**
     * @brief Makes every detour pass straight through to the original, ahead of hook removal.
     */
    void disarm_engine_silhouette() noexcept;

    /**
     * @brief Waits (bounded) until no thread is inside a render-path detour; call after the hooks were disabled and
     *        before they are destroyed.
     * @return True when every detour drained; false means a thread may still return through a trampoline.
     */
    [[nodiscard]] bool shutdown_engine_silhouette() noexcept;

    /**
     * @brief Reports whether a registered custom stage was seen on the executing pipeline recently and every mask
     *        and composite call resolved.
     * @return True when silhouette words can render.
     */
    [[nodiscard]] bool engine_silhouette_available() noexcept;

    /**
     * @brief Reports whether static brushes can be outlined too (engine_silhouette_available() and the CBrush::Render
     *        hook is installed).
     * @return True when a brush's silhouette word renders.
     */
    [[nodiscard]] bool engine_brush_outline_available() noexcept;

    /**
     * @brief Reports whether the configuration asks for the custom stage ([Render] Backend=Engine or Auto).
     * @return True when the stage should be registered.
     */
    [[nodiscard]] bool stage_registration_wanted() noexcept;

    /**
     * @brief The render-object flags every entity and every highlighted object carries: the mod's marker, plus
     *        FOB_HUD_REQUIRE_DEPTHTEST while [Render] SeeThrough is off, so walls and grass hide the occluded part.
     * @details The flags reach a persistent render object only when it is filled, so a SeeThrough change needs the
     *          highlighted nodes' render objects refreshed; compiled objects that keep a silhouette PSO built for the
     *          other setting are rebuilt by the mask pass.
     * @return The flags to OR into SRendParams::dwFObjFlags or CRenderObject::m_ObjFlags.
     */
    [[nodiscard]] std::uint64_t silhouette_object_flags() noexcept;

    /**
     * @brief Publishes the per-frame parameters from the main-thread tick.
     * @param params The values.
     */
    void publish_silhouette_params(const SilhouetteFrameParams &params) noexcept;

    /**
     * @brief Starts or stops the focus coverage mask: each frame the render thread draws, before the post-effect stage,
     *        red = 1 on the background and 0 on highlighted objects into $SceneDiffuseTmp, the mask the game's
     *        VisualArtifacts effect darkens by (render/focus_effect).
     */
    void set_focus_coverage_wanted(bool wanted) noexcept;

    /**
     * @brief The name the engine finds the focus coverage texture by, or empty while no stage renders.
     * @note Main thread.
     */
    [[nodiscard]] std::string focus_coverage_texture_name();

    /** @brief Frames the focus coverage mask was drawn in (diagnostics). */
    [[nodiscard]] std::uint64_t focus_coverage_frames() noexcept;

    /**
     * @brief Logs the pipeline, stage, hook and counter state (the state report).
     */
    void log_engine_silhouette_state();

} // namespace HenrySenses

#endif // HENRYSENSES_ENGINE_SILHOUETTE_HPP
