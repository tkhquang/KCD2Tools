/**
 * @file hooks/twins/twin_state.hpp
 * @brief The shader twins' types, shared state and the functions one part of the module calls in another.
 *
 * @details Internal to hooks/shader_twins.cpp and hooks/twins/: no other module includes it. The design is
 *          described in shader_twins.cpp. The state lives once as inline variables, each with the threading
 *          contract its comment states. A function declared here is defined in the part file its group names; a
 *          part's own helpers stay in that file's anonymous namespace.
 */
#ifndef TPVCAMERA_HOOKS_TWINS_TWIN_STATE_HPP
#define TPVCAMERA_HOOKS_TWINS_TWIN_STATE_HPP

#include "hooks/shader_twins.hpp"
#include "hooks/twin_common.hpp"
#include "hooks/twin_inventory.hpp"
#if defined(TPVCAMERA_DEV_BUILD)
#include "dev/twin_clone/twin_clone.hpp"
#endif
#include "aob_resolver.hpp"
#include "camera_rules_files.hpp"
#include "constants.hpp"
#include "dev/hook_profile.hpp"
#include "dev/see_through_trace.hpp"
#include "hooks/dissolve.hpp"
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

namespace TPVCamera::Twins
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
    using PakOpenFn = void *(__fastcall *)(std::uintptr_t pak, const char *name, const char *mode, std::uint32_t flags);
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
    using SaveGameLoadFn = bool(__fastcall *)(std::uintptr_t manager, std::uintptr_t chunk, std::uintptr_t save_path);
    using LoadCGFMaterialFn = std::uintptr_t(__fastcall *)(std::uintptr_t manager, const char *material,
                                                           const char *asset, std::uint32_t flags);
    using LoadMaterialShaderFn = bool(__fastcall *)(std::uintptr_t material, std::uintptr_t parent, const char *shader,
                                                    std::uint64_t mask, std::uintptr_t resources,
                                                    std::uintptr_t public_params);
    using FlushRTCommandsFn = bool(__fastcall *)(std::uintptr_t renderer, bool wait, bool immediately, bool force);

    struct ResourceName
    {
        std::size_t length;
        std::size_t capacity;
        const char *data;
    };
    using ResourceLookupFn = std::uintptr_t(__fastcall *)(const std::uint32_t *, const ResourceName *, bool);

    inline constexpr int k_max_nodes = Constants::TREE_HOLE_MAX_NODES;
    inline constexpr int k_max_sources = Constants::SHADER_TWIN_MAX_SOURCES;
    // The stock shaders with a twin (blobs::k_twins), indexed by kind.
    inline constexpr int k_kinds = static_cast<int>(std::size(blobs::k_twins));
    static_assert(k_kinds <= 255, "A swap set keeps each source's kind in a byte.");
    // The character's render nodes a swap set names at most.
    inline constexpr int k_max_character_nodes = Constants::CHARACTER_TWIN_MAX_NODES;
    inline constexpr int k_row_floats = Constants::PER_VIEW_HOLE_ROW_FLOATS;
    inline constexpr int k_pso_slots = Constants::COMPILED_OBJECT_PSO_SLOTS;
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
    inline constexpr std::size_t k_max_candidates = 512;
    // Of the pool, the slots only plant and frame candidates (TwinScope::ListedPlants and ListedFrames) may take. A
    // character candidate (an Eye, Hair or IllumFade source, the player's or another character's) takes one of the
    // others or none, and never evicts a plant or frame one, so the eyes and hair of every character a loading
    // screen loads cannot push out the plants and frames a pause or the next boundary prepares.
    inline constexpr std::size_t k_reserved_listed_candidates = 256;
    static_assert(k_reserved_listed_candidates < k_max_candidates, "The character candidates need slots too.");

    /** @brief A listed source instance and the kind of its twin, as the level inventory found them. */
    struct InventoryKey
    {
        std::uintptr_t shader = 0;
        int kind = -1;
        auto operator<=>(const InventoryKey &) const = default;
    };
    inline constexpr std::uintptr_t k_scope_plants = Rules::TWIN_PLANTS;
    inline constexpr std::uintptr_t k_scope_frames = Rules::TWIN_FRAMES;
    inline constexpr std::uintptr_t k_scope_character = Rules::TWIN_CHARACTER;

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
        float hold = 0.0f;      // seconds left before an unlisted node is swapped back
        bool seen = false;      // listed by the camera this frame
        bool fresh = false;     // newly observed; activation waits for a ready twin of this scope
        bool activated = false; // this target could draw a twin and needs restoration when released
        const char *why = "";   // why it was swapped off, once it was
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
    inline std::atomic<AddRenderObjectFn> s_add_render_object_original{nullptr};
    inline std::atomic<ShaderGenLoadFn> s_shader_gen_original{nullptr};
    inline std::atomic<CreatePsosFn> s_create_psos_original{nullptr};
    inline ForwardPsosFn s_character_forward_psos = nullptr; // immutable after hook publication; optional
    inline PrepassPsosFn s_plant_prepass_psos = nullptr;     // immutable after hook publication; optional
    inline GBufferPsosFn s_plant_gbuffer_psos = nullptr;     // immutable after hook publication; optional
    inline std::atomic<SetSystemGlobalStateFn> s_set_system_state_original{nullptr};
    inline std::atomic<SaveGameLoadFn> s_save_game_load_original{nullptr};
    inline std::atomic<LoadCGFMaterialFn> s_load_cgf_material_original{nullptr};
    inline std::atomic<LoadMaterialShaderFn> s_load_material_shader_original{nullptr};
    inline std::atomic<bool> s_available{false};
    // Set for the session when the feature had to stop (the rows are in use, no twin loaded, a failed
    // preparation).
    inline std::atomic<bool> s_disabled{false};
    // Set by the render thread when a row the engine leaves zero is in use: the plant twins stop for the session
    // (the character fade's twins need no rows and go on).
    inline std::atomic<bool> s_rows_in_use{false};
    // Set by a render job that queued a twin request for a shader of the character's items the swap set did not
    // know yet; the main thread takes it each frame, and neither readiness holds that frame.
    inline std::atomic<bool> s_character_unknown{false};
    // eye_fade_ready() and character_fade_ready(), refreshed by each update.
    inline std::atomic<bool> s_eye_ready{false};
    inline std::atomic<bool> s_character_ready{false};

    // Published tables. The three of each are static, so a reader that already loaded one keeps reading it safely.
    inline std::array<SwapSet, 3> s_sets{};
    inline int s_next_set = 0;
    inline std::atomic<const SwapSet *> s_set_published{nullptr};
    inline std::array<HoleRows, 3> s_rows{};
    inline int s_next_rows = 0;
    inline std::atomic<const HoleRows *> s_rows_published{nullptr};

    inline std::mutex s_candidate_mutex;
    inline std::array<TwinCandidate, k_max_candidates> s_candidates{};
    inline std::size_t s_candidate_count = 0;
    // The pairs a render job or a loader thread evicted from the pool, for the main thread to release
    // (drop_evicted()). Nothing but the pool may still hold such a pair (its material can be gone), so its Release
    // can be the last one, which destroys the shader or the resources: it runs on the engine main thread only. A
    // full queue refuses the eviction instead (collect_candidate()).
    inline RequestQueue<TwinRequest, 64> s_evicted;
    inline std::atomic<bool> s_level_loading{false};
    inline std::atomic<std::uintptr_t> s_loading_save_manager{0};
    inline std::atomic<bool> s_loading_finalizing{false};
    inline std::atomic<std::uint64_t> s_load_epoch{0};
    inline DWORD s_asset_scope_tls = TLS_OUT_OF_INDEXES;
    inline ResourceLookupFn s_resource_lookup = nullptr;
    inline PSRWLOCK s_resource_lock = nullptr;
    inline std::uintptr_t s_shader_container_global = 0;
    inline bool s_recovery_done = false;
    // The level inventory (hooks/twin_inventory.hpp): the global that holds CObjManager, or 0 when the
    // TwinInventory gate failed and the twins come from captured material loads only. Resolved at init, read-only
    // afterward.
    inline std::uintptr_t s_inventory_objman_global = 0;
    // Set by a render job that retained a render miss of a listed node, so the main thread checks it against the
    // level inventory within a second (log_render_misses()).
    inline std::atomic<bool> s_render_miss_new{false};
    // Set once the camera asks for the pause-time creation (service_shader_twins_paused()): a variant drawn without
    // its twin then waits for a pause, not only a loading screen (the trace's reason).
    inline std::atomic<bool> s_pause_service{false};
    // The loading epoch (s_load_epoch) whose level inventory was taken, by its boundary or by the first pause after
    // a DEV hot reload, whose module saw no boundary of its own; UINT64_MAX until one was.
    inline std::atomic<std::uint64_t> s_inventory_epoch{UINT64_MAX};
    // The camera rules generation (Rules::current_generation(), read before the walk) the level inventory scoped
    // its assets with. Rules edited since then take the inventory again at the next pause, so a model that is now
    // a plant or a frame gets its twins.
    inline std::atomic<std::uint64_t> s_inventory_rules{0};
    // Plant and frame variants retained from material loads during play since the last log line. Any thread adds.
    inline std::atomic<std::uint32_t> s_playtime_captures{0};
    inline RequestQueue<PsoRecord, 128> s_pso_records;
    // The render nodes of swapped trees whose compiled objects kept their stock look past the frame's twin-build
    // budget, for the main thread to refill shortly.
    inline RequestQueue<std::uintptr_t, 64> s_deferred_nodes;

    // The ready plant twins and their sources, for the CreatePipelineStates detour. Append-only on the main thread:
    // an entry is written before the count that covers it is published, and a twin never stops being ready.
    inline std::array<std::atomic<std::uintptr_t>, k_max_sources> s_guard_twins{};
    inline std::array<std::atomic<std::uintptr_t>, k_max_sources> s_guard_sources{};
    // Whether the twin's items take the hole in the depth prepass (prepass_kind()).
    inline std::array<std::atomic<bool>, k_max_sources> s_guard_prepass{};
