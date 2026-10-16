/**
 * @file hooks/twins/render_swap.cpp
 * @brief The render-job side of the shader twins: AddRenderObject's twin choice for each item, the per-view hole rows
 * and their upload, and the helpers that read shaders, items and nodes.
 */

#include "hooks/twins/twin_state.hpp"

namespace TPVCamera::Twins
{
    namespace
    {

        /** @brief True when every source name has one kind per scope at most, as a kind lookup needs. */
        [[nodiscard]] constexpr bool kinds_unique() noexcept
        {
            for (int a = 0; a < k_kinds; ++a)
            {
                for (int b = a + 1; b < k_kinds; ++b)
                {
                    if (blobs::k_twins[a].scope == blobs::k_twins[b].scope &&
                        equals_nocase(blobs::k_twins[a].source_name, blobs::k_twins[b].source_name))
                    {
                        return false;
                    }
                }
            }
            return true;
        }
        static_assert(kinds_unique(), "Two k_twins rows give one source name two twins of the same scope.");
        static_assert(k_eye_kind >= 0 && blobs::k_twins[k_eye_kind].scope == blobs::TwinScope::Dissolving,
                      "The generated header has no Dissolving Eye twin.");

        /**
         * @brief Checks that the four rows of every view in the upload are still zero, then writes @p values into
         *        them.
         * @details @p data is the generator's buffer (count * 0x470 bytes) and @p stack the stack pointer at the upload
         *          call, where the view count is its fifth argument. Contract: each listed item reads the rows in
         *          exactly one depth-writing pass, and the rows are uploaded once per main-view generation, so a row
         *          change cannot make the depth and opaque passes disagree. The engine never fills the rows and only
         *          the DebugPostAA technique reads them (.z of rows 0 and 1 as jitter). A transposed perspective fill
         *          would zero both gates (row 0 .w, row 3 .x), but an off-centre orthographic fill sets row 0 .w, so
         *          any non-zero row reports InUse and turns the window off instead of being overwritten.
         * @note Render thread. SEH-guarded, POD body.
         */
        [[nodiscard]] RowWrite write_rows_guarded(std::uintptr_t data, std::uintptr_t stack,
                                                  const float *values) noexcept
        {
            __try
            {
                const std::uint32_t count =
                    *reinterpret_cast<const std::uint32_t *>(stack + Constants::PER_VIEW_UPLOAD_COUNT_STACK_OFFSET);
                if (count == 0 || count > Constants::PER_VIEW_MAX_VIEWS)
                {
                    return RowWrite::Skipped;
                }
                for (std::uint32_t view = 0; view < count; ++view)
                {
                    const auto *rows = reinterpret_cast<const std::uint32_t *>(
                        data + view * Constants::PER_VIEW_BUFFER_STRIDE + Constants::PER_VIEW_HOLE_ROWS_OFFSET);
                    for (int i = 0; i < k_row_floats; ++i)
                    {
                        if (rows[i] != 0)
                        {
                            return RowWrite::InUse;
                        }
                    }
                }
                for (std::uint32_t view = 0; view < count; ++view)
                {
                    std::memcpy(reinterpret_cast<void *>(data + view * Constants::PER_VIEW_BUFFER_STRIDE +
                                                         Constants::PER_VIEW_HOLE_ROWS_OFFSET),
                                values, sizeof(float) * k_row_floats);
                }
                return RowWrite::Written;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return RowWrite::Fault;
            }
        }

        /**
         * @brief True when the upload is the game view's: @p pipeline is the standard graphics pipeline and @p buffer
         *        is the main view's per-view buffer its holder keeps at +0x1F8.
         * @details Every graphics pipeline class (billboard and character-tool ones too) runs the same per-view update,
         *          and a fullscreen pass with its own per-view buffer uploads another buffer, so both tests are needed.
         *          A value in r13 that is not a pipeline fails the type test and leaves the rows alone.
         */
        [[nodiscard]] bool main_view_upload(std::uintptr_t pipeline, std::uintptr_t buffer) noexcept
        {
            if (pipeline == 0 || buffer == 0)
            {
                return false;
            }
            const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{pipeline});
            if (!vtable || !vtable_is(GameClass::StandardPipeline, *vtable))
            {
                return false;
            }
            const auto held =
                DMK::memory::read<std::uintptr_t>(DMK::Address{pipeline + Constants::PIPELINE_PER_VIEW_BUFFER_OFFSET});
            return held && *held == buffer;
        }

