/**
 * @file render/engine_silhouette.cpp
 * @brief P1 stage registration, P2 PSO guard, P3 word injection, P4 mask draw and C1 composite.
 *
 * Threads: P1 runs on the pipeline-creation thread before the pipeline is published; P2 on the render thread and
 * the parallel object-compile jobs; P3 on the main thread and the 3D-engine job threads; P4 on the render thread
 * inside CStandardGraphicsPipeline::Execute. The detours are lock-free; every engine call they make runs in a POD
 * helper under SEH, and a fault latches the render path off for the session.
 *
 * P+0x50 transitions. Two engine readers test the slot and then read it again: the per-frame stage Update loop
 * (pipeline vtable slot 5) re-reads each slot after IsStageActive, and the object-compile PSO block tests the slot
 * before it reloads the stage it calls CreatePipelineState on. A 0 -> stage store is safe for both from any thread,
 * because the re-read can only see the stage. A stage -> 0 store is not: between the test and the re-read the reader
 * would call through null. Both readers run before CStandardGraphicsPipeline::Execute (compiles finish before it,
 * Update runs ahead of it on the render thread), so the mod unpublishes only from P4, inside Execute, where the
 * slot is not being read. Publishing (late attach, adoption) happens at the same point, so stage Update never runs
 * with a render view left over from an earlier frame. The one reader left in Execute after P4, the editor-highlight
 * gate, loads the slot after P4 returns and is false in game.
 */

