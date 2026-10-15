/**
 * @file hooks/twins/pso_guard.cpp
 * @brief The shader twins' guarded pipeline-state builds: CreatePipelineStates, the paired stock and twin sets, the
 * sets built aside for the fade, the shared build-key table and the build budget.
 */

#include "hooks/twins/twin_state.hpp"

namespace TPVCamera::Twins
{
    namespace
    {

        /** @brief Adds a reference to a PSO, as the engine's smart pointer does, under SEH. */
        [[nodiscard]] bool add_pso_ref_guarded(std::uintptr_t pso) noexcept
        {
            __try
            {
                _InterlockedIncrement(reinterpret_cast<volatile long *>(pso + Constants::PSO_REF_COUNT_OFFSET));
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        /**
         * @brief Drops one reference to a PSO unless it is the last one, under SEH.
         * @details The last reference destroys the PSO through its vtable, which belongs to the engine's own release
         *          path, so that one is left held instead. The factory keeps a reference to every PSO it made, so only
         *          a factory flush could leave the mod holding the last one.
         * @return True when the reference was dropped, false when it was left held or the read faulted.
         */
        [[nodiscard]] bool drop_pso_ref_guarded(std::uintptr_t pso) noexcept
        {
            __try
            {
                auto *count = reinterpret_cast<volatile long *>(pso + Constants::PSO_REF_COUNT_OFFSET);
                long seen = *count;
                while (seen > 1)
                {
                    const long prior = _InterlockedCompareExchange(count, seen - 1, seen);
                    if (prior == seen)
                    {
                        return true;
                    }
                    seen = prior;
                }
                return false;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        /**
         * @brief Writes the attempt count (Constants::PSO_STATE_ATTEMPTS_MASK) of each PSO of @p set in @p slots into
         *        @p out, 0 for an empty slot.
         * @note Any thread, on PSOs the caller holds references to. SEH-guarded, POD body.
         */
        void read_attempts_guarded(const std::uintptr_t *set, std::uint32_t slots, std::uint32_t *out) noexcept
        {
            __try
            {
                for (int i = 0; i < k_pso_slots; ++i)
                {
                    out[i] =
                        ((slots & (1u << i)) != 0 && set[i] != 0)
                            ? *reinterpret_cast<const volatile std::uint32_t *>(set[i] + Constants::PSO_STATE_OFFSET) &
                                  Constants::PSO_STATE_ATTEMPTS_MASK
                            : 0;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                out[0] = 0;
            }
        }

        /**
         * @brief The render node of the compiled object whose PSO array is @p psos (object -> CRenderObject -> node)
         *        when that render object is persistent, else 0.
         * @details Only a persistent render object keeps its compiled objects until the node is refilled. A temporary
         *          one is built again every frame and takes the twin's PSOs by itself once they are ready, so nothing
         *          needs to wait for it. A merged grass patch's persistent object names no node and is matched to its
         *          patch by origin (merged_node_of_object()).
         * @note Render thread. SEH-guarded, POD body.
         */
        [[nodiscard]] std::uintptr_t persistent_node_of_psos_guarded(const std::uintptr_t *psos) noexcept
        {
            __try
            {
                const std::uintptr_t object =
                    reinterpret_cast<std::uintptr_t>(psos) - Constants::COMPILED_OBJECT_PSO_OFFSET;
                const std::uintptr_t render_object =
                    *reinterpret_cast<const std::uintptr_t *>(object + Constants::COMPILED_OBJECT_RENDER_OBJECT_OFFSET);
                if (render_object == 0)
                {
                    return 0;
                }
                const std::uint8_t persistent =
                    *reinterpret_cast<const std::uint8_t *>(render_object + Constants::RENDER_OBJECT_PERSISTENT_OFFSET);
                if (persistent == 0)
                {
                    return 0;
                }
                const std::uintptr_t node =
                    *reinterpret_cast<const std::uintptr_t *>(render_object + Constants::RENDER_OBJECT_NODE_OFFSET);
                if (node != 0)
                {
                    return node;
                }
                const SwapSet *set = s_set_published.load(std::memory_order_acquire);
                return set != nullptr
                           ? merged_node_of_object(*set, reinterpret_cast<const std::uint8_t *>(render_object))
                           : 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }

        // Reads the compiled object and the description while the native call owns them. The recorder keeps the
        // pointers as identities and never dereferences them later.
        void trace_psos(trace::Event event, const std::uintptr_t *psos, const std::uint8_t *desc, std::uint64_t a = 0,
                        std::uint64_t b = 0) noexcept
        {
            if constexpr (trace::enabled)
            {
                __try
                {
                    const auto compiled =
                        reinterpret_cast<std::uintptr_t>(psos) - Constants::COMPILED_OBJECT_PSO_OFFSET;
                    const auto node = persistent_node_of_psos_guarded(psos);
                    trace_event(event, node, compiled, *reinterpret_cast<const std::uintptr_t *>(desc),
                                *reinterpret_cast<const std::uintptr_t *>(desc + Constants::PSO_DESC_RESOURCES_OFFSET),
                                a, b);
                    if (event == trace::Event::BuildBegin)
                        trace_event(
                            trace::Event::CompiledBinding, node, compiled, 0,
                            *reinterpret_cast<const std::uintptr_t *>(compiled +
                                                                      Constants::COMPILED_OBJECT_RENDER_OBJECT_OFFSET),
                            *reinterpret_cast<const std::uint64_t *>(desc + Constants::PSO_DESC_RT_MASK_OFFSET),
                            *reinterpret_cast<const std::uint64_t *>(desc + Constants::PSO_DESC_OBJECT_FLAGS_OFFSET));
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    trace_event(trace::Event::RecordLost, 0, reinterpret_cast<std::uintptr_t>(psos), 0, 0, 4);
                }
            }
        }

        /**
         * @brief The source instance of @p shader when it is a ready twin, else 0, and in @p prepass whether its items
         *        take the hole in the depth prepass (prepass_kind()). Any thread.
         */
        [[nodiscard]] std::uintptr_t source_of_twin(std::uintptr_t shader, bool *prepass = nullptr) noexcept
        {
            const int count = std::min(s_guard_count.load(std::memory_order_acquire), k_max_sources);
            for (int i = 0; i < count; ++i)
            {
                const auto at = static_cast<std::size_t>(i);
                if (s_guard_twins[at].load(std::memory_order_relaxed) == shader)
                {
                    if (prepass != nullptr)
                    {
                        *prepass = s_guard_prepass[at].load(std::memory_order_relaxed);
                    }
                    return s_guard_sources[at].load(std::memory_order_relaxed);
                }
            }
            return 0;
        }

        // Observe natural stock opportunities and actual plant-twin demand, never the synthetic stock build in a
        // pair. Only scalar copies escape this callback; the recorder neither retains nor invokes engine objects.
        void trace_pso_opportunity(void *pipeline, const std::uintptr_t *psos, const std::uint8_t *desc, void *cache,
                                   std::uintptr_t source = 0) noexcept
        {
#if defined(TPVCAMERA_DEV_BUILD) && defined(DMK_ENABLE_PROFILING)
            if (!s_available.load(std::memory_order_acquire) || s_disabled.load(std::memory_order_acquire))
                return;
            __try
            {
                const auto shader = *reinterpret_cast<const std::uintptr_t *>(desc);
                const bool demand = source != 0;
                const auto stock = demand ? source : shader;
                const char *name = *reinterpret_cast<const char *const *>(stock + Constants::SHADER_NAME_OFFSET);
                if (name == nullptr)
                    return;
                int kind = -1;
                for (int i = 0; i < k_kinds; ++i)
                {
                    if (blobs::k_twins[i].scope == blobs::TwinScope::ListedPlants &&
                        equals_nocase(name, blobs::k_twins[i].source_name))
                    {
                        kind = i;
                        break;
                    }
                }
                if (kind < 0)
                    return;
                std::uintptr_t ready_twin = 0;
                const int count = std::min(s_guard_count.load(std::memory_order_acquire), k_max_sources);
                for (int i = 0; i < count; ++i)
                {
                    const auto at = static_cast<std::size_t>(i);
                    if (s_guard_sources[at].load(std::memory_order_relaxed) == stock &&
                        s_trace_guard_kinds[at].load(std::memory_order_relaxed) == kind)
                    {
                        ready_twin = s_guard_twins[at].load(std::memory_order_relaxed);
                        break;
                    }
                }
                if (demand && ready_twin != shader)
                    return; // Frame/character Illum variants are not plant demand.
                trace::PsoDescriptor key{};
                static_assert(sizeof(key.words) == Constants::PSO_DESC_SIZE);
                std::memcpy(key.words.data(), desc, sizeof(key.words));
                key.words[0] = stock;
                key.pipeline = reinterpret_cast<std::uintptr_t>(pipeline);
                key.cache = reinterpret_cast<std::uintptr_t>(cache);
                key.load_epoch = s_load_epoch.load(std::memory_order_relaxed);
                key.kind = static_cast<std::uint32_t>(kind);
                const auto compiled = reinterpret_cast<std::uintptr_t>(psos) - Constants::COMPILED_OBJECT_PSO_OFFSET;
                trace::observe_pso_descriptor(key, persistent_node_of_psos_guarded(psos), compiled, ready_twin, demand);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                trace::pso_descriptor_fault();
            }
#else
            (void)pipeline;
            (void)psos;
            (void)desc;
            (void)cache;
            (void)source;
#endif
        }

        /** @brief Raises the slowest-build mark to @p micros. */
        void note_build_time(std::uint32_t micros) noexcept
        {
            std::uint32_t seen = s_pso_slowest_us.load(std::memory_order_relaxed);
            while (micros > seen && !s_pso_slowest_us.compare_exchange_weak(seen, micros, std::memory_order_relaxed))
            {
            }
        }

        /** @brief Both PSO sets of one build: the twin's, built aside, and the stock's, built into the object. */
        struct PsoPair
        {
            PsoSet twin{};
            std::uint32_t slots = 0;       // bit i: slot i was written by this build
            bool twin_built = false;       // the twin build reported success
            std::uint8_t stock_built = 0;  // the stock build's result
            std::uint32_t twin_micros = 0; // how long the twin build took
            bool reused_stock_stages = false;
        };

        /** @brief The 64-bit field of a pipeline-state description at @p offset. */
        [[nodiscard]] std::uint64_t desc_field(const std::uint8_t *desc, std::ptrdiff_t offset) noexcept
        {
            std::uint64_t value = 0;
            std::memcpy(&value, desc + offset, sizeof(value));
            return value;
        }

        /** @brief Reads a stage of the current native call's pipeline, never retaining a pipeline pointer. */
        [[nodiscard]] void *pipeline_stage_guarded(void *pipeline, std::ptrdiff_t offset) noexcept
        {
            __try
            {
                return *reinterpret_cast<void **>(reinterpret_cast<std::uintptr_t>(pipeline) + offset);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return nullptr;
            }
        }

        /**
         * @brief The slots where a plant or frame twin draws differently from its stock shader, among those the stock
         *        build of @p psos filled: the depth prepass (slot 0) for a prepassed Vegetation item, else the G-buffer
         *        passes 0 and 1 (slots 5 and 6). 0 when none of them was filled.
         * @details A prepassed item draws slot 0 and then slot 6, the Attr pass, whose stock PSO tests depth EQUAL
         *          without writing it: it draws only where the twin's prepass left the item's own depth, so the hole
         *          carries over with the stock PSO, which also keeps the early depth test the twin's would lose. Slot 7
         *          (Micro) is never drawn, and the twins change neither the forward nor the custom stage. Any other
         *          item draws slot 5, or slot 6 as a decal (the same description unless the object has FOB_ZPREPASS).
         *          The object's FOB_ZPREPASS comes from any of its items, so it picks the prepass only for the
         *          Vegetation kind, whose items carry the batch flag (prepass_kind()). An item the engine draws without
         *          the prepass anyway (r_UseZPass below 2, past r_ZPrepassMaxDist) only goes without the hole; no pass
         *          loses its PSO.
         */
        [[nodiscard]] std::uint32_t plant_drawn_slots(const std::uintptr_t *psos, const std::uint8_t *desc,
                                                      bool prepass) noexcept
        {
            if (prepass && psos[Constants::PSO_ZPREPASS_SLOT] != 0 &&
                (desc_field(desc, Constants::PSO_DESC_OBJECT_FLAGS_OFFSET) & Constants::RENDER_OBJECT_FLAG_ZPREPASS) !=
                    0)
            {
                return 1u << Constants::PSO_ZPREPASS_SLOT;
            }
            std::uint32_t slots = 0;
            for (const int slot : {Constants::PSO_GBUFFER_FULL_SLOT, Constants::PSO_GBUFFER_ATTR_SLOT})
            {
                slots |= psos[slot] != 0 ? 1u << slot : 0u;
            }
            return slots;
        }

        /**
         * @brief Builds only the twin passes @p slots names (plant_drawn_slots()) into @p twin, through the native
         *        depth-prepass and G-buffer pass builders, so the engine compiles no twin permutation an item never
         *        draws. No pipeline, cache or description is retained past this native callback.
         * @return True when every pass built (its PSO may still be compiling).
         */
        [[nodiscard]] bool build_plant_twin(const ProfiledCreatePsos &original, void *pipeline, PsoSet &twin,
                                            std::uint8_t *desc, std::uint32_t slots) noexcept
        {
            HookProfile::EngineScope timing(HookProfile::Path::EnginePsoTwin, original.owner);
            bool built = true;
            if ((slots & (1u << Constants::PSO_ZPREPASS_SLOT)) != 0)
            {
                void *stage = pipeline_stage_guarded(pipeline, Constants::PIPELINE_ZPREPASS_STAGE_OFFSET);
                std::memcpy(desc + Constants::PSO_DESC_TECHNIQUE_TYPE_OFFSET, &Constants::PSO_TECHNIQUE_ZPREPASS,
                            sizeof(Constants::PSO_TECHNIQUE_ZPREPASS));
                built = stage != nullptr &&
                        s_plant_prepass_psos(stage, desc, twin.data() + Constants::PSO_ZPREPASS_SLOT) != 0;
            }
            constexpr std::pair<int, std::uint8_t> k_gbuffer_passes[] = {
                {Constants::PSO_GBUFFER_FULL_SLOT, Constants::PSO_GBUFFER_FULL_PASS},
                {Constants::PSO_GBUFFER_ATTR_SLOT, Constants::PSO_GBUFFER_ATTR_PASS},
            };
            for (const auto &[slot, pass] : k_gbuffer_passes)
            {
                if ((slots & (1u << slot)) == 0)
                {
                    continue;
                }
                void *stage = pipeline_stage_guarded(pipeline, Constants::PIPELINE_GBUFFER_STAGE_OFFSET);
                std::memcpy(desc + Constants::PSO_DESC_TECHNIQUE_TYPE_OFFSET, &Constants::PSO_TECHNIQUE_GBUFFER,
                            sizeof(Constants::PSO_TECHNIQUE_GBUFFER));
                built = built && stage != nullptr && s_plant_gbuffer_psos(stage, desc, pass, twin.data() + slot) != 0;
            }
            timing.result(built);
            return built;
        }

        /**
         * @brief Character twins change only forward pixel shaders. Keep owned stock PSOs in unchanged stages,
         *        and let the native forward builder replace just its five slots, through its existing local cache.
         * @details build_shader_twins.py proves the other techniques/includes are unchanged. A complete stock build
         *          must succeed first. Any missing anchor/stage or failed reference copy uses the full twin builder.
         *          No pipeline, cache or description is retained past this native callback. Forward starts empty:
         *          a pass the twin omits must never inherit a stock PSO and falsely pass the fade-readiness test.
         */
        [[nodiscard]] std::uint8_t build_character_twin(const ProfiledCreatePsos &original, void *pipeline,
                                                        PsoSet &twin, std::uint8_t *desc, void *cache,
                                                        const std::uintptr_t *stock, bool stock_built,
                                                        bool &reused_stock_stages) noexcept
        {
            reused_stock_stages = false;
            void *stage = s_character_forward_psos != nullptr && stock_built
                              ? pipeline_stage_guarded(pipeline, Constants::PIPELINE_FORWARD_STAGE_OFFSET)
                              : nullptr;
            if (stage != nullptr)
            {
                bool copied = true;
                for (std::size_t i = 0; i < twin.size(); ++i)
                {
                    if (i >= Constants::PSO_FORWARD_FIRST_SLOT &&
                        i < Constants::PSO_FORWARD_FIRST_SLOT + Constants::PSO_STAGE_SLOT_COUNT)
                    {
                        continue;
                    }
                    if (stock[i] != 0 && !add_pso_ref_guarded(stock[i]))
                    {
                        copied = false;
                        break;
                    }
                    twin[i] = stock[i];
                }
                if (copied)
                {
                    constexpr std::int16_t forward_technique = -1;
                    std::memcpy(desc + Constants::PSO_DESC_TECHNIQUE_TYPE_OFFSET, &forward_technique,
                                sizeof(forward_technique));
                    reused_stock_stages = true;
                    HookProfile::EngineScope timing(HookProfile::Path::EnginePsoTwinForward, original.owner);
                    const auto built =
                        s_character_forward_psos(stage, twin.data() + Constants::PSO_FORWARD_FIRST_SLOT, desc, cache);
                    timing.result(built != 0);
                    return built;
                }
                drop_pso_refs(twin);
                twin.fill(0);
            }
            return original(pipeline, twin.data(), desc, cache, 0, HookProfile::Path::EnginePsoTwin);
        }

        /** @brief What a stock/twin pair is built for. */
        enum class PairUse : std::uint8_t
        {
            Plant,        // a plant or frame item: the twin changes the depth and G-buffer passes
            PrepassPlant, // a plant item of a prepass_kind(): its hole only matters in the depth prepass
            Character,    // a character item: the twin changes only forward
        };

        /** @brief Microseconds since @p started, saturated. */
        [[nodiscard]] std::uint32_t micros_since(std::chrono::steady_clock::time_point started) noexcept
        {
            const auto took =
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started);
            return static_cast<std::uint32_t>(std::min<long long>(took.count(), UINT32_MAX));
        }

        /**
         * @brief Builds a stock/twin pair, reusing the stock's unchanged stages for a character or a plant when
         *        verified.
         * @details Both descriptions are copies of the one the engine built for the object with only the shader
         *          replaced, so the resources' local PSO cache keys the two apart. For character pairs the stock goes
         *          first and build_character_twin() replaces only forward when available. For plant pairs, when the
         *          PlantDrawnPsos builders resolved, the stock goes first too and build_plant_twin() builds only the
         *          twin passes the item draws (plant_drawn_slots()); those are the only slots compared and swapped, so
         *          the engine never compiles a twin permutation that cannot reach the screen and the item waits for
         *          none.
         *          Otherwise the twin set goes first, which asks the engine for every permutation it needs with the
         *          object's real RT mask, flags and pipeline state. A written slot has a twin PSO or a changed stock
         *          PSO; character forward slots are always compared, including an omitted twin pass whose stock pointer
         *          did not change.
         * @note Render thread and compile jobs, inside the engine's own call; re-entrant. Never throws.
         */
        [[nodiscard]] PsoPair build_pair(const ProfiledCreatePsos &original, void *pipeline, std::uintptr_t *psos,
                                         const std::uint8_t *desc, void *cache, std::uintptr_t twin_shader,
                                         std::uintptr_t stock_shader, PairUse use) noexcept
        {
            std::array<std::uint8_t, Constants::PSO_DESC_SIZE> twin_desc{};
            std::array<std::uint8_t, Constants::PSO_DESC_SIZE> stock_desc{};
            std::memcpy(twin_desc.data(), desc, twin_desc.size());
            std::memcpy(stock_desc.data(), desc, stock_desc.size());
            std::memcpy(twin_desc.data(), &twin_shader, sizeof(twin_shader));
            std::memcpy(stock_desc.data(), &stock_shader, sizeof(stock_shader));
            PsoSet before{};
            std::memcpy(before.data(), psos, sizeof(before));

            PsoPair pair{};
            std::uint32_t drawn = 0; // the plant passes build_plant_twin() built, if it ran
            // Character twins preserve the stock depth/G-buffer/custom shaders. Build those once, then replace
            // only forward.
            if (use == PairUse::Character)
            {
                pair.stock_built = original(pipeline, psos, stock_desc.data(), cache, 0);
                pair.twin_built = build_character_twin(original, pipeline, pair.twin, twin_desc.data(), cache, psos,
                                                       pair.stock_built != 0, pair.reused_stock_stages) != 0;
            }
            else if (s_plant_prepass_psos != nullptr && s_plant_gbuffer_psos != nullptr)
            {
                pair.stock_built = original(pipeline, psos, stock_desc.data(), cache, 0);
                drawn = plant_drawn_slots(psos, desc, use == PairUse::PrepassPlant);
                const auto started = std::chrono::steady_clock::now();
                pair.twin_built = drawn != 0 ? build_plant_twin(original, pipeline, pair.twin, twin_desc.data(), drawn)
                                             : original(pipeline, pair.twin.data(), twin_desc.data(), cache, 0,
                                                        HookProfile::Path::EnginePsoTwin) != 0;
                pair.twin_micros = micros_since(started);
                pair.reused_stock_stages = drawn != 0;
            }
            else
            {
                const auto started = std::chrono::steady_clock::now();
                pair.twin_built = original(pipeline, pair.twin.data(), twin_desc.data(), cache, 0,
                                           HookProfile::Path::EnginePsoTwin) != 0;
                pair.twin_micros = micros_since(started);
                pair.stock_built = original(pipeline, psos, stock_desc.data(), cache, 0);
            }
            if (drawn != 0)
            {
                pair.slots = drawn;
                return pair;
            }
            for (int i = 0; i < k_pso_slots; ++i)
            {
                const auto slot = static_cast<std::size_t>(i);
                pair.slots |= (pair.twin[slot] != 0 || psos[slot] != before[slot]) ? (1u << i) : 0u;
            }
            if (use == PairUse::Character)
            {
                // A missing twin pass must still be compared when the source build kept the object's existing PSO.
                pair.slots |= ((1u << Constants::PSO_STAGE_SLOT_COUNT) - 1u) << Constants::PSO_FORWARD_FIRST_SLOT;
            }
            return pair;
        }

        /**
         * @brief Puts @p pair's twin set into the object in every slot the build wrote when it can draw every pass the
         *        stock set draws (pso_set_ready_guarded()), and drops the stock references that come out (the local
         *        cache and the factory keep theirs).
         * @return True when the object took the twin set; false leaves it on the stock set and @p pair as it was.
         */
        [[nodiscard]] bool take_ready_twin(std::uintptr_t *psos, PsoPair &pair) noexcept
        {
            if (!pair.twin_built || pso_set_ready_guarded(psos, pair.twin.data(), pair.slots) != PsoReady::Ready)
            {
                return false;
            }
            for (int i = 0; i < k_pso_slots; ++i)
            {
                if ((pair.slots & (1u << i)) != 0)
                {
                    std::swap(psos[i], pair.twin[static_cast<std::size_t>(i)]);
                }
            }
            drop_pso_refs(pair.twin);
            return true;
        }

        /**
         * @brief The record of a build whose object kept the stock set, for the main thread: both sets, with a
         *        reference added on each stock PSO the build wrote (the twin set's hold the build's own).
         */
        [[nodiscard]] PsoRecord hold_record(const std::uintptr_t *psos, const std::uint8_t *desc,
                                            std::uintptr_t twin_shader, std::uintptr_t node,
                                            const PsoPair &pair) noexcept
        {
            PsoRecord record{
                .node = node,
                .object = reinterpret_cast<std::uintptr_t>(psos) - Constants::COMPILED_OBJECT_PSO_OFFSET,
                .twin_shader = twin_shader,
                .rt_mask = *reinterpret_cast<const std::uint64_t *>(desc + Constants::PSO_DESC_RT_MASK_OFFSET),
                .slots = pair.slots,
                .twin_built = pair.twin_built,
                .twin = pair.twin,
            };
            for (int i = 0; i < k_pso_slots; ++i)
            {
                const std::uintptr_t pso = psos[i];
                if ((pair.slots & (1u << i)) != 0 && pso != 0 && add_pso_ref_guarded(pso))
                {
                    record.stock[static_cast<std::size_t>(i)] = pso;
                }
            }
            return record;
        }

        /** @brief Nonblocking admission and completion accounting for one plant twin build. */
        class TwinBuildPermit
        {
        public:
            TwinBuildPermit() noexcept
            {
                if (s_budget_in_flight.test_and_set(std::memory_order_acquire))
                {
                    s_budget_busy_deferrals.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                const std::uint32_t frame = s_frame_seq.load(std::memory_order_relaxed);
                if (s_budget_frame != frame)
                {
                    s_budget_frame = frame;
                    s_budget_used_us = 0;
                }
                if (s_budget_used_us >= Constants::TREE_HOLE_TWIN_BUILD_BUDGET_US)
                {
                    s_budget_in_flight.clear(std::memory_order_release);
                    return;
                }
                m_owned = true;
            }

            // An early exit must release admission and conservatively spend the allowance when no cost was supplied.
            ~TwinBuildPermit() { complete(Constants::TREE_HOLE_TWIN_BUILD_BUDGET_US); }
            TwinBuildPermit(const TwinBuildPermit &) = delete;
            TwinBuildPermit &operator=(const TwinBuildPermit &) = delete;
            TwinBuildPermit(TwinBuildPermit &&) = delete;
            TwinBuildPermit &operator=(TwinBuildPermit &&) = delete;

            [[nodiscard]] explicit operator bool() const noexcept { return m_owned; }

            void complete(std::uint32_t micros) noexcept
            {
                if (!m_owned)
                {
                    return;
                }
                const std::uint32_t frame = s_frame_seq.load(std::memory_order_relaxed);
                if (s_budget_frame != frame)
                {
                    s_budget_frame = frame;
                    s_budget_used_us = 0;
                    s_budget_carryovers.fetch_add(1, std::memory_order_relaxed);
                }
                // Only the threshold matters. Saturation prevents a long call from wrapping the allowance open.
                const std::uint32_t remaining = Constants::TREE_HOLE_TWIN_BUILD_BUDGET_US - s_budget_used_us;
                s_budget_used_us += std::min(micros, remaining);
                m_owned = false;
                s_budget_in_flight.clear(std::memory_order_release);
            }

        private:
            bool m_owned = false;
        };

        /**
         * @brief Builds a compiled object that names a plant twin with its source shader only, as the engine would have
         *        without the swap, and hands its tree to the main thread for a refill TREE_HOLE_DEFER_RETRY_MS later.
         * @details The frame's twin-build budget is spent, so the twin build, which can load a permutation for tens of
         *          milliseconds, waits for a later frame instead of stacking onto this one. The object draws its stock
         *          look until that refill. A temporary render object is built again next frame by itself. A queue too
         *          full to take the tree schedules a retry for every swapped tree instead.
         * @return The stock build's result.
         * @note Render thread and compile jobs, inside the engine's own call; re-entrant. Never throws.
         */
        [[nodiscard]] std::uint8_t build_deferred_psos(const ProfiledCreatePsos &original, void *pipeline,
                                                       std::uintptr_t *psos, const std::uint8_t *desc, void *cache,
                                                       std::uintptr_t source) noexcept
        {
            trace_psos(trace::Event::BuildDeferred, psos, desc);
            std::array<std::uint8_t, Constants::PSO_DESC_SIZE> stock_desc{};
            std::memcpy(stock_desc.data(), desc, stock_desc.size());
            std::memcpy(stock_desc.data(), &source, sizeof(source));
            const std::uint8_t built = original(pipeline, psos, stock_desc.data(), cache, 0);
            s_pso_deferred.fetch_add(1, std::memory_order_relaxed);
            const std::uintptr_t node = persistent_node_of_psos_guarded(psos);
            if (node != 0 && s_available.load(std::memory_order_acquire) && !s_deferred_nodes.push(node))
            {
                trace_psos(trace::Event::RecordLost, psos, desc, 1);
                s_pso_record_lost.store(true, std::memory_order_relaxed);
            }
            return built;
        }

        /**
         * @brief Builds the PSOs of a compiled object that names a plant twin twice (build_pair()), and keeps the
         *        twin's only once they can draw every pass the stock's draw.
         * @details The twin's description is the one the engine built for the object, and the stock's the same with
         *          the source shader. When the twin set is ready it takes the place of the stock set
         *          (take_ready_twin()). Otherwise the object keeps the stock set and draws as stock, and both sets go
         *          to the main thread with a reference held on each PSO, so it can watch them and refill the tree when
         *          the twin's are valid. Once the frame's twin builds took TREE_HOLE_TWIN_BUILD_BUDGET_US, the object
         *          is built with its source shader only and its tree refilled shortly (build_deferred_psos()).
         * @return The result of the build whose set the object keeps; with only the drawn twin passes built, the stock
         *         build's, whose PSOs fill every other slot.
         * @note Render thread and compile jobs, inside the engine's own call; re-entrant. Never throws.
         */
        [[nodiscard]] std::uint8_t build_guarded_psos(const ProfiledCreatePsos &original, void *pipeline,
                                                      std::uintptr_t *psos, const std::uint8_t *desc, void *cache,
                                                      std::uintptr_t source, bool prepass) noexcept
        {
            DMK_PROFILE_SCOPE("twins.tree_pso_build");
            TwinBuildPermit budget;
            if (!budget)
            {
                return build_deferred_psos(original, pipeline, psos, desc, cache, source);
            }
            const std::uintptr_t twin_shader = *reinterpret_cast<const std::uintptr_t *>(desc);
            trace_psos(trace::Event::BuildBegin, psos, desc);
            PsoPair pair = build_pair(original, pipeline, psos, desc, cache, twin_shader, source,
                                      prepass ? PairUse::PrepassPlant : PairUse::Plant);
            budget.complete(pair.twin_micros);
            if (pair.reused_stock_stages)
            {
                (pair.slots == 1u << Constants::PSO_ZPREPASS_SLOT ? s_pso_prepass_only : s_pso_gbuffer_only)
                    .fetch_add(1, std::memory_order_relaxed);
            }
            trace_psos(trace::Event::BuildEnd, psos, desc, pair.twin_micros,
                       static_cast<std::uint64_t>(pair.slots) | (static_cast<std::uint64_t>(pair.twin_built) << 32) |
                           (static_cast<std::uint64_t>(pair.stock_built) << 40));
            note_build_time(pair.twin_micros);
            s_pso_guarded.fetch_add(1, std::memory_order_relaxed);
            if (take_ready_twin(psos, pair))
            {
                trace_psos(trace::Event::PsoSelectedTwin, psos, desc, pair.slots);
                s_pso_took_twin.fetch_add(1, std::memory_order_relaxed);
                // With only the drawn passes built for the twin, every other slot holds the stock build's PSO, so the
                // stock build's result says whether every stage built (as in build_fading_psos()).
                return pair.reused_stock_stages ? pair.stock_built : 1;
            }
            s_pso_kept_stock.fetch_add(1, std::memory_order_relaxed);
            trace_psos(trace::Event::PsoSelectedStock, psos, desc, pair.slots);

            // Nothing would wait on the record (a temporary render object, which is built again next frame, or the
            // module is shutting down): the object keeps the stock set and the twin references go now. A record pushed
            // while the shutdown runs is dropped once the hooks have retired (release_shader_twin_records()).
            const std::uintptr_t node = persistent_node_of_psos_guarded(psos);
            if (node == 0 || !s_available.load(std::memory_order_acquire))
            {
                trace_psos(trace::Event::RecordDropped, psos, desc, node == 0 ? 1 : 2);
                s_pso_temporary.fetch_add(node == 0 ? 1 : 0, std::memory_order_relaxed);
                drop_pso_refs(pair.twin);
                return pair.stock_built;
            }
            const PsoRecord record = hold_record(psos, desc, twin_shader, node, pair);
            if (!s_pso_records.push(record))
            {
                trace_psos(trace::Event::RecordLost, psos, desc, 0);
                s_pso_dropped.fetch_add(1, std::memory_order_relaxed);
                s_pso_record_lost.store(true, std::memory_order_relaxed);
                drop_pso_refs(record.stock);
                drop_pso_refs(record.twin);
            }
            else
            {
                trace_psos(trace::Event::RecordQueued, psos, desc, record.slots);
            }
            return pair.stock_built;
        }

        /** @brief The warm-up entry whose twin is @p shader (a ready character twin, fade-warm or not), or -1. */
        [[nodiscard]] int character_twin_slot(std::uintptr_t shader) noexcept
        {
            const int count = std::min(s_warm_count.load(std::memory_order_acquire), k_max_sources);
            for (int i = 0; i < count; ++i)
            {
                if (s_warm_twins[static_cast<std::size_t>(i)].load(std::memory_order_relaxed) == shader)
                {
                    return i;
                }
            }
            return -1;
        }

        /** @brief The warm-up entry whose source is @p shader while its twin is not warm for the fade, or -1. */
        [[nodiscard]] int warming_slot(std::uintptr_t shader) noexcept
        {
            const int count = std::min(s_warm_count.load(std::memory_order_acquire), k_max_sources);
            for (int i = 0; i < count; ++i)
            {
                const auto slot = static_cast<std::size_t>(i);
                if (s_warm_sources[slot].load(std::memory_order_relaxed) == shader)
                {
                    return s_warm_done[slot].load(std::memory_order_acquire) ? -1 : i;
                }
            }
            return -1;
        }

        /**
         * @brief True when the compiled object whose PSO array is @p psos belongs to a temporary render object that
         *        dissolves (dissolving_object()) and names one of @p set's character nodes, so no other character's
         *        LOD dissolve builds a twin set aside.
         * @note Render thread. SEH-guarded, POD body.
         */
        [[nodiscard]] bool dissolving_psos_guarded(const std::uintptr_t *psos, const SwapSet &set) noexcept
        {
            __try
            {
                const std::uintptr_t object =
                    reinterpret_cast<std::uintptr_t>(psos) - Constants::COMPILED_OBJECT_PSO_OFFSET;
                const std::uintptr_t render_object =
                    *reinterpret_cast<const std::uintptr_t *>(object + Constants::COMPILED_OBJECT_RENDER_OBJECT_OFFSET);
                if (render_object == 0)
                {
                    return false;
                }
                const auto *ro = reinterpret_cast<const std::uint8_t *>(render_object);
                return dissolving_object(ro) && character_object(set, ro);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        /**
         * @brief The key of a character twin set: a 64-bit FNV-1a over @p desc with @p shader in its shader field,
         *        with the top bit set so it is never k_key_empty or k_key_removed.
         * @details Two builds that would hand CreatePipelineStates the same description get the same key, which is
         *          also what the resources' local PSO cache keys a set by.
         */
        [[nodiscard]] std::uint64_t desc_key(const std::uint8_t *desc, std::uintptr_t shader) noexcept
        {
            std::uint64_t hash = 0xCBF29CE484222325ull;
            const auto mix = [&hash](std::uint8_t byte)
            {
                hash ^= byte;
                hash *= 0x100000001B3ull;
            };
            for (std::size_t i = 0; i < sizeof(shader); ++i)
            {
                mix(static_cast<std::uint8_t>(shader >> (8 * i)));
            }
            for (std::size_t i = sizeof(shader); i < Constants::PSO_DESC_SIZE; ++i)
            {
                mix(desc[i]);
            }
            return hash | (1ull << 63);
        }

        /** @brief True when a set of @p key was built ahead or aside (s_warm_keys). Any thread. */
        [[nodiscard]] bool key_known(std::uint64_t key) noexcept
        {
            constexpr std::size_t mask = Constants::CHARACTER_WARM_KEY_SLOTS - 1;
            std::size_t at = static_cast<std::size_t>(key) & mask;
            for (std::size_t probe = 0; probe < Constants::CHARACTER_WARM_KEY_PROBES; ++probe, at = (at + 1) & mask)
            {
                const std::uint64_t seen = s_warm_keys[at].load(std::memory_order_acquire);
                if (seen == key)
                {
                    return true;
                }
                if (seen == k_key_empty)
                {
                    return false;
                }
            }
            return false;
        }

        /**
         * @brief The character's own render node the published swap set names (the first of its character nodes), or
         *        0, for the record of a character twin set (PsoRecord::node). Render threads and compile jobs.
         */
        [[nodiscard]] std::uintptr_t published_character_root() noexcept
        {
            const SwapSet *set = s_set_published.load(std::memory_order_acquire);
            return (set != nullptr && set->character_count > 0) ? set->character_nodes[0] : 0;
        }

        /**
         * @brief True when a dissolving build of the stock shader of the warm-up entry @p slot may build its twin's set
         *        aside: always, or for a transparent-only kind (IllumFade) only when the description's resources read
         *        as transparent (item_opacity_guarded() below 1), the test twin_item() makes, so an opaque Illum item
         *        of the character (his clothes) that shares the eye film's shader instance never builds one.
         */
        [[nodiscard]] bool warming_item_fits(int slot, const std::uint8_t *desc) noexcept
        {
            return !s_warm_transparent_only[static_cast<std::size_t>(slot)].load(std::memory_order_relaxed) ||
                   item_opacity_guarded(
                       static_cast<std::uintptr_t>(desc_field(desc, Constants::PSO_DESC_RESOURCES_OFFSET))) < 1.0f;
        }

        /**
         * @brief Hands a character twin set to the main thread with the references it holds, reading its stock PSOs'
         *        attempt counts first; drops them while the module shuts down (a record pushed while the shutdown runs
         *        is dropped once the hooks have retired) or when the queue is full, and then frees its key so a later
         *        build can make the set again.
         */
        void hand_over_warm_record(PsoRecord &record) noexcept
        {
            if (s_available.load(std::memory_order_acquire))
            {
                read_attempts_guarded(record.stock.data(), record.slots, record.attempts.data());
                if (s_warm_records.push(record))
                {
                    return;
                }
                s_warm_records_dropped.fetch_add(1, std::memory_order_relaxed);
            }
            drop_pso_refs(record.stock);
            drop_pso_refs(record.twin);
            if (record.origin != WarmOrigin::Cold)
            {
                release_key(record.key);
            }
        }

        /**
         * @brief Builds a dissolving character item's stock PSOs into its compiled object as usual, and its twin's set
         *        aside (build_pair()), so the engine compiles the twin's permutations while the item keeps its stock
         *        look; nothing aside when that set was built before.
         * @details This is the stock shader's build while the twin is not warm for the fade (the close-up fade started
         *          before every set built ahead could draw, or a set no build made ahead). The object keeps the stock
         *          set. Both sets go to the main thread; once every set of that twin can draw, the twin is warm, the
         *          swap set carries it for dissolving items too, and the item's next build (the swap changes its key)
         *          finds the twin's PSOs in the caches.
         * @param slot The twin's warm-up entry.
         * @param root The character's own render node the swap set names, for the record.
         * @return The stock build's result.
         * @note Render thread and compile jobs, inside the engine's own call; re-entrant. Never throws.
         */
        [[nodiscard]] std::uint8_t build_warming_psos(const ProfiledCreatePsos &original, void *pipeline,
                                                      std::uintptr_t *psos, std::uint8_t *desc, void *cache, int slot,
                                                      std::uintptr_t root) noexcept
        {
            DMK_PROFILE_SCOPE("twins.warming_build");
            const std::int64_t build_started = HookProfile::stamp();
            const std::uintptr_t twin_shader =
                s_warm_twins[static_cast<std::size_t>(slot)].load(std::memory_order_relaxed);
            const std::uintptr_t source = *reinterpret_cast<const std::uintptr_t *>(desc);
            const std::uint64_t key = desc_key(desc, twin_shader);
            if (claim_key(key) != Claim::New)
            {
                return original(pipeline, psos, desc, cache, 0);
            }
            const PsoPair pair =
                build_pair(original, pipeline, psos, desc, cache, twin_shader, source, PairUse::Character);
            HookProfile::count_slower_than(HookProfile::Counter::CharBuildOverBudget, build_started,
                                           Constants::TREE_HOLE_TWIN_BUILD_BUDGET_US);
            s_warming_builds.fetch_add(1, std::memory_order_relaxed);
            PsoRecord record = hold_record(psos, desc, twin_shader, root, pair);
            record.origin = WarmOrigin::Warming;
            record.key = key;
            record.flags = desc_field(desc, Constants::PSO_DESC_OBJECT_FLAGS_OFFSET);
            hand_over_warm_record(record);
            return pair.stock_built;
        }

        /**
         * @brief Builds a character twin item that does not dissolve: the stock set into the object, and first, aside,
         *        the twin's and the stock's sets for the same item as the close-up fade will ask for them.
         * @details The twin draws exactly like its stock shader while nothing dissolves, so the object keeps the stock
         *          set, which is compiled, and the twin's own permutations for an item at rest are never asked for.
         *          The fade's description is this one with FOB_DISSOLVE and FOB_DISSOLVE_OUT in the flags and the
         *          dissolve bit in the RT mask, exactly what the fading build of this pooled object will hand
         *          CreatePipelineStates (see Constants::PSO_DESC_SIZE). Building both sets for it into local arrays
         *          asks the engine to compile the twin's dissolving permutations now, in the background, and puts them
         *          in the resources' local PSO cache under that description. The sets go to the main thread, which
         *          holds them until they can draw. A description built before (s_warm_keys) is not built again.
         * @param slot The twin's warm-up entry.
         * @param root The character's own render node the swap set names, for the record.
         * @return The stock build's result.
         * @note Render thread and compile jobs, inside the engine's own call; re-entrant. Never throws.
         */
        [[nodiscard]] std::uint8_t build_resting_psos(const ProfiledCreatePsos &original, void *pipeline,
                                                      std::uintptr_t *psos, const std::uint8_t *desc, void *cache,
                                                      int slot, std::uintptr_t root) noexcept
        {
            s_rest_builds.fetch_add(1, std::memory_order_relaxed);
            const std::uintptr_t twin_shader = *reinterpret_cast<const std::uintptr_t *>(desc);
            const std::uintptr_t source =
                s_warm_sources[static_cast<std::size_t>(slot)].load(std::memory_order_relaxed);
            const std::uint64_t rest_rt_mask = desc_field(desc, Constants::PSO_DESC_RT_MASK_OFFSET);
            const std::uint64_t rest_flags = desc_field(desc, Constants::PSO_DESC_OBJECT_FLAGS_OFFSET);
            const std::uint64_t fade_rt_mask = rest_rt_mask | s_dissolve_rt_bit.load(std::memory_order_relaxed);
            const std::uint64_t fade_flags =
                rest_flags | Constants::RENDER_OBJECT_FLAG_DISSOLVE | Constants::RENDER_OBJECT_FLAG_DISSOLVE_OUT;
            std::array<std::uint8_t, Constants::PSO_DESC_SIZE> twin_desc{};
            std::memcpy(twin_desc.data(), desc, twin_desc.size());
            std::memcpy(twin_desc.data() + Constants::PSO_DESC_RT_MASK_OFFSET, &fade_rt_mask, sizeof(fade_rt_mask));
            std::memcpy(twin_desc.data() + Constants::PSO_DESC_OBJECT_FLAGS_OFFSET, &fade_flags, sizeof(fade_flags));
            const std::uint64_t key = desc_key(twin_desc.data(), twin_shader);
            switch (claim_key(key))
            {
            case Claim::New:
            {
                DMK_PROFILE_SCOPE("twins.fade_ahead_build");
                const std::int64_t build_started = HookProfile::stamp();
                std::array<std::uint8_t, Constants::PSO_DESC_SIZE> stock_desc = twin_desc;
                std::memcpy(stock_desc.data(), &source, sizeof(source));
                PsoRecord record{
                    .node = root,
                    .twin_shader = twin_shader,
                    .rt_mask = fade_rt_mask,
                    .origin = WarmOrigin::Ahead,
                    .key = key,
                    .flags = fade_flags,
                    .rest_rt_mask = rest_rt_mask,
                    .rest_flags = rest_flags,
                };
                const bool stock_built = original(pipeline, record.stock.data(), stock_desc.data(), cache, 0) != 0;
                bool reused_stock_stages = false;
                record.twin_built = build_character_twin(original, pipeline, record.twin, twin_desc.data(), cache,
                                                         record.stock.data(), stock_built, reused_stock_stages) != 0;
                for (int i = 0; i < k_pso_slots; ++i)
                {
                    const auto at = static_cast<std::size_t>(i);
                    record.slots |= (record.twin[at] != 0 || record.stock[at] != 0) ? (1u << i) : 0u;
                }
                HookProfile::count_slower_than(HookProfile::Counter::CharBuildOverBudget, build_started,
                                               Constants::TREE_HOLE_TWIN_BUILD_BUDGET_US);
                s_ahead_builds.fetch_add(1, std::memory_order_relaxed);
                hand_over_warm_record(record);
                break;
            }
            case Claim::Known:
                s_ahead_known.fetch_add(1, std::memory_order_relaxed);
                break;
            case Claim::Full:
            default:
                s_ahead_full.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            std::array<std::uint8_t, Constants::PSO_DESC_SIZE> stock_desc{};
            std::memcpy(stock_desc.data(), desc, stock_desc.size());
            std::memcpy(stock_desc.data(), &source, sizeof(source));
            return original(pipeline, psos, stock_desc.data(), cache, 0);
        }

        /** @brief Keeps the masks of the first fading build whose description no set was built ahead for, for a log. */
        void note_fade_miss(const std::uint8_t *desc) noexcept
        {
            if (!s_fade_miss_taken.exchange(true, std::memory_order_acq_rel))
            {
                s_fade_miss_rt_mask.store(desc_field(desc, Constants::PSO_DESC_RT_MASK_OFFSET),
                                          std::memory_order_relaxed);
                s_fade_miss_flags.store(desc_field(desc, Constants::PSO_DESC_OBJECT_FLAGS_OFFSET),
                                        std::memory_order_relaxed);
                s_fade_miss_ready.store(true, std::memory_order_release);
            }
        }

        /**
         * @brief Builds a dissolving item that names a character twin like build_guarded_psos(): the twin's set aside
         *        and its source's into the object, and the twin's taken only when it can draw every pass the source's
         *        draws.
         * @details This is the fading build the sets built ahead prepared: its description is the one they were built
         *          for, so the twin's PSOs come from the resources' local PSO cache, compiled, and it takes them at
         *          once (counted, with whether its key was built ahead, for the log). A build can still ask for a
         *          permutation no set prepared (an RT mask the item never had at rest, a quality change), and an
         *          invalid PSO would leave the eyes or the hair undrawn while it compiles. Such a build keeps the
         *          source's PSOs, so the item draws solid but never vanishes, sets the entry's cold flag, and hands
         *          both sets to the main thread, which turns the twin cold: its items go back to the stock shader for
         *          the fade (their pooled compiled objects rebuild, since the shader changed), the close-up fade hides
         *          the head again, and the warm-up holds this set until it can draw.
         * @param slot The twin's warm-up entry.
         * @param root The character's own render node the swap set names, for the record.
         * @return The result of the build whose set the object keeps.
         * @note Render thread and compile jobs, inside the engine's own call; re-entrant. Never throws.
         */
        [[nodiscard]] std::uint8_t build_fading_psos(const ProfiledCreatePsos &original, void *pipeline,
                                                     std::uintptr_t *psos, const std::uint8_t *desc, void *cache,
                                                     int slot, std::uintptr_t root) noexcept
        {
            DMK_PROFILE_SCOPE("twins.fading_build");
            const auto entry = static_cast<std::size_t>(slot);
            const std::uintptr_t twin_shader = *reinterpret_cast<const std::uintptr_t *>(desc);
            const std::uintptr_t source = s_warm_sources[entry].load(std::memory_order_relaxed);
            const std::uint64_t key = desc_key(desc, twin_shader);
            const bool ahead = key_known(key);
            if (!ahead)
            {
                note_fade_miss(desc);
            }
            PsoPair pair = build_pair(original, pipeline, psos, desc, cache, twin_shader, source, PairUse::Character);
            if (take_ready_twin(psos, pair))
            {
                s_fade_warm_builds.fetch_add(1, std::memory_order_relaxed);
                s_fade_warm_ahead.fetch_add(ahead ? 1 : 0, std::memory_order_relaxed);
                return pair.reused_stock_stages ? pair.stock_built : 1;
            }
            s_fade_cold_builds.fetch_add(1, std::memory_order_relaxed);
            s_warm_cold[entry].store(true, std::memory_order_release);
            PsoRecord record = hold_record(psos, desc, twin_shader, root, pair);
            record.origin = WarmOrigin::Cold;
            record.key = key;
            record.flags = desc_field(desc, Constants::PSO_DESC_OBJECT_FLAGS_OFFSET);
            hand_over_warm_record(record);
            return pair.stock_built;
        }
    } // namespace

    void drop_pso_refs(const PsoSet &set) noexcept
    {
        for (const std::uintptr_t pso : set)
        {
            if (pso != 0 && !drop_pso_ref_guarded(pso))
            {
                s_pso_leaked.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    [[nodiscard]] PsoReady pso_set_ready_guarded(const std::uintptr_t *stock, const std::uintptr_t *twin,
                                                 std::uint32_t slots) noexcept
    {
        __try
        {
            int drawn = 0;
            for (int i = 0; i < k_pso_slots; ++i)
            {
                if ((slots & (1u << i)) == 0 || stock[i] == 0)
                {
                    continue;
                }
                if (*reinterpret_cast<const volatile std::uint8_t *>(stock[i] + Constants::PSO_VALID_OFFSET) == 0)
                {
                    const std::uint32_t state =
                        *reinterpret_cast<const volatile std::uint32_t *>(stock[i] + Constants::PSO_STATE_OFFSET);
                    if ((state & Constants::PSO_STATE_QUEUED) != 0)
                    {
                        return PsoReady::Waiting;
                    }
                    continue;
                }
                ++drawn;
                if (twin[i] == 0 ||
                    *reinterpret_cast<const volatile std::uint8_t *>(twin[i] + Constants::PSO_VALID_OFFSET) == 0)
                {
                    return PsoReady::Waiting;
                }
            }
            return drawn > 0 ? PsoReady::Ready : PsoReady::Waiting;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return PsoReady::Fault;
        }
    }

#if defined(TPVCAMERA_DEV_BUILD) && defined(DMK_ENABLE_PROFILING)

    [[nodiscard]] PsoObservation observe_pso_guarded(std::uintptr_t pso) noexcept
    {
        PsoObservation result{};
        if (pso == 0)
            return result;
        __try
        {
            result.valid = *reinterpret_cast<const volatile std::uint8_t *>(pso + Constants::PSO_VALID_OFFSET) != 0;
            result.state = *reinterpret_cast<const volatile std::uint32_t *>(pso + Constants::PSO_STATE_OFFSET);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            result = {.fault = true};
        }
        return result;
    }

    void trace_pending_psos(PendingPsos &pending, std::uint64_t now) noexcept
    {
        const bool progress_due = pending.traced_slots == 0 || now - pending.traced_attempts_ms >= 100;
        const auto &record = pending.record;
        for (int i = 0; i < k_pso_slots; ++i)
        {
            const auto bit = 1u << i;
            if ((record.slots & bit) == 0)
                continue;
            const auto slot = static_cast<std::size_t>(i);
            const auto stock = observe_pso_guarded(record.stock[slot]);
            const auto twin = observe_pso_guarded(record.twin[slot]);
            const auto states = stock.state | (static_cast<std::uint64_t>(twin.state) << 32);
            const auto flags = static_cast<std::uint8_t>((stock.valid ? 1u : 0u) | (twin.valid ? 2u : 0u) |
                                                         (stock.fault ? 4u : 0u) | (twin.fault ? 8u : 0u));
            const bool first = (pending.traced_slots & bit) == 0;
            const auto changed_states = states ^ pending.traced_pso_states[slot];
            constexpr auto requested_bits =
                std::uint64_t{Constants::PSO_STATE_QUEUED} | (std::uint64_t{Constants::PSO_STATE_QUEUED} << 32);
            if (first || flags != pending.traced_pso_flags[slot] || (changed_states & requested_bits) != 0 ||
                (progress_due && changed_states != 0))
            {
                trace_event(trace::Event::PsoSlotState, record.node, record.object, record.stock[slot],
                            record.twin[slot], slot | (std::uint64_t{flags} << 8), states);
                pending.traced_slots |= bit;
                pending.traced_pso_states[slot] = states;
                pending.traced_pso_flags[slot] = flags;
            }
        }
        if (progress_due)
            pending.traced_attempts_ms = now;
    }
#endif

    [[nodiscard]] PsoReady warm_set_ready_guarded(const std::uintptr_t *stock, const std::uintptr_t *twin,
                                                  std::uint32_t slots, std::uint32_t *attempts,
                                                  bool &stock_idle) noexcept
    {
        __try
        {
            int drawn = 0;
            bool waiting = false;
            stock_idle = false;
            for (int i = 0; i < k_pso_slots; ++i)
            {
                if ((slots & (1u << i)) == 0 || stock[i] == 0)
                {
                    continue;
                }
                if (*reinterpret_cast<const volatile std::uint8_t *>(stock[i] + Constants::PSO_VALID_OFFSET) == 0)
                {
                    const std::uint32_t state =
                        *reinterpret_cast<const volatile std::uint32_t *>(stock[i] + Constants::PSO_STATE_OFFSET);
                    const std::uint32_t count = state & Constants::PSO_STATE_ATTEMPTS_MASK;
                    const bool moving = (state & Constants::PSO_STATE_QUEUED) != 0 || count != attempts[i];
                    waiting = waiting || moving;
                    stock_idle = stock_idle || !moving;
                    attempts[i] = count;
                    continue;
                }
                ++drawn;
                waiting = waiting || twin[i] == 0 ||
                          *reinterpret_cast<const volatile std::uint8_t *>(twin[i] + Constants::PSO_VALID_OFFSET) == 0;
            }
            return (drawn > 0 && !waiting) ? PsoReady::Ready : PsoReady::Waiting;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return PsoReady::Fault;
        }
    }

    [[nodiscard]] bool twin_stalled_guarded(const std::uintptr_t *stock, const std::uintptr_t *twin,
                                            std::uint32_t slots, std::uint32_t *attempts) noexcept
    {
        __try
        {
            bool stalled = false;
            for (int i = 0; i < k_pso_slots; ++i)
            {
                if ((slots & (1u << i)) == 0 || stock[i] == 0 ||
                    *reinterpret_cast<const volatile std::uint8_t *>(stock[i] + Constants::PSO_VALID_OFFSET) == 0)
                {
                    continue;
                }
                if (twin[i] == 0)
                {
                    stalled = true;
                    continue;
                }
                if (*reinterpret_cast<const volatile std::uint8_t *>(twin[i] + Constants::PSO_VALID_OFFSET) != 0)
                {
                    continue;
                }
                const std::uint32_t state =
                    *reinterpret_cast<const volatile std::uint32_t *>(twin[i] + Constants::PSO_STATE_OFFSET);
                const std::uint32_t count = state & Constants::PSO_STATE_ATTEMPTS_MASK;
                stalled = stalled || ((state & Constants::PSO_STATE_QUEUED) == 0 && count == attempts[i]);
                attempts[i] = count;
            }
            return stalled;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    [[nodiscard]] Claim claim_key(std::uint64_t key) noexcept
    {
        constexpr std::size_t mask = Constants::CHARACTER_WARM_KEY_SLOTS - 1;
        std::size_t at = static_cast<std::size_t>(key) & mask;
        for (std::size_t probe = 0; probe < Constants::CHARACTER_WARM_KEY_PROBES; ++probe, at = (at + 1) & mask)
        {
            std::uint64_t seen = s_warm_keys[at].load(std::memory_order_acquire);
            if (seen == key)
            {
                return Claim::Known;
            }
            if (seen == k_key_empty)
            {
                if (s_warm_keys[at].compare_exchange_strong(seen, key, std::memory_order_acq_rel))
                {
                    return Claim::New;
                }
                if (seen == key)
                {
                    return Claim::Known;
                }
            }
        }
        return Claim::Full;
    }

    void release_key(std::uint64_t key) noexcept
    {
        constexpr std::size_t mask = Constants::CHARACTER_WARM_KEY_SLOTS - 1;
        std::size_t at = static_cast<std::size_t>(key) & mask;
        for (std::size_t probe = 0; probe < Constants::CHARACTER_WARM_KEY_PROBES; ++probe, at = (at + 1) & mask)
        {
            std::uint64_t seen = key;
            if (s_warm_keys[at].compare_exchange_strong(seen, k_key_removed, std::memory_order_acq_rel) ||
                seen == k_key_empty)
            {
                return;
            }
        }
    }

    std::uint8_t __fastcall create_psos_detour(void *pipeline, std::uintptr_t *psos, std::uint8_t *desc, void *cache,
                                               int is_shadow) noexcept
    {
        const DetourScope in_flight;
        HookProfile::Scope timing(HookProfile::Path::CreatePsos);
        const ProfiledCreatePsos original{s_create_psos_original.load(std::memory_order_acquire), timing};
        if (is_shadow != 0)
        {
            timing.outcome(HookProfile::Outcome::Shadow);
        }
        if (is_shadow == 0 && psos != nullptr && desc != nullptr)
        {
            const std::uintptr_t shader = *reinterpret_cast<const std::uintptr_t *>(desc);
            if (s_guard_count.load(std::memory_order_acquire) > 0)
            {
                bool prepass = false;
                const std::uintptr_t source = source_of_twin(shader, &prepass);
                if (source != 0)
                {
                    timing.outcome(HookProfile::Outcome::Guarded);
                    trace_pso_opportunity(pipeline, psos, desc, cache, source);
                    return build_guarded_psos(original, pipeline, psos, desc, cache, source, prepass);
                }
            }
            if (s_warm_count.load(std::memory_order_acquire) > 0)
            {
                if (const int slot = character_twin_slot(shader); slot >= 0)
                {
                    const bool fading = (desc_field(desc, Constants::PSO_DESC_OBJECT_FLAGS_OFFSET) &
                                         Constants::RENDER_OBJECT_FLAG_DISSOLVE) != 0;
                    const std::uintptr_t root = published_character_root();
                    timing.outcome(fading ? HookProfile::Outcome::Fading : HookProfile::Outcome::Resting);
                    return fading ? build_fading_psos(original, pipeline, psos, desc, cache, slot, root)
                                  : build_resting_psos(original, pipeline, psos, desc, cache, slot, root);
                }
                const int slot = warming_slot(shader);
                const SwapSet *set = slot >= 0 ? s_set_published.load(std::memory_order_acquire) : nullptr;
                if (set != nullptr && set->character_count > 0 && dissolving_psos_guarded(psos, *set) &&
                    warming_item_fits(slot, desc))
                {
                    timing.outcome(HookProfile::Outcome::Warming);
                    return build_warming_psos(original, pipeline, psos, desc, cache, slot, set->character_nodes[0]);
                }
            }
            trace_pso_opportunity(pipeline, psos, desc, cache);
        }
        return original(pipeline, psos, desc, cache, is_shadow, HookProfile::Path::EnginePsoPassthrough);
    }
} // namespace TPVCamera::Twins