        /** @brief Turns the feature off from a render thread, once, when a row the engine leaves zero is in use. */
        void report_rows_in_use() noexcept
        {
            s_hole_rows.withdraw();
            s_swap_sets.withdraw();
            if (!s_rows_in_use.exchange(true, std::memory_order_acq_rel))
            {
                (void)DMK::log().try_log(DMK::LogLevel::Error,
                                         "ShaderTwins: the game now fills the per-view rows the window rides in "
                                         "(CV_ProjMatr); the tree see-through is off for the session (the "
                                         "character fade's twins need no rows and go on)");
            }
        }

        /**
         * @brief True when @p object belongs to a node of @p set (a merged grass patch's by its origin); @p scope then
         *        receives the scope its items swap in: ListedFrames for a bare frame, else ListedPlants.
         */
        [[nodiscard]] bool listed_object(const SwapSet &set, const std::uint8_t *object, blobs::TwinScope &scope,
                                         std::uintptr_t &matched_node) noexcept
        {
            if (set.node_count <= 0)
            {
                return false;
            }
            const std::uintptr_t node =
                *reinterpret_cast<const std::uintptr_t *>(object + Constants::RENDER_OBJECT_NODE_OFFSET);
            scope = blobs::TwinScope::ListedPlants;
            if (node == 0)
            {
                matched_node = merged_node_of_object(set, object);
                return matched_node != 0;
            }
            const int node_count = std::min(set.node_count, k_max_nodes);
            for (int i = 0; i < node_count; ++i)
            {
                const auto slot = static_cast<std::size_t>(i);
                if (set.nodes[slot] == node)
                {
                    matched_node = node;
                    if (set.frames[slot])
                    {
                        scope = blobs::TwinScope::ListedFrames;
                    }
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief Retains a missing variant for the next pause (service_shader_twins_paused()) or loading screen; never
         *        creates it in a camera frame.
         */
        void request_twin(const SwapSet &set, const ShaderItem &item, int kind) noexcept
        {
            if (set.sources_full)
            {
                return;
            }
            HookProfile::count(HookProfile::Counter::UntwinnedRetry);
            (void)collect_candidate(item, kind, 2, false);
        }

        /** @brief Counts an opaque item of a transparent-only kind that kept its source, while it dissolves. */
        void note_opacity_skip(bool dissolving) noexcept
        {
            if (dissolving)
            {
                s_opacity_skips.fetch_add(1, std::memory_order_relaxed);
            }
        }

        /**
         * @brief Puts @p item's twin in @p out when the item is in a scope of @p set and its shader is a source
         *        instance of a kind of that scope whose twin is ready (and, for a dissolving item of the character,
         *        warm for the fade).
         * @details An item is in the ListedPlants scope when @p object belongs to a listed plant (an object that names
         *          no node is a merged grass patch's when its origin is a listed patch's), in the ListedFrames scope
         *          when it belongs to a listed bare frame, else in the Dissolving
         *          scope when the character twins run and the object is a temporary one that names one of the set's
         *          character nodes (the character's own render node or a carried item's); every other character keeps
         *          the stock shaders. Such an item takes its twin as soon as the twin is ready while the object does
         *          not dissolve (the twin draws like the stock shader then, and its builds prepare the fade's sets,
         *          see build_resting_psos()), and while it dissolves only once the twin's sets for the fade are warm.
         *          A render job stamps each character source it sees, so the readiness knows the character's current
         *          items. A shader of the item's scope with no entry yet is queued for the main thread to create its
         *          twin; the item stays on its source this time (a listed node is refilled once the twin is ready, a
         *          temporary object is added again next frame). A transparent-only kind (IllumFade) takes only an item
         *          whose opacity is below 1: an opaque Illum item of the character (his clothes) never asks for, warms
         *          or swaps to that twin. An item that is not set up yet (preprocess flags -1) is left to the engine,
         *          which invalidates the object itself.
         * @note Render jobs. Reads engine data that is valid for the call.
         */
        [[nodiscard]] bool twin_item(const SwapSet &set, const ShaderItem &item, std::uint8_t *object, ShaderItem &out,
                                     std::uintptr_t &matched_node) noexcept
        {
            // A local recorder uses only identities already read by classification. No new engine lookups.
            struct ItemTrace
            {
                const SwapSet &set;
                const ShaderItem &item;
                std::uint8_t *object;
                const std::uintptr_t &node;
                trace::ItemReason reason = trace::ItemReason::InvalidItem;
                std::uintptr_t twin = 0;
                ~ItemTrace() noexcept
                {
                    if constexpr (trace::enabled)
                        if (node != 0)
                            trace_event(twin != 0 ? trace::Event::ItemTwin : trace::Event::ItemStock, node,
                                        reinterpret_cast<std::uintptr_t>(object), item.shader, item.resources,
                                        twin != 0 ? twin : static_cast<std::uint64_t>(reason), set.stamp_ms);
                }
            } observation{set, item, object, matched_node};
            if (item.shader == 0 || item.preprocess_flags == 0xFFFFFFFFu)
            {
                if constexpr (trace::enabled)
                {
                    blobs::TwinScope ignored{};
                    (void)listed_object(set, object, ignored, matched_node);
                }
                return false;
            }
            blobs::TwinScope scope = blobs::TwinScope::ListedPlants;
            bool dissolving = false;
            bool npc = false;
            if (!listed_object(set, object, scope, matched_node))
            {
                observation.reason = trace::ItemReason::BindingExpired;
                if ((set.character_count <= 0 && set.npc_count <= 0) ||
                    object[Constants::RENDER_OBJECT_PERSISTENT_OFFSET] != 0)
                {
                    return false;
                }
                dissolving = dissolving_object(object);
                if (!character_object(set, object))
                {
                    // A person or an animal the NPC lens fade dissolves: only its dissolving items take a twin.
                    npc = dissolving && npc_object(set, object);
                    if (!npc)
                    {
                        if (dissolving)
                        {
                            s_node_rejects.fetch_add(1, std::memory_order_relaxed);
                        }
                        return false;
                    }
                }
                scope = blobs::TwinScope::Dissolving;
            }
            // The camera stopped updating the set (a menu, a reload): refills keep the stock shader.
            const std::uint64_t now = GetTickCount64();
            if (now - set.stamp_ms > Constants::SHADER_TWIN_STALE_MS)
            {
                observation.reason = trace::ItemReason::StaleSet;
                return false;
            }
            const bool character = scope == blobs::TwinScope::Dissolving;
            const int scan_count = std::min(character ? set.character_source_count : set.source_count, k_max_sources);
            for (int i = 0; i < scan_count; ++i)
            {
                const auto slot = character
                                      ? static_cast<std::size_t>(set.character_sources[static_cast<std::size_t>(i)])
                                      : static_cast<std::size_t>(i);
                const int kind = set.kinds[slot];
                if (set.sources[slot] != item.shader || blobs::k_twins[kind].scope != scope)
                {
                    continue;
                }
                // An opaque item of a transparent-only kind keeps its source and does not count as on the character.
                if (!item_fits_kind(kind, item))
                {
                    observation.reason = trace::ItemReason::Opacity;
                    note_opacity_skip(dissolving);
                    return false;
                }
                if (character && !npc)
                {
                    s_character_seen_ms[slot].store(now, std::memory_order_relaxed);
                }
                // Not ready, given up, or (a dissolving item of the character) not warm for the fade yet: the item
                // keeps its source.
                if (set.twins[slot] == 0 || (dissolving && !set.fade_ready[slot]))
                {
                    observation.reason = trace::ItemReason::TwinUnavailable;
                    if (npc)
                    {
                        s_npc_misses.fetch_add(1, std::memory_order_relaxed);
                    }
                    return false;
                }
                if (npc)
                {
                    s_npc_swaps.fetch_add(1, std::memory_order_relaxed);
                }
                out = item;
                out.shader = set.twins[slot];
                observation.twin = out.shader;
                s_item_swaps.fetch_add(1, std::memory_order_relaxed);
                if (character && !dissolving)
                {
                    s_rest_swaps.fetch_add(1, std::memory_order_relaxed);
                }
                else
                {
                    s_kind_swaps[static_cast<std::size_t>(kind)].fetch_add(1, std::memory_order_relaxed);
                }
                return true;
            }
            // The name test first, which rejects nearly every item of the character.
            const int kind = source_kind_guarded(item.shader, scope);
            if (kind < 0)
            {
                observation.reason = trace::ItemReason::UnsupportedShader;
                return false;
            }
            if (!item_fits_kind(kind, item))
            {
                observation.reason = trace::ItemReason::Opacity;
                note_opacity_skip(dissolving);
                return false;
            }
            if (npc)
            {
                // A shader of a person's that the twins have no entry for: no twin is asked for here, the player's
                // close-up fade decides which twins exist.
                observation.reason = trace::ItemReason::TwinUnavailable;
                s_npc_misses.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            if (character && !set.sources_full)
            {
                s_character_unknown.store(true, std::memory_order_relaxed);
            }
            request_twin(set, item, kind);
            observation.reason = set.sources_full                                  ? trace::ItemReason::SourceTableFull
                                 : s_pause_service.load(std::memory_order_relaxed) ? trace::ItemReason::AwaitingPause
                                                                                   : trace::ItemReason::AwaitingLoading;
            return false;
        }
    } // namespace

    [[nodiscard]] int source_kind_guarded(std::uintptr_t shader, blobs::TwinScope scope) noexcept
    {
        __try
        {
            const char *name = *reinterpret_cast<const char *const *>(shader + Constants::SHADER_NAME_OFFSET);
            if (name == nullptr)
            {
                return -1;
            }
            for (int kind = 0; kind < k_kinds; ++kind)
            {
                if (blobs::k_twins[kind].scope == scope && equals_nocase(name, blobs::k_twins[kind].source_name))
                {
                    return kind;
                }
            }
            return -1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    [[nodiscard]] bool match_feature_flags_guarded(std::uintptr_t twin, std::uintptr_t source, std::uint16_t &before,
                                                   std::uint16_t &after) noexcept
    {
        __try
        {
            auto *word = reinterpret_cast<volatile short *>(twin + Constants::SHADER_FEATURE_FLAGS_OFFSET);
            const std::uint16_t wanted =
                *reinterpret_cast<const volatile std::uint16_t *>(source + Constants::SHADER_FEATURE_FLAGS_OFFSET);
            before = static_cast<std::uint16_t>(*word);
            const auto missing = static_cast<std::uint16_t>(wanted & ~before);
            if (missing != 0)
            {
                _InterlockedOr16(word, static_cast<short>(missing));
            }
            after = static_cast<std::uint16_t>(*word);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    [[nodiscard]] bool read_shader_state_guarded(std::uintptr_t shader, ShaderState &out) noexcept
    {
        __try
        {
            out.flags = *reinterpret_cast<const volatile std::uint32_t *>(shader + Constants::SHADER_FLAGS_OFFSET);
            out.flags2 = *reinterpret_cast<const volatile std::uint32_t *>(shader + Constants::SHADER_FLAGS2_OFFSET);
            out.techniques =
                *reinterpret_cast<const volatile std::uint32_t *>(shader + Constants::SHADER_TECHNIQUE_COUNT_OFFSET);
            out.mask = *reinterpret_cast<const volatile std::uint64_t *>(shader + Constants::SHADER_GEN_MASK_OFFSET);
            out.features =
                *reinterpret_cast<const volatile std::uint16_t *>(shader + Constants::SHADER_FEATURE_FLAGS_OFFSET);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    [[nodiscard]] float item_opacity_guarded(std::uintptr_t resources) noexcept
    {
        __try
        {
            if (resources == 0)
            {
                return 1.0f;
            }
            const std::uintptr_t vtable = *reinterpret_cast<const std::uintptr_t *>(resources);
            if (!vtable_is(GameClass::ShaderResources, vtable))
            {
                return 1.0f;
            }
            using GetStrengthFn = float(__fastcall *)(std::uintptr_t, int);
            const auto get_strength = *reinterpret_cast<const GetStrengthFn *>(
                vtable + Constants::SHADER_RESOURCES_VTABLE_GET_STRENGTH_OFFSET);
            return get_strength(resources, Constants::SHADER_RESOURCES_OPACITY_SLOT);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 1.0f;
        }
    }

    [[nodiscard]] bool item_fits_kind(int kind, const ShaderItem &item) noexcept
    {
        return !blobs::k_twins[kind].transparent_only || item_opacity_guarded(item.resources) < 1.0f;
    }

    [[nodiscard]] bool merged_origin_guarded(std::uintptr_t node, float *out) noexcept
    {
        __try
        {
            using GetTypeFn = int(__fastcall *)(std::uintptr_t);
            const auto *vtable = *reinterpret_cast<const std::uintptr_t *const *>(node);
            const auto get_type = reinterpret_cast<GetTypeFn>(vtable[Constants::RENDERNODE_VTABLE_GETTYPE_OFFSET / 8]);
            if (get_type(node) != Constants::EERTYPE_MERGED_MESH)
            {
                return false;
            }
            const auto *origin = reinterpret_cast<const float *>(node + Constants::MERGED_MESH_ORIGIN_OFFSET);
            out[0] = origin[0];
            out[1] = origin[1];
            out[2] = origin[2];
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    [[nodiscard]] std::uintptr_t merged_node_of_object(const SwapSet &set, const std::uint8_t *object) noexcept
    {
        const auto *matrix = reinterpret_cast<const float *>(object + Constants::RENDER_OBJECT_MATRIX_OFFSET);
        const float x = matrix[3];
        const float y = matrix[7];
        const float z = matrix[11];
        const int node_count = std::min(set.node_count, k_max_nodes);
        for (int i = 0; i < node_count; ++i)
        {
            const auto slot = static_cast<std::size_t>(i);
            if (set.merged[slot] && set.origins[slot][0] == x && set.origins[slot][1] == y && set.origins[slot][2] == z)
            {
                return set.nodes[slot];
            }
        }
        return 0;
    }

    void copy_text_field_guarded(std::uintptr_t object, std::ptrdiff_t offset, char *out, std::size_t cap) noexcept
    {
        __try
        {
            const char *text = *reinterpret_cast<const char *const *>(object + offset);
            std::size_t i = 0;
            if (text != nullptr)
            {
                for (; i + 1 < cap && text[i] != '\0'; ++i)
                {
                    out[i] = (text[i] >= 0x20 && text[i] < 0x7F) ? text[i] : '?';
                }
            }
            out[i] = '\0';
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            out[0] = '\0';
        }
    }

    [[nodiscard]] Invalidate invalidate_node_guarded(std::uintptr_t node) noexcept
    {
        __try
        {
            const std::uintptr_t temp =
                *reinterpret_cast<const std::uintptr_t *>(node + Constants::RENDERNODE_TEMP_DATA_OFFSET);
            if (temp == 0)
            {
                return Invalidate::NoRender;
            }
            if (*reinterpret_cast<const std::uintptr_t *>(temp + Constants::RENDER_TEMP_DATA_OWNER_OFFSET) != node)
            {
                return Invalidate::NotTheNode;
            }
            const long stamp =
                *reinterpret_cast<const volatile long *>(temp + Constants::TEMP_DATA_STAMP_SOURCE_OFFSET);
            _InterlockedExchange(reinterpret_cast<volatile long *>(temp + Constants::TEMP_DATA_STAMP_TARGET_OFFSET),
                                 stamp);
            *reinterpret_cast<volatile std::int32_t *>(node + Constants::RENDERNODE_INVALIDATE_STAMP_OFFSET) = -1;
            return Invalidate::Done;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return Invalidate::Fault;
        }
    }

    [[nodiscard]] Invalidate node_state_guarded(std::uintptr_t node) noexcept
    {
        __try
        {
            const std::uintptr_t temp =
                *reinterpret_cast<const std::uintptr_t *>(node + Constants::RENDERNODE_TEMP_DATA_OFFSET);
            if (temp == 0)
            {
                return Invalidate::NoRender;
            }
            return *reinterpret_cast<const std::uintptr_t *>(temp + Constants::RENDER_TEMP_DATA_OWNER_OFFSET) == node
                       ? Invalidate::Done
                       : Invalidate::NotTheNode;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return Invalidate::Fault;
        }
    }

    void per_view_upload_detour(DMK::hook::MidContext &ctx) noexcept
    {
        HookProfile::Scope timing(HookProfile::Path::PerViewUpload);
        const HoleRows *rows = s_hole_rows.latest();
        if (rows == nullptr || GetTickCount64() - rows->stamp_ms > Constants::SHADER_TWIN_STALE_MS ||
            !main_view_upload(DMK::hook::gpr(ctx, DMK::hook::Gpr::R13), DMK::hook::gpr(ctx, DMK::hook::Gpr::Rcx)))
        {
            return;
        }
        const std::uintptr_t data = DMK::hook::gpr(ctx, DMK::hook::Gpr::Rdx);
        if (data == 0)
        {
            return;
        }
        const auto written = write_rows_guarded(data, DMK::hook::stack_pointer(ctx), rows->values.data());
        trace_event(trace::Event::RowsWrite, 0, data, 0, rows->stamp_ms, static_cast<std::uint64_t>(written));
        switch (written)
        {
        case RowWrite::Written:
            timing.outcome(HookProfile::Outcome::Written);
            s_rows_written.fetch_add(1, std::memory_order_relaxed);
            break;
        case RowWrite::InUse:
            timing.outcome(HookProfile::Outcome::InUse);
            report_rows_in_use();
            break;
        case RowWrite::Fault:
            timing.outcome(HookProfile::Outcome::Fault);
            s_guard_faults.fetch_add(1, std::memory_order_relaxed);
            break;
        case RowWrite::Skipped:
        default:
            break;
        }
    }

    [[nodiscard]] bool dissolving_object(const std::uint8_t *object) noexcept
    {
        const auto flags = *reinterpret_cast<const std::uint64_t *>(object + Constants::RENDER_OBJECT_FLAGS_OFFSET);
        return (flags & Constants::RENDER_OBJECT_FLAG_DISSOLVE) != 0 &&
               object[Constants::RENDER_OBJECT_PERSISTENT_OFFSET] == 0;
    }

    [[nodiscard]] bool character_object(const SwapSet &set, const std::uint8_t *object) noexcept
    {
        const std::uintptr_t node =
            *reinterpret_cast<const std::uintptr_t *>(object + Constants::RENDER_OBJECT_NODE_OFFSET);
        if (node == 0)
        {
            return false;
        }
        const int count = std::min(set.character_count, k_max_character_nodes);
        for (int i = 0; i < count; ++i)
        {
            if (set.character_nodes[static_cast<std::size_t>(i)] == node)
            {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool npc_object(const SwapSet &set, const std::uint8_t *object) noexcept
    {
        const std::uintptr_t node =
            *reinterpret_cast<const std::uintptr_t *>(object + Constants::RENDER_OBJECT_NODE_OFFSET);
        if (node == 0)
        {
            return false;
        }
        const int count = std::min(set.npc_count, Constants::NPC_FADE_MAX_ENTITIES);
        for (int i = 0; i < count; ++i)
        {
            if (set.npc_nodes[static_cast<std::size_t>(i)] == node)
            {
                return true;
            }
        }
        return false;
    }

    void __fastcall add_render_object_detour(void *view, void *element, const ShaderItem *item, std::uint8_t *object,
                                             const std::uint8_t *pass, int list, int after_water) noexcept
    {
        const DetourScope in_flight;
        HookProfile::Scope timing(HookProfile::Path::AddRenderObject);
        const AddRenderObjectFn original = s_add_render_object_original.load(std::memory_order_acquire);
        const SwapSet *set = s_swap_sets.latest();
        if (set != nullptr && element != nullptr && item != nullptr && object != nullptr && pass != nullptr &&
            main_view_pass(pass))
        {
            ShaderItem swapped{};
            std::uintptr_t matched_node = 0;
            if (twin_item(*set, *item, object, swapped, matched_node))
            {
                timing.outcome(HookProfile::Outcome::Twin);
                {
                    const HookProfile::EngineScope engine(HookProfile::Path::EngineAddTwin, timing);
                    original(view, element, &swapped, object, pass, list, after_water);
                }
                if (matched_node != 0)
                    trace_event(trace::Event::SubmitTwinReturned, matched_node,
                                reinterpret_cast<std::uintptr_t>(object), swapped.shader, swapped.resources,
                                reinterpret_cast<std::uintptr_t>(element), set->stamp_ms);
                return;
            }
        }
        const HookProfile::EngineScope engine(HookProfile::Path::EngineAddStock, timing);
        original(view, element, item, object, pass, list, after_water);
    }
} // namespace TPVCamera::Twins