#if defined(TPVCAMERA_DEV_BUILD) && defined(DMK_ENABLE_PROFILING)
    inline std::array<std::atomic<std::uint8_t>, k_max_sources> s_trace_guard_kinds{};
#endif
    inline std::atomic<int> s_guard_count{0};
    // The ready character twins and their sources, for the detour, the same way, with whether the twin's kind is
    // transparent-only (IllumFade); an entry's done flag is set by the main thread while the twin is warm for the
    // fade or once it was given up, and a dissolving build of its source then builds nothing aside. A render
    // thread whose check of a fading build found a permutation not compiled sets the entry's cold flag, and the
    // main thread clears done.
    inline std::array<std::atomic<std::uintptr_t>, k_max_sources> s_warm_twins{};
    inline std::array<std::atomic<std::uintptr_t>, k_max_sources> s_warm_sources{};
    inline std::array<std::atomic<bool>, k_max_sources> s_warm_transparent_only{};
    inline std::array<std::atomic<bool>, k_max_sources> s_warm_done{};
    inline std::array<std::atomic<bool>, k_max_sources> s_warm_cold{};
    inline std::atomic<int> s_warm_count{0};
    // Character twin sets handed from the render thread to the main thread, with a reference held on every PSO in
    // them.
    inline RequestQueue<PsoRecord, 128> s_warm_records;
    // When a render job last saw an item of each source (by its index in the swap set, which is the main thread's)
    // on the character, in GetTickCount64 milliseconds.
    inline std::array<std::atomic<std::uint64_t>, k_max_sources> s_character_seen_ms{};
    // The keys (desc_key()) of the character twin sets built aside so far, shared by the render threads: open
    // addressing, a slot taken with one compare-exchange from k_key_empty and never reused, k_key_removed for a
    // set that could not be handed over (so it may be built again). The main thread clears it on a level change
    // and takes the keys of the sets it keeps again (forget_warm_sets()).
    inline constexpr std::uint64_t k_key_empty = 0;
    inline constexpr std::uint64_t k_key_removed = 1;
    inline std::array<std::atomic<std::uint64_t>, Constants::CHARACTER_WARM_KEY_SLOTS> s_warm_keys{};
    static_assert((Constants::CHARACTER_WARM_KEY_SLOTS & (Constants::CHARACTER_WARM_KEY_SLOTS - 1)) == 0,
                  "The key table's size must be a power of two.");
    // The dissolve's RT-mask bit (the DissolveRtBit anchor's global), read on the main thread when the twins are
    // prepared and before any set is built ahead; 0 when it did not resolve or did not check out, and the sets
    // built ahead then carry the dissolve in their flags only.
    inline std::atomic<std::uint64_t> s_dissolve_rt_bit{0};

    // Set by a render thread whose PSO wait did not fit the hand-over queue; it cannot say for which tree, so the
    // main thread schedules a retry for every swapped tree.
    inline std::atomic<bool> s_pso_record_lost{false};

    // The camera publishes its frame sequence. The TwinBuildPermit that set s_budget_in_flight exclusively owns
    // s_budget_frame and s_budget_used_us while a plant build is in flight; competing callers defer without
    // waiting. Completion charges the current camera frame, so an update during a slow native build cannot make
    // the next caller erase that build's cost.
    inline std::atomic<std::uint32_t> s_frame_seq{0};
    inline std::atomic_flag s_budget_in_flight{};
    inline std::uint32_t s_budget_frame = 0;
    inline std::uint32_t s_budget_used_us = 0;
    // Builds charged to a later camera frame than the one they started in, and builds deferred because another
    // build held the permit, since the last log line.
    inline std::atomic<std::uint32_t> s_budget_carryovers{0};
    inline std::atomic<std::uint32_t> s_budget_busy_deferrals{0};

    /** @brief Records a see-through trace event stamped with the camera frame (DEV profiling builds only). */
    inline void trace_event(trace::Event event, std::uintptr_t node = 0, std::uintptr_t object = 0,
                            std::uintptr_t shader = 0, std::uintptr_t resources = 0, std::uint64_t a = 0,
                            std::uint64_t b = 0) noexcept
    {
        if constexpr (trace::enabled)
            trace::emit(event, s_frame_seq.load(std::memory_order_relaxed), node, object, shader, resources, a, b);
    }

    // Counters for the once-a-second log line. Render threads add, the main thread takes.
    inline std::atomic<std::uint32_t> s_item_swaps{0};
    // Of s_item_swaps, the character's items at rest (not dissolving), and the others by kind; the dissolving
    // items of another character (the engine's LOD dissolve) that kept the stock shader; and the character's
    // dissolving items of a transparent-only kind that kept it, being opaque.
    inline std::atomic<std::uint32_t> s_rest_swaps{0};
    inline std::array<std::atomic<std::uint32_t>, k_kinds> s_kind_swaps{};
    inline std::atomic<std::uint32_t> s_node_rejects{0};
    inline std::atomic<std::uint32_t> s_opacity_skips{0};
    inline std::atomic<std::uint32_t> s_ext_aliases{0};
    inline std::atomic<std::uint32_t> s_rows_written{0};
    inline std::atomic<std::uint32_t> s_guard_faults{0};
    inline std::atomic<std::uint32_t> s_dropped_requests{0};
    // The CreatePipelineStates detour's: compiled objects that named a twin, those that took the twin's PSOs and
    // those that kept the stock's, of which those of a temporary render object (nothing waits on them), records
    // that could not be handed over or held, and the slowest twin build (microseconds).
    inline std::atomic<std::uint32_t> s_pso_guarded{0};
    inline std::atomic<std::uint32_t> s_pso_took_twin{0};
    inline std::atomic<std::uint32_t> s_pso_kept_stock{0};
    inline std::atomic<std::uint32_t> s_pso_temporary{0};
    inline std::atomic<std::uint32_t> s_pso_dropped{0};
    inline std::atomic<std::uint32_t> s_pso_slowest_us{0};
    // Of the guarded builds, those that built only the twin's depth prepass and those that built only its G-buffer
    // passes (build_plant_twin()); the rest built the twin's whole set.
    inline std::atomic<std::uint32_t> s_pso_prepass_only{0};
    inline std::atomic<std::uint32_t> s_pso_gbuffer_only{0};
    // Compiled objects that kept their stock look because the frame's twin-build budget was spent.
    inline std::atomic<std::uint32_t> s_pso_deferred{0};
    // The character twins' builds: those of an item that does not dissolve (it keeps the stock PSOs), the sets
    // they built ahead for the fade, the ones they did not build because the set was built before or the key
    // table was full; the dissolving builds of the stock shader that built the twin's set aside; the sets that
    // could not be handed over; and the fading builds of the twin, those that took its PSOs at once (and of those,
    // the ones whose set had been built ahead) and those that kept the stock PSOs (the twin turned cold).
    inline std::atomic<std::uint32_t> s_rest_builds{0};
    inline std::atomic<std::uint32_t> s_ahead_builds{0};
    inline std::atomic<std::uint32_t> s_ahead_known{0};
    inline std::atomic<std::uint32_t> s_ahead_full{0};
    inline std::atomic<std::uint32_t> s_warming_builds{0};
    inline std::atomic<std::uint32_t> s_warm_records_dropped{0};
    inline std::atomic<std::uint32_t> s_fade_warm_builds{0};
    inline std::atomic<std::uint32_t> s_fade_warm_ahead{0};
    inline std::atomic<std::uint32_t> s_fade_cold_builds{0};
    // The first fading build of the twin whose description no set built ahead had (its RT mask and flags), for one
    // log line: a render thread takes the claim, writes the masks, then sets ready; the main thread logs it once.
    inline std::atomic<bool> s_fade_miss_taken{false};
    inline std::atomic<std::uint64_t> s_fade_miss_rt_mask{0};
    inline std::atomic<std::uint64_t> s_fade_miss_flags{0};
    inline std::atomic<bool> s_fade_miss_ready{false};
    // PSO references the mod left held rather than drop the last one off the engine's own release path. Any
    // thread adds; read for the logs.
    inline std::atomic<std::uint32_t> s_pso_leaked{0};

    // Stats for shader_twin_stats(), refreshed at the end of each update.
    inline std::atomic<int> s_stat_swapped{0};
    inline std::atomic<int> s_stat_twins{0};
    inline std::atomic<int> s_stat_ready{0};

    // Resolved at init, read-only afterward.
    inline std::uintptr_t s_genv = 0;
    inline std::uintptr_t s_for_name = 0;
    inline std::uintptr_t s_adjust_file_name = 0;
    inline std::uintptr_t s_flush_rt_commands = 0;
    inline std::ptrdiff_t s_pak_open_slot = 0;
    inline std::ptrdiff_t s_pak_read_slot = 0;
    inline std::ptrdiff_t s_pak_close_slot = 0;
    // The DissolveRtBit global, or 0 when its gate failed.
    inline std::uintptr_t s_dissolve_rt_global = 0;

    // Main-thread state, guarded by s_main_mutex so shutdown_shader_twins() can run on the teardown thread while
    // the camera frame may still call update_shader_twins().
    inline std::mutex s_main_mutex;
    inline Prepare s_prepare = Prepare::Pending;
    inline std::uint64_t s_prepare_retry_ms = 0;
    inline std::uintptr_t s_shader_manager = 0;
    inline std::array<Source, k_max_sources> s_sources{};
    inline int s_source_count = 0;