#include "render/engine_silhouette.hpp"
#include "render/herb_outline.hpp"
#include "render/silhouette_shader.hpp"
#include "aob_resolver.hpp"
#include "config.hpp"
#include "constants.hpp"
#include "global_state.hpp"
#include "rtti_types.hpp"
#include "engine/engine_env.hpp"
#include "engine/seh.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <malloc.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace HenrySenses
{
    namespace
    {
        using StdInitFn = void(__fastcall *)(std::uintptr_t pipeline);
        using RegisterStageFn = std::uintptr_t(__fastcall *)(std::uintptr_t pipeline, std::uintptr_t unused);
        using CreatePsoFn = bool(__fastcall *)(void *stage, const void *desc, std::uint8_t pass_id, void **out_pso);
        using ProxyRenderFn = void(__fastcall *)(void *self, void *params, const void *pass_info);
        using AfterPostHdrFn = void(__fastcall *)(void *forward_stage);
        using StageInitFn = void(__fastcall *)(std::uintptr_t stage);
        using ClearSurfaceFn = void(__fastcall *)(std::uintptr_t pass, std::uintptr_t texture, const float *color);
        using PrepareRenderPassFn = void(__fastcall *)(std::uintptr_t pass, std::uintptr_t command_list);
        using DrawRenderItemsFn = void(__fastcall *)(
            std::uintptr_t pass,
            std::uintptr_t render_view,
            std::uint8_t list,
            std::int32_t start,
            std::int32_t end
        );
        using DrawerFn = void(__fastcall *)(std::uintptr_t drawer);
        using SetTechniqueFn = void(__fastcall *)(
            std::uintptr_t primitive,
            std::uintptr_t shader,
            const std::uint32_t *technique_crc,
            std::uint64_t rt_mask,
            bool apply_quality
        );
        using SetRenderTargetFn =
            bool(__fastcall *)(std::uintptr_t pass, std::uint32_t slot, std::uintptr_t texture, std::uint8_t view);
        using SetTextureFn = void(__fastcall *)(
            std::uintptr_t desc,
            std::uint8_t slot,
            std::uintptr_t texture,
            std::uint8_t view,
            std::uint16_t stages
        );
        using SetSamplerFn = std::uint32_t(__fastcall *)(
            std::uintptr_t desc,
            std::uint8_t slot,
            std::uint16_t sampler,
            std::uint32_t stages
        );
        using BeginConstantUpdateFn = void(__fastcall *)(std::uintptr_t pass);
        using SetConstantFn =
            bool(__fastcall *)(std::uintptr_t pass, const void *name, const float *value, std::uint8_t shader_class);
        using FullscreenExecuteFn = bool(__fastcall *)(std::uintptr_t pass);
        using CryNameRFn = void *(__fastcall *)(void *self, const char *name);
        using DisplayTargetFn = std::uintptr_t(__fastcall *)(std::uintptr_t resources, std::uint32_t stage);
        using SceneSetRenderTargetsFn = void(__fastcall *)(
            std::uintptr_t pass,
            std::uintptr_t depth,
            std::uintptr_t color0,
            std::uintptr_t color1,
            std::uintptr_t color2,
            std::uintptr_t color3
        );
        // SPostEffectsUtils::GetOrCreateRenderTarget / GetOrCreateDepthStencil: creates the named target into *slot,
        // or resizes the one already there; true when it has a device texture.
        using PostFxTargetFn = bool(__fastcall *)(
            const char *name,
            std::uintptr_t *slot,
            std::int32_t width,
            std::int32_t height,
            const float *clear,
            bool use_alpha,
            bool mips,
            std::uint8_t format,
            std::int32_t custom_id,
            std::uint32_t flags
        );
        using ClearDepthFn = void(__fastcall *)(
            std::uintptr_t pass,
            std::uintptr_t texture,
            std::int32_t flags,
            float depth,
            std::uint8_t stencil
        );
        using TextureReleaseFn = std::int32_t(__fastcall *)(std::uintptr_t texture);

        /**
         * @brief Standard CRC-32 of the lower-cased text: the engine's CCryNameTSCRC technique key.
         * @param text Technique name.
         * @return The key.
         */
        [[nodiscard]] constexpr std::uint32_t technique_crc(std::string_view text) noexcept
        {
            std::uint32_t crc = 0xFFFFFFFFu;
            for (const char c : text)
            {
                const auto byte = static_cast<std::uint8_t>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
                crc ^= byte;
                for (int bit = 0; bit < 8; ++bit)
                {
                    crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
                }
            }
            return ~crc;
        }

        constexpr std::uint32_t DEFERRED_SILHOUETTES_CRC = technique_crc("DeferredSilhouettesOptimised");
        static_assert(DEFERRED_SILHOUETTES_CRC == 0x6BB4DF7Cu, "Technique key must match the shipped permutation.");
        // The mod's own composite technique (render/silhouette_shader.hpp).
        constexpr std::uint32_t HENRY_SENSES_SILHOUETTE_CRC = technique_crc(constants::SILHOUETTE_TECHNIQUE);
        constexpr std::uint32_t HENRY_SENSES_COVERAGE_CRC = technique_crc(constants::SILHOUETTE_COVERAGE_TECHNIQUE);
        // Composites the mod's technique may fail (its permutation still compiling) before it has drawn once; past
        // them the stock technique is kept for the session.
        constexpr std::uint32_t CUSTOM_WARMUP_ATTEMPTS = 900;

        // Composite constants that are not user settings: BOOST and BRIGHTNESS of the shipped shader.
        constexpr float COMPOSITE_BOOST = 1.7f;
        constexpr float COMPOSITE_BRIGHTNESS = 1.333f;
        // A registered stage counts as live while P4 saw it within this window.
        constexpr std::int64_t STAGE_LIVE_WINDOW_MS = 2000;
        // Period of the render-path statistics line (LogLevel = DEBUG).
        constexpr std::int64_t STATS_INTERVAL_MS = 5000;
        // Upper bound of the shutdown wait for in-flight detours; a cold-cache PSO compile inside P2 or a loading
        // stall inside P4 can hold a detour for a long time.
        constexpr std::int64_t RUNDOWN_TIMEOUT_MS = 10000;
        // Settle time after the counters read zero: a thread that took a patch jump just before the patch went away
        // has not counted itself yet.
        constexpr DWORD ENTRY_WINDOW_MS = 50;
        // P3 in-flight stripes; P3 runs for every visible proxy on every job thread, so one shared counter would
        // bounce a cache line between them.
        constexpr std::size_t P3_STRIPES = 32;
        constexpr std::size_t CACHE_LINE = 64;
        // Standard pipelines tracked per generation (the default one, plus any the game creates for extra views).
        constexpr std::size_t TRACKED_PIPELINES = 4;
        // Stash layout in the loader's persistent slots: slot 0 holds the tag, then one entry per pipeline.
        constexpr std::uint64_t STASH_LAYOUT_TAG = UINT64_C(0x4853535441534831); // ASCII "HSSTASH1"
        constexpr std::size_t STASH_ENTRIES = 4;
        constexpr std::size_t STASH_ENTRY_SLOTS = 3; // pipeline, stage, forward stage
        constexpr std::size_t STASH_SLOT_COUNT = 1 + STASH_ENTRIES * STASH_ENTRY_SLOTS;
        // The slot after the stash entries keeps the PSO depth table for the process (bind_pso_depth_table). The tag
        // does not cover it, so a layout change never drops it.
        constexpr std::size_t STASH_PSO_DEPTH_SLOT = STASH_SLOT_COUNT;

        /**
         * @brief Every render-thread call and data slot, resolved once at init and read-only afterwards.
         */
        struct RenderCalls
        {
            std::uintptr_t register_stage{0};
            std::uintptr_t clear_surface{0};
            std::uintptr_t prepare_render_pass{0};
            std::uintptr_t draw_render_items{0};
            std::uintptr_t jobify{0};
            std::uintptr_t wait{0};
            std::uintptr_t set_technique{0};
            std::uintptr_t set_render_target{0};
            std::uintptr_t set_texture{0};
            std::uintptr_t set_sampler{0};
            std::uintptr_t begin_constant_update{0};
            std::uintptr_t set_constant{0};
            std::uintptr_t fullscreen_execute{0};
            std::uintptr_t cry_name_r{0};
            std::uintptr_t display_target{0};
            std::uintptr_t ldr_target{0};
            std::uintptr_t core_command_list_slot{0};
            std::uintptr_t recursion_counter{0};
            std::uintptr_t post_effects_game_slot{0};
            std::uintptr_t scene_set_render_targets{0};
            std::uintptr_t post_fx_render_target{0};
            std::uintptr_t post_fx_depth_stencil{0};
            std::uintptr_t clear_depth{0};
        };

        RenderCalls s_calls{};
        std::atomic<bool> s_mask_calls_ready{false};
        std::atomic<bool> s_composite_calls_ready{false};

        // CClearSurfacePass depth-stencil clear flags: depth and stencil.
        constexpr std::int32_t CLEAR_DEPTH_AND_STENCIL = 3;

        // The supersampled mask: the mod's own colour and depth targets the mask pass draws into instead of
        // $SceneNormalsMap and the view's depth target, and the blur's intermediate of the same size. Created on the
        // render thread, resized there with the output, released at shutdown once the render path is idle.
        struct SupersampledTargets
        {
            std::uintptr_t mask{0};
            std::uintptr_t depth{0};
            std::uintptr_t temp{0};
        };
        SupersampledTargets s_ss{};
        // Their names carry a per-load token: a reloaded generation never finds the texture its predecessor released.
        std::array<char, 48> s_ss_mask_name{};
        std::array<char, 48> s_ss_depth_name{};
        std::array<char, 48> s_ss_temp_name{};
        std::atomic<bool> s_ss_calls_ready{false};
        // Latched when creating or drawing into the targets failed; the render-resolution mask is used from then on.
        std::atomic<bool> s_ss_faulted{false};
        std::atomic<std::uint64_t> s_ss_masks{0};

        /**
         * @struct MaskVisibility
         * @brief The depth-tested mask ([Render] SeeThrough = false) that says which parts of the supersampled
         *        silhouettes are visible, and the UV offset that undoes its camera jitter.
         */
        struct MaskVisibility
        {
            std::uintptr_t texture{0};
            float offset_u{0.0f};
            float offset_v{0.0f};
        };

        // P6 -> P4: the depth-tested mask drawn before the upscaler, where the camera still matches the scene depth,
        // and the view it was drawn for (render thread only).
        std::uintptr_t s_tested_mask_view{0};
        std::uintptr_t s_tested_mask_texture{0};

        /**
         * @struct CompositeHandoff
         * @brief P4 -> P5: what P4 drew for the view, composited by P5 (render thread only).
         */
        struct CompositeHandoff
        {
            std::uintptr_t view{0};
            std::uintptr_t mask{0};
            MaskVisibility visibility{};
            // The mask is the mod's supersampled target (its blur uses s_ss.temp, not $SceneDiffuseTmp).
            bool supersampled{false};
        };
        CompositeHandoff s_handoff{};

        // Trampolines, published before each hook is armed.
        StdInitFn s_std_init_original = nullptr;
        CreatePsoFn s_create_pso_original = nullptr;
        ProxyRenderFn s_proxy_render_original = nullptr;
        /**
         * @brief Where in the frame the silhouette is composited: always after tone-mapping, so a highlight adds its
         *        own colour whatever the scene's exposure. A composite into the HDR scene before the upscaler went
         *        through exposure, tone curve, colour grading and bloom (pale at dusk, near white at night) and so
         *        looked different from the same highlight composited here.
         */
        enum class CompositePoint : std::uint8_t
        {
            /// After HDR post-processing (P4), into the display target.
            AfterHdr,
            /// After LDR post-processing (P5), into the render output.
            AfterLdr,
        };

        [[nodiscard]] constexpr const char *composite_point_name(CompositePoint point) noexcept
        {
            return point == CompositePoint::AfterLdr ? "LDR (P5)" : "HDR (P4)";
        }

        using SuperResolutionFn = void(__fastcall *)(void *stage);
        SuperResolutionFn s_super_resolution_original = nullptr;
        std::atomic<bool> s_p6_installed{false};
        std::atomic<std::uint64_t> s_p6_frames{0};

        using BlurCtorFn = void *(__fastcall *)(void *self, std::uintptr_t pipeline, const char *name);
        using BlurExecuteFn = void(__fastcall *)(
            void *self,
            std::uintptr_t src_dst,
            std::uintptr_t temp,
            float scale,
            float distribution,
            bool alpha_only
        );
        using ScalarDtorFn = void *(__fastcall *)(void *self, unsigned int flags);

        // The mask blur: a CGaussianBlurPass the mod owns, constructed on the render thread for the pipeline it blurs
        // on and destroyed at shutdown once the render path is idle.
        constexpr const char *BLUR_PASS_NAME = "HenrySenses Silhouette Blur";
        void *s_blur_pass = nullptr;
        std::uintptr_t s_blur_pipeline = 0;
        std::atomic<bool> s_blur_faulted{false};

        using RenderInternalFn = std::uintptr_t(__fastcall *)(
            void *stat_obj,
            void *render_object,
            void *hide_mask,
            void *lod_value,
            const void *pass_info
        );
        RenderInternalFn s_render_internal_original = nullptr;
        AfterPostHdrFn s_after_post_hdr_original = nullptr;
        AfterPostHdrFn s_after_post_ldr_original = nullptr;
        DrawRenderItemsFn s_draw_render_items_original = nullptr;

        std::atomic<bool> s_armed{true};
        std::atomic<bool> s_p1_installed{false};
        std::atomic<bool> s_p2_installed{false};
        std::atomic<bool> s_p3_installed{false};
        std::atomic<bool> s_p3b_installed{false};
        std::atomic<bool> s_p4_installed{false};
        std::atomic<bool> s_p7_installed{false};
        // Herb silhouette draws taken out of the depth pre-pass (P7), and a fault that turned the filter off.
        std::atomic<std::uint64_t> s_p7_stripped{0};
        std::atomic<bool> s_p7_faulted{false};
        // Latched when an engine call faulted on the render thread; the render path then stays off.
        std::atomic<bool> s_render_faulted{false};
        std::atomic<bool> s_late_attach_failed{false};

        // Threads currently inside P1, P2 or P4, for the shutdown rundown.
        std::atomic<int> s_in_flight{0};
        // P4 bodies in progress. The unpublish request and this counter are sequentially consistent on both sides, so
        // a body the teardown thread does not wait for is one that sees the request.
        std::atomic<int> s_p4_bodies{0};

        /**
         * @brief One P3 in-flight counter, padded to its own cache line.
         */
        struct InFlightStripe
        {
            std::atomic<int> count{0};
            char padding[CACHE_LINE - sizeof(std::atomic<int>)]{};
        };
        static_assert(sizeof(InFlightStripe) == CACHE_LINE, "One stripe per cache line.");
        std::array<InFlightStripe, P3_STRIPES> s_p3_in_flight{};

        std::atomic<std::uintptr_t> s_last_pipeline{0};
        std::atomic<std::uintptr_t> s_last_stage{0};
        std::atomic<std::int64_t> s_stage_seen_ms{0};
        std::atomic<std::uint32_t> s_p1_calls{0};
        std::atomic<std::uint32_t> s_p1_registered{0};
        std::atomic<std::uint32_t> s_late_attached{0};

        std::atomic<float> s_intensity{1.0f};
        std::atomic<float> s_interior_opacity{0.0f};

        // Bumped after every pipeline Init and late attach, so the composite configures a new stage's pass even when
        // the allocator hands the new stage the old addresses.
        std::atomic<std::uint32_t> s_binding_generation{0};

        // Counters, written on the threads named in the file comment and read by the stats line and the state report.
        std::atomic<std::uint64_t> s_p2_debug_suppressed{0};
        std::atomic<std::uint64_t> s_p2_pass4_uninitialized{0};
        std::atomic<std::uint64_t> s_p2_pass4_ok{0};
        std::atomic<std::uint64_t> s_p2_pass4_failed{0};
        std::atomic<std::uint64_t> s_p2_pass4_overridden{0};
        // Pass-4 requests refused because the object does not carry the marker flag.
        std::atomic<std::uint64_t> s_p2_pass4_gated{0};
        // P5 (after-LDR) hook state and frames it drew in.
        std::atomic<bool> s_p5_installed{false};
        std::atomic<std::uint64_t> s_p5_frames{0};
        // Whether the last composite binding bound its render target (render thread only).
        bool s_bound_target_ok = false;

        /** @brief Read-only state of the composite pass after its last Execute (render thread only). */
        struct CompositeDiag
        {
            std::uint32_t primitives{0};
            std::uintptr_t device_pass{0};
            std::uint8_t device_pass_valid{0};
            std::uint8_t scheduler_enabled{0};
            std::uint32_t scheduler_active{0};

            [[nodiscard]] bool operator==(const CompositeDiag &) const = default;
        };
        CompositeDiag s_diag{};
        // OR of the objectFlags of every unmarked pass-4 request: the marker bit must never appear here.
        std::atomic<std::uint64_t> s_p2_seen_flags{0};
        std::atomic<std::uint64_t> s_p3_injected{0};
        std::atomic<std::uint64_t> s_p3b_marked{0};
        std::atomic<std::uint64_t> s_p4_frames{0};
        std::atomic<std::uint64_t> s_p4_stage_frames{0};
        std::atomic<std::uint64_t> s_p4_item_frames{0};
        std::atomic<std::uint64_t> s_p4_masks{0};
        std::atomic<std::uint64_t> s_p4_composites{0};
        std::atomic<std::uint64_t> s_p4_rebuilds{0};
        // The share of s_p4_rebuilds asked for a silhouette PSO built for the other [Render] SeeThrough.
        std::atomic<std::uint64_t> s_p4_depth_rebuilds{0};
        // The share asked once per SeeThrough generation for a silhouette PSO P2 did not record.
        std::atomic<std::uint64_t> s_p4_unrecorded_rebuilds{0};
        std::atomic<std::uint32_t> s_p4_last_items{0};
        std::atomic<std::uint32_t> s_p4_max_items{0};
        std::atomic<std::uint32_t> s_p4_last_flags{0};
        std::atomic<bool> s_p4_last_pending{false};

        // CCryNameR objects for the composite constants: 8-byte handles into the engine name table, interned once
        // on the render thread and never destroyed.
        std::uintptr_t s_name_ps_params{0};
        std::uintptr_t s_name_vs_params{0};
        bool s_names_ready{false};
        std::uintptr_t s_name_custom_params0{0};
        std::uintptr_t s_name_custom_params1{0};
        std::uintptr_t s_name_custom_params2{0};
        bool s_custom_names_ready{false};
        // The mod's technique drew once (then it is bound like the stock one), and the composites it failed before.
        bool s_custom_confirmed{false};
        std::uint32_t s_custom_failures{0};
        // Composite pass bindings, rebuilt when the pass, the destination, the mask texture or the binding
        // generation changes (render thread only).
        std::uintptr_t s_bound_pass{0};
        std::uint32_t s_bound_destination_id{0xFFFFFFFFu};
        std::uintptr_t s_bound_mask{0};
        std::uint32_t s_bound_generation{0};
        std::uintptr_t s_bound_shader{0};
        std::uintptr_t s_bound_depth{0};
        std::uintptr_t s_bound_visibility{0};
        std::uint32_t s_technique_key{DEFERRED_SILHOUETTES_CRC};
        std::uint32_t s_custom_technique_key{HENRY_SENSES_SILHOUETTE_CRC};
        std::uint32_t s_custom_coverage_key{HENRY_SENSES_COVERAGE_CRC};
        // The mod's coverage technique drew once, and the coverages it failed before (its permutation compiles on its
        // own); past CUSTOM_WARMUP_ATTEMPTS the stock coverage is kept for the session.
        bool s_coverage_confirmed{false};
        bool s_coverage_given_up{false};
        std::uint32_t s_coverage_failures{0};
        std::uintptr_t s_focus_bound_shader{0};
        std::uintptr_t s_focus_bound_depth{0};
        std::uintptr_t s_focus_bound_visibility{0};

        // Focus: the controller asks for the coverage mask the game's VisualArtifacts effect darkens by (red = 1 on the
        // background, 0 on highlighted objects); the render thread draws it into $SceneDiffuseTmp each frame before
        // the post-effect stage runs that effect.
        std::atomic<bool> s_focus_wanted{false};
        std::atomic<bool> s_focus_faulted{false};
        std::atomic<std::uint64_t> s_focus_frames{0};
        // The coverage pass's bindings, as the composite's (render thread only).
        std::uintptr_t s_focus_bound_pass{0};
        std::uint32_t s_focus_bound_target_id{0xFFFFFFFFu};
        std::uintptr_t s_focus_bound_mask{0};
        std::uint32_t s_focus_bound_generation{0};
        // The composite's fill term for the coverage pass: large enough that any covered texel's alpha saturates.
        constexpr float FOCUS_FILL_SCALE = 1000.0f;

        /**
         * @brief True while a frame has something to draw: a showing group (the composite strength the tick publishes
         *        is above 0) or the focus coverage.
         * @details With neither, a drawn mask would be composited at strength 0 (composite_mask skips it) and nothing
         *          else reads it, so the draw hooks return right after the original. The stage lifecycle in P4 still
         *          runs every frame. Before the first tick publishes, the strength reads 1 (the hooks draw as before).
         */
        [[nodiscard]] bool render_work_pending() noexcept
        {
            return s_intensity.load(std::memory_order_relaxed) > 0.0f || s_focus_wanted.load(std::memory_order_relaxed);
        }

        /**
         * @brief Drops the per-frame hand-offs (P6 to P4, P4 to P5), so a view drawn before a frame went idle is never
         *        mistaken for a later frame's (render views are reused). Render thread.
         */
        void clear_view_handoffs() noexcept
        {
            s_tested_mask_view = 0;
            s_tested_mask_texture = 0;
            s_handoff = {};
        }

        /**
         * @enum StageOrigin
         * @brief How the stage on a tracked pipeline came to be published.
         */
        enum class StageOrigin : std::uint8_t
        {
            /// Not decided yet; the first frame with a stage decides between PipelineInit and Found.
            Unknown,
            /// P1 registered it before the pipeline's own Init (the early hooks were in time).
            PipelineInit,
            /// Registered and initialized from the render thread because P1 missed the pipeline's Init.
            LateAttach,
            /// Republished from the stash a previous generation left in the persistent slots.
            Adopted,
            /// Already published when this generation first saw it (a previous generation or another mod).
            Found,
        };

        /**
         * @struct TrackedPipeline
         * @brief One standard pipeline P4 executed, with the stage this generation used on it.
         * @details P4 is the only writer (the render thread). The teardown thread reads the atomics while it waits
         *          for the unpublish.
         */
        struct TrackedPipeline
        {
            std::atomic<std::uintptr_t> pipeline{0};
            std::atomic<std::uintptr_t> stage{0};
            /// Last frame P4 executed the pipeline.
            std::atomic<std::int64_t> executed_ms{0};
            /// Last frame P4 executed the pipeline with a stage published.
            std::atomic<std::int64_t> seen_ms{0};
            std::atomic<StageOrigin> origin{StageOrigin::Unknown};
            std::atomic<bool> unpublished{false};
            /// Render thread only: the stage the origin line was logged for.
            std::uintptr_t reported_stage{0};
            /// Render thread only: the first-frame line was logged.
            bool first_frame_reported{false};
        };

        std::array<TrackedPipeline, TRACKED_PIPELINES> s_tracked{};

        // Pipelines whose Init P1 saw, and the stage P1 registered on each (0 when it registered none).
        std::array<std::atomic<std::uintptr_t>, TRACKED_PIPELINES> s_p1_pipelines{};
        std::array<std::atomic<std::uintptr_t>, TRACKED_PIPELINES> s_p1_stages{};

        // Loader-owned persistent slots, bound once before P4 is installed; empty in the release ASI.
        std::span<std::uint64_t> s_stash_slots{};
        // Set by unpublish_engine_stage(); P4 then unpublishes instead of drawing, and P1 registers nothing.
        std::atomic<bool> s_unpublish_requested{false};
        std::atomic<std::uint32_t> s_adopted{0};
        std::atomic<std::uint32_t> s_unpublished{0};

        /**
         * @struct StashEntry
         * @brief One unpublished stage: the pipeline it belongs to, the stage, and the pipeline's forward stage.
         */
        struct StashEntry
        {
            std::uintptr_t pipeline{0};
            std::uintptr_t stage{0};
            std::uintptr_t forward{0};
        };

        /**
         * @brief Counts a thread inside a long-running detour for the shutdown rundown.
         */
        class InFlightScope
        {
        public:
            InFlightScope() noexcept { s_in_flight.fetch_add(1, std::memory_order_acq_rel); }
            ~InFlightScope() noexcept { s_in_flight.fetch_sub(1, std::memory_order_acq_rel); }
            InFlightScope(const InFlightScope &) = delete;
            InFlightScope &operator=(const InFlightScope &) = delete;
        };

        /**
         * @brief Counts one P4 body for the unpublish handshake.
         */
        class P4BodyScope
        {
        public:
            P4BodyScope() noexcept { s_p4_bodies.fetch_add(1, std::memory_order_seq_cst); }
            ~P4BodyScope() noexcept { s_p4_bodies.fetch_sub(1, std::memory_order_seq_cst); }
            P4BodyScope(const P4BodyScope &) = delete;
            P4BodyScope &operator=(const P4BodyScope &) = delete;
        };

        /**
         * @brief Counts a thread inside P3 on its own stripe.
         */
        class StripedInFlightScope
        {
        public:
            StripedInFlightScope() noexcept : m_count(s_p3_in_flight[(GetCurrentThreadId() >> 2) % P3_STRIPES].count)
            {
                m_count.fetch_add(1, std::memory_order_acq_rel);
            }
            ~StripedInFlightScope() noexcept { m_count.fetch_sub(1, std::memory_order_acq_rel); }
            StripedInFlightScope(const StripedInFlightScope &) = delete;
            StripedInFlightScope &operator=(const StripedInFlightScope &) = delete;

        private:
            std::atomic<int> &m_count;
        };

        /**
         * @brief Reports whether no thread is inside any detour of this file.
         */
        [[nodiscard]] bool detours_idle() noexcept
        {
            if (s_in_flight.load(std::memory_order_acquire) != 0)
            {
                return false;
            }
            for (const InFlightStripe &stripe : s_p3_in_flight)
            {
                if (stripe.count.load(std::memory_order_acquire) != 0)
                {
                    return false;
                }
            }
            return true;
        }

        /**
         * @brief Returns the steady-clock time in milliseconds.
         */
        [[nodiscard]] std::int64_t now_ms() noexcept
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch()
            )
                .count();
        }

        /**
         * @brief Raw typed load for use inside the SEH helpers only.
         */
        template <typename T> [[nodiscard]] T load(std::uintptr_t address) noexcept
        {
            return *reinterpret_cast<const volatile T *>(address);
        }

        /**
         * @brief Raw typed store for use inside the SEH helpers only.
         */
        template <typename T> void store(std::uintptr_t address, T value) noexcept
        {
            *reinterpret_cast<volatile T *>(address) = value;
        }

        [[nodiscard]] std::uintptr_t guarded_register_stage(std::uintptr_t fn, std::uintptr_t pipeline) noexcept
        {
            __try
            {
                return reinterpret_cast<RegisterStageFn>(fn)(pipeline, 0);
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return 0;
            }
        }

        [[nodiscard]] bool guarded_stage_init(std::uintptr_t fn, std::uintptr_t stage) noexcept
        {
            __try
            {
                reinterpret_cast<StageInitFn>(fn)(stage);
                return true;
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return false;
            }
        }

        /**
         * @brief Runs WaitForDrawSubmission after a fault so no recording job outlives the mask draw.
         * @param fn The wait function.
         * @param drawer The CRenderItemDrawer.
         */
        void guarded_wait_draws(std::uintptr_t fn, std::uintptr_t drawer) noexcept
        {
            __try
            {
                reinterpret_cast<DrawerFn>(fn)(drawer);
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
            }
        }

        [[nodiscard]] bool guarded_make_name(std::uintptr_t fn, std::uintptr_t *storage, const char *name) noexcept
        {
            __try
            {
                reinterpret_cast<CryNameRFn>(fn)(storage, name);
                return *storage != 0;
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return false;
            }
        }

        /**
         * @brief Releases an engine intrusive PSO pointer exactly like the engine reset helper.
         * @param slot The PSO out slot.
         */
        void release_pso(void **slot) noexcept
        {
            void *const pso = *slot;
            if (pso == nullptr)
            {
                return;
            }
            *slot = nullptr;
            auto *const ref = reinterpret_cast<volatile long *>(
                reinterpret_cast<std::uintptr_t>(pso) + constants::DEVICE_OBJECT_REFCOUNT_OFFSET
            );
            if (_InterlockedExchangeAdd(ref, -1) == 1)
            {
                using DeletingDtor = void(__fastcall *)(void *, int);
                const DeletingDtor *const vtable = *reinterpret_cast<const DeletingDtor *const *>(pso);
                vtable[0](pso, 1);
            }
        }

        /**
         * @brief Atomically replaces the pipeline's stage slot when it still holds @p expected.
         * @details The slot is an 8-byte aligned member of a heap object, so the interlocked exchange is one atomic
         *          store as seen by every engine reader.
         * @param pipeline The CStandardGraphicsPipeline.
         * @param expected The value the slot must hold.
         * @param desired The value to store.
         * @return True when the slot held @p expected and now holds @p desired.
         */
        [[nodiscard]] bool
        guarded_exchange_stage_slot(std::uintptr_t pipeline, std::uintptr_t expected, std::uintptr_t desired) noexcept
        {
            __try
            {
                auto *const slot =
                    reinterpret_cast<volatile long long *>(pipeline + constants::PIPELINE_CUSTOM_STAGE_OFFSET);
                return static_cast<std::uintptr_t>(_InterlockedCompareExchange64(
                           slot,
                           static_cast<long long>(desired),
                           static_cast<long long>(expected)
                       )) == expected;
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return false;
            }
        }

        /**
         * @brief Returns persistent slot @p index as an atomic word.
         * @note The caller checks stash_available() first.
         */
        [[nodiscard]] std::atomic_ref<std::uint64_t> stash_word(std::size_t index) noexcept
        {
            return std::atomic_ref<std::uint64_t>(s_stash_slots[index]);
        }

        /**
         * @brief Reports whether a persistent area is bound and carries the stash layout.
         */
        [[nodiscard]] bool stash_available() noexcept
        {
            return s_stash_slots.size() >= STASH_SLOT_COUNT &&
                   stash_word(0).load(std::memory_order_acquire) == STASH_LAYOUT_TAG;
        }

        /// Reads stash entry @p entry.
        [[nodiscard]] StashEntry stash_read(std::size_t entry) noexcept
        {
            const std::size_t base = 1 + entry * STASH_ENTRY_SLOTS;
            return StashEntry{
                .pipeline = static_cast<std::uintptr_t>(stash_word(base).load(std::memory_order_acquire)),
                .stage = static_cast<std::uintptr_t>(stash_word(base + 1).load(std::memory_order_acquire)),
                .forward = static_cast<std::uintptr_t>(stash_word(base + 2).load(std::memory_order_acquire)),
            };
        }

        /// Writes stash entry @p entry; a zero pipeline clears it.
        void stash_write(std::size_t entry, const StashEntry &value) noexcept
        {
            // The pipeline is the key a reader matches on, so it is written last and cleared first.
            const std::size_t base = 1 + entry * STASH_ENTRY_SLOTS;
            if (value.pipeline == 0)
            {
                stash_word(base).store(0, std::memory_order_release);
            }
            stash_word(base + 1).store(value.stage, std::memory_order_release);
            stash_word(base + 2).store(value.forward, std::memory_order_release);
            if (value.pipeline != 0)
            {
                stash_word(base).store(value.pipeline, std::memory_order_release);
            }
        }

        /**
         * @brief Returns the stash entry recorded for @p pipeline, or STASH_ENTRIES when none is.
         */
        [[nodiscard]] std::size_t stash_find(std::uintptr_t pipeline) noexcept
        {
            if (!stash_available() || pipeline == 0)
            {
                return STASH_ENTRIES;
            }
            for (std::size_t entry = 0; entry < STASH_ENTRIES; ++entry)
            {
                if (stash_read(entry).pipeline == pipeline)
                {
                    return entry;
                }
            }
            return STASH_ENTRIES;
        }

        /**
         * @brief Records @p value, replacing the entry of the same pipeline or taking a free one.
         * @return False when no persistent area is bound or every entry holds another pipeline.
         */
        [[nodiscard]] bool stash_put(const StashEntry &value) noexcept
        {
            if (!stash_available())
            {
                return false;
            }
            std::size_t target = stash_find(value.pipeline);
            for (std::size_t entry = 0; entry < STASH_ENTRIES && target == STASH_ENTRIES; ++entry)
            {
                if (stash_read(entry).pipeline == 0)
                {
                    target = entry;
                }
            }
            if (target == STASH_ENTRIES)
            {
                return false;
            }
            stash_write(target, value);
            return true;
        }

        /**
         * @brief Clears a tracking entry for reuse.
         * @note Render thread, or the init thread before P4 exists.
         */
        void reset_tracked(TrackedPipeline &entry) noexcept
        {
            // The pipeline is the key the teardown thread matches on, so it is cleared first.
            entry.pipeline.store(0, std::memory_order_release);
            entry.stage.store(0, std::memory_order_relaxed);
            entry.executed_ms.store(0, std::memory_order_relaxed);
            entry.seen_ms.store(0, std::memory_order_relaxed);
            entry.origin.store(StageOrigin::Unknown, std::memory_order_relaxed);
            entry.unpublished.store(false, std::memory_order_relaxed);
            entry.reported_stage = 0;
            entry.first_frame_reported = false;
        }

        /**
         * @brief Returns the tracking entry of @p pipeline, claiming one on first sight, and stamps its frame time.
         * @details A full table reuses the entry of a pipeline P4 has not executed for longer than the live window,
         *          so pipelines the game creates and destroys for extra views cannot starve the default one.
         * @return The entry, or nullptr when every entry tracks a pipeline that still renders.
         * @note Render thread only.
         */
        [[nodiscard]] TrackedPipeline *track_pipeline(std::uintptr_t pipeline) noexcept
        {
            const std::int64_t now = now_ms();
            TrackedPipeline *free_entry = nullptr;
            TrackedPipeline *oldest = nullptr;
            for (TrackedPipeline &entry : s_tracked)
            {
                const std::uintptr_t current = entry.pipeline.load(std::memory_order_relaxed);
                if (current == pipeline)
                {
                    entry.executed_ms.store(now, std::memory_order_relaxed);
                    return &entry;
                }
                if (current == 0)
                {
                    free_entry = free_entry != nullptr ? free_entry : &entry;
                }
                else if (oldest == nullptr || entry.executed_ms.load(std::memory_order_relaxed) <
                                                  oldest->executed_ms.load(std::memory_order_relaxed))
                {
                    oldest = &entry;
                }
            }
            if (free_entry == nullptr && oldest != nullptr &&
                now - oldest->executed_ms.load(std::memory_order_relaxed) > STAGE_LIVE_WINDOW_MS)
            {
                reset_tracked(*oldest);
                free_entry = oldest;
            }
            if (free_entry != nullptr)
            {
                free_entry->executed_ms.store(now, std::memory_order_relaxed);
                free_entry->pipeline.store(pipeline, std::memory_order_release);
            }
            return free_entry;
        }

        /**
         * @brief Records a stage this frame published on a tracked pipeline, so the teardown waits for its unpublish.
         * @param track The pipeline's tracking entry, or nullptr.
         * @param stage The stage now at P+0x50.
         * @param origin How it was published.
         * @note Render thread, inside P4.
         */
        void note_published_stage(TrackedPipeline *track, std::uintptr_t stage, StageOrigin origin) noexcept
        {
            if (track == nullptr)
            {
                return;
            }
            track->origin.store(origin, std::memory_order_release);
            track->stage.store(stage, std::memory_order_relaxed);
            track->seen_ms.store(now_ms(), std::memory_order_release);
        }

        /**
         * @brief Returns the stage a tracked pipeline still holds and P4 has not taken off, or 0.
         * @details Reads the live slot instead of the entry's last recorded stage, so a stage published by any path
         *          (P1, late attach, adoption, another mod) counts, and a pipeline that is gone does not.
         * @param entry The tracking entry.
         * @return The published stage, or 0.
         * @note Teardown thread; every read is guarded because the render thread can destroy the pipeline.
         */
        [[nodiscard]] std::uintptr_t stage_left_published(const TrackedPipeline &entry) noexcept
        {
            const std::uintptr_t pipeline = entry.pipeline.load(std::memory_order_acquire);
            if (pipeline == 0 || entry.unpublished.load(std::memory_order_acquire) ||
                !object_is(GameClass::StdPipeline, pipeline))
            {
                return 0;
            }
            const auto slot =
                DMK::memory::read<std::uintptr_t>(DMK::Address{pipeline + constants::PIPELINE_CUSTOM_STAGE_OFFSET});
            if (!slot || *slot == 0 || !object_is(GameClass::CustomStage, *slot))
            {
                return 0;
            }
            return *slot;
        }

        /**
         * @brief Records a pipeline whose Init P1 saw and the stage P1 registered on it.
         * @param pipeline The pipeline.
         * @param registered The stage, or 0 when P1 registered none.
         */
        void note_p1_pipeline(std::uintptr_t pipeline, std::uintptr_t registered) noexcept
        {
            for (std::size_t i = 0; i < TRACKED_PIPELINES; ++i)
            {
                std::uintptr_t expected = 0;
                const bool claimed =
                    s_p1_pipelines[i].compare_exchange_strong(expected, pipeline, std::memory_order_acq_rel);
                if (claimed || expected == pipeline)
                {
                    if (claimed || registered != 0)
                    {
                        s_p1_stages[i].store(registered, std::memory_order_release);
                    }
                    return;
                }
            }
        }

        /**
         * @brief Reports whether P1 saw @p pipeline's Init, and which stage it registered there.
         * @param pipeline The pipeline.
         * @param registered Receives the stage P1 registered, or 0.
         * @return True when P1 saw the Init.
         */
        [[nodiscard]] bool p1_saw_pipeline(std::uintptr_t pipeline, std::uintptr_t &registered) noexcept
        {
            registered = 0;
            for (std::size_t i = 0; i < TRACKED_PIPELINES; ++i)
            {
                if (s_p1_pipelines[i].load(std::memory_order_acquire) == pipeline)
                {
                    registered = s_p1_stages[i].load(std::memory_order_acquire);
                    return true;
                }
            }
            return false;
        }

        /**
         * @enum MaskStatus
         * @brief Outcome of one mask draw.
         */
        enum class MaskStatus : std::uint8_t
        {
            Drawn,
            NoItems,
            NoTexture,
            Busy,
            Fault,
        };

        /**
         * @brief One highlighted render item's sort key: its object's camera distance and its place in the list.
         */
        struct MaskItemKey
        {
            float distance;
            std::uint32_t index;
        };

        using RenderItemWords = std::array<std::uint64_t, 4>;
        static_assert(sizeof(RenderItemWords) == (std::size_t{1} << constants::RENDER_LIST_ITEM_SHIFT));

        // Scratch of sort_custom_list_far_to_near; render thread only, where every mask draw runs. A longer list draws
        // unsorted (nothing is dropped); the bound sits well above every herb, loot and Places item of a wide radius.
        constexpr std::size_t MAX_SORTED_ITEMS = 32768;
        std::array<MaskItemKey, MAX_SORTED_ITEMS> s_mask_keys{};
        std::array<RenderItemWords, MAX_SORTED_ITEMS> s_mask_items{};

        // heal_silhouette_psos: rebuild requests per compiled object, render thread only. A compiled object seen
        // needing one again within HEAL_EPISODE_DRAWS mask draws is the same episode (the rebuild did not take);
        // after HEAL_MAX_REQUESTS of those it is left alone while it stays in view (a pipeline state that fails to
        // compile would otherwise be rebuilt every frame). A later sighting starts a new episode.
        struct HealSlot
        {
            std::uintptr_t compiled;
            std::uint64_t draw;
            std::uint32_t requests;
        };
        constexpr std::size_t HEAL_SLOTS = 1024;
        constexpr std::size_t HEAL_PROBES = 16;
        constexpr std::uint32_t HEAL_MAX_REQUESTS = 8;
        constexpr std::uint64_t HEAL_EPISODE_DRAWS = 16;
        std::array<HealSlot, HEAL_SLOTS> s_heal_slots{};
        std::uint64_t s_heal_draw{0};

        // The depth test of every silhouette PSO P2 built (render thread and compile jobs write, the mask pass reads):
        // the PSO pointer, with bit 0 set when its request carried FOB_HUD_REQUIRE_DEPTHTEST. P2 sees a PSO only once
        // per material and request, after which the material's own cache hands it out, so the dev build keeps the
        // table for the process in the loader's persistent slots, where a reloaded generation finds it. A PSO past a
        // full table is unknown. 4096 slots (32 KB) hold a long session's silhouette PSOs at a low load factor.
        constexpr std::size_t PSO_DEPTH_SLOTS = 4096;
        constexpr std::size_t PSO_DEPTH_PROBES = 16;
        constexpr std::uintptr_t PSO_DEPTH_TESTED_BIT = 1;
        // The header lets a reloaded generation refuse a table another layout allocated.
        constexpr std::uint64_t PSO_DEPTH_TAG = UINT64_C(0x485350534F445431); // ASCII "HSPSODT1"

        struct PsoDepthTable
        {
            std::uint64_t tag{PSO_DEPTH_TAG};
            std::uint64_t slot_count{PSO_DEPTH_SLOTS};
            std::array<std::atomic<std::uintptr_t>, PSO_DEPTH_SLOTS> slots{};
        };
        PsoDepthTable s_pso_depth_own{};
        // Set once by bind_pso_depth_table, before the hooks go in.
        PsoDepthTable *s_pso_depth = &s_pso_depth_own;
        // An earlier generation ran in this process (dev reload), so compiled objects may hold PSOs of another
        // SeeThrough; set once by bind_pso_depth_table.
        bool s_pso_depth_inherited = false;

        /**
         * @enum PsoDepth
         * @brief The depth test a silhouette PSO was built with, as P2 recorded it.
         */
        enum class PsoDepth : std::uint8_t
        {
            Unknown,
            Tested,
            NotTested,
        };

        [[nodiscard]] std::size_t pso_depth_start(std::uintptr_t pso) noexcept
        {
            return static_cast<std::size_t>((pso >> 4) * 0x9E3779B97F4A7C15ull) % PSO_DEPTH_SLOTS;
        }

        /** @brief Records the depth test @p pso was built with; a freed PSO's address reused by a new one is updated.
         */
        void record_pso_depth(std::uintptr_t pso, bool depth_tested) noexcept
        {
            const std::uintptr_t entry = pso | (depth_tested ? PSO_DEPTH_TESTED_BIT : 0);
            const std::size_t start = pso_depth_start(pso);
            for (std::size_t probe = 0; probe < PSO_DEPTH_PROBES; ++probe)
            {
                std::atomic<std::uintptr_t> &slot = s_pso_depth->slots[(start + probe) % PSO_DEPTH_SLOTS];
                std::uintptr_t current = slot.load(std::memory_order_relaxed);
                if (current == 0 && slot.compare_exchange_strong(current, entry, std::memory_order_relaxed))
                {
                    return;
                }
                if ((current & ~PSO_DEPTH_TESTED_BIT) == pso)
                {
                    slot.store(entry, std::memory_order_relaxed);
                    return;
                }
            }
        }

        [[nodiscard]] PsoDepth pso_depth(std::uintptr_t pso) noexcept
        {
            const std::size_t start = pso_depth_start(pso);
            for (std::size_t probe = 0; probe < PSO_DEPTH_PROBES; ++probe)
            {
                const std::uintptr_t entry =
                    s_pso_depth->slots[(start + probe) % PSO_DEPTH_SLOTS].load(std::memory_order_relaxed);
                if (entry == 0)
                {
                    return PsoDepth::Unknown;
                }
                if ((entry & ~PSO_DEPTH_TESTED_BIT) == pso)
                {
                    return (entry & PSO_DEPTH_TESTED_BIT) != 0 ? PsoDepth::Tested : PsoDepth::NotTested;
                }
            }
            return PsoDepth::Unknown;
        }

        /**
         * @brief Uses the PSO depth table kept in the loader's persistent slots, allocating it for the process on the
         *        first generation; without the slots (release) the module's own table stays in use.
         * @param slots The loader's persistent slots, or an empty span.
         */
        void bind_pso_depth_table(std::span<std::uint64_t> slots) noexcept
        {
            if (slots.size() <= STASH_PSO_DEPTH_SLOT)
            {
                return;
            }
            // The stash tag is written by the first generation of the process.
            s_pso_depth_inherited =
                std::atomic_ref<std::uint64_t>(slots[0]).load(std::memory_order_acquire) == STASH_LAYOUT_TAG;
            const std::atomic_ref<std::uint64_t> slot(slots[STASH_PSO_DEPTH_SLOT]);
            std::uint64_t kept = slot.load(std::memory_order_acquire);
            // Every layout starts with at least 16 readable bytes (the first had no header: 1024 bare slots).
            const auto *const table = reinterpret_cast<const PsoDepthTable *>(kept);
            if (kept == 0 || table->tag != PSO_DEPTH_TAG || table->slot_count != PSO_DEPTH_SLOTS)
            {
                // The process heap outlives this image; never freed, a later generation keeps using it (a table of
                // another layout is abandoned). DetourModKit has no allocation that survives an unload.
                void *const memory = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PsoDepthTable));
                if (memory == nullptr)
                {
                    return;
                }
                kept = reinterpret_cast<std::uint64_t>(new (memory) PsoDepthTable{});
                slot.store(kept, std::memory_order_release);
            }
            s_pso_depth = reinterpret_cast<PsoDepthTable *>(kept);
        }

        // heal_silhouette_psos: the compiled objects already asked to rebuild in the current SeeThrough generation,
        // render thread only. P2 records only the PSOs it builds, and a material's cache keeps handing out those an
        // earlier generation of the mod built, so an object whose PSO is unrecorded is asked once after every
        // SeeThrough change and once after this module loads; its render object's current flags then decide the PSO.
        // The first SEEN_SETTLE_DRAWS mask draws of a generation ask nothing: frames filled with the old flags are
        // still in flight.
        struct SeenSlot
        {
            std::uintptr_t compiled;
            std::uint32_t generation;
        };
        constexpr std::size_t SEEN_SLOTS = 4096;
        constexpr std::size_t SEEN_PROBES = 16;
        constexpr std::uint64_t SEEN_SETTLE_DRAWS = 4;
        std::array<SeenSlot, SEEN_SLOTS> s_seen_slots{};
        std::uint32_t s_seen_generation{0};
        std::uint64_t s_seen_generation_draw{0};
        // FOB_HUD_REQUIRE_DEPTHTEST as the current generation saw it; 1 (never a flag value) before the first draw.
        std::uint64_t s_seen_depth_bit{1};

        [[nodiscard]] bool is_herb_proxy(std::uintptr_t render_object) noexcept;

        /** @brief True when @p compiled was already recorded in the current SeeThrough generation (a lookup only). */
        [[nodiscard]] bool seen_this_generation(std::uintptr_t compiled) noexcept
        {
            const std::size_t start = static_cast<std::size_t>((compiled >> 4) * 0x9E3779B97F4A7C15ull) % SEEN_SLOTS;
            for (std::size_t probe = 0; probe < SEEN_PROBES; ++probe)
            {
                const SeenSlot &slot = s_seen_slots[(start + probe) % SEEN_SLOTS];
                if (slot.compiled == compiled)
                {
                    return slot.generation == s_seen_generation;
                }
                if (slot.compiled == 0)
                {
                    return false;
                }
            }
            return false;
        }

        /**
         * @brief Records @p compiled for the current SeeThrough generation.
         * @return True the first time it is seen in this generation.
         */
        [[nodiscard]] bool first_seen_this_generation(std::uintptr_t compiled) noexcept
        {
            const std::size_t start = static_cast<std::size_t>((compiled >> 4) * 0x9E3779B97F4A7C15ull) % SEEN_SLOTS;
            SeenSlot *free = nullptr;
            for (std::size_t probe = 0; probe < SEEN_PROBES; ++probe)
            {
                SeenSlot &slot = s_seen_slots[(start + probe) % SEEN_SLOTS];
                if (slot.compiled == compiled)
                {
                    if (slot.generation == s_seen_generation)
                    {
                        return false;
                    }
                    slot.generation = s_seen_generation;
                    return true;
                }
                if (free == nullptr && (slot.compiled == 0 || slot.generation != s_seen_generation))
                {
                    free = &slot;
                }
            }
            // A full neighbourhood gives up its first slot; the object evicted is at worst asked once more.
            *(free != nullptr ? free : &s_seen_slots[start]) = SeenSlot{compiled, s_seen_generation};
            return true;
        }

        /**
         * @brief Takes one rebuild request for @p compiled; false once it had its share.
         */
        [[nodiscard]] bool take_heal_request(std::uintptr_t compiled) noexcept
        {
            const std::size_t start = static_cast<std::size_t>((compiled >> 4) * 0x9E3779B97F4A7C15ull) % HEAL_SLOTS;
            HealSlot *free = nullptr;
            for (std::size_t probe = 0; probe < HEAL_PROBES; ++probe)
            {
                HealSlot &slot = s_heal_slots[(start + probe) % HEAL_SLOTS];
                const bool stale = slot.compiled == 0 || s_heal_draw - slot.draw > HEAL_EPISODE_DRAWS;
                if (slot.compiled == compiled)
                {
                    if (stale)
                    {
                        slot.requests = 0;
                    }
                    slot.draw = s_heal_draw;
                    if (slot.requests >= HEAL_MAX_REQUESTS)
                    {
                        return false;
                    }
                    ++slot.requests;
                    return true;
                }
                if (stale && free == nullptr)
                {
                    free = &slot;
                }
            }
            if (free == nullptr)
            {
                return false;
            }
            *free = HealSlot{compiled, s_heal_draw, 1};
            return true;
        }

        /**
         * @brief Marks every compiled object of EFSLIST_CUSTOM whose silhouette PSO is missing, or was built for the
         *        other [Render] SeeThrough, for a pipeline-state rebuild, so it reaches the mask again as configured.
         * @details The per-material PSO builder creates the custom stage's pipeline states only while the pipeline
         *          holds the stage, and completes the object without them otherwise (the stage is unpublished for the
         *          seconds a hot reload takes); a request whose flags lack the marker gets none either (P2). The engine
         *          keeps two compiled objects per character part, one per frame, and rebuilds their pipeline states
         *          only when marked dirty. Both copies need repair or a missing silhouette alternates each frame.
         *          The same holds across a SeeThrough change: the render object takes the new flags while compiled
         *          objects retain their old depth-test state. P2 records that state for comparison; unknown PSOs
         *          rebuild once per SeeThrough generation (first_seen_this_generation). Depth checks start only
         *          after a setting change or a dev reload can leave an older PSO alive, avoiding unnecessary work
         *          while all compiled objects share the initial setting. The dirty bit makes the next
         *          CCompiledRenderObject::Compile of that object rebuild them from its render object's flags; a
         *          character's parts compile every frame, so they draw again a frame later. Raw reads and no object
         *          with a destructor: it runs inside guarded_draw_mask's SEH scope.
         * @param render_view The executing CRenderView.
         */
        void heal_silhouette_psos(std::uintptr_t render_view) noexcept
        {
            constexpr int max_segments = 64;
            constexpr std::size_t item_size = std::size_t{1} << constants::RENDER_LIST_ITEM_SHIFT;
            ++s_heal_draw;
            const bool shadow_view = load<std::uint32_t>(render_view + constants::RENDER_VIEW_TYPE_OFFSET) ==
                                     constants::RENDER_VIEW_TYPE_SHADOW;
            const std::ptrdiff_t dirty_offset = constants::COMPILED_OBJECT_DIRTY_OFFSET + (shadow_view ? 1 : 0);
            const std::uint64_t depth_bit = silhouette_object_flags() & constants::FOB_HUD_REQUIRE_DEPTHTEST;
            if (depth_bit != s_seen_depth_bit)
            {
                s_seen_depth_bit = depth_bit;
                ++s_seen_generation;
                s_seen_generation_draw = s_heal_draw;
            }
            // A PSO of the other SeeThrough exists only after a change in this generation or one inherited from an
            // earlier generation (dev reload); until then only the missing PSOs are looked for, at the old cost.
            const bool depth_checks = s_pso_depth_inherited || s_seen_generation > 1;
            const bool settled = s_heal_draw - s_seen_generation_draw >= SEEN_SETTLE_DRAWS;
            const PsoDepth stale_depth = depth_bit != 0 ? PsoDepth::NotTested : PsoDepth::Tested;
            std::uintptr_t last_pso = 0;
            PsoDepth last_depth = PsoDepth::Unknown;
            std::uintptr_t segment = render_view + constants::RENDER_VIEW_LISTS_OFFSET +
                                     constants::RENDER_VIEW_LIST_STRIDE * constants::RENDER_LIST_CUSTOM;
            for (int depth = 0; segment != 0 && depth < max_segments; ++depth)
            {
                const auto begin = load<std::uintptr_t>(segment + constants::RENDER_LIST_BEGIN_OFFSET);
                const auto end = load<std::uintptr_t>(segment + constants::RENDER_LIST_END_OFFSET);
                if (begin != 0 && end > begin && (end - begin) % item_size == 0)
                {
                    for (std::uintptr_t item = begin; item < end; item += item_size)
                    {
                        const auto compiled =
                            load<std::uintptr_t>(item + constants::RENDER_ITEM_COMPILED_OBJECT_OFFSET);
                        if (compiled == 0)
                        {
                            continue;
                        }
                        const auto pso =
                            load<std::uintptr_t>(compiled + constants::COMPILED_OBJECT_SILHOUETTE_PSO_OFFSET);
                        bool wrong_depth = false;
                        bool unrecorded = false;
                        if (pso != 0 && depth_checks)
                        {
                            // Neighbouring items mostly share a PSO (one per shader permutation), so the last answer
                            // is kept and the table is read only when the PSO changes.
                            if (pso != last_pso)
                            {
                                last_pso = pso;
                                last_depth = pso_depth(pso);
                            }
                            wrong_depth = last_depth == stale_depth;
                            // An object already asked in this generation costs one lookup. A herb draw is a temporary
                            // render object compiled every frame with the current flags; its compiled objects come and
                            // go, so each would look new every time.
                            unrecorded =
                                last_depth == PsoDepth::Unknown && settled && !seen_this_generation(compiled) &&
                                !is_herb_proxy(
                                    load<std::uintptr_t>(compiled + constants::COMPILED_OBJECT_RENDER_OBJECT_OFFSET)
                                ) &&
                                first_seen_this_generation(compiled);
                        }
                        if ((pso != 0 && !wrong_depth && !unrecorded) || !take_heal_request(compiled))
                        {
                            continue;
                        }
                        _InterlockedOr8(
                            reinterpret_cast<volatile char *>(compiled + dirty_offset),
                            static_cast<char>(constants::COMPILED_OBJECT_DIRTY_PSO)
                        );
                        s_p4_rebuilds.fetch_add(1, std::memory_order_relaxed);
                        if (wrong_depth)
                        {
                            s_p4_depth_rebuilds.fetch_add(1, std::memory_order_relaxed);
                        }
                        else if (unrecorded)
                        {
                            s_p4_unrecorded_rebuilds.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
                segment = load<std::uintptr_t>(segment + constants::RENDER_LIST_PENDING_OFFSET);
            }
        }

        /**
         * @brief Orders EFSLIST_CUSTOM far to near by each item's object distance, so where two highlighted objects
         *        overlap the nearer one draws last and keeps the mask.
         * @details The engine sorts this list by the same distance, but every highlighted node carries
         *          ERF_RENDER_ALWAYS, which collapses the key (constants::RENDER_ITEM_COMPILED_OBJECT_OFFSET), and the
         *          see-through mask is written without a depth test: a farther chest painted over the NPC in front of
         *          it. Ties keep the engine's order, so an object's parts stay together. Raw reads and no object with a
         *          destructor: it runs inside guarded_draw_mask's SEH scope.
         * @param render_view The executing CRenderView.
         */
        void sort_custom_list_far_to_near(std::uintptr_t render_view) noexcept
        {
            const std::uintptr_t list = render_view + constants::RENDER_VIEW_LISTS_OFFSET +
                                        constants::RENDER_VIEW_LIST_STRIDE * constants::RENDER_LIST_CUSTOM;
            const auto begin = load<std::uintptr_t>(list + constants::RENDER_LIST_BEGIN_OFFSET);
            const auto end = load<std::uintptr_t>(list + constants::RENDER_LIST_END_OFFSET);
            if (begin == 0 || end <= begin)
            {
                return;
            }
            const std::size_t count = (end - begin) >> constants::RENDER_LIST_ITEM_SHIFT;
            if (count < 2 || count > MAX_SORTED_ITEMS)
            {
                return;
            }
            bool ordered = true;
            float previous = std::numeric_limits<float>::max();
            for (std::size_t i = 0; i < count; ++i)
            {
                const std::uintptr_t item = begin + i * sizeof(RenderItemWords);
                std::memcpy(&s_mask_items[i], reinterpret_cast<const void *>(item), sizeof(RenderItemWords));
                const auto compiled = load<std::uintptr_t>(item + constants::RENDER_ITEM_COMPILED_OBJECT_OFFSET);
                const auto object =
                    compiled != 0 ? load<std::uintptr_t>(compiled + constants::COMPILED_OBJECT_RENDER_OBJECT_OFFSET)
                                  : 0;
                float distance = object != 0 ? load<float>(object + constants::RENDER_OBJECT_DISTANCE_OFFSET) : 0.0f;
                if (!std::isfinite(distance))
                {
                    distance = 0.0f;
                }
                s_mask_keys[i] = MaskItemKey{distance, static_cast<std::uint32_t>(i)};
                ordered = ordered && distance <= previous;
                previous = distance;
            }
            if (ordered)
            {
                return;
            }
            std::sort(
                s_mask_keys.begin(),
                s_mask_keys.begin() + static_cast<std::ptrdiff_t>(count),
                [](const MaskItemKey &a, const MaskItemKey &b)
                { return a.distance != b.distance ? a.distance > b.distance : a.index < b.index; }
            );
            for (std::size_t i = 0; i < count; ++i)
            {
                std::memcpy(
                    reinterpret_cast<void *>(begin + i * sizeof(RenderItemWords)),
                    &s_mask_items[s_mask_keys[i].index],
                    sizeof(RenderItemWords)
                );
            }
        }

        /**
         * @brief Moves the mask pass's viewports by the camera jitter of @p render_view, so the mask is rasterized
         *        where an unjittered camera would put it; saves the originals into @p saved.
         * @details The view's projection adds its clip-space jitter to m20 / m21 of a right-handed projection
         *          (w = -z), moving the image by minus that jitter in NDC: x by -jx * width / 2 pixels, y by
         *          +jy * height / 2. The viewport moves the other way. A composite past the upscaler then gets the same
         *          mask every frame from a still camera, where a jittered mask flips its edge pixels every frame. Raw
         *          loads and stores: called inside guarded_draw_mask's SEH scope.
         * @return True when the viewports were moved (restore_viewports() must follow the draw).
         */
        bool move_viewports_by_jitter(std::uintptr_t mask_pass, std::uintptr_t render_view, float *saved) noexcept
        {
            const float jx = load<float>(render_view + constants::RENDER_VIEW_SUBPIXEL_OFFSET);
            const float jy = load<float>(render_view + constants::RENDER_VIEW_SUBPIXEL_OFFSET + 4);
            if (!std::isfinite(jx) || !std::isfinite(jy) || (jx == 0.0f && jy == 0.0f))
            {
                return false;
            }
            const std::uintptr_t viewports = mask_pass + constants::SCENE_PASS_VIEWPORTS_OFFSET;
            std::memcpy(
                saved,
                reinterpret_cast<const void *>(viewports),
                sizeof(float) * constants::SCENE_PASS_VIEWPORT_COUNT * constants::VIEWPORT_FLOATS
            );
            for (std::size_t i = 0; i < constants::SCENE_PASS_VIEWPORT_COUNT; ++i)
            {
                const float *viewport = saved + i * constants::VIEWPORT_FLOATS;
                const float width = viewport[2];
                const float height = viewport[3];
                // A jitter stays within a pixel: 2 / size in clip space.
                if (!(width > 0.0f && height > 0.0f) || std::fabs(jx) * width > 4.0f || std::fabs(jy) * height > 4.0f)
                {
                    return false;
                }
            }
            for (std::size_t i = 0; i < constants::SCENE_PASS_VIEWPORT_COUNT; ++i)
            {
                const float *viewport = saved + i * constants::VIEWPORT_FLOATS;
                const std::uintptr_t target = viewports + i * constants::VIEWPORT_FLOATS * sizeof(float);
                store<float>(target, viewport[0] + jx * viewport[2] * 0.5f);
                store<float>(target + sizeof(float), viewport[1] - jy * viewport[3] * 0.5f);
            }
            return true;
        }

        /** @brief Puts back the viewports move_viewports_by_jitter() saved. Raw stores: inside the SEH scope. */
        void restore_viewports(std::uintptr_t mask_pass, const float *saved) noexcept
        {
            std::memcpy(
                reinterpret_cast<void *>(mask_pass + constants::SCENE_PASS_VIEWPORTS_OFFSET),
                saved,
                sizeof(float) * constants::SCENE_PASS_VIEWPORT_COUNT * constants::VIEWPORT_FLOATS
            );
        }

        /**
         * @brief Draws EFSLIST_CUSTOM into $SceneNormalsMap, the sequence of CSceneCustomStage::ExecuteSilhouettePass.
         * @param pipeline The executing CStandardGraphicsPipeline.
         * @param stage Its CSceneCustomStage.
         * @param render_view The executing CRenderView.
         * @param out_mask Receives the mask texture when drawn.
         * @return The outcome; Fault leaves the recursion counter and pipeline flags restored.
         */
        [[nodiscard]] MaskStatus guarded_draw_mask(
            std::uintptr_t pipeline,
            std::uintptr_t stage,
            std::uintptr_t render_view,
            bool unjitter,
            std::uintptr_t *out_mask
        ) noexcept
        {
            volatile bool viewports_moved = false;
            float saved_viewports[constants::SCENE_PASS_VIEWPORT_COUNT * constants::VIEWPORT_FLOATS]{};
            volatile bool counted = false;
            volatile bool flags_saved = false;
            volatile std::uint64_t saved_flags = 0;
            // Set before JobifyDrawSubmission: once it may have spawned recording jobs, every exit must wait for
            // them, or the next pass re-initializes the drawer under them.
            volatile bool jobified = false;
            volatile std::uintptr_t jobified_drawer = 0;
            const std::uintptr_t mask_pass = stage + constants::CUSTOM_STAGE_MASK_PASS_OFFSET;
            auto *const counter = reinterpret_cast<volatile std::int32_t *>(s_calls.recursion_counter);
            __try
            {
                if (*counter != 0)
                {
                    return MaskStatus::Busy;
                }
                const std::uint32_t list_flags = load<std::uint32_t>(
                    render_view + constants::RENDER_VIEW_BATCH_FLAGS_OFFSET + 4 * constants::RENDER_LIST_CUSTOM
                );
                if ((list_flags & constants::FB_CUSTOM_RENDER) == 0)
                {
                    return MaskStatus::NoItems;
                }
                const std::uintptr_t resources = load<std::uintptr_t>(stage + constants::STAGE_RESOURCES_OFFSET);
                const std::uintptr_t mask =
                    resources != 0 ? load<std::uintptr_t>(resources + constants::RESOURCES_SCENE_NORMALS_OFFSET) : 0;
                if (mask == 0 || load<std::uintptr_t>(mask + constants::TEXTURE_DEVICE_OFFSET) == 0)
                {
                    return MaskStatus::NoTexture;
                }

                saved_flags = load<std::uint64_t>(pipeline + constants::PIPELINE_FLAGS_OFFSET);
                flags_saved = true;

                const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                reinterpret_cast<ClearSurfaceFn>(s_calls.clear_surface)(
                    stage + constants::CUSTOM_STAGE_MASK_CLEAR_PASS_OFFSET,
                    mask,
                    zero
                );

                reinterpret_cast<PrepareRenderPassFn>(s_calls.prepare_render_pass)(
                    mask_pass,
                    load<std::uintptr_t>(s_calls.core_command_list_slot)
                );

                // InitDrawSubmission: empty the drawer's pass list.
                const std::uintptr_t drawer = render_view + constants::RENDER_VIEW_DRAWER_OFFSET;
                store<std::uint64_t>(
                    drawer + constants::DRAWER_PASSES_END_OFFSET,
                    load<std::uint64_t>(drawer + constants::DRAWER_PASSES_BEGIN_OFFSET)
                );

                *counter = *counter + 1;
                counted = true;
                store<std::uint32_t>(mask_pass + constants::SCENE_PASS_GROUP_COUNT_OFFSET, 0);
                store<std::uint16_t>(
                    mask_pass + constants::SCENE_PASS_TECHNIQUE_OFFSET,
                    constants::TECHNIQUE_CUSTOM_RENDER_PASS
                );
                store<std::uint32_t>(
                    mask_pass + constants::SCENE_PASS_STAGE_PASS_OFFSET,
                    constants::CUSTOM_STAGE_PASS_SILHOUETTE
                );
                store<std::uint64_t>(mask_pass + constants::SCENE_PASS_FILTERS_OFFSET, constants::FB_CUSTOM_RENDER);
                if (unjitter)
                {
                    viewports_moved = move_viewports_by_jitter(mask_pass, render_view, saved_viewports);
                }
                heal_silhouette_psos(render_view);
                sort_custom_list_far_to_near(render_view);
                reinterpret_cast<DrawRenderItemsFn>(s_calls.draw_render_items)(
                    mask_pass,
                    render_view,
                    constants::RENDER_LIST_CUSTOM,
                    0,
                    0x7FFFFFFF
                );
                *counter = *counter - 1;
                counted = false;

                jobified_drawer = drawer;
                jobified = true;
                reinterpret_cast<DrawerFn>(s_calls.jobify)(drawer);
                reinterpret_cast<DrawerFn>(s_calls.wait)(drawer);
                jobified = false;
                if (viewports_moved)
                {
                    restore_viewports(mask_pass, saved_viewports);
                    viewports_moved = false;
                }

                store<std::uint64_t>(pipeline + constants::PIPELINE_FLAGS_OFFSET, saved_flags);
                *out_mask = mask;
                return MaskStatus::Drawn;
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                if (jobified)
                {
                    guarded_wait_draws(s_calls.wait, jobified_drawer);
                }
                if (viewports_moved)
                {
                    restore_viewports(mask_pass, saved_viewports);
                }
                if (counted)
                {
                    *counter = *counter - 1;
                }
                if (flags_saved)
                {
                    store<std::uint64_t>(pipeline + constants::PIPELINE_FLAGS_OFFSET, saved_flags);
                }
                return MaskStatus::Fault;
            }
        }

        /**
         * @brief Places the mask pass's viewports and scissor for a target @p scale_x by @p scale_y times the size the
         *        stage set them for, moved by the camera jitter as move_viewports_by_jitter() moves them.
         * @param saved The viewports the stage set (SCENE_PASS_VIEWPORT_COUNT * VIEWPORT_FLOATS floats).
         * @details The scissor, which the stage derives from the first viewport, covers the scaled viewport. Raw loads
         *          and stores: called inside guarded_draw_supersampled_mask's SEH scope.
         */
        void place_supersampled_viewports(
            std::uintptr_t mask_pass,
            std::uintptr_t render_view,
            const float *saved,
            float scale_x,
            float scale_y
        ) noexcept
        {
            const float jx = load<float>(render_view + constants::RENDER_VIEW_SUBPIXEL_OFFSET);
            const float jy = load<float>(render_view + constants::RENDER_VIEW_SUBPIXEL_OFFSET + 4);
            const float base_width = saved[2];
            const float base_height = saved[3];
            // A jitter stays within a pixel: 2 / size in clip space.
            const bool jittered = std::isfinite(jx) && std::isfinite(jy) && std::fabs(jx) * base_width <= 4.0f &&
                                  std::fabs(jy) * base_height <= 4.0f;
            const std::uintptr_t viewports = mask_pass + constants::SCENE_PASS_VIEWPORTS_OFFSET;
            for (std::size_t i = 0; i < constants::SCENE_PASS_VIEWPORT_COUNT; ++i)
            {
                const float *viewport = saved + i * constants::VIEWPORT_FLOATS;
                const std::uintptr_t target = viewports + i * constants::VIEWPORT_FLOATS * sizeof(float);
                const float width = viewport[2] * scale_x;
                const float height = viewport[3] * scale_y;
                float x = viewport[0] * scale_x;
                float y = viewport[1] * scale_y;
                if (jittered)
                {
                    x += jx * width * 0.5f;
                    y -= jy * height * 0.5f;
                }
                store<float>(target, x);
                store<float>(target + sizeof(float), y);
                store<float>(target + 2 * sizeof(float), width);
                store<float>(target + 3 * sizeof(float), height);
            }
            const std::uintptr_t scissor = mask_pass + constants::SCENE_PASS_SCISSOR_OFFSET;
            store<std::int32_t>(scissor, static_cast<std::int32_t>(saved[0] * scale_x));
            store<std::int32_t>(scissor + 4, static_cast<std::int32_t>(saved[1] * scale_y));
            store<std::int32_t>(scissor + 8, static_cast<std::int32_t>((saved[0] + saved[2]) * scale_x));
            store<std::int32_t>(scissor + 12, static_cast<std::int32_t>((saved[1] + saved[3]) * scale_y));
        }

        /** @brief CSceneRenderPass::SetRenderTargets under its own SEH scope; false on a fault. */
        [[nodiscard]] bool
        guarded_set_render_targets(std::uintptr_t mask_pass, std::uintptr_t depth, std::uintptr_t color) noexcept
        {
            __try
            {
                reinterpret_cast<SceneSetRenderTargetsFn>(s_calls.scene_set_render_targets)(
                    mask_pass,
                    depth,
                    color,
                    0,
                    0,
                    0
                );
                return true;
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return false;
            }
        }

        /**
         * @brief Draws EFSLIST_CUSTOM into the supersampled targets (s_ss), as guarded_draw_mask() draws it into
         *        $SceneNormalsMap, without the camera jitter.
         * @details The mask pass is pointed at the mod's colour and depth targets, which share the formats of
         *          $SceneNormalsMap and the view's depth target, so every silhouette PSO draws into them unchanged; its
         *          viewports and scissor are scaled to their size. The depth target is cleared to the far plane, which
         *          a depth-tested silhouette PSO (SeeThrough = false) always passes: here every object is drawn whole,
         *          and the depth-tested mask says what is visible. Targets, viewports and scissor are put back before
         *          returning, on a fault too.
         * @return The outcome; Fault leaves the pass, the recursion counter and the pipeline flags restored.
         */
        [[nodiscard]] MaskStatus guarded_draw_supersampled_mask(
            std::uintptr_t pipeline,
            std::uintptr_t stage,
            std::uintptr_t render_view
        ) noexcept
        {
            volatile bool placed = false;
            float saved_viewports[constants::SCENE_PASS_VIEWPORT_COUNT * constants::VIEWPORT_FLOATS]{};
            std::int32_t saved_scissor[4]{};
            volatile bool retargeted = false;
            volatile std::uintptr_t saved_color = 0;
            volatile std::uintptr_t saved_depth = 0;
            volatile bool counted = false;
            volatile bool flags_saved = false;
            volatile std::uint64_t saved_flags = 0;
            volatile bool jobified = false;
            volatile std::uintptr_t jobified_drawer = 0;
            const std::uintptr_t mask_pass = stage + constants::CUSTOM_STAGE_MASK_PASS_OFFSET;
            auto *const counter = reinterpret_cast<volatile std::int32_t *>(s_calls.recursion_counter);
            __try
            {
                if (*counter != 0)
                {
                    return MaskStatus::Busy;
                }
                const std::uint32_t list_flags = load<std::uint32_t>(
                    render_view + constants::RENDER_VIEW_BATCH_FLAGS_OFFSET + 4 * constants::RENDER_LIST_CUSTOM
                );
                if ((list_flags & constants::FB_CUSTOM_RENDER) == 0)
                {
                    return MaskStatus::NoItems;
                }
                const std::uintptr_t mask = s_ss.mask;
                const std::uintptr_t depth = s_ss.depth;
                const std::uintptr_t color =
                    load<std::uintptr_t>(mask_pass + constants::SCENE_PASS_COLOR_TARGET_OFFSET);
                const std::uintptr_t view_depth =
                    load<std::uintptr_t>(mask_pass + constants::SCENE_PASS_DEPTH_TARGET_OFFSET);
                if (mask == 0 || depth == 0 || color == 0 || view_depth == 0 ||
                    load<std::uintptr_t>(mask + constants::TEXTURE_DEVICE_OFFSET) == 0 ||
                    load<std::uintptr_t>(depth + constants::TEXTURE_DEVICE_OFFSET) == 0)
                {
                    return MaskStatus::NoTexture;
                }
                const float color_width = load<std::uint16_t>(color + constants::TEXTURE_WIDTH_OFFSET);
                const float color_height = load<std::uint16_t>(color + constants::TEXTURE_HEIGHT_OFFSET);
                const float mask_width = load<std::uint16_t>(mask + constants::TEXTURE_WIDTH_OFFSET);
                const float mask_height = load<std::uint16_t>(mask + constants::TEXTURE_HEIGHT_OFFSET);
                if (!(color_width > 0.0f && color_height > 0.0f && mask_width > 0.0f && mask_height > 0.0f))
                {
                    return MaskStatus::NoTexture;
                }

                saved_flags = load<std::uint64_t>(pipeline + constants::PIPELINE_FLAGS_OFFSET);
                flags_saved = true;

                const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                const std::uintptr_t clear_pass = stage + constants::CUSTOM_STAGE_MASK_CLEAR_PASS_OFFSET;
                reinterpret_cast<ClearSurfaceFn>(s_calls.clear_surface)(clear_pass, mask, zero);
                reinterpret_cast<ClearDepthFn>(s_calls.clear_depth)(
                    clear_pass,
                    depth,
                    CLEAR_DEPTH_AND_STENCIL,
                    constants::SUPERSAMPLED_MASK_FAR_DEPTH,
                    0
                );

                saved_color = color;
                saved_depth = view_depth;
                reinterpret_cast<SceneSetRenderTargetsFn>(s_calls.scene_set_render_targets)(
                    mask_pass,
                    depth,
                    mask,
                    0,
                    0,
                    0
                );
                retargeted = true;
                std::memcpy(
                    saved_viewports,
                    reinterpret_cast<const void *>(mask_pass + constants::SCENE_PASS_VIEWPORTS_OFFSET),
                    sizeof(saved_viewports)
                );
                std::memcpy(
                    saved_scissor,
                    reinterpret_cast<const void *>(mask_pass + constants::SCENE_PASS_SCISSOR_OFFSET),
                    sizeof(saved_scissor)
                );
                placed = true;
                place_supersampled_viewports(
                    mask_pass,
                    render_view,
                    saved_viewports,
                    mask_width / color_width,
                    mask_height / color_height
                );

                reinterpret_cast<PrepareRenderPassFn>(s_calls.prepare_render_pass)(
                    mask_pass,
                    load<std::uintptr_t>(s_calls.core_command_list_slot)
                );

                // InitDrawSubmission: empty the drawer's pass list.
                const std::uintptr_t drawer = render_view + constants::RENDER_VIEW_DRAWER_OFFSET;
                store<std::uint64_t>(
                    drawer + constants::DRAWER_PASSES_END_OFFSET,
                    load<std::uint64_t>(drawer + constants::DRAWER_PASSES_BEGIN_OFFSET)
                );

                *counter = *counter + 1;
                counted = true;
                store<std::uint32_t>(mask_pass + constants::SCENE_PASS_GROUP_COUNT_OFFSET, 0);
                store<std::uint16_t>(
                    mask_pass + constants::SCENE_PASS_TECHNIQUE_OFFSET,
                    constants::TECHNIQUE_CUSTOM_RENDER_PASS
                );
                store<std::uint32_t>(
                    mask_pass + constants::SCENE_PASS_STAGE_PASS_OFFSET,
                    constants::CUSTOM_STAGE_PASS_SILHOUETTE
                );
                store<std::uint64_t>(mask_pass + constants::SCENE_PASS_FILTERS_OFFSET, constants::FB_CUSTOM_RENDER);
                heal_silhouette_psos(render_view);
                sort_custom_list_far_to_near(render_view);
                reinterpret_cast<DrawRenderItemsFn>(s_calls.draw_render_items)(
                    mask_pass,
                    render_view,
                    constants::RENDER_LIST_CUSTOM,
                    0,
                    0x7FFFFFFF
                );
                *counter = *counter - 1;
                counted = false;

                jobified_drawer = drawer;
                jobified = true;
                reinterpret_cast<DrawerFn>(s_calls.jobify)(drawer);
                reinterpret_cast<DrawerFn>(s_calls.wait)(drawer);
                jobified = false;

                std::memcpy(
                    reinterpret_cast<void *>(mask_pass + constants::SCENE_PASS_VIEWPORTS_OFFSET),
                    saved_viewports,
                    sizeof(saved_viewports)
                );
                std::memcpy(
                    reinterpret_cast<void *>(mask_pass + constants::SCENE_PASS_SCISSOR_OFFSET),
                    saved_scissor,
                    sizeof(saved_scissor)
                );
                placed = false;
                reinterpret_cast<SceneSetRenderTargetsFn>(s_calls.scene_set_render_targets)(
                    mask_pass,
                    saved_depth,
                    saved_color,
                    0,
                    0,
                    0
                );
                retargeted = false;

                store<std::uint64_t>(pipeline + constants::PIPELINE_FLAGS_OFFSET, saved_flags);
                return MaskStatus::Drawn;
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                if (jobified)
                {
                    guarded_wait_draws(s_calls.wait, jobified_drawer);
                }
                if (placed)
                {
                    std::memcpy(
                        reinterpret_cast<void *>(mask_pass + constants::SCENE_PASS_VIEWPORTS_OFFSET),
                        saved_viewports,
                        sizeof(saved_viewports)
                    );
                    std::memcpy(
                        reinterpret_cast<void *>(mask_pass + constants::SCENE_PASS_SCISSOR_OFFSET),
                        saved_scissor,
                        sizeof(saved_scissor)
                    );
                }
                if (retargeted)
                {
                    (void)guarded_set_render_targets(mask_pass, saved_depth, saved_color);
                }
                if (counted)
                {
                    *counter = *counter - 1;
                }
                if (flags_saved)
                {
                    store<std::uint64_t>(pipeline + constants::PIPELINE_FLAGS_OFFSET, saved_flags);
                }
                return MaskStatus::Fault;
            }
        }

        /**
         * @brief The supersampled mask's size for an output of @p output_width by @p output_height:
         *        SUPERSAMPLED_MASK_SCALE times it on each axis, scaled down to SUPERSAMPLED_MASK_MAX_PIXELS.
         */
        [[nodiscard]] std::pair<std::int32_t, std::int32_t>
        supersampled_mask_size(std::uint32_t output_width, std::uint32_t output_height) noexcept
        {
            const double pixels = static_cast<double>(output_width) * static_cast<double>(output_height);
            const double scale = std::min<double>(
                constants::SUPERSAMPLED_MASK_SCALE,
                std::sqrt(constants::SUPERSAMPLED_MASK_MAX_PIXELS / std::max(pixels, 1.0))
            );
            const auto side = [scale](std::uint32_t size)
            {
                const double scaled = std::round(static_cast<double>(size) * scale);
                return static_cast<std::int32_t>(
                    std::clamp(scaled, 1.0, static_cast<double>(constants::SUPERSAMPLED_MASK_MAX_SIDE))
                );
            };
            return {side(output_width), side(output_height)};
        }

        /**
         * @brief Creates the supersampled targets at @p width by @p height, or resizes them, with the formats and
         *        flags of the targets the mask pass normally draws into (render thread).
         * @param with_temp Also the blur's intermediate ([Render] Softness above 0).
         * @return True when every target exists at that size and format.
         */
        [[nodiscard]] bool guarded_ensure_supersampled_targets(
            std::uintptr_t stage,
            std::int32_t width,
            std::int32_t height,
            bool with_temp
        ) noexcept
        {
            __try
            {
                const std::uintptr_t mask_pass = stage + constants::CUSTOM_STAGE_MASK_PASS_OFFSET;
                const std::uintptr_t color_reference =
                    load<std::uintptr_t>(mask_pass + constants::SCENE_PASS_COLOR_TARGET_OFFSET);
                const std::uintptr_t depth_reference =
                    load<std::uintptr_t>(mask_pass + constants::SCENE_PASS_DEPTH_TARGET_OFFSET);
                if (color_reference == 0 || depth_reference == 0)
                {
                    return false;
                }
                const auto color_format = load<std::uint8_t>(color_reference + constants::TEXTURE_FORMAT_OFFSET);
                const std::uint32_t color_flags =
                    load<std::uint32_t>(color_reference + constants::TEXTURE_FLAGS_OFFSET) &
                    ~constants::TEXTURE_FLAG_DONT_RELEASE;
                const auto depth_format = load<std::uint8_t>(depth_reference + constants::TEXTURE_FORMAT_OFFSET);
                const std::uint32_t depth_flags =
                    load<std::uint32_t>(depth_reference + constants::TEXTURE_FLAGS_OFFSET) &
                    ~constants::TEXTURE_FLAG_DONT_RELEASE;
                const float clear_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                const float clear_depth[4] = {constants::SUPERSAMPLED_MASK_FAR_DEPTH, 0.0f, 0.0f, 0.0f};
                const auto create_target = reinterpret_cast<PostFxTargetFn>(s_calls.post_fx_render_target);
                const auto create_depth = reinterpret_cast<PostFxTargetFn>(s_calls.post_fx_depth_stencil);
                if (!create_target(
                        s_ss_mask_name.data(),
                        &s_ss.mask,
                        width,
                        height,
                        clear_color,
                        false,
                        false,
                        color_format,
                        -1,
                        color_flags
                    ) ||
                    !create_depth(
                        s_ss_depth_name.data(),
                        &s_ss.depth,
                        width,
                        height,
                        clear_depth,
                        false,
                        false,
                        depth_format,
                        -1,
                        depth_flags
                    ))
                {
                    return false;
                }
                if (with_temp && !create_target(
                                     s_ss_temp_name.data(),
                                     &s_ss.temp,
                                     width,
                                     height,
                                     clear_color,
                                     false,
                                     false,
                                     color_format,
                                     -1,
                                     color_flags
                                 ))
                {
                    return false;
                }
                const auto matches = [width, height](std::uintptr_t texture, std::uint8_t format)
                {
                    return texture != 0 && load<std::uint8_t>(texture + constants::TEXTURE_FORMAT_OFFSET) == format &&
                           load<std::uint16_t>(texture + constants::TEXTURE_WIDTH_OFFSET) == width &&
                           load<std::uint16_t>(texture + constants::TEXTURE_HEIGHT_OFFSET) == height;
                };
                return matches(s_ss.mask, color_format) && matches(s_ss.depth, depth_format) &&
                       (!with_temp || matches(s_ss.temp, color_format));
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return false;
            }
        }

        /** @brief Releases one of the mod's textures (CTexture vtable Release); false on a fault. */
        [[nodiscard]] bool guarded_release_texture(std::uintptr_t texture) noexcept
        {
            __try
            {
                const std::uintptr_t vtable = load<std::uintptr_t>(texture);
                (void)reinterpret_cast<
                    TextureReleaseFn>(load<std::uintptr_t>(vtable + constants::TEXTURE_VTABLE_RELEASE_OFFSET))(texture);
                return true;
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return false;
            }
        }

        /** @brief Releases the supersampled targets (shutdown, once the render path is idle). */
        void release_supersampled_targets() noexcept
        {
            for (std::uintptr_t *slot : {&s_ss.mask, &s_ss.depth, &s_ss.temp})
            {
                if (*slot != 0)
                {
                    // A texture whose Release faulted is left alone rather than released twice.
                    (void)guarded_release_texture(*slot);
                    *slot = 0;
                }
            }
        }

        /**
         * @brief The texture the composite writes to (the render output after P5, else the display target), whose
         *        size is the output resolution; 0 when it is not there or the call faulted.
         */
        [[nodiscard]] std::uintptr_t guarded_output_target(std::uintptr_t stage) noexcept
        {
            __try
            {
                const std::uintptr_t resources = load<std::uintptr_t>(stage + constants::STAGE_RESOURCES_OFFSET);
                const std::uintptr_t fn = s_p5_installed.load(std::memory_order_acquire) && s_calls.ldr_target != 0
                                              ? s_calls.ldr_target
                                              : s_calls.display_target;
                return resources != 0
                           ? reinterpret_cast<DisplayTargetFn>(fn)(resources, constants::DISPLAY_TARGET_STAGE)
                           : 0;
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return 0;
            }
        }

        /**
         * @enum CompositeStatus
         * @brief Outcome of one composite.
         */
        enum class CompositeStatus : std::uint8_t
        {
            Executed,
            NoShader,
            NoTarget,
            NoNames,
            Fault,
        };

        /**
         * @struct CustomComposite
         * @brief The mod's technique and what it draws with (shaders/HenrySensesSilhouette.cfx).
         */
        struct CustomComposite
        {
            /// The CShader holding the technique.
            std::uintptr_t shader{0};
            /// Outline brightness.
            float strength{0.0f};
            /// Tint inside an outline word's silhouette.
            float interior{0.0f};
            /// Outline width in mask texels (the destination may be larger than the mask).
            float outline_width{1.0f};
        };

        /**
         * @brief The view's linear depth ($ZTarget) for the mod's technique, or 0 while it has no device texture.
         *        Raw reads: called inside the composite's SEH scope.
         */
        [[nodiscard]] std::uintptr_t linear_depth_texture(std::uintptr_t stage) noexcept
        {
            const std::uintptr_t resources = load<std::uintptr_t>(stage + constants::STAGE_RESOURCES_OFFSET);
            const std::uintptr_t depth =
                resources != 0 ? load<std::uintptr_t>(resources + constants::RESOURCES_LINEAR_DEPTH_OFFSET) : 0;
            return depth != 0 && load<std::uintptr_t>(depth + constants::TEXTURE_DEVICE_OFFSET) != 0 ? depth : 0;
        }

        /** @brief Interns the mod's technique constant names once (render thread); false when one failed. */
        [[nodiscard]] bool ensure_custom_names() noexcept
        {
            if (s_custom_names_ready)
            {
                return true;
            }
            const bool first_ok = guarded_make_name(s_calls.cry_name_r, &s_name_custom_params0, "HSSilhouetteParams0");
            const bool second_ok = guarded_make_name(s_calls.cry_name_r, &s_name_custom_params1, "HSSilhouetteParams1");
            const bool third_ok = guarded_make_name(s_calls.cry_name_r, &s_name_custom_params2, "HSSilhouetteParams2");
            s_custom_names_ready = first_ok && second_ok && third_ok;
            return s_custom_names_ready;
        }

        /**
         * @brief Sets the mod's technique constants for a pass that draws into @p target from @p mask.
         * @details The outline width is given in texels of the render-resolution mask (the INI's unit), whatever the
         *          mask's own size; the edge's soft band is one mask texel in target pixels. Raw loads: called inside
         *          a composite's SEH scope.
         * @return False when a size reads 0.
         */
        [[nodiscard]] bool set_custom_constants(
            std::uintptr_t pass,
            std::uintptr_t stage,
            std::uintptr_t mask,
            std::uintptr_t target,
            const MaskVisibility &visibility,
            float strength,
            float interior,
            float outline_width
        ) noexcept
        {
            const std::uintptr_t resources = load<std::uintptr_t>(stage + constants::STAGE_RESOURCES_OFFSET);
            const std::uintptr_t unit =
                resources != 0 ? load<std::uintptr_t>(resources + constants::RESOURCES_SCENE_NORMALS_OFFSET) : mask;
            const float unit_width = load<std::uint16_t>((unit != 0 ? unit : mask) + constants::TEXTURE_WIDTH_OFFSET);
            const float mask_width = load<std::uint16_t>(mask + constants::TEXTURE_WIDTH_OFFSET);
            const float target_width = load<std::uint16_t>(target + constants::TEXTURE_WIDTH_OFFSET);
            const float target_height = load<std::uint16_t>(target + constants::TEXTURE_HEIGHT_OFFSET);
            if (!(unit_width > 0.0f && mask_width > 0.0f && target_width > 0.0f && target_height > 0.0f))
            {
                return false;
            }
            const float params0[4] = {
                strength,
                interior,
                outline_width * target_width / unit_width,
                std::max(target_width / mask_width, 0.75f)
            };
            const float params1[4] =
                {visibility.offset_u, visibility.offset_v, 1.0f / target_width, 1.0f / target_height};
            const float params2[4] = {visibility.texture != 0 ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
            const auto set_constant = reinterpret_cast<SetConstantFn>(s_calls.set_constant);
            (void)set_constant(pass, &s_name_custom_params0, params0, 1);
            (void)set_constant(pass, &s_name_custom_params1, params1, 1);
            (void)set_constant(pass, &s_name_custom_params2, params2, 1);
            return true;
        }

        /**
         * @brief Composites the mask into the frame after tone-mapping with the stage's own fullscreen pass.
         * @param stage The CSceneCustomStage.
         * @param mask The mask texture (supersampled, or $SceneNormalsMap).
         * @param visibility The depth-tested mask (SeeThrough = false), or none.
         * @param ps_params psParams (strength, boost, brightness, fill strength).
         * @param vs_params vsParams (outline offset in texels, 0, 0, 0).
         * @param custom The mod's technique to draw with instead of the stock one, or nullptr.
         * @return The outcome.
         */
        [[nodiscard]] CompositeStatus guarded_composite(
            std::uintptr_t stage,
            std::uintptr_t mask,
            const MaskVisibility &visibility,
            const float *ps_params,
            const float *vs_params,
            CompositePoint point,
            const CustomComposite *custom,
            std::uint32_t *out_results,
            std::uintptr_t *out_destination
        ) noexcept
        {
            __try
            {
                if (!s_names_ready)
                {
                    const bool ps_ok = guarded_make_name(s_calls.cry_name_r, &s_name_ps_params, "psParams");
                    const bool vs_ok = guarded_make_name(s_calls.cry_name_r, &s_name_vs_params, "vsParams");
                    if (!ps_ok || !vs_ok)
                    {
                        return CompositeStatus::NoNames;
                    }
                    s_names_ready = true;
                }
                if (custom != nullptr && !ensure_custom_names())
                {
                    return CompositeStatus::NoNames;
                }
                const std::uintptr_t shader =
                    custom != nullptr ? custom->shader : load<std::uintptr_t>(s_calls.post_effects_game_slot);
                if (shader == 0)
                {
                    return CompositeStatus::NoShader;
                }
                // The stock technique knows one mask only: with SeeThrough = false it draws the depth-tested one.
                if (custom == nullptr && visibility.texture != 0)
                {
                    mask = visibility.texture;
                }
                const std::uintptr_t resources = load<std::uintptr_t>(stage + constants::STAGE_RESOURCES_OFFSET);
                // After the HDR chain the frame is $DisplayTargetDst (past the upscaler); after the LDR chain it is the
                // render output's colour target, which the engine's own after-LDR pass fetches the same way.
                std::uintptr_t destination = 0;
                if (point == CompositePoint::AfterLdr && s_calls.ldr_target != 0)
                {
                    destination = reinterpret_cast<DisplayTargetFn>(s_calls.ldr_target)(
                        resources,
                        constants::DISPLAY_TARGET_STAGE
                    );
                }
                else
                {
                    destination = reinterpret_cast<DisplayTargetFn>(s_calls.display_target)(
                        resources,
                        constants::DISPLAY_TARGET_STAGE
                    );
                }
                if (destination == 0 || load<std::uintptr_t>(destination + constants::TEXTURE_DEVICE_OFFSET) == 0)
                {
                    return CompositeStatus::NoTarget;
                }
                *out_destination = destination;

                const std::uintptr_t pass = stage + constants::CUSTOM_STAGE_HIGHLIGHT_PASS_OFFSET;
                if (load<std::uint8_t>(pass + constants::FULLSCREEN_REQUIRE_PER_VIEW_CB_OFFSET) == 0)
                {
                    store<std::uint8_t>(pass + constants::FULLSCREEN_REQUIRE_PER_VIEW_CB_OFFSET, 1);
                }
                const std::uint32_t destination_id = load<std::uint32_t>(destination + constants::TEXTURE_ID_OFFSET);
                const std::uint32_t generation = s_binding_generation.load(std::memory_order_acquire);
                const std::uintptr_t depth = custom != nullptr ? linear_depth_texture(stage) : 0;
                // The technique reads t2 in every permutation, so it is bound to the mask when nothing is depth-tested.
                const std::uintptr_t visible =
                    custom != nullptr ? (visibility.texture != 0 ? visibility.texture : mask) : std::uintptr_t{0};
                // Until the mod's technique has drawn once it is bound every frame: its parse and its permutation
                // finish in the background, and a technique set before that is not taken.
                if (pass != s_bound_pass || destination_id != s_bound_destination_id || mask != s_bound_mask ||
                    generation != s_bound_generation || shader != s_bound_shader || depth != s_bound_depth ||
                    visible != s_bound_visibility || (custom != nullptr && !s_custom_confirmed))
                {
                    reinterpret_cast<SetTechniqueFn>(s_calls.set_technique)(
                        pass + constants::FULLSCREEN_PRIMITIVE_OFFSET,
                        shader,
                        custom != nullptr ? &s_custom_technique_key : &s_technique_key,
                        0,
                        false
                    );
                    const bool target_ok = reinterpret_cast<SetRenderTargetFn>(s_calls.set_render_target)(
                        pass + constants::PRIMITIVE_PASS_RENDER_TARGET_THIS_OFFSET,
                        0,
                        destination,
                        constants::RESOURCE_VIEW_RENDER_TARGET
                    );
                    s_bound_target_ok = target_ok;
                    const std::uintptr_t dirty = pass + constants::FULLSCREEN_DIRTY_OFFSET;
                    if (load<std::uint64_t>(pass + constants::FULLSCREEN_RENDER_STATE_OFFSET) !=
                        constants::RENDER_STATE_ADDITIVE_NO_DEPTH)
                    {
                        store<std::uint64_t>(
                            pass + constants::FULLSCREEN_RENDER_STATE_OFFSET,
                            constants::RENDER_STATE_ADDITIVE_NO_DEPTH
                        );
                        store<std::uint32_t>(
                            dirty,
                            load<std::uint32_t>(dirty) | constants::FULLSCREEN_DIRTY_RENDER_STATE
                        );
                    }
                    reinterpret_cast<SetTextureFn>(s_calls.set_texture)(
                        pass + constants::FULLSCREEN_RESOURCE_DESC_OFFSET,
                        0,
                        mask,
                        0,
                        constants::SHADER_STAGE_PIXEL
                    );
                    if (depth != 0)
                    {
                        reinterpret_cast<SetTextureFn>(s_calls.set_texture)(
                            pass + constants::FULLSCREEN_RESOURCE_DESC_OFFSET,
                            1,
                            depth,
                            0,
                            constants::SHADER_STAGE_PIXEL
                        );
                    }
                    if (visible != 0)
                    {
                        reinterpret_cast<SetTextureFn>(s_calls.set_texture)(
                            pass + constants::FULLSCREEN_RESOURCE_DESC_OFFSET,
                            2,
                            visible,
                            0,
                            constants::SHADER_STAGE_PIXEL
                        );
                    }
                    (void)reinterpret_cast<SetSamplerFn>(s_calls.set_sampler)(
                        pass + constants::FULLSCREEN_RESOURCE_DESC_OFFSET,
                        0,
                        constants::SAMPLER_LINEAR_CLAMP,
                        constants::SHADER_STAGE_PIXEL
                    );
                    if (load<std::uint32_t>(pass + constants::FULLSCREEN_PRIMITIVE_TYPE_OFFSET) !=
                        constants::PRIMITIVE_TYPE_PROCEDURAL_TRIANGLE)
                    {
                        store<std::uint32_t>(
                            pass + constants::FULLSCREEN_PRIMITIVE_TYPE_OFFSET,
                            constants::PRIMITIVE_TYPE_PROCEDURAL_TRIANGLE
                        );
                        store<std::uint32_t>(
                            dirty,
                            load<std::uint32_t>(dirty) | constants::FULLSCREEN_DIRTY_PRIMITIVE_TYPE
                        );
                    }
                    s_bound_pass = pass;
                    s_bound_destination_id = destination_id;
                    s_bound_mask = mask;
                    s_bound_generation = generation;
                    s_bound_shader = shader;
                    s_bound_depth = depth;
                    s_bound_visibility = visible;
                }

                reinterpret_cast<BeginConstantUpdateFn>(s_calls.begin_constant_update)(pass);
                if (custom != nullptr)
                {
                    if (!set_custom_constants(
                            pass,
                            stage,
                            mask,
                            destination,
                            visibility,
                            custom->strength,
                            custom->interior,
                            custom->outline_width
                        ))
                    {
                        return CompositeStatus::NoTarget;
                    }
                }
                else
                {
                    (void)reinterpret_cast<SetConstantFn>(s_calls.set_constant)(pass, &s_name_ps_params, ps_params, 1);
                    (void)reinterpret_cast<SetConstantFn>(s_calls.set_constant)(pass, &s_name_vs_params, vs_params, 0);
                }
                const bool executed = reinterpret_cast<FullscreenExecuteFn>(s_calls.fullscreen_execute)(pass);
                // Bit 0: the render target bound; bit 1: Execute reported success.
                *out_results = (s_bound_target_ok ? 1u : 0u) | (executed ? 2u : 0u);
                return CompositeStatus::Executed;
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return CompositeStatus::Fault;
            }
        }

        /**
         * @brief Draws the focus coverage mask: @p target cleared to white, then multiplied by one minus the composite
         *        technique's alpha over @p mask, so red = 0 wherever an Outline or OutlineFill word covers a texel.
         * @details The stage's resolve pass draws it with the composite's technique; the fill term is large enough
         *          that every covered texel's alpha saturates, whatever its fade. A Fill word carries no alpha, so its
         *          object counts as background.
         * @param stage The CSceneCustomStage.
         * @param mask The mask texture (supersampled, or $SceneNormalsMap).
         * @param visibility The depth-tested mask (SeeThrough = false), or none.
         * @param target $SceneDiffuseTmp (render resolution; the mask is sampled by UV, whatever its size).
         * @return 1 when the pass executed, 0 when it had nothing to draw yet (its shader or names not ready, or a
         *         primitive still compiling), -1 on a fault.
         */
        [[nodiscard]] int guarded_focus_coverage(
            std::uintptr_t stage,
            std::uintptr_t mask,
            const MaskVisibility &visibility,
            std::uintptr_t target,
            std::uintptr_t custom_shader,
            float outline_width
        ) noexcept
        {
            __try
            {
                if (!s_names_ready)
                {
                    const bool ps_ok = guarded_make_name(s_calls.cry_name_r, &s_name_ps_params, "psParams");
                    const bool vs_ok = guarded_make_name(s_calls.cry_name_r, &s_name_vs_params, "vsParams");
                    if (!ps_ok || !vs_ok)
                    {
                        return 0;
                    }
                    s_names_ready = true;
                }
                if (custom_shader != 0 && !ensure_custom_names())
                {
                    return 0;
                }
                const std::uintptr_t shader =
                    custom_shader != 0 ? custom_shader : load<std::uintptr_t>(s_calls.post_effects_game_slot);
                if (shader == 0)
                {
                    return 0;
                }
                if (custom_shader == 0 && visibility.texture != 0)
                {
                    mask = visibility.texture;
                }
                const std::uintptr_t pass = stage + constants::CUSTOM_STAGE_RESOLVE_PASS_OFFSET;
                if (load<std::uint8_t>(pass + constants::FULLSCREEN_REQUIRE_PER_VIEW_CB_OFFSET) == 0)
                {
                    store<std::uint8_t>(pass + constants::FULLSCREEN_REQUIRE_PER_VIEW_CB_OFFSET, 1);
                }
                const std::uint32_t target_id = load<std::uint32_t>(target + constants::TEXTURE_ID_OFFSET);
                const std::uint32_t generation = s_binding_generation.load(std::memory_order_acquire);
                const std::uintptr_t depth = custom_shader != 0 ? linear_depth_texture(stage) : 0;
                const std::uintptr_t visible =
                    custom_shader != 0 ? (visibility.texture != 0 ? visibility.texture : mask) : std::uintptr_t{0};
                if (pass != s_focus_bound_pass || target_id != s_focus_bound_target_id || mask != s_focus_bound_mask ||
                    generation != s_focus_bound_generation || shader != s_focus_bound_shader ||
                    depth != s_focus_bound_depth || visible != s_focus_bound_visibility ||
                    (custom_shader != 0 && !s_coverage_confirmed))
                {
                    reinterpret_cast<SetTechniqueFn>(s_calls.set_technique)(
                        pass + constants::FULLSCREEN_PRIMITIVE_OFFSET,
                        shader,
                        custom_shader != 0 ? &s_custom_coverage_key : &s_technique_key,
                        0,
                        false
                    );
                    (void)reinterpret_cast<SetRenderTargetFn>(s_calls.set_render_target)(
                        pass + constants::PRIMITIVE_PASS_RENDER_TARGET_THIS_OFFSET,
                        0,
                        target,
                        constants::RESOURCE_VIEW_RENDER_TARGET
                    );
                    const std::uintptr_t dirty = pass + constants::FULLSCREEN_DIRTY_OFFSET;
                    if (load<std::uint64_t>(pass + constants::FULLSCREEN_RENDER_STATE_OFFSET) !=
                        constants::RENDER_STATE_COVERAGE_NO_DEPTH)
                    {
                        store<std::uint64_t>(
                            pass + constants::FULLSCREEN_RENDER_STATE_OFFSET,
                            constants::RENDER_STATE_COVERAGE_NO_DEPTH
                        );
                        store<std::uint32_t>(
                            dirty,
                            load<std::uint32_t>(dirty) | constants::FULLSCREEN_DIRTY_RENDER_STATE
                        );
                    }
                    reinterpret_cast<SetTextureFn>(s_calls.set_texture)(
                        pass + constants::FULLSCREEN_RESOURCE_DESC_OFFSET,
                        0,
                        mask,
                        0,
                        constants::SHADER_STAGE_PIXEL
                    );
                    if (depth != 0)
                    {
                        reinterpret_cast<SetTextureFn>(s_calls.set_texture)(
                            pass + constants::FULLSCREEN_RESOURCE_DESC_OFFSET,
                            1,
                            depth,
                            0,
                            constants::SHADER_STAGE_PIXEL
                        );
                    }
                    if (visible != 0)
                    {
                        reinterpret_cast<SetTextureFn>(s_calls.set_texture)(
                            pass + constants::FULLSCREEN_RESOURCE_DESC_OFFSET,
                            2,
                            visible,
                            0,
                            constants::SHADER_STAGE_PIXEL
                        );
                    }
                    (void)reinterpret_cast<SetSamplerFn>(s_calls.set_sampler)(
                        pass + constants::FULLSCREEN_RESOURCE_DESC_OFFSET,
                        0,
                        constants::SAMPLER_LINEAR_CLAMP,
                        constants::SHADER_STAGE_PIXEL
                    );
                    if (load<std::uint32_t>(pass + constants::FULLSCREEN_PRIMITIVE_TYPE_OFFSET) !=
                        constants::PRIMITIVE_TYPE_PROCEDURAL_TRIANGLE)
                    {
                        store<std::uint32_t>(
                            pass + constants::FULLSCREEN_PRIMITIVE_TYPE_OFFSET,
                            constants::PRIMITIVE_TYPE_PROCEDURAL_TRIANGLE
                        );
                        store<std::uint32_t>(
                            dirty,
                            load<std::uint32_t>(dirty) | constants::FULLSCREEN_DIRTY_PRIMITIVE_TYPE
                        );
                    }
                    s_focus_bound_pass = pass;
                    s_focus_bound_target_id = target_id;
                    s_focus_bound_mask = mask;
                    s_focus_bound_generation = generation;
                    s_focus_bound_shader = shader;
                    s_focus_bound_depth = depth;
                    s_focus_bound_visibility = visible;
                }

                const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                reinterpret_cast<ClearSurfaceFn>(s_calls.clear_surface)(
                    stage + constants::CUSTOM_STAGE_MASK_CLEAR_PASS_OFFSET,
                    target,
                    white
                );
                reinterpret_cast<BeginConstantUpdateFn>(s_calls.begin_constant_update)(pass);
                const auto set_constant = reinterpret_cast<SetConstantFn>(s_calls.set_constant);
                if (custom_shader != 0)
                {
                    // The same outline the composite draws, at the coverage target's resolution.
                    if (!set_custom_constants(pass, stage, mask, target, visibility, 0.0f, 0.0f, outline_width))
                    {
                        return 0;
                    }
                }
                else
                {
                    // psParams: strength, boost, brightness, fill; vsParams: the edge sample offset (unused here).
                    const float ps_params[4] = {1.0f, 0.0f, 1.0f, FOCUS_FILL_SCALE};
                    const float vs_params[4] = {1.0f, 0.0f, 0.0f, 0.0f};
                    (void)set_constant(pass, &s_name_ps_params, ps_params, 1);
                    (void)set_constant(pass, &s_name_vs_params, vs_params, 0);
                }
                return reinterpret_cast<FullscreenExecuteFn>(s_calls.fullscreen_execute)(pass) ? 1 : 0;
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return -1;
            }
        }

        /** @brief Clears the focus coverage target to white (everything darkens); false on a fault. */
        [[nodiscard]] bool guarded_clear_coverage(std::uintptr_t stage, std::uintptr_t target) noexcept
        {
            __try
            {
                const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                reinterpret_cast<ClearSurfaceFn>(s_calls.clear_surface)(
                    stage + constants::CUSTOM_STAGE_MASK_CLEAR_PASS_OFFSET,
                    target,
                    white
                );
                return true;
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return false;
            }
        }

        /**
         * @brief Draws the focus coverage mask from @p mask while the controller asks for it (render thread).
         * @param stage The CSceneCustomStage.
         * @param mask The mask this view drew, or 0 (nothing highlighted: the coverage is cleared).
         * @param visibility The depth-tested mask (SeeThrough = false), or none.
         */
        void render_focus_coverage(std::uintptr_t stage, std::uintptr_t mask, const MaskVisibility &visibility) noexcept
        {
            if (!s_focus_wanted.load(std::memory_order_relaxed) || s_focus_faulted.load(std::memory_order_relaxed) ||
                !s_composite_calls_ready.load(std::memory_order_acquire))
            {
                return;
            }
            constexpr std::array<std::ptrdiff_t, 3> target_chain{
                constants::STAGE_RESOURCES_OFFSET,
                constants::RESOURCES_SCENE_DIFFUSE_TMP_OFFSET,
                0
            };
            const auto target = DMK::memory::walk(DMK::Address{stage}, target_chain);
            if (!target)
            {
                return;
            }
            const auto device = DMK::memory::read<std::uintptr_t>(target->offset(constants::TEXTURE_DEVICE_OFFSET));
            if (mask == 0)
            {
                // Nothing highlighted this frame. The darkening reads the target every frame, so it is cleared
                // (everything darkens) instead of keeping the last highlight's coverage, which showed a picked-up
                // sword's shape on every later pulse.
                if (device && *device != 0 && !guarded_clear_coverage(stage, target->raw()))
                {
                    s_focus_faulted.store(true, std::memory_order_relaxed);
                    (void)DMK::log().log_noexcept(
                        DMK::LogLevel::Error,
                        "EngineSilhouette: clearing the focus coverage faulted; focus stays off for this session"
                    );
                }
                return;
            }
            if (!device || *device == 0)
            {
                return;
            }
            // The mod's coverage technique covers the outline ring the composite draws around each silhouette too, so
            // the darkening leaves the whole highlight lit; until it draws, the stock technique covers the silhouette.
            const std::uintptr_t custom =
                s_custom_confirmed && !s_coverage_given_up ? silhouette_shader() : std::uintptr_t{0};
            const float outline_width = settings().outline_width.load(std::memory_order_relaxed);
            int result = guarded_focus_coverage(stage, mask, visibility, target->raw(), custom, outline_width);
            if (custom != 0 && !s_coverage_confirmed)
            {
                if (result == 1)
                {
                    s_coverage_confirmed = true;
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Info,
                        "Focus: the {} coverage drew after {} attempt(s)",
                        constants::SILHOUETTE_COVERAGE_TECHNIQUE,
                        s_coverage_failures + 1
                    );
                }
                else if (result == 0)
                {
                    if (++s_coverage_failures >= CUSTOM_WARMUP_ATTEMPTS)
                    {
                        s_coverage_given_up = true;
                        (void)DMK::log().try_log(
                            DMK::LogLevel::Warning,
                            "Focus: the {} coverage never drew; the stock coverage is used for the session",
                            constants::SILHOUETTE_COVERAGE_TECHNIQUE
                        );
                    }
                    result = guarded_focus_coverage(stage, mask, visibility, target->raw(), 0, outline_width);
                }
            }
            if (result < 0)
            {
                s_focus_faulted.store(true, std::memory_order_relaxed);
                (void)DMK::log().log_noexcept(
                    DMK::LogLevel::Error,
                    "EngineSilhouette: the focus coverage pass faulted; focus stays off for this session"
                );
                return;
            }
            if (result > 0)
            {
                s_focus_frames.fetch_add(1, std::memory_order_relaxed);
            }
        }

        /**
         * @brief Reads the composite pass's state for the composite report: Execute's return value only says the pass
         *        holds compiled primitives, so the device pass and the owning pipeline's scheduler are read too.
         * @param stage The stage that owns the pass.
         * @return The state; a field whose read fails stays zero.
         */
        [[nodiscard]] CompositeDiag read_composite_diag(std::uintptr_t stage) noexcept
        {
            const std::uintptr_t pass = stage + constants::CUSTOM_STAGE_HIGHLIGHT_PASS_OFFSET;
            CompositeDiag diag{};
            const auto begin =
                DMK::memory::read<std::uintptr_t>(DMK::Address{pass + constants::PRIMITIVE_PASS_COMPILED_BEGIN_OFFSET});
            const auto end =
                DMK::memory::read<std::uintptr_t>(DMK::Address{pass + constants::PRIMITIVE_PASS_COMPILED_END_OFFSET});
            if (begin && end && *end >= *begin)
            {
                diag.primitives = static_cast<std::uint32_t>((*end - *begin) / sizeof(void *));
            }
            const auto device_pass =
                DMK::memory::read<std::uintptr_t>(DMK::Address{pass + constants::PRIMITIVE_PASS_DEVICE_PASS_OFFSET});
            if (device_pass && DMK::memory::is_plausible_ptr(DMK::Address{*device_pass}))
            {
                diag.device_pass = *device_pass;
                diag.device_pass_valid = DMK::memory::read<std::uint8_t>(
                                             DMK::Address{*device_pass + constants::DEVICE_RENDER_PASS_VALID_OFFSET}
                )
                                             .value_or(0);
            }
            const auto pipeline =
                DMK::memory::read<std::uintptr_t>(DMK::Address{pass + constants::PRIMITIVE_PASS_PIPELINE_OFFSET});
            if (pipeline && DMK::memory::is_plausible_ptr(DMK::Address{*pipeline}))
            {
                diag.scheduler_enabled = DMK::memory::read<std::uint8_t>(
                                             DMK::Address{*pipeline + constants::PIPELINE_SCHEDULER_ENABLED_OFFSET}
                )
                                             .value_or(0);
                constexpr std::array<std::ptrdiff_t, 2> active_chain{
                    constants::PIPELINE_SCHEDULER_OWNER_OFFSET,
                    constants::SCHEDULER_OWNER_ACTIVE_OFFSET
                };
                if (const auto active = DMK::memory::walk(DMK::Address{*pipeline}, active_chain))
                {
                    diag.scheduler_active = DMK::memory::read<std::uint32_t>(*active).value_or(0);
                }
            }
            return diag;
        }

        /**
         * @brief Reads the item count of render list 27 without taking any engine lock.
         * @param render_view The executing view.
         * @param pending Receives whether worker chunks were still unmerged.
         * @return The item count.
         */
        [[nodiscard]] std::uint32_t read_custom_list_count(std::uintptr_t render_view, bool &pending) noexcept
        {
            const std::uintptr_t list = render_view + constants::RENDER_VIEW_LISTS_OFFSET +
                                        constants::RENDER_VIEW_LIST_STRIDE * constants::RENDER_LIST_CUSTOM;
            const auto begin =
                DMK::memory::read<std::uintptr_t>(DMK::Address{list + constants::RENDER_LIST_BEGIN_OFFSET});
            const auto end = DMK::memory::read<std::uintptr_t>(DMK::Address{list + constants::RENDER_LIST_END_OFFSET});
            const auto chunk =
                DMK::memory::read<std::uintptr_t>(DMK::Address{list + constants::RENDER_LIST_PENDING_OFFSET});
            pending = chunk.has_value() && *chunk != 0;
            if (!begin || !end || *end < *begin)
            {
                return 0;
            }
            return static_cast<std::uint32_t>((*end - *begin) >> constants::RENDER_LIST_ITEM_SHIFT);
        }

        /**
         * @brief Reports whether a custom stage's Init ran: its resource layout and mask render pass exist.
         * @details Raw reads for the P2 and P4 paths, where the stage is engine-owned and valid for the call.
         * @param stage A CSceneCustomStage.
         * @return True when both pointers are set.
         */
        [[nodiscard]] bool stage_initialized(std::uintptr_t stage) noexcept
        {
            return load<std::uintptr_t>(stage + constants::CUSTOM_STAGE_RESOURCE_LAYOUT_OFFSET) != 0 &&
                   load<std::uintptr_t>(
                       stage + constants::CUSTOM_STAGE_MASK_PASS_OFFSET + constants::SCENE_PASS_DEVICE_PASS_OFFSET
                   ) != 0;
        }

        /**
         * @brief Logs the P1 outcome after the original Init returned.
         * @param pipeline The pipeline.
         * @param registered The stage the detour registered, or 0.
         * @param wanted Whether registration was requested.
         */
        void report_pipeline_init(std::uintptr_t pipeline, std::uintptr_t registered, bool wanted) noexcept
        {
            const auto slot =
                DMK::memory::read<std::uintptr_t>(DMK::Address{pipeline + constants::PIPELINE_CUSTOM_STAGE_OFFSET});
            const std::uintptr_t stage = slot ? *slot : 0;
            const bool pipeline_ok = object_is(GameClass::StdPipeline, pipeline);
            const bool stage_ok = stage != 0 && object_is(GameClass::CustomStage, stage);
            std::uintptr_t layout = 0;
            std::uintptr_t device_pass = 0;
            if (stage != 0)
            {
                const auto l = DMK::memory::read<std::uintptr_t>(
                    DMK::Address{stage + constants::CUSTOM_STAGE_RESOURCE_LAYOUT_OFFSET}
                );
                const auto d = DMK::memory::read<std::uintptr_t>(DMK::Address{
                    stage + constants::CUSTOM_STAGE_MASK_PASS_OFFSET + constants::SCENE_PASS_DEVICE_PASS_OFFSET
                });
                layout = l ? *l : 0;
                device_pass = d ? *d : 0;
            }
            if (registered != 0)
            {
                s_p1_registered.fetch_add(1, std::memory_order_relaxed);
                s_last_stage.store(stage, std::memory_order_relaxed);
            }
            const DMK::LogLevel level = registered != 0 && (!stage_ok || layout == 0 || device_pass == 0)
                                            ? DMK::LogLevel::Error
                                            : DMK::LogLevel::Info;
            (void)DMK::log().try_log(
                level,
                "EngineSilhouette[A1] P1: CStandardGraphicsPipeline::Init #{} pipeline=0x{:016X} (vtable {}) "
                "registration {} "
                "-> P+0x50=0x{:016X} (CSceneCustomStage vtable {}), stage layout=0x{:016X}, mask render "
                "pass=0x{:016X}{}",
                s_p1_calls.load(std::memory_order_relaxed),
                pipeline,
                pipeline_ok ? "ok" : "MISMATCH",
                registered != 0 ? "done before Init" : (wanted ? "FAILED or skipped (slot occupied)" : "not requested"),
                stage,
                stage == 0 ? "-" : (stage_ok ? "ok" : "MISMATCH"),
                layout,
                device_pass,
                registered != 0 && (layout == 0 || device_pass == 0) ? " - stage Init did NOT run" : ""
            );
        }

        /**
         * @brief P1: registers the custom stage before the original Init so the base Init initializes it.
         * @param pipeline The CStandardGraphicsPipeline being initialized.
         */
        void __fastcall detour_std_pipeline_init(std::uintptr_t pipeline) noexcept
        {
            const InFlightScope scope;
            s_p1_calls.fetch_add(1, std::memory_order_relaxed);
            const bool wanted = s_armed.load(std::memory_order_acquire) &&
                                !s_unpublish_requested.load(std::memory_order_acquire) && stage_registration_wanted() &&
                                s_p2_installed.load(std::memory_order_acquire);
            std::uintptr_t registered = 0;
            if (wanted && pipeline != 0)
            {
                const auto slot =
                    DMK::memory::read<std::uintptr_t>(DMK::Address{pipeline + constants::PIPELINE_CUSTOM_STAGE_OFFSET});
                if (slot && *slot == 0)
                {
                    registered = guarded_register_stage(anchor_address(AnchorId::RegisterCustomStage), pipeline);
                }
            }
            s_std_init_original(pipeline);
            s_binding_generation.fetch_add(1, std::memory_order_acq_rel);
            s_last_pipeline.store(pipeline, std::memory_order_relaxed);
            note_p1_pipeline(pipeline, registered);
            report_pipeline_init(pipeline, registered, wanted);
        }

        /**
         * @brief P2: keeps the debug passes from compiling and, optionally, a failed silhouette PSO from hiding an
         *        object, and records the depth test of every silhouette PSO it builds (heal_silhouette_psos).
         *        Re-entrant (render thread and compile jobs).
         */
        bool __fastcall detour_custom_create_pso(
            void *stage,
            const void *desc,
            std::uint8_t pass_id,
            void **out_pso
        ) noexcept
        {
            // Counted: the original can block for a long time on a cold-cache pipeline compile.
            const InFlightScope scope;
            if (pass_id < constants::CUSTOM_PASS_SILHOUETTE)
            {
                // The debug-view passes are never drawn in game; success with a null PSO keeps the object complete.
                release_pso(out_pso);
                s_p2_debug_suppressed.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            std::uint64_t object_flags = 0;
            if (pass_id == constants::CUSTOM_PASS_SILHOUETTE && desc != nullptr)
            {
                // Every compiled object requests the silhouette PSO; only the objects the mod highlights carry the
                // marker, and the per-material cache keys the two apart. The rest complete with no PSO, as the debug
                // passes do, so the engine never compiles silhouette shaders for them.
                object_flags = *reinterpret_cast<const std::uint64_t *>(
                    static_cast<const std::uint8_t *>(desc) + constants::PSO_DESC_OBJECT_FLAGS_OFFSET
                );
                if ((object_flags & constants::FOB_HENRYSENSES_MARKER) == 0)
                {
                    s_p2_seen_flags.fetch_or(object_flags, std::memory_order_relaxed);
                    release_pso(out_pso);
                    s_p2_pass4_gated.fetch_add(1, std::memory_order_relaxed);
                    return true;
                }
            }
            if (pass_id == constants::CUSTOM_PASS_SILHOUETTE &&
                !stage_initialized(reinterpret_cast<std::uintptr_t>(stage)))
            {
                // A stage whose Init never ran or aborted has no layout or mask render pass, so the silhouette PSO
                // cannot exist; it completes with no PSO without counting as a failed compile.
                release_pso(out_pso);
                s_p2_pass4_uninitialized.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            bool ok = false;
            {
                // A pipeline-state compile on a cold cache stalls the calling thread.
                DMK_PROFILE_SCOPE("create_pso_original");
                ok = s_create_pso_original(stage, desc, pass_id, out_pso);
            }
            if (pass_id != constants::CUSTOM_PASS_SILHOUETTE)
            {
                return ok;
            }
            if (ok)
            {
                s_p2_pass4_ok.fetch_add(1, std::memory_order_relaxed);
                // The original decides the depth test from the same bit (CSceneCustomStage::CreatePipelineState).
                if (*out_pso != nullptr)
                {
                    record_pso_depth(
                        reinterpret_cast<std::uintptr_t>(*out_pso),
                        (object_flags & constants::FOB_HUD_REQUIRE_DEPTHTEST) != 0
                    );
                }
                return true;
            }
            // A failed silhouette PSO would leave the object incomplete in every pass (missing from the world), so it
            // completes with no PSO instead: the object loses only its outline.
            s_p2_pass4_failed.fetch_add(1, std::memory_order_relaxed);
            release_pso(out_pso);
            s_p2_pass4_overridden.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        /**
         * @brief P3: adds the silhouette flags to every entity's SRendParams for the general pass, and the node's
         *        silhouette word when it has one.
         * @details The proxy never reads its own +0x3C (only CBrush does), so the mod owns that word for entities.
         *          The copy constructor inside the original copies +0x78 to every slot's render parameters, and no
         *          emitter downstream writes it. Lock-free, no SEH (hot path on job threads): the proxy and the pass
         *          info are engine-owned and valid for the call.
         */
        void __fastcall detour_proxy_render(void *self, void *params, const void *pass_info) noexcept
        {
            // Counted: the original can stall (character skinning, streaming) and then returns into this module.
            const StripedInFlightScope scope;
            const auto node = reinterpret_cast<std::uintptr_t>(self);
            const std::uint32_t word =
                *reinterpret_cast<const volatile std::uint32_t *>(node + constants::RENDERNODE_HUD_SILHOUETTE_OFFSET);
            const auto *pass = static_cast<const std::uint8_t *>(pass_info);
            if (params == nullptr || pass == nullptr || pass[constants::PASS_INFO_RECURSION_OFFSET] != 0 ||
                pass[constants::PASS_INFO_SHADOW_OFFSET] != 0 || !s_armed.load(std::memory_order_relaxed))
            {
                s_proxy_render_original(self, params, pass_info);
                // Visible entities render every frame (static meshes reach RenderInternal only when their cached
                // render object is rebuilt), so this is the dependable once-per-frame point for the herb draws.
                if (s_armed.load(std::memory_order_relaxed))
                {
                    herb_outline_on_render(pass_info);
                }
                return;
            }
            const auto base = reinterpret_cast<std::uintptr_t>(params);
            auto *const slot =
                reinterpret_cast<volatile std::uint32_t *>(base + constants::SRENDPARAMS_HUD_SILHOUETTE_OFFSET);
            auto *const flags =
                reinterpret_cast<volatile std::uint64_t *>(base + constants::SRENDPARAMS_OBJ_FLAGS_OFFSET);
            const std::uint32_t saved = *slot;
            const std::uint64_t saved_flags = *flags;
            // Every entity carries the silhouette flags, highlighted or not, and only the word decides whether it is
            // drawn. The flags are part of every pipeline-state key, so an entity that gained them only when it was
            // highlighted had its cached compiled objects rebuilt, and one of the two copies the engine alternates
            // between frames kept its old pipeline states (no silhouette PSO) for seconds: the fill blinked every other
            // frame. The marker also lets P2 build the silhouette PSO for entities only.
            *flags = saved_flags | silhouette_object_flags();
            if (word != 0)
            {
                *slot = word;
            }
            s_proxy_render_original(self, params, pass_info);
            *flags = saved_flags;
            *slot = saved;
            if (word != 0)
            {
                s_p3_injected.fetch_add(1, std::memory_order_relaxed);
            }
            herb_outline_on_render(pass_info);
        }

        /**
         * @brief P3b: marks a render object that carries a silhouette word before its mesh is submitted.
         * @details The octree renders brushes without SRendParams: it copies the brush's HUD word (+0x3C) straight into
         *          the CRenderObject (+0xA4) and calls RenderInternal, which only ORs flags into the object. The marker
         *          therefore goes into the object's m_ObjFlags here, before the render thread compiles it, and lets P2
         *          build the silhouette PSO for it. Objects that came through P3 already carry it. Lock-free, no SEH:
         *          the object is engine-owned and valid for the call.
         */
        std::uintptr_t __fastcall detour_render_internal(
            void *stat_obj,
            void *render_object,
            void *hide_mask,
            void *lod_value,
            const void *pass_info
        ) noexcept
        {
            const StripedInFlightScope scope;
            const bool armed = s_armed.load(std::memory_order_relaxed);
            if (render_object != nullptr && armed)
            {
                const auto object = reinterpret_cast<std::uintptr_t>(render_object);
                auto *const word_slot =
                    reinterpret_cast<volatile std::uint32_t *>(object + constants::RENDER_OBJECT_HUD_SILHOUETTE_OFFSET);
                // A herb this thread submits: CStatObj::Render does not copy the SRendParams word, so it goes in here.
                if (const std::uint32_t herb_word = herb_outline_thread_word(); herb_word != 0)
                {
                    *word_slot = herb_word;
                }
                const std::uint32_t word = *word_slot;
                auto *const flags =
                    reinterpret_cast<volatile long long *>(object + constants::RENDER_OBJECT_FLAGS_OFFSET);
                const std::uint64_t wanted = silhouette_object_flags();
                if (word != 0 && (static_cast<std::uint64_t>(*flags) & wanted) != wanted)
                {
                    _InterlockedOr64(flags, static_cast<long long>(wanted));
                    s_p3b_marked.fetch_add(1, std::memory_order_relaxed);
                }
            }
            const std::uintptr_t result =
                s_render_internal_original(stat_obj, render_object, hide_mask, lod_value, pass_info);
            if (armed)
            {
                herb_outline_on_render(pass_info);
            }
            return result;
        }

        /**
         * @brief True for a render object of a herb silhouette draw: the proxy alpha and a silhouette word. No engine
         *        object pairs a word with that exact alpha.
         */
        [[nodiscard]] bool is_herb_proxy(std::uintptr_t render_object) noexcept
        {
            return load<std::uint32_t>(render_object + constants::RENDER_OBJECT_HUD_SILHOUETTE_OFFSET) != 0 &&
                   load<std::uint32_t>(render_object + constants::RENDER_OBJECT_ALPHA_OFFSET) ==
                       std::bit_cast<std::uint32_t>(constants::HERB_PROXY_ALPHA);
        }

        /**
         * @brief Takes the herb silhouette draws out of EFSLIST_ZPREPASS of @p render_view, keeping the order of the
         *        rest.
         * @details Scene passes ignore object alpha, so a herb proxy, drawn only for its silhouette, would also write
         *          its depth (the plant at rest, without the wind) in front of the real plant. Each segment of the
         *          list (the head and its pending chunks) is compacted in place and its end moved back; the count
         *          DrawRenderItems takes next sums the segments again. Raw reads and no object with a destructor: it
         *          runs inside guarded_strip_herb_prepass's SEH scope.
         * @return The number of items removed.
         */
        std::size_t strip_herb_prepass(std::uintptr_t render_view) noexcept
        {
            constexpr int max_segments = 64;
            constexpr std::size_t item_size = std::size_t{1} << constants::RENDER_LIST_ITEM_SHIFT;
            std::size_t removed = 0;
            std::uintptr_t segment = render_view + constants::RENDER_VIEW_LISTS_OFFSET +
                                     constants::RENDER_VIEW_LIST_STRIDE * constants::RENDER_LIST_ZPREPASS;
            for (int depth = 0; segment != 0 && depth < max_segments; ++depth)
            {
                const auto begin = load<std::uintptr_t>(segment + constants::RENDER_LIST_BEGIN_OFFSET);
                const auto end = load<std::uintptr_t>(segment + constants::RENDER_LIST_END_OFFSET);
                if (begin != 0 && end > begin && (end - begin) % item_size == 0)
                {
                    std::uintptr_t write = begin;
                    for (std::uintptr_t item = begin; item < end; item += item_size)
                    {
                        const auto compiled =
                            load<std::uintptr_t>(item + constants::RENDER_ITEM_COMPILED_OBJECT_OFFSET);
                        const auto object =
                            compiled != 0
                                ? load<std::uintptr_t>(compiled + constants::COMPILED_OBJECT_RENDER_OBJECT_OFFSET)
                                : 0;
                        if (object != 0 && is_herb_proxy(object))
                        {
                            ++removed;
                            continue;
                        }
                        if (write != item)
                        {
                            std::memcpy(
                                reinterpret_cast<void *>(write),
                                reinterpret_cast<const void *>(item),
                                item_size
                            );
                        }
                        write += item_size;
                    }
                    if (write != end)
                    {
                        store<std::uintptr_t>(segment + constants::RENDER_LIST_END_OFFSET, write);
                    }
                }
                segment = load<std::uintptr_t>(segment + constants::RENDER_LIST_PENDING_OFFSET);
            }
            return removed;
        }

        /** @brief strip_herb_prepass() under SEH; std::nullopt when it faulted. */
        [[nodiscard]] std::optional<std::size_t> guarded_strip_herb_prepass(std::uintptr_t render_view) noexcept
        {
            __try
            {
                return strip_herb_prepass(render_view);
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return std::nullopt;
            }
        }

        /**
         * @brief P7: CSceneRenderPass::DrawRenderItems. Before the depth pre-pass draws EFSLIST_ZPREPASS, the herb
         *        silhouette draws are taken out of it, so they reach the silhouette mask only.
         */
        void __fastcall detour_draw_render_items(
            std::uintptr_t pass,
            std::uintptr_t render_view,
            std::uint8_t list,
            std::int32_t start,
            std::int32_t end
        ) noexcept
        {
            const StripedInFlightScope scope;
            if (list == constants::RENDER_LIST_ZPREPASS && render_view != 0 &&
                s_armed.load(std::memory_order_relaxed) && !s_p7_faulted.load(std::memory_order_relaxed) &&
                herb_outline_count() != 0)
            {
                if (const std::optional<std::size_t> removed = guarded_strip_herb_prepass(render_view);
                    !removed.has_value())
                {
                    s_p7_faulted.store(true, std::memory_order_relaxed);
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Error,
                        "EngineSilhouette: the depth pre-pass filter faulted on render view 0x{:016X}; herb "
                        "silhouette draws stay in the pre-pass for this session",
                        render_view
                    );
                }
                else
                {
                    s_p7_stripped.fetch_add(*removed, std::memory_order_relaxed);
                }
            }
            s_draw_render_items_original(pass, render_view, list, start, end);
        }

        /**
         * @brief Registers and initializes the stage from the render thread when P1 missed the pipeline.
         * @param pipeline The executing pipeline.
         * @param track Its tracking entry, or nullptr.
         */
        void late_attach(std::uintptr_t pipeline, TrackedPipeline *track) noexcept
        {
            const std::uintptr_t stage =
                guarded_register_stage(anchor_address(AnchorId::RegisterCustomStage), pipeline);
            if (stage == 0)
            {
                s_late_attach_failed.store(true, std::memory_order_release);
                (void)DMK::log().try_log(
                    DMK::LogLevel::Error,
                    "EngineSilhouette[A1] late attach: RegisterStage failed on pipeline 0x{:016X}; "
                    "late attach disabled",
                    pipeline
                );
                return;
            }
            // RegisterStage already published the engine-constructed stage at P+0x50, so it is initialized whatever
            // the identity check says: a published stage without Init would make every silhouette PSO fail. Init is
            // taken from the new object's own vtable (slot 1), so no extra anchor is needed.
            const bool identity_ok = object_is(GameClass::CustomStage, stage);
            const std::uintptr_t init = read_vtable_slot(stage, constants::STAGE_VTABLE_INIT_OFFSET);
            const bool initialized = init != 0 && guarded_stage_init(init, stage);
            if (!initialized || !identity_ok)
            {
                s_late_attach_failed.store(true, std::memory_order_release);
            }
            s_binding_generation.fetch_add(1, std::memory_order_acq_rel);
            s_late_attached.fetch_add(1, std::memory_order_relaxed);
            s_last_stage.store(stage, std::memory_order_relaxed);
            note_published_stage(track, stage, StageOrigin::LateAttach);
            const auto layout =
                DMK::memory::read<std::uintptr_t>(DMK::Address{stage + constants::CUSTOM_STAGE_RESOURCE_LAYOUT_OFFSET});
            (void)DMK::log().try_log(
                initialized && identity_ok ? DMK::LogLevel::Info : DMK::LogLevel::Error,
                "EngineSilhouette[A1] late attach on the render thread: pipeline=0x{:016X} stage=0x{:016X} "
                "(CSceneCustomStage "
                "vtable {}) Init={} layout=0x{:016X}; objects compiled earlier gain the silhouette PSO on their next "
                "compile",
                pipeline,
                stage,
                identity_ok ? "ok" : "MISMATCH",
                initialized ? "ok" : "FAILED",
                layout ? *layout : 0
            );
        }

        /**
         * @brief Checks that a stashed stage still belongs to the executing pipeline and is initialized.
         * @details The default standard pipeline is created once per renderer, so a pipeline found at the stashed
         *          address with the stashed forward stage, owning a stage that points back at it, is the same pipeline.
         * @param entry The stash entry.
         * @param pipeline The executing pipeline.
         * @param forward_stage Its CSceneForwardStage.
         * @return nullptr when the stage can be adopted, else the reason it cannot.
         */
        [[nodiscard]] const char *
        stashed_stage_rejection(const StashEntry &entry, std::uintptr_t pipeline, std::uintptr_t forward_stage) noexcept
        {
            if (entry.forward != forward_stage)
            {
                return "the pipeline has another forward stage, so it was recreated";
            }
            if (entry.stage == 0 || !object_is(GameClass::CustomStage, entry.stage))
            {
                return "the stashed object is not a CSceneCustomStage";
            }
            const auto owner =
                DMK::memory::read<std::uintptr_t>(DMK::Address{entry.stage + constants::STAGE_PIPELINE_OFFSET});
            const auto resources =
                DMK::memory::read<std::uintptr_t>(DMK::Address{entry.stage + constants::STAGE_RESOURCES_OFFSET});
            if (!owner || *owner != pipeline || !resources ||
                *resources != pipeline + constants::PIPELINE_RESOURCES_OFFSET)
            {
                return "the stage belongs to another pipeline";
            }
            const auto layout = DMK::memory::read<std::uintptr_t>(
                DMK::Address{entry.stage + constants::CUSTOM_STAGE_RESOURCE_LAYOUT_OFFSET}
            );
            const auto device_pass = DMK::memory::read<std::uintptr_t>(DMK::Address{
                entry.stage + constants::CUSTOM_STAGE_MASK_PASS_OFFSET + constants::SCENE_PASS_DEVICE_PASS_OFFSET
            });
            if (!layout || *layout == 0 || !device_pass || *device_pass == 0)
            {
                return "the stage was never initialized";
            }
            return nullptr;
        }

        /**
         * @brief Republishes the stage a previous generation unpublished from this pipeline.
         * @details Adoption comes before late attach: the stashed stage is already initialized, and objects compiled
         *          while it was published keep PSOs built from its passes. A rejected entry is dropped (its stage stays
         *          allocated and unused) and the caller takes the fresh path.
         * @param pipeline The executing pipeline, whose stage slot is 0.
         * @param forward_stage Its CSceneForwardStage.
         * @param track Its tracking entry, or nullptr.
         * @return True when this frame published a stage (adopted, or the slot changed under the exchange).
         * @note Render thread, inside P4, with P2 installed.
         */
        [[nodiscard]] bool
        adopt_stashed_stage(std::uintptr_t pipeline, std::uintptr_t forward_stage, TrackedPipeline *track) noexcept
        {
            const std::size_t index = stash_find(pipeline);
            if (index == STASH_ENTRIES)
            {
                return false;
            }
            const StashEntry entry = stash_read(index);
            if (const char *const rejection = stashed_stage_rejection(entry, pipeline, forward_stage);
                rejection != nullptr)
            {
                stash_write(index, StashEntry{});
                (void)DMK::log().try_log(
                    DMK::LogLevel::Error,
                    "EngineSilhouette[A1] stashed stage 0x{:016X} for pipeline 0x{:016X} not adopted ({}); it "
                    "stays allocated and unused, and the pipeline takes the fresh path",
                    entry.stage,
                    pipeline,
                    rejection
                );
                return false;
            }
            if (!guarded_exchange_stage_slot(pipeline, 0, entry.stage))
            {
                // Something published a stage since the slot was read; the next frame sees it.
                return true;
            }
            stash_write(index, StashEntry{});
            s_binding_generation.fetch_add(1, std::memory_order_acq_rel);
            s_adopted.fetch_add(1, std::memory_order_relaxed);
            s_last_stage.store(entry.stage, std::memory_order_relaxed);
            note_published_stage(track, entry.stage, StageOrigin::Adopted);
            (void)DMK::log().try_log(
                DMK::LogLevel::Info,
                "EngineSilhouette[A1] adopted stage 0x{:016X} on pipeline 0x{:016X}: the previous generation "
                "unpublished it, P2 was armed before this republish, and the stage was neither "
                "registered nor initialized again",
                entry.stage,
                pipeline
            );
            return true;
        }

        /**
         * @brief P4's shutdown mode: takes the stage off the executing pipeline and records it for the next generation.
         * @details The stage is stashed before the slot is cleared, so no instant exists in which it is neither
         *          published nor recorded. Without a persistent area (the release ASI) or a free stash entry the stage
         *          is still unpublished: an allocated, unused stage is harmless, while a published one without P2
         *          compiles the debug-view shaders.
         * @param pipeline The executing pipeline.
         * @param forward_stage Its CSceneForwardStage.
         * @param stage The value read from P+0x50.
         * @param track Its tracking entry, or nullptr.
         * @note Render thread, inside P4. See the file comment for why only this point may clear the slot.
         */
        void unpublish_on_render_thread(
            std::uintptr_t pipeline,
            std::uintptr_t forward_stage,
            std::uintptr_t stage,
            TrackedPipeline *track
        ) noexcept
        {
            if (track != nullptr && track->unpublished.load(std::memory_order_acquire))
            {
                return;
            }
            if (stage == 0 || !object_is(GameClass::CustomStage, stage))
            {
                if (track != nullptr)
                {
                    track->unpublished.store(true, std::memory_order_release);
                }
                return;
            }
            const bool stashed = stash_put(
                StashEntry{
                    .pipeline = pipeline,
                    .stage = stage,
                    .forward = forward_stage,
                }
            );
            if (!guarded_exchange_stage_slot(pipeline, stage, 0))
            {
                if (stashed)
                {
                    stash_write(stash_find(pipeline), StashEntry{});
                }
                return;
            }
            s_unpublished.fetch_add(1, std::memory_order_relaxed);
            if (track != nullptr)
            {
                track->stage.store(stage, std::memory_order_relaxed);
                track->unpublished.store(true, std::memory_order_release);
            }
            (void)DMK::log().try_log(
                DMK::LogLevel::Info,
                "EngineSilhouette[A1] unpublished stage 0x{:016X} from pipeline 0x{:016X} on the render thread "
                "(P+0x50 = 0); {}",
                stage,
                pipeline,
                stashed                 ? "recorded for the next generation to adopt"
                : s_stash_slots.empty() ? "no persistent area, so it stays allocated and unused"
                                        : "the stash is full, so it stays allocated and unused"
            );
        }

        /**
         * @brief Logs, once per pipeline, the first frame P4 executes it without a stage.
         * @param pipeline The pipeline.
         * @param track Its tracking entry, or nullptr.
         */
        void report_first_stageless_frame(std::uintptr_t pipeline, TrackedPipeline *track) noexcept
        {
            if (track == nullptr || track->first_frame_reported)
            {
                return;
            }
            track->first_frame_reported = true;
            std::uintptr_t registered = 0;
            const bool p1_saw = p1_saw_pipeline(pipeline, registered);
            (void)DMK::log().try_log(
                DMK::LogLevel::Info,
                "EngineSilhouette[A1] pipeline 0x{:016X} first frame without a stage; P1 {} its Init; "
                "registration {}{}",
                pipeline,
                p1_saw ? "saw" : "missed (the pipeline existed before the early hooks)",
                stage_registration_wanted() ? "requested" : "not requested",
                stash_find(pipeline) != STASH_ENTRIES ? "; a stashed stage waits for it" : ""
            );
        }

        /**
         * @brief Records a frame with a stage on @p pipeline and logs how the stage got there, once per stage.
         * @param pipeline The pipeline.
         * @param stage Its CSceneCustomStage.
         * @param track Its tracking entry, or nullptr.
         */
        void note_stage_frame(std::uintptr_t pipeline, std::uintptr_t stage, TrackedPipeline *track) noexcept
        {
            s_last_stage.store(stage, std::memory_order_relaxed);
            s_stage_seen_ms.store(now_ms(), std::memory_order_relaxed);
            s_p4_stage_frames.fetch_add(1, std::memory_order_relaxed);
            if (track == nullptr)
            {
                return;
            }
            track->stage.store(stage, std::memory_order_relaxed);
            track->seen_ms.store(now_ms(), std::memory_order_release);
            if (track->reported_stage == stage)
            {
                return;
            }
            track->reported_stage = stage;
            track->first_frame_reported = true;

            std::uintptr_t registered = 0;
            const bool p1_saw = p1_saw_pipeline(pipeline, registered);
            StageOrigin origin = track->origin.load(std::memory_order_acquire);
            if (origin == StageOrigin::Unknown)
            {
                origin = registered == stage ? StageOrigin::PipelineInit : StageOrigin::Found;
                track->origin.store(origin, std::memory_order_release);
            }
            // A stash entry for a pipeline that already shows a stage is stale: that stage is what the pipeline uses.
            if (const std::size_t stale = stash_find(pipeline); stale != STASH_ENTRIES)
            {
                const StashEntry entry = stash_read(stale);
                stash_write(stale, StashEntry{});
                if (entry.stage != stage)
                {
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Warning,
                        "EngineSilhouette[A1] dropped stashed stage 0x{:016X} for pipeline 0x{:016X}: the "
                        "pipeline already shows stage 0x{:016X}; the stashed one stays allocated and "
                        "unused",
                        entry.stage,
                        pipeline,
                        stage
                    );
                }
            }
            const char *path = "found already published (a previous generation kept it published, or another mod "
                               "registered it)";
            switch (origin)
            {
            case StageOrigin::PipelineInit:
                path = "registered by P1 before CStandardGraphicsPipeline::Init (the early hooks were in time)";
                break;
            case StageOrigin::LateAttach:
                path = "registered and initialized by the render-thread late attach (P1 missed the Init)";
                break;
            case StageOrigin::Adopted:
                path = "adopted from the stash of the previous generation";
                break;
            case StageOrigin::Found:
            case StageOrigin::Unknown:
            default:
                break;
            }
            (void)DMK::log().try_log(
                DMK::LogLevel::Info,
                "EngineSilhouette[A1] pipeline 0x{:016X} draws with stage 0x{:016X}: {}; P1 {} this pipeline's "
                "Init",
                pipeline,
                stage,
                path,
                p1_saw ? "saw" : "missed"
            );
        }

        /**
         * @brief Logs the first frame (and every change) of the stage's engine-driven state (A2).
         * @param pipeline The executing pipeline.
         * @param stage The stage.
         * @param render_view The stage's render view.
         */
        void report_stage_state(std::uintptr_t pipeline, std::uintptr_t stage, std::uintptr_t render_view) noexcept
        {
            static std::uintptr_t s_reported_stage = 0;
            static bool s_reported_texture = false;
            const auto current_view =
                DMK::memory::read<std::uintptr_t>(DMK::Address{pipeline + constants::PIPELINE_CURRENT_VIEW_OFFSET});
            // stage -> resources -> $SceneNormalsMap -> its device texture, in one walk; the trace keeps the mask even
            // when the device hop fails.
            constexpr std::array<std::ptrdiff_t, 4> mask_chain{
                constants::STAGE_RESOURCES_OFFSET,
                constants::RESOURCES_SCENE_NORMALS_OFFSET,
                constants::TEXTURE_DEVICE_OFFSET,
                0
            };
            std::array<DMK::Address, 4> trace{};
            (void)DMK::memory::walk(DMK::Address{stage}, mask_chain, trace);
            const std::uintptr_t mask = DMK::memory::is_plausible_ptr(trace[1]) ? trace[1].raw() : 0;
            const std::uintptr_t device = DMK::memory::is_plausible_ptr(trace[2]) ? trace[2].raw() : 0;
            const bool texture_ok = device != 0;
            if (stage == s_reported_stage && texture_ok == s_reported_texture)
            {
                return;
            }
            s_reported_stage = stage;
            s_reported_texture = texture_ok;
            const auto layout =
                DMK::memory::read<std::uintptr_t>(DMK::Address{stage + constants::CUSTOM_STAGE_RESOURCE_LAYOUT_OFFSET});
            const auto device_pass = DMK::memory::read<std::uintptr_t>(DMK::Address{
                stage + constants::CUSTOM_STAGE_MASK_PASS_OFFSET + constants::SCENE_PASS_DEVICE_PASS_OFFSET
            });
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            if (mask != 0)
            {
                const auto w = DMK::memory::read<std::uint16_t>(DMK::Address{mask + constants::TEXTURE_WIDTH_OFFSET});
                const auto h = DMK::memory::read<std::uint16_t>(DMK::Address{mask + constants::TEXTURE_HEIGHT_OFFSET});
                width = w ? *w : 0;
                height = h ? *h : 0;
            }
            (void)DMK::log().try_log(
                texture_ok ? DMK::LogLevel::Info : DMK::LogLevel::Warning,
                "EngineSilhouette[A2] stage 0x{:016X} on pipeline 0x{:016X}: render view 0x{:016X} ({} P+0x1978), "
                "layout 0x{:016X}, mask render "
                "pass 0x{:016X}, mask $SceneNormalsMap 0x{:016X} device 0x{:016X} ({}x{})",
                stage,
                pipeline,
                render_view,
                current_view && *current_view == render_view ? "==" : "!=",
                layout ? *layout : 0,
                device_pass ? *device_pass : 0,
                mask,
                device,
                width,
                height
            );
        }

        /**
         * @brief Emits the periodic render-path statistics line (A3, A4, B1, B3, C1).
         */
        void report_stats() noexcept
        {
            static std::int64_t s_next_ms = 0;
            static std::uint64_t s_prev[8]{};
            static std::uint64_t s_prev_brush = 0;
            static std::uint64_t s_prev_stripped = 0;
            static std::uint64_t s_prev_rebuilds = 0;
            static std::uint64_t s_prev_depth_rebuilds = 0;
            static std::uint64_t s_prev_unrecorded_rebuilds = 0;
            const std::int64_t now = now_ms();
            if (s_next_ms == 0)
            {
                s_next_ms = now + STATS_INTERVAL_MS;
                return;
            }
            if (now < s_next_ms)
            {
                return;
            }
            s_next_ms = now + STATS_INTERVAL_MS;
            const std::uint64_t brush_marked = s_p3b_marked.load(std::memory_order_relaxed);
            const std::uint64_t brush_delta = brush_marked - s_prev_brush;
            s_prev_brush = brush_marked;
            const std::uint64_t stripped = s_p7_stripped.load(std::memory_order_relaxed);
            const std::uint64_t stripped_delta = stripped - s_prev_stripped;
            s_prev_stripped = stripped;
            const std::uint64_t rebuilds = s_p4_rebuilds.load(std::memory_order_relaxed);
            const std::uint64_t rebuilds_delta = rebuilds - s_prev_rebuilds;
            s_prev_rebuilds = rebuilds;
            const std::uint64_t depth_rebuilds = s_p4_depth_rebuilds.load(std::memory_order_relaxed);
            const std::uint64_t depth_rebuilds_delta = depth_rebuilds - s_prev_depth_rebuilds;
            s_prev_depth_rebuilds = depth_rebuilds;
            const std::uint64_t unrecorded_rebuilds = s_p4_unrecorded_rebuilds.load(std::memory_order_relaxed);
            const std::uint64_t unrecorded_rebuilds_delta = unrecorded_rebuilds - s_prev_unrecorded_rebuilds;
            s_prev_unrecorded_rebuilds = unrecorded_rebuilds;
            const std::uint64_t current[8] = {
                s_p4_frames.load(std::memory_order_relaxed),
                s_p4_stage_frames.load(std::memory_order_relaxed),
                s_p4_item_frames.load(std::memory_order_relaxed),
                s_p4_masks.load(std::memory_order_relaxed),
                s_p4_composites.load(std::memory_order_relaxed),
                s_p3_injected.load(std::memory_order_relaxed),
                s_p2_pass4_ok.load(std::memory_order_relaxed),
                s_p2_pass4_failed.load(std::memory_order_relaxed),
            };
            std::uint64_t delta[8]{};
            for (int i = 0; i < 8; ++i)
            {
                delta[i] = current[i] - s_prev[i];
                s_prev[i] = current[i];
            }
            // Without highlighted items, masks, composites, injections or silhouette compiles there is nothing to
            // report (stage frames alone count every in-game frame, idle ones too).
            if (delta[2] == 0 && delta[3] == 0 && delta[4] == 0 && delta[5] == 0 && delta[6] == 0 && delta[7] == 0 &&
                brush_delta == 0)
            {
                return;
            }
            (void)DMK::log().try_log(
                DMK::LogLevel::Debug,
                "EngineSilhouette stats ({}s): frames={} stageFrames={} list27[A4] framesWithItems={} lastItems={} "
                "maxItems={} flags={:#x} | masks[B1]={} composites[C1]={} rebuilds asked={} (SeeThrough {}, "
                "unrecorded {}) | "
                "P3 injected={} "
                "P3b marked={} | P7 {} "
                "herb draw(s) kept out of the depth pre-pass{} | P2[A3] pass4 ok={} failed={} (total failed={}, "
                "overridden={}, gated={}, debug suppressed={})",
                STATS_INTERVAL_MS / 1000,
                delta[0],
                delta[1],
                delta[2],
                s_p4_last_items.load(std::memory_order_relaxed),
                s_p4_max_items.exchange(0, std::memory_order_relaxed),
                s_p4_last_flags.load(std::memory_order_relaxed),
                delta[3],
                delta[4],
                rebuilds_delta,
                depth_rebuilds_delta,
                unrecorded_rebuilds_delta,
                delta[5],
                brush_delta,
                stripped_delta,
                !s_p7_installed.load(std::memory_order_relaxed) ? " (hook off)"
                : s_p7_faulted.load(std::memory_order_relaxed)  ? " (faulted, off)"
                                                                : "",
                delta[6],
                delta[7],
                current[7],
                s_p2_pass4_overridden.load(std::memory_order_relaxed),
                s_p2_pass4_gated.load(std::memory_order_relaxed),
                s_p2_debug_suppressed.load(std::memory_order_relaxed)
            );
        }

        void draw_frame(std::uintptr_t pipeline, std::uintptr_t stage, std::uintptr_t render_view) noexcept;

        [[nodiscard]] void *guarded_construct_blur(std::uintptr_t ctor, void *memory, std::uintptr_t pipeline) noexcept
        {
            __try
            {
                return reinterpret_cast<BlurCtorFn>(ctor)(memory, pipeline, BLUR_PASS_NAME);
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return nullptr;
            }
        }

        [[nodiscard]] bool guarded_destroy_blur(void *pass) noexcept
        {
            __try
            {
                const auto vtable = *static_cast<const std::uintptr_t *>(pass);
                reinterpret_cast<ScalarDtorFn>(*reinterpret_cast<const std::uintptr_t *>(vtable))(pass, 0);
                return true;
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return false;
            }
        }

        [[nodiscard]] bool guarded_blur(
            std::uintptr_t execute,
            void *pass,
            std::uintptr_t texture,
            std::uintptr_t temp,
            float distribution
        ) noexcept
        {
            __try
            {
                reinterpret_cast<BlurExecuteFn>(execute)(pass, texture, temp, 1.0f, distribution, false);
                return true;
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return false;
            }
        }

        /**
         * @brief Destroys the mask blur pass (render thread, or shutdown once the render path is idle).
         */
        void release_blur_pass() noexcept
        {
            if (s_blur_pass == nullptr)
            {
                return;
            }
            void *const pass = s_blur_pass;
            s_blur_pass = nullptr;
            s_blur_pipeline = 0;
            if (guarded_destroy_blur(pass))
            {
                _aligned_free(pass);
            }
            // A pass whose destructor faulted is leaked rather than freed under engine references.
        }

        /**
         * @brief Blurs the mask in place with the mod's CGaussianBlurPass through an intermediate of its size: the
         *        supersampled targets' own, or $SceneDiffuseTmp for the render-resolution mask (render thread).
         * @param pipeline The executing pipeline, which the pass is built for.
         * @param stage The custom stage.
         * @param mask The mask texture.
         * @param supersampled The mask is the supersampled one (s_ss.mask).
         * @param distribution The Gaussian sigma in render texels.
         */
        void blur_mask(
            std::uintptr_t pipeline,
            std::uintptr_t stage,
            std::uintptr_t mask,
            bool supersampled,
            float distribution
        ) noexcept
        {
            if (s_blur_faulted.load(std::memory_order_relaxed))
            {
                return;
            }
            const std::uintptr_t ctor = gated_anchor_address(Feature::MaskBlur, AnchorId::BlurPassCtor);
            const std::uintptr_t execute = gated_anchor_address(Feature::MaskBlur, AnchorId::BlurPassExecute);
            if (ctor == 0 || execute == 0)
            {
                return;
            }
            constexpr std::array<std::ptrdiff_t, 3> temp_chain{
                constants::STAGE_RESOURCES_OFFSET,
                constants::RESOURCES_SCENE_DIFFUSE_TMP_OFFSET,
                0
            };
            constexpr std::array<std::ptrdiff_t, 3> unit_chain{
                constants::STAGE_RESOURCES_OFFSET,
                constants::RESOURCES_SCENE_NORMALS_OFFSET,
                0
            };
            std::uintptr_t temp_texture = 0;
            if (supersampled)
            {
                temp_texture = s_ss.temp;
            }
            else if (const auto walked = DMK::memory::walk(DMK::Address{stage}, temp_chain))
            {
                temp_texture = walked->raw();
            }
            if (temp_texture == 0)
            {
                return;
            }
            const DMK::Address temp{temp_texture};
            // The sigma is in render texels; the supersampled mask has more texels per render texel.
            if (supersampled)
            {
                const auto unit = DMK::memory::walk(DMK::Address{stage}, unit_chain);
                if (!unit)
                {
                    return;
                }
                const auto unit_width = DMK::memory::read<std::uint16_t>(unit->offset(constants::TEXTURE_WIDTH_OFFSET));
                const auto width =
                    DMK::memory::read<std::uint16_t>(DMK::Address{mask + constants::TEXTURE_WIDTH_OFFSET});
                if (!unit_width || *unit_width == 0 || !width)
                {
                    return;
                }
                distribution *= static_cast<float>(*width) / static_cast<float>(*unit_width);
            }
            const auto temp_device = DMK::memory::read<std::uintptr_t>(temp.offset(constants::TEXTURE_DEVICE_OFFSET));
            const auto temp_width = DMK::memory::read<std::uint16_t>(temp.offset(constants::TEXTURE_WIDTH_OFFSET));
            const auto temp_height = DMK::memory::read<std::uint16_t>(temp.offset(constants::TEXTURE_HEIGHT_OFFSET));
            const auto mask_width =
                DMK::memory::read<std::uint16_t>(DMK::Address{mask + constants::TEXTURE_WIDTH_OFFSET});
            const auto mask_height =
                DMK::memory::read<std::uint16_t>(DMK::Address{mask + constants::TEXTURE_HEIGHT_OFFSET});
            if (!temp_device || *temp_device == 0 || !temp_width || !temp_height || !mask_width || !mask_height ||
                *temp_width != *mask_width || *temp_height != *mask_height)
            {
                return;
            }
            if (s_blur_pass != nullptr && s_blur_pipeline != pipeline)
            {
                release_blur_pass();
            }
            if (s_blur_pass == nullptr)
            {
                void *memory = _aligned_malloc(constants::BLUR_PASS_SIZE, 16);
                if (memory == nullptr)
                {
                    return;
                }
                std::memset(memory, 0, constants::BLUR_PASS_SIZE);
                if (guarded_construct_blur(ctor, memory, pipeline) == nullptr)
                {
                    _aligned_free(memory);
                    s_blur_faulted.store(true, std::memory_order_relaxed);
                    (void)DMK::log().log_noexcept(
                        DMK::LogLevel::Error,
                        "EngineSilhouette: the mask blur pass could not be built; outlines "
                        "stay hard-edged"
                    );
                    return;
                }
                s_blur_pass = memory;
                s_blur_pipeline = pipeline;
                (void)DMK::log().try_log(
                    DMK::LogLevel::Info,
                    "EngineSilhouette: mask blur pass built for pipeline 0x{:016X} ({}x{} through {})",
                    pipeline,
                    *mask_width,
                    *mask_height,
                    supersampled ? "the supersampled intermediate" : "$SceneDiffuseTmp"
                );
            }
            if (!guarded_blur(execute, s_blur_pass, mask, temp.raw(), distribution))
            {
                s_blur_faulted.store(true, std::memory_order_relaxed);
                (void)DMK::log().log_noexcept(
                    DMK::LogLevel::Error,
                    "EngineSilhouette: the mask blur faulted; outlines stay hard-edged"
                );
            }
        }

        /**
         * @brief P4 body: validates the pipeline, publishes a stage (adoption, then late attach) or unpublishes it at
         *        shutdown, draws the mask and composites it.
         * @param forward_stage The CSceneForwardStage whose after-HDR pass just returned.
         */
        void after_post_hdr(std::uintptr_t forward_stage) noexcept
        {
            DMK_PROFILE_FUNCTION();
            s_p4_frames.fetch_add(1, std::memory_order_relaxed);
            report_stats();
            // A latched fault stops publishing and drawing only. The pipeline bookkeeping and the unpublish still run,
            // because the teardown waits on them before P2 goes.
            const bool faulted = s_render_faulted.load(std::memory_order_relaxed);

            const auto pipeline_read =
                DMK::memory::read<std::uintptr_t>(DMK::Address{forward_stage + constants::STAGE_PIPELINE_OFFSET});
            if (!pipeline_read || *pipeline_read == 0)
            {
                return;
            }
            const std::uintptr_t pipeline = *pipeline_read;
            // Other pipelines (character preview, minimum) run their own Execute; only the standard pipeline whose
            // forward stage is this one is handled.
            const auto forward =
                DMK::memory::read<std::uintptr_t>(DMK::Address{pipeline + constants::PIPELINE_FORWARD_STAGE_OFFSET});
            if (!forward || *forward != forward_stage || !object_is(GameClass::StdPipeline, pipeline))
            {
                return;
            }
            s_last_pipeline.store(pipeline, std::memory_order_relaxed);
            TrackedPipeline *const track = track_pipeline(pipeline);

            const auto stage_read =
                DMK::memory::read<std::uintptr_t>(DMK::Address{pipeline + constants::PIPELINE_CUSTOM_STAGE_OFFSET});
            if (!stage_read)
            {
                return;
            }
            const std::uintptr_t stage = *stage_read;
            if (s_unpublish_requested.load(std::memory_order_seq_cst))
            {
                unpublish_on_render_thread(pipeline, forward_stage, stage, track);
                return;
            }
            if (stage == 0)
            {
                report_first_stageless_frame(pipeline, track);
                // P2 must guard a stage before it is published, whichever path publishes it.
                if (faulted || !stage_registration_wanted() || !s_p2_installed.load(std::memory_order_acquire))
                {
                    return;
                }
                if (adopt_stashed_stage(pipeline, forward_stage, track))
                {
                    return;
                }
                if (!s_late_attach_failed.load(std::memory_order_acquire))
                {
                    late_attach(pipeline, track);
                }
                return;
            }
            if (!object_is(GameClass::CustomStage, stage))
            {
                return;
            }
            note_stage_frame(pipeline, stage, track);
            if (faulted)
            {
                return;
            }
            if (!render_work_pending())
            {
                // Nothing shows: the stage bookkeeping above ran, the view reads, the list diagnostics and the draw
                // wait for a frame with work. P5, when installed, consumes the hand-offs itself.
                if (!s_p5_installed.load(std::memory_order_acquire))
                {
                    clear_view_handoffs();
                }
                return;
            }

            const auto view_read =
                DMK::memory::read<std::uintptr_t>(DMK::Address{stage + constants::STAGE_RENDER_VIEW_OFFSET});
            const std::uintptr_t render_view = view_read ? *view_read : 0;
            const auto current_view =
                DMK::memory::read<std::uintptr_t>(DMK::Address{pipeline + constants::PIPELINE_CURRENT_VIEW_OFFSET});
            report_stage_state(pipeline, stage, render_view);
            if (render_view == 0 || !current_view || *current_view != render_view)
            {
                return;
            }

            bool pending = false;
            const std::uint32_t items = read_custom_list_count(render_view, pending);
            s_p4_last_pending.store(pending, std::memory_order_relaxed);
            const auto list_flags = DMK::memory::read<std::uint32_t>(DMK::Address{
                render_view + constants::RENDER_VIEW_BATCH_FLAGS_OFFSET + 4 * constants::RENDER_LIST_CUSTOM
            });
            const std::uint32_t flags = list_flags ? *list_flags : 0;
            s_p4_last_items.store(items, std::memory_order_relaxed);
            s_p4_last_flags.store(flags, std::memory_order_relaxed);
            if (items > s_p4_max_items.load(std::memory_order_relaxed))
            {
                s_p4_max_items.store(items, std::memory_order_relaxed);
            }
            if ((flags & constants::FB_CUSTOM_RENDER) != 0)
            {
                s_p4_item_frames.fetch_add(1, std::memory_order_relaxed);
            }

            draw_frame(pipeline, stage, render_view);
        }

        /**
         * @brief Draws the silhouette mask for the executing view.
         * @param pipeline The executing CStandardGraphicsPipeline.
         * @param stage Its published, initialized CSceneCustomStage.
         * @param render_view The executing CRenderView.
         * @return The mask texture, or 0 when nothing was drawn (a fault latches the render path off).
         */
        [[nodiscard]] std::uintptr_t
        draw_mask(std::uintptr_t pipeline, std::uintptr_t stage, std::uintptr_t render_view, bool unjitter) noexcept
        {
            DMK_PROFILE_FUNCTION();
            if (!s_mask_calls_ready.load(std::memory_order_acquire))
            {
                return 0;
            }
            const auto layout =
                DMK::memory::read<std::uintptr_t>(DMK::Address{stage + constants::CUSTOM_STAGE_RESOURCE_LAYOUT_OFFSET});
            const auto device_pass = DMK::memory::read<std::uintptr_t>(DMK::Address{
                stage + constants::CUSTOM_STAGE_MASK_PASS_OFFSET + constants::SCENE_PASS_DEVICE_PASS_OFFSET
            });
            if (!layout || *layout == 0 || !device_pass || *device_pass == 0)
            {
                // Init never ran on this stage; its mask pass has nothing to draw with (report_stage_state logged it).
                return 0;
            }
            std::uintptr_t mask = 0;
            const MaskStatus status = guarded_draw_mask(pipeline, stage, render_view, unjitter, &mask);
            if (status == MaskStatus::Fault)
            {
                s_render_faulted.store(true, std::memory_order_release);
                (void)DMK::log().log_noexcept(
                    DMK::LogLevel::Error,
                    "EngineSilhouette[B1]: the mask draw faulted; the render path is "
                    "latched off for this session"
                );
                return 0;
            }
            static MaskStatus s_reported_mask = MaskStatus::Drawn;
            if (status != s_reported_mask && (status == MaskStatus::NoTexture || status == MaskStatus::Busy))
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Warning,
                    "EngineSilhouette[B1]: mask draw skipped ({})",
                    status == MaskStatus::NoTexture ? "$SceneNormalsMap has no device texture"
                                                    : "a scene pass was still open"
                );
            }
            s_reported_mask = status;
            if (status != MaskStatus::Drawn)
            {
                return 0;
            }
            s_p4_masks.fetch_add(1, std::memory_order_relaxed);
            return mask;
        }

        /** @brief True while the supersampled mask can be drawn (its calls resolved, no fault latched). */
        [[nodiscard]] bool supersampled_mask_ready() noexcept
        {
            return s_ss_calls_ready.load(std::memory_order_acquire) && !s_ss_faulted.load(std::memory_order_relaxed);
        }

        /** @brief Latches the supersampled mask off for the session and says why; the render-resolution mask takes
         * over. */
        void reject_supersampled_mask(const char *reason) noexcept
        {
            if (!s_ss_faulted.exchange(true, std::memory_order_relaxed))
            {
                (void)DMK::log().log_noexcept(
                    DMK::LogLevel::Error,
                    "EngineSilhouette: the supersampled mask is off for this session; outlines are drawn from the "
                    "render-resolution mask"
                );
                (void)DMK::log().try_log(DMK::LogLevel::Error, "EngineSilhouette: supersampled mask: {}", reason);
            }
        }

        /**
         * @brief Draws the supersampled mask for the executing view, creating or resizing its targets first.
         * @return The mask texture, or 0 when nothing was drawn.
         */
        [[nodiscard]] std::uintptr_t
        draw_supersampled_mask(std::uintptr_t pipeline, std::uintptr_t stage, std::uintptr_t render_view) noexcept
        {
            DMK_PROFILE_FUNCTION();
            if (!s_mask_calls_ready.load(std::memory_order_acquire))
            {
                return 0;
            }
            const auto layout =
                DMK::memory::read<std::uintptr_t>(DMK::Address{stage + constants::CUSTOM_STAGE_RESOURCE_LAYOUT_OFFSET});
            const auto device_pass = DMK::memory::read<std::uintptr_t>(DMK::Address{
                stage + constants::CUSTOM_STAGE_MASK_PASS_OFFSET + constants::SCENE_PASS_DEVICE_PASS_OFFSET
            });
            if (!layout || *layout == 0 || !device_pass || *device_pass == 0)
            {
                return 0;
            }
            const std::uintptr_t output = guarded_output_target(stage);
            if (output == 0)
            {
                return 0;
            }
            const auto output_width =
                DMK::memory::read<std::uint16_t>(DMK::Address{output + constants::TEXTURE_WIDTH_OFFSET});
            const auto output_height =
                DMK::memory::read<std::uint16_t>(DMK::Address{output + constants::TEXTURE_HEIGHT_OFFSET});
            if (!output_width || !output_height || *output_width == 0 || *output_height == 0)
            {
                return 0;
            }
            const auto [width, height] = supersampled_mask_size(*output_width, *output_height);
            const bool with_temp = settings().softness.load(std::memory_order_relaxed) > 0.0f;
            if (!guarded_ensure_supersampled_targets(stage, width, height, with_temp))
            {
                reject_supersampled_mask("its render targets could not be created");
                return 0;
            }
            static std::int32_t s_reported_width = 0;
            static std::int32_t s_reported_height = 0;
            if (width != s_reported_width || height != s_reported_height)
            {
                s_reported_width = width;
                s_reported_height = height;
                const auto format =
                    DMK::memory::read<std::uint8_t>(DMK::Address{s_ss.mask + constants::TEXTURE_FORMAT_OFFSET});
                const auto depth_format =
                    DMK::memory::read<std::uint8_t>(DMK::Address{s_ss.depth + constants::TEXTURE_FORMAT_OFFSET});
                (void)DMK::log().try_log(
                    DMK::LogLevel::Info,
                    "EngineSilhouette: supersampled mask {}x{} for a {}x{} output (format {}, depth format {}){}",
                    width,
                    height,
                    *output_width,
                    *output_height,
                    format ? *format : 0,
                    depth_format ? *depth_format : 0,
                    with_temp ? ", with a blur intermediate" : ""
                );
            }
            const MaskStatus status = guarded_draw_supersampled_mask(pipeline, stage, render_view);
            if (status == MaskStatus::Fault)
            {
                reject_supersampled_mask("drawing into its render targets faulted");
                return 0;
            }
            static MaskStatus s_reported = MaskStatus::Drawn;
            if (status != s_reported && (status == MaskStatus::NoTexture || status == MaskStatus::Busy))
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Warning,
                    "EngineSilhouette[B1]: supersampled mask draw skipped ({})",
                    status == MaskStatus::NoTexture ? "a target has no device texture" : "a scene pass was still open"
                );
            }
            s_reported = status;
            if (status != MaskStatus::Drawn)
            {
                return 0;
            }
            s_p4_masks.fetch_add(1, std::memory_order_relaxed);
            s_ss_masks.fetch_add(1, std::memory_order_relaxed);
            return s_ss.mask;
        }

        /**
         * @brief The depth-tested @p mask as the visibility of the supersampled one: it was drawn with the view's
         *        camera jitter, which moved its content by minus the clip-space jitter, so it is sampled that much
         *        back (u - jx / 2, v + jy / 2 for this right-handed projection).
         */
        [[nodiscard]] MaskVisibility tested_visibility(std::uintptr_t render_view, std::uintptr_t mask) noexcept
        {
            const auto jx =
                DMK::memory::read<float>(DMK::Address{render_view + constants::RENDER_VIEW_SUBPIXEL_OFFSET});
            const auto jy =
                DMK::memory::read<float>(DMK::Address{render_view + constants::RENDER_VIEW_SUBPIXEL_OFFSET + 4});
            MaskVisibility visibility{.texture = mask};
            if (jx && jy && std::isfinite(*jx) && std::isfinite(*jy))
            {
                visibility.offset_u = -*jx * 0.5f;
                visibility.offset_v = *jy * 0.5f;
            }
            return visibility;
        }

        /**
         * @brief Composites the masks P4 drew at @p point.
         * @param pipeline The executing CStandardGraphicsPipeline.
         * @param stage Its published, initialized CSceneCustomStage.
         * @param frame The masks: the supersampled or render-resolution one, and the depth-tested visibility.
         * @param point Where the composite runs.
         */
        void composite_mask(
            std::uintptr_t pipeline,
            std::uintptr_t stage,
            const CompositeHandoff &frame,
            CompositePoint point
        ) noexcept
        {
            DMK_PROFILE_FUNCTION();
            const std::uintptr_t mask = frame.mask;
            if (mask == 0)
            {
                return;
            }
            const float intensity = s_intensity.load(std::memory_order_relaxed);
            if (!s_composite_calls_ready.load(std::memory_order_acquire) || intensity <= 0.0f)
            {
                return;
            }
            const LiveSettings &s = settings();
            const float strength = s.strength.load(std::memory_order_relaxed);
            // The shader draws an outline pixel as colour * (interior + edge * boost) * strength, so the interior
            // term is divided by the strength: the tint then lands at the fill opacity whatever the edge brightness.
            const float interior = s_interior_opacity.load(std::memory_order_relaxed);
            const float ps_params[4] = {
                strength * intensity,
                COMPOSITE_BOOST,
                COMPOSITE_BRIGHTNESS,
                strength > 0.0f ? interior / strength : 0.0f
            };
            const float vs_params[4] = {s.outline_width.load(std::memory_order_relaxed), 0.0f, 0.0f, 0.0f};
            // A soft mask turns the outline edge into a gradient: small details merge and sub-pixel camera motion no
            // longer flips edge pixels on and off.
            if (const float softness = s.softness.load(std::memory_order_relaxed); softness > 0.0f)
            {
                blur_mask(pipeline, stage, mask, frame.supersampled, softness);
            }
            std::uint32_t results = 0;
            std::uintptr_t destination = 0;
            // The mod's technique: no interlace or vignette and an even outline. The outline brightness keeps the
            // stock technique's edge boost.
            CustomComposite custom{};
            const CustomComposite *use_custom = nullptr;
            if (const std::uintptr_t shader = silhouette_shader(); shader != 0)
            {
                custom.shader = shader;
                custom.strength = strength * intensity * COMPOSITE_BOOST;
                custom.interior = interior * intensity;
                custom.outline_width = s.outline_width.load(std::memory_order_relaxed);
                use_custom = &custom;
            }
            CompositeStatus composite = guarded_composite(
                stage,
                mask,
                frame.visibility,
                ps_params,
                vs_params,
                point,
                use_custom,
                &results,
                &destination
            );
            if (use_custom != nullptr && !s_custom_confirmed)
            {
                if (composite == CompositeStatus::Executed && (results & 2u) != 0)
                {
                    s_custom_confirmed = true;
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Info,
                        "EngineSilhouette: the {} composite drew after {} attempt(s) and replaces the stock technique",
                        constants::SILHOUETTE_TECHNIQUE,
                        s_custom_failures + 1
                    );
                }
                else
                {
                    if (++s_custom_failures >= CUSTOM_WARMUP_ATTEMPTS)
                    {
                        reject_silhouette_shader("its composite never drew (the permutation did not compile)");
                    }
                    // Until it draws, the frame keeps the stock composite.
                    composite = guarded_composite(
                        stage,
                        mask,
                        frame.visibility,
                        ps_params,
                        vs_params,
                        point,
                        nullptr,
                        &results,
                        &destination
                    );
                }
            }
            if (composite == CompositeStatus::Executed)
            {
                s_p4_composites.fetch_add(1, std::memory_order_relaxed);
                s_diag = read_composite_diag(stage);
                static std::uint32_t s_reported_results = 0xFFFFFFFFu;
                static std::uintptr_t s_reported_mask = 0;
                static std::uintptr_t s_reported_destination = 0;
                static CompositeDiag s_reported_diag{0xFFFFFFFFu};
                if (results != s_reported_results || mask != s_reported_mask || destination != s_reported_destination ||
                    !(s_diag == s_reported_diag))
                {
                    s_reported_results = results;
                    s_reported_mask = mask;
                    s_reported_destination = destination;
                    s_reported_diag = s_diag;
                    (void)DMK::log().try_log(
                        s_diag.primitives != 0 && s_diag.device_pass_valid != 0 ? DMK::LogLevel::Info
                                                                                : DMK::LogLevel::Warning,
                        "EngineSilhouette[C1]: composite pass state: {} compiled primitive(s), device render pass "
                        "0x{:016X} "
                        "({}), render-pass scheduler enabled={} active={} ({})",
                        s_diag.primitives,
                        s_diag.device_pass,
                        s_diag.device_pass_valid != 0 ? "valid" : "INVALID - Execute draws nothing",
                        s_diag.scheduler_enabled,
                        s_diag.scheduler_active,
                        s_diag.scheduler_enabled != 0 && s_diag.scheduler_active != 0
                            ? "queued on the scheduler"
                            : "recorded on the core command list"
                    );
                    const auto width =
                        DMK::memory::read<std::uint16_t>(DMK::Address{destination + constants::TEXTURE_WIDTH_OFFSET});
                    const auto height =
                        DMK::memory::read<std::uint16_t>(DMK::Address{destination + constants::TEXTURE_HEIGHT_OFFSET});
                    // SetRenderTarget returns whether the binding changed, so "unchanged" after a rebind is normal.
                    (void)DMK::log().try_log(
                        (results & 2u) != 0 ? DMK::LogLevel::Info : DMK::LogLevel::Warning,
                        "EngineSilhouette[C1]: composite at {} mask 0x{:016X} -> destination 0x{:016X} ({}x{}), render "
                        "target {}, fullscreen Execute {}",
                        composite_point_name(point),
                        mask,
                        destination,
                        width ? *width : 0,
                        height ? *height : 0,
                        (results & 1u) != 0 ? "rebound" : "unchanged",
                        (results & 2u) != 0 ? "ok" : "FAILED"
                    );
                }
                return;
            }
            static CompositeStatus s_reported = CompositeStatus::Executed;
            if (composite != s_reported)
            {
                s_reported = composite;
                (void)DMK::log().try_log(
                    DMK::LogLevel::Warning,
                    "EngineSilhouette[C1]: composite skipped ({})",
                    composite == CompositeStatus::NoShader   ? "s_shPostEffectsGame not loaded"
                    : composite == CompositeStatus::NoTarget ? "no $DisplayTargetDst"
                    : composite == CompositeStatus::NoNames  ? "constant names unavailable"
                                                             : "fault"
                );
            }
            if (composite == CompositeStatus::Fault)
            {
                s_render_faulted.store(true, std::memory_order_release);
            }
        }

        /**
         * @brief P4: draws the executing view's masks after the HDR chain and either composites them here or hands
         *        them to P5, which composites them after the LDR chain.
         * @details Every composite runs after tone-mapping, so a highlight's colour does not depend on the exposure or
         *          on which other groups show. The upscaler does not anti-alias it there, so the silhouettes are drawn
         *          into the supersampled mask, without the camera jitter (stable edges from a still camera). With
         *          SeeThrough = false that mask holds whole silhouettes and the depth-tested mask (drawn by P6 before
         *          the upscaler, or here without one, where the camera matches the scene depth) says which parts are
         *          visible. When the supersampled mask is unavailable the render-resolution mask is used: drawn without
         *          the jitter, or the depth-tested one. The focus coverage is drawn here, before the post-effect stage
         *          that applies it.
         */
        void draw_frame(std::uintptr_t pipeline, std::uintptr_t stage, std::uintptr_t render_view) noexcept
        {
            const bool see_through = settings().see_through.load(std::memory_order_relaxed);
            const std::uintptr_t handed = s_tested_mask_view == render_view ? s_tested_mask_texture : 0;
            clear_view_handoffs();

            std::uintptr_t tested = 0;
            if (!see_through)
            {
                tested = handed != 0 ? handed : draw_mask(pipeline, stage, render_view, false);
                if (tested == 0)
                {
                    render_focus_coverage(stage, 0, {});
                    return;
                }
            }
            CompositeHandoff frame{.view = render_view};
            if (supersampled_mask_ready())
            {
                frame.mask = draw_supersampled_mask(pipeline, stage, render_view);
                frame.supersampled = frame.mask != 0;
                if (frame.supersampled && !see_through)
                {
                    frame.visibility = tested_visibility(render_view, tested);
                }
            }
            if (frame.mask == 0)
            {
                frame.mask = see_through ? draw_mask(pipeline, stage, render_view, true) : tested;
            }
            if (frame.mask == 0)
            {
                render_focus_coverage(stage, 0, {});
                return;
            }
            if (s_p5_installed.load(std::memory_order_acquire))
            {
                render_focus_coverage(stage, frame.mask, frame.visibility);
                s_handoff = frame;
                return;
            }
            // The render-resolution mask's blur goes through $SceneDiffuseTmp, so the coverage is drawn after it.
            composite_mask(pipeline, stage, frame, CompositePoint::AfterHdr);
            render_focus_coverage(stage, frame.mask, frame.visibility);
        }

        /**
         * @brief P4: post-call hook on CSceneForwardStage::ExecuteAfterPostProcessHDR.
         * @details This is where the engine's own editor highlight composites into $DisplayTargetDst, so composited
         *          pixels flow through PostToFrame and every LDR post effect (fades hide them as intended).
         */
        void __fastcall detour_after_post_hdr(void *forward_stage) noexcept
        {
            const InFlightScope scope;
            s_after_post_hdr_original(forward_stage);
            if (s_armed.load(std::memory_order_acquire))
            {
                const P4BodyScope body;
                after_post_hdr(reinterpret_cast<std::uintptr_t>(forward_stage));
            }
        }

        /**
         * @brief P5 body: composites the masks P4 drew for this view after the LDR post chain, into the render output.
         * @details Publishing and unpublishing stay with P4, earlier in the same Execute; P5 only composites for a
         *          stage P4 left published, so a stage unpublished this frame is already 0 here.
         * @param forward_stage The CSceneForwardStage whose after-LDR pass just returned.
         */
        void after_post_ldr(std::uintptr_t forward_stage) noexcept
        {
            DMK_PROFILE_FUNCTION();
            if (s_render_faulted.load(std::memory_order_relaxed) ||
                s_unpublish_requested.load(std::memory_order_seq_cst))
            {
                return;
            }
            if (!render_work_pending())
            {
                clear_view_handoffs();
                return;
            }
            const auto pipeline_read =
                DMK::memory::read<std::uintptr_t>(DMK::Address{forward_stage + constants::STAGE_PIPELINE_OFFSET});
            if (!pipeline_read || *pipeline_read == 0)
            {
                return;
            }
            const std::uintptr_t pipeline = *pipeline_read;
            const auto forward =
                DMK::memory::read<std::uintptr_t>(DMK::Address{pipeline + constants::PIPELINE_FORWARD_STAGE_OFFSET});
            if (!forward || *forward != forward_stage || !object_is(GameClass::StdPipeline, pipeline))
            {
                return;
            }
            const auto stage_read =
                DMK::memory::read<std::uintptr_t>(DMK::Address{pipeline + constants::PIPELINE_CUSTOM_STAGE_OFFSET});
            if (!stage_read || *stage_read == 0 || !object_is(GameClass::CustomStage, *stage_read))
            {
                return;
            }
            const std::uintptr_t stage = *stage_read;
            const auto view_read =
                DMK::memory::read<std::uintptr_t>(DMK::Address{stage + constants::STAGE_RENDER_VIEW_OFFSET});
            const std::uintptr_t render_view = view_read ? *view_read : 0;
            const auto current_view =
                DMK::memory::read<std::uintptr_t>(DMK::Address{pipeline + constants::PIPELINE_CURRENT_VIEW_OFFSET});
            if (render_view == 0 || !current_view || *current_view != render_view)
            {
                return;
            }
            const CompositeHandoff frame = s_handoff;
            s_handoff = {};
            if (frame.view != render_view || frame.mask == 0)
            {
                return;
            }
            s_p5_frames.fetch_add(1, std::memory_order_relaxed);
            composite_mask(pipeline, stage, frame, CompositePoint::AfterLdr);
        }

        /**
         * @brief P6 body: draws the depth-tested mask (SeeThrough = false) before the upscaler and hands it to P4.
         * @details Only here does the camera, jitter included, match the scene depth the mask is tested against; after
         *          the upscaler an angled surface fails the depth test as a whole on some frames. Nothing is
         *          composited here: a composite into the HDR scene would go through the exposure and the tone curve.
         *          Publishing stays with P4, later in the same Execute: P6 only draws with a stage P4 left published
         *          for the view being executed.
         * @param super_resolution The CSuperResolutionStage about to run.
         */
        void before_upscale(std::uintptr_t super_resolution) noexcept
        {
            DMK_PROFILE_FUNCTION();
            if (settings().see_through.load(std::memory_order_relaxed) ||
                s_render_faulted.load(std::memory_order_relaxed) ||
                s_unpublish_requested.load(std::memory_order_seq_cst))
            {
                return;
            }
            const auto pipeline_read =
                DMK::memory::read<std::uintptr_t>(DMK::Address{super_resolution + constants::STAGE_PIPELINE_OFFSET});
            if (!pipeline_read || *pipeline_read == 0 || !object_is(GameClass::StdPipeline, *pipeline_read))
            {
                return;
            }
            const std::uintptr_t pipeline = *pipeline_read;
            const auto stage_read =
                DMK::memory::read<std::uintptr_t>(DMK::Address{pipeline + constants::PIPELINE_CUSTOM_STAGE_OFFSET});
            if (!stage_read || *stage_read == 0 || !object_is(GameClass::CustomStage, *stage_read))
            {
                return;
            }
            const std::uintptr_t stage = *stage_read;
            const auto view_read =
                DMK::memory::read<std::uintptr_t>(DMK::Address{stage + constants::STAGE_RENDER_VIEW_OFFSET});
            const std::uintptr_t render_view = view_read ? *view_read : 0;
            const auto current_view =
                DMK::memory::read<std::uintptr_t>(DMK::Address{pipeline + constants::PIPELINE_CURRENT_VIEW_OFFSET});
            if (render_view == 0 || !current_view || *current_view != render_view)
            {
                return;
            }
            s_p6_frames.fetch_add(1, std::memory_order_relaxed);
            const std::uintptr_t mask = draw_mask(pipeline, stage, render_view, false);
            s_tested_mask_texture = mask;
            s_tested_mask_view = mask != 0 ? render_view : 0;
        }

        /**
         * @brief P6: pre-call hook on CSuperResolutionStage::Execute.
         */
        void __fastcall detour_super_resolution(void *stage) noexcept
        {
            const InFlightScope scope;
            // Nothing to draw: straight to the original (P4 and P5 then find no hand-off).
            if (s_armed.load(std::memory_order_acquire) && render_work_pending())
            {
                before_upscale(reinterpret_cast<std::uintptr_t>(stage));
            }
            s_super_resolution_original(stage);
        }

        /**
         * @brief P5: post-call hook on CSceneForwardStage::ExecuteAfterPostProcessLDR.
         */
        void __fastcall detour_after_post_ldr(void *forward_stage) noexcept
        {
            const InFlightScope scope;
            s_after_post_ldr_original(forward_stage);
            if (s_armed.load(std::memory_order_acquire))
            {
                after_post_ldr(reinterpret_cast<std::uintptr_t>(forward_stage));
            }
        }

        /**
         * @brief Installs one inline hook, stores it, and arms it.
         * @tparam Detour Detour function type.
         * @tparam Original Trampoline pointer type.
         * @param hooks The set; it owns the hook before the arm, so a half-armed hook is still torn down.
         * @param name Hook name.
         * @param target Target address.
         * @param detour The detour.
         * @param original Receives the trampoline before the hook is armed.
         * @return An empty value, or the install Error.
         */
        template <typename Detour, typename Original>
        [[nodiscard]] DMK::Result<void>
        install_hook(HookSet &hooks, const char *name, std::uintptr_t target, Detour *detour, Original &original)
        {
            DMK_TRY(
                created,
                DMK::hook::inline_at(
                    DMK::hook::InlineRequest{
                        .name = name,
                        .target = DMK::Address{target},
                    },
                    detour
                )
            );
            original = created.template original<Original>();
            DMK::hook::Hook &stored = hooks.push(std::move(created));
            DMK_TRY_VOID(stored.enable());
            DMK::log().info(
                "EngineSilhouette: hook {} armed at {} (RVA {:#x})",
                name,
                DMK::format::format_address(target),
                target - module_info().base
            );
            return {};
        }
    } // namespace

    void bind_engine_stage_stash(std::span<std::uint64_t> slots) noexcept
    {
        bind_pso_depth_table(slots);
        if (slots.size() < STASH_SLOT_COUNT)
        {
            s_stash_slots = {};
            return;
        }
        s_stash_slots = slots;
        const std::uint64_t tag = stash_word(0).load(std::memory_order_acquire);
        if (tag != STASH_LAYOUT_TAG)
        {
            // Another layout (or none yet): its contents cannot be read as stash entries, so they are dropped.
            bool dropped = false;
            for (std::size_t i = 1; i < STASH_SLOT_COUNT; ++i)
            {
                dropped = stash_word(i).exchange(0, std::memory_order_acq_rel) != 0 || dropped;
            }
            stash_word(0).store(STASH_LAYOUT_TAG, std::memory_order_release);
            if (tag != 0 || dropped)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Warning,
                    "EngineSilhouette: the persistent slots carried layout {:#x}, not {:#x}; their "
                    "contents were dropped",
                    tag,
                    STASH_LAYOUT_TAG
                );
            }
            return;
        }
        std::size_t waiting = 0;
        for (std::size_t entry = 0; entry < STASH_ENTRIES; ++entry)
        {
            const StashEntry stashed = stash_read(entry);
            if (stashed.pipeline == 0)
            {
                continue;
            }
            ++waiting;
            (void)DMK::log().try_log(
                DMK::LogLevel::Info,
                "EngineSilhouette: the previous generation left stage 0x{:016X} unpublished for pipeline "
                "0x{:016X} (forward stage 0x{:016X}); P4 adopts it on the first frame of that pipeline",
                stashed.stage,
                stashed.pipeline,
                stashed.forward
            );
        }
        if (waiting == 0)
        {
            (void)DMK::log().try_log(
                DMK::LogLevel::Info,
                "EngineSilhouette: persistent slots bound; no stage is waiting for adoption"
            );
        }
    }

    DMK::Result<void> install_engine_silhouette_early(HookSet &hooks)
    {
        // A refused unload re-runs init on the same image, whose statics still hold the previous teardown. The P1
        // list stays: it records pipeline Inits this image really saw. The installed flags are cleared because that
        // teardown destroyed the hooks they describe, and P4 must never take a stale P2 flag as a guard.
        s_p1_installed.store(false, std::memory_order_release);
        s_p2_installed.store(false, std::memory_order_release);
        s_p3_installed.store(false, std::memory_order_release);
        s_p3b_installed.store(false, std::memory_order_release);
        s_p4_installed.store(false, std::memory_order_release);
        s_p5_installed.store(false, std::memory_order_release);
        s_p6_installed.store(false, std::memory_order_release);
        s_p7_installed.store(false, std::memory_order_release);
        s_p7_faulted.store(false, std::memory_order_release);
        s_armed.store(true, std::memory_order_release);
        s_unpublish_requested.store(false, std::memory_order_release);
        for (TrackedPipeline &entry : s_tracked)
        {
            reset_tracked(entry);
        }

        if (!feature_ready(Feature::StageRegistration))
        {
            DMK::log().warning("EngineSilhouette: the stage registration gate failed; the stage cannot be registered");
            return std::unexpected(DMK::Error{DMK::ErrorCode::NoMatch, "engine_silhouette/stage_gate"});
        }
        const std::uintptr_t init = anchor_address(AnchorId::StdPipelineInit);
        const std::uintptr_t create_pso = anchor_address(AnchorId::CustomCreatePso);
        // P2 first: a stage must never exist without the debug-pass guard in front of it.
        if (auto p2 = install_hook(
                hooks,
                "CustomCreatePipelineState",
                create_pso,
                detour_custom_create_pso,
                s_create_pso_original
            );
            !p2.has_value())
        {
            return p2;
        }
        s_p2_installed.store(true, std::memory_order_release);
        if (auto p1 = install_hook(hooks, "StdPipelineInit", init, detour_std_pipeline_init, s_std_init_original);
            !p1.has_value())
        {
            return p1;
        }
        s_p1_installed.store(true, std::memory_order_release);
        DMK::log().info(
            "EngineSilhouette: P1/P2 armed (pipeline Init calls so far: {}); stage registration is {} (Backend={})",
            s_p1_calls.load(std::memory_order_relaxed),
            stage_registration_wanted() ? "ON" : "off",
            settings().backend.load(std::memory_order_relaxed)
        );
        return {};
    }

    DMK::Result<void> initialize_engine_silhouette(HookSet &hooks)
    {
        DMK::Logger &logger = DMK::log();

        s_calls.register_stage = anchor_address(AnchorId::RegisterCustomStage);
        s_calls.clear_surface = anchor_address(AnchorId::ClearSurface);
        s_calls.prepare_render_pass = anchor_address(AnchorId::PrepareRenderPass);
        s_calls.draw_render_items = anchor_address(AnchorId::DrawRenderItems);
        s_calls.jobify = anchor_address(AnchorId::JobifyDraws);
        s_calls.wait = anchor_address(AnchorId::WaitDraws);
        s_calls.set_technique = anchor_address(AnchorId::SetTechnique);
        s_calls.set_render_target = anchor_address(AnchorId::SetRenderTarget);
        s_calls.set_texture = anchor_address(AnchorId::SetTexture);
        s_calls.set_sampler = anchor_address(AnchorId::SetSampler);
        s_calls.begin_constant_update = anchor_address(AnchorId::BeginConstantUpdate);
        s_calls.set_constant = anchor_address(AnchorId::SetConstant);
        s_calls.fullscreen_execute = anchor_address(AnchorId::FullscreenExecute);
        s_calls.cry_name_r = anchor_address(AnchorId::CryNameR);
        s_calls.display_target = anchor_address(AnchorId::DisplayTargetDst);
        s_calls.ldr_target = gated_anchor_address(Feature::AfterLdr, AnchorId::LdrTarget);
        s_calls.core_command_list_slot = anchor_address(AnchorId::CoreCommandListSlot);
        s_calls.recursion_counter = anchor_address(AnchorId::RecursionCounter);
        s_calls.post_effects_game_slot = anchor_address(AnchorId::PostEffectsGameSlot);
        s_calls.scene_set_render_targets =
            gated_anchor_address(Feature::SupersampledMask, AnchorId::SceneSetRenderTargets);
        s_calls.post_fx_render_target = gated_anchor_address(Feature::SupersampledMask, AnchorId::PostFxRenderTarget);
        s_calls.post_fx_depth_stencil = gated_anchor_address(Feature::SupersampledMask, AnchorId::PostFxDepthStencil);
        s_calls.clear_depth = gated_anchor_address(Feature::SupersampledMask, AnchorId::ClearDepth);

        const bool mask_ready = feature_ready(Feature::StageRegistration) && feature_ready(Feature::MaskDraw);
        const bool composite_ready = mask_ready && feature_ready(Feature::Composite);
        s_mask_calls_ready.store(mask_ready, std::memory_order_release);
        s_composite_calls_ready.store(composite_ready, std::memory_order_release);

        // A refused unload re-runs init on the same image: the previous teardown released the targets.
        s_ss = {};
        s_ss_faulted.store(false, std::memory_order_relaxed);
        const bool supersampled_ready = composite_ready && feature_ready(Feature::SupersampledMask);
        if (supersampled_ready)
        {
            // A per-load token in the names: a reloaded generation never asks for a texture its predecessor released.
            LARGE_INTEGER counter{};
            QueryPerformanceCounter(&counter);
            const auto token = static_cast<std::uint32_t>(counter.QuadPart ^ (counter.QuadPart >> 32));
            const auto name = [token](std::array<char, 48> &out, const char *role)
            {
                const auto written =
                    std::format_to_n(out.data(), out.size() - 1, "$HenrySensesMask{}_{:08X}", role, token);
                *written.out = '\0';
            };
            name(s_ss_mask_name, "");
            name(s_ss_depth_name, "Depth");
            name(s_ss_temp_name, "Tmp");
        }
        s_ss_calls_ready.store(supersampled_ready, std::memory_order_release);
        logger.info(
            "EngineSilhouette: mask draw calls {}, composite calls {}, supersampled mask {} (technique key {:#x})",
            mask_ready ? "ready" : "MISSING",
            composite_ready ? "ready" : "MISSING",
            supersampled_ready ? "ready" : "unavailable (outlines use the render-resolution mask)",
            s_technique_key
        );
        logger.info(
            "EngineSilhouette: vtables CStandardGraphicsPipeline={} CSceneCustomStage={} "
            "CSceneForwardStage={} CRenderProxy={}",
            DMK::format::format_address(class_vtable(GameClass::StdPipeline)),
            DMK::format::format_address(class_vtable(GameClass::CustomStage)),
            DMK::format::format_address(class_vtable(GameClass::ForwardStage)),
            DMK::format::format_address(class_vtable(GameClass::RenderProxy))
        );

        const std::uintptr_t proxy_render = gated_anchor_address(Feature::ProxyInjection, AnchorId::ProxyRender);
        if (proxy_render == 0)
        {
            logger.warning("EngineSilhouette: the proxy injection gate failed; words cannot be injected");
            return std::unexpected(DMK::Error{DMK::ErrorCode::NoMatch, "engine_silhouette/proxy_gate"});
        }
        if (auto p3 = install_hook(hooks, "ProxyRender", proxy_render, detour_proxy_render, s_proxy_render_original);
            !p3.has_value())
        {
            return p3;
        }
        s_p3_installed.store(true, std::memory_order_release);

        // P3b is optional: without it brushes are drawn as markers instead of outlined.
        if (const std::uintptr_t render_internal =
                gated_anchor_address(Feature::BrushMarking, AnchorId::StatObjRenderInternal);
            render_internal == 0)
        {
            logger.warning("EngineSilhouette: the brush marking gate failed; world objects fall back to markers");
        }
        else if (auto p3b = install_hook(
                     hooks,
                     "StatObjRenderInternal",
                     render_internal,
                     detour_render_internal,
                     s_render_internal_original
                 );
                 !p3b.has_value())
        {
            logger.warning(
                "EngineSilhouette: the CStatObj::RenderInternal hook failed; world objects fall back to markers"
            );
        }
        else
        {
            s_p3b_installed.store(true, std::memory_order_release);
        }

        // P7 is optional: without it the herb silhouette draws also write the depth pre-pass.
        if (!feature_ready(Feature::MaskDraw) || !feature_ready(Feature::HerbOutline) || s_calls.draw_render_items == 0)
        {
            logger.warning(
                "EngineSilhouette: the depth pre-pass filter is off (mask draw or herb outline gate failed)"
            );
        }
        else if (auto p7 = install_hook(
                     hooks,
                     "DrawRenderItems",
                     s_calls.draw_render_items,
                     detour_draw_render_items,
                     s_draw_render_items_original
                 );
                 !p7.has_value())
        {
            logger.warning(
                "EngineSilhouette: the DrawRenderItems hook failed; herb silhouette draws also write the depth "
                "pre-pass"
            );
        }
        else
        {
            s_p7_installed.store(true, std::memory_order_release);
        }

        const std::uintptr_t after_post_hdr = gated_anchor_address(Feature::AfterHdr, AnchorId::AfterPostHdr);
        if (after_post_hdr == 0)
        {
            logger.warning("EngineSilhouette: the after-HDR gate failed; no mask draw");
            return std::unexpected(DMK::Error{DMK::ErrorCode::NoMatch, "engine_silhouette/after_hdr_gate"});
        }
        if (auto p4 = install_hook(
                hooks,
                "AfterPostProcessHDR",
                after_post_hdr,
                detour_after_post_hdr,
                s_after_post_hdr_original
            );
            !p4.has_value())
        {
            return p4;
        }
        s_p4_installed.store(true, std::memory_order_release);

        // P6 is optional: without it a depth-tested mask (SeeThrough = false) is drawn after the upscaler, where an
        // angled surface can fail the depth test on some frames.
        if (const std::uintptr_t super_resolution =
                gated_anchor_address(Feature::BeforeUpscale, AnchorId::SuperResolutionExecute);
            super_resolution != 0)
        {
            if (auto p6 = install_hook(
                    hooks,
                    "SuperResolutionExecute",
                    super_resolution,
                    detour_super_resolution,
                    s_super_resolution_original
                );
                p6.has_value())
            {
                s_p6_installed.store(true, std::memory_order_release);
            }
            else
            {
                logger.warning(
                    "EngineSilhouette: the super-resolution hook failed; with an upscaler a depth-tested mask is "
                    "drawn after it"
                );
            }
        }

        // P5 is optional: without it the HDR-point composite (P4) stays available.
        const std::uintptr_t after_post_ldr_address = gated_anchor_address(Feature::AfterLdr, AnchorId::AfterPostLdr);
        if (after_post_ldr_address == 0)
        {
            logger.warning(
                "EngineSilhouette: the after-LDR gate failed; the silhouette composites after the HDR chain only"
            );
            return {};
        }
        if (auto p5 = install_hook(
                hooks,
                "AfterPostProcessLDR",
                after_post_ldr_address,
                detour_after_post_ldr,
                s_after_post_ldr_original
            );
            !p5.has_value())
        {
            logger.warning(
                "EngineSilhouette: the after-LDR hook failed; the silhouette composites after the HDR "
                "chain only"
            );
            return {};
        }
        s_p5_installed.store(true, std::memory_order_release);
        return {};
    }

    std::size_t unpublish_engine_stage(std::chrono::milliseconds timeout) noexcept
    {
        const std::int64_t requested_ms = now_ms();
        const std::int64_t deadline = requested_ms + timeout.count();
        s_unpublish_requested.store(true, std::memory_order_seq_cst);

        // A P4 body that read the flag before the store above may still publish a stage (adoption, late attach), so
        // it is let out first. Every later body sees the request.
        while (s_p4_bodies.load(std::memory_order_seq_cst) != 0 && now_ms() < deadline)
        {
            Sleep(1);
        }

        // Every tracked pipeline that still holds a stage is waited for, however long ago it last rendered: only P4
        // may clear the slot, and P2 must guard the stage until it does.
        const auto pending = []() noexcept
        {
            std::size_t count = 0;
            for (const TrackedPipeline &entry : s_tracked)
            {
                count += stage_left_published(entry) != 0 ? 1 : 0;
            }
            return count;
        };
        if (s_p4_installed.load(std::memory_order_acquire))
        {
            while (pending() != 0 && now_ms() < deadline)
            {
                Sleep(5);
            }
        }

        std::size_t kept = 0;
        for (const TrackedPipeline &entry : s_tracked)
        {
            const std::uintptr_t stage = stage_left_published(entry);
            if (stage == 0)
            {
                continue;
            }
            ++kept;
            (void)DMK::log().try_log(
                DMK::LogLevel::Warning,
                "EngineSilhouette: pipeline 0x{:016X} rendered no frame within {} ms, so stage 0x{:016X} stays "
                "published; unload must retain P2 until the render thread removes the stage",
                entry.pipeline.load(std::memory_order_acquire),
                timeout.count(),
                stage
            );
        }
        for (std::size_t i = 0; i < TRACKED_PIPELINES; ++i)
        {
            const std::uintptr_t pipeline = s_p1_pipelines[i].load(std::memory_order_acquire);
            const std::uintptr_t stage = s_p1_stages[i].load(std::memory_order_acquire);
            if (pipeline == 0 || stage == 0)
            {
                continue;
            }
            bool tracked = false;
            for (const TrackedPipeline &entry : s_tracked)
            {
                tracked = tracked || entry.pipeline.load(std::memory_order_acquire) == pipeline;
            }
            if (tracked || !object_is(GameClass::StdPipeline, pipeline))
            {
                continue;
            }
            const auto slot =
                DMK::memory::read<std::uintptr_t>(DMK::Address{pipeline + constants::PIPELINE_CUSTOM_STAGE_OFFSET});
            if (!slot || *slot != stage)
            {
                continue;
            }
            ++kept;
            (void)DMK::log().try_log(
                DMK::LogLevel::Warning,
                "EngineSilhouette: P4 never executed pipeline 0x{:016X}, so stage 0x{:016X} that P1 "
                "registered on it stays published while that pipeline exists",
                pipeline,
                stage
            );
        }
        (void)DMK::log().try_log(
            kept == 0 ? DMK::LogLevel::Info : DMK::LogLevel::Warning,
            "EngineSilhouette: unpublish done in {} ms: {} stage(s) unpublished on the render "
            "thread, {} kept published",
            now_ms() - requested_ms,
            s_unpublished.load(std::memory_order_relaxed),
            kept
        );
        return kept;
    }

    void disarm_engine_silhouette() noexcept
    {
        s_armed.store(false, std::memory_order_release);
    }

    bool shutdown_engine_silhouette() noexcept
    {
        // Inline hooks have no rundown of their own: a thread can still be inside a detour (inside the original)
        // when the prologue is restored, and must leave before the trampolines are freed and this module unmaps.
        const std::int64_t deadline = now_ms() + RUNDOWN_TIMEOUT_MS;
        for (;;)
        {
            while (!detours_idle() && now_ms() < deadline)
            {
                Sleep(5);
            }
            Sleep(ENTRY_WINDOW_MS);
            if (detours_idle())
            {
                release_blur_pass();
                release_supersampled_targets();
                return true;
            }
            if (now_ms() >= deadline)
            {
                (void)DMK::log().log_noexcept(
                    DMK::LogLevel::Error,
                    "EngineSilhouette: a render-path detour was still running at shutdown"
                );
                return false;
            }
        }
    }

    bool engine_silhouette_available() noexcept
    {
        // Without every mask and composite call the words would move nodes and suppress the markers while nothing
        // can be drawn.
        if (s_render_faulted.load(std::memory_order_relaxed) || !s_p3_installed.load(std::memory_order_relaxed) ||
            !s_p4_installed.load(std::memory_order_relaxed) || !s_mask_calls_ready.load(std::memory_order_relaxed) ||
            !s_composite_calls_ready.load(std::memory_order_relaxed))
        {
            return false;
        }
        const std::int64_t seen = s_stage_seen_ms.load(std::memory_order_relaxed);
        return seen != 0 && now_ms() - seen < STAGE_LIVE_WINDOW_MS;
    }

    bool engine_brush_outline_available() noexcept
    {
        return s_p3b_installed.load(std::memory_order_relaxed) && engine_silhouette_available();
    }

    bool stage_registration_wanted() noexcept
    {
        const std::uint32_t backend = settings().backend.load(std::memory_order_relaxed);
        return backend == static_cast<std::uint32_t>(RenderBackend::Engine) ||
               backend == static_cast<std::uint32_t>(RenderBackend::Auto);
    }

    std::uint64_t silhouette_object_flags() noexcept
    {
        return settings().see_through.load(std::memory_order_relaxed)
                   ? constants::FOB_HENRYSENSES_MARKER
                   : constants::FOB_HENRYSENSES_MARKER | constants::FOB_HUD_REQUIRE_DEPTHTEST;
    }

    void set_focus_coverage_wanted(bool wanted) noexcept
    {
        s_focus_wanted.store(wanted, std::memory_order_relaxed);
    }

    std::string focus_coverage_texture_name()
    {
        const std::uintptr_t stage = s_last_stage.load(std::memory_order_relaxed);
        if (stage == 0 || !engine_silhouette_available() || s_focus_faulted.load(std::memory_order_relaxed))
        {
            return {};
        }
        constexpr std::array<std::ptrdiff_t, 4> name_chain{
            constants::STAGE_RESOURCES_OFFSET,
            constants::RESOURCES_SCENE_DIFFUSE_TMP_OFFSET,
            constants::TEXTURE_NAME_OFFSET,
            0
        };
        const auto name = DMK::memory::walk(DMK::Address{stage}, name_chain);
        return name ? read_c_string(name->raw(), 128) : std::string{};
    }

    std::uint64_t focus_coverage_frames() noexcept
    {
        return s_focus_frames.load(std::memory_order_relaxed);
    }

    void publish_silhouette_params(const SilhouetteFrameParams &params) noexcept
    {
        // Main thread, every tick: the first call writes and requests the mod's composite shader.
        load_silhouette_shader();
        s_intensity.store(params.intensity, std::memory_order_relaxed);
        s_interior_opacity.store(params.interior_opacity, std::memory_order_relaxed);
    }

    void log_engine_silhouette_state()
    {
        DMK::Logger &logger = DMK::log();
        const std::uintptr_t pipeline = s_last_pipeline.load(std::memory_order_relaxed);
        const std::uintptr_t stage = s_last_stage.load(std::memory_order_relaxed);
        logger.info(
            "EngineSilhouette: hooks P1={} P2={} P3={} P4={} armed={} faulted={} available={} "
            "registrationWanted={}",
            s_p1_installed.load(),
            s_p2_installed.load(),
            s_p3_installed.load(),
            s_p4_installed.load(),
            s_armed.load(),
            s_render_faulted.load(),
            engine_silhouette_available(),
            stage_registration_wanted()
        );
        logger.info(
            "EngineSilhouette: pipeline={} stage={} P1 calls={} P1 registered={} late attached={} "
            "(late attach failed={}) adopted={} unpublished={} stash={}",
            DMK::format::format_address(pipeline),
            DMK::format::format_address(stage),
            s_p1_calls.load(),
            s_p1_registered.load(),
            s_late_attached.load(),
            s_late_attach_failed.load(),
            s_adopted.load(),
            s_unpublished.load(),
            stash_available() ? "bound" : "none"
        );
        for (std::size_t entry = 0; stash_available() && entry < STASH_ENTRIES; ++entry)
        {
            const StashEntry stashed = stash_read(entry);
            if (stashed.pipeline != 0)
            {
                logger.info(
                    "EngineSilhouette: stash entry {}: pipeline={} stage={} forward={}",
                    entry,
                    DMK::format::format_address(stashed.pipeline),
                    DMK::format::format_address(stashed.stage),
                    DMK::format::format_address(stashed.forward)
                );
            }
        }
        if (pipeline != 0)
        {
            const auto slot =
                DMK::memory::read<std::uintptr_t>(DMK::Address{pipeline + constants::PIPELINE_CUSTOM_STAGE_OFFSET});
            logger.info(
                "EngineSilhouette: live P+0x50={} (CSceneCustomStage vtable {})",
                DMK::format::format_address(slot ? *slot : 0),
                slot && *slot != 0 && object_is(GameClass::CustomStage, *slot) ? "ok" : "-"
            );
        }
        logger.info(
            "EngineSilhouette: P2 debug suppressed={} pass4 ok={} failed={} overridden={} uninitialized={} "
            "gated={} (unmarked objectFlags seen {:#018x}, marker {:#018x}); "
            "P3 injected={}; P4 frames={} stageFrames={} itemFrames={} masks={} composites={} lastItems={} "
            "lastFlags={:#x} pendingChunks={}; intensity={:.2f} interior={:.2f}",
            s_p2_debug_suppressed.load(),
            s_p2_pass4_ok.load(),
            s_p2_pass4_failed.load(),
            s_p2_pass4_overridden.load(),
            s_p2_pass4_uninitialized.load(),
            s_p2_pass4_gated.load(),
            s_p2_seen_flags.load(),
            constants::FOB_HENRYSENSES_MARKER,
            s_p3_injected.load(),
            s_p4_frames.load(),
            s_p4_stage_frames.load(),
            s_p4_item_frames.load(),
            s_p4_masks.load(),
            s_p4_composites.load(),
            s_p4_last_items.load(),
            s_p4_last_flags.load(),
            s_p4_last_pending.load(),
            s_intensity.load(),
            s_interior_opacity.load()
        );
    }

} // namespace HenrySenses
