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

#include "shader_twins.hpp"
#include "twin_inventory.hpp"
#if defined(TPVCAMERA_DEV_BUILD)
#include "twin_clone.hpp"
#endif
#include "aob_resolver.hpp"
#include "camera_rules_files.hpp"
#include "constants.hpp"
#include "dev/hook_profile.hpp"
#include "dev/see_through_trace.hpp"
#include "dissolve.hpp"
#include "generated/shader_twin_blobs.hpp"
#include "global_state.hpp"
#include "render_occlusion.hpp"
#include "rtti_types.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace TPVCamera
{

    namespace
    {
        namespace blobs = shader_twin_blobs;
        namespace trace = SeeThroughTrace;

        // The .ext loader (shader manager, SShaderGen** out, name), which returns out.
        using ShaderGenLoadFn = void *(__fastcall *)(void *manager, void *out, const char *name);
        // CShaderMan::mfForName(manager, name, flags, resources, gen mask) -> CShader*, with a reference added.
        using ForNameFn = std::uintptr_t(__fastcall *)(std::uintptr_t manager, const char *name, std::uint32_t flags,
                                                       std::uintptr_t resources, std::uint64_t mask);
        using AdjustFileNameFn = const char *(__fastcall *)(std::uintptr_t pak, const char *source, char *destination,
                                                            std::uint32_t flags);
        using PakOpenFn = void *(__fastcall *)(std::uintptr_t pak, const char *name, const char *mode,
                                               std::uint32_t flags);
        using PakReadRawFn = std::size_t(__fastcall *)(std::uintptr_t pak, void *data, std::size_t length,
                                                       std::size_t count, void *file);
        using PakCloseFn = int(__fastcall *)(std::uintptr_t pak, void *file);

        // CreatePipelineStates(pipeline, the compiled object's PSO array, the description, the local PSO cache,
        // shadow flag) -> true when every stage built its PSOs.
        using CreatePsosFn = std::uint8_t(__fastcall *)(void *pipeline, std::uintptr_t *psos, std::uint8_t *desc,
                                                        void *cache, int is_shadow);
        using ForwardPsosFn = std::uint8_t(__fastcall *)(void *stage, std::uintptr_t *psos, std::uint8_t *desc,
                                                         void *cache);
        // The depth-prepass builder (stage, description, its slot) and the G-buffer pass builder (stage, description,
        // pass, its slot) CreatePipelineStates calls. Each releases the slot's PSO, puts a referenced one there, and
        // returns true when it built one or the pass has nothing to draw (the slot then stays empty).
        using PrepassPsosFn = std::uint8_t(__fastcall *)(void *stage, std::uint8_t *desc, std::uintptr_t *pso);
        using GBufferPsosFn = std::uint8_t(__fastcall *)(void *stage, std::uint8_t *desc, std::uint8_t pass,
                                                         std::uintptr_t *pso);
        struct ProfiledCreatePsos
        {
            CreatePsosFn original;
            HookProfile::Scope &owner;

            std::uint8_t operator()(void *pipeline, std::uintptr_t *psos, std::uint8_t *desc, void *cache, int shadow,
                                    HookProfile::Path path = HookProfile::Path::EnginePsoStock) const noexcept
            {
                HookProfile::EngineScope timing(path, owner);
                const auto result = original(pipeline, psos, desc, cache, shadow);
                timing.result(result != 0);
                return result;
            }
        };
        using SetSystemGlobalStateFn = void(__fastcall *)(std::uintptr_t system, std::int32_t state);
        using SaveGameLoadFn = bool(__fastcall *)(std::uintptr_t manager, std::uintptr_t chunk,
                                                  std::uintptr_t save_path);
        using LoadCGFMaterialFn = std::uintptr_t(__fastcall *)(std::uintptr_t manager, const char *material,
                                                               const char *asset, std::uint32_t flags);
        using LoadMaterialShaderFn = bool(__fastcall *)(std::uintptr_t material, std::uintptr_t parent,
                                                        const char *shader, std::uint64_t mask,
                                                        std::uintptr_t resources, std::uintptr_t public_params);
        using FlushRTCommandsFn = bool(__fastcall *)(std::uintptr_t renderer, bool wait, bool immediately, bool force);

        struct ResourceName
        {
            std::size_t length;
            std::size_t capacity;
            const char *data;
        };
        using ResourceLookupFn = std::uintptr_t(__fastcall *)(const std::uint32_t *, const ResourceName *, bool);

        constexpr int k_max_nodes = Constants::TREE_HOLE_MAX_NODES;
        constexpr int k_max_sources = Constants::SHADER_TWIN_MAX_SOURCES;
        // The stock shaders with a twin (blobs::k_twins), indexed by kind.
        constexpr int k_kinds = static_cast<int>(std::size(blobs::k_twins));
        static_assert(k_kinds <= 255, "A swap set keeps each source's kind in a byte.");
        // The character's render nodes a swap set names at most.
        constexpr int k_max_character_nodes = Constants::CHARACTER_TWIN_MAX_NODES;
        constexpr int k_row_floats = Constants::PER_VIEW_HOLE_ROW_FLOATS;
        constexpr int k_pso_slots = Constants::COMPILED_OBJECT_PSO_SLOTS;
        static_assert(k_pso_slots <= 32, "The written slots are a 32-bit mask.");

        /** @brief A compiled object's PSO array, or a copy of it: each entry a CDeviceGraphicsPSO or 0. */
        using PsoSet = std::array<std::uintptr_t, k_pso_slots>;

        /** @brief SShaderItem as this build lays it out (see Constants::SHADER_ITEM_SIZE). */
        struct ShaderItem
        {
            std::uintptr_t shader;
            std::uintptr_t resources;
            std::int32_t technique;
            std::uint32_t preprocess_flags;
            std::array<std::uint8_t, 8> tail;
        };
        static_assert(sizeof(ShaderItem) == Constants::SHADER_ITEM_SIZE, "SShaderItem is 32 bytes in this build.");

        // CRenderView::AddRenderObject(view, render element, item, render object, pass, list, after water).
        using AddRenderObjectFn = void(__fastcall *)(void *view, void *element, const ShaderItem *item,
                                                     std::uint8_t *object, const std::uint8_t *pass, int list,
                                                     int after_water);

        /** @brief One published frame of the swap: the nodes, and each source instance with its twin. */
        struct SwapSet
        {
            std::uint64_t stamp_ms = 0;
            int node_count = 0;
            std::array<std::uintptr_t, k_max_nodes> nodes{};
            // A merged grass patch (its render objects name no node) and its origin, which they carry instead.
            std::array<bool, k_max_nodes> merged{};
            std::array<std::array<float, 3>, k_max_nodes> origins{};
            // A bare frame: its items swap to a twin of TwinScope::ListedFrames instead of a plant twin.
            std::array<bool, k_max_nodes> frames{};
            // While the close-up fade's twins run (third person), the player character's render node and his carried
            // items': the items of a temporary render object that names one of them swap to a Dissolving twin. None
            // while they do not run.
            int character_count = 0;
            std::array<std::uintptr_t, k_max_character_nodes> character_nodes{};
            // The source table is full: a shader without an entry gets none, so it is not requested again.
            bool sources_full = false;
            int source_count = 0;
            std::array<std::uintptr_t, k_max_sources> sources{};
            // The twin to draw with, or 0 while it is not ready or was given up: the item then stays on its source.
            std::array<std::uintptr_t, k_max_sources> twins{};
            // Each source's kind (blobs::k_twins), so an item swaps only to a twin of its own scope, and a
            // transparent-only twin only on a transparent item.
            std::array<std::uint8_t, k_max_sources> kinds{};
            // For a character twin: whether its sets for the fade are warm. A dissolving item takes the twin only
            // then; one that does not dissolve takes it as soon as it is ready.
            std::array<bool, k_max_sources> fade_ready{};
            // The indices of the sources of a Dissolving kind, so an item of the character, which every frame draws,
            // scans only those (a handful) instead of every plant source.
            int character_source_count = 0;
            std::array<std::uint8_t, k_max_sources> character_sources{};
        };
        static_assert(k_max_sources <= 256, "A swap set keeps each character source's index in a byte.");

        /** @brief One published frame of the window and the near cut, the four rows in CV_ProjMatr's order. */
        struct HoleRows
        {
            std::uint64_t stamp_ms = 0;
            std::array<float, k_row_floats> values{};
        };

        /**
         * @brief A source shader a render job saw on a swapped tree or on the character, and the kind it asks a twin
         *        of, for the main thread to create.
         */
        struct TwinRequest
        {
            std::uintptr_t shader = 0;
            std::uintptr_t resources = 0;
            int kind = -1;
        };

        // One reference to each pointer belongs to the candidate, until consumed or discarded. Source entries keep
        // their own shader reference. Retail mfForName reads resources synchronously; its parse closure owns a shader.
        struct TwinCandidate
        {
            TwinRequest request{};
            std::uint8_t priority = 0; // 2: actually drawn; 1: character; 0: other loading candidates
            // A render miss (priority 2) already logged as outside the level inventory (log_render_misses()).
            bool reported = false;
            // When it was first retained (GetTickCount64), for the oldest wait the once-a-second line reports.
            std::uint64_t first_ms = 0;
        };
        constexpr std::size_t k_max_candidates = 512;
        // Of the pool, the slots only plant and frame candidates (TwinScope::ListedPlants and ListedFrames) may take. A
        // character candidate (an Eye, Hair or IllumFade source, the player's or another character's) takes one of the
        // others or none, and never evicts a plant or frame one, so the eyes and hair of every character a loading
        // screen loads cannot push out the plants and frames a pause or the next boundary prepares.
        constexpr std::size_t k_reserved_listed_candidates = 256;
        static_assert(k_reserved_listed_candidates < k_max_candidates, "The character candidates need slots too.");

        /** @brief A listed source instance and the kind of its twin, as the level inventory found them. */
        struct InventoryKey
        {
            std::uintptr_t shader = 0;
            int kind = -1;
            auto operator<=>(const InventoryKey &) const = default;
        };
        constexpr std::uintptr_t k_scope_plants = Rules::TWIN_PLANTS;
        constexpr std::uintptr_t k_scope_frames = Rules::TWIN_FRAMES;
        constexpr std::uintptr_t k_scope_character = Rules::TWIN_CHARACTER;

        /** @brief Why a character twin's set went to the main thread. */
        enum class WarmOrigin : std::uint8_t
        {
            None,    // a tree's wait
            Ahead,   // built aside by a build that does not dissolve, as the close-up fade will ask for it
            Warming, // built aside by a dissolving build of the stock shader, with that build's description
            Cold,    // a dissolving build of the twin that kept the stock PSOs (its set did not draw yet)
        };

        /**
         * @brief A swapped tree's compiled object that kept its stock PSOs because the twin's were not ready, or a
         *        character twin's set, handed from the render thread to the main thread with a reference held on
         *        every PSO in it.
         */
        struct PsoRecord
        {
            // A tree's render node, or for a character twin's set the character's own render node the published swap
            // set named when it was built, so a level change keeps the sets built for the new one (forget_warm_sets()).
            std::uintptr_t node = 0;
            std::uintptr_t object = 0; // the CCompiledRenderObject, which identifies a tree's record
            std::uintptr_t twin_shader = 0;
            std::uint64_t rt_mask = 0;
            std::uint32_t slots = 0; // bit i: slot i was built by this compile
            bool twin_built = false; // the twin build reported success
            PsoSet stock{};
            PsoSet twin{};
            // A character twin's set: why it came, its description's key (desc_key()), its flags, and for one built
            // ahead the RT mask and flags of the build that did not dissolve, for the log.
            WarmOrigin origin = WarmOrigin::None;
            std::uint64_t key = 0;
            std::uint64_t flags = 0;
            std::uint64_t rest_rt_mask = 0;
            std::uint64_t rest_flags = 0;
            // The attempt counts of the stock PSOs when the set was handed over (Constants::PSO_STATE_ATTEMPTS_MASK).
            std::array<std::uint32_t, k_pso_slots> attempts{};
        };

        /**
         * @class RequestQueue
         * @brief Bounded lock-free queue, many producers (render jobs) and one consumer (the main thread).
         * @details Each cell carries a sequence number, so a producer claims a cell with one compare-exchange and
         *          publishes it with a release store, and the consumer takes it only after that store. A full queue
         *          drops the value: a twin request comes again with the item's next draw or its tree's next refill, a
         *          lost PSO wait schedules a refill of the swapped trees, and a set for the fade handed over frees its
         *          key, so a later build makes it again.
         */
        template <typename T, std::size_t N> class RequestQueue
        {
            static_assert((N & (N - 1)) == 0, "The capacity must be a power of two.");

        public:
            RequestQueue() noexcept
            {
                for (std::size_t i = 0; i < N; ++i)
                {
                    m_cells[i].sequence.store(i, std::memory_order_relaxed);
                }
            }

            [[nodiscard]] bool push(const T &value) noexcept
            {
                std::size_t position = m_enqueue.load(std::memory_order_relaxed);
                for (;;)
                {
                    Cell &cell = m_cells[position & (N - 1)];
                    const std::size_t sequence = cell.sequence.load(std::memory_order_acquire);
                    const auto difference = static_cast<std::intptr_t>(sequence) - static_cast<std::intptr_t>(position);
                    if (difference == 0)
                    {
                        if (m_enqueue.compare_exchange_weak(position, position + 1, std::memory_order_relaxed))
                        {
                            cell.value = value;
                            cell.sequence.store(position + 1, std::memory_order_release);
                            return true;
                        }
                    }
                    else if (difference < 0)
                    {
                        return false;
                    }
                    else
                    {
                        position = m_enqueue.load(std::memory_order_relaxed);
                    }
                }
            }

            /** @brief Takes the oldest value. Consumer thread only. */
            [[nodiscard]] bool pop(T &out) noexcept
            {
                Cell &cell = m_cells[m_dequeue & (N - 1)];
                if (cell.sequence.load(std::memory_order_acquire) != m_dequeue + 1)
                {
                    return false;
                }
                out = cell.value;
                cell.sequence.store(m_dequeue + N, std::memory_order_release);
                ++m_dequeue;
                return true;
            }

        private:
            struct Cell
            {
                std::atomic<std::size_t> sequence{0};
                T value{};
            };
            std::array<Cell, N> m_cells{};
            std::atomic<std::size_t> m_enqueue{0};
            std::size_t m_dequeue = 0;
        };

        enum class TwinState : std::uint8_t
        {
            Pending, // created, its parse not finished (EF_LOADED clear)
            Ready,   // parsed with as many techniques as its source; items swap to it
            Failed,  // given up for the session; its source keeps drawing
        };

        /** @brief The main thread's record of one source instance and its twin. */
        struct Source
        {
            std::uintptr_t shader = 0;
            int kind = 0; // its kind in blobs::k_twins
            std::uintptr_t twin = 0;
            std::uint64_t mask = 0;
            std::uint32_t techniques = 0;
            std::uint64_t created_ms = 0;
            TwinState state = TwinState::Pending;
            std::array<char, 64> name{};
            // Recovery takes temporary references in addition to the original generation's session pins.
            bool recovered = false;
            // A ready twin the engine reloaded or changed since it was checked, with its source or on its own (another
            // gen mask or technique count, a parse under way, EF_RELOADED newly set, a feature bit of its source lost
            // to a reparse of the twin): its items keep the source until both read back consistent for
            // SHADER_TWIN_REVALIDATE_MS (revalidate_twins()). stale_ms is when they last read back otherwise, and
            // reload_bits the EF_RELOADED of the source (bit 0) and of the twin (bit 1) when the twin was last found
            // ready.
            bool stale = false;
            std::uint64_t stale_ms = 0;
            std::uint32_t reload_bits = 0;
            // A character twin: its entry in the warm-up table, when it became ready, whether every set of it for the
            // fade can draw (only then do its dissolving items keep it; a fading build that finds a permutation not
            // compiled turns it cold again), and since when all of its sets could draw (0 while one waits or none is
            // kept). A cold report holds it cold from cold_ms until the set of that fading build arrives, or for at
            // most CHARACTER_TWIN_COLD_HOLD_MS.
            int warm_slot = -1;
            std::uint64_t ready_ms = 0;
            bool warm = false;
            std::uint64_t drawable_since_ms = 0;
            bool cold = false;
            std::uint64_t cold_ms = 0;
#if defined(TPVCAMERA_DEV_BUILD) && defined(DMK_ENABLE_PROFILING)
            std::uint64_t traced_state = UINT64_MAX;
#endif
        };

        /** @brief The main thread's record of one swapped tree or frame. */
        struct Track
        {
            std::uintptr_t node = 0;
            float hold = 0.0f;         // seconds left before an unlisted node is swapped back
            bool seen = false;         // listed by the camera this frame
            bool fresh = false;        // newly observed; activation waits for a ready twin of this scope
            bool activated = false;    // this target could draw a twin and needs restoration when released
            const char *why = "";      // why it was swapped off, once it was
            std::uint64_t swapped_ms = 0;
            bool wait_logged = false; // a long wait for the twin's PSOs was logged
            // A wait of one of its compiled objects was dropped (a full queue or list), so nothing would refill it
            // once the twin's PSOs are ready: it is refilled again TREE_HOLE_RETRY_MS after retry_ms.
            bool retry = false;
            std::uint64_t retry_ms = 0;
            // A compiled object of it kept its stock look because the frame's twin-build budget was spent: it is
            // refilled TREE_HOLE_DEFER_RETRY_MS after deferred_ms, unless a refill for another reason comes first.
            bool deferred = false;
            std::uint64_t deferred_ms = 0;
            bool merged = false;           // a merged grass patch, matched by origin
            std::array<float, 3> origin{}; // the patch's m_pos
            bool frame = false;            // a bare frame (TwinScope::ListedFrames)
            std::array<char, 96> name{};
        };

        /** @brief A PsoRecord the main thread holds, and when it first arrived for its compiled object. */
        struct PendingPsos
        {
            PsoRecord record{};
            std::uint64_t first_ms = 0;
#if defined(TPVCAMERA_DEV_BUILD) && defined(DMK_ENABLE_PROFILING)
            std::uint8_t traced_state = 255;
            std::uint32_t traced_slots = 0;
            std::uint64_t traced_attempts_ms = 0;
            std::array<std::uint64_t, k_pso_slots> traced_pso_states{};
            std::array<std::uint8_t, k_pso_slots> traced_pso_flags{};
#endif
        };

        /**
         * @brief One character twin set for the fade the main thread keeps, by its description's key: held and
         *        polled until it can draw, then kept (with no PSO) so the same set is not built again.
         */
        struct WarmEntry
        {
            PsoRecord record{}; // its PSO sets are held while it waits and empty once it is warm
            int source = -1;    // its source's index in s_sources
            bool warm = false;
            bool wait_logged = false;
            std::uint64_t first_ms = 0;
            std::uint64_t warm_ms = 0;
            // Its twin PSOs' attempt counts at the last poll, and since when and over how many polls in a row it has
            // not been able to draw for good (twin_stalled_guarded(), Constants::CHARACTER_TWIN_GIVE_UP_MS); 0 while it
            // can.
            std::array<std::uint32_t, k_pso_slots> twin_attempts{};
            std::uint64_t stalled_ms = 0;
            int stalled_polls = 0;
        };

        enum class Prepare : std::uint8_t
        {
            Pending,
            Ready,
            Failed,
        };

        /** @brief The outcome of a guarded node invalidation. */
        enum class Invalidate : std::uint8_t
        {
            Done,       // the node's persistent render objects refill next frame
            NoRender,   // the node has no render data yet; it fills through AddRenderObject when it is drawn
            NotTheNode, // the temp data no longer names the node: it is gone or reused
            Fault,
        };

        /** @brief The outcome of the guarded row write. */
        enum class RowWrite : std::uint8_t
        {
            Written,
            Skipped, // a view count no game view has
            InUse,   // a row the engine leaves zero is not zero
            Fault,
        };

        // Hooks and their originals.
        std::atomic<AddRenderObjectFn> s_add_render_object_original{nullptr};
        std::atomic<ShaderGenLoadFn> s_shader_gen_original{nullptr};
        std::atomic<CreatePsosFn> s_create_psos_original{nullptr};
        ForwardPsosFn s_character_forward_psos = nullptr; // immutable after hook publication; optional
        PrepassPsosFn s_plant_prepass_psos = nullptr;     // immutable after hook publication; optional
        GBufferPsosFn s_plant_gbuffer_psos = nullptr;     // immutable after hook publication; optional
        std::atomic<SetSystemGlobalStateFn> s_set_system_state_original{nullptr};
        std::atomic<SaveGameLoadFn> s_save_game_load_original{nullptr};
        std::atomic<LoadCGFMaterialFn> s_load_cgf_material_original{nullptr};
        std::atomic<LoadMaterialShaderFn> s_load_material_shader_original{nullptr};
        std::atomic<bool> s_available{false};
        // Set for the session when the feature had to stop (the rows are in use, no twin loaded, a failed
        // preparation).
        std::atomic<bool> s_disabled{false};
        // Set by the render thread when a row the engine leaves zero is in use: the plant twins stop for the session
        // (the character fade's twins need no rows and go on).
        std::atomic<bool> s_rows_in_use{false};
        // Set by a render job that queued a twin request for a shader of the character's items the swap set did not
        // know yet; the main thread takes it each frame, and neither readiness holds that frame.
        std::atomic<bool> s_character_unknown{false};
        // eye_fade_ready() and character_fade_ready(), refreshed by each update.
        std::atomic<bool> s_eye_ready{false};
        std::atomic<bool> s_character_ready{false};

        // Published tables. The three of each are static, so a reader that already loaded one keeps reading it safely.
        std::array<SwapSet, 3> s_sets{};
        int s_next_set = 0;
        std::atomic<const SwapSet *> s_set_published{nullptr};
        std::array<HoleRows, 3> s_rows{};
        int s_next_rows = 0;
        std::atomic<const HoleRows *> s_rows_published{nullptr};

        std::mutex s_candidate_mutex;
        std::array<TwinCandidate, k_max_candidates> s_candidates{};
        std::size_t s_candidate_count = 0;
        // The pairs a render job or a loader thread evicted from the pool, for the main thread to release
        // (drop_evicted()). Nothing but the pool may still hold such a pair (its material can be gone), so its Release
        // can be the last one, which destroys the shader or the resources: it runs on the engine main thread only. A
        // full queue refuses the eviction instead (collect_candidate()).
        RequestQueue<TwinRequest, 64> s_evicted;
        std::atomic<bool> s_level_loading{false};
        std::atomic<std::uintptr_t> s_loading_save_manager{0};
        std::atomic<bool> s_loading_finalizing{false};
        std::atomic<std::uint64_t> s_load_epoch{0};
        DWORD s_asset_scope_tls = TLS_OUT_OF_INDEXES;
        ResourceLookupFn s_resource_lookup = nullptr;
        PSRWLOCK s_resource_lock = nullptr;
        std::uintptr_t s_shader_container_global = 0;
        bool s_recovery_done = false;
        // The level inventory (hooks/twin_inventory.hpp): the global that holds CObjManager, or 0 when the
        // TwinInventory gate failed and the twins come from captured material loads only. Resolved at init, read-only
        // afterward.
        std::uintptr_t s_inventory_objman_global = 0;
        // Set by a render job that retained a render miss of a listed node, so the main thread checks it against the
        // level inventory within a second (log_render_misses()).
        std::atomic<bool> s_render_miss_new{false};
        // Set once the camera asks for the pause-time creation (service_shader_twins_paused()): a variant drawn without
        // its twin then waits for a pause, not only a loading screen (the trace's reason).
        std::atomic<bool> s_pause_service{false};
        // The loading epoch (s_load_epoch) whose level inventory was taken, by its boundary or by the first pause after
        // a DEV hot reload, whose module saw no boundary of its own; UINT64_MAX until one was.
        std::atomic<std::uint64_t> s_inventory_epoch{UINT64_MAX};
        // The camera rules generation (Rules::current_generation(), read before the walk) the level inventory scoped
        // its assets with. Rules edited since then take the inventory again at the next pause, so a model that is now
        // a plant or a frame gets its twins.
        std::atomic<std::uint64_t> s_inventory_rules{0};
        // Plant and frame variants retained from material loads during play since the last log line. Any thread adds.
        std::atomic<std::uint32_t> s_playtime_captures{0};
        RequestQueue<PsoRecord, 128> s_pso_records;
        // The render nodes of swapped trees whose compiled objects kept their stock look past the frame's twin-build
        // budget, for the main thread to refill shortly.
        RequestQueue<std::uintptr_t, 64> s_deferred_nodes;

        // The ready plant twins and their sources, for the CreatePipelineStates detour. Append-only on the main thread:
        // an entry is written before the count that covers it is published, and a twin never stops being ready.
        std::array<std::atomic<std::uintptr_t>, k_max_sources> s_guard_twins{};
        std::array<std::atomic<std::uintptr_t>, k_max_sources> s_guard_sources{};
        // Whether the twin's items take the hole in the depth prepass (prepass_kind()).
        std::array<std::atomic<bool>, k_max_sources> s_guard_prepass{};
#if defined(TPVCAMERA_DEV_BUILD) && defined(DMK_ENABLE_PROFILING)
        std::array<std::atomic<std::uint8_t>, k_max_sources> s_trace_guard_kinds{};
#endif
        std::atomic<int> s_guard_count{0};
        // The ready character twins and their sources, for the detour, the same way, with whether the twin's kind is
        // transparent-only (IllumFade); an entry's done flag is set by the main thread while the twin is warm for the
        // fade or once it was given up, and a dissolving build of its source then builds nothing aside. A render
        // thread whose check of a fading build found a permutation not compiled sets the entry's cold flag, and the
        // main thread clears done.
        std::array<std::atomic<std::uintptr_t>, k_max_sources> s_warm_twins{};
        std::array<std::atomic<std::uintptr_t>, k_max_sources> s_warm_sources{};
        std::array<std::atomic<bool>, k_max_sources> s_warm_transparent_only{};
        std::array<std::atomic<bool>, k_max_sources> s_warm_done{};
        std::array<std::atomic<bool>, k_max_sources> s_warm_cold{};
        std::atomic<int> s_warm_count{0};
        // Character twin sets handed from the render thread to the main thread, with a reference held on every PSO in
        // them.
        RequestQueue<PsoRecord, 128> s_warm_records;
        // When a render job last saw an item of each source (by its index in the swap set, which is the main thread's)
        // on the character, in GetTickCount64 milliseconds.
        std::array<std::atomic<std::uint64_t>, k_max_sources> s_character_seen_ms{};
        // The keys (desc_key()) of the character twin sets built aside so far, shared by the render threads: open
        // addressing, a slot taken with one compare-exchange from k_key_empty and never reused, k_key_removed for a
        // set that could not be handed over (so it may be built again). The main thread clears it on a level change
        // and takes the keys of the sets it keeps again (forget_warm_sets()).
        constexpr std::uint64_t k_key_empty = 0;
        constexpr std::uint64_t k_key_removed = 1;
        std::array<std::atomic<std::uint64_t>, Constants::CHARACTER_WARM_KEY_SLOTS> s_warm_keys{};
        static_assert((Constants::CHARACTER_WARM_KEY_SLOTS & (Constants::CHARACTER_WARM_KEY_SLOTS - 1)) == 0,
                      "The key table's size must be a power of two.");
        // The dissolve's RT-mask bit (the DissolveRtBit anchor's global), read on the main thread when the twins are
        // prepared and before any set is built ahead; 0 when it did not resolve or did not check out, and the sets
        // built ahead then carry the dissolve in their flags only.
        std::atomic<std::uint64_t> s_dissolve_rt_bit{0};

        // Set by a render thread whose PSO wait did not fit the hand-over queue; it cannot say for which tree, so the
        // main thread schedules a retry for every swapped tree.
        std::atomic<bool> s_pso_record_lost{false};

        // The camera publishes its frame sequence. The TwinBuildPermit that set s_budget_in_flight exclusively owns
        // s_budget_frame and s_budget_used_us while a plant build is in flight; competing callers defer without
        // waiting. Completion charges the current camera frame, so an update during a slow native build cannot make
        // the next caller erase that build's cost.
        std::atomic<std::uint32_t> s_frame_seq{0};
        std::atomic_flag s_budget_in_flight{};
        std::uint32_t s_budget_frame = 0;
        std::uint32_t s_budget_used_us = 0;
        // Builds charged to a later camera frame than the one they started in, and builds deferred because another
        // build held the permit, since the last log line.
        std::atomic<std::uint32_t> s_budget_carryovers{0};
        std::atomic<std::uint32_t> s_budget_busy_deferrals{0};

        /** @brief Records a see-through trace event stamped with the camera frame (DEV profiling builds only). */
        void trace_event(trace::Event event, std::uintptr_t node = 0, std::uintptr_t object = 0,
                         std::uintptr_t shader = 0, std::uintptr_t resources = 0, std::uint64_t a = 0,
                         std::uint64_t b = 0) noexcept
        {
            if constexpr (trace::enabled)
                trace::emit(event, s_frame_seq.load(std::memory_order_relaxed), node, object, shader, resources, a, b);
        }

        // Counters for the once-a-second log line. Render threads add, the main thread takes.
        std::atomic<std::uint32_t> s_item_swaps{0};
        // Of s_item_swaps, the character's items at rest (not dissolving), and the others by kind; the dissolving
        // items of another character (the engine's LOD dissolve) that kept the stock shader; and the character's
        // dissolving items of a transparent-only kind that kept it, being opaque.
        std::atomic<std::uint32_t> s_rest_swaps{0};
        std::array<std::atomic<std::uint32_t>, k_kinds> s_kind_swaps{};
        std::atomic<std::uint32_t> s_node_rejects{0};
        std::atomic<std::uint32_t> s_opacity_skips{0};
        std::atomic<std::uint32_t> s_ext_aliases{0};
        std::atomic<std::uint32_t> s_rows_written{0};
        std::atomic<std::uint32_t> s_guard_faults{0};
        std::atomic<std::uint32_t> s_dropped_requests{0};
        // The CreatePipelineStates detour's: compiled objects that named a twin, those that took the twin's PSOs and
        // those that kept the stock's, of which those of a temporary render object (nothing waits on them), records
        // that could not be handed over or held, and the slowest twin build (microseconds).
        std::atomic<std::uint32_t> s_pso_guarded{0};
        std::atomic<std::uint32_t> s_pso_took_twin{0};
        std::atomic<std::uint32_t> s_pso_kept_stock{0};
        std::atomic<std::uint32_t> s_pso_temporary{0};
        std::atomic<std::uint32_t> s_pso_dropped{0};
        std::atomic<std::uint32_t> s_pso_slowest_us{0};
        // Of the guarded builds, those that built only the twin's depth prepass and those that built only its G-buffer
        // passes (build_plant_twin()); the rest built the twin's whole set.
        std::atomic<std::uint32_t> s_pso_prepass_only{0};
        std::atomic<std::uint32_t> s_pso_gbuffer_only{0};
        // Compiled objects that kept their stock look because the frame's twin-build budget was spent.
        std::atomic<std::uint32_t> s_pso_deferred{0};
        // The character twins' builds: those of an item that does not dissolve (it keeps the stock PSOs), the sets
        // they built ahead for the fade, the ones they did not build because the set was built before or the key
        // table was full; the dissolving builds of the stock shader that built the twin's set aside; the sets that
        // could not be handed over; and the fading builds of the twin, those that took its PSOs at once (and of those,
        // the ones whose set had been built ahead) and those that kept the stock PSOs (the twin turned cold).
        std::atomic<std::uint32_t> s_rest_builds{0};
        std::atomic<std::uint32_t> s_ahead_builds{0};
        std::atomic<std::uint32_t> s_ahead_known{0};
        std::atomic<std::uint32_t> s_ahead_full{0};
        std::atomic<std::uint32_t> s_warming_builds{0};
        std::atomic<std::uint32_t> s_warm_records_dropped{0};
        std::atomic<std::uint32_t> s_fade_warm_builds{0};
        std::atomic<std::uint32_t> s_fade_warm_ahead{0};
        std::atomic<std::uint32_t> s_fade_cold_builds{0};
        // The first fading build of the twin whose description no set built ahead had (its RT mask and flags), for one
        // log line: a render thread takes the claim, writes the masks, then sets ready; the main thread logs it once.
        std::atomic<bool> s_fade_miss_taken{false};
        std::atomic<std::uint64_t> s_fade_miss_rt_mask{0};
        std::atomic<std::uint64_t> s_fade_miss_flags{0};
        std::atomic<bool> s_fade_miss_ready{false};
        // PSO references the mod left held rather than drop the last one off the engine's own release path. Any
        // thread adds; read for the logs.
        std::atomic<std::uint32_t> s_pso_leaked{0};

        // Stats for shader_twin_stats(), refreshed at the end of each update.
        std::atomic<int> s_stat_swapped{0};
        std::atomic<int> s_stat_twins{0};
        std::atomic<int> s_stat_ready{0};

        // Resolved at init, read-only afterward.
        std::uintptr_t s_genv = 0;
        std::uintptr_t s_for_name = 0;
        std::uintptr_t s_adjust_file_name = 0;
        std::uintptr_t s_flush_rt_commands = 0;
        std::ptrdiff_t s_pak_open_slot = 0;
        std::ptrdiff_t s_pak_read_slot = 0;
        std::ptrdiff_t s_pak_close_slot = 0;
        // The DissolveRtBit global, or 0 when its gate failed.
        std::uintptr_t s_dissolve_rt_global = 0;

        // Main-thread state, guarded by s_main_mutex so shutdown_shader_twins() can run on the teardown thread while
        // the camera frame may still call update_shader_twins().
        std::mutex s_main_mutex;
        Prepare s_prepare = Prepare::Pending;
        std::uint64_t s_prepare_retry_ms = 0;
        std::uintptr_t s_shader_manager = 0;
        std::array<Source, k_max_sources> s_sources{};
        int s_source_count = 0;
#if defined(TPVCAMERA_DEV_BUILD)
        // Goes up each time a twin becomes ready (refresh_twins()) or checks out again after going stale
        // (revalidate_twins()), so the DEV clone dry-run lists its pairs again after any such change (dry_run_pairs()).
        int s_usable_generation = 0;
#endif
        bool s_sources_full_logged = false;
        // The listed pairs the last level inventory found, sorted: a render miss of a pair outside them is logged
        // (its material is scoped otherwise than its node, or the level tables do not hold it). Identities only,
        // never read through; empty until an inventory found a level.
        std::vector<InventoryKey> s_inventory_pairs;
        // Twin requests whose shader or resources were gone by the time they were handled, since the last log line.
        std::uint32_t s_stale_requests = 0;
        // A kind whose stock shader a game patch changed, or whose twin binary the engine refused (EF_NOTFOUND): no
        // twins of that kind for the session, and the other kinds go on. The feature turns off once every kind failed.
        // The plant kinds also fail together when the window's rows turn out to be in use, or when the Z pass
        // includes their copies were spliced from changed.
        std::array<bool, k_kinds> s_kind_failed{};
        // The character's own render node the twins ran for last frame (the first of update_shader_twins's
        // character_nodes), the last one that held for CHARACTER_ROOT_SETTLE_FRAMES (another one means a new level or
        // save, see forget_warm_sets()), and the readiness last logged (bit 0 eye_fade_ready(), bit 1
        // character_fade_ready()).
        std::uintptr_t s_character_root = 0;
        std::uintptr_t s_last_character_root = 0;
        // Another node seen in place of s_last_character_root, and for how many frames in a row.
        std::uintptr_t s_root_candidate = 0;
        int s_root_frames = 0;
        int s_ready_logged = 0;
        // Whether the first fading build whose description no set built ahead had was logged.
        bool s_fade_miss_logged = false;
        std::array<Track, k_max_nodes> s_tracks{};
        int s_track_count = 0;
        // The frame's refills, run after the swap set is published, so a render object refilled right after its node
        // was invalidated already sees the set that adds or drops the node: the nodes swapped off this frame (evicted
        // or unlisted past their hold), and whether a twin became ready, which refills every swapped node.
        std::array<Track, 2 * k_max_nodes> s_released{};
        int s_released_count = 0;
        bool s_refill_all = false;
        // The compiled objects of swapped trees waiting for the twin's PSOs, and the refills that ended a wait.
        std::array<PendingPsos, Constants::TREE_HOLE_MAX_PENDING> s_pending{};
        int s_pending_count = 0;
        // The character twins' sets for the fade the main thread keeps (see WarmEntry), those of them still waiting,
        // and the sets dropped since the last log line because the table was full of waiting ones.
        std::array<WarmEntry, Constants::CHARACTER_WARM_MAX_ENTRIES> s_warm_entries{};
        int s_warm_entry_count = 0;
        int s_warm_pending_count = 0;
        std::uint32_t s_warm_entries_dropped = 0;
        // The waiting count last logged, so the once-a-second line only comes when something changed.
        int s_last_logged_pending = 0;
        std::uint32_t s_promotions = 0;
        std::uint64_t s_last_log_ms = 0;
        int s_last_logged_swapped = 0;
        // The level inventory the first pause after a DEV hot reload took (pause_inventory()), or the pairs of a
        // boundary's inventory whose stock shader still parsed there (finalize_loading()): its owned candidates still
        // to create, outside the 512-entry pool so none is dropped for room, and the ones already handled, kept owned
        // until the coverage line read once the last is handled has named them. The next boundary drops both. The
        // handled list always has room for every entry still to create, so moving one along never allocates.
        std::vector<TwinCandidate> s_pause_inventory;
        std::vector<TwinCandidate> s_pause_inventory_done;
        // Set only around the create_twin() call of a frame the game stands still in (create_during_pause()): its
        // permit outside a loading boundary.
        bool s_pause_permit = false;

        /** @brief What lets a frame create a twin outside a loading screen (pause_reason()). */
        enum class PauseReason : std::uint8_t
        {
            None,
            Menu,    // the Esc game menu over the stopped game clock
            Overlay, // a UI overlay (inventory, map, codex) over the stopped game clock
            // The stopped game clock with neither seen: the Esc menu open across a DEV hot reload (the menu and overlay
            // flags come from events, so a new module sees a menu only once it opens again), or any other standstill.
            Clock,
        };

        /**
         * @brief The pause the camera frames report (service_shader_twins_paused()). Main thread only, the camera frame
         *        and log_second(): the frames that do not stand still read and write it without a lock.
         */
        struct PauseState
        {
            PauseReason reason = PauseReason::None;
            std::uint64_t since_ms = 0;        // when the current pause began; 0 while the game runs
            std::uint64_t next_ms = 0;         // no creation before this: the spacing after the last barrier
            std::uint64_t last_created_ms = 0; // when the last creation's barrier returned; 0 for none in this pause
            int created = 0;                   // twins created in the current pause
            int ready = 0;                     // of them, ready once their barrier returned
            bool servicing = false;            // inside create_during_pause(), whose barrier's wait could re-enter
            bool off = false;                  // a barrier failed: no more pause-time creation this session
            bool full_logged = false;          // a full source table was logged in the current pause
        };
        PauseState s_pause;
        // The twins created during pauses and the pauses that ended within SHADER_TWIN_PAUSE_SPACING_MS of a creation,
        // this session, and what the once-a-second wait line last reported (log_pause_waits()). Main thread only.
        std::uint32_t s_pause_created_total = 0;
        std::uint32_t s_pause_overlaps = 0;
        int s_logged_waiting = 0;
        int s_logged_inventory_left = 0;
        std::uint32_t s_logged_pause_created = 0;
        std::uint32_t s_logged_overlaps = 0;

        [[nodiscard]] constexpr char lower(char c) noexcept
        {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
        }

        /** @brief True when @p text starts with @p prefix, ignoring case. */
        [[nodiscard]] bool starts_with_nocase(const char *text, const char *prefix) noexcept
        {
            for (; *prefix != '\0'; ++prefix, ++text)
            {
                if (*text == '\0' || lower(*text) != lower(*prefix))
                {
                    return false;
                }
            }
            return true;
        }

        /** @brief True when @p text equals @p want, ignoring case. */
        [[nodiscard]] constexpr bool equals_nocase(const char *text, const char *want) noexcept
        {
            for (;; ++text, ++want)
            {
                if (lower(*text) != lower(*want))
                {
                    return false;
                }
                if (*want == '\0')
                {
                    return true;
                }
            }
        }

        /** @brief The kind whose label is @p label, or -1. */
        [[nodiscard]] constexpr int kind_labelled(const char *label) noexcept
        {
            for (int kind = 0; kind < k_kinds; ++kind)
            {
                if (equals_nocase(blobs::k_twins[kind].label, label))
                {
                    return kind;
                }
            }
            return -1;
        }

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

        /**
         * @brief True for a scope whose items belong to a node the camera lists (a plant or a frame): its twins include
         *        the hole, which reads the window rows, and copy the Z pass includes.
         */
        [[nodiscard]] constexpr bool listed_scope(blobs::TwinScope scope) noexcept
        {
            return scope == blobs::TwinScope::ListedPlants || scope == blobs::TwinScope::ListedFrames;
        }

        /**
         * @brief True for a listed kind whose items the engine draws in the depth prepass: a twin of Vegetation, the
         *        one shader with ShaderType Vegetation among the sources, which is the type the engine gives the
         *        FB_ZPREPASS batch flag (sub_180C17074; Illum is General and never prepassed).
         */
        [[nodiscard]] constexpr bool prepass_kind(int kind) noexcept
        {
            return kind >= 0 && kind < k_kinds && listed_scope(blobs::k_twins[kind].scope) &&
                   equals_nocase(blobs::k_twins[kind].source_name, "Vegetation");
        }

        // The Eye kind: whether its twins are warm decides eye_fade_ready(), where a given-up Hair or IllumFade twin
        // only holds character_fade_ready() back.
        constexpr int k_eye_kind = kind_labelled("Eye");
        static_assert(k_eye_kind >= 0 && blobs::k_twins[k_eye_kind].scope == blobs::TwinScope::Dissolving,
                      "The generated header has no Dissolving Eye twin.");

        /**
         * @brief The kind (index in blobs::k_twins) of @p scope whose stock shader @p shader is an instance of, by the
         *        shader's name (IShader::GetName, the string at +0x88), or -1 for any other shader.
         * @details One source name has one kind per scope at most (kinds_unique()); an Illum instance is the Illum kind
         *          on a listed plant and the IllumFade kind on the character.
         * @note Render jobs, for a shader on a swapped tree or a dissolving character that has no twin entry yet, and
         *       the main thread. SEH-guarded, POD body.
         */
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

        /**
         * @brief Gives @p twin the feature bits (Constants::SHADER_FEATURE_FLAGS_OFFSET) its source @p source has and
         *        it lacks, the ones the parse sets from a shader's name; @p before and @p after get the twin's word.
         * @note Main thread, on a twin whose parse finished and that no item names yet. SEH-guarded, POD body.
         */
        [[nodiscard]] bool match_feature_flags_guarded(std::uintptr_t twin, std::uintptr_t source,
                                                       std::uint16_t &before, std::uint16_t &after) noexcept
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

        /**
         * @brief What revalidate_twins() compares of a CShader: its flags and flags2, technique count, gen mask and
         *        feature word.
         */
        struct ShaderState
        {
            std::uint32_t flags = 0;
            std::uint32_t flags2 = 0;
            std::uint32_t techniques = 0;
            std::uint64_t mask = 0;
            std::uint16_t features = 0; // Constants::SHADER_FEATURE_FLAGS_OFFSET
        };

        /** @brief Reads @p shader's ShaderState. Main thread. SEH-guarded, POD body. */
        [[nodiscard]] bool read_shader_state_guarded(std::uintptr_t shader, ShaderState &out) noexcept
        {
            __try
            {
                out.flags = *reinterpret_cast<const volatile std::uint32_t *>(shader + Constants::SHADER_FLAGS_OFFSET);
                out.flags2 =
                    *reinterpret_cast<const volatile std::uint32_t *>(shader + Constants::SHADER_FLAGS2_OFFSET);
                out.techniques = *reinterpret_cast<const volatile std::uint32_t *>(
                    shader + Constants::SHADER_TECHNIQUE_COUNT_OFFSET);
                out.mask =
                    *reinterpret_cast<const volatile std::uint64_t *>(shader + Constants::SHADER_GEN_MASK_OFFSET);
                out.features =
                    *reinterpret_cast<const volatile std::uint16_t *>(shader + Constants::SHADER_FEATURE_FLAGS_OFFSET);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        /** @brief EF_RELOADED of a source (bit 0) and of its twin (bit 1). */
        [[nodiscard]] constexpr std::uint32_t reload_bits(const ShaderState &stock, const ShaderState &twin) noexcept
        {
            return ((stock.flags & Constants::SHADER_FLAG_RELOADED) != 0 ? 1u : 0u) |
                   ((twin.flags & Constants::SHADER_FLAG_RELOADED) != 0 ? 2u : 0u);
        }

        /**
         * @brief True while @p twin has every feature bit of its source @p stock (match_feature_flags_guarded() gave
         *        them when it became ready): a reparse of the twin rebuilds its word from its own name and drops them.
         */
        [[nodiscard]] constexpr bool features_kept(const ShaderState &stock, const ShaderState &twin) noexcept
        {
            return (stock.features & ~twin.features) == 0;
        }

        /** @brief True for a shader whose parse finished and loaded: EF_LOADED, not EF_NOTFOUND, some technique. */
        [[nodiscard]] constexpr bool shader_parsed(const ShaderState &state) noexcept
        {
            return (state.flags & Constants::SHADER_FLAG_LOADED) != 0 &&
                   (state.flags & Constants::SHADER_FLAG_NOT_FOUND) == 0 && state.techniques != 0;
        }

        /**
         * @brief The opacity of an item's material, @p resources (its IRenderShaderResources), as the engine's render
         *        list select reads it: GetStrengthValue(EFTT_OPACITY) through the CShaderResources vtable (see
         *        Constants::SHADER_RESOURCES_VTABLE_GET_STRENGTH_OFFSET).
         * @return The value, or 1 (opaque, so nothing swaps) for no resources, another type, or a fault.
         * @note Render jobs, on the item AddRenderObject was handed; the call is a read with no side effects, the one
         *       the engine makes on the same item right after. SEH-guarded, POD body.
         */
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

        /**
         * @brief True when @p item may swap to a twin of @p kind: any item, or for a transparent-only kind (IllumFade)
         *        one whose opacity is below 1, the engine's own test for the transparent list (a NaN reads opaque).
         */
        [[nodiscard]] bool item_fits_kind(int kind, const ShaderItem &item) noexcept
        {
            return !blobs::k_twins[kind].transparent_only || item_opacity_guarded(item.resources) < 1.0f;
        }

        /**
         * @brief Reads a merged grass patch's origin (m_pos) into @p out when @p node is one (EERType 0x17).
         * @return False for any other node, or on a fault. Main thread. SEH-guarded, POD body.
         */
        [[nodiscard]] bool merged_origin_guarded(std::uintptr_t node, float *out) noexcept
        {
            __try
            {
                using GetTypeFn = int(__fastcall *)(std::uintptr_t);
                const auto *vtable = *reinterpret_cast<const std::uintptr_t *const *>(node);
                const auto get_type =
                    reinterpret_cast<GetTypeFn>(vtable[Constants::RENDERNODE_VTABLE_GETTYPE_OFFSET / 8]);
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

        /**
         * @brief The merged grass patch of @p set whose origin is the translation of @p object's world matrix, or 0.
         * @details A patch's render objects leave m_pRenderNode null, and their world matrix is the patch origin
         *          (Constants::RENDER_OBJECT_MATRIX_OFFSET), so the origin is how a render job tells which patch drew.
         * @note Render jobs, on a render object valid for the call.
         */
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
                if (set.merged[slot] && set.origins[slot][0] == x && set.origins[slot][1] == y &&
                    set.origins[slot][2] == z)
                {
                    return set.nodes[slot];
                }
            }
            return 0;
        }

        /**
         * @brief Copies the C string whose pointer is stored at @p object + @p offset into @p out, printable only.
         * @note SEH-guarded, POD body. @p out is empty on a fault.
         */
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

        /**
         * @brief Invalidates @p node's persistent render objects the way AddRenderObject does for an item that is not
         *        ready (0x1804A510E): the temp-data stamp at +0xF8 goes to +0xFC atomically and the node's stamp at
         *        +0x38 becomes -1, so the next frame refills them through AddRenderObject.
         * @details Nothing is written unless the temp data's owner link names @p node, so a node that was freed (a
         *          level change while it was held) or whose memory now holds another object is left alone.
         * @note Main thread. SEH-guarded, POD body.
         */
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

        /**
         * @brief The same proof as invalidate_node_guarded() without the write: Done for a live node with render data.
         * @note Main thread. SEH-guarded, POD body.
         */
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
                return *reinterpret_cast<const std::uintptr_t *>(temp + Constants::RENDER_TEMP_DATA_OWNER_OFFSET) ==
                               node
                           ? Invalidate::Done
                           : Invalidate::NotTheNode;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return Invalidate::Fault;
            }
        }

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
            s_rows_published.store(nullptr, std::memory_order_release);
            s_set_published.store(nullptr, std::memory_order_release);
            if (!s_rows_in_use.exchange(true, std::memory_order_acq_rel))
            {
                (void)DMK::log().try_log(DMK::LogLevel::Error,
                                         "ShaderTwins: the game now fills the per-view rows the window rides in "
                                         "(CV_ProjMatr); the tree see-through is off for the session (the "
                                         "character fade's twins need no rows and go on)");
            }
        }

        /**
         * @brief Per-view upload mid hook: writes the window rows into the main view's buffer right before it uploads.
         * @details At the call, by its ABI, rcx is the buffer, rdx the data and the view count is at [rsp+0x20], and
         *          the function still holds its pipeline in r13 (keeps_pipeline_in_r13()). Every other upload (another
         *          pipeline or buffer, or a call while the hole is off) is left alone, so the rows stay the memset
         *          zeros and the twin's early-out skips the hole.
         */
        void per_view_upload_detour(DMK::hook::MidContext &ctx) noexcept
        {
            HookProfile::Scope timing(HookProfile::Path::PerViewUpload);
            const HoleRows *rows = s_rows_published.load(std::memory_order_acquire);
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
         * @brief True when @p object is a temporary render object drawn with a dissolve (FOB_DISSOLVE in its flags):
         *        a character the close-up fade (or the engine's own LOD dissolve) is fading.
         * @details A persistent object keeps its items until its node refills, so a swap there would outlast the
         *          dissolve; the skin attachments that draw a character's head are temporary.
         */
        [[nodiscard]] bool dissolving_object(const std::uint8_t *object) noexcept
        {
            const auto flags = *reinterpret_cast<const std::uint64_t *>(object + Constants::RENDER_OBJECT_FLAGS_OFFSET);
            return (flags & Constants::RENDER_OBJECT_FLAG_DISSOLVE) != 0 &&
                   object[Constants::RENDER_OBJECT_PERSISTENT_OFFSET] == 0;
        }

        /** @brief True when @p object names one of the character's render nodes in @p set. */
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

        bool collect_candidate(const ShaderItem &item, int kind, std::uint8_t priority, bool loading_only) noexcept;

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
            if (!listed_object(set, object, scope, matched_node))
            {
                observation.reason = trace::ItemReason::BindingExpired;
                if (set.character_count <= 0 || object[Constants::RENDER_OBJECT_PERSISTENT_OFFSET] != 0)
                {
                    return false;
                }
                dissolving = dissolving_object(object);
                if (!character_object(set, object))
                {
                    if (dissolving)
                    {
                        s_node_rejects.fetch_add(1, std::memory_order_relaxed);
                    }
                    return false;
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
                if (character)
                {
                    s_character_seen_ms[slot].store(now, std::memory_order_relaxed);
                }
                // Not ready, given up, or (a dissolving item of the character) not warm for the fade yet: the item
                // keeps its source.
                if (set.twins[slot] == 0 || (dissolving && !set.fade_ready[slot]))
                {
                    observation.reason = trace::ItemReason::TwinUnavailable;
                    return false;
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

        /**
         * @brief CRenderView::AddRenderObject detour: a main-view item of a swapped tree, or an Eye, Hair or
         *        transparent Illum item of the player character in third person, draws with its twin shader.
         * @details The twin goes in a stack copy of the item (same resources, technique and flags). The engine keeps
         *          only a packed key of it in a persistent object (by the twin's id), and only its shader and resources
         *          pointers in a pooled compiled object (sub_1804A6104), and the twin is never released, so the copy
         *          does not outlive anything it names.
         * @note Raw arbitrary-signature detour (DMK [B-84]): it must not throw, and every call in it is noexcept.
         */
        void __fastcall add_render_object_detour(void *view, void *element, const ShaderItem *item,
                                                 std::uint8_t *object, const std::uint8_t *pass, int list,
                                                 int after_water) noexcept
        {
            const DetourScope in_flight;
            HookProfile::Scope timing(HookProfile::Path::AddRenderObject);
            const AddRenderObjectFn original = s_add_render_object_original.load(std::memory_order_acquire);
            const SwapSet *set = s_set_published.load(std::memory_order_acquire);
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

        /**
         * @brief .ext loader detour: a twin's name loads its stock shader's gen flags.
         * @details The loader formats "%s%s.ext" from the name and caches the result by name, so passing the stock name
         *          ("Vegetation", "Illum", "Eye", "Hair") returns the SShaderGen every instance of it already uses, and
         *          the twin's gen-mask bits mean the same thing as its source's. mfForName passes the name it was
         *          given, the twin's full name; a longer name that starts with it is taken too.
         * @note Raw arbitrary-signature detour (DMK [B-84]): it must not throw, and every call in it is noexcept.
         */
        void *__fastcall shader_gen_load_detour(void *manager, void *out, const char *name) noexcept
        {
            const DetourScope in_flight;
            const ShaderGenLoadFn original = s_shader_gen_original.load(std::memory_order_acquire);
            if (name != nullptr)
            {
                for (const blobs::Twin &twin : blobs::k_twins)
                {
                    if (starts_with_nocase(name, twin.twin_name))
                    {
                        s_ext_aliases.fetch_add(1, std::memory_order_relaxed);
                        return original(manager, out, twin.source_name);
                    }
                }
            }
            return original(manager, out, name);
        }

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

        /** @brief Drops every reference @p set holds, counting the ones left held. Any thread. */
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

        /** @brief The outcome of comparing a stock and a twin PSO set. */
        enum class PsoReady : std::uint8_t
        {
            Ready,
            Waiting,
            Fault,
        };

        /**
         * @brief Ready when, over the slots in @p slots, the stock set draws at least one pass and every pass it draws
         *        (a PSO whose valid byte is set) has a valid twin PSO in the same slot.
         * @details A slot the stock set never draws (no PSO, or one that stays invalid like the unused forward pass)
         *          does not hold the twin back. A stock set that is still compiling itself waits, and so does one with
         *          a PSO whose D3D12 creation was requested (Constants::PSO_STATE_QUEUED): that pass may become valid
         *          next, and a twin taken now could lack it for the whole compile of its own. A twin PSO whose
         *          creation keeps failing leaves the item on its stock PSOs: a tree keeps its stock look, which the
         *          long-wait log reports, and the warm-up gives a character twin up (give_up_fade()).
         * @note Any thread, on PSOs the caller holds references to. SEH-guarded, POD body.
         */
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
        // Only held PSOs are read. Fields can change between reads; diagnostics never drive readiness policy.
        struct PsoObservation
        {
            std::uint32_t state = 0;
            bool valid = false;
            bool fault = false;
        };

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

        // Initial/validity/creation-request changes are immediate; attempt-only progress is limited to 10 Hz.
        // One event pairs both PSOs of a slot. The recorder stores identities, not borrowed objects to read later.
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
         * @brief pso_set_ready_guarded() for a character twin's set for the fade, which also waits for a stock PSO that
         *        is still compiling.
         * @details A set built ahead holds the stock shader's dissolving permutations too, which may not be compiled
         *          either. A stock PSO that is not valid and not queued counts as a pass that never draws only while
         *          its attempt count stands still: the factory creates a PSO again every frame while its shaders
         *          compile, so a count that moved since the last poll (@p attempts, updated here) means it still
         *          compiles, and the set waits. The first poll compares with the counts read at the hand-over, which
         *          the factory may not have tried again yet, so @p stock_idle says a slot counted as never drawing,
         *          and the caller takes such a set only once it has been held for a moment.
         * @note Main thread, on PSOs it holds references to. SEH-guarded, POD body.
         */
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
                    waiting =
                        waiting || twin[i] == 0 ||
                        *reinterpret_cast<const volatile std::uint8_t *>(twin[i] + Constants::PSO_VALID_OFFSET) == 0;
                }
                return (drawn > 0 && !waiting) ? PsoReady::Ready : PsoReady::Waiting;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return PsoReady::Fault;
            }
        }

        /**
         * @brief True when a pass the stock set draws (a valid stock PSO) has a twin PSO that cannot draw for good:
         *        none, or one that is not valid, not queued, and whose attempt count stood still since the last call
         *        (@p attempts, updated here), which the factory no longer creates again (see
         *        Constants::CHARACTER_TWIN_GIVE_UP_MS).
         * @details A twin PSO whose count moved still compiles, and a slot the stock set does not draw is not looked
         *          at. The first call compares with zero, and every creation adds to the count, so a twin PSO the
         *          factory tried at all is not still at the first call.
         * @note Main thread, on PSOs it holds references to. SEH-guarded, POD body; false on a fault.
         */
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

        /** @brief The outcome of claiming a key in s_warm_keys. */
        enum class Claim : std::uint8_t
        {
            New,   // the caller took the key and builds the set
            Known, // a set of that key was built before
            Full,  // no slot within reach: nothing is built
        };

        /**
         * @brief Takes @p key in s_warm_keys unless it is there already.
         * @note Render threads and compile jobs; lock-free, at most Constants::CHARACTER_WARM_KEY_PROBES slots.
         */
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

        /** @brief Marks @p key removed, so its set may be built again (it could not be handed over or kept). */
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

        /**
         * @brief CreatePipelineStates detour: a compiled object that names a ready plant twin keeps its stock PSOs
         *        until the twin's are built (see build_guarded_psos()); one that names a character twin and does not
         *        dissolve keeps the stock set and builds the fade's sets ahead (see build_resting_psos()); one that
         *        names a character twin and dissolves takes the twin's only when they can draw (see
         *        build_fading_psos()); and a dissolving stock item of the character whose twin is not warm for the
         *        fade, and that the twin's kind may take (warming_item_fits()), builds the twin's set aside (see
         *        build_warming_psos()).
         * @details Shadow builds are never swapped and pass straight through, as does every object with another shader
         *          (two atomic loads while no twin is ready).
         * @note Raw arbitrary-signature detour (DMK [B-84]): it must not throw, and every call in it is noexcept.
         */
        std::uint8_t __fastcall create_psos_detour(void *pipeline, std::uintptr_t *psos, std::uint8_t *desc,
                                                   void *cache, int is_shadow) noexcept
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

        /** @brief Calls CShaderMan::mfForName under SEH. 0 on a fault. */
        [[nodiscard]] std::uintptr_t for_name_guarded(std::uintptr_t fn, std::uintptr_t manager, const char *name,
                                                      std::uintptr_t resources, std::uint64_t mask) noexcept
        {
            __try
            {
                return reinterpret_cast<ForNameFn>(fn)(manager, name, Constants::SHADER_LOAD_FLAGS_MATERIAL, resources,
                                                       mask);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }

        /** @brief Adds a reference to a CShader (what IShader::AddRef, vtable slot 2, does) under SEH. */
        [[nodiscard]] bool add_shader_ref_guarded(std::uintptr_t shader) noexcept
        {
            __try
            {
                _InterlockedIncrement(reinterpret_cast<volatile long *>(shader + Constants::SHADER_REF_COUNT_OFFSET));
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        /** @brief Calls ICryPak::AdjustFileName under SEH. False on a fault or a null result. */
        [[nodiscard]] bool adjust_file_name_guarded(std::uintptr_t fn, std::uintptr_t pak, const char *source,
                                                    char *destination, std::uint32_t flags) noexcept
        {
            __try
            {
                return reinterpret_cast<AdjustFileNameFn>(fn)(pak, source, destination, flags) != nullptr;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        /** @brief The ICryPak calls the stock shader headers are read through. */
        struct PakCalls
        {
            std::uintptr_t open = 0;
            std::uintptr_t read = 0;
            std::uintptr_t close = 0;
        };

        /** @brief The outcome of a guarded header read. */
        enum class HeaderRead : std::uint8_t
        {
            Read,
            Missing,
            Fault,
        };

        /**
         * @brief Reads the first Constants::SHADER_BIN_HEADER_SIZE bytes of @p path through ICryPak, the way the
         *        shader-binary loader reads a header (FOpen "rb" with the engine flags 0, FReadRaw, FClose).
         * @note Main thread. SEH-guarded, POD body.
         */
        [[nodiscard]] HeaderRead read_pak_header_guarded(const PakCalls &calls, std::uintptr_t pak, const char *path,
                                                         std::uint8_t *out) noexcept
        {
            __try
            {
                void *file = reinterpret_cast<PakOpenFn>(calls.open)(pak, path, "rb", 0);
                if (file == nullptr)
                {
                    return HeaderRead::Missing;
                }
                (void)reinterpret_cast<PakReadRawFn>(calls.read)(pak, out, 1, Constants::SHADER_BIN_HEADER_SIZE, file);
                (void)reinterpret_cast<PakCloseFn>(calls.close)(pak, file);
                return HeaderRead::Read;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return HeaderRead::Fault;
            }
        }

        /** @brief True when @p address lies inside the WHGame.dll image. */
        [[nodiscard]] bool in_game_image(std::uintptr_t address) noexcept
        {
            const ModuleInfo &mod = module_info();
            return mod.base != 0 && address >= mod.base && address < mod.base + mod.size;
        }

        /**
         * @brief The function in @p object's vtable slot at @p slot_offset when it is @p expected, or a jmp thunk that
         *        lands on it. 0 otherwise.
         */
        [[nodiscard]] std::uintptr_t validated_slot(std::uintptr_t object, std::ptrdiff_t slot_offset,
                                                    std::uintptr_t expected) noexcept
        {
            const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{object});
            if (!vtable || expected == 0)
            {
                return 0;
            }
            const auto fn = DMK::memory::read<std::uintptr_t>(DMK::Address{*vtable + slot_offset});
            if (!fn)
            {
                return 0;
            }
            if (*fn == expected)
            {
                return *fn;
            }
            // A jmp thunk in the slot (another layer's hook, or an incremental-link stub) that lands on the function.
            const auto opcode = DMK::memory::read<std::uint8_t>(DMK::Address{*fn});
            if (opcode && *opcode == std::to_integer<std::uint8_t>(DMK::scan::PREFIX_JMP_REL32[0]))
            {
                const auto target =
                    DMK::scan::resolve_rip_relative(DMK::Address{*fn}, DMK::scan::PREFIX_JMP_REL32.size(),
                                                    DMK::scan::PREFIX_JMP_REL32.size() + sizeof(std::int32_t));
                if (target && target->raw() == expected)
                {
                    return *fn;
                }
            }
            return 0;
        }

        /** @brief The function in @p object's vtable slot at @p slot_offset when it lies in the game image, else 0. */
        [[nodiscard]] std::uintptr_t image_slot(std::uintptr_t object, std::ptrdiff_t slot_offset) noexcept
        {
            const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{object});
            if (!vtable || !in_game_image(*vtable))
            {
                return 0;
            }
            const auto fn = DMK::memory::read<std::uintptr_t>(DMK::Address{*vtable + slot_offset});
            return fn && in_game_image(*fn) ? *fn : 0;
        }

        /**
         * @brief True when @p object's vtable carries the RTTI name @p mangled.
         * @note Setup/control-plane only: the RTTI walk takes the loader lock.
         */
        [[nodiscard]] bool object_has_type(std::uintptr_t object, std::string_view mangled) noexcept
        {
            const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{object});
            return vtable && *vtable != 0 && DMK::rtti::vtable_is_type(DMK::Address{*vtable}, mangled);
        }

        /** @brief True when @p object (an IShader pointer) is a CShader. Any thread. */
        [[nodiscard]] bool is_shader(std::uintptr_t object) noexcept
        {
            const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{object});
            return vtable && vtable_is(GameClass::Shader, *vtable);
        }

        /** @brief True when @p object (an IRenderShaderResources pointer) is a CShaderResources. Any thread. */
        [[nodiscard]] bool is_shader_resources(std::uintptr_t object) noexcept
        {
            const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{object});
            return vtable && vtable_is(GameClass::ShaderResources, *vtable);
        }

        /** @brief Calls a verified resource reference method. Keep the SEH body free of C++ owners. */
        [[nodiscard]] bool resource_ref_guarded(std::uintptr_t fn, std::uintptr_t object) noexcept
        {
            __try
            {
                reinterpret_cast<void(__fastcall *)(std::uintptr_t)>(fn)(object);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        /** @brief Drops one reference to a CShader (IShader::Release, vtable slot 3); nothing for another type. */
        void release_candidate_shader(std::uintptr_t shader) noexcept
        {
            if (shader == 0 || !is_shader(shader))
            {
                return;
            }
            const auto fn = image_slot(shader, Constants::SHADER_VTABLE_RELEASE_OFFSET);
            if (fn != 0)
            {
                // IShader::Release returns an int; no caller uses it here.
                __try
                {
                    (void)reinterpret_cast<int(__fastcall *)(std::uintptr_t)>(fn)(shader);
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    s_guard_faults.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        /**
         * @brief Drops the references @p candidate owns and empties it.
         * @note A Release here can be the last one unless a callback still owns the item: such a drop runs on the
         *       engine main thread (or the teardown thread), outside every mod mutex.
         */
        void drop_candidate(TwinCandidate &candidate) noexcept
        {
            const TwinRequest request = std::exchange(candidate.request, TwinRequest{});
            if (request.resources != 0 && is_shader_resources(request.resources))
            {
                const auto fn = image_slot(request.resources, Constants::SHADER_RESOURCES_VTABLE_RELEASE_OFFSET);
                if (fn == 0 || !resource_ref_guarded(fn, request.resources))
                {
                    s_guard_faults.fetch_add(1, std::memory_order_relaxed);
                }
            }
            release_candidate_shader(request.shader);
        }

        /**
         * @brief Drops the pairs a render job or a loader thread evicted from the pool (s_evicted).
         * @note The queue's one consumer: the engine main thread outside every mod mutex, or the teardown thread once
         *       no detour runs.
         */
        void drop_evicted() noexcept
        {
            TwinCandidate evicted{};
            while (s_evicted.pop(evicted.request))
            {
                drop_candidate(evicted);
            }
        }

        /** @brief The engine main thread's id as @p renderer's render thread records it, or 0 when unreadable. */
        [[nodiscard]] DWORD engine_main_thread_id(std::uintptr_t renderer) noexcept
        {
            const auto rt =
                DMK::memory::read<std::uintptr_t>(DMK::Address{renderer + Constants::RENDERER_RENDER_THREAD_OFFSET});
            if (!rt || *rt == 0)
            {
                return 0;
            }
            const auto id =
                DMK::memory::read<DWORD>(DMK::Address{*rt + Constants::RENDER_THREAD_MAIN_THREAD_ID_OFFSET});
            return id ? *id : 0;
        }

        /** @brief True on the engine main thread (g_env's renderer says which it is). Any thread. */
        [[nodiscard]] bool on_engine_main_thread() noexcept
        {
            const auto renderer =
                DMK::memory::read<std::uintptr_t>(DMK::Address{s_genv + Constants::GENV_RENDERER_OFFSET});
            if (!renderer || *renderer == 0)
            {
                return false;
            }
            const DWORD main_thread = engine_main_thread_id(*renderer);
            return main_thread != 0 && main_thread == GetCurrentThreadId();
        }

        [[nodiscard]] bool loading_materials_admitted() noexcept
        {
            if (s_level_loading.load(std::memory_order_acquire))
            {
                return true;
            }
            const auto manager = s_loading_save_manager.load(std::memory_order_acquire);
            if (manager == 0)
            {
                return false;
            }
            const auto flag =
                DMK::memory::read<std::uint8_t>(DMK::Address{manager + Constants::SAVE_MANAGER_ENGINE_LOADING_OFFSET});
            return flag && *flag == 1;
        }

        /** @brief How far take_candidate_refs() got. */
        enum class Ownership : std::uint8_t
        {
            Owned,      // a reference to the shader, and one to the resources when there are any, are held
            Rejected,   // another type, or the shader's reference could not be taken: nothing is held
            ShaderOnly, // the shader's reference is held and the resources' could not be taken: release the shader
        };

        /**
         * @brief Checks that @p item's shader is a CShader and its resources a CShaderResources (or none), and takes
         *        one reference to each: the pair a TwinCandidate owns until drop_candidate().
         * @note The caller keeps the item alive for the call (its callback owns it, or the level tables hold it). No
         *       engine Release runs here, so a caller may hold a mod mutex.
         */
        [[nodiscard]] Ownership take_candidate_refs(const ShaderItem &item) noexcept
        {
            if (!is_shader(item.shader) || (item.resources != 0 && !is_shader_resources(item.resources)))
            {
                return Ownership::Rejected;
            }
            const auto add_resource =
                item.resources != 0 ? image_slot(item.resources, Constants::SHADER_RESOURCES_VTABLE_ADD_REF_OFFSET) : 0;
            if ((item.resources != 0 && add_resource == 0) || !add_shader_ref_guarded(item.shader))
            {
                return Ownership::Rejected;
            }
            if (item.resources != 0 && !resource_ref_guarded(add_resource, item.resources))
            {
                return Ownership::ShaderOnly;
            }
            return Ownership::Owned;
        }

        /**
         * @brief Puts in @p out an owned candidate of @p item for @p kind (take_candidate_refs()).
         * @return False, holding nothing, when the item's types or references did not check out.
         * @note Main thread, outside the candidate mutex: a failed resources reference releases the shader's here.
         */
        [[nodiscard]] bool own_candidate(const ShaderItem &item, int kind, std::uint8_t priority,
                                         TwinCandidate &out) noexcept
        {
            const Ownership owned = take_candidate_refs(item);
            if (owned == Ownership::ShaderOnly)
            {
                // The level tables still hold the item, so this cannot delete the shader.
                release_candidate_shader(item.shader);
            }
            if (owned != Ownership::Owned)
            {
                return false;
            }
            out = TwinCandidate{
                .request = {item.shader, item.resources, kind}, .priority = priority, .first_ms = GetTickCount64()};
            return true;
        }

        /** @brief True for a plant or frame kind (TwinScope::ListedPlants or ListedFrames). */
        [[nodiscard]] bool listed_kind(int kind) noexcept
        {
            return kind >= 0 && kind < k_kinds && listed_scope(blobs::k_twins[kind].scope);
        }

        /**
         * @brief How a candidate of @p kind with @p priority ranks for a pool slot or a source slot: every plant or
         *        frame candidate above every character one, then by priority (a drawn item's, a character material's,
         *        a loaded material's).
         */
        [[nodiscard]] int candidate_rank(int kind, std::uint8_t priority) noexcept
        {
            return (listed_kind(kind) ? 4 : 0) + priority;
        }

        [[nodiscard]] int candidate_rank(const TwinCandidate &candidate) noexcept
        {
            return candidate_rank(candidate.request.kind, candidate.priority);
        }

        /**
         * @brief Keeps one owned shader/resource pair per source and kind, with no parsing or disk I/O.
         * @details The originating loader/render callback still owns the item while its references are taken.
         *          Duplicate requests only raise priority; render jobs never wait for the candidate mutex. The
         *          finalizer closes admission before taking this mutex, so a worker racing the close either belongs
         *          to the detached batch or stays stock. Engine Release calls run after the mutex is released, and an
         *          evicted pair's only on the main thread (s_evicted). A render miss (priority 2) flags the main thread
         *          to check it against the level inventory. The character's candidates take at most
         *          k_max_candidates - k_reserved_listed_candidates slots; a full pool, or a full character share, makes
         *          room only by evicting the lowest-ranked entry (candidate_rank()) that ranks below the new one, and a
         *          character candidate never evicts a plant or frame one.
         * @note Any thread: render jobs, loader callbacks and the main thread. It waits on no mod lock, and its type
         *       tests compare vtables the RTTI registry resolved at startup.
         * @return True when a new entry was retained; false for a duplicate (whose priority may rise), a request
         *         dropped for room or a busy mutex, or an item whose types or references did not check out.
         */
        bool collect_candidate(const ShaderItem &item, int kind, std::uint8_t priority, bool loading_only) noexcept
        {
            if (kind < 0 || kind >= k_kinds || item.shader == 0 || !s_available.load(std::memory_order_acquire) ||
                s_disabled.load(std::memory_order_acquire) || s_loading_finalizing.load(std::memory_order_acquire) ||
                (loading_only && !loading_materials_admitted()))
            {
                return false;
            }
            TwinCandidate evicted{};
            bool added = false;
            try
            {
                std::unique_lock lock(s_candidate_mutex, std::try_to_lock);
                if (!lock.owns_lock() || !s_available.load(std::memory_order_acquire) ||
                    s_loading_finalizing.load(std::memory_order_acquire) ||
                    (loading_only && !loading_materials_admitted()))
                {
                    s_dropped_requests.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }
                std::size_t characters = 0;
                for (std::size_t i = 0; i < s_candidate_count; ++i)
                {
                    auto &existing = s_candidates[i];
                    if (existing.request.shader == item.shader && existing.request.kind == kind)
                    {
                        if (priority == 2 && existing.priority < 2)
                        {
                            s_render_miss_new.store(true, std::memory_order_relaxed);
                        }
                        existing.priority = std::max(existing.priority, priority);
                        return false;
                    }
                    characters += listed_kind(existing.request.kind) ? 0 : 1;
                }
                const bool listed = listed_kind(kind);
                const bool share_full = !listed && characters >= k_max_candidates - k_reserved_listed_candidates;
                std::size_t slot = s_candidate_count;
                if (slot == s_candidates.size() || share_full)
                {
                    // The lowest-ranked entry whose slot this one may take: with the character share full, only
                    // another character's.
                    slot = s_candidates.size();
                    for (std::size_t i = 0; i < s_candidate_count; ++i)
                    {
                        if (share_full && listed_kind(s_candidates[i].request.kind))
                        {
                            continue;
                        }
                        if (slot == s_candidates.size() ||
                            candidate_rank(s_candidates[i]) < candidate_rank(s_candidates[slot]))
                        {
                            slot = i;
                        }
                    }
                    if (slot == s_candidates.size() ||
                        candidate_rank(s_candidates[slot]) >= candidate_rank(kind, priority))
                    {
                        s_dropped_requests.fetch_add(1, std::memory_order_relaxed);
                        return false;
                    }
                }
                const Ownership owned = take_candidate_refs(item);
                if (owned == Ownership::ShaderOnly)
                {
                    // The shader is still owned by this callback, so this cannot delete it.
                    lock.unlock();
                    release_candidate_shader(item.shader);
                    return false;
                }
                if (owned != Ownership::Owned)
                {
                    return false;
                }
                bool commit = true;
                if (slot < s_candidate_count)
                {
                    // The evicted pair's Release can be the last one (s_evicted): on the main thread it runs below,
                    // once the mutex is released, and elsewhere the main thread runs it later. With no room in
                    // s_evicted the evicted pair stays and the new one goes instead: its callback owns the item, so
                    // that Release is not the last one.
                    if (on_engine_main_thread())
                    {
                        evicted = s_candidates[slot];
                    }
                    else if (!s_evicted.push(s_candidates[slot].request))
                    {
                        evicted.request = {item.shader, item.resources, kind};
                        commit = false;
                    }
                    s_dropped_requests.fetch_add(1, std::memory_order_relaxed);
                }
                else
                {
                    ++s_candidate_count;
                }
                if (commit)
                {
                    s_candidates[slot] = TwinCandidate{.request = {item.shader, item.resources, kind},
                                                       .priority = priority,
                                                       .first_ms = GetTickCount64()};
                    added = true;
                    if (priority == 2)
                    {
                        s_render_miss_new.store(true, std::memory_order_relaxed);
                    }
                }
            }
            catch (...)
            {
                s_dropped_requests.fetch_add(1, std::memory_order_relaxed);
            }
            drop_candidate(evicted);
            return added;
        }

        void clear_candidates() noexcept
        {
            std::array<TwinCandidate, k_max_candidates> dropped{};
            std::size_t count = 0;
            try
            {
                const std::lock_guard lock(s_candidate_mutex);
                count = std::exchange(s_candidate_count, 0);
                std::copy_n(s_candidates.begin(), count, dropped.begin());
                std::fill_n(s_candidates.begin(), count, TwinCandidate{});
            }
            catch (...)
            {
                return; // Keep references if the ownership mutex cannot be acquired.
            }
            for (std::size_t i = 0; i < count; ++i)
            {
                drop_candidate(dropped[i]);
            }
        }

        /**
         * @brief Calls @p fn with each kind @p item's shader is a source of for the twin scope bits @p scopes: a
         *        kind whose scope those bits name, or with @p character_shaders any Dissolving kind that is not
         *        transparent-only (the character's Eye and Hair shaders, whatever the asset), and that the item fits
         *        (item_fits_kind()).
         * @note Reads the shader's name (and, for a transparent-only kind, the item's opacity through its resources),
         *       nothing else. Without Rules::TWIN_CHARACTER and @p character_shaders, only listed kinds come out,
         *       whose fit needs no call.
         */
        template <typename Fn>
        void for_each_twin_kind(const ShaderItem &item, std::uintptr_t scopes, bool character_shaders, Fn &&fn)
        {
            for (int kind = 0; kind < k_kinds; ++kind)
            {
                const auto scope = blobs::k_twins[kind].scope;
                const bool character_shader = character_shaders && scope == blobs::TwinScope::Dissolving &&
                                              !blobs::k_twins[kind].transparent_only;
                const auto needed = scope == blobs::TwinScope::ListedPlants   ? k_scope_plants
                                    : scope == blobs::TwinScope::ListedFrames ? k_scope_frames
                                                                              : k_scope_character;
                if (((scopes & needed) != 0 || character_shader) && source_kind_guarded(item.shader, scope) == kind &&
                    item_fits_kind(kind, item))
                {
                    fn(kind);
                }
            }
        }

        /** @brief Validates the renderer, public flush slot, and engine main-thread ID before engine work. */
        [[nodiscard]] std::uintptr_t loading_renderer() noexcept
        {
            const auto renderer =
                DMK::memory::read<std::uintptr_t>(DMK::Address{s_genv + Constants::GENV_RENDERER_OFFSET});
            if (!renderer || *renderer == 0)
            {
                return 0;
            }
            const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{*renderer});
            if (!vtable || !vtable_is(GameClass::Renderer, *vtable) ||
                validated_slot(*renderer, Constants::RENDERER_FLUSH_RT_COMMANDS_VTABLE_OFFSET, s_flush_rt_commands) ==
                    0)
            {
                return 0;
            }
            const DWORD main_thread = engine_main_thread_id(*renderer);
            return main_thread != 0 && main_thread == GetCurrentThreadId() ? *renderer : 0;
        }

        [[nodiscard]] bool flush_loading_commands(std::uintptr_t renderer) noexcept
        {
            __try
            {
                return reinterpret_cast<FlushRTCommandsFn>(s_flush_rt_commands)(renderer, true, true, true);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        [[nodiscard]] std::string lower_text(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), lower);
            return text;
        }

        /**
         * @brief @p path in UTF-8, as the engine's paths and the log are, whatever the system code page; a profile
         *        folder named with letters outside that code page neither throws nor comes out garbled.
         */
        [[nodiscard]] std::string utf8_text(const std::filesystem::path &path)
        {
            const std::u8string text = path.u8string();
            return {text.begin(), text.end()};
        }

        /**
         * @brief Checks the stock shader binary @p file in ShadersBin.pak against the header CRC the twins were
         *        built from.
         * @return An empty string when it matches, else what differs.
         */
        [[nodiscard]] std::string check_stock_header(const PakCalls &calls, std::uintptr_t pak, std::string_view file,
                                                     std::uint32_t expected_crc)
        {
            const std::string path = std::string(Constants::SHADER_ENGINE_CACHE_DIR) + std::string(file);
            std::array<std::uint8_t, Constants::SHADER_BIN_HEADER_SIZE> header{};
            switch (read_pak_header_guarded(calls, pak, path.c_str(), header.data()))
            {
            case HeaderRead::Missing:
                return std::format("{} is not in the game's shader binaries", path);
            case HeaderRead::Fault:
                return std::format("reading {} through CryPak faulted", path);
            case HeaderRead::Read:
            default:
                break;
            }
            std::uint32_t magic = 0;
            std::uint32_t crc = 0;
            std::uint16_t version_low = 0;
            std::uint16_t version_high = 0;
            std::memcpy(&magic, header.data(), sizeof(magic));
            std::memcpy(&crc, header.data() + 4, sizeof(crc));
            std::memcpy(&version_low, header.data() + 8, sizeof(version_low));
            std::memcpy(&version_high, header.data() + 10, sizeof(version_high));
            if (magic != Constants::SHADER_BIN_MAGIC || version_low != Constants::SHADER_BIN_VERSION_LOW ||
                version_high != Constants::SHADER_BIN_VERSION_HIGH)
            {
                return std::format("{} is not an FXB0 3.4 shader binary (magic {:#010x}, version {}.{})", path, magic,
                                   version_low, version_high);
            }
            if (crc != expected_crc)
            {
                return std::format("the game's {} changed (header CRC {:#010x}, the twins were built from {:#010x}); "
                                   "rebuild the twins with scripts/build_shader_twins.py",
                                   path, crc, expected_crc);
            }
            return {};
        }

        /**
         * @brief Writes @p blob to @p path unless the file there already starts with exactly it.
         * @details After it parses a shader permutation the engine appends its parse cache (the local info: for each
         *          gen mask, the parameters, samplers, textures and functions the parse kept) to the binary in the user
         *          cache, past the string table the header points it to, and leaves the header and the tokens as they
         *          were. A file that holds the blob followed by that cache is kept, so a later session's twin parses
         *          with it: rewriting the file dropped it, and every twin instance was parsed from scratch again,
         *          which stalled the render thread about 190 ms each.
         * @param written Set when the file was (re)written.
         * @return An empty string on success, else what went wrong.
         */
        [[nodiscard]] std::string write_blob(const std::filesystem::path &path, const blobs::Blob &blob, bool &written)
        {
            written = false;
            std::error_code error;
            if (const std::uintmax_t size = std::filesystem::file_size(path, error); !error && size >= blob.size)
            {
                std::ifstream existing(path, std::ios::binary);
                std::vector<char> bytes(blob.size);
                if (existing.read(bytes.data(), static_cast<std::streamsize>(bytes.size())) &&
                    std::equal(bytes.begin(), bytes.end(), blob.data,
                               [](char a, unsigned char b) { return static_cast<unsigned char>(a) == b; }))
                {
                    return {};
                }
            }
            std::filesystem::create_directories(path.parent_path(), error);
            if (error)
            {
                return std::format("cannot create {}: {}", utf8_text(path.parent_path()), error.message());
            }
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char *>(blob.data), static_cast<std::streamsize>(blob.size));
            out.close();
            if (!out)
            {
                return std::format("cannot write {}", utf8_text(path));
            }
            written = true;
            return {};
        }

        /**
         * @brief The lower-case name of @p entry when it is a regular file whose name starts with blobs::k_file_prefix,
         *        else nothing.
         * @details The mod's files have printable ASCII names, so a name with any other character is not one of them,
         *          and the test reads the native wide name with no code-page conversion.
         */
        [[nodiscard]] std::optional<std::string> mod_file_name(const std::filesystem::directory_entry &entry)
        {
            std::error_code error;
            if (!entry.is_regular_file(error))
            {
                return std::nullopt;
            }
            const std::wstring wide = entry.path().filename().wstring();
            if (std::ranges::any_of(wide, [](wchar_t c) { return c < 0x20 || c > 0x7E; }))
            {
                return std::nullopt;
            }
            std::string name(wide.size(), '\0');
            std::ranges::transform(wide, name.begin(), [](wchar_t c) { return lower(static_cast<char>(c)); });
            if (!name.starts_with(blobs::k_file_prefix))
            {
                return std::nullopt;
            }
            return name;
        }

        /**
         * @brief Deletes the mod's shader binaries in @p directory that this build does not ship (another CRC in the
         *        name).
         * @details Only files of the directory itself that start with blobs::k_file_prefix and end in .cfxb or .cfib;
         *          the engine's compiled caches sit in subfolders and are left alone.
         * @return The number of files removed.
         */
        [[nodiscard]] int remove_stale_blobs(const std::filesystem::path &directory)
        {
            int removed = 0;
            std::error_code error;
            // Iterated with error codes, so a folder that cannot be read ends the sweep instead of throwing.
            for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end;
                 it.increment(error))
            {
                const std::optional<std::string> name = mod_file_name(*it);
                if (!name)
                {
                    continue;
                }
                const bool ours = name->ends_with(".cfxb") || name->ends_with(".cfib");
                const bool shipped = std::ranges::any_of(blobs::k_blobs, [&name](const blobs::Blob &blob)
                                                         { return *name == blob.file_name; });
                std::error_code remove_error;
                if (ours && !shipped && std::filesystem::remove(it->path(), remove_error))
                {
                    DMK::log().info("ShaderTwins: removed the stale twin file {}", utf8_text(it->path()));
                    ++removed;
                }
            }
            return removed;
        }

        /**
         * @brief Deletes the engine's compiled entries of the mod's twins that this build does not ship, in the
         *        compiled-shader folders under @p directory (Constants::SHADER_COMPILED_CACHE_DIRS).
         * @details An entry is "<shader name>@<entry>"; only one whose shader name starts with blobs::k_file_prefix
         *          and is no current twin's (another CRC: an older build of the mod, or a game patch the twins were
         *          rebuilt for) is deleted. Those names are never loaded again, so their compiled permutations only
         *          take room. An entry the engine still holds open (a twin of an older build loaded earlier in this
         *          session) cannot be deleted and is left for the next start.
         * @param stale_twins Set to the distinct twin names removed, comma-separated, for the log.
         * @return The number of entries removed.
         */
        [[nodiscard]] int remove_stale_compiled(const std::filesystem::path &directory, std::string &stale_twins)
        {
            std::array<std::string, k_kinds> current{};
            for (int kind = 0; kind < k_kinds; ++kind)
            {
                current[static_cast<std::size_t>(kind)] = lower_text(std::string(blobs::k_twins[kind].twin_name));
            }
            int removed = 0;
            for (const std::string_view folder : Constants::SHADER_COMPILED_CACHE_DIRS)
            {
                // Iterated with error codes, so a folder that cannot be read is skipped instead of throwing.
                std::error_code error;
                for (std::filesystem::directory_iterator it(directory / folder, error), end; !error && it != end;
                     it.increment(error))
                {
                    const std::optional<std::string> name = mod_file_name(*it);
                    const std::size_t at = name ? name->find('@') : std::string::npos;
                    if (at == std::string::npos)
                    {
                        continue;
                    }
                    const std::string twin = name->substr(0, at);
                    std::error_code remove_error;
                    if (std::ranges::find(current, twin) != current.end() ||
                        !std::filesystem::remove(it->path(), remove_error))
                    {
                        continue;
                    }
                    ++removed;
                    if (stale_twins.find(twin) == std::string::npos)
                    {
                        stale_twins += std::format("{}{}", stale_twins.empty() ? "" : ", ", twin);
                    }
                }
            }
            return removed;
        }

        /**
         * @brief Reads the dissolve's RT-mask bit (the DissolveRtBit global) into s_dissolve_rt_bit, which the sets
         *        built ahead for the fade add to their RT mask.
         * @details The renderer fills the global at startup, long before a third-person frame. It must be one bit of
         *          the low 32 (AddRenderObject ORs its low dword into the render object's 32-bit mask); anything else,
         *          or no global, leaves it 0, and the sets built ahead then carry the dissolve in their flags only: the
         *          stages add the same bit for FOB_DISSOLVE, so the PSOs they ask for are the same, and only the
         *          resources' local PSO cache keys them apart from the fading build's.
         * @note Main thread, before any set is built ahead.
         */
        void read_dissolve_rt_bit() noexcept
        {
            if (s_dissolve_rt_global == 0)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Warning,
                    "ShaderTwins: the dissolve's RT-mask bit did not resolve; the character twins' "
                    "sets for the fade are built ahead with the dissolve in their flags only");
                return;
            }
            const auto bit = DMK::memory::read<std::uint64_t>(DMK::Address{s_dissolve_rt_global});
            if (!bit || *bit == 0 || *bit > 0xFFFFFFFFull || (*bit & (*bit - 1)) != 0)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Warning,
                    "ShaderTwins: the dissolve's RT-mask bit at {:#x} reads {:#x}, not one bit of the low 32; the "
                    "character twins' sets for the fade are built ahead with the dissolve in their flags only",
                    s_dissolve_rt_global, bit ? *bit : 0);
                return;
            }
            s_dissolve_rt_bit.store(*bit, std::memory_order_relaxed);
            (void)DMK::log().try_log(
                DMK::LogLevel::Trace,
                "ShaderTwins: the character twins' sets for the fade are built ahead with FOB_DISSOLVE {:#x} and "
                "FOB_DISSOLVE_OUT {:#x} in the flags and the dissolve bit {:#x} in the RT mask",
                Constants::RENDER_OBJECT_FLAG_DISSOLVE, Constants::RENDER_OBJECT_FLAG_DISSOLVE_OUT, *bit);
        }

        /**
         * @brief Proves the engine interfaces, checks the stock shaders the twins were built from, and writes the
         *        twins' files to the user shader cache.
         * @return An empty string on success, else why the feature cannot run.
         * @throws std::bad_alloc, std::filesystem::filesystem_error on allocation or file-system failure.
         */
        [[nodiscard]] std::string prepare_engine(std::uintptr_t pak, std::uintptr_t renderer)
        {
            DMK_PROFILE_SCOPE("twins.load_prepare");
            DMK::Logger &logger = DMK::log();
            const auto renderer_vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{renderer});
            if (!renderer_vtable || !vtable_is(GameClass::Renderer, *renderer_vtable))
            {
                return "g_env->pRenderer is not a CD3D9Renderer";
            }
            const std::uintptr_t manager = renderer + Constants::RENDERER_SHADER_MAN_OFFSET;
            std::array<char, 32> shaders_path{};
            copy_text_field_guarded(manager, Constants::SHADER_MAN_SHADERS_PATH_OFFSET, shaders_path.data(),
                                    shaders_path.size());
            if (std::string_view{shaders_path.data()} != Constants::SHADER_MAN_SHADERS_PATH)
            {
                return std::format("the renderer's shader manager did not check out (m_ShadersPath \"{}\")",
                                   shaders_path.data());
            }
            if (!object_has_type(pak, Constants::CRY_PAK_RTTI_NAME))
            {
                return "g_env->pCryPak is not a CCryPak";
            }
            const std::uintptr_t adjust =
                validated_slot(pak, Constants::CRY_PAK_VTABLE_ADJUST_FILE_NAME_OFFSET, s_adjust_file_name);
            const PakCalls calls{
                .open = image_slot(pak, s_pak_open_slot),
                .read = image_slot(pak, s_pak_read_slot),
                .close = image_slot(pak, s_pak_close_slot),
            };
            if (adjust == 0 || calls.open == 0 || calls.read == 0 || calls.close == 0)
            {
                return "a CryPak vtable slot did not check out (AdjustFileName, FOpen, FReadRaw or FClose)";
            }

            // The generated header must ship every twin's binary.
            for (const blobs::Twin &twin : blobs::k_twins)
            {
                const std::string twin_file = lower_text(std::string(twin.twin_name)) + ".cfxb";
                if (std::ranges::none_of(blobs::k_blobs,
                                         [&twin_file](const blobs::Blob &blob) { return twin_file == blob.file_name; }))
                {
                    return std::format("the generated shader header has no {}", twin_file);
                }
            }

            // Every plant and frame twin copies commonzpass.cfib and commonzprepass.cfib, and every twin its own stock
            // shader. A game patch that changed one of them would leave a twin out of step with the gen flags it
            // borrows: a changed Z pass turns the plant and frame twins off, and a changed stock shader only its own
            // twin, whose items keep the stock shader. The character twins keep the stock includes, which their own
            // source CRCs cover.
            const std::array<std::pair<std::string_view, std::uint32_t>, 2> passes{{
                {"commonzpass.cfib", blobs::k_source_commonzpass_crc},
                {"commonzprepass.cfib", blobs::k_source_commonzprepass_crc},
            }};
            for (const auto &[file, crc] : passes)
            {
                if (std::string error = check_stock_header(calls, pak, file, crc); !error.empty())
                {
                    for (int kind = 0; kind < k_kinds; ++kind)
                    {
                        if (listed_scope(blobs::k_twins[kind].scope))
                        {
                            s_kind_failed[static_cast<std::size_t>(kind)] = true;
                        }
                    }
                    logger.warning("ShaderTwins: {}; the plants and frames keep the stock shaders (no see-through, no "
                                   "near cut)",
                                   error);
                }
            }
            std::string twin_names;
            for (int kind = 0; kind < k_kinds; ++kind)
            {
                const blobs::Twin &twin = blobs::k_twins[kind];
                if (s_kind_failed[static_cast<std::size_t>(kind)])
                {
                    continue;
                }
                if (std::string error = check_stock_header(calls, pak, twin.source_file, twin.source_crc);
                    !error.empty())
                {
                    s_kind_failed[static_cast<std::size_t>(kind)] = true;
                    logger.warning("ShaderTwins: {}; the {} twin is off and its items keep the stock shader", error,
                                   twin.label);
                    continue;
                }
                twin_names += std::format("{}{} ({})", twin_names.empty() ? "" : ", ", twin.twin_name, twin.label);
            }
            if (twin_names.empty())
            {
                return "no stock shader the twins were built from matches the game's";
            }

            // The user shader cache, through the engine's %USER% alias, so a relocated Saved Games folder works.
            std::filesystem::path directory;
            int written_count = 0;
            for (const blobs::Blob &blob : blobs::k_blobs)
            {
                const std::string source = std::string(Constants::SHADER_USER_CACHE_DIR) + blob.file_name;
                std::array<char, Constants::CRY_PAK_MAX_PATH> resolved{};
                if (!adjust_file_name_guarded(adjust, pak, source.c_str(), resolved.data(),
                                              Constants::CRY_PAK_FLAGS_PATH_REAL |
                                                  Constants::CRY_PAK_FLAGS_FOR_WRITING))
                {
                    return std::format("{} did not resolve (CryPak AdjustFileName)", source);
                }
                resolved.back() = '\0';
                // CryPak resolves %USER% to UTF-8 (the known folder through its UTF-8 encoder) and opens files by
                // converting UTF-8 to wide, so the path is read as UTF-8, not in the system code page.
                const std::filesystem::path path{
                    std::u8string_view{reinterpret_cast<const char8_t *>(resolved.data())}};
                bool written = false;
                if (std::string error = write_blob(path, blob, written); !error.empty())
                {
                    return error;
                }
                if (written)
                {
                    ++written_count;
                    logger.info("ShaderTwins: wrote {} ({} bytes)", utf8_text(path), blob.size);
                }
                directory = path.parent_path();
            }
            const int removed = remove_stale_blobs(directory);
            // The compiled permutations of the twins an older build shipped: before any twin of this build is created,
            // so none of its own entries is open yet.
            std::string stale_twins;
            const int removed_compiled = remove_stale_compiled(directory, stale_twins);
            if (removed_compiled != 0)
            {
                logger.info("ShaderTwins: removed {} compiled shader entr{} of twin(s) this build does not ship ({})",
                            removed_compiled, removed_compiled == 1 ? "y" : "ies", stale_twins);
            }

            s_shader_manager = manager;
            logger.info(
                "ShaderTwins: {} twin file(s) in {} ({} written, {} stale removed); twins {}, whose stock shaders "
                "(and Z passes, for the plants) match the game's",
                std::size(blobs::k_blobs), utf8_text(directory), written_count, removed, twin_names);
            read_dissolve_rt_bit();
            return {};
        }

        /** @brief The track index of @p node, or -1. */
        [[nodiscard]] int find_track(std::uintptr_t node) noexcept
        {
            for (int i = 0; i < s_track_count; ++i)
            {
                if (s_tracks[static_cast<std::size_t>(i)].node == node)
                {
                    return i;
                }
            }
            return -1;
        }

        /** @brief The source index of @p shader as a source of @p kind, or -1. */
        [[nodiscard]] int find_source(std::uintptr_t shader, int kind) noexcept
        {
            for (int i = 0; i < s_source_count; ++i)
            {
                const Source &source = s_sources[static_cast<std::size_t>(i)];
                if (source.shader == shader && source.kind == kind)
                {
                    return i;
                }
            }
            return -1;
        }

        [[nodiscard]] const char *invalidate_text(Invalidate result) noexcept
        {
            switch (result)
            {
            case Invalidate::Done:
                return "refills next frame";
            case Invalidate::NoRender:
                return "no render data yet";
            case Invalidate::NotTheNode:
                return "no longer live";
            case Invalidate::Fault:
            default:
                return "fault";
            }
        }

        /** @brief Drops a held record's references. */
        void drop_record(const PsoRecord &record) noexcept
        {
            drop_pso_refs(record.stock);
            drop_pso_refs(record.twin);
        }

        /** @brief Drops the records of @p node, whose compiled objects are about to be rebuilt. */
        void drop_records(std::uintptr_t node) noexcept
        {
            int kept = 0;
            for (int i = 0; i < s_pending_count; ++i)
            {
                const PendingPsos &pending = s_pending[static_cast<std::size_t>(i)];
                if (pending.record.node == node)
                {
                    trace_event(trace::Event::RecordDropped, node, pending.record.object, pending.record.twin_shader, 0,
                                0, pending.first_ms);
                    drop_record(pending.record);
                    continue;
                }
                if (kept != i)
                {
                    s_pending[static_cast<std::size_t>(kept)] = pending;
                }
                ++kept;
            }
            s_pending_count = kept;
        }

        /**
         * @brief Invalidates the track's node, so its render objects refill with (or without) the twin. The rebuild
         *        hands over new waits for every compiled object of it, so a retry it was due is done.
         */
        Invalidate refill(Track &track, trace::RefillReason reason) noexcept
        {
            trace_event(trace::Event::RefillBegin, track.node, 0, 0, 0, static_cast<std::uint64_t>(reason));
            track.retry = false;
            track.deferred = false;
            drop_records(track.node);
            const Invalidate result = invalidate_node_guarded(track.node);
            trace_event(trace::Event::RefillEnd, track.node, 0, 0, 0, static_cast<std::uint64_t>(reason),
                        static_cast<std::uint64_t>(result));
            if (result == Invalidate::Fault)
            {
                s_guard_faults.fetch_add(1, std::memory_order_relaxed);
            }
            return result;
        }

        /** @brief Swaps the track's node back: unpublished by the caller, then refilled with the stock shader. */
        void release_track(Track &track, std::uint64_t now, const char *why) noexcept
        {
            trace_event(trace::Event::TrackReleased, track.node, 0, 0, 0, track.activated);
            if (!track.activated)
            {
                drop_records(track.node);
                return;
            }
            const Invalidate result = refill(track, trace::RefillReason::Release);
            (void)DMK::log().try_log(DMK::LogLevel::Debug, "ShaderTwins: swapped off {} ({}, {:.1f} s, {})",
                                     track.name.data(), why, static_cast<double>(now - track.swapped_ms) / 1000.0,
                                     invalidate_text(result));
        }

        /** @brief Swaps every node back and empties the set. */
        void release_all(std::uint64_t now, const char *why) noexcept
        {
            if constexpr (trace::enabled)
                trace::cancel_candidates(s_frame_seq.load(std::memory_order_relaxed));
            s_set_published.store(nullptr, std::memory_order_release);
            s_rows_published.store(nullptr, std::memory_order_release);
            for (int i = 0; i < s_released_count; ++i)
            {
                Track &released = s_released[static_cast<std::size_t>(i)];
                release_track(released, now, released.why);
            }
            s_released_count = 0;
            for (int i = 0; i < s_track_count; ++i)
            {
                release_track(s_tracks[static_cast<std::size_t>(i)], now, why);
            }
            s_track_count = 0;
            s_refill_all = false;
        }

        /** @brief Turns the feature off for the session, swaps every node back and says why, once. */
        void turn_off(std::uint64_t now, const char *why) noexcept
        {
            if (s_disabled.exchange(true, std::memory_order_acq_rel))
            {
                return;
            }
            s_eye_ready.store(false, std::memory_order_release);
            s_character_ready.store(false, std::memory_order_release);
            release_all(now, "feature off");
            (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                     "ShaderTwins: {}; trees and the character keep the stock shaders (no see-through, "
                                     "no character fade twins) for the session",
                                     why);
        }

        /** @brief True while a kind of @p scope has not failed. */
        [[nodiscard]] bool scope_usable(blobs::TwinScope scope) noexcept
        {
            for (int kind = 0; kind < k_kinds; ++kind)
            {
                if (blobs::k_twins[kind].scope == scope && !s_kind_failed[static_cast<std::size_t>(kind)])
                {
                    return true;
                }
            }
            return false;
        }

        /** @brief True while a plant or a frame kind has not failed, so the level inventory has pairs to queue. */
        [[nodiscard]] bool listed_scopes_usable() noexcept
        {
            return scope_usable(blobs::TwinScope::ListedPlants) || scope_usable(blobs::TwinScope::ListedFrames);
        }

        /**
         * @brief True for a candidate's kind that gets no twin (s_kind_failed, or no kind at all): it is never queued
         *        from the level inventory nor created during a pause, and at a loading boundary it ranks below every
         *        other candidate.
         * @note Main thread: s_kind_failed is written only there.
         */
        [[nodiscard]] bool kind_off(int kind) noexcept
        {
            return kind < 0 || kind >= k_kinds || s_kind_failed[static_cast<std::size_t>(kind)];
        }

        /** @brief True when this scope has a parsed twin that render items can actually select. */
        [[nodiscard]] bool scope_ready(blobs::TwinScope scope) noexcept
        {
            for (int i = 0; i < s_source_count; ++i)
            {
                const Source &source = s_sources[static_cast<std::size_t>(i)];
                if (source.state == TwinState::Ready && blobs::k_twins[source.kind].scope == scope &&
                    !s_kind_failed[static_cast<std::size_t>(source.kind)])
                {
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief Stops the plant and frame twins for the session (the window's rows are in use): every tree and frame
         *        swaps back, their kinds fail, and the character fade's twins go on; the whole feature turns off when
         *        no kind is left.
         */
        void stop_plants(std::uint64_t now, const char *why) noexcept
        {
            if (!scope_usable(blobs::TwinScope::ListedPlants) && !scope_usable(blobs::TwinScope::ListedFrames))
            {
                return;
            }
            for (int kind = 0; kind < k_kinds; ++kind)
            {
                if (listed_scope(blobs::k_twins[kind].scope))
                {
                    s_kind_failed[static_cast<std::size_t>(kind)] = true;
                }
            }
            if (!scope_usable(blobs::TwinScope::Dissolving))
            {
                turn_off(now, why);
                return;
            }
            release_all(now, "plants off");
            (void)DMK::log().try_log(
                DMK::LogLevel::Warning,
                "ShaderTwins: {}; trees and frames keep the stock shader (no see-through, no near cut) for "
                "the session, the character fade's twins go on",
                why);
        }

        /**
         * @brief Prepares the twins inside a verified loading transaction, once the engine is up.
         * @return True once prepared; false while waiting for the engine (retried every
         *         SHADER_TWIN_PREPARE_RETRY_MS) or after a failure (the feature is then off).
         */
        [[nodiscard]] bool prepare(std::uint64_t now) noexcept
        {
            if (s_prepare != Prepare::Pending)
            {
                return s_prepare == Prepare::Ready;
            }
            if (now < s_prepare_retry_ms)
            {
                return false;
            }
            s_prepare_retry_ms = now + Constants::SHADER_TWIN_PREPARE_RETRY_MS;
            const auto pak = DMK::memory::read<std::uintptr_t>(DMK::Address{s_genv + Constants::GENV_CRY_PAK_OFFSET});
            const auto renderer =
                DMK::memory::read<std::uintptr_t>(DMK::Address{s_genv + Constants::GENV_RENDERER_OFFSET});
            if (!pak || !renderer || *pak == 0 || *renderer == 0)
            {
                (void)DMK::log().try_log(DMK::LogLevel::Debug, "ShaderTwins: waiting for g_env's CryPak and renderer");
                return false;
            }
            std::string error;
            try
            {
                error = prepare_engine(*pak, *renderer);
            }
            catch (const std::exception &e)
            {
                error = std::string("preparing the twins threw: ") + e.what();
            }
            catch (...)
            {
                error = "preparing the twins threw";
            }
            if (!error.empty())
            {
                s_prepare = Prepare::Failed;
                turn_off(now, error.c_str());
                return false;
            }
            s_prepare = Prepare::Ready;
            return true;
        }

        /**
         * @brief Creates an owned candidate's twin inside the loading transaction, or in a frame the game stands still
         *        in (create_during_pause()).
         * @details The twin is created the way the material path creates its source (EF_LoadShaderItem -> mfForName):
         *          flags 0, the material's CShaderResources and the source's gen mask. A request whose shader or
         *          resources are no longer what they were takes no slot, so a live shader at that address later still
         *          gets its twin. A source that takes a slot keeps one more reference for the session, whatever becomes
         *          of its twin, so its address can never be reused by another shader while the swap set names it, and
         *          the twin's reference is never released, because persistent render objects name it by id and pooled
         *          compiled objects keep pointers to it. A source of a failed kind (s_kind_failed) keeps its stock
         *          shader. One shader instance can be the source of a kind of each scope (an Illum instance of a plant
         *          twin and of the IllumFade twin), each with an entry of its own.
         */
        void create_twin(const TwinRequest &request, std::uint64_t now) noexcept
        {
            // Only finalize_loading(), or create_during_pause() in a frame the game stands still in, may create a
            // shader, on the verified engine main thread. In particular a retained gameplay request is not a permit,
            // and a source still parsing must not reserve a Failed entry permanently.
            if ((!s_loading_finalizing.load(std::memory_order_acquire) && !s_pause_permit) || loading_renderer() == 0)
            {
                return;
            }
            if (request.kind < 0 || request.kind >= k_kinds || find_source(request.shader, request.kind) >= 0)
            {
                return;
            }
            DMK_PROFILE_SCOPE("twins.create");
            if (s_source_count >= k_max_sources)
            {
                if (!s_sources_full_logged)
                {
                    s_sources_full_logged = true;
                    (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                             "ShaderTwins: {} source shader instances have twins already; items with "
                                             "another material keep the stock shader",
                                             k_max_sources);
                }
                return;
            }
            decltype(Source::name) name{};
            copy_text_field_guarded(request.shader, Constants::SHADER_RESOURCE_NAME_OFFSET, name.data(), name.size());

            const auto flags =
                DMK::memory::read<std::uint32_t>(DMK::Address{request.shader + Constants::SHADER_FLAGS_OFFSET});
            const auto mask =
                DMK::memory::read<std::uint64_t>(DMK::Address{request.shader + Constants::SHADER_GEN_MASK_OFFSET});
            const auto techniques = DMK::memory::read<std::uint32_t>(
                DMK::Address{request.shader + Constants::SHADER_TECHNIQUE_COUNT_OFFSET});
            if (!flags || (*flags & Constants::SHADER_FLAG_LOADED) == 0 ||
                (*flags & Constants::SHADER_FLAG_NOT_FOUND) != 0 || !mask || !techniques || *techniques == 0)
            {
                return;
            }
            // The candidate owns both pointers; the source table takes its own shader reference before it can ever
            // publish the identity. Resource identity is checked again before mfForName uses its texture flags.
            const int kind = is_shader(request.shader)
                                 ? source_kind_guarded(request.shader, blobs::k_twins[request.kind].scope)
                                 : -1;
            if (kind != request.kind || (request.resources != 0 && !is_shader_resources(request.resources)) ||
                !add_shader_ref_guarded(request.shader))
            {
                ++s_stale_requests;
                (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                         "ShaderTwins: shader {:#x} ({}) is no longer a source instance with its "
                                         "resources; no twin",
                                         request.shader, name.data());
                return;
            }
            Source &source = s_sources[static_cast<std::size_t>(s_source_count++)];
            source = Source{
                .shader = request.shader, .kind = kind, .created_ms = now, .state = TwinState::Failed, .name = name};
            const blobs::Twin &twin = blobs::k_twins[kind];
            if (s_kind_failed[static_cast<std::size_t>(kind)])
            {
                return;
            }
            source.mask = *mask;
            source.techniques = *techniques;
            trace_event(trace::Event::SourceRequest, 0, 0, source.shader, request.resources, source.mask, kind);
            source.twin =
                for_name_guarded(s_for_name, s_shader_manager, twin.twin_name, request.resources, source.mask);
            trace_event(trace::Event::SourceResult, 0, source.twin, source.shader, request.resources, source.mask,
                        kind);
            if (source.twin == 0)
            {
                s_guard_faults.fetch_add(1, std::memory_order_relaxed);
                (void)DMK::log().try_log(DMK::LogLevel::Warning, "ShaderTwins: mfForName({}) failed for {}; no twin",
                                         twin.twin_name, source.name.data());
                return;
            }
            source.state = TwinState::Pending;
            (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                     "ShaderTwins: {} twin {:#x} requested for {} (CShader {:#x}, gen mask {:#x}, "
                                     "{} techniques, resources {:#x})",
                                     twin.label, source.twin, source.name.data(), source.shader, source.mask,
                                     source.techniques, request.resources);
        }

        /**
         * @brief Moves pending twins to ready (or gives them up) and refills the swapped nodes when a plant twin became
         *        ready.
         * @details A twin is ready once its parse finished (EF_LOADED, set only after every technique's pass links and
         *          flags are written; the technique count is final earlier) with as many techniques as its source and
         *          it is not EF_NOTFOUND. Its id must fit the 11 bits the persistent key packs it in, and its final gen
         *          mask must equal its source's, or its permutations would not be the source's. It then takes the
         *          feature bits its source got from its name (match_feature_flags_guarded()), before any item names it.
         *          A twin that did not load at all (EF_NOTFOUND) means its kind's files were refused: that kind gets no
         *          more twins, and the feature turns off once no kind is left.
         */
        void refresh_twins(std::uint64_t now) noexcept
        {
            bool newly_ready = false;
            for (int i = 0; i < s_source_count; ++i)
            {
                Source &source = s_sources[static_cast<std::size_t>(i)];
                if (source.state != TwinState::Pending)
                {
                    continue;
                }
                const auto flags =
                    DMK::memory::read<std::uint32_t>(DMK::Address{source.twin + Constants::SHADER_FLAGS_OFFSET});
                const auto techniques = DMK::memory::read<std::uint32_t>(
                    DMK::Address{source.twin + Constants::SHADER_TECHNIQUE_COUNT_OFFSET});
                const auto id =
                    DMK::memory::read<std::uint32_t>(DMK::Address{source.twin + Constants::SHADER_ID_OFFSET});
                const auto mask =
                    DMK::memory::read<std::uint64_t>(DMK::Address{source.twin + Constants::SHADER_GEN_MASK_OFFSET});
                const auto flags2 =
                    DMK::memory::read<std::uint32_t>(DMK::Address{source.twin + Constants::SHADER_FLAGS2_OFFSET});
                const auto source_flags2 =
                    DMK::memory::read<std::uint32_t>(DMK::Address{source.shader + Constants::SHADER_FLAGS2_OFFSET});
                const double waited = static_cast<double>(now - source.created_ms) / 1000.0;
                const blobs::Twin &twin = blobs::k_twins[source.kind];
                if (!flags || !techniques || !id || !mask || !flags2 || !source_flags2)
                {
                    source.state = TwinState::Failed;
                    (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                             "ShaderTwins: {} twin {:#x} for {} could not be read", twin.label,
                                             source.twin, source.name.data());
                    continue;
                }
                if ((*flags & Constants::SHADER_FLAG_NOT_FOUND) != 0)
                {
                    source.state = TwinState::Failed;
                    if (!std::exchange(s_kind_failed[static_cast<std::size_t>(source.kind)], true))
                    {
                        (void)DMK::log().try_log(DMK::LogLevel::Error,
                                                 "ShaderTwins: twin {} did not load from the user shader cache "
                                                 "(EF_NOTFOUND, flags {:#x}, after {:.2f} s); the {} twin's items keep "
                                                 "the stock shader",
                                                 twin.twin_name, *flags, waited, twin.label);
                    }
                    if (std::ranges::all_of(s_kind_failed, [](bool failed) { return failed; }))
                    {
                        turn_off(now, "no twin shader loaded");
                        return;
                    }
                    continue;
                }
                const bool loaded = (*flags & Constants::SHADER_FLAG_LOADED) != 0;
                std::uint16_t features_before = 0;
                std::uint16_t features = 0;
                if (loaded && *techniques != source.techniques)
                {
                    source.state = TwinState::Failed;
                    (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                             "ShaderTwins: {} twin {:#x} for {} parsed with {} techniques, its source "
                                             "has {} (flags {:#x}, after {:.2f} s); items with this material keep the "
                                             "stock shader",
                                             twin.label, source.twin, source.name.data(), *techniques,
                                             source.techniques, *flags, waited);
                }
                else if (loaded)
                {
                    if (*id >= Constants::SHADER_MAX_ID)
                    {
                        source.state = TwinState::Failed;
                        (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                                 "ShaderTwins: {} twin {:#x} for {} has shader id {}, past the {} a "
                                                 "render object can name; items with this material keep the stock "
                                                 "shader",
                                                 twin.label, source.twin, source.name.data(), *id,
                                                 Constants::SHADER_MAX_ID);
                    }
                    else if (*mask != source.mask)
                    {
                        source.state = TwinState::Failed;
                        (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                                 "ShaderTwins: {} twin {:#x} for {} has gen mask {:#x}, its source "
                                                 "{:#x}; items with this material keep the stock shader",
                                                 twin.label, source.twin, source.name.data(), *mask, source.mask);
                    }
                    else if (((*flags2 ^ *source_flags2) & Constants::SHADER_FLAG2_HAIR) != 0)
                    {
                        source.state = TwinState::Failed;
                        (void)DMK::log().try_log(
                            DMK::LogLevel::Warning,
                            "ShaderTwins: {} twin {:#x} for {} has flags2 {:#x}, its source {:#x}: "
                            "the hair flag (EF2_HAIR) differs, so its items would draw in another "
                            "render list; items with this material keep the stock shader",
                            twin.label, source.twin, source.name.data(), *flags2, *source_flags2);
                    }
                    else if (!match_feature_flags_guarded(source.twin, source.shader, features_before, features))
                    {
                        source.state = TwinState::Failed;
                        s_guard_faults.fetch_add(1, std::memory_order_relaxed);
                        (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                                 "ShaderTwins: {} twin {:#x} for {}: its feature word could not be "
                                                 "matched to its source's; items with this material keep the stock "
                                                 "shader",
                                                 twin.label, source.twin, source.name.data());
                    }
                    else
                    {
                        source.state = TwinState::Ready;
                        source.ready_ms = now;
#if defined(TPVCAMERA_DEV_BUILD)
                        ++s_usable_generation;
#endif
                        // The reloads so far, so revalidate_twins() notices only a later one.
                        ShaderState stock_state{};
                        ShaderState twin_state{};
                        source.reload_bits = read_shader_state_guarded(source.shader, stock_state) &&
                                                     read_shader_state_guarded(source.twin, twin_state)
                                                 ? reload_bits(stock_state, twin_state)
                                                 : 0;
                        source.stale = false;
                        // A plant or frame twin refills the swapped nodes, and the PSO guard learns it before any item
                        // can name it (the swap set that carries it is published after this). A character twin goes in
                        // the warm-up table instead, also before the set that carries it: its items, added again every
                        // frame, take it at rest without a refill, and their builds then prepare its sets for the fade.
                        const bool plant = listed_scope(twin.scope);
                        newly_ready = newly_ready || plant;
                        const int at = plant ? s_guard_count.load(std::memory_order_relaxed)
                                             : s_warm_count.load(std::memory_order_relaxed);
                        if (plant && at < k_max_sources)
                        {
                            s_guard_twins[static_cast<std::size_t>(at)].store(source.twin, std::memory_order_relaxed);
                            s_guard_sources[static_cast<std::size_t>(at)].store(source.shader,
                                                                                std::memory_order_relaxed);
                            s_guard_prepass[static_cast<std::size_t>(at)].store(prepass_kind(source.kind),
                                                                                std::memory_order_relaxed);
#if defined(TPVCAMERA_DEV_BUILD) && defined(DMK_ENABLE_PROFILING)
                            s_trace_guard_kinds[static_cast<std::size_t>(at)].store(
                                static_cast<std::uint8_t>(source.kind), std::memory_order_relaxed);
#endif
                            s_guard_count.store(at + 1, std::memory_order_release);
                        }
                        else if (!plant && at < k_max_sources)
                        {
                            source.warm_slot = at;
                            s_warm_twins[static_cast<std::size_t>(at)].store(source.twin, std::memory_order_relaxed);
                            s_warm_sources[static_cast<std::size_t>(at)].store(source.shader,
                                                                               std::memory_order_relaxed);
                            s_warm_transparent_only[static_cast<std::size_t>(at)].store(twin.transparent_only,
                                                                                        std::memory_order_relaxed);
                            s_warm_done[static_cast<std::size_t>(at)].store(false, std::memory_order_relaxed);
                            s_warm_cold[static_cast<std::size_t>(at)].store(false, std::memory_order_relaxed);
                            s_warm_count.store(at + 1, std::memory_order_release);
                        }
                        (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                                 "ShaderTwins: {} twin {:#x} ready for {} after {:.2f} s (id {}, {} "
                                                 "techniques, flags {:#x}, flags2 {:#x} (source {:#x}), gen mask "
                                                 "{:#x}, feature bits {:#x}, {:#x} before it took its source's)",
                                                 twin.label, source.twin, source.name.data(), waited, *id, *techniques,
                                                 *flags, *flags2, *source_flags2, *mask, features, features_before);
                    }
                }
                else if (waited > Constants::SHADER_TWIN_TIMEOUT_SECONDS)
                {
                    source.state = TwinState::Failed;
                    (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                             "ShaderTwins: {} twin {:#x} for {} not parsed after {:.0f} s ({} of {} "
                                             "techniques, flags {:#x}); items with this material keep the stock shader",
                                             twin.label, source.twin, source.name.data(), waited, *techniques,
                                             source.techniques, *flags);
                }
            }
            // The swapped trees refilled with their source while the twin parsed; they refill with it once the set that
            // carries it is published.
            s_refill_all = s_refill_all || newly_ready;
        }

        /**
         * @brief Takes a ready twin off its items while the engine reloads or changes it or its source, and gives it
         *        back once both check out again.
         * @details A shader-quality change or a shader file reload makes the engine free and parse instances again in
         *          place (EF_RELOADED on the first reload; the technique count, and the gen mask any later mfForName of
         *          the name rewrites), and a twin's parse rebuilds its feature word from its own name. A ready twin
         *          whose source or twin reads back with another gen mask or technique count, unparsed, EF_NOTFOUND,
         *          with EF_RELOADED newly set, or whose twin lacks a feature bit of its source goes stale: the swap set
         *          leaves it out, so its items keep the stock shader, and the swapped nodes refill. EF_RELOADED marks
         *          only the first reload, so the feature word is what shows a later reparse of the twin that finished
         *          between two camera frames with the same technique count and gen mask. Once both have read back
         *          parsed and unchanged for SHADER_TWIN_REVALIDATE_MS with the same gen mask, technique count and hair
         *          flag (the feature word aside, which only the twin's own name sets), the twin takes its source's
         *          feature bits again, is ready with the new values, and the nodes refill with it; one that never
         *          checks out keeps the stock shader for the session. Nothing is parsed or created here.
         * @note Main thread, each camera frame, after refresh_twins().
         */
        void revalidate_twins(std::uint64_t now) noexcept
        {
            int went_stale = 0;
            int revalidated = 0;
            const Source *first_stale = nullptr;
            ShaderState first_stock{};
            ShaderState first_twin{};
            for (int i = 0; i < s_source_count; ++i)
            {
                Source &source = s_sources[static_cast<std::size_t>(i)];
                if (source.state != TwinState::Ready)
                {
                    continue;
                }
                ShaderState stock{};
                ShaderState twin{};
                const bool read =
                    read_shader_state_guarded(source.shader, stock) && read_shader_state_guarded(source.twin, twin);
                const bool consistent = read && shader_parsed(stock) && shader_parsed(twin) &&
                                        twin.techniques == stock.techniques && twin.mask == stock.mask &&
                                        ((twin.flags2 ^ stock.flags2) & Constants::SHADER_FLAG2_HAIR) == 0;
                const bool unchanged = consistent && stock.techniques == source.techniques &&
                                       stock.mask == source.mask && reload_bits(stock, twin) == source.reload_bits;
                if (!source.stale)
                {
                    // A ready twin also keeps its source's feature bits; a stale one gets them back when it checks out.
                    if (unchanged && features_kept(stock, twin))
                    {
                        continue;
                    }
                    source.stale = true;
                    source.stale_ms = now;
                    ++went_stale;
                    if (first_stale == nullptr)
                    {
                        first_stale = &source;
                        first_stock = stock;
                        first_twin = twin;
                    }
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Debug,
                        "ShaderTwins: {} twin {:#x} for {} went stale (source flags {:#x}, {} techniques, gen mask "
                        "{:#x}, feature bits {:#x}; twin flags {:#x}, {} techniques, gen mask {:#x}, feature bits "
                        "{:#x}; ready with {} techniques, gen mask {:#x}, reloads {:#x}{})",
                        blobs::k_twins[source.kind].label, source.twin, source.name.data(), stock.flags,
                        stock.techniques, stock.mask, stock.features, twin.flags, twin.techniques, twin.mask,
                        twin.features, source.techniques, source.mask, source.reload_bits, read ? "" : "; unreadable");
                }
                else if (!unchanged)
                {
                    // Still changing: the wait starts again from what it reads now.
                    source.techniques = stock.techniques;
                    source.mask = stock.mask;
                    source.reload_bits = reload_bits(stock, twin);
                    source.stale_ms = now;
                }
                else if (now - source.stale_ms >= Constants::SHADER_TWIN_REVALIDATE_MS)
                {
                    std::uint16_t features_before = 0;
                    std::uint16_t features = 0;
                    if (!match_feature_flags_guarded(source.twin, source.shader, features_before, features))
                    {
                        s_guard_faults.fetch_add(1, std::memory_order_relaxed);
                        source.stale_ms = now;
                        continue;
                    }
                    source.stale = false;
                    ++revalidated;
#if defined(TPVCAMERA_DEV_BUILD)
                    ++s_usable_generation;
#endif
                    (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                             "ShaderTwins: {} twin {:#x} for {} checks out again ({} techniques, gen "
                                             "mask {:#x}, feature bits {:#x})",
                                             blobs::k_twins[source.kind].label, source.twin, source.name.data(),
                                             source.techniques, source.mask, features);
                }
            }
            if (went_stale != 0)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Warning,
                    "ShaderTwins: the engine reloaded or changed {} ready twin(s) or their sources (first: {} twin "
                    "for {}, source {} techniques, gen mask {:#x}, flags {:#x}; twin {} techniques, gen mask {:#x}, "
                    "flags {:#x}); their items keep the stock shader until both check out again",
                    went_stale, blobs::k_twins[first_stale->kind].label, first_stale->name.data(),
                    first_stock.techniques, first_stock.mask, first_stock.flags, first_twin.techniques, first_twin.mask,
                    first_twin.flags);
            }
            if (revalidated != 0)
            {
                (void)DMK::log().try_log(DMK::LogLevel::Info,
                                         "ShaderTwins: {} reloaded twin(s) check out against their sources again and "
                                         "swap again",
                                         revalidated);
            }
            s_refill_all = s_refill_all || went_stale != 0 || revalidated != 0;
        }

        struct CachedTwin
        {
            std::uintptr_t shader = 0;
            int kind = 0;
            std::array<char, 128> name{};
        };

        /** @brief Under the engine's shared resource lock, retain only loaded current-version twin permutations. */
        [[nodiscard]] bool snapshot_twins_guarded(CachedTwin *entries, int &count) noexcept
        {
            __try
            {
                const auto container = *reinterpret_cast<const std::uintptr_t *>(s_shader_container_global);
                if (container == 0)
                {
                    return true;
                }
                const auto begin =
                    *reinterpret_cast<const std::uintptr_t *>(container + Constants::RESOURCE_CONTAINER_BEGIN_OFFSET);
                const auto end =
                    *reinterpret_cast<const std::uintptr_t *>(container + Constants::RESOURCE_CONTAINER_END_OFFSET);
                if (end < begin || (end - begin) % sizeof(std::uintptr_t) != 0 || (begin == 0 && end != 0))
                {
                    return false;
                }
                // A persistent render-item key has 11 shader-ID bits; vector index is ID minus one.
                const auto size = std::min<std::size_t>((end - begin) / sizeof(std::uintptr_t), 2047);
                for (std::size_t i = 0; i < size && count < k_max_sources; ++i)
                {
                    const auto base = reinterpret_cast<const std::uintptr_t *>(begin)[i];
                    if (base < Constants::SHADER_BASE_RESOURCE_OFFSET)
                    {
                        continue;
                    }
                    const auto shader = base - Constants::SHADER_BASE_RESOURCE_OFFSET;
                    const auto flags =
                        *reinterpret_cast<const std::uint32_t *>(shader + Constants::SHADER_FLAGS_OFFSET);
                    if ((flags & Constants::SHADER_FLAG_LOADED) == 0 || (flags & Constants::SHADER_FLAG_NOT_FOUND) != 0)
                    {
                        continue;
                    }
                    const char *name =
                        *reinterpret_cast<const char *const *>(shader + Constants::SHADER_RESOURCE_NAME_OFFSET);
                    if (name == nullptr)
                    {
                        continue;
                    }
                    for (int kind = 0; kind < k_kinds; ++kind)
                    {
                        const auto &twin = blobs::k_twins[kind];
                        if (s_kind_failed[static_cast<std::size_t>(kind)] || !starts_with_nocase(name, twin.twin_name))
                        {
                            continue;
                        }
                        CachedTwin &entry = entries[count];
                        std::size_t length = 0;
                        for (; length + 1 < entry.name.size() && name[length] != '\0'; ++length)
                        {
                            entry.name[length] = name[length];
                        }
                        entry.name[length] = '\0';
                        if (name[length] != '\0')
                        {
                            continue;
                        }
                        const char *suffix = entry.name.data() + std::strlen(twin.twin_name);
                        const char *platform_end = std::strchr(suffix, ')');
                        const char *mask_end = platform_end != nullptr ? std::strchr(platform_end + 1, ')') : nullptr;
                        if (*suffix != '(' || platform_end == nullptr || platform_end[1] != '(' ||
                            mask_end == nullptr || mask_end[1] != '\0')
                        {
                            continue; // A template name has no permutation mask and must never be recovered.
                        }
                        const auto add_ref = image_slot(base, Constants::BASE_RESOURCE_VTABLE_ADD_REF_OFFSET);
                        if (add_ref != 0 && resource_ref_guarded(add_ref, base))
                        {
                            entry.shader = shader;
                            entry.kind = kind;
                            ++count;
                        }
                        break;
                    }
                }
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        [[nodiscard]] std::uintptr_t lookup_cached_shader_guarded(const ResourceName &name) noexcept
        {
            __try
            {
                const auto base = s_resource_lookup(&Constants::CSHADER_RESOURCE_CLASS_CRC, &name, true);
                return base >= Constants::SHADER_BASE_RESOURCE_OFFSET ? base - Constants::SHADER_BASE_RESOURCE_OFFSET
                                                                      : 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }

        /** @brief Rebuild the DLL's mapping once, without parsing shaders or enumerating mutable materials. */
        void recover_cached_twins(std::uint64_t now) noexcept
        {
            if (s_recovery_done || s_resource_lookup == nullptr || s_resource_lock == nullptr ||
                s_shader_container_global == 0 || loading_renderer() == 0)
            {
                return;
            }
            if (s_source_count != 0)
            {
                s_recovery_done = true; // This generation already prepared its own loading transaction.
                return;
            }
            if (!TryAcquireSRWLockShared(s_resource_lock))
            {
                return; // Never stall a camera frame waiting for resource registration/removal.
            }
            DMK_PROFILE_SCOPE("twins.recover_cache");
            std::array<CachedTwin, k_max_sources> entries{};
            int count = 0;
            const bool complete = snapshot_twins_guarded(entries.data(), count);
            ReleaseSRWLockShared(s_resource_lock);
            s_recovery_done = true;
            int recovered = 0;
            for (int i = 0; i < count; ++i)
            {
                CachedTwin &entry = entries[static_cast<std::size_t>(i)];
                const auto &twin = blobs::k_twins[entry.kind];
                std::array<char, 128> stock_name{};
                const std::size_t prefix = std::strlen(twin.source_name);
                const char *suffix = entry.name.data() + std::strlen(twin.twin_name);
                const std::size_t suffix_length = std::strlen(suffix);
                std::uintptr_t stock = 0;
                if (complete && prefix + suffix_length < stock_name.size())
                {
                    for (std::size_t n = 0; n < prefix; ++n)
                    {
                        stock_name[n] = lower(twin.source_name[n]);
                    }
                    std::memcpy(stock_name.data() + prefix, suffix, suffix_length + 1);
                    const ResourceName key{prefix + suffix_length, stock_name.size(), stock_name.data()};
                    // The lookup acquires the same SRW lock: call it only after releasing the inventory lock.
                    stock = lookup_cached_shader_guarded(key);
                }
                const auto flags =
                    DMK::memory::read<std::uint32_t>(DMK::Address{stock + Constants::SHADER_FLAGS_OFFSET});
                const auto mask =
                    DMK::memory::read<std::uint64_t>(DMK::Address{stock + Constants::SHADER_GEN_MASK_OFFSET});
                const auto twin_mask =
                    DMK::memory::read<std::uint64_t>(DMK::Address{entry.shader + Constants::SHADER_GEN_MASK_OFFSET});
                const auto techniques =
                    DMK::memory::read<std::uint32_t>(DMK::Address{stock + Constants::SHADER_TECHNIQUE_COUNT_OFFSET});
                if (stock != 0 && is_shader(stock) && is_shader(entry.shader) &&
                    source_kind_guarded(stock, twin.scope) == entry.kind && find_source(stock, entry.kind) < 0 &&
                    flags && (*flags & Constants::SHADER_FLAG_LOADED) != 0 &&
                    (*flags & Constants::SHADER_FLAG_NOT_FOUND) == 0 && mask && twin_mask && *mask == *twin_mask &&
                    techniques && *techniques != 0)
                {
                    Source &source = s_sources[static_cast<std::size_t>(s_source_count++)];
                    source = Source{.shader = stock,
                                    .kind = entry.kind,
                                    .twin = entry.shader,
                                    .mask = *mask,
                                    .techniques = *techniques,
                                    .created_ms = now,
                                    .recovered = true};
                    copy_text_field_guarded(stock, Constants::SHADER_RESOURCE_NAME_OFFSET, source.name.data(),
                                            source.name.size());
                    ++recovered;
                }
                else
                {
                    release_candidate_shader(stock);
                    release_candidate_shader(entry.shader);
                }
            }
            if (recovered != 0)
            {
                read_dissolve_rt_bit();
            }
            (void)DMK::log().try_log(
                complete ? DMK::LogLevel::Info : DMK::LogLevel::Warning,
                "ShaderTwins: cache recovery retained {} of {} loaded twin(s), inventory {}; no shader creation",
                recovered, count, complete ? "complete" : "faulted");
        }

        /** @brief Owns a detached candidate batch, including every early-return/exception path. */
        struct CandidateBatch
        {
            std::array<TwinCandidate, k_max_candidates> entries{};
            std::size_t count = 0;
            ~CandidateBatch() noexcept
            {
                for (std::size_t i = 0; i < count; ++i)
                {
                    drop_candidate(entries[i]);
                }
            }
        };

        /** @brief Owns one loading epoch's level-inventory candidates, outside the 512-entry pool, on every path. */
        struct InventoryBatch
        {
            std::vector<TwinCandidate> entries;
            ~InventoryBatch() noexcept
            {
                for (TwinCandidate &entry : entries)
                {
                    drop_candidate(entry);
                }
            }
        };

        /** @brief What one loading epoch's level inventory found and queued, for its log lines. */
        struct InventoryOutcome
        {
            bool ran = false;        // the inventory is on and was taken
            bool failed = false;     // its lists could not allocate; the capture-only twins go on
            bool listed_off = false; // not taken: every plant and frame kind is off
            TwinInventoryStats walk{};
            int plant_pairs = 0; // distinct listed pairs found, by scope
            int frame_pairs = 0;
            int off_pairs = 0; // of them, of a kind that is off (kind_off()), neither queued nor counted for coverage
            int existing = 0;  // of them, already a source
            int captured = 0;  // already a candidate this load captured
            int queued = 0;    // queued as an inventory candidate
            int rejected = 0;  // whose shader or resources did not check out
        };

        /** @brief True when @p batch holds a candidate of @p shader for @p kind. */
        [[nodiscard]] bool batch_holds(const CandidateBatch &batch, std::uintptr_t shader, int kind) noexcept
        {
            for (std::size_t i = 0; i < batch.count; ++i)
            {
                const TwinRequest &request = batch.entries[i].request;
                if (request.shader == shader && request.kind == kind)
                {
                    return true;
                }
            }
            return false;
        }

        /** @brief A listed pair the level inventory found, with the resources of the first material found with it. */
        struct InventoryFound
        {
            InventoryKey key{};
            std::uintptr_t resources = 0;
        };

        /**
         * @brief Walks the level tables (hooks/twin_inventory.hpp) and puts in @p found, sorted and once each, every
         *        listed (shader, kind) pair their drawn materials name, counted by scope in the outcome it returns.
         * @details The walk calls no engine code. Each drawn material's shader item is read the way a captured one is
         *          (load_material_shader_detour()) and goes through the same kind test for the scope its asset path
         *          gives it, the character's kinds excepted; a pair keeps the resources of the first material found
         *          with it.
         * @return The outcome, not run (and @p found empty) when the inventory is off, or when every plant and frame
         *         kind is off (its pairs could only take source slots with no twin).
         * @throws std::bad_alloc If the lists cannot allocate.
         * @note Main thread, inside finalize_loading() after the first render-command barrier, or in a frame the game
         *       stands still in (pause_inventory()).
         */
        [[nodiscard]] InventoryOutcome walk_inventory(std::vector<InventoryFound> &found)
        {
            InventoryOutcome outcome{};
            if (s_inventory_objman_global == 0)
            {
                return outcome;
            }
            if (!listed_scopes_usable())
            {
                outcome.listed_off = true;
                return outcome;
            }
            outcome.ran = true;
            std::vector<TwinInventoryMaterial> materials;
            materials.reserve(4096);
            outcome.walk = enumerate_level_twin_materials(s_genv, s_inventory_objman_global, materials);
            found.reserve(512);
            for (const TwinInventoryMaterial &drawn : materials)
            {
                ShaderItem item{};
                if (!DMK::memory::read_into(DMK::Address{drawn.material + Constants::MATERIAL_SHADER_ITEM_OFFSET},
                                            std::as_writable_bytes(std::span{&item, 1})) ||
                    item.shader == 0)
                {
                    continue;
                }
                for_each_twin_kind(
                    item, drawn.scopes, false, [&found, &item](int kind)
                    { found.push_back(InventoryFound{.key = {item.shader, kind}, .resources = item.resources}); });
            }
            // One entry per pair, the first material's resources kept.
            std::stable_sort(found.begin(), found.end(),
                             [](const InventoryFound &a, const InventoryFound &b) { return a.key < b.key; });
            found.erase(std::unique(found.begin(), found.end(),
                                    [](const InventoryFound &a, const InventoryFound &b) { return a.key == b.key; }),
                        found.end());
            for (const InventoryFound &pair : found)
            {
                const bool frame = blobs::k_twins[pair.key.kind].scope == blobs::TwinScope::ListedFrames;
                ++(frame ? outcome.frame_pairs : outcome.plant_pairs);
            }
            return outcome;
        }

        /**
         * @brief Queues in @p inventory an owned candidate for every pair of @p found that is neither a source nor one
         *        @p held(shader, kind) says is a candidate already, and makes s_inventory_pairs the pairs found.
         * @details Nothing is dropped for room: the inventory holds the level's own pairs, outside the 512-entry pool.
         *          A pair of a kind that is off (kind_off()) is left out of both: it would take a source slot with no
         *          twin, ahead of the character's, and its plants keep the stock shader anyway.
         * @throws std::bad_alloc If the lists cannot allocate (what was queued so far stays owned by @p inventory).
         * @note Main thread, under s_main_mutex.
         */
        template <typename Held>
        void queue_inventory(const std::vector<InventoryFound> &found, Held &&held, InventoryBatch &inventory,
                             InventoryOutcome &outcome)
        {
            std::vector<InventoryKey> pairs;
            pairs.reserve(found.size());
            for (const InventoryFound &pair : found)
            {
                if (!kind_off(pair.key.kind))
                {
                    pairs.push_back(pair.key);
                }
            }
            // Room for every pair first, so no owned candidate can be lost to a failed push.
            inventory.entries.reserve(inventory.entries.size() + found.size());
            for (const InventoryFound &pair : found)
            {
                if (kind_off(pair.key.kind))
                {
                    ++outcome.off_pairs;
                    continue;
                }
                if (find_source(pair.key.shader, pair.key.kind) >= 0)
                {
                    ++outcome.existing;
                    continue;
                }
                if (held(pair.key.shader, pair.key.kind))
                {
                    ++outcome.captured;
                    continue;
                }
                ShaderItem item{};
                item.shader = pair.key.shader;
                item.resources = pair.resources;
                TwinCandidate candidate{};
                if (!own_candidate(item, pair.key.kind, 0, candidate))
                {
                    ++outcome.rejected;
                    continue;
                }
                inventory.entries.push_back(candidate);
                ++outcome.queued;
            }
            s_inventory_pairs = std::move(pairs);
        }

        /**
         * @brief Takes the level inventory (hooks/twin_inventory.hpp) and queues in @p inventory, outside the 512-entry
         *        batch, an owned candidate for every listed (shader, kind) pair it found that is neither a source nor a
         *        candidate @p batch captured; s_inventory_pairs becomes the pairs it found.
         * @details The walk reads the level tables with no mod mutex held (walk_inventory()), then the pairs are queued
         *          under s_main_mutex (queue_inventory()). A pair whose stock shader still parses moves on to the pause
         *          inventory (finalize_loading()). An inventory not taken leaves no pairs of an earlier load behind.
         * @throws std::bad_alloc If the inventory's lists cannot allocate (the caller goes on with the captured ones).
         * @note Main thread, inside finalize_loading() after the first render-command barrier.
         */
        [[nodiscard]] InventoryOutcome take_inventory(const CandidateBatch &batch, InventoryBatch &inventory)
        {
            std::vector<InventoryFound> found;
            InventoryOutcome outcome = walk_inventory(found);
            const std::lock_guard lock(s_main_mutex);
            if (!outcome.ran)
            {
                s_inventory_pairs.clear();
                return outcome;
            }
            queue_inventory(
                found, [&batch](std::uintptr_t shader, int kind) { return batch_holds(batch, shader, kind); },
                inventory, outcome);
            return outcome;
        }

        /** @brief The level inventory's line for the epoch log: what the walk read and what it queued. */
        void log_inventory(const InventoryOutcome &outcome) noexcept
        {
            if (outcome.listed_off)
            {
                (void)DMK::log().try_log(DMK::LogLevel::Info, "ShaderTwins: level inventory not taken: every plant and "
                                                              "frame twin kind is off, so trees and frames keep the "
                                                              "stock shader");
            }
            if (!outcome.ran)
            {
                return;
            }
            const TwinInventoryStats &walk = outcome.walk;
            const double ms = static_cast<double>(walk.micros) / 1000.0;
            if (outcome.failed)
            {
                (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                         "ShaderTwins: level inventory could not allocate its lists; this load "
                                         "prepares only the captured twins");
                return;
            }
            if (!walk.level)
            {
                (void)DMK::log().try_log(walk.faults != 0 ? DMK::LogLevel::Warning : DMK::LogLevel::Info,
                                         "ShaderTwins: level inventory: {} ({:.1f} ms); this load prepares only the "
                                         "captured twins",
                                         walk.reason, ms);
                return;
            }
            (void)DMK::log().try_log(
                DMK::LogLevel::Info,
                "ShaderTwins: level inventory in {:.1f} ms: {} vegetation group(s), {} statobj(s) with {} compound "
                "part(s), {} brush material(s); {} plant or frame asset(s), {} distinct material(s), {} drawn -> {} "
                "plant + {} frame pair(s): {} of a twin kind that is off, {} with a twin source already, {} captured "
                "by this load, {} queued, {} rejected",
                ms, walk.groups, walk.statobjs, walk.children, walk.brush_materials, walk.scoped_assets,
                walk.distinct_materials, walk.drawn_materials, outcome.plant_pairs, outcome.frame_pairs,
                outcome.off_pairs, outcome.existing, outcome.captured, outcome.queued, outcome.rejected);
            if (walk.faults != 0 || walk.not_materials != 0 || walk.skipped_submaterials != 0 || walk.dedupe_full)
            {
                (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                         "ShaderTwins: level inventory: {} faulted read(s) or tables that did not "
                                         "check out{}{}, {} pointer(s) that are not a CMatInfo, {} sub-material(s) "
                                         "past the {} read per material{}; pairs behind them come from captures only",
                                         walk.faults, walk.reason[0] != '\0' ? "; first: " : "", walk.reason,
                                         walk.not_materials, walk.skipped_submaterials,
                                         Constants::TWIN_INVENTORY_MAX_SUBMATERIALS,
                                         walk.dedupe_full ? "; the dedupe set filled" : "");
            }
        }

        /** @brief Where the level inventory stands on a render miss of a listed node (classify_render_miss()). */
        enum class MissScope : std::uint8_t
        {
            NotChecked,  // not a listed node's render miss, or no inventory found a level
            Inventoried, // the inventory found its pair
            OtherScope,  // the inventory found its shader only for the other listed scope
            NotInLevel,  // no level table holds a material of a plant or frame asset with it
        };

        /**
         * @brief Whether a render miss of a listed node is a pair the last level inventory found. A miss outside it
         *        has a material scoped otherwise than its node (an override material named outside the plant folders,
         *        a plant shader on a frame) or one the level tables do not hold (an entity's, content spawned later).
         * @note Main thread, under s_main_mutex. Compares identities only.
         */
        [[nodiscard]] MissScope classify_render_miss(const TwinCandidate &candidate) noexcept
        {
            const int kind = candidate.request.kind;
            // A kind that is off gets no twin wherever its pair is (the inventory leaves its pairs out).
            if (s_inventory_pairs.empty() || candidate.priority != 2 || kind_off(kind) ||
                !listed_scope(blobs::k_twins[kind].scope))
            {
                return MissScope::NotChecked;
            }
            const auto found = [](std::uintptr_t shader, int of)
            { return std::ranges::binary_search(s_inventory_pairs, InventoryKey{shader, of}); };
            if (found(candidate.request.shader, kind))
            {
                return MissScope::Inventoried;
            }
            for (int other = 0; other < k_kinds; ++other)
            {
                if (other != kind && listed_scope(blobs::k_twins[other].scope) &&
                    found(candidate.request.shader, other))
                {
                    return MissScope::OtherScope;
                }
            }
            return MissScope::NotInLevel;
        }

        /** @brief A render miss outside the level inventory, copied for a log line. */
        struct MissReport
        {
            int kind = 0;
            MissScope scope = MissScope::NotChecked;
            std::uint64_t mask = 0;
            std::array<char, 64> name{};
        };

        /** @brief Copies @p candidate's identity for log_render_miss() while its references are held. */
        [[nodiscard]] MissReport miss_report(const TwinCandidate &candidate, MissScope scope) noexcept
        {
            MissReport report{.kind = candidate.request.kind, .scope = scope};
            copy_text_field_guarded(candidate.request.shader, Constants::SHADER_RESOURCE_NAME_OFFSET,
                                    report.name.data(), report.name.size());
            report.mask = DMK::memory::read<std::uint64_t>(
                              DMK::Address{candidate.request.shader + Constants::SHADER_GEN_MASK_OFFSET})
                              .value_or(0);
            return report;
        }

        /**
         * @brief Logs a render miss outside the level inventory by name and gen mask: a variant whose twin only its
         *        render miss prepares, @p at_boundary this loading boundary, else the next pause or boundary.
         */
        void log_render_miss(const MissReport &report, bool at_boundary) noexcept
        {
            const blobs::Twin &twin = blobs::k_twins[report.kind];
            (void)DMK::log().try_log(
                DMK::LogLevel::Info,
                "ShaderTwins: a listed {} draws {} {} (gen mask {:#x}), which the level inventory {}; its render miss "
                "prepares its twin {}",
                twin.scope == blobs::TwinScope::ListedFrames ? "frame" : "plant", twin.label, report.name.data(),
                report.mask,
                report.scope == MissScope::OtherScope
                    ? "found only for the other listed scope (a material named outside its node's folders)"
                    : "did not find in the level tables (an entity's or override material, or content loaded "
                      "after the boundary)",
                at_boundary                                       ? "at this loading boundary"
                : s_pause_service.load(std::memory_order_relaxed) ? "at the next pause or loading boundary"
                                                                  : "at the next loading boundary");
        }

        /**
         * @brief Logs, once each, the render misses of @p batch outside the level inventory just taken.
         * @return How many there were.
         * @note Main thread, under s_main_mutex, inside finalize_loading() while the batch holds its references.
         */
        int report_boundary_misses(CandidateBatch &batch) noexcept
        {
            int outside = 0;
            for (std::size_t i = 0; i < batch.count; ++i)
            {
                TwinCandidate &candidate = batch.entries[i];
                if (candidate.request.shader == 0 || candidate.priority != 2 || candidate.reported)
                {
                    continue;
                }
                const MissScope scope = classify_render_miss(candidate);
                candidate.reported = scope != MissScope::NotChecked;
                if (scope == MissScope::OtherScope || scope == MissScope::NotInLevel)
                {
                    ++outside;
                    log_render_miss(miss_report(candidate, scope), true);
                }
            }
            return outside;
        }

        /**
         * @brief Within a second of a render job retaining a render miss, logs each one of a listed node outside the
         *        last level inventory (classify_render_miss()), once per candidate, at most eight a second.
         * @note Main thread, under s_main_mutex, from log_second(). Never waits for the candidate mutex: a busy one
         *       leaves the check for the next second.
         */
        void log_render_misses() noexcept
        {
            if (s_inventory_pairs.empty() || !s_render_miss_new.load(std::memory_order_relaxed))
            {
                return;
            }
            std::array<MissReport, 8> reports{};
            std::size_t count = 0;
            try
            {
                const std::unique_lock lock(s_candidate_mutex, std::try_to_lock);
                if (!lock.owns_lock())
                {
                    return;
                }
                s_render_miss_new.store(false, std::memory_order_relaxed);
                for (std::size_t i = 0; i < s_candidate_count; ++i)
                {
                    TwinCandidate &candidate = s_candidates[i];
                    if (candidate.priority != 2 || candidate.reported)
                    {
                        continue;
                    }
                    const MissScope scope = classify_render_miss(candidate);
                    if (scope == MissScope::OtherScope || scope == MissScope::NotInLevel)
                    {
                        if (count == reports.size())
                        {
                            s_render_miss_new.store(true, std::memory_order_relaxed);
                            break;
                        }
                        reports[count++] = miss_report(candidate, scope);
                    }
                    candidate.reported = true;
                }
            }
            catch (...)
            {
                return;
            }
            for (std::size_t i = 0; i < count; ++i)
            {
                log_render_miss(reports[i], false);
            }
        }

        /** @brief The level inventory's pairs that have a ready twin after a loading epoch, by scope. */
        struct Coverage
        {
            int plant = 0;
            int plant_ready = 0;
            int frame = 0;
            int frame_ready = 0;
            int missing = 0;
            std::array<char, 640> missing_text{};
        };

        /**
         * @brief Counts the last level inventory's pairs that have a ready twin, and names the others (their kind,
         *        name with gen mask, and why: no source, still parsing, failed, stale, or their kind off), each also in
         *        a Debug line.
         * @note Main thread, under s_main_mutex, inside finalize_loading() while the inventory, the batch and the
         *       source table hold every pair's shader.
         */
        [[nodiscard]] Coverage compute_coverage() noexcept
        {
            Coverage coverage{};
            std::size_t used = 0;
            for (const InventoryKey &pair : s_inventory_pairs)
            {
                const blobs::Twin &twin = blobs::k_twins[pair.kind];
                const bool frame = twin.scope == blobs::TwinScope::ListedFrames;
                ++(frame ? coverage.frame : coverage.plant);
                const int at = find_source(pair.shader, pair.kind);
                const Source *source = at >= 0 ? &s_sources[static_cast<std::size_t>(at)] : nullptr;
                const bool kind_failed = s_kind_failed[static_cast<std::size_t>(pair.kind)];
                if (source != nullptr && source->state == TwinState::Ready && !source->stale && !kind_failed)
                {
                    ++(frame ? coverage.frame_ready : coverage.plant_ready);
                    continue;
                }
                ++coverage.missing;
                const char *why = kind_failed                           ? "kind off"
                                  : source == nullptr                   ? "no source"
                                  : source->state == TwinState::Pending ? "parsing"
                                  : source->state == TwinState::Failed  ? "failed"
                                                                        : "stale";
                decltype(Source::name) name{};
                if (source != nullptr)
                {
                    name = source->name;
                }
                else
                {
                    copy_text_field_guarded(pair.shader, Constants::SHADER_RESOURCE_NAME_OFFSET, name.data(),
                                            name.size());
                }
                const auto mask =
                    DMK::memory::read<std::uint64_t>(DMK::Address{pair.shader + Constants::SHADER_GEN_MASK_OFFSET});
                (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                         "ShaderTwins: coverage: {} {} (CShader {:#x}, gen mask {:#x}) has no ready "
                                         "twin ({})",
                                         twin.label, name.data(), pair.shader, mask.value_or(0), why);
                if (used + 1 < coverage.missing_text.size())
                {
                    const auto written =
                        std::format_to_n(coverage.missing_text.data() + used, coverage.missing_text.size() - 1 - used,
                                         "{}{} {} ({})", used == 0 ? "" : ", ", twin.label, name.data(), why);
                    used = std::min(used + static_cast<std::size_t>(written.size), coverage.missing_text.size() - 1);
                }
            }
            coverage.missing_text[used] = '\0';
            return coverage;
        }

        /** @brief Where a candidate came from, for the capacity log line. */
        [[nodiscard]] const char *candidate_origin(const TwinCandidate &candidate, bool from_inventory) noexcept
        {
            if (from_inventory)
            {
                return "the level inventory";
            }
            return candidate.priority == 2   ? "a render miss"
                   : candidate.priority == 1 ? "a character material"
                                             : "a material load";
        }

#if defined(TPVCAMERA_DEV_BUILD)
        // The s_usable_generation the DEV clone dry-run was last handed its pairs at (dry_run_pairs()), so a paused
        // frame hands them over again only once a twin became ready or checked out again since; -1 until the first
        // hand-over of this module, so the first pause after a DEV hot reload hands over the twins its recovery took.
        // Main thread only.
        int s_dry_run_generation = -1;
        // Set by create_during_pause() in a frame whose pause had nothing left to create (no candidate it could take,
        // or every source slot in use): only such a frame runs a step of the DEV clone dry-run
        // (service_paused_dry_run()). Main thread only.
        bool s_pause_drained = false;

        /**
         * @brief The ready plant and frame twins of a kind the DEV clone can derive, for its dry-run (which leaves out
         *        the pairs it handled already this session).
         * @param changed_only Hand back none when no twin became ready or checked out again since the last call.
         * @throws std::bad_alloc If the list cannot allocate.
         * @note Main thread, under s_main_mutex. The source table holds every pair's shaders for the session.
         */
        [[nodiscard]] std::vector<TwinClone::DryRunPair> dry_run_pairs(bool changed_only)
        {
            const auto wanted = [](const Source &source)
            {
                return source.state == TwinState::Ready && !source.stale && !kind_off(source.kind) &&
                       listed_scope(blobs::k_twins[source.kind].scope) && TwinClone::clone_capable(source.kind);
            };
            std::vector<TwinClone::DryRunPair> pairs;
            if (changed_only && s_usable_generation == s_dry_run_generation)
            {
                return pairs;
            }
            const int ready =
                static_cast<int>(std::count_if(s_sources.begin(), s_sources.begin() + s_source_count, wanted));
            pairs.reserve(static_cast<std::size_t>(ready));
            for (int i = 0; i < s_source_count; ++i)
            {
                const Source &source = s_sources[static_cast<std::size_t>(i)];
                if (wanted(source))
                {
                    pairs.push_back(TwinClone::DryRunPair{.stock = source.shader,
                                                          .twin = source.twin,
                                                          .kind = source.kind,
                                                          .mask = source.mask,
                                                          .name = source.name,
                                                          .recovered = source.recovered});
                }
            }
            s_dry_run_generation = s_usable_generation;
            return pairs;
        }
#endif

        /**
         * @brief The loading screen's preparation and creation, called while an engine loading operation still owns the
         *        screen (the only other creation is one twin at a time in a frame the game stands still in,
         *        create_during_pause()).
         * @details No mod mutex spans a render flush: the engine's wait services callbacks. Closing admission first
         *          makes racing loader/render callbacks stay stock. Sources still pending after the first barrier
         *          remain owned for a later load rather than occupying a permanently failed source slot. After the
         *          first barrier the level inventory (take_inventory()) adds, outside the 512-entry batch, every plant
         *          and frame pair the level's tables can draw that has no source yet, so every boundary covers the
         *          level's content whatever this load happened to load (a same-level save load, the first load after
         *          a DEV hot reload). The plant and frame candidates go before the character's, and those of a kind
         *          that is off after every other, so a full source table turns a character twin away first, and the
         *          pairs that miss a slot are logged by name and gen mask (up to SHADER_TWIN_CAPACITY_LOG_LINES). An
         *          inventory pair whose stock shader still parses after the first barrier moves on to the pause
         *          inventory, so the next pause creates it. The epoch closes with the inventory's coverage: its pairs
         *          with a ready twin, and the others by name.
         */
        void finalize_loading(const char *reason) noexcept
        {
            DMK_PROFILE_SCOPE("twins.load_finalize");
            const std::uintptr_t renderer = loading_renderer();
            if (renderer == 0 || !s_available.load(std::memory_order_acquire) ||
                s_disabled.load(std::memory_order_acquire) ||
                s_loading_finalizing.exchange(true, std::memory_order_acq_rel))
            {
                return;
            }
            struct FinalizingScope
            {
                ~FinalizingScope() noexcept { s_loading_finalizing.store(false, std::memory_order_release); }
            } finalizing_scope;
            drop_evicted();
            CandidateBatch batch;
            std::array<bool, k_max_candidates> deferred{};
            std::size_t deferred_count = 0;
            int created = 0;
            int ready = 0;
            int outstanding = 0;
            int capacity_skipped = 0;
            int total_sources = 0;
            int inventory_created = 0;
            int inventory_deferred = 0;
            int inventory_unplaced = 0;
            int misses_outside = 0;
            bool issued = false;
#if defined(TPVCAMERA_DEV_BUILD)
            // The ready plant and frame twins after the creations, for the DEV clone dry-run.
            std::vector<TwinClone::DryRunPair> dry_run;
#endif
            const auto started = GetTickCount64();
            try
            {
                {
                    const std::lock_guard lock(s_candidate_mutex);
                    batch.count = std::exchange(s_candidate_count, 0);
                    std::copy_n(s_candidates.begin(), batch.count, batch.entries.begin());
                    std::fill_n(s_candidates.begin(), batch.count, TwinCandidate{});
                }
                bool prepared = false;
                // The level inventory a pause took after a DEV hot reload: this boundary's own inventory supersedes
                // it, and its references are dropped when this scope ends, outside every mod mutex.
                InventoryBatch pause_left;
                InventoryBatch pause_done;
                {
                    const std::lock_guard lock(s_main_mutex);
                    pause_left.entries.swap(s_pause_inventory);
                    pause_done.entries.swap(s_pause_inventory_done);
                    prepared = s_available.load(std::memory_order_acquire) && prepare(started);
                }
                if (!prepared)
                {
                    // A temporarily unavailable engine may become ready at the save-load boundary of this load.
                    const std::lock_guard lock(s_candidate_mutex);
                    if (s_available.load(std::memory_order_acquire) && !s_disabled.load(std::memory_order_acquire))
                    {
                        // An entry that does not fit stays in the batch, whose destructor drops it.
                        for (std::size_t i = 0; i < batch.count && s_candidate_count < s_candidates.size(); ++i)
                        {
                            s_candidates[s_candidate_count++] = batch.entries[i];
                            batch.entries[i].request = {};
                        }
                    }
                    return;
                }
                if (!flush_loading_commands(renderer))
                {
                    const std::lock_guard lock(s_main_mutex);
                    turn_off(GetTickCount64(), "loading screen could not flush the stock shader commands");
                    return;
                }
                // The level inventory, after the barrier, so its stock shaders are parsed like the captured ones.
                InventoryBatch inventory;
                InventoryOutcome inventoried{};
                const std::uint64_t rules = Rules::current_generation();
                try
                {
                    inventoried = take_inventory(batch, inventory);
                }
                catch (...)
                {
                    inventoried.ran = true;
                    inventoried.failed = true;
                }
                if ((inventoried.ran && !inventoried.failed) || inventoried.listed_off)
                {
                    // This epoch has its inventory (or needs none) under these rules: the next pause takes none of its
                    // own unless the rules change.
                    s_inventory_epoch.store(s_load_epoch.load(std::memory_order_relaxed), std::memory_order_relaxed);
                    s_inventory_rules.store(rules, std::memory_order_relaxed);
                }
                log_inventory(inventoried);
                // The plant and frame candidates first, a drawn item's before a loaded material's, then the
                // character's, and last those of a kind that is off (kind_off(): a source slot with no twin and no
                // parse, so a later capture of theirs is not retained again): the batch's listed ones, the
                // inventory's, then the batch's others.
                const auto rank = [](const TwinCandidate &candidate)
                { return kind_off(candidate.request.kind) ? -1 : candidate_rank(candidate); };
                std::sort(batch.entries.begin(), batch.entries.begin() + static_cast<std::ptrdiff_t>(batch.count),
                          [&rank](const TwinCandidate &a, const TwinCandidate &b) { return rank(a) > rank(b); });
                const std::size_t listed_count = static_cast<std::size_t>(std::count_if(
                    batch.entries.begin(), batch.entries.begin() + static_cast<std::ptrdiff_t>(batch.count),
                    [&rank](const TwinCandidate &candidate) { return rank(candidate) >= 4; }));
                {
                    const std::lock_guard lock(s_main_mutex);
                    if (s_available.load(std::memory_order_acquire) && !s_disabled.load(std::memory_order_acquire))
                    {
                        const int before = s_source_count;
                        const std::size_t inventory_count = inventory.entries.size();
                        // Room in both pause lists for every inventory pair first, so one whose stock shader still
                        // parses moves on to the next pause (create_during_pause()) without an allocation that could
                        // lose it; when there is none, such a pair waits for the next boundary instead.
                        bool pause_room = false;
                        try
                        {
                            s_pause_inventory.reserve(s_pause_inventory.size() + inventory_count);
                            s_pause_inventory_done.reserve(s_pause_inventory_done.size() + s_pause_inventory.size() +
                                                           inventory_count);
                            pause_room = true;
                        }
                        catch (...)
                        {
                        }
                        for (std::size_t n = 0; n < batch.count + inventory_count; ++n)
                        {
                            const bool from_inventory = n >= listed_count && n < listed_count + inventory_count;
                            const std::size_t i = n < listed_count ? n
                                                  : from_inventory ? n - listed_count
                                                                   : n - inventory_count;
                            TwinCandidate &candidate = from_inventory ? inventory.entries[i] : batch.entries[i];
                            const auto &request = candidate.request;
                            if (find_source(request.shader, request.kind) >= 0)
                            {
                                continue;
                            }
                            if (s_source_count >= k_max_sources)
                            {
                                if (kind_off(request.kind))
                                {
                                    continue; // It would get no twin anyway.
                                }
                                // Each by name up to a bound per load; the epoch line counts them all.
                                if (++capacity_skipped > Constants::SHADER_TWIN_CAPACITY_LOG_LINES)
                                {
                                    continue;
                                }
                                decltype(Source::name) name{};
                                copy_text_field_guarded(request.shader, Constants::SHADER_RESOURCE_NAME_OFFSET,
                                                        name.data(), name.size());
                                const auto mask = DMK::memory::read<std::uint64_t>(
                                    DMK::Address{request.shader + Constants::SHADER_GEN_MASK_OFFSET});
                                (void)DMK::log().try_log(
                                    DMK::LogLevel::Info,
                                    "ShaderTwins: no source slot left ({} in use) for the {} twin of {} (gen mask "
                                    "{:#x}, CShader {:#x}, resources {:#x}) from {}; its items keep the stock shader{}",
                                    k_max_sources, blobs::k_twins[request.kind].label, name.data(), mask.value_or(0),
                                    request.shader, request.resources, candidate_origin(candidate, from_inventory),
                                    capacity_skipped == Constants::SHADER_TWIN_CAPACITY_LOG_LINES
                                        ? " (any more this load are only counted)"
                                        : "");
                                continue;
                            }
                            const auto flags = DMK::memory::read<std::uint32_t>(
                                DMK::Address{request.shader + Constants::SHADER_FLAGS_OFFSET});
                            if (flags && (*flags & Constants::SHADER_FLAG_NOT_FOUND) == 0 &&
                                (*flags & Constants::SHADER_FLAG_LOADED) == 0)
                            {
                                // A captured one stays in the pool; an inventory pair moves on to the pause
                                // inventory, which owns it from here (this epoch's inventory is taken, so no pause
                                // would find it again).
                                if (!from_inventory)
                                {
                                    deferred[i] = true;
                                    ++deferred_count;
                                }
                                else if (pause_room)
                                {
                                    // Its room was reserved above, so this does not allocate.
                                    s_pause_inventory.push_back(std::exchange(candidate, TwinCandidate{}));
                                    ++inventory_deferred;
                                }
                                else
                                {
                                    ++inventory_unplaced;
                                }
                                continue;
                            }
                            issued = true; // Even a guarded engine fault may have submitted a parse command.
                            const int sources_before = s_source_count;
                            create_twin(request, GetTickCount64());
                            inventory_created += from_inventory && s_source_count > sources_before ? 1 : 0;
                        }
                        created = s_source_count - before;
                        total_sources = s_source_count;
                    }
                }
                // Also flush when every name was already cached. This keeps the close protocol independent of
                // mfForName's internal cache behavior, including partial failures.
                const bool completed = flush_loading_commands(renderer);
                issued = false;
                Coverage coverage{};
                {
                    const std::lock_guard lock(s_main_mutex);
                    if (!completed)
                    {
                        turn_off(GetTickCount64(), "loading screen could not flush the twin shader commands");
                    }
                    else if (s_available.load(std::memory_order_acquire))
                    {
                        refresh_twins(GetTickCount64());
                        for (int i = 0; i < s_source_count; ++i)
                        {
                            ready += s_sources[static_cast<std::size_t>(i)].state == TwinState::Ready ? 1 : 0;
                            outstanding += s_sources[static_cast<std::size_t>(i)].state == TwinState::Pending ? 1 : 0;
                        }
                        // While the batch and the inventory still hold their shaders. A failed inventory leaves pairs
                        // of an earlier load, which nothing holds any more.
                        if (inventoried.failed)
                        {
                            s_inventory_pairs.clear();
                        }
                        misses_outside = report_boundary_misses(batch);
                        coverage = compute_coverage();
#if defined(TPVCAMERA_DEV_BUILD)
                        if constexpr (TwinClone::k_stage == TwinClone::Stage::DryRun)
                        {
                            try
                            {
                                dry_run = dry_run_pairs(false);
                            }
                            catch (...)
                            {
                                dry_run.clear();
                            }
                        }
#endif
                    }
                }
                {
                    const std::lock_guard lock(s_candidate_mutex);
                    if (s_available.load(std::memory_order_acquire) && !s_disabled.load(std::memory_order_acquire))
                    {
                        // An entry that does not fit stays in the batch, whose destructor drops it.
                        for (std::size_t i = 0; i < batch.count && s_candidate_count < s_candidates.size(); ++i)
                        {
                            if (deferred[i])
                            {
                                s_candidates[s_candidate_count++] = batch.entries[i];
                                batch.entries[i].request = {};
                            }
                        }
                    }
                }
                (void)DMK::log().try_log(
                    capacity_skipped > 0 ? DMK::LogLevel::Warning : DMK::LogLevel::Info,
                    "ShaderTwins: loading epoch {} closed at {} on thread {}: {} candidate(s) and {} from the level "
                    "inventory, {} new source(s) ({} of them the inventory's), {}/{} source slots, {} ready twin(s), "
                    "{} outstanding parse(s), {} source(s) deferred, {} inventory pair(s) still parsing "
                    "(created at the next pause; {} more found again at the next boundary), {} candidate(s) skipped at "
                    "capacity, {} render miss(es) outside the level inventory; render commands {}; total preparation "
                    "{} ms; unpaused gameplay creates no twins",
                    s_load_epoch.load(std::memory_order_relaxed), reason, GetCurrentThreadId(), batch.count,
                    inventory.entries.size(), created, inventory_created, total_sources, k_max_sources, ready,
                    outstanding, deferred_count, inventory_deferred, inventory_unplaced, capacity_skipped,
                    misses_outside, completed ? "flushed" : "failed", GetTickCount64() - started);
                if (coverage.plant + coverage.frame > 0)
                {
                    (void)DMK::log().try_log(coverage.missing > 0 ? DMK::LogLevel::Warning : DMK::LogLevel::Info,
                                             "ShaderTwins: coverage: plant {}/{} ready, frame {}/{} ready{}{}",
                                             coverage.plant_ready, coverage.plant, coverage.frame_ready, coverage.frame,
                                             coverage.missing > 0 ? "; missing " : "", coverage.missing_text.data());
                }
#if defined(TPVCAMERA_DEV_BUILD)
                // The DEV clone dry-run of the ready plant and frame twins, inside the boundary and with every mod
                // mutex released (it takes the HW shader factory's lock without waiting and calls the resource lookup).
                // What its budget leaves goes on in the next pause with nothing left to create. A failed flush that
                // just turned the twins off runs none of it.
                if constexpr (TwinClone::k_stage == TwinClone::Stage::DryRun)
                {
                    if (!s_disabled.load(std::memory_order_acquire))
                    {
                        std::array<char, 48> occasion{};
                        (void)std::format_to_n(occasion.data(), occasion.size() - 1, "loading epoch {}",
                                               s_load_epoch.load(std::memory_order_relaxed));
                        (void)TwinClone::queue_dry_run(dry_run, occasion.data());
                        TwinClone::run_dry_run(Constants::TWIN_CLONE_DRY_RUN_LOADING_US, 0, occasion.data());
                    }
                }
#endif
            }
            catch (...)
            {
                // Stack unwinding has released every mod mutex before the engine wait below.
                if (issued)
                {
                    (void)flush_loading_commands(renderer);
                }
                s_disabled.store(true, std::memory_order_release);
                s_set_published.store(nullptr, std::memory_order_release);
                s_rows_published.store(nullptr, std::memory_order_release);
                (void)DMK::log().try_log(DMK::LogLevel::Error,
                                         "ShaderTwins: loading preparation failed; remaining items keep stock shaders");
            }
        }

        /**
         * @brief What lets this frame create a twin outside a loading screen: the stopped game clock, under the Esc
         *        game menu, a UI overlay or neither, and never a dialogue.
         * @details A dialogue runs in real time, and so may a screen over a running clock (the dialog screen is an
         *          overlay), so neither the menu nor an overlay counts on its own: the game clock the camera reads must
         *          have advanced the world by nothing this frame. The Esc menu stops it; a menu flag over a running
         *          clock (one that outlived its menu) does not count, and a clock the camera cannot read counts as
         *          running. A stopped clock counts without a menu or an overlay too, since nothing moves in the world
         *          then: the Esc menu open across a DEV hot reload is one the new module has not seen open. The
         *          dialogue bit is read whenever the clock stands still, so a conversation never counts.
         */
        [[nodiscard]] PauseReason pause_reason(const TwinPauseSignals &signals) noexcept
        {
            if (!signals.clock_paused || signals.dialogue)
            {
                return PauseReason::None;
            }
            return signals.menu_open    ? PauseReason::Menu
                   : signals.overlay_up ? PauseReason::Overlay
                                        : PauseReason::Clock;
        }

        [[nodiscard]] const char *pause_text(PauseReason reason) noexcept
        {
            switch (reason)
            {
            case PauseReason::Menu:
                return "the game menu";
            case PauseReason::Overlay:
                return "a UI overlay over the stopped game clock";
            case PauseReason::Clock:
                return "the stopped game clock (no game menu or UI overlay seen)";
            case PauseReason::None:
            default:
                return "no pause";
            }
        }

        /**
         * @brief Closes the pause the frames before this one stood still in: logs what it created, and counts it in
         *        s_pause_overlaps when it ended within SHADER_TWIN_PAUSE_SPACING_MS of a creation's barrier.
         * @note Main thread, the camera frame; no lock.
         */
        void end_pause(std::uint64_t now) noexcept
        {
            PauseState &pause = s_pause;
            if (pause.since_ms == 0)
            {
                return;
            }
            const double lasted = static_cast<double>(now - pause.since_ms) / 1000.0;
            if (pause.last_created_ms != 0 && now - pause.last_created_ms < Constants::SHADER_TWIN_PAUSE_SPACING_MS)
            {
                ++s_pause_overlaps;
                (void)DMK::log().try_log(DMK::LogLevel::Info,
                                         "ShaderTwins: {} ended {} ms after the barrier of a twin created in it; that "
                                         "twin's parse had finished inside the pause",
                                         pause_text(pause.reason), now - pause.last_created_ms);
            }
            if (pause.created != 0)
            {
                (void)DMK::log().try_log(DMK::LogLevel::Info,
                                         "ShaderTwins: {} ended after {:.1f} s with {} twin(s) created in it, {} of "
                                         "them ready once their barrier returned",
                                         pause_text(pause.reason), lasted, pause.created, pause.ready);
            }
            pause = PauseState{.off = pause.off};
        }

        /**
         * @brief Drops the pause inventory's candidates, both lists, outside every mod mutex.
         * @note Main thread or the teardown thread.
         */
        void clear_pause_inventory() noexcept
        {
            InventoryBatch left;
            InventoryBatch done;
            try
            {
                const std::lock_guard lock(s_main_mutex);
                left.entries.swap(s_pause_inventory);
                done.entries.swap(s_pause_inventory_done);
            }
            catch (...)
            {
                // A lock that cannot be taken leaves those references held: the shaders stay alive, nothing breaks.
            }
        }

        /**
         * @brief Puts in @p pooled the (shader, kind) pairs the pool holds, sorted, so the pause inventory does not
         *        queue them twice.
         * @return False, with @p pooled empty, when the candidate mutex is busy (a loader or a render job holds it for
         *         a moment): the caller tries again in a later frame.
         * @throws std::bad_alloc If the list cannot allocate.
         * @note Main thread, under s_main_mutex in a camera frame, so it never waits for the candidate mutex. Holds it
         *       only to copy the identities.
         */
        [[nodiscard]] bool pooled_pairs(std::vector<InventoryKey> &pooled)
        {
            pooled.clear();
            pooled.reserve(k_max_candidates);
            {
                const std::unique_lock lock(s_candidate_mutex, std::try_to_lock);
                if (!lock.owns_lock())
                {
                    return false;
                }
                for (std::size_t i = 0; i < s_candidate_count; ++i)
                {
                    pooled.push_back(InventoryKey{s_candidates[i].request.shader, s_candidates[i].request.kind});
                }
            }
            std::ranges::sort(pooled);
            return true;
        }

        /**
         * @brief Takes the level inventory in a frame the game stands still in, for a loading epoch whose boundary did
         *        not take one in this module (the first pause after a DEV hot reload) or whose inventory scoped its
         *        assets with camera rules edited since, and keeps the pairs it queued in s_pause_inventory for
         *        create_during_pause().
         * @details Runs once per epoch and rules generation, whatever comes of it: a walk that finds no level or fails
         *          is not repeated until the next boundary takes its own or the rules change again. Only a busy
         *          candidate mutex leaves it for a later frame, before anything is read. Pairs with a source already,
         *          a candidate in the pool (a render miss, a play-time capture) or a place in the pause inventory are
         *          not queued. A pair the new rules no longer scope keeps its twin, which only the scan's classes use.
         * @note Main thread, under s_main_mutex, in a frame the game stands still in (the level tables are written
         *       only while a level loads).
         */
        void pause_inventory(PauseReason reason) noexcept
        {
            const std::uint64_t epoch = s_load_epoch.load(std::memory_order_relaxed);
            const std::uint64_t rules = Rules::current_generation();
            // The epoch has an inventory already: this walk follows a rules edit.
            const bool rules_changed = s_inventory_epoch.load(std::memory_order_relaxed) == epoch;
            InventoryBatch inventory;
            InventoryOutcome outcome{};
            try
            {
                std::vector<InventoryKey> pooled;
                if (!pooled_pairs(pooled))
                {
                    return;
                }
                for (const TwinCandidate &waiting : s_pause_inventory)
                {
                    pooled.push_back(InventoryKey{waiting.request.shader, waiting.request.kind});
                }
                std::ranges::sort(pooled);
                s_inventory_epoch.store(epoch, std::memory_order_relaxed);
                s_inventory_rules.store(rules, std::memory_order_relaxed);
                std::vector<InventoryFound> found;
                outcome = walk_inventory(found);
                if (!outcome.ran)
                {
                    // Not taken: no pairs of an earlier load stay behind.
                    s_inventory_pairs.clear();
                }
                else
                {
                    queue_inventory(
                        found, [&pooled](std::uintptr_t shader, int kind)
                        { return std::ranges::binary_search(pooled, InventoryKey{shader, kind}); }, inventory, outcome);
                    // Room for every entry in both lists first, so moving one along never allocates.
                    const std::size_t total = s_pause_inventory.size() + inventory.entries.size();
                    s_pause_inventory.reserve(total);
                    s_pause_inventory_done.reserve(s_pause_inventory_done.size() + total);
                    s_pause_inventory.insert(s_pause_inventory.end(), inventory.entries.begin(),
                                             inventory.entries.end());
                    inventory.entries.clear();
                }
            }
            catch (...)
            {
                s_inventory_epoch.store(epoch, std::memory_order_relaxed);
                s_inventory_rules.store(rules, std::memory_order_relaxed);
                s_inventory_pairs.clear();
                outcome.ran = true;
                outcome.failed = true;
            }
            log_inventory(outcome);
            if (!outcome.failed && outcome.walk.level)
            {
                if (rules_changed)
                {
                    (void)DMK::log().try_log(DMK::LogLevel::Info,
                                             "ShaderTwins: the camera rules changed (generation {}), so this pause "
                                             "({}) took the level inventory again: {} pair(s) wait for their twin, "
                                             "created one at a time while the pause lasts",
                                             rules, pause_text(reason), s_pause_inventory.size());
                }
                else
                {
                    (void)DMK::log().try_log(DMK::LogLevel::Info,
                                             "ShaderTwins: the first pause of loading epoch {} ({}) took the level "
                                             "inventory no loading screen took for this module (a DEV hot reload): {} "
                                             "pair(s) wait for their twin, created one at a time while the pause lasts",
                                             epoch, pause_text(reason), s_pause_inventory.size());
                }
            }
        }

        /** @brief What take_pause_pick() takes out besides its pick: entries to drop once every mod mutex is free. */
        struct PauseStale
        {
            std::array<TwinCandidate, 16> entries{};
            std::size_t count = 0;
            ~PauseStale() noexcept
            {
                for (std::size_t i = 0; i < count; ++i)
                {
                    drop_candidate(entries[i]);
                }
            }
        };

        /**
         * @brief True when @p candidate is done with: its kind is off (kind_off(): a pause spends no barrier on a
         *        source slot with no twin), its pair has a source already, or its stock shader did not load (or reads
         *        back as anything but a loaded or loading shader). Sets @p loaded to whether it finished its parse.
         * @note Main thread, under s_main_mutex, while the candidate holds its shader.
         */
        [[nodiscard]] bool pause_candidate_done(const TwinCandidate &candidate, bool &loaded) noexcept
        {
            loaded = false;
            if (kind_off(candidate.request.kind) || find_source(candidate.request.shader, candidate.request.kind) >= 0)
            {
                return true;
            }
            const auto flags = DMK::memory::read<std::uint32_t>(
                DMK::Address{candidate.request.shader + Constants::SHADER_FLAGS_OFFSET});
            if (!flags || (*flags & Constants::SHADER_FLAG_NOT_FOUND) != 0)
            {
                return true;
            }
            loaded = (*flags & Constants::SHADER_FLAG_LOADED) != 0;
            return false;
        }

        /**
         * @brief Takes out the candidate a pause creates next: a variant a render job drew without its twin (a listed
         *        node's, then the character's), else the pause inventory's next one, else a plant or frame variant a
         *        material loaded during play; within each, the one waiting longest. Only a candidate whose stock
         *        shader finished its own parse is taken; one still parsing stays.
         * @details The pool's other characters' candidates (their eyes and hair from a loading screen) and those of a
         *          kind that is off are left for the next boundary. A pool candidate that is done with
         *          (pause_candidate_done()) leaves the pool into @p stale, at most its capacity per call; an inventory
         *          one (its kind off included) moves to s_pause_inventory_done, which keeps its references for the
         *          coverage line.
         * @param out Receives the pick, owned by the caller.
         * @param from_inventory Set when the pick is the pause inventory's.
         * @param left Receives how many candidates a pause could still create after this one.
         * @param stale Receives the pool's candidates that are done with, which the caller drops once it released
         *        every mod mutex.
         * @return True when a candidate was taken.
         * @note Main thread, under s_main_mutex. Takes the candidate mutex without waiting: a busy one leaves the pool
         *       for the next frame. Releases nothing.
         */
        [[nodiscard]] bool take_pause_pick(TwinCandidate &out, bool &from_inventory, int &left,
                                           PauseStale &stale) noexcept
        {
            from_inventory = false;
            left = 0;
            try
            {
                // Held through both lists, so the pool's best keeps its index; without it only the inventory is read.
                const std::unique_lock lock(s_candidate_mutex, std::try_to_lock);
                int pool_rank = -1;
                std::size_t pool_at = k_max_candidates;
                std::uint64_t pool_first = 0;
                for (std::size_t i = 0; lock.owns_lock() && i < s_candidate_count;)
                {
                    TwinCandidate &candidate = s_candidates[i];
                    const bool listed = listed_kind(candidate.request.kind);
                    // One of a kind that is off stays for the next boundary, which gives it a source slot after
                    // every other candidate: dropped here, a render job would retain it again the next frame.
                    if ((!listed && candidate.priority != 2) || kind_off(candidate.request.kind))
                    {
                        ++i;
                        continue;
                    }
                    bool loaded = false;
                    if (pause_candidate_done(candidate, loaded))
                    {
                        if (stale.count < stale.entries.size())
                        {
                            // The last entry, not looked at yet, takes this slot.
                            stale.entries[stale.count++] = candidate;
                            candidate = s_candidates[--s_candidate_count];
                            s_candidates[s_candidate_count] = TwinCandidate{};
                            continue;
                        }
                        ++i;
                        continue;
                    }
                    ++left;
                    const int rank = (candidate.priority == 2 ? 2 : 0) + (listed ? 1 : 0);
                    if (loaded && (rank > pool_rank || (rank == pool_rank && candidate.first_ms < pool_first)))
                    {
                        pool_rank = rank;
                        pool_at = i;
                        pool_first = candidate.first_ms;
                    }
                    ++i;
                }
                const auto take_pooled = [&out, &left](std::size_t at)
                {
                    out = s_candidates[at];
                    s_candidates[at] = s_candidates[--s_candidate_count];
                    s_candidates[s_candidate_count] = TwinCandidate{};
                    --left;
                };
                // A variant drawn now goes before the inventory's.
                if (pool_at < s_candidate_count && pool_rank >= 2)
                {
                    take_pooled(pool_at);
                    left += static_cast<int>(s_pause_inventory.size());
                    return true;
                }
                bool picked = false;
                for (std::size_t i = 0; i < s_pause_inventory.size();)
                {
                    const TwinCandidate candidate = s_pause_inventory[i];
                    bool loaded = false;
                    const bool done = pause_candidate_done(candidate, loaded);
                    if (!done && (picked || !loaded))
                    {
                        ++left;
                        ++i;
                        continue;
                    }
                    if (done)
                    {
                        // Its room was reserved for every entry the list ever had, so this does not allocate.
                        s_pause_inventory_done.push_back(candidate);
                    }
                    else
                    {
                        out = candidate;
                        from_inventory = true;
                        picked = true;
                    }
                    s_pause_inventory.erase(s_pause_inventory.begin() + static_cast<std::ptrdiff_t>(i));
                }
                if (picked)
                {
                    return true;
                }
                if (pool_at < s_candidate_count)
                {
                    take_pooled(pool_at);
                    return true;
                }
            }
            catch (...)
            {
                // Whatever was taken out stays owned by out, stale or s_pause_inventory_done.
            }
            return false;
        }

        /**
         * @brief Logs the pause inventory's coverage once its last candidate was handled, and hands back the handled
         *        ones, whose references the caller drops outside every mod mutex.
         * @note Main thread, under s_main_mutex, after the last candidate's barrier, while the handled ones still hold
         *       the shaders the coverage line names.
         */
        void finish_pause_inventory(InventoryBatch &handled) noexcept
        {
            const Coverage coverage = compute_coverage();
            if (coverage.plant + coverage.frame > 0)
            {
                (void)DMK::log().try_log(coverage.missing > 0 ? DMK::LogLevel::Warning : DMK::LogLevel::Info,
                                         "ShaderTwins: coverage after the pause inventory: plant {}/{} ready, frame "
                                         "{}/{} ready{}{}",
                                         coverage.plant_ready, coverage.plant, coverage.frame_ready, coverage.frame,
                                         coverage.missing > 0 ? "; missing " : "", coverage.missing_text.data());
            }
            handled.entries.swap(s_pause_inventory_done);
        }

        /**
         * @brief One step of the pause-time creation: once per loading epoch a boundary did not cover in this module
         *        (after a DEV hot reload) and once after each camera rules edit, the level inventory; else one twin,
         *        finished inside this frame.
         * @details Follows the loading screen's protocol for one candidate: under s_main_mutex, the cache recovery of
         *          a hot reload first (so nothing new takes the slot of a twin the previous module left loaded), the
         *          first-use preparation, the pending twins' poll, and the candidate's creation under the pause permit
         *          (create_twin() checks it, and that this is the engine's main thread); then with every mod mutex
         *          released, a render-command barrier, FlushRTCommands(true, true, true), so the twin's parse finishes
         *          on the render thread before this paused frame goes on and never lands on the first frame after the
         *          pause; then the twin's poll again and its log line. Only a candidate whose stock shader finished its
         *          own parse is created, so no barrier is needed before it. A barrier that fails turns the pause-time
         *          creation off for the session (the loading screens go on).
         * @note Main thread, the camera frame, from service_shader_twins_paused() once the pause passed its dwell and
         *       the spacing after the last creation.
         */
        void create_during_pause(std::uint64_t now, PauseReason reason) noexcept
        {
            PauseState &pause = s_pause;
            pause.servicing = true;
            struct Servicing
            {
                ~Servicing() noexcept { s_pause.servicing = false; }
            } servicing;
#if defined(TPVCAMERA_DEV_BUILD)
            s_pause_drained = false;
#endif
            // Owned while handled, dropped below outside every mod mutex (an inventory pick moves to its done list).
            TwinCandidate picked{};
            PauseStale stale;
            InventoryBatch handled;
            bool from_inventory = false;
            bool inventory_drained = false;
            const char *origin = "";
            int left = 0;
            int created_at = -1;
            std::uintptr_t renderer = 0;
            try
            {
                const std::unique_lock lock(s_main_mutex, std::try_to_lock);
                if (!lock.owns_lock() || !s_available.load(std::memory_order_acquire) ||
                    s_disabled.load(std::memory_order_acquire))
                {
                    return;
                }
                renderer = loading_renderer();
                if (renderer == 0)
                {
                    return;
                }
                recover_cached_twins(now);
                if (s_inventory_objman_global != 0 &&
                    (s_inventory_epoch.load(std::memory_order_relaxed) !=
                         s_load_epoch.load(std::memory_order_relaxed) ||
                     s_inventory_rules.load(std::memory_order_relaxed) != Rules::current_generation()))
                {
                    pause_inventory(reason);
                    // The walk was this frame's work; the first creation comes after the spacing.
                    pause.next_ms = GetTickCount64() + Constants::SHADER_TWIN_PAUSE_SPACING_MS;
                    return;
                }
                if (!prepare(now))
                {
                    return;
                }
                refresh_twins(now);
                if (s_source_count >= k_max_sources)
                {
                    if (!std::exchange(pause.full_logged, true))
                    {
                        (void)DMK::log().try_log(DMK::LogLevel::Info,
                                                 "ShaderTwins: all {} source slots are in use; this pause creates no "
                                                 "twin",
                                                 k_max_sources);
                    }
#if defined(TPVCAMERA_DEV_BUILD)
                    s_pause_drained = true;
#endif
                    return;
                }
                const bool had_inventory = !s_pause_inventory.empty();
                const bool taken = take_pause_pick(picked, from_inventory, left, stale);
                inventory_drained = had_inventory && s_pause_inventory.empty();
                if (!taken)
                {
                    // Nothing to create now (or the pool was busy): look again after the spacing.
                    pause.next_ms = now + Constants::SHADER_TWIN_PAUSE_SPACING_MS;
#if defined(TPVCAMERA_DEV_BUILD)
                    s_pause_drained = true;
#endif
                }
                else
                {
                    origin = from_inventory ? "the pause inventory" : candidate_origin(picked, false);
                    s_pause_permit = true;
                    const int before = s_source_count;
                    create_twin(picked.request, now);
                    s_pause_permit = false;
                    created_at = s_source_count > before ? before : -1;
                    if (from_inventory)
                    {
                        // Its room was reserved with the inventory, so this does not allocate.
                        s_pause_inventory_done.push_back(std::exchange(picked, TwinCandidate{}));
                    }
                }
                if (inventory_drained && created_at < 0)
                {
                    finish_pause_inventory(handled);
                }
            }
            catch (...)
            {
                s_pause_permit = false;
            }
            if (created_at < 0)
            {
                // Nothing was created, so nothing was parsed: the candidate goes (handled, or not a source any more).
                drop_candidate(picked);
                return;
            }
            // The twin's parse, queued by mfForName, runs on the render thread before this paused frame goes on.
            const std::uint64_t barrier_started = GetTickCount64();
            const bool completed = flush_loading_commands(renderer);
            const std::uint64_t finished = GetTickCount64();
            drop_candidate(picked);
            pause.next_ms = finished + Constants::SHADER_TWIN_PAUSE_SPACING_MS;
            pause.last_created_ms = finished;
            ++pause.created;
            ++s_pause_created_total;
            if (!completed)
            {
                pause.off = true;
                (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                         "ShaderTwins: the render-command barrier after a twin created during {} did "
                                         "not complete, so its parse may still run after the pause; no more twins are "
                                         "created during pauses this session (loading screens still create them)",
                                         pause_text(reason));
            }
            try
            {
                const std::lock_guard lock(s_main_mutex);
                if (!s_available.load(std::memory_order_acquire))
                {
                    return;
                }
                refresh_twins(finished);
                const Source &source = s_sources[static_cast<std::size_t>(created_at)];
                const bool ready = source.state == TwinState::Ready;
                pause.ready += ready ? 1 : 0;
                (void)DMK::log().try_log(
                    DMK::LogLevel::Info,
                    "ShaderTwins: {} twin requested for {} (gen mask {:#x}) from {} during {} ({:.1f} s into it); its "
                    "barrier took {} ms and it is {}; {} candidate(s) left",
                    blobs::k_twins[source.kind].label, source.name.data(), source.mask, origin, pause_text(reason),
                    static_cast<double>(barrier_started - pause.since_ms) / 1000.0, finished - barrier_started,
                    ready                                ? "ready"
                    : source.state == TwinState::Pending ? "still parsing"
                                                         : "given up",
                    left);
                if (inventory_drained)
                {
                    finish_pause_inventory(handled);
                }
            }
            catch (...)
            {
                // The twin's state is polled again next frame; only its log line is lost.
            }
        }

#if defined(TPVCAMERA_DEV_BUILD)
        /**
         * @brief A step of the DEV clone dry-run in a frame the game stands still in whose pause had nothing left to
         *        create (create_during_pause() set s_pause_drained): hands the dry-run the ready plant and frame twins
         *        once a twin became ready or checked out again (the pause inventory's and the play-time ones, and after
         *        a DEV hot reload those its recovery took), then compares at most TWIN_CLONE_DRY_RUN_PAUSE_PAIRS pairs
         *        or for TWIN_CLONE_DRY_RUN_PAUSE_US. While pairs are left, the next paused frame goes on at once rather
         *        than after the creation spacing; a creation still comes first in any frame it can.
         * @note Main thread, the camera frame, from service_shader_twins_paused(). Holds s_main_mutex only to list the
         *       pairs, never across the dry-run, which takes the HW shader factory's lock without waiting and calls the
         *       resource lookup.
         */
        void service_paused_dry_run(PauseReason reason) noexcept
        {
            if constexpr (TwinClone::k_stage == TwinClone::Stage::DryRun)
            {
                if (!std::exchange(s_pause_drained, false) || !TwinClone::dry_run_available())
                {
                    return;
                }
                try
                {
                    std::vector<TwinClone::DryRunPair> pairs;
                    {
                        const std::unique_lock lock(s_main_mutex, std::try_to_lock);
                        if (!lock.owns_lock() || !s_available.load(std::memory_order_acquire) ||
                            s_disabled.load(std::memory_order_acquire))
                        {
                            return;
                        }
                        pairs = dry_run_pairs(true);
                    }
                    (void)TwinClone::queue_dry_run(pairs, pause_text(reason));
                }
                catch (...)
                {
                    // A list that could not allocate is taken again in the next paused frame with nothing to create.
                }
                if (!TwinClone::dry_run_pending())
                {
                    return;
                }
                TwinClone::run_dry_run(Constants::TWIN_CLONE_DRY_RUN_PAUSE_US,
                                       Constants::TWIN_CLONE_DRY_RUN_PAUSE_PAIRS, pause_text(reason));
                if (TwinClone::dry_run_pending())
                {
                    s_pause.next_ms = GetTickCount64();
                }
            }
        }
#endif

        /**
         * @brief Once a second, when anything changed: the variants that wait for a pause or a loading screen (those a
         *        render job drew without their twin, those the pause inventory holds, the oldest wait), the plant and
         *        frame variants materials loaded during play, and the twins created during pauses so far.
         * @note Main thread, under s_main_mutex, from log_second(). Never waits for the candidate mutex: a busy one
         *       leaves the line for the next second.
         */
        void log_pause_waits(std::uint64_t now) noexcept
        {
            int waiting = 0;
            int drawn = 0;
            std::uint64_t oldest = now;
            try
            {
                const std::unique_lock lock(s_candidate_mutex, std::try_to_lock);
                if (!lock.owns_lock())
                {
                    return;
                }
                for (std::size_t i = 0; i < s_candidate_count; ++i)
                {
                    const TwinCandidate &candidate = s_candidates[i];
                    // The pause creates a plant or frame variant, or one a render job drew; another character's,
                    // and one of a kind that is off, waits for the next boundary.
                    if ((!listed_kind(candidate.request.kind) && candidate.priority != 2) ||
                        kind_off(candidate.request.kind) ||
                        find_source(candidate.request.shader, candidate.request.kind) >= 0)
                    {
                        continue;
                    }
                    ++waiting;
                    drawn += candidate.priority == 2 ? 1 : 0;
                    oldest = candidate.first_ms != 0 ? std::min(oldest, candidate.first_ms) : oldest;
                }
            }
            catch (...)
            {
                return;
            }
            const int inventory_left = static_cast<int>(s_pause_inventory.size());
            for (const TwinCandidate &candidate : s_pause_inventory)
            {
                oldest = candidate.first_ms != 0 ? std::min(oldest, candidate.first_ms) : oldest;
            }
            const std::uint32_t captures = s_playtime_captures.exchange(0, std::memory_order_relaxed);
            if (waiting == s_logged_waiting && inventory_left == s_logged_inventory_left && captures == 0 &&
                s_pause_created_total == s_logged_pause_created && s_pause_overlaps == s_logged_overlaps)
            {
                return;
            }
            s_logged_waiting = waiting;
            s_logged_inventory_left = inventory_left;
            s_logged_pause_created = s_pause_created_total;
            s_logged_overlaps = s_pause_overlaps;
            (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                     "ShaderTwins: {} variant(s) wait for a pause or loading screen ({} drawn without "
                                     "their twin, {} from the pause inventory; oldest {:.1f} s); {} play-time "
                                     "capture(s) in the last second; {} twin(s) created during pauses, {} pause(s) "
                                     "ended within {} ms of a creation",
                                     waiting + inventory_left, drawn, inventory_left,
                                     static_cast<double>(now - oldest) / 1000.0, captures, s_pause_created_total,
                                     s_pause_overlaps, Constants::SHADER_TWIN_PAUSE_SPACING_MS);
        }

        /**
         * @brief False when a plant or frame variant a material loads during play needs no candidate: the published
         *        swap set already names @p shader as a source of @p kind, or the source table is full.
         * @details The sources only ever grow while the module runs, so a match in any swap set, even one the main
         *          thread is replacing, is a source; a set that does not name it (or none yet) lets the candidate in,
         *          and the pause drops it if it had a source after all.
         * @note Any thread: a loader's material callback. Unlike a render job, a loader is not bound to the frame a set
         *       stays valid for, so it may read a set the main thread is rewriting: the scan is bounded by
         *       SHADER_TWIN_MAX_SOURCES and reads only integers, and since every set lists the append-only source table
         *       by index, a torn read can only miss a source (one more candidate, dropped by the pause), never find
         *       one the table does not hold.
         */
        [[nodiscard]] bool playtime_capture_wanted(std::uintptr_t shader, int kind) noexcept
        {
            const SwapSet *set = s_set_published.load(std::memory_order_acquire);
            if (set == nullptr)
            {
                return true;
            }
            if (set->sources_full)
            {
                return false;
            }
            const int count = std::min(set->source_count, k_max_sources);
            for (int i = 0; i < count; ++i)
            {
                const auto at = static_cast<std::size_t>(i);
                if (set->sources[at] == shader && set->kinds[at] == kind)
                {
                    return false;
                }
            }
            return true;
        }

        /**
         * @brief The Debug line of a plant or frame variant a material loaded during play, which waits for the next
         *        pause or loading screen for its twin.
         * @note The material-load callback, while it owns @p material and the item.
         */
        void log_playtime_capture(const ShaderItem &item, int kind, std::uintptr_t material) noexcept
        {
            if (!DMK::log().is_enabled(DMK::LogLevel::Debug))
            {
                return;
            }
            std::array<char, 64> name{};
            copy_text_field_guarded(item.shader, Constants::SHADER_RESOURCE_NAME_OFFSET, name.data(), name.size());
            std::array<char, 160> material_name{};
            copy_text_field_guarded(material, Constants::MATERIAL_NAME_OFFSET, material_name.data(),
                                    material_name.size());
            const auto mask =
                DMK::memory::read<std::uint64_t>(DMK::Address{item.shader + Constants::SHADER_GEN_MASK_OFFSET});
            (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                     "ShaderTwins: play-time {} variant {} (gen mask {:#x}) from material {} waits "
                                     "for a pause or loading screen",
                                     blobs::k_twins[kind].label, name.data(), mask.value_or(0), material_name.data());
        }

        /**
         * @brief LoadCGFMaterial detour: carries the asset path's twin scope bits to the LoadMaterialShader calls
         *        nested inside it, in a Win32 TLS slot (DMK [B-86]: no thread_local on a host thread).
         * @details The slot ORs the outer loads' bits in and restores them on return, so nested loads keep every
         *          enclosing asset's scopes.
         * @note Raw arbitrary-signature detour (DMK [B-84]): it must not throw, and every call in it is noexcept.
         */
        std::uintptr_t __fastcall load_cgf_material_detour(std::uintptr_t manager, const char *material,
                                                           const char *asset, std::uint32_t flags) noexcept
        {
            const DetourScope detour_scope;
            const auto original = s_load_cgf_material_original.load(std::memory_order_acquire);
            const auto slot = s_asset_scope_tls;
            struct AssetScope
            {
                DWORD slot;
                void *previous;
                ~AssetScope() noexcept
                {
                    if (slot != TLS_OUT_OF_INDEXES)
                    {
                        (void)TlsSetValue(slot, previous);
                    }
                }
            } scope{slot, slot != TLS_OUT_OF_INDEXES ? TlsGetValue(slot) : nullptr};
            if (slot != TLS_OUT_OF_INDEXES && s_available.load(std::memory_order_acquire))
            {
                const auto bits = reinterpret_cast<std::uintptr_t>(scope.previous) | twin_asset_scopes(asset);
                (void)TlsSetValue(slot, reinterpret_cast<void *>(bits));
            }
            return original(manager, material, asset, flags);
        }

        /**
         * @brief LoadMaterialShader detour: retains the shader item a material loaded as a candidate of every twin
         *        kind its asset scopes give it (collect_candidate()).
         * @note Raw arbitrary-signature detour (DMK [B-84]): it must not throw, so its work after the original call
         *       runs inside a local try/catch.
         */
        bool __fastcall load_material_shader_detour(std::uintptr_t material, std::uintptr_t parent, const char *shader,
                                                    std::uint64_t mask, std::uintptr_t resources,
                                                    std::uintptr_t public_params) noexcept
        {
            const DetourScope detour_scope;
            const auto original = s_load_material_shader_original.load(std::memory_order_acquire);
            const bool result = original(material, parent, shader, mask, resources, public_params);
            if (!result || !s_available.load(std::memory_order_acquire))
            {
                return result;
            }
            try
            {
                // Inside a loading screen every kind is captured; during play only a plant or frame variant, which
                // waits for the next pause (service_shader_twins_paused()): the character's twins come from loading
                // screens and from his own drawn items, so another character's eyes and hair never spend a pause's
                // parse.
                const bool loading = loading_materials_admitted();
                // ShaderItem contains only integers and bytes. DMK's typed reader deliberately excludes structs;
                // copy its representation into a local item while this material-load callback owns the material.
                ShaderItem item{};
                if (!DMK::memory::read_into(DMK::Address{material + Constants::MATERIAL_SHADER_ITEM_OFFSET},
                                            std::as_writable_bytes(std::span{&item, 1})) ||
                    item.shader == 0)
                {
                    return result;
                }
                std::uintptr_t scopes = twin_material_scopes(parent) | twin_material_scopes(material);
                if (s_asset_scope_tls != TLS_OUT_OF_INDEXES)
                {
                    scopes |= reinterpret_cast<std::uintptr_t>(TlsGetValue(s_asset_scope_tls));
                }
                if (loading)
                {
                    for_each_twin_kind(
                        item, scopes, true,
                        [&item](int kind)
                        {
                            (void)collect_candidate(
                                item, kind, blobs::k_twins[kind].scope == blobs::TwinScope::Dissolving ? 1 : 0, true);
                        });
                    return result;
                }
                for_each_twin_kind(item, scopes & (k_scope_plants | k_scope_frames), false,
                                   [&item, material](int kind)
                                   {
                                       if (!playtime_capture_wanted(item.shader, kind) ||
                                           !collect_candidate(item, kind, 0, false))
                                       {
                                           return;
                                       }
                                       s_playtime_captures.fetch_add(1, std::memory_order_relaxed);
                                       log_playtime_capture(item, kind, material);
                                   });
            }
            catch (...)
            {
                s_dropped_requests.fetch_add(1, std::memory_order_relaxed);
            }
            return result;
        }

        /**
         * @brief SetSystemGlobalState detour: opens material admission when a level load starts (states 1 to 9) and
         *        runs finalize_loading() when it ends (into state 10 or later), on the verified main thread only.
         * @note Raw arbitrary-signature detour (DMK [B-84]): it must not throw, and every call in it is noexcept.
         */
        void __fastcall set_system_state_detour(std::uintptr_t system, std::int32_t state) noexcept
        {
            const DetourScope detour_scope;
            const auto original = s_set_system_state_original.load(std::memory_order_acquire);
            // A bad/off-thread close can never authorize preparation, but it must still close admission. Otherwise
            // an unexpected engine transition could leave loader callbacks admitted during gameplay.
            if (state < 1 || state > 9)
            {
                s_level_loading.store(false, std::memory_order_release);
            }
            bool valid = false;
            bool opening = false;
            try
            {
                const auto expected =
                    DMK::memory::read<std::uintptr_t>(DMK::Address{s_genv + Constants::GENV_SYSTEM_OFFSET});
                const auto previous =
                    DMK::memory::read<std::int32_t>(DMK::Address{system + Constants::SYSTEM_GLOBAL_STATE_OFFSET});
                valid = s_available.load(std::memory_order_acquire) && expected && *expected == system && previous &&
                        *previous >= 0 && *previous <= 13 && state >= 0 && state <= 13 && loading_renderer() != 0;
                if (valid && !s_loading_finalizing.load(std::memory_order_acquire))
                {
                    opening = state >= 1 && state <= 9;
                    if (!opening)
                    {
                        s_level_loading.store(false, std::memory_order_release);
                        if (*previous >= 1 && *previous <= 9 && state >= 10)
                        {
                            finalize_loading("level load");
                        }
                    }
                }
            }
            catch (...)
            {
                s_level_loading.store(false, std::memory_order_release);
            }
            original(system, state);
            if (valid && opening && !s_loading_finalizing.load(std::memory_order_acquire))
            {
                if (!s_level_loading.exchange(true, std::memory_order_acq_rel))
                {
                    s_load_epoch.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        /**
         * @brief SaveGameManager::LoadCryEngineData detour: admits the material loads of an engine save load and runs
         *        finalize_loading() once it returns, on the verified main thread only.
         * @note Raw arbitrary-signature detour (DMK [B-84]): it must not throw, and every call in it is noexcept.
         */
        bool __fastcall save_game_load_detour(std::uintptr_t manager, std::uintptr_t chunk,
                                              std::uintptr_t save_path) noexcept
        {
            const DetourScope detour_scope;
            const auto original = s_save_game_load_original.load(std::memory_order_acquire);
            const bool admitted = s_available.load(std::memory_order_acquire) && loading_renderer() != 0 &&
                                  !s_loading_finalizing.load(std::memory_order_acquire);
            struct SaveScope
            {
                bool admitted;
                std::uintptr_t previous;
                ~SaveScope() noexcept
                {
                    if (admitted)
                    {
                        s_loading_save_manager.store(previous, std::memory_order_release);
                    }
                }
            } scope{admitted, admitted ? s_loading_save_manager.exchange(manager, std::memory_order_acq_rel) : 0};
            if (admitted)
            {
                s_load_epoch.fetch_add(1, std::memory_order_relaxed);
            }
            const bool result = original(manager, chunk, save_path);
            // This wrapper's caller still owns the loading screen, including the failed-load return path. The
            // manager's engine-loading byte is already clear here; it only admits material callbacks inside original.
            if (admitted)
            {
                finalize_loading("engine save load");
            }
            return result;
        }

        /** @brief Queues @p track's swap-off for apply_refills(). */
        void queue_release(const Track &track, const char *why) noexcept
        {
            if (s_released_count < static_cast<int>(s_released.size()))
            {
                Track &released = s_released[static_cast<std::size_t>(s_released_count++)];
                released = track;
                released.why = why;
            }
        }

        /**
         * @brief Adds the newly listed nodes (swapped on), holds the unlisted ones, and drops those whose hold ran out.
         * @details Two passes, so a node listed this frame is never the one evicted: first mark the listed nodes that
         *          already have a track, then give the new ones a slot. With the table full a new node takes the slot
         *          of the unlisted track with the least hold left; at most TREE_HOLE_MAX_NODES nodes are taken, the
         *          frames first, so such a track exists. A node whose temp data does not name it back is not live and
         *          is skipped. The refills themselves wait for apply_refills().
         */
        void update_tracks(std::span<void *const> trees, std::span<void *const> frames, float delta_time,
                           std::uint64_t now) noexcept
        {
            for (int i = 0; i < s_track_count; ++i)
            {
                s_tracks[static_cast<std::size_t>(i)].seen = false;
            }
            std::array<void *, Constants::TREE_HOLE_MAX_NODES> nodes{};
            const std::size_t n_frames = std::min(frames.size(), nodes.size());
            const std::size_t listed = n_frames + std::min(trees.size(), nodes.size() - n_frames);
            std::copy_n(frames.begin(), n_frames, nodes.begin());
            std::copy_n(trees.begin(), listed - n_frames, nodes.begin() + static_cast<std::ptrdiff_t>(n_frames));
            for (std::size_t i = 0; i < listed; ++i)
            {
                const int found = find_track(reinterpret_cast<std::uintptr_t>(nodes[i]));
                if (found < 0)
                {
                    continue;
                }
                Track &track = s_tracks[static_cast<std::size_t>(found)];
                if (track.frame != (i < n_frames))
                {
                    // The camera rules moved the node between a plant and a frame (a rule whose check gives the other
                    // kind, or new rules), so its twin kind changed: it is swapped off and taken again below.
                    queue_release(track, "kind changed");
                    const auto at = s_tracks.begin() + static_cast<std::ptrdiff_t>(found);
                    std::copy(at + 1, s_tracks.begin() + static_cast<std::ptrdiff_t>(s_track_count), at);
                    --s_track_count;
                    continue;
                }
                track.seen = true;
                track.hold = Constants::TREE_HOLE_HOLD_SECONDS;
            }
            for (std::size_t i = 0; i < listed; ++i)
            {
                const auto node = reinterpret_cast<std::uintptr_t>(nodes[i]);
                if (node == 0 || find_track(node) >= 0)
                {
                    continue;
                }
                if (const Invalidate state = node_state_guarded(node);
                    state == Invalidate::NotTheNode || state == Invalidate::Fault)
                {
                    trace_event(trace::Event::TrackRejected, node, 0, 0, 0, static_cast<std::uint64_t>(state));
                    (void)DMK::log().try_log(DMK::LogLevel::Debug, "ShaderTwins: node {:#x} skipped ({})", node,
                                             invalidate_text(state));
                    continue;
                }
                int slot = -1;
                if (s_track_count < Constants::TREE_HOLE_MAX_NODES)
                {
                    slot = s_track_count++;
                }
                else
                {
                    for (int t = 0; t < s_track_count; ++t)
                    {
                        const Track &candidate = s_tracks[static_cast<std::size_t>(t)];
                        if (!candidate.seen &&
                            (slot < 0 || candidate.hold < s_tracks[static_cast<std::size_t>(slot)].hold))
                        {
                            slot = t;
                        }
                    }
                    if (slot < 0)
                    {
                        trace_event(trace::Event::TrackRejected, node, 0, 0, 0, 4);
                        continue;
                    }
                    queue_release(s_tracks[static_cast<std::size_t>(slot)], "evicted");
                }
                Track &track = s_tracks[static_cast<std::size_t>(slot)];
                track = Track{.node = node,
                              .hold = Constants::TREE_HOLE_HOLD_SECONDS,
                              .seen = true,
                              .fresh = true,
                              .swapped_ms = now};
                track.frame = i < n_frames;
                track.merged = merged_origin_guarded(node, track.origin.data());
                if (track.merged)
                {
                    // A merged grass patch has no single mesh to name; its origin tells the patches apart.
                    (void)std::format_to_n(track.name.data(), track.name.size() - 1, "grass patch at ({:.0f}, {:.0f})",
                                           track.origin[0], track.origin[1]);
                }
                else
                {
                    render_node_name(nodes[i], track.name.data(), static_cast<int>(track.name.size()));
                }
                trace_event(trace::Event::TrackAdded, node, 0, 0, 0, track.frame, track.merged);
                trace::label(node, track.name.data());
            }

            int kept = 0;
            for (int i = 0; i < s_track_count; ++i)
            {
                Track &track = s_tracks[static_cast<std::size_t>(i)];
                if (!track.seen)
                {
                    track.hold -= delta_time;
                    if (track.hold <= 0.0f)
                    {
                        queue_release(track, "unlisted");
                        continue;
                    }
                }
                s_tracks[static_cast<std::size_t>(kept++)] = track;
            }
            s_track_count = kept;
        }

        /**
         * @brief Runs the frame's refills after the swap set is published: the nodes swapped on, the nodes swapped
         *        off, and every swapped node when a twin became ready.
         */
        void apply_refills(std::uint64_t now) noexcept
        {
            for (int i = 0; i < s_released_count; ++i)
            {
                Track &released = s_released[static_cast<std::size_t>(i)];
                release_track(released, now, released.why);
            }
            s_released_count = 0;
            const bool plants_ready = scope_ready(blobs::TwinScope::ListedPlants);
            const bool frames_ready = scope_ready(blobs::TwinScope::ListedFrames);
            int refilled = 0;
            for (int i = 0; i < s_track_count; ++i)
            {
                Track &track = s_tracks[static_cast<std::size_t>(i)];
                if (!track.fresh && !s_refill_all)
                {
                    continue;
                }
                // Publishing a target permits passive discovery on natural render submissions. Until a twin can
                // be selected, forcing its persistent objects to rebuild only recreates the stock rendering.
                if (!(track.frame ? frames_ready : plants_ready))
                {
                    continue;
                }
                track.activated = true;
                const Invalidate result =
                    refill(track, track.fresh ? trace::RefillReason::Activate : trace::RefillReason::SourceReady);
                refilled += result == Invalidate::Done ? 1 : 0;
                if (track.fresh)
                {
                    track.fresh = false;
                    (void)DMK::log().try_log(DMK::LogLevel::Debug, "ShaderTwins: swapped on {} (node {:#x}, {})",
                                             track.name.data(), track.node, invalidate_text(result));
                }
            }
            if (s_refill_all)
            {
                s_refill_all = false;
                (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                         "ShaderTwins: refilled {} of {} swapped tree(s) for a twin that became ready",
                                         refilled, s_track_count);
            }
        }

        /** @brief Schedules a retry refill of @p track, which lost a wait; an earlier one still due keeps its time. */
        void mark_retry(Track &track, std::uint64_t now) noexcept
        {
            if (!track.retry)
            {
                trace_event(trace::Event::RetryScheduled, track.node, 0, 0, 0, Constants::TREE_HOLE_RETRY_MS);
                track.retry = true;
                track.retry_ms = now;
            }
        }

        /**
         * @brief Takes the records the render threads handed over, keeping the newest per compiled object of a swapped
         *        tree (with the time its wait began).
         * @details A record that does not fit the list schedules a retry of its tree, and so does, for every swapped
         *          tree, one a render thread could not hand over (it cannot say for which tree).
         */
        void drain_pso_records(std::uint64_t now) noexcept
        {
            if (s_pso_record_lost.exchange(false, std::memory_order_relaxed))
            {
                for (int t = 0; t < s_track_count; ++t)
                {
                    mark_retry(s_tracks[static_cast<std::size_t>(t)], now);
                }
            }
            std::uintptr_t deferred_node = 0;
            while (s_deferred_nodes.pop(deferred_node))
            {
                const int track = find_track(deferred_node);
                if (track >= 0 && !s_tracks[static_cast<std::size_t>(track)].deferred)
                {
                    trace_event(trace::Event::DeferredScheduled, deferred_node, 0, 0, 0,
                                Constants::TREE_HOLE_DEFER_RETRY_MS);
                    s_tracks[static_cast<std::size_t>(track)].deferred = true;
                    s_tracks[static_cast<std::size_t>(track)].deferred_ms = now;
                }
            }
            PsoRecord record{};
            while (s_pso_records.pop(record))
            {
                trace_event(trace::Event::RecordReceived, record.node, record.object, record.twin_shader, 0,
                            record.slots, record.rt_mask);
                const int track = find_track(record.node);
                if (track < 0)
                {
                    trace_event(trace::Event::RecordDropped, record.node, record.object, record.twin_shader, 0, 3);
                    drop_record(record);
                    continue;
                }
                int slot = -1;
                for (int i = 0; i < s_pending_count && slot < 0; ++i)
                {
                    const PsoRecord &held = s_pending[static_cast<std::size_t>(i)].record;
                    slot = (held.object == record.object && held.node == record.node) ? i : -1;
                }
                if (slot >= 0)
                {
                    PendingPsos &pending = s_pending[static_cast<std::size_t>(slot)];
                    drop_record(pending.record);
                    pending.record = record;
#if defined(TPVCAMERA_DEV_BUILD) && defined(DMK_ENABLE_PROFILING)
                    pending.traced_state = 255;
                    pending.traced_slots = 0;
                    pending.traced_attempts_ms = 0;
#endif
                    trace_event(trace::Event::RecordReplaced, record.node, record.object, record.twin_shader, 0,
                                record.slots, pending.first_ms);
                    continue;
                }
                if (s_pending_count >= static_cast<int>(s_pending.size()))
                {
                    trace_event(trace::Event::RecordLost, record.node, record.object, record.twin_shader, 0, 2);
                    s_pso_dropped.fetch_add(1, std::memory_order_relaxed);
                    mark_retry(s_tracks[static_cast<std::size_t>(track)], now);
                    drop_record(record);
                    continue;
                }
                s_pending[static_cast<std::size_t>(s_pending_count++)] = PendingPsos{.record = record, .first_ms = now};
            }
        }

        /** @brief The twin PSOs a ready record brings, for the log. */
        [[nodiscard]] int twin_pso_count(const PsoRecord &record) noexcept
        {
            int count = 0;
            for (int i = 0; i < k_pso_slots; ++i)
            {
                count += ((record.slots & (1u << i)) != 0 && record.twin[static_cast<std::size_t>(i)] != 0) ? 1 : 0;
            }
            return count;
        }

        /**
         * @brief Refills each swapped tree whose compiled objects can now take the twin's PSOs.
         * @details A tree is refilled once every compiled object it waits on has a ready twin set, or once one has and
         *          the oldest wait passed SHADER_TWIN_PSO_WAIT_LOG_SECONDS (a twin permutation that failed to compile
         *          never becomes valid, and its object keeps the stock look). The refill drops the tree's records, and
         *          the rebuild finds the PSOs in the engine's caches and keeps the twin's, or hands over new records.
         *          A tree still waiting past that time is logged once with the twin and the RT mask it waits for. A
         *          tree that holds no record but lost one is refilled again TREE_HOLE_RETRY_MS after it lost it.
         */
        void poll_pso_records(std::uint64_t now) noexcept
        {
            for (int t = 0; t < s_track_count; ++t)
            {
                Track &track = s_tracks[static_cast<std::size_t>(t)];
                if (!track.activated)
                {
                    continue;
                }
                int waiting = 0;
                int ready = 0;
                int psos = 0;
                std::uint64_t oldest = now;
                std::uintptr_t stuck_twin = 0;
                std::uint64_t stuck_mask = 0;
                bool stuck_built = true;
                for (int i = 0; i < s_pending_count; ++i)
                {
                    PendingPsos &pending = s_pending[static_cast<std::size_t>(i)];
                    const PsoRecord &record = pending.record;
                    if (record.node != track.node)
                    {
                        continue;
                    }
                    ++waiting;
                    oldest = std::min(oldest, pending.first_ms);
                    const auto state =
                        record.twin_built ? pso_set_ready_guarded(record.stock.data(), record.twin.data(), record.slots)
                                          : PsoReady::Waiting;
#if defined(TPVCAMERA_DEV_BUILD) && defined(DMK_ENABLE_PROFILING)
                    trace_pending_psos(pending, now);
                    const auto traced_state = record.twin_built ? static_cast<std::uint8_t>(state) : std::uint8_t{3};
                    if (pending.traced_state != traced_state)
                    {
                        pending.traced_state = traced_state;
                        trace_event(trace::Event::RecordState, record.node, record.object, record.twin_shader, 0,
                                    traced_state, pending.first_ms);
                    }
#endif
                    if (record.twin_built && state == PsoReady::Ready)
                    {
                        ++ready;
                        psos += twin_pso_count(record);
                    }
                    else if (stuck_twin == 0)
                    {
                        stuck_twin = record.twin_shader;
                        stuck_mask = record.rt_mask;
                        stuck_built = record.twin_built;
                    }
                }
                if (waiting == 0)
                {
                    // A compiled object that kept its stock look past the frame's twin-build budget is built again
                    // shortly, in another frame.
                    if (track.deferred && now - track.deferred_ms >= Constants::TREE_HOLE_DEFER_RETRY_MS)
                    {
                        (void)refill(track, trace::RefillReason::Deferred);
                        continue;
                    }
                    // Nothing holds a wait of this tree, so a dropped one would leave it on the stock look for as long
                    // as it is listed: refill it, and its compiled objects hand over new waits (or take the twin).
                    if (track.retry && now - track.retry_ms >= Constants::TREE_HOLE_RETRY_MS)
                    {
                        const Invalidate result = refill(track, trace::RefillReason::LostRecord);
                        (void)DMK::log().try_log(
                            DMK::LogLevel::Debug,
                            "ShaderTwins: {} refilled again, a wait of it was dropped {:.1f} s ago "
                            "({})",
                            track.name.data(), static_cast<double>(now - track.retry_ms) / 1000.0,
                            invalidate_text(result));
                    }
                    continue;
                }
                const double waited = static_cast<double>(now - oldest) / 1000.0;
                const bool overdue = waited > static_cast<double>(Constants::SHADER_TWIN_PSO_WAIT_LOG_SECONDS);
                if (ready > 0 && (ready == waiting || overdue))
                {
                    const Invalidate result = refill(track, trace::RefillReason::PsosReady);
                    ++s_promotions;
                    track.wait_logged = false;
                    (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                             "ShaderTwins: {} twin PSOs ready ({} of {} compiled object(s), {} PSO(s)) "
                                             "{:.2f} s after track creation, {:.2f} s since oldest pending record "
                                             "(internal timing; visible delay unmeasured; {})",
                                             track.name.data(), ready, waiting, psos,
                                             static_cast<double>(now - track.swapped_ms) / 1000.0, waited,
                                             invalidate_text(result));
                }
                else if (overdue && !track.wait_logged)
                {
                    track.wait_logged = true;
                    (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                             "ShaderTwins: {} still waits for the twin's PSOs after {:.0f} s ({} of {} "
                                             "compiled object(s) ready; twin {:#x}, RT mask {:#x}{}); it keeps the "
                                             "stock look, and a permutation that failed to compile keeps it so",
                                             track.name.data(), waited, ready, waiting, stuck_twin, stuck_mask,
                                             stuck_built ? "" : ", the twin build failed");
                }
            }
        }

        /** @brief The source index whose twin is @p twin, or -1. */
        [[nodiscard]] int find_source_by_twin(std::uintptr_t twin) noexcept
        {
            for (int i = 0; i < s_source_count; ++i)
            {
                if (s_sources[static_cast<std::size_t>(i)].twin == twin)
                {
                    return i;
                }
            }
            return -1;
        }

        /**
         * @brief Sets whether @p source's twin is warm for the fade, for the swap set and the detour; the entry of a
         *        given-up twin reads done whatever it is set to, so no dissolving build of its source builds a set of
         *        it aside any more.
         */
        void set_fade_warm(Source &source, bool warm) noexcept
        {
            source.warm = warm;
            if (source.warm_slot >= 0)
            {
                s_warm_done[static_cast<std::size_t>(source.warm_slot)].store(warm || source.state == TwinState::Failed,
                                                                              std::memory_order_release);
            }
        }

        /**
         * @brief Turns @p source cold when a render thread reported a fading build of its twin that had to keep the
         *        stock PSOs (build_fading_psos()): the next swap set leaves its dissolving items on the stock shader,
         *        and the warm-up holds the set that build handed over until it can draw.
         */
        void take_cold_report(Source &source, std::uint64_t now) noexcept
        {
            if (source.warm_slot < 0 ||
                !s_warm_cold[static_cast<std::size_t>(source.warm_slot)].exchange(false, std::memory_order_acq_rel))
            {
                return;
            }
            const bool was_warm = source.warm;
            source.cold = true;
            source.cold_ms = now;
            source.drawable_since_ms = 0;
            set_fade_warm(source, false);
            if (was_warm)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "ShaderTwins: {} twin {:#x} for {} turned cold: a fading build asked for a permutation not "
                    "compiled yet and kept the stock PSOs; its dissolving items go back to the stock shader while it "
                    "warms up again",
                    blobs::k_twins[source.kind].label, source.twin, source.name.data());
            }
        }

        /** @brief take_cold_report() for every source. */
        void take_cold_reports(std::uint64_t now) noexcept
        {
            for (int i = 0; i < s_source_count; ++i)
            {
                take_cold_report(s_sources[static_cast<std::size_t>(i)], now);
            }
        }

        /** @brief The index of the kept set whose key is @p key, or -1. */
        [[nodiscard]] int find_warm_entry(std::uint64_t key) noexcept
        {
            for (int i = 0; i < s_warm_entry_count; ++i)
            {
                if (s_warm_entries[static_cast<std::size_t>(i)].record.key == key)
                {
                    return i;
                }
            }
            return -1;
        }

        /** @brief Drops the PSO references a kept set holds, so a warm one holds none. */
        void release_entry_psos(WarmEntry &entry) noexcept
        {
            drop_record(entry.record);
            entry.record.stock = {};
            entry.record.twin = {};
        }

        /**
         * @brief Makes room for one more kept set by dropping the one that turned warm first (it holds no PSO, and
         *        its key stays taken, so its set is not built again).
         * @return The freed index, or -1 when every kept set still waits.
         */
        [[nodiscard]] int make_warm_room() noexcept
        {
            int oldest = -1;
            for (int i = 0; i < s_warm_entry_count; ++i)
            {
                const WarmEntry &entry = s_warm_entries[static_cast<std::size_t>(i)];
                if (entry.warm &&
                    (oldest < 0 || entry.warm_ms < s_warm_entries[static_cast<std::size_t>(oldest)].warm_ms))
                {
                    oldest = i;
                }
            }
            return oldest;
        }

        [[nodiscard]] const char *origin_text(WarmOrigin origin) noexcept
        {
            switch (origin)
            {
            case WarmOrigin::Ahead:
                return "built ahead";
            case WarmOrigin::Warming:
                return "built aside by a fading build of the stock shader";
            case WarmOrigin::Cold:
                return "from a fading build that kept the stock PSOs";
            case WarmOrigin::None:
            default:
                return "of unknown origin";
            }
        }

        /**
         * @brief Takes the character twin sets the render threads handed over and keeps one per description key.
         * @details A set built ahead or aside whose key is already kept is a duplicate (the key table was cleared
         *          while it was built) and goes. A set from a fading build that kept the stock PSOs replaces the kept
         *          one of its key, which waits again if it was warm, and ends its twin's cold hold, since that set now
         *          holds the twin back. With the table full, the set that turned warm first makes room; with every set
         *          still waiting, the new one goes and its key is freed, so a later build makes it again.
         */
        void drain_warm_records(std::uint64_t now) noexcept
        {
            PsoRecord record{};
            while (s_warm_records.pop(record))
            {
                const int index = find_source_by_twin(record.twin_shader);
                if (index < 0 || s_sources[static_cast<std::size_t>(index)].state != TwinState::Ready)
                {
                    drop_record(record);
                    continue;
                }
                Source &source = s_sources[static_cast<std::size_t>(index)];
                const bool cold_set = record.origin == WarmOrigin::Cold;
                int at = find_warm_entry(record.key);
                if (at >= 0)
                {
                    WarmEntry &kept = s_warm_entries[static_cast<std::size_t>(at)];
                    if (!cold_set)
                    {
                        drop_record(record);
                        continue;
                    }
                    // The fading build's set replaces the kept one; the masks at rest, for the log, stay.
                    const std::uint64_t rest_rt_mask = kept.record.rest_rt_mask;
                    const std::uint64_t rest_flags = kept.record.rest_flags;
                    drop_record(kept.record);
                    kept.record = record;
                    kept.record.rest_rt_mask = rest_rt_mask;
                    kept.record.rest_flags = rest_flags;
                    kept.twin_attempts = {};
                    kept.stalled_ms = 0;
                    kept.stalled_polls = 0;
                    if (kept.warm)
                    {
                        kept.warm = false;
                        kept.wait_logged = false;
                        kept.first_ms = now;
                    }
                    source.cold = false;
                    continue;
                }
                at = s_warm_entry_count < static_cast<int>(s_warm_entries.size()) ? s_warm_entry_count++
                                                                                  : make_warm_room();
                if (at < 0)
                {
                    ++s_warm_entries_dropped;
                    if (!cold_set)
                    {
                        release_key(record.key);
                    }
                    drop_record(record);
                    continue;
                }
                s_warm_entries[static_cast<std::size_t>(at)] =
                    WarmEntry{.record = record, .source = index, .first_ms = now};
                source.cold = source.cold && !cold_set;
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "ShaderTwins: {} twin {:#x} for {}: a set for the fade {} (RT mask {:#x}, flags "
                    "{:#x}; at rest RT mask {:#x}, flags {:#x}), {} PSO slot(s)",
                    blobs::k_twins[source.kind].label, source.twin, source.name.data(), origin_text(record.origin),
                    record.rt_mask, record.flags, record.rest_rt_mask, record.rest_flags, std::popcount(record.slots));
            }
        }

        /** @brief The kept sets that still wait. */
        [[nodiscard]] int count_waiting_entries() noexcept
        {
            int waiting = 0;
            for (int i = 0; i < s_warm_entry_count; ++i)
            {
                waiting += s_warm_entries[static_cast<std::size_t>(i)].warm ? 0 : 1;
            }
            return waiting;
        }

        /**
         * @brief Gives the twin of the kept set at @p at up for the session, because that set cannot draw (see
         *        Constants::CHARACTER_TWIN_GIVE_UP_MS): its source is Failed, so its items keep the stock shader, at
         *        rest too, no dissolving build of it builds a set aside any more, and every kept set of it goes (their
         *        keys stay taken, so none is built again).
         * @details refresh_character_ready() does not let a given-up Hair or IllumFade twin hold the eyes back, so
         *          they still fade and the close-up fade hides the head only at the very end; a given-up Eye twin
         *          keeps the eyes from fading, and the head is hidden most of the way along.
         */
        void give_up_fade(int at, std::uint64_t now) noexcept
        {
            const WarmEntry stuck = s_warm_entries[static_cast<std::size_t>(at)];
            Source &source = s_sources[static_cast<std::size_t>(stuck.source)];
            source.state = TwinState::Failed;
            source.cold = false;
            source.drawable_since_ms = 0;
            set_fade_warm(source, false);
            int kept = 0;
            for (int i = 0; i < s_warm_entry_count; ++i)
            {
                const WarmEntry &entry = s_warm_entries[static_cast<std::size_t>(i)];
                if (entry.source == stuck.source)
                {
                    drop_record(entry.record);
                    continue;
                }
                if (kept != i)
                {
                    s_warm_entries[static_cast<std::size_t>(kept)] = entry;
                }
                ++kept;
            }
            const int dropped = s_warm_entry_count - kept;
            s_warm_entry_count = kept;
            const bool eye = source.kind == k_eye_kind;
            (void)DMK::log().try_log(
                DMK::LogLevel::Warning,
                "ShaderTwins: {} twin {:#x} for {} given up for the fade: a set of it ({}, RT mask {:#x}, flags {:#x}) "
                "{} for {:.0f} s ({:.0f} s after it arrived); its items keep the stock shader for the session ({} kept "
                "set(s) dropped), {}",
                blobs::k_twins[source.kind].label, source.twin, source.name.data(), origin_text(stuck.record.origin),
                stuck.record.rt_mask, stuck.record.flags,
                stuck.record.twin_built ? "has a pass with no twin PSO, or one the engine stopped trying to create "
                                          "(its permutation failed to compile),"
                                        : "could not be built (the twin build failed)",
                static_cast<double>(now - stuck.stalled_ms) / 1000.0,
                static_cast<double>(now - stuck.first_ms) / 1000.0, dropped,
                eye ? "and the close-up fade hides the head most of the way along"
                    : "the eyes can still fade, and then the close-up fade hides the head only at the very end");
        }

        /**
         * @brief Polls every kept set that still waits, and lets it go warm once it can draw every pass its stock set
         *        draws (warm_set_ready_guarded()); a warm set drops its PSO references.
         * @details A set waits for as long as its PSOs compile: the engine compiles on one thread, and a first run can
         *          take tens of seconds for all of them. One that still waits after SHADER_TWIN_PSO_WAIT_LOG_SECONDS is
         *          logged once. A set that cannot draw for good (its twin build failed, or a pass its stock set draws
         *          has a twin PSO the factory stopped creating, twin_stalled_guarded()) for CHARACTER_TWIN_GIVE_UP_MS
         *          and CHARACTER_TWIN_GIVE_UP_POLLS polls in a row gives its twin up (give_up_fade(), one twin per
         *          frame). Nothing else drops a waiting set but a level change and the shutdown. Only the valid bytes
         *          and state words of the PSOs a waiting set holds are read.
         */
        void poll_warm_entries(std::uint64_t now) noexcept
        {
            int pending = 0;
            int stuck = -1;
            for (int i = 0; i < s_warm_entry_count; ++i)
            {
                WarmEntry &entry = s_warm_entries[static_cast<std::size_t>(i)];
                if (entry.warm)
                {
                    continue;
                }
                PsoRecord &record = entry.record;
                bool stock_idle = false;
                const bool ready =
                    record.twin_built && warm_set_ready_guarded(record.stock.data(), record.twin.data(), record.slots,
                                                                record.attempts.data(), stock_idle) == PsoReady::Ready;
                if (ready && (!stock_idle || now - entry.first_ms >= Constants::CHARACTER_TWIN_SETTLE_MS))
                {
                    entry.warm = true;
                    entry.warm_ms = now;
                    release_entry_psos(entry);
                    continue;
                }
                ++pending;
                const bool stalled =
                    !record.twin_built || twin_stalled_guarded(record.stock.data(), record.twin.data(), record.slots,
                                                               entry.twin_attempts.data());
                if (!stalled)
                {
                    entry.stalled_ms = 0;
                    entry.stalled_polls = 0;
                }
                else
                {
                    entry.stalled_ms = entry.stalled_ms != 0 ? entry.stalled_ms : now;
                    ++entry.stalled_polls;
                    if (stuck < 0 && now - entry.stalled_ms >= Constants::CHARACTER_TWIN_GIVE_UP_MS &&
                        entry.stalled_polls >= Constants::CHARACTER_TWIN_GIVE_UP_POLLS)
                    {
                        stuck = i;
                    }
                }
                const double waited = static_cast<double>(now - entry.first_ms) / 1000.0;
                if (!entry.wait_logged && waited > static_cast<double>(Constants::SHADER_TWIN_PSO_WAIT_LOG_SECONDS))
                {
                    entry.wait_logged = true;
                    const Source &source = s_sources[static_cast<std::size_t>(entry.source)];
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Warning,
                        "ShaderTwins: {} twin {:#x} for {}: a set for the fade ({}, RT mask {:#x}{}) "
                        "still waits for its PSOs after {:.0f} s; it is held while they compile, "
                        "and until then its dissolving items keep the stock shader and the "
                        "close-up fade hides the head",
                        blobs::k_twins[source.kind].label, source.twin, source.name.data(), origin_text(record.origin),
                        record.rt_mask,
                        !record.twin_built ? ", the twin build failed"
                        : stalled          ? ", a twin PSO no longer compiling"
                                           : "",
                        waited);
                }
            }
            if (stuck >= 0)
            {
                give_up_fade(stuck, now);
                pending = count_waiting_entries();
            }
            s_warm_pending_count = pending;
        }

        /**
         * @brief Drops the kept sets of every other character render node than @p root and empties the key table, for
         *        a level change (another character render node held for CHARACTER_ROOT_SETTLE_FRAMES): the old
         *        materials and their local PSO caches are gone, so the old keys name nothing that will be built again
         *        (and a reused address could make one look built), and an old set that still waited would hold its
         *        twin back for good.
         * @details The sets built for @p root since it appeared stay, and their keys are taken again: his items took
         *          the twins at once, and their pooled compiled objects are not built again while he stays at rest, so
         *          those sets would never be built ahead again. A twin stays warm only when it was and every set of it
         *          kept now can draw; refresh_fade_warm() warms the others once theirs can. A given-up twin stays
         *          given up. Main thread, after this frame's drain_warm_records().
         */
        void forget_warm_sets(std::uintptr_t root, const char *why) noexcept
        {
            int kept = 0;
            for (int i = 0; i < s_warm_entry_count; ++i)
            {
                const WarmEntry &entry = s_warm_entries[static_cast<std::size_t>(i)];
                if (entry.record.node != root)
                {
                    drop_record(entry.record);
                    continue;
                }
                if (kept != i)
                {
                    s_warm_entries[static_cast<std::size_t>(kept)] = entry;
                }
                ++kept;
            }
            const int dropped = s_warm_entry_count - kept;
            s_warm_entry_count = kept;
            for (std::atomic<std::uint64_t> &key : s_warm_keys)
            {
                key.store(k_key_empty, std::memory_order_relaxed);
            }
            std::array<int, k_max_sources> kept_sets{};
            std::array<int, k_max_sources> waiting_sets{};
            for (int i = 0; i < kept; ++i)
            {
                const WarmEntry &entry = s_warm_entries[static_cast<std::size_t>(i)];
                (void)claim_key(entry.record.key);
                ++kept_sets[static_cast<std::size_t>(entry.source)];
                waiting_sets[static_cast<std::size_t>(entry.source)] += entry.warm ? 0 : 1;
            }
            s_warm_pending_count = count_waiting_entries();
            int still_warm = 0;
            for (int i = 0; i < s_source_count; ++i)
            {
                Source &source = s_sources[static_cast<std::size_t>(i)];
                source.cold = false;
                source.drawable_since_ms = 0;
                if (source.warm_slot >= 0 && source.state == TwinState::Ready)
                {
                    const auto at = static_cast<std::size_t>(i);
                    set_fade_warm(source, source.warm && kept_sets[at] != 0 && waiting_sets[at] == 0);
                    still_warm += source.warm ? 1 : 0;
                }
            }
            (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                     "ShaderTwins: {}; dropped the {} kept set(s) for the fade of the old one and kept "
                                     "the {} built for the new one ({} waiting); {} character twin(s) stay warm, the "
                                     "others warm up again",
                                     why, dropped, kept, s_warm_pending_count, still_warm);
        }

        /**
         * @brief Marks each character twin warm for the fade once every kept set of it has been able to draw for
         *        CHARACTER_TWIN_SETTLE_MS, so the next swap set carries it for dissolving items too.
         * @details A twin with no kept set is not warm: its items have not built a set for the fade yet. The settle
         *          time covers a multi-pass hair item, whose compiled objects build their sets frames apart. A warm
         *          twin turns not warm only through a cold report (take_cold_report()), a level change
         *          (forget_warm_sets()) or a give-up (give_up_fade()), not when a new set of it waits, so the readiness
         *          does not flicker with a LOD or RT-mask change at rest. A cold hold ends when the set of the fading
         *          build that caused it arrives (drain_warm_records()), or after CHARACTER_TWIN_COLD_HOLD_MS when it
         *          never does, and the twin turns warm again once all of its sets can draw.
         */
        void refresh_fade_warm(std::uint64_t now) noexcept
        {
            struct Count
            {
                int kept = 0;
                int waiting = 0;
                int first = -1;               // the first kept set, for the masks in the log
                std::uint64_t slowest_ms = 0; // the longest a warm set waited, for the log
            };
            std::array<Count, k_max_sources> counts{};
            for (int i = 0; i < s_warm_entry_count; ++i)
            {
                const WarmEntry &entry = s_warm_entries[static_cast<std::size_t>(i)];
                Count &count = counts[static_cast<std::size_t>(entry.source)];
                ++count.kept;
                count.waiting += entry.warm ? 0 : 1;
                count.first = count.first >= 0 ? count.first : i;
                count.slowest_ms =
                    entry.warm ? std::max(count.slowest_ms, entry.warm_ms - entry.first_ms) : count.slowest_ms;
            }
            for (int i = 0; i < s_source_count; ++i)
            {
                Source &source = s_sources[static_cast<std::size_t>(i)];
                if (source.warm_slot < 0 || source.state != TwinState::Ready)
                {
                    continue;
                }
                if (source.cold && now - source.cold_ms >= Constants::CHARACTER_TWIN_COLD_HOLD_MS)
                {
                    source.cold = false;
                }
                // A warm twin stays warm while a new set of it waits (another LOD or RT mask at rest): only a fading
                // build that could not draw turns it cold (take_cold_report()), so the readiness never flickers.
                const Count &count = counts[static_cast<std::size_t>(i)];
                if (source.warm || count.kept == 0 || count.waiting != 0 || source.cold)
                {
                    source.drawable_since_ms = 0;
                    continue;
                }
                source.drawable_since_ms = source.drawable_since_ms != 0 ? source.drawable_since_ms : now;
                if (now - source.drawable_since_ms < Constants::CHARACTER_TWIN_SETTLE_MS)
                {
                    continue;
                }
                set_fade_warm(source, true);
                source.drawable_since_ms = 0;
                const WarmEntry &first = s_warm_entries[static_cast<std::size_t>(count.first)];
                (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                         "ShaderTwins: {} twin {:#x} warm for the fade for {}: {} set(s) can draw (the "
                                         "first {}, RT mask {:#x} -> {:#x}, flags {:#x} -> {:#x}; the slowest waited "
                                         "{:.2f} s) {:.2f} s after the twin was ready; its dissolving items keep it "
                                         "from the next frame",
                                         blobs::k_twins[source.kind].label, source.twin, source.name.data(), count.kept,
                                         origin_text(first.record.origin), first.record.rest_rt_mask,
                                         first.record.rt_mask, first.record.rest_flags, first.record.flags,
                                         static_cast<double>(count.slowest_ms) / 1000.0,
                                         static_cast<double>(now - source.ready_ms) / 1000.0);
            }
        }

        /** @brief Logs, once, the first fading build whose description no set was built ahead for. */
        void log_fade_miss() noexcept
        {
            if (s_fade_miss_logged || !s_fade_miss_ready.load(std::memory_order_acquire))
            {
                return;
            }
            s_fade_miss_logged = true;
            (void)DMK::log().try_log(
                DMK::LogLevel::Debug,
                "ShaderTwins: a fading build of a character twin asked for a description no set was "
                "built ahead for (RT mask {:#x}, flags {:#x}); it takes the twin only if the "
                "engine has its permutations, else it turns the twin cold",
                s_fade_miss_rt_mask.load(std::memory_order_relaxed), s_fade_miss_flags.load(std::memory_order_relaxed));
        }

        /**
         * @brief Publishes the swapped nodes, the character's render nodes, and the twins, or nothing when no node is
         *        swapped and the character's twins do not run.
         * @param character_nodes The character's render nodes (none zero, at most k_max_character_nodes), or empty.
         */
        void publish_set(std::span<const std::uintptr_t> character_nodes, std::uint64_t now) noexcept
        {
            if (s_track_count == 0 && character_nodes.empty())
            {
                trace_event(trace::Event::SetPublished, 0, 0, 0, now);
                s_set_published.store(nullptr, std::memory_order_release);
                return;
            }
            SwapSet &set = s_sets[static_cast<std::size_t>(s_next_set)];
            s_next_set = (s_next_set + 1) % static_cast<int>(s_sets.size());
            set.node_count = s_track_count;
            for (int i = 0; i < s_track_count; ++i)
            {
                const Track &track = s_tracks[static_cast<std::size_t>(i)];
                set.nodes[static_cast<std::size_t>(i)] = track.node;
                set.merged[static_cast<std::size_t>(i)] = track.merged;
                set.origins[static_cast<std::size_t>(i)] = track.origin;
                set.frames[static_cast<std::size_t>(i)] = track.frame;
            }
            set.character_count = static_cast<int>(std::min(character_nodes.size(), set.character_nodes.size()));
            std::copy_n(character_nodes.begin(), set.character_count, set.character_nodes.begin());
            set.sources_full = s_source_count >= k_max_sources;
            set.source_count = s_source_count;
            set.character_source_count = 0;
            for (int i = 0; i < s_source_count; ++i)
            {
                Source &source = s_sources[static_cast<std::size_t>(i)];
                const auto at = static_cast<std::size_t>(i);
                const bool usable = source.state == TwinState::Ready && !source.stale &&
                                    !s_kind_failed[static_cast<std::size_t>(source.kind)];
#if defined(TPVCAMERA_DEV_BUILD) && defined(DMK_ENABLE_PROFILING)
                const auto state =
                    static_cast<std::uint64_t>(source.state) |
                    (static_cast<std::uint64_t>(s_kind_failed[static_cast<std::size_t>(source.kind)]) << 8) |
                    (static_cast<std::uint64_t>(source.recovered) << 9);
                if (source.traced_state != state)
                {
                    source.traced_state = state;
                    trace_event(trace::Event::SourceState, 0, source.twin, source.shader, source.mask, state,
                                static_cast<std::uint64_t>(source.kind));
                }
#endif
                set.sources[at] = source.shader;
                set.twins[at] = usable ? source.twin : 0;
                set.kinds[at] = static_cast<std::uint8_t>(source.kind);
                set.fade_ready[at] = usable && source.warm;
                if (blobs::k_twins[source.kind].scope == blobs::TwinScope::Dissolving)
                {
                    set.character_sources[static_cast<std::size_t>(set.character_source_count++)] =
                        static_cast<std::uint8_t>(i);
                }
            }
            set.stamp_ms = now;
            s_set_published.store(&set, std::memory_order_release);
            trace_event(trace::Event::SetPublished, 0, 0, 0, now, set.node_count, set.source_count);
        }

        /**
         * @brief Whether the eyes, and everything of the character, fade with him now (eye_fade_ready(),
         *        character_fade_ready()), logged once when either changes, with the counts.
         * @details Only the character's twin sources a render job saw on him in the last CHARACTER_TWIN_RECENT_MS count
         *          (his eyes, eyelashes, beard, hair and eye film, whether he dissolves or not), and a shader seen on
         *          him that the table did not know yet counts as not warm. The eyes fade once the twins run, an Eye
         *          source on him is warm for the fade, and every such source is warm, except a Hair or IllumFade one
         *          that was given up: its items stay solid, which the head hide at the top of the fade covers, while an
         *          Eye one would leave solid eyes in the face. Everything fades once, besides, every such source is
         *          warm, none given up. A warm source turns not warm only when a fading build reports it cold
         *          (take_cold_report()), a level change finds a set of it for the new character node still waiting
         *          (forget_warm_sets()) or it is given up (give_up_fade()), and a new item's source starts not warm,
         *          so the readiness does not flicker; while none of his sources was seen (he is not drawn) it keeps
         *          its last value, and so it does while the twins can run but no character node was passed (first
         *          person), so a view toggle does not flip it. Only the character's items are stamped and warm up
         *          (twin_item(), build_resting_psos()), so other characters fading by distance never count. A stamp a
         *          render job made after this frame read its clock counts as recent.
         * @param can_run The Dissolving kinds have not all failed: the character's twins can run.
         * @param running The twins run this frame: they can, and the character's render nodes were passed.
         */
        void refresh_character_ready(bool can_run, bool running, std::uint64_t now) noexcept
        {
            const bool unknown = s_character_unknown.exchange(false, std::memory_order_relaxed);
            if (can_run && !running)
            {
                return;
            }
            // Per kind: the sources seen on the character now, how many of them are warm, and how many were given up.
            struct KindCount
            {
                int recent = 0;
                int warm = 0;
                int given_up = 0;
            };
            std::array<KindCount, k_kinds> counts{};
            int recent = 0;
            int eye_warm = 0;
            bool eyes_held = false;
            bool all_held = false;
            const Source *first_cold = nullptr;
            for (int i = 0; i < s_source_count; ++i)
            {
                const Source &source = s_sources[static_cast<std::size_t>(i)];
                if (blobs::k_twins[source.kind].scope != blobs::TwinScope::Dissolving)
                {
                    continue;
                }
                const std::uint64_t seen =
                    s_character_seen_ms[static_cast<std::size_t>(i)].load(std::memory_order_relaxed);
                if (seen == 0 || (seen < now && now - seen > Constants::CHARACTER_TWIN_RECENT_MS))
                {
                    continue;
                }
                const bool kind_failed = s_kind_failed[static_cast<std::size_t>(source.kind)];
                const bool usable = source.state == TwinState::Ready && !source.stale && source.warm && !kind_failed;
                const bool eye = source.kind == k_eye_kind;
                ++recent;
                eye_warm += (eye && usable) ? 1 : 0;
                KindCount &count = counts[static_cast<std::size_t>(source.kind)];
                ++count.recent;
                count.warm += usable ? 1 : 0;
                if (usable)
                {
                    continue;
                }
                const bool given_up = source.state == TwinState::Failed || kind_failed;
                count.given_up += given_up ? 1 : 0;
                eyes_held = eyes_held || eye || !given_up;
                all_held = true;
                first_cold = first_cold != nullptr ? first_cold : &source;
            }
            bool eyes_ready = false;
            bool all_ready = false;
            if (running && recent == 0 && !unknown)
            {
                eyes_ready = s_eye_ready.load(std::memory_order_relaxed);
                all_ready = s_character_ready.load(std::memory_order_relaxed);
            }
            else if (running)
            {
                eyes_ready = eye_warm > 0 && !eyes_held && !unknown;
                all_ready = eyes_ready && !all_held;
            }
            s_eye_ready.store(eyes_ready, std::memory_order_release);
            s_character_ready.store(all_ready, std::memory_order_release);
            const int logged = (eyes_ready ? 1 : 0) | (all_ready ? 2 : 0);
            if (logged == s_ready_logged)
            {
                return;
            }
            s_ready_logged = logged;
            if (!DMK::log().is_enabled(DMK::LogLevel::Debug))
            {
                return;
            }
            // Per kind "Hair 4/5 warm" (of the sources on him now), then the first one not warm.
            std::array<char, 320> seen_text{};
            std::size_t used = 0;
            for (int kind = 0; kind < k_kinds; ++kind)
            {
                const KindCount &count = counts[static_cast<std::size_t>(kind)];
                if (blobs::k_twins[kind].scope != blobs::TwinScope::Dissolving || used + 1 >= seen_text.size())
                {
                    continue;
                }
                const auto written =
                    std::format_to_n(seen_text.data() + used, seen_text.size() - 1 - used, "{}{} {}/{} warm{}",
                                     used == 0 ? "" : ", ", blobs::k_twins[kind].label, count.warm, count.recent,
                                     count.given_up != 0 ? " (some given up)" : "");
                used = std::min(used + static_cast<std::size_t>(written.size), seen_text.size() - 1);
            }
            if (first_cold != nullptr && used + 1 < seen_text.size())
            {
                const auto written =
                    std::format_to_n(seen_text.data() + used, seen_text.size() - 1 - used, "; first not warm: {} {}",
                                     blobs::k_twins[first_cold->kind].label, first_cold->name.data());
                used = std::min(used + static_cast<std::size_t>(written.size), seen_text.size() - 1);
            }
            seen_text[used] = '\0';
            (void)DMK::log().try_log(
                DMK::LogLevel::Debug,
                "ShaderTwins: the character's twins: the eyes {}, everything {} ({} Eye twin(s) "
                "warm for the fade; on him now: {}; {} of {} set(s) for the fade waiting{}{})",
                eyes_ready ? "fade with him" : "do not fade yet", all_ready ? "fades" : "does not fade yet", eye_warm,
                seen_text.data(), s_warm_pending_count, s_warm_entry_count,
                unknown ? "; a shader without a twin entry seen on him" : "", running ? "" : "; the twins do not run");
        }

        [[nodiscard]] float finite_or(float value, float fallback) noexcept
        {
            return std::isfinite(value) ? value : fallback;
        }

        /**
         * @brief Publishes the window and near cut rows while a tree or frame is swapped and the window is open or the
         *        near cut is on, else nothing. A window that is shut, or has no anchor, goes out with strength 0.
         */
        void publish_rows(const TreeHoleWindow &window, std::uint64_t now) noexcept
        {
            const bool anchored =
                std::isfinite(window.anchor.x) && std::isfinite(window.anchor.y) && std::isfinite(window.anchor.z);
            const float strength = anchored ? std::clamp(finite_or(window.strength, 0.0f), 0.0f, 1.0f) : 0.0f;
            const float near_strength = std::clamp(finite_or(window.near_strength, 0.0f), 0.0f, 1.0f);
            if (s_track_count == 0 || (strength <= 0.0f && near_strength <= 0.0f))
            {
                trace_event(trace::Event::RowsDisabled, 0, 0, 0, now, s_track_count == 0 ? 1 : 2);
                s_rows_published.store(nullptr, std::memory_order_release);
                return;
            }
            const float near_full = std::max(finite_or(window.near_full, 0.0f), 0.0f);
            HoleRows &rows = s_rows[static_cast<std::size_t>(s_next_rows)];
            s_next_rows = (s_next_rows + 1) % static_cast<int>(s_rows.size());
            rows.values = {
                anchored ? window.anchor.x : 0.0f,
                anchored ? window.anchor.y : 0.0f,
                anchored ? window.anchor.z : 0.0f,
                strength,
                std::max(finite_or(window.half_width, 0.0f), 0.0f),
                std::max(finite_or(window.half_height, 0.0f), 0.0f),
                std::max(finite_or(window.depth_margin, 0.0f), 0.0f),
                std::max(finite_or(window.depth_feather, 0.0f), 0.0f),
                std::clamp(finite_or(window.softness, 0.0f), 0.0f, 1.0f),
                finite_or(window.floor_z, -1.0e6f),
                std::max(finite_or(window.floor_feather, 0.0f), 0.0f),
                std::clamp(finite_or(window.tunnel, 0.0f), 0.0f, 1.0f),
                near_strength,
                near_full,
                std::max(finite_or(window.near_clear, 0.0f), near_full),
                0.0f,
            };
            rows.stamp_ms = now;
            s_rows_published.store(&rows, std::memory_order_release);
            trace_event(trace::Event::RowsPublished, 0, 0, 0, now, std::bit_cast<std::uint32_t>(strength),
                        std::bit_cast<std::uint32_t>(near_strength));
        }

        /** @brief Refreshes the stats and, once a second, logs what the hooks did when anything happened. */
        void log_second(std::uint64_t now) noexcept
        {
            int twins = 0;
            int ready = 0;
            for (int i = 0; i < s_source_count; ++i)
            {
                const Source &source = s_sources[static_cast<std::size_t>(i)];
                twins += source.twin != 0 ? 1 : 0;
                ready += source.state == TwinState::Ready ? 1 : 0;
            }
            int active = 0;
            for (int i = 0; i < s_track_count; ++i)
            {
                active += s_tracks[static_cast<std::size_t>(i)].activated ? 1 : 0;
            }
            s_stat_swapped.store(active, std::memory_order_relaxed);
            s_stat_twins.store(twins, std::memory_order_relaxed);
            s_stat_ready.store(ready, std::memory_order_relaxed);
            if (now - s_last_log_ms < 1000)
            {
                return;
            }
            s_last_log_ms = now;
            log_render_misses();
            log_pause_waits(now);
            const std::uint32_t swaps = s_item_swaps.exchange(0, std::memory_order_relaxed);
            const std::uint32_t rest_swaps = s_rest_swaps.exchange(0, std::memory_order_relaxed);
            std::array<std::uint32_t, k_kinds> kind_swaps{};
            std::uint32_t dissolve_swaps = 0;
            for (int kind = 0; kind < k_kinds; ++kind)
            {
                const auto at = static_cast<std::size_t>(kind);
                kind_swaps[at] = s_kind_swaps[at].exchange(0, std::memory_order_relaxed);
                dissolve_swaps += blobs::k_twins[kind].scope == blobs::TwinScope::Dissolving ? kind_swaps[at] : 0u;
            }
            const std::uint32_t node_rejects = s_node_rejects.exchange(0, std::memory_order_relaxed);
            const std::uint32_t opacity_skips = s_opacity_skips.exchange(0, std::memory_order_relaxed);
            const std::uint32_t aliases = s_ext_aliases.exchange(0, std::memory_order_relaxed);
            const std::uint32_t rows = s_rows_written.exchange(0, std::memory_order_relaxed);
            const std::uint32_t faults = s_guard_faults.exchange(0, std::memory_order_relaxed);
            const std::uint32_t dropped = s_dropped_requests.exchange(0, std::memory_order_relaxed);
            const std::uint32_t guarded = s_pso_guarded.exchange(0, std::memory_order_relaxed);
            const std::uint32_t prepass_only = s_pso_prepass_only.exchange(0, std::memory_order_relaxed);
            const std::uint32_t gbuffer_only = s_pso_gbuffer_only.exchange(0, std::memory_order_relaxed);
            const std::uint32_t took_twin = s_pso_took_twin.exchange(0, std::memory_order_relaxed);
            const std::uint32_t kept_stock = s_pso_kept_stock.exchange(0, std::memory_order_relaxed);
            const std::uint32_t records_dropped = s_pso_dropped.exchange(0, std::memory_order_relaxed);
            const std::uint32_t temporary = s_pso_temporary.exchange(0, std::memory_order_relaxed);
            const std::uint32_t slowest_us = s_pso_slowest_us.exchange(0, std::memory_order_relaxed);
            const std::uint32_t deferred = s_pso_deferred.exchange(0, std::memory_order_relaxed);
            const std::uint32_t carryovers = s_budget_carryovers.exchange(0, std::memory_order_relaxed);
            const std::uint32_t busy_deferrals = s_budget_busy_deferrals.exchange(0, std::memory_order_relaxed);
            const std::uint32_t promotions = std::exchange(s_promotions, 0u);
            const std::uint32_t stale = std::exchange(s_stale_requests, 0u);
            const std::uint32_t rest_builds = s_rest_builds.exchange(0, std::memory_order_relaxed);
            const std::uint32_t ahead = s_ahead_builds.exchange(0, std::memory_order_relaxed);
            const std::uint32_t ahead_known = s_ahead_known.exchange(0, std::memory_order_relaxed);
            const std::uint32_t ahead_full = s_ahead_full.exchange(0, std::memory_order_relaxed);
            const std::uint32_t warmups = s_warming_builds.exchange(0, std::memory_order_relaxed);
            const std::uint32_t warm_dropped = s_warm_records_dropped.exchange(0, std::memory_order_relaxed);
            const std::uint32_t fade_warm = s_fade_warm_builds.exchange(0, std::memory_order_relaxed);
            const std::uint32_t fade_warm_ahead = s_fade_warm_ahead.exchange(0, std::memory_order_relaxed);
            const std::uint32_t fade_cold = s_fade_cold_builds.exchange(0, std::memory_order_relaxed);
            const std::uint32_t kept_dropped = std::exchange(s_warm_entries_dropped, 0u);
            // The character's items at rest swap every frame in third person, and other characters fade by distance
            // all the time, so neither alone is news; everything else is.
            const bool character_news = rest_builds != 0 || ahead != 0 || ahead_full != 0 || warmups != 0 ||
                                        warm_dropped != 0 || fade_warm != 0 || fade_cold != 0 || kept_dropped != 0 ||
                                        s_warm_pending_count != s_last_logged_pending;
            if (swaps == rest_swaps && aliases == 0 && faults == 0 && dropped == 0 && guarded == 0 && deferred == 0 &&
                carryovers == 0 && busy_deferrals == 0 && promotions == 0 && records_dropped == 0 && stale == 0 &&
                !character_news && active == s_last_logged_swapped)
            {
                return;
            }
            if (character_news)
            {
                s_last_logged_pending = s_warm_pending_count;
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "ShaderTwins: character twins in the last second: {} build(s) at rest, {} set(s) "
                    "for the fade built ahead ({} built before, {} with the key table full), {} "
                    "built aside by a fading build of the stock shader; {} fading build(s) of a "
                    "twin took its PSOs at once ({} of them from a set built ahead), {} kept the "
                    "stock PSOs; {} set(s) not handed over, {} not kept; {} of {} kept set(s) "
                    "waiting",
                    rest_builds, ahead, ahead_known, ahead_full, warmups, fade_warm, fade_warm_ahead, fade_cold,
                    warm_dropped, kept_dropped, s_warm_pending_count, s_warm_entry_count);
            }
            if (dissolve_swaps != 0)
            {
                std::array<char, 160> per_kind{};
                std::size_t used = 0;
                for (int kind = 0; kind < k_kinds; ++kind)
                {
                    if (blobs::k_twins[kind].scope != blobs::TwinScope::Dissolving || used + 1 >= per_kind.size())
                    {
                        continue;
                    }
                    const auto written = std::format_to_n(per_kind.data() + used, per_kind.size() - 1 - used, "{}{} {}",
                                                          used == 0 ? "" : " / ", blobs::k_twins[kind].label,
                                                          kind_swaps[static_cast<std::size_t>(kind)]);
                    used = std::min(used + static_cast<std::size_t>(written.size), per_kind.size() - 1);
                }
                per_kind[used] = '\0';
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "ShaderTwins: dissolving swaps {} in the last second; {} dissolving item(s) of "
                    "another character kept the stock shader (node filter), {} opaque item(s) "
                    "kept it (opacity)",
                    per_kind.data(), node_rejects, opacity_skips);
            }
            s_last_logged_swapped = active;
            (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                     "ShaderTwins: {} tree(s) swapped, {} twin item(s) added ({} of them the "
                                     "character's while he fades, {} his at rest), {} window upload(s), {} .ext "
                                     "alias(es) in the last second; twins {}/{} ready",
                                     active, swaps, dissolve_swaps, rest_swaps, rows, aliases, ready, twins);
            if (guarded != 0 || deferred != 0 || carryovers != 0 || busy_deferrals != 0 || promotions != 0 ||
                s_pending_count != 0)
            {
                std::uint64_t oldest = now;
                for (int i = 0; i < s_pending_count; ++i)
                {
                    oldest = std::min(oldest, s_pending[static_cast<std::size_t>(i)].first_ms);
                }
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "ShaderTwins: {} twin PSO build(s) in the last second ({} of only the depth prepass, {} of "
                    "only the G-buffer passes; {} took the twin, {} kept the stock look of which {} temporary, "
                    "slowest {:.1f} ms), {} deferred past the frame's budget ({} while another build runs); {} "
                    "build(s) charged across camera updates; {} compiled object(s) waiting (oldest {:.1f} s), {} "
                    "tree refill(s) for ready twin PSOs; {} PSO reference(s) left held",
                    guarded, prepass_only, gbuffer_only, took_twin, kept_stock, temporary,
                    static_cast<double>(slowest_us) / 1000.0, deferred, busy_deferrals, carryovers, s_pending_count,
                    static_cast<double>(now - oldest) / 1000.0, promotions,
                    s_pso_leaked.load(std::memory_order_relaxed));
            }
            if (faults != 0 || dropped != 0 || records_dropped != 0 || stale != 0)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Warning,
                    "ShaderTwins: {} guarded read/write fault(s), {} dropped twin request(s), {} twin "
                    "request(s) for a shader gone before it was handled and {} dropped PSO "
                    "wait(s) in the last second (a tree that lost a wait is refilled again {:.0f} "
                    "s later unless another refill comes first)",
                    faults, dropped, stale, records_dropped,
                    static_cast<double>(Constants::TREE_HOLE_RETRY_MS) / 1000.0);
            }
        }

    } // namespace

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
        publish_set(listed_characters, now);
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
            s_loading_finalizing.load(std::memory_order_acquire) || loading_materials_admitted())
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