#if defined(TPVCAMERA_DEV_BUILD)
    // Goes up each time a twin becomes ready (refresh_twins()) or checks out again after going stale
    // (revalidate_twins()), so the DEV clone dry-run lists its pairs again after any such change (dry_run_pairs()).
    inline int s_usable_generation = 0;
#endif
    inline bool s_sources_full_logged = false;
    // The listed pairs the last level inventory found, sorted: a render miss of a pair outside them is logged
    // (its material is scoped otherwise than its node, or the level tables do not hold it). Identities only,
    // never read through; empty until an inventory found a level.
    inline std::vector<InventoryKey> s_inventory_pairs;
    // Twin requests whose shader or resources were gone by the time they were handled, since the last log line.
    inline std::uint32_t s_stale_requests = 0;
    // A kind whose stock shader a game patch changed, or whose twin binary the engine refused (EF_NOTFOUND): no
    // twins of that kind for the session, and the other kinds go on. The feature turns off once every kind failed.
    // The plant kinds also fail together when the window's rows turn out to be in use, or when the Z pass
    // includes their copies were spliced from changed.
    inline std::array<bool, k_kinds> s_kind_failed{};
    // The character's own render node the twins ran for last frame (the first of update_shader_twins's
    // character_nodes), the last one that held for CHARACTER_ROOT_SETTLE_FRAMES (another one means a new level or
    // save, see forget_warm_sets()), and the readiness last logged (bit 0 eye_fade_ready(), bit 1
    // character_fade_ready()).
    inline std::uintptr_t s_character_root = 0;
    inline std::uintptr_t s_last_character_root = 0;
    // Another node seen in place of s_last_character_root, and for how many frames in a row.
    inline std::uintptr_t s_root_candidate = 0;
    inline int s_root_frames = 0;
    inline int s_ready_logged = 0;
    // Whether the first fading build whose description no set built ahead had was logged.
    inline bool s_fade_miss_logged = false;
    inline std::array<Track, k_max_nodes> s_tracks{};
    inline int s_track_count = 0;
    // The frame's refills, run after the swap set is published, so a render object refilled right after its node
    // was invalidated already sees the set that adds or drops the node: the nodes swapped off this frame (evicted
    // or unlisted past their hold), and whether a twin became ready, which refills every swapped node.
    inline std::array<Track, 2 * k_max_nodes> s_released{};
    inline int s_released_count = 0;
    inline bool s_refill_all = false;
    // The compiled objects of swapped trees waiting for the twin's PSOs, and the refills that ended a wait.
    inline std::array<PendingPsos, Constants::TREE_HOLE_MAX_PENDING> s_pending{};
    inline int s_pending_count = 0;
    // The character twins' sets for the fade the main thread keeps (see WarmEntry), those of them still waiting,
    // and the sets dropped since the last log line because the table was full of waiting ones.
    inline std::array<WarmEntry, Constants::CHARACTER_WARM_MAX_ENTRIES> s_warm_entries{};
    inline int s_warm_entry_count = 0;
    inline int s_warm_pending_count = 0;
    inline std::uint32_t s_warm_entries_dropped = 0;
    // The waiting count last logged, so the once-a-second line only comes when something changed.
    inline int s_last_logged_pending = 0;
    inline std::uint32_t s_promotions = 0;
    inline std::uint64_t s_last_log_ms = 0;
    inline int s_last_logged_swapped = 0;
    // The level inventory the first pause after a DEV hot reload took (pause_inventory()), or the pairs of a
    // boundary's inventory whose stock shader still parsed there (finalize_loading()): its owned candidates still
    // to create, outside the 512-entry pool so none is dropped for room, and the ones already handled, kept owned
    // until the coverage line read once the last is handled has named them. The next boundary drops both. The
    // handled list always has room for every entry still to create, so moving one along never allocates.
    inline std::vector<TwinCandidate> s_pause_inventory;
    inline std::vector<TwinCandidate> s_pause_inventory_done;
    // Set only around the create_twin() call of a frame the game stands still in (create_during_pause()): its
    // permit outside a loading boundary.
    inline bool s_pause_permit = false;

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
    inline PauseState s_pause;
    // The twins created during pauses and the pauses that ended within SHADER_TWIN_PAUSE_SPACING_MS of a creation,
    // this session, and what the once-a-second wait line last reported (log_pause_waits()). Main thread only.
    inline std::uint32_t s_pause_created_total = 0;
    inline std::uint32_t s_pause_overlaps = 0;
    inline int s_logged_waiting = 0;
    inline int s_logged_inventory_left = 0;
    inline std::uint32_t s_logged_pause_created = 0;
    inline std::uint32_t s_logged_overlaps = 0;

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

    /** @brief The outcome of comparing a stock and a twin PSO set. */
    enum class PsoReady : std::uint8_t
    {
        Ready,
        Waiting,
        Fault,
    };

    /** @brief The outcome of claiming a key in s_warm_keys. */
    enum class Claim : std::uint8_t
    {
        New,   // the caller took the key and builds the set
        Known, // a set of that key was built before
        Full,  // no slot within reach: nothing is built
    };

    // hooks/twins/render_swap.cpp

    /**
     * @brief The kind (index in blobs::k_twins) of @p scope whose stock shader @p shader is an instance of, by the
     *        shader's name (IShader::GetName, the string at +0x88), or -1 for any other shader.
     * @details One source name has one kind per scope at most (kinds_unique()); an Illum instance is the Illum kind
     *          on a listed plant and the IllumFade kind on the character.
     * @note Render jobs, for a shader on a swapped tree or a dissolving character that has no twin entry yet, and
     *       the main thread. SEH-guarded, POD body.
     */
    [[nodiscard]] int source_kind_guarded(std::uintptr_t shader, blobs::TwinScope scope) noexcept;

    /**
     * @brief Gives @p twin the feature bits (Constants::SHADER_FEATURE_FLAGS_OFFSET) its source @p source has and
     *        it lacks, the ones the parse sets from a shader's name; @p before and @p after get the twin's word.
     * @note Main thread, on a twin whose parse finished and that no item names yet. SEH-guarded, POD body.
     */
    [[nodiscard]] bool match_feature_flags_guarded(std::uintptr_t twin, std::uintptr_t source, std::uint16_t &before,
                                                   std::uint16_t &after) noexcept;

    /** @brief Reads @p shader's ShaderState. Main thread. SEH-guarded, POD body. */
    [[nodiscard]] bool read_shader_state_guarded(std::uintptr_t shader, ShaderState &out) noexcept;

    /**
     * @brief The opacity of an item's material, @p resources (its IRenderShaderResources), as the engine's render
     *        list select reads it: GetStrengthValue(EFTT_OPACITY) through the CShaderResources vtable (see
     *        Constants::SHADER_RESOURCES_VTABLE_GET_STRENGTH_OFFSET).
     * @return The value, or 1 (opaque, so nothing swaps) for no resources, another type, or a fault.
     * @note Render jobs, on the item AddRenderObject was handed; the call is a read with no side effects, the one
     *       the engine makes on the same item right after. SEH-guarded, POD body.
     */
    [[nodiscard]] float item_opacity_guarded(std::uintptr_t resources) noexcept;

    /**
     * @brief True when @p item may swap to a twin of @p kind: any item, or for a transparent-only kind (IllumFade)
     *        one whose opacity is below 1, the engine's own test for the transparent list (a NaN reads opaque).
     */
    [[nodiscard]] bool item_fits_kind(int kind, const ShaderItem &item) noexcept;

    /**
     * @brief Reads a merged grass patch's origin (m_pos) into @p out when @p node is one (EERType 0x17).
     * @return False for any other node, or on a fault. Main thread. SEH-guarded, POD body.
     */
    [[nodiscard]] bool merged_origin_guarded(std::uintptr_t node, float *out) noexcept;

    /**
     * @brief The merged grass patch of @p set whose origin is the translation of @p object's world matrix, or 0.
     * @details A patch's render objects leave m_pRenderNode null, and their world matrix is the patch origin
     *          (Constants::RENDER_OBJECT_MATRIX_OFFSET), so the origin is how a render job tells which patch drew.
     * @note Render jobs, on a render object valid for the call.
     */
    [[nodiscard]] std::uintptr_t merged_node_of_object(const SwapSet &set, const std::uint8_t *object) noexcept;

    /**
     * @brief Copies the C string whose pointer is stored at @p object + @p offset into @p out, printable only.
     * @note SEH-guarded, POD body. @p out is empty on a fault.
     */
    void copy_text_field_guarded(std::uintptr_t object, std::ptrdiff_t offset, char *out, std::size_t cap) noexcept;

    /**
     * @brief Invalidates @p node's persistent render objects the way AddRenderObject does for an item that is not
     *        ready (0x1804A510E): the temp-data stamp at +0xF8 goes to +0xFC atomically and the node's stamp at
     *        +0x38 becomes -1, so the next frame refills them through AddRenderObject.
     * @details Nothing is written unless the temp data's owner link names @p node, so a node that was freed (a
     *          level change while it was held) or whose memory now holds another object is left alone.
     * @note Main thread. SEH-guarded, POD body.
     */
    [[nodiscard]] Invalidate invalidate_node_guarded(std::uintptr_t node) noexcept;

    /**
     * @brief The same proof as invalidate_node_guarded() without the write: Done for a live node with render data.
     * @note Main thread. SEH-guarded, POD body.
     */
    [[nodiscard]] Invalidate node_state_guarded(std::uintptr_t node) noexcept;

    /**
     * @brief Per-view upload mid hook: writes the window rows into the main view's buffer right before it uploads.
     * @details At the call, by its ABI, rcx is the buffer, rdx the data and the view count is at [rsp+0x20], and
     *          the function still holds its pipeline in r13 (keeps_pipeline_in_r13()). Every other upload (another
     *          pipeline or buffer, or a call while the hole is off) is left alone, so the rows stay the memset
     *          zeros and the twin's early-out skips the hole.
     */
    void per_view_upload_detour(DMK::hook::MidContext &ctx) noexcept;

    /**
     * @brief True when @p object is a temporary render object drawn with a dissolve (FOB_DISSOLVE in its flags):
     *        a character the close-up fade (or the engine's own LOD dissolve) is fading.
     * @details A persistent object keeps its items until its node refills, so a swap there would outlast the
     *          dissolve; the skin attachments that draw a character's head are temporary.
     */
    [[nodiscard]] bool dissolving_object(const std::uint8_t *object) noexcept;

    /** @brief True when @p object names one of the character's render nodes in @p set. */
    [[nodiscard]] bool character_object(const SwapSet &set, const std::uint8_t *object) noexcept;

    /**
     * @brief CRenderView::AddRenderObject detour: a main-view item of a swapped tree, or an Eye, Hair or
     *        transparent Illum item of the player character in third person, draws with its twin shader.
     * @details The twin goes in a stack copy of the item (same resources, technique and flags). The engine keeps
     *          only a packed key of it in a persistent object (by the twin's id), and only its shader and resources
     *          pointers in a pooled compiled object (sub_1804A6104), and the twin is never released, so the copy
     *          does not outlive anything it names.
     * @note Raw arbitrary-signature detour (DMK [B-84]): it must not throw, and every call in it is noexcept.
     */
    void __fastcall add_render_object_detour(void *view, void *element, const ShaderItem *item, std::uint8_t *object,
                                             const std::uint8_t *pass, int list, int after_water) noexcept;

    // hooks/twins/pso_guard.cpp

    /** @brief Drops every reference @p set holds, counting the ones left held. Any thread. */
    void drop_pso_refs(const PsoSet &set) noexcept;

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
                                                 std::uint32_t slots) noexcept;

