/**
 * @file hooks/shader_twins.cpp
 * @brief Implementation of the shader twins (see shader_twins.hpp).
 *
 * @details Three kinds of threads meet here. The camera frame on the main thread owns the swapped nodes, the twin
 *          shaders and the window, and publishes two small tables through triple buffers and atomic pointers, like the
 *          occluder fade's FadeSet: the swap set (the nodes and each source instance's twin) and the window rows. A
 *          source is an instance of a stock shader the generated header lists in k_twins (Vegetation, Illum, Eye,
 *          Hair), and its twin an instance of that kind's twin. The render jobs run CRenderView::AddRenderObject and
 *          only read the swap set. An item without a twin contributes an owned, bounded candidate for the next
 *          loading screen. Material loads contribute the same candidates, and at each boundary the level inventory
 *          (hooks/twin_inventory.hpp) adds every plant and frame pair the level's own tables hold, so the boundary
 *          covers the level whatever it loaded. Only a verified main-thread loading boundary creates twins, with
 *          render-command barriers before and after their creation, and, for the plant and frame variants that came
 *          after it (and the character's drawn items), a main-thread frame in which the game clock stands still (under
 *          the Esc menu, a UI overlay or neither), one twin at a time, each finished by a barrier inside that frame
 *          (service_shader_twins_paused()); unpaused gameplay, dialogue included, never creates one. A ready twin the
 *          engine reloads in place (a shader-quality change) keeps its items on the stock shader until it checks out
 *          against its source again. The render thread fills the per-view constant buffer and copies the window rows
 *          into the main view's buffer right before its upload. A table is reused two frames after it was replaced,
 *          and every reader finishes inside its frame, so no reader sees one being rewritten.
 *
 *          The twin's permutations compile the first time a swapped tree asks for them, which can take seconds, and
 *          until then the PSOs built for them are invalid and the engine skips those passes: the tree would vanish
 *          from the main view. So every compiled object that names a twin builds its PSOs twice in
 *          CreatePipelineStates, the stock set into the object and the twin set aside, and takes the twin set only
 *          once every pass the stock set draws has a valid twin PSO. Until then the tree draws exactly as stock, and
 *          the render thread hands both sets (with a reference held on each PSO) to the main thread, which watches
 *          their valid bytes and refills the tree once the twin's are ready; the refill builds them again from the
 *          engine's caches and keeps the twin. The main thread only reads those bytes and drops the references it
 *          holds, never the last one, so it calls no engine code.
 *
 *          The character fade's twins ride the same tables. A k_twins entry with TwinScope::Dissolving (Eye, Hair, and
 *          IllumFade from the Illum shader) has no tree listing: its twin replaces the stock shader on the main-view
 *          items of the temporary render objects that name one of the character's render nodes (his own render proxy
 *          and his carried items', which the swap set lists), his head, hair and beard, for as long as the close-up
 *          fade's twins run (third person), so the set is published every frame they run, with or without listed
 *          trees. A kind is a source name and a scope, so one Illum instance can have a plant twin, a frame twin and
 *          an IllumFade twin; the IllumFade twin is taken only by an item whose opacity is below 1 (the engine's own
 *          test for the transparent list), the eye film, and every opaque Illum item keeps the stock shader. A bare
 *          frame the camera lists is tracked like a tree, and its items take a twin of TwinScope::ListedFrames.
 *
 *          The twin's pixel shaders read the dissolve without the engine's RT gate, so at no dissolve they draw exactly
 *          like the stock ones, and the items name the twin whether or not he dissolves: that is what lets the fade's
 *          shaders compile before the first close-up. Those items come from a skin attachment's pooled compiled
 *          objects, which keep the PSOs they were built with for as long as the item and its flags stay the same (see
 *          Constants). A build of one that does not dissolve keeps the stock PSOs (they look the same) and builds aside
 *          the twin's and the stock's sets as the close-up fade will ask for them, with the dissolve in the
 *          description's flags and RT mask: the engine compiles the twin's dissolving permutations in the background,
 *          and the main thread holds each such set until it can draw. A table the render threads share keys every set
 *          built aside by its description, so one is never built twice. A twin is warm once every set of it can draw;
 *          only then does a dissolving item keep the twin (its pooled object rebuilds when the dissolve starts and
 *          finds the twin's PSOs compiled), while until then it takes the stock shader for the fade and its stock
 *          build also builds the twin's set aside. A dissolving build of an item that names the twin is checked like a
 *          tree's: one that asks for a permutation not compiled yet (another RT mask, a quality change) keeps the stock
 *          PSOs and turns the twin cold, so its items go back to the stock shader for the fade, rebuild, and warm it up
 *          again. A level change drops the sets of the old character node and keeps the ones built for the new one,
 *          and a twin with a set that can never draw (its permutation failed to compile) is given up. A render job
 *          also stamps each character source it sees on him, so eye_fade_ready() and character_fade_ready() know which
 *          sources he uses now. When the native forward-stage call is verified, character pairs build the stock set
 *          first and reuse its unchanged depth/G-buffer/custom PSOs, then request only the twin's forward stage.
 *          The generator checks that no other technique calls an edited pixel shader. Readiness and owned references
 *          still cover the resulting complete set; an unavailable proof keeps the full paired build.
 */

#include "hooks/shader_twins.hpp"
#include "config.hpp"
#include "hooks/twins/twin_state.hpp"

namespace TPVCamera
{
    using namespace Twins;

    DMK::Result<void> initialize_shader_twins(HookSet &hooks)
    {
        HookProfile::initialize();
        trace::initialize();
        DMK::Logger &logger = DMK::log();
        const std::uintptr_t genv = gated_anchor_address(Feature::ShaderTwins, AnchorId::Genv);
        const std::uintptr_t add_render_object = gated_anchor_address(Feature::ShaderTwins, AnchorId::AddRenderObject);
        const std::uintptr_t shader_gen = gated_anchor_address(Feature::ShaderTwins, AnchorId::ShaderGenLoad);
        const std::uintptr_t for_name = gated_anchor_address(Feature::ShaderTwins, AnchorId::ShaderForName);
        const std::uintptr_t adjust = gated_anchor_address(Feature::ShaderTwins, AnchorId::AdjustFileName);
        const std::uintptr_t create_psos = gated_anchor_address(Feature::ShaderTwins, AnchorId::CreatePsos);
        const auto forward_call =
            gated_anchor_address(Feature::CharacterForwardPsos, AnchorId::CharacterForwardPsosCall);
        s_character_forward_psos = nullptr;
        if (forward_call != 0 && site_in_function(forward_call, create_psos))
        {
            if (const auto target = DMK::scan::resolve_rip_relative(DMK::Address{forward_call}, 1, 5))
            {
                s_character_forward_psos = reinterpret_cast<ForwardPsosFn>(target->raw());
            }
        }
        DMK::log().info("ShaderTwins: character PSOs use {}",
                        s_character_forward_psos != nullptr ? "stock unchanged stages and native twin forward stage"
                                                            : "full paired builds (forward-stage proof unavailable)");
        // Both builders or neither: build_pair() needs the depth prepass for a Vegetation item and the G-buffer passes
        // for every other one.
        const auto prepass_call = gated_anchor_address(Feature::PlantDrawnPsos, AnchorId::PlantPrepassPsosCall);
        const auto gbuffer_call = gated_anchor_address(Feature::PlantDrawnPsos, AnchorId::PlantGBufferPsosCall);
        s_plant_prepass_psos = nullptr;
        s_plant_gbuffer_psos = nullptr;
        if (prepass_call != 0 && gbuffer_call != 0 && site_in_function(prepass_call, create_psos) &&
            site_in_function(gbuffer_call, create_psos))
        {
            const auto prepass = DMK::scan::resolve_rip_relative(DMK::Address{prepass_call}, 1, 5);
            const auto gbuffer = DMK::scan::resolve_rip_relative(DMK::Address{gbuffer_call}, 1, 5);
            if (prepass && gbuffer)
            {
                s_plant_prepass_psos = reinterpret_cast<PrepassPsosFn>(prepass->raw());
                s_plant_gbuffer_psos = reinterpret_cast<GBufferPsosFn>(gbuffer->raw());
            }
        }
        DMK::log().info("ShaderTwins: plant PSOs use {}",
                        s_plant_prepass_psos != nullptr
                            ? "stock sets with only the twin passes each item draws"
                            : "full paired builds (depth and G-buffer stage proof unavailable)");
        const auto set_state = gated_anchor_address(Feature::ShaderTwins, AnchorId::SetSystemGlobalState);
        const auto save_load = gated_anchor_address(Feature::ShaderTwins, AnchorId::SaveGameLoad);
        const auto load_cgf = gated_anchor_address(Feature::ShaderTwins, AnchorId::LoadCGFMaterial);
        const auto load_shader = gated_anchor_address(Feature::ShaderTwins, AnchorId::LoadMaterialShader);
        const auto flush_commands = gated_anchor_address(Feature::ShaderTwins, AnchorId::FlushRTCommands);
        const std::optional<std::int64_t> open_slot = gated_anchor_value(Feature::ShaderTwins, AnchorId::PakOpenSlot);
        const std::optional<std::int64_t> read_slot = gated_anchor_value(Feature::ShaderTwins, AnchorId::PakReadSlot);
        const std::optional<std::int64_t> close_slot = gated_anchor_value(Feature::ShaderTwins, AnchorId::PakCloseSlot);
        if (genv == 0 || add_render_object == 0 || shader_gen == 0 || for_name == 0 || adjust == 0 ||
            create_psos == 0 || set_state == 0 || save_load == 0 || load_cgf == 0 || load_shader == 0 ||
            flush_commands == 0 || !open_slot || !read_slot || !close_slot)
        {
            return std::unexpected(DMK::Error{DMK::ErrorCode::NoMatch, "shader_twins/anchors"});
        }
        s_genv = genv;
        s_for_name = for_name;
        s_adjust_file_name = adjust;
        s_flush_rt_commands = flush_commands;
        s_resource_lookup = reinterpret_cast<ResourceLookupFn>(
            gated_anchor_address(Feature::ShaderTwinRecovery, AnchorId::ResourceLookup));
        s_resource_lock =
            reinterpret_cast<PSRWLOCK>(gated_anchor_address(Feature::ShaderTwinRecovery, AnchorId::ResourceLock));
        s_shader_container_global =
            gated_anchor_address(Feature::ShaderTwinRecovery, AnchorId::ShaderResourceContainer);
        // The level inventory reads the CObjManager global; LevelBrushTables only proves the C3DEngine table offsets,
        // and g_env must be the one the twins use. Any miss keeps the capture-only twins.
        const std::uintptr_t inventory_genv = gated_anchor_address(Feature::TwinInventory, AnchorId::Genv);
        const std::uintptr_t vegetation_groups =
            gated_anchor_address(Feature::TwinInventory, AnchorId::VegetationGroups);
        const std::uintptr_t level_tables = gated_anchor_address(Feature::TwinInventory, AnchorId::LevelBrushTables);
        s_inventory_objman_global =
            inventory_genv == genv && vegetation_groups != 0 && level_tables != 0 ? vegetation_groups : 0;
        if (s_inventory_objman_global != 0)
        {
            logger.info("ShaderTwins: level inventory available (CObjManager global {}, level tables proven at {}); "
                        "every loading boundary prepares the level's plant and frame twins",
                        DMK::format::format_address(vegetation_groups), DMK::format::format_address(level_tables));
        }
        else
        {
            logger.warning("ShaderTwins: level inventory off ({}); twins come only from the materials a loading "
                           "screen loads and the plants drawn without one",
                           vegetation_groups == 0 || level_tables == 0
                               ? "the VegetationGroups or LevelBrushTables anchor did not resolve"
                               : "the TwinInventory gate's g_env differs");
        }
        s_asset_scope_tls = TlsAlloc();
        if (s_asset_scope_tls == TLS_OUT_OF_INDEXES)
        {
            return std::unexpected(DMK::Error{DMK::ErrorCode::NoMatch, "shader_twins/asset_scope_tls"});
        }
        s_pak_open_slot = static_cast<std::ptrdiff_t>(*open_slot);
        s_pak_read_slot = static_cast<std::ptrdiff_t>(*read_slot);
        s_pak_close_slot = static_cast<std::ptrdiff_t>(*close_slot);
        // The dissolve's RT-mask bit the character twins' sets for the fade are built ahead with; read when the twins
        // are prepared (read_dissolve_rt_bit()).
        s_dissolve_rt_global = gated_anchor_address(Feature::FadeWarmupRtBit, AnchorId::DissolveRtBit);

        // The window rows only the plants read. The upload mid hook reads the pipeline from r13, so it must sit inside
        // GeneratePerViewConstantBuffer and that function must keep its pipeline there; the hook writes only for the
        // game view's standard pipeline. Without it the plant twins stay off and the character fade's twins, which
        // need no rows, go on.
        const std::uintptr_t per_view = gated_anchor_address(Feature::TreeHoleWindow, AnchorId::PerViewConstants);
        const std::uintptr_t upload = gated_anchor_address(Feature::TreeHoleWindow, AnchorId::PerViewUpload);
        const std::optional<std::uintptr_t> standard_pipeline = class_vtable(GameClass::StandardPipeline);
        const char *window_miss = nullptr;
        if (per_view == 0 || upload == 0)
        {
            window_miss = "the per-view constant anchors did not resolve";
        }
        else if (!site_in_function(upload, per_view))
        {
            window_miss = "the per-view upload is not inside GeneratePerViewConstantBuffer";
        }
        else if (!keeps_pipeline_in_r13(per_view))
        {
            window_miss = "GeneratePerViewConstantBuffer no longer keeps its pipeline in r13";
        }
        else if (!standard_pipeline)
        {
            window_miss = "the CStandardGraphicsPipeline type did not resolve";
        }

        // The .ext alias first, so it is in place before any twin is created, and the PSO guard before the swap, so
        // no compiled object ever takes a twin PSO set that cannot draw yet. The other detours stay inert until the
        // first frame publishes a swap set and a window.
        DMK_TRY_VOID(
            install_inline_hook(hooks, "ShaderGenLoad", shader_gen, s_shader_gen_original, shader_gen_load_detour));
#if defined(TPVCAMERA_DEV_BUILD)
        TwinClone::initialize_twin_clone();
#endif
        DMK_TRY_VOID(install_inline_hook(hooks, "CreatePipelineStates", create_psos, s_create_psos_original,
                                         create_psos_detour));
        DMK_TRY_VOID(install_inline_hook(hooks, "CRenderViewAddRenderObject", add_render_object,
                                         s_add_render_object_original, add_render_object_detour));
        DMK_TRY_VOID(install_inline_hook(hooks, "TwinLoadCGFMaterial", load_cgf, s_load_cgf_material_original,
                                         load_cgf_material_detour));
        DMK_TRY_VOID(install_inline_hook(hooks, "TwinLoadMaterialShader", load_shader, s_load_material_shader_original,
                                         load_material_shader_detour));
        DMK_TRY_VOID(install_inline_hook(hooks, "TwinSetSystemGlobalState", set_state, s_set_system_state_original,
                                         set_system_state_detour));
        DMK_TRY_VOID(install_inline_hook(hooks, "TwinSaveGameLoad", save_load, s_save_game_load_original,
                                         save_game_load_detour));
        std::string window_error;
        if (window_miss == nullptr)
        {
            auto mid = DMK::hook::mid_at(
                DMK::hook::MidRequest{.name = "PerViewConstantUpload", .target = DMK::Address{upload}},
                per_view_upload_detour);
            if (!mid.has_value())
            {
                window_error = std::format("the per-view upload hook did not install ({})", mid.error().message());
            }
            else if (auto armed = hooks.push(std::move(*mid)).enable(); !armed.has_value())
            {
                window_error = std::format("the per-view upload hook did not arm ({})", armed.error().message());
            }
        }
        else
        {
            window_error = window_miss;
        }
        if (!window_error.empty())
        {
            for (int kind = 0; kind < k_kinds; ++kind)
            {
                if (listed_scope(blobs::k_twins[kind].scope))
                {
                    s_kind_failed[static_cast<std::size_t>(kind)] = true;
                }
            }
            logger.warning("ShaderTwins: {}; trees and frames keep the stock shader (no see-through, no near cut), the "
                           "character fade's twins go on",
                           window_error);
        }

        s_available.store(true, std::memory_order_release);
        if (window_error.empty())
        {
            logger.info(
                "ShaderTwins: hooks on the per-view upload {} (in GeneratePerViewConstantBuffer {}, pipeline in "
                "r13, CStandardGraphicsPipeline {}), AddRenderObject {}, the .ext loader {}, "
                "CreatePipelineStates {}; mfForName {}, CryPak slots FOpen {:#x} FReadRaw {:#x} FClose {:#x}; "
                "files and twins are prepared only inside verified level/save loading boundaries and paused frames",
                DMK::format::format_address(upload), DMK::format::format_address(per_view),
                DMK::format::format_address(*standard_pipeline), DMK::format::format_address(add_render_object),
                DMK::format::format_address(shader_gen), DMK::format::format_address(create_psos),
                DMK::format::format_address(for_name), s_pak_open_slot, s_pak_read_slot, s_pak_close_slot);
        }
        else
        {
            logger.info("ShaderTwins: hooks on AddRenderObject {}, the .ext loader {}, CreatePipelineStates {} (no "
                        "per-view upload); mfForName {}, CryPak slots FOpen {:#x} FReadRaw {:#x} FClose {:#x}; the "
                        "twins are prepared only inside verified level/save loading boundaries and paused frames",
                        DMK::format::format_address(add_render_object), DMK::format::format_address(shader_gen),
                        DMK::format::format_address(create_psos), DMK::format::format_address(for_name),
                        s_pak_open_slot, s_pak_read_slot, s_pak_close_slot);
        }
        logger.info("ShaderTwins: loading hooks SetSystemGlobalState {}, SaveGameLoad {}, LoadCGFMaterial {}, "
                    "LoadMaterialShader {}; FlushRTCommands {}; admission starts closed, {} retained candidate slots "
                    "({} of them only for plants and frames)",
                    DMK::format::format_address(set_state), DMK::format::format_address(save_load),
                    DMK::format::format_address(load_cgf), DMK::format::format_address(load_shader),
                    DMK::format::format_address(flush_commands), k_max_candidates, k_reserved_listed_candidates);
        logger.info("ShaderTwins: a plant or frame variant that comes after its loading screen (and an item of the "
                    "character drawn without its twin) gets its twin while the game stands still: the stopped game "
                    "clock (under the game menu, a UI overlay or neither), never a dialogue, from {} ms into it, one "
                    "at a time at least {} ms apart, each parse finished by a render-command barrier inside the pause",
                    Constants::SHADER_TWIN_PAUSE_DWELL_MS, Constants::SHADER_TWIN_PAUSE_SPACING_MS);
        return {};
    }