#if defined(TPVCAMERA_DEV_BUILD) && defined(DMK_ENABLE_PROFILING)
    // Only held PSOs are read. Fields can change between reads; diagnostics never drive readiness policy.
    struct PsoObservation
    {
        std::uint32_t state = 0;
        bool valid = false;
        bool fault = false;
    };

    [[nodiscard]] PsoObservation observe_pso_guarded(std::uintptr_t pso) noexcept;

    // Initial/validity/creation-request changes are immediate; attempt-only progress is limited to 10 Hz.
    // One event pairs both PSOs of a slot. The recorder stores identities, not borrowed objects to read later.
    void trace_pending_psos(PendingPsos &pending, std::uint64_t now) noexcept;
#endif

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
                                                  bool &stock_idle) noexcept;

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
                                            std::uint32_t slots, std::uint32_t *attempts) noexcept;

    /**
     * @brief Takes @p key in s_warm_keys unless it is there already.
     * @note Render threads and compile jobs; lock-free, at most Constants::CHARACTER_WARM_KEY_PROBES slots.
     */
    [[nodiscard]] Claim claim_key(std::uint64_t key) noexcept;

    /** @brief Marks @p key removed, so its set may be built again (it could not be handed over or kept). */
    void release_key(std::uint64_t key) noexcept;

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
    std::uint8_t __fastcall create_psos_detour(void *pipeline, std::uintptr_t *psos, std::uint8_t *desc, void *cache,
                                               int is_shadow) noexcept;

    // hooks/twins/creation.cpp

    /**
     * @brief .ext loader detour: a twin's name loads its stock shader's gen flags.
     * @details The loader formats "%s%s.ext" from the name and caches the result by name, so passing the stock name
     *          ("Vegetation", "Illum", "Eye", "Hair") returns the SShaderGen every instance of it already uses, and
     *          the twin's gen-mask bits mean the same thing as its source's. mfForName passes the name it was
     *          given, the twin's full name; a longer name that starts with it is taken too.
     * @note Raw arbitrary-signature detour (DMK [B-84]): it must not throw, and every call in it is noexcept.
     */
    void *__fastcall shader_gen_load_detour(void *manager, void *out, const char *name) noexcept;

    /** @brief Drops one reference to a CShader (IShader::Release, vtable slot 3); nothing for another type. */
    void release_candidate_shader(std::uintptr_t shader) noexcept;

    /**
     * @brief Drops the pairs a render job or a loader thread evicted from the pool (s_evicted).
     * @note The queue's one consumer: the engine main thread outside every mod mutex, or the teardown thread once
     *       no detour runs.
     */
    void drop_evicted() noexcept;

    [[nodiscard]] bool loading_materials_admitted() noexcept;

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
    bool collect_candidate(const ShaderItem &item, int kind, std::uint8_t priority, bool loading_only) noexcept;

    void clear_candidates() noexcept;

    /** @brief The track index of @p node, or -1. */
    [[nodiscard]] int find_track(std::uintptr_t node) noexcept;

    [[nodiscard]] const char *invalidate_text(Invalidate result) noexcept;

    /** @brief Drops a held record's references. */
    void drop_record(const PsoRecord &record) noexcept;

    /**
     * @brief Invalidates the track's node, so its render objects refill with (or without) the twin. The rebuild
     *        hands over new waits for every compiled object of it, so a retry it was due is done.
     */
    Invalidate refill(Track &track, trace::RefillReason reason) noexcept;

    /** @brief Swaps the track's node back: unpublished by the caller, then refilled with the stock shader. */
    void release_track(Track &track, std::uint64_t now, const char *why) noexcept;

    /** @brief Swaps every node back and empties the set. */
    void release_all(std::uint64_t now, const char *why) noexcept;

    /** @brief True while a kind of @p scope has not failed. */
    [[nodiscard]] bool scope_usable(blobs::TwinScope scope) noexcept;

    /** @brief True when this scope has a parsed twin that render items can actually select. */
    [[nodiscard]] bool scope_ready(blobs::TwinScope scope) noexcept;

    /**
     * @brief Stops the plant and frame twins for the session (the window's rows are in use): every tree and frame
     *        swaps back, their kinds fail, and the character fade's twins go on; the whole feature turns off when
     *        no kind is left.
     */
    void stop_plants(std::uint64_t now, const char *why) noexcept;

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
    void refresh_twins(std::uint64_t now) noexcept;

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
    void revalidate_twins(std::uint64_t now) noexcept;

    /** @brief Rebuild the DLL's mapping once, without parsing shaders or enumerating mutable materials. */
    void recover_cached_twins(std::uint64_t now) noexcept;

    /**
     * @brief Within a second of a render job retaining a render miss, logs each one of a listed node outside the
     *        last level inventory (classify_render_miss()), once per candidate, at most eight a second.
     * @note Main thread, under s_main_mutex, from log_second(). Never waits for the candidate mutex: a busy one
     *       leaves the check for the next second.
     */
    void log_render_misses() noexcept;

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
    [[nodiscard]] PauseReason pause_reason(const TwinPauseSignals &signals) noexcept;

    /**
     * @brief Closes the pause the frames before this one stood still in: logs what it created, and counts it in
     *        s_pause_overlaps when it ended within SHADER_TWIN_PAUSE_SPACING_MS of a creation's barrier.
     * @note Main thread, the camera frame; no lock.
     */
    void end_pause(std::uint64_t now) noexcept;

    /**
     * @brief Drops the pause inventory's candidates, both lists, outside every mod mutex.
     * @note Main thread or the teardown thread.
     */
    void clear_pause_inventory() noexcept;

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
    void create_during_pause(std::uint64_t now, PauseReason reason) noexcept;

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
    void service_paused_dry_run(PauseReason reason) noexcept;