    bool shader_twins_available() noexcept
    {
        return s_available.load(std::memory_order_acquire) && !s_disabled.load(std::memory_order_acquire);
    }

    bool shader_twins_wanted() noexcept
    {
        const LiveSettings &cfg = settings();
        return cfg.use_tree_see_through.load(std::memory_order_relaxed) ||
               cfg.use_lens_cut.load(std::memory_order_relaxed) || cfg.close_up_fade.load(std::memory_order_relaxed);
    }

    void set_npc_twin_nodes(std::span<const std::uintptr_t> nodes) noexcept
    {
        s_npc_node_count = static_cast<int>(std::min(nodes.size(), s_npc_nodes.size()));
        std::copy_n(nodes.begin(), s_npc_node_count, s_npc_nodes.begin());
    }

    bool eye_fade_ready() noexcept
    {
        return shader_twins_available() && s_eye_ready.load(std::memory_order_acquire);
    }

    bool character_fade_ready() noexcept
    {
        return shader_twins_available() && s_character_ready.load(std::memory_order_acquire);
    }

    void update_shader_twins(std::span<const std::uintptr_t> character_nodes, const TreeHoleWindow &window,
                             std::span<void *const> trees, std::span<void *const> frames, float delta_time) noexcept
    {
        if (!s_available.load(std::memory_order_acquire) || s_loading_finalizing.load(std::memory_order_acquire))
        {
            trace_event(trace::Event::UpdateSkipped, 0, 0, 0, 0, 0);
            return;
        }
        DMK_PROFILE_SCOPE("twins.update");
        const std::unique_lock lock(s_main_mutex, std::try_to_lock);
        if (!lock.owns_lock() || !s_available.load(std::memory_order_acquire) ||
            s_loading_finalizing.load(std::memory_order_acquire))
        {
            trace_event(trace::Event::UpdateSkipped, 0, 0, 0, 0, 1);
            return;
        }
        // A new camera frame. A plant build still in flight will charge this frame when it completes.
        s_frame_seq.fetch_add(1, std::memory_order_relaxed);
        if constexpr (trace::enabled)
            trace::candidates(s_frame_seq.load(std::memory_order_relaxed), trees, frames, window.strength,
                              window.near_strength);
        const std::uint64_t now = GetTickCount64();
        if (s_rows_in_use.load(std::memory_order_acquire))
        {
            stop_plants(now, "the per-view rows the window rides in are in use");
        }
        if (s_disabled.load(std::memory_order_acquire))
        {
            trace_event(trace::Event::UpdateSkipped, 0, 0, 0, 0, 2);
            return;
        }
        // The character's render nodes, his own first, without zeros and at most k_max_character_nodes.
        std::array<std::uintptr_t, k_max_character_nodes> characters{};
        std::size_t character_count = 0;
        for (const std::uintptr_t node : character_nodes)
        {
            if (node != 0 && character_count < characters.size())
            {
                characters[character_count++] = node;
            }
        }
        // Recovery only retains already loaded shaders. Missing variants still wait for a pause
        // (service_shader_twins_paused()) or a loading transaction.
        recover_cached_twins(now);
        refresh_twins(now);
        revalidate_twins(now);
        if (s_disabled.load(std::memory_order_acquire))
        {
            return;
        }
        const std::uintptr_t root = character_count != 0 ? characters[0] : 0;
        if (root != s_character_root)
        {
            s_character_root = root;
            (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                     "ShaderTwins: the character's twins {} (character node {:#x}, {} carried node(s))",
                                     root != 0 ? "run" : "stopped", root,
                                     character_count != 0 ? character_count - 1 : 0);
        }
        // The sets for the fade are held and polled every frame, in first person too, until they can draw. A character
        // twin a render thread found cold leaves this frame's set, and one that warms up this frame goes in it, the
        // frame eye_fade_ready() turns true.
        take_cold_reports(now);
        drain_warm_records(now);
        // Another character render node than the last one, for CHARACTER_ROOT_SETTLE_FRAMES frames in a row, is a new
        // level or save: the kept sets of the old node belong to the old materials, and go. The sets handed over so
        // far are drained first, so every set built for the new node is kept. First person (no node) keeps them, and
        // so does a frame without the twins or a node that changes back.
        if (root != 0 && root != s_last_character_root)
        {
            s_root_frames = root == s_root_candidate ? s_root_frames + 1 : 1;
            s_root_candidate = root;
            if (s_root_frames >= Constants::CHARACTER_ROOT_SETTLE_FRAMES)
            {
                if (s_last_character_root != 0)
                {
                    forget_warm_sets(root, "the character's render node changed (a level or save was loaded)");
                }
                s_last_character_root = root;
            }
        }
        else if (root != 0)
        {
            s_root_candidate = 0;
            s_root_frames = 0;
        }
        poll_warm_entries(now);
        refresh_fade_warm(now);
        log_fade_miss();
        // Plants or frames whose twins all failed are not swapped at all; the nodes still held go back after their
        // hold.
        const bool plants = scope_usable(blobs::TwinScope::ListedPlants);
        const bool bare_frames = scope_usable(blobs::TwinScope::ListedFrames);
        const bool character_twins = scope_usable(blobs::TwinScope::Dissolving);
        const std::span<const std::uintptr_t> listed_characters(characters.data(),
                                                                character_twins ? character_count : 0);
        update_tracks(plants ? trees : std::span<void *const>{}, bare_frames ? frames : std::span<void *const>{},
                      std::max(delta_time, 0.0f), now);
        const std::span<const std::uintptr_t> listed_npcs(
            s_npc_nodes.data(), character_twins ? static_cast<std::size_t>(s_npc_node_count) : 0u);
        publish_set(listed_characters, listed_npcs, now);
        refresh_character_ready(character_twins, !listed_characters.empty(), now);
        // The records first, so a refill this frame drops the ones its rebuild replaces.
        drain_pso_records(now);
        apply_refills(now);
        poll_pso_records(now);
        if (scope_ready(blobs::TwinScope::ListedPlants) || scope_ready(blobs::TwinScope::ListedFrames))
        {
            publish_rows(window, now);
        }
        else
        {
            s_rows_published.store(nullptr, std::memory_order_release);
            trace_event(trace::Event::RowsDisabled, 0, 0, 0, now, 3);
        }
        log_second(now);
    }

    void service_shader_twins_paused(const TwinPauseSignals &signals) noexcept
    {
        if (!s_available.load(std::memory_order_acquire))
        {
            return;
        }
        if (!s_pause_service.load(std::memory_order_relaxed))
        {
            s_pause_service.store(true, std::memory_order_relaxed);
        }
        PauseState &pause = s_pause;
        if (pause.servicing)
        {
            return; // A camera frame inside a barrier's wait of create_during_pause().
        }
        if (!s_loading_finalizing.load(std::memory_order_acquire))
        {
            drop_evicted();
        }
        const std::uint64_t now = GetTickCount64();
        const PauseReason reason = pause_reason(signals);
        if (reason == PauseReason::None || s_disabled.load(std::memory_order_acquire) ||
            s_loading_finalizing.load(std::memory_order_acquire) || loading_materials_admitted() ||
            !shader_twins_wanted())
        {
            end_pause(now);
            return;
        }
        if (pause.since_ms == 0)
        {
            pause.since_ms = now;
        }
        pause.reason = reason;
        if (pause.off || now - pause.since_ms < Constants::SHADER_TWIN_PAUSE_DWELL_MS || now < pause.next_ms)
        {
            return;
        }
        create_during_pause(now, reason);
#if defined(TPVCAMERA_DEV_BUILD)
        service_paused_dry_run(reason);
#endif
    }