#endif

    /**
     * @brief Once a second, when anything changed: the variants that wait for a pause or a loading screen (those a
     *        render job drew without their twin, those the pause inventory holds, the oldest wait), the plant and
     *        frame variants materials loaded during play, and the twins created during pauses so far.
     * @note Main thread, under s_main_mutex, from log_second(). Never waits for the candidate mutex: a busy one
     *       leaves the line for the next second.
     */
    void log_pause_waits(std::uint64_t now) noexcept;

    /**
     * @brief LoadCGFMaterial detour: carries the asset path's twin scope bits to the LoadMaterialShader calls
     *        nested inside it, in a Win32 TLS slot (DMK [B-86]: no thread_local on a host thread).
     * @details The slot ORs the outer loads' bits in and restores them on return, so nested loads keep every
     *          enclosing asset's scopes.
     * @note Raw arbitrary-signature detour (DMK [B-84]): it must not throw, and every call in it is noexcept.
     */
    std::uintptr_t __fastcall load_cgf_material_detour(std::uintptr_t manager, const char *material, const char *asset,
                                                       std::uint32_t flags) noexcept;

    /**
     * @brief LoadMaterialShader detour: retains the shader item a material loaded as a candidate of every twin
     *        kind its asset scopes give it (collect_candidate()).
     * @note Raw arbitrary-signature detour (DMK [B-84]): it must not throw, so its work after the original call
     *       runs inside a local try/catch.
     */
    bool __fastcall load_material_shader_detour(std::uintptr_t material, std::uintptr_t parent, const char *shader,
                                                std::uint64_t mask, std::uintptr_t resources,
                                                std::uintptr_t public_params) noexcept;

    /**
     * @brief SetSystemGlobalState detour: opens material admission when a level load starts (states 1 to 9) and
     *        runs finalize_loading() when it ends (into state 10 or later), on the verified main thread only.
     * @note Raw arbitrary-signature detour (DMK [B-84]): it must not throw, and every call in it is noexcept.
     */
    void __fastcall set_system_state_detour(std::uintptr_t system, std::int32_t state) noexcept;

    /**
     * @brief SaveGameManager::LoadCryEngineData detour: admits the material loads of an engine save load and runs
     *        finalize_loading() once it returns, on the verified main thread only.
     * @note Raw arbitrary-signature detour (DMK [B-84]): it must not throw, and every call in it is noexcept.
     */
    bool __fastcall save_game_load_detour(std::uintptr_t manager, std::uintptr_t chunk,
                                          std::uintptr_t save_path) noexcept;

    /** @brief Queues @p track's swap-off for apply_refills(). */
    void queue_release(const Track &track, const char *why) noexcept;

    // hooks/twins/tracks.cpp

    /**
     * @brief Adds the newly listed nodes (swapped on), holds the unlisted ones, and drops those whose hold ran out.
     * @details Two passes, so a node listed this frame is never the one evicted: first mark the listed nodes that
     *          already have a track, then give the new ones a slot. With the table full a new node takes the slot
     *          of the unlisted track with the least hold left; at most TREE_HOLE_MAX_NODES nodes are taken, the
     *          frames first, so such a track exists. A node whose temp data does not name it back is not live and
     *          is skipped. The refills themselves wait for apply_refills().
     */
    void update_tracks(std::span<void *const> trees, std::span<void *const> frames, float delta_time,
                       std::uint64_t now) noexcept;

    /**
     * @brief Runs the frame's refills after the swap set is published: the nodes swapped on, the nodes swapped
     *        off, and every swapped node when a twin became ready.
     */
    void apply_refills(std::uint64_t now) noexcept;

    /**
     * @brief Takes the records the render threads handed over, keeping the newest per compiled object of a swapped
     *        tree (with the time its wait began).
     * @details A record that does not fit the list schedules a retry of its tree, and so does, for every swapped
     *          tree, one a render thread could not hand over (it cannot say for which tree).
     */
    void drain_pso_records(std::uint64_t now) noexcept;

    /**
     * @brief Refills each swapped tree whose compiled objects can now take the twin's PSOs.
     * @details A tree is refilled once every compiled object it waits on has a ready twin set, or once one has and
     *          the oldest wait passed SHADER_TWIN_PSO_WAIT_LOG_SECONDS (a twin permutation that failed to compile
     *          never becomes valid, and its object keeps the stock look). The refill drops the tree's records, and
     *          the rebuild finds the PSOs in the engine's caches and keeps the twin's, or hands over new records.
     *          A tree still waiting past that time is logged once with the twin and the RT mask it waits for. A
     *          tree that holds no record but lost one is refilled again TREE_HOLE_RETRY_MS after it lost it.
     */
    void poll_pso_records(std::uint64_t now) noexcept;

    /** @brief take_cold_report() for every source. */
    void take_cold_reports(std::uint64_t now) noexcept;

    /**
     * @brief Takes the character twin sets the render threads handed over and keeps one per description key.
     * @details A set built ahead or aside whose key is already kept is a duplicate (the key table was cleared
     *          while it was built) and goes. A set from a fading build that kept the stock PSOs replaces the kept
     *          one of its key, which waits again if it was warm, and ends its twin's cold hold, since that set now
     *          holds the twin back. With the table full, the set that turned warm first makes room; with every set
     *          still waiting, the new one goes and its key is freed, so a later build makes it again.
     */
    void drain_warm_records(std::uint64_t now) noexcept;

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
    void poll_warm_entries(std::uint64_t now) noexcept;

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
    void forget_warm_sets(std::uintptr_t root, const char *why) noexcept;

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
    void refresh_fade_warm(std::uint64_t now) noexcept;

    /** @brief Logs, once, the first fading build whose description no set was built ahead for. */
    void log_fade_miss() noexcept;

    /**
     * @brief Publishes the swapped nodes, the character's render nodes, and the twins, or nothing when no node is
     *        swapped and the character's twins do not run.
     * @param character_nodes The character's render nodes (none zero, at most k_max_character_nodes), or empty.
     */
    void publish_set(std::span<const std::uintptr_t> character_nodes, std::uint64_t now) noexcept;

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
    void refresh_character_ready(bool can_run, bool running, std::uint64_t now) noexcept;

    /**
     * @brief Publishes the window and near cut rows while a tree or frame is swapped and the window is open or the
     *        near cut is on, else nothing. A window that is shut, or has no anchor, goes out with strength 0.
     */
    void publish_rows(const TreeHoleWindow &window, std::uint64_t now) noexcept;

    /** @brief Refreshes the stats and, once a second, logs what the hooks did when anything happened. */
    void log_second(std::uint64_t now) noexcept;
} // namespace TPVCamera::Twins

#endif // TPVCAMERA_HOOKS_TWINS_TWIN_STATE_HPP