    void shutdown_shader_twins() noexcept
    {
        s_level_loading.store(false, std::memory_order_release);
        s_loading_save_manager.store(0, std::memory_order_release);
        try
        {
            const std::lock_guard lock(s_main_mutex);
            if (!s_available.exchange(false, std::memory_order_acq_rel))
            {
                return;
            }
            s_eye_ready.store(false, std::memory_order_release);
            s_character_ready.store(false, std::memory_order_release);
            const int swapped = s_track_count;
            release_all(GetTickCount64(), "shutdown");
            // A render thread that sees the module off drops its record's references itself, but one that checked just
            // before the exchange above can still hand a record over until the hooks retire. These are dropped now,
            // and release_shader_twin_records() drops any that arrive later, once no detour can run.
            PsoRecord record{};
            while (s_pso_records.pop(record) || s_warm_records.pop(record))
            {
                drop_record(record);
            }
            for (int i = 0; i < s_pending_count; ++i)
            {
                drop_record(s_pending[static_cast<std::size_t>(i)].record);
            }
            s_pending_count = 0;
            for (int i = 0; i < s_warm_entry_count; ++i)
            {
                drop_record(s_warm_entries[static_cast<std::size_t>(i)].record);
            }
            s_warm_entry_count = 0;
            s_warm_pending_count = 0;
            int twins = 0;
            for (int i = 0; i < s_source_count; ++i)
            {
                twins += s_sources[static_cast<std::size_t>(i)].twin != 0 ? 1 : 0;
            }
            (void)DMK::log().try_log(DMK::LogLevel::Info,
                                     "ShaderTwins: shutdown swapped {} tree(s) back; {} twin shader(s) stay loaded for "
                                     "the session; {} PSO reference(s) left held rather than drop a last one",
                                     swapped, twins, s_pso_leaked.load(std::memory_order_relaxed));
        }
        catch (...)
        {
            // A lock that cannot be taken leaves the trees to the next refill; the hooks still retire.
            s_available.store(false, std::memory_order_release);
            s_eye_ready.store(false, std::memory_order_release);
            s_character_ready.store(false, std::memory_order_release);
            s_set_published.store(nullptr, std::memory_order_release);
            s_rows_published.store(nullptr, std::memory_order_release);
        }
        clear_pause_inventory();
        clear_candidates();
    }

    void release_shader_twin_records() noexcept
    {
        try
        {
            const std::lock_guard lock(s_main_mutex);
            for (int i = 0; i < s_source_count; ++i)
            {
                Source &source = s_sources[static_cast<std::size_t>(i)];
                if (std::exchange(source.recovered, false))
                {
                    // The creating generation's session pins remain. Drop only this reload's extra lookup refs
                    // after hook rundown, so repeated reloads do not accumulate another pair of references.
                    release_candidate_shader(source.shader);
                    release_candidate_shader(source.twin);
                }
            }
            int released = 0;
            PsoRecord record{};
            while (s_pso_records.pop(record) || s_warm_records.pop(record))
            {
                drop_record(record);
                ++released;
            }
            if (released != 0)
            {
                (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                         "ShaderTwins: dropped {} PSO wait(s) and set(s) for the fade handed over "
                                         "while the shutdown ran",
                                         released);
            }
        }
        catch (...)
        {
            // A lock that cannot be taken leaves those references held: a few PSOs stay alive, nothing breaks.
        }
        // Hook rundown has completed, so no callback can still add a candidate or carry this slot's old scope across
        // its original call.
        drop_evicted();
        clear_pause_inventory();
        clear_candidates();
        if (s_asset_scope_tls != TLS_OUT_OF_INDEXES)
        {
            (void)TlsFree(s_asset_scope_tls);
            s_asset_scope_tls = TLS_OUT_OF_INDEXES;
        }
    }

    ShaderTwinStats shader_twin_stats() noexcept
    {
        return ShaderTwinStats{
            .swapped = s_stat_swapped.load(std::memory_order_relaxed),
            .twins = s_stat_twins.load(std::memory_order_relaxed),
            .twins_ready = s_stat_ready.load(std::memory_order_relaxed),
        };
    }

} // namespace TPVCamera
