/**
 * @file hooks/shader_twins.cpp
 * @brief Implementation of the shader twins (see shader_twins.hpp).
 *
 * @details Three kinds of threads meet here. The camera frame on the main thread owns the twin shaders and publishes
 *          the swap set (the character's render nodes and each source instance's twin) through a triple buffer and an
 *          atomic pointer. A source is an instance of a stock shader the generated header lists in k_twins (Eye, Hair,
 *          and IllumFade from the Illum shader), and its twin an instance of that kind's twin. The render jobs run
 *          CRenderView::AddRenderObject and only read the swap set: a main-view item of a temporary render object
 *          that names one of the character's render nodes (his own render proxy and his carried items') draws with
 *          its twin, for as long as the close-up fade can run (third person). The IllumFade twin is taken only by an
 *          item whose opacity is below 1 (the engine's own test for the transparent list), the eye film, and every
 *          opaque Illum item keeps the stock shader. An item whose shader is a source without a twin yet is handed back
 *          to the main thread through a lock-free bounded queue, because a shader can only be created there. The
 *          render thread and the compile jobs build the PSOs (CreatePipelineStates) and hand the sets they build aside,
 *          with a reference held on each PSO, to the main thread the same way. A table is reused two frames after it
 *          was replaced, and every reader finishes inside its frame, so no reader sees one being rewritten.
 *
 *          The twin's pixel shaders read the dissolve without the engine's RT gate, so at no dissolve they draw exactly
 *          like the stock ones, and the items name the twin whether or not he dissolves: that is what lets the fade's
 *          shaders compile before the first close-up. Those items come from a skin attachment's pooled compiled
 *          objects, which keep the PSOs they were built with for as long as the item and its flags stay the same (see
 *          Constants). A build of one that does not dissolve keeps the stock PSOs (they look the same) and builds aside
 *          the twin's and the stock's sets as the close-up fade will ask for them, with the dissolve in the
 *          description's flags and RT mask: the engine compiles the twin's dissolving permutations in the background,
 *          and the main thread holds each such set until it can draw. It only reads the PSOs' valid bytes and state
 *          words and drops the references it holds, never the last one, so it calls no engine code. A table the render
 *          threads share keys every set built aside by its description, so one is never built twice. A twin is warm
 *          once every set of it can draw; only then does a dissolving item keep the twin (its pooled object rebuilds
 *          when the dissolve starts and finds the twin's PSOs compiled), while until then it takes the stock shader for
 *          the fade and its stock build also builds the twin's set aside. A dissolving build of an item that names the
 *          twin builds both sets and takes the twin's only when it can draw every pass the stock's draws: one that asks
 *          for a permutation not compiled yet (another RT mask, a quality change) keeps the stock PSOs and turns the
 *          twin cold, so its items go back to the stock shader for the fade, rebuild, and warm it up again. A level
 *          change drops the sets of the old character node and keeps the ones built for the new one, and a twin with a
 *          set that can never draw (its permutation failed to compile) is given up. A render job also stamps each
 *          source it sees on him, so eye_fade_ready() and character_fade_ready() know which sources he uses now.
 */

#include "shader_twins.hpp"
#include "aob_resolver.hpp"
#include "constants.hpp"
#include "dissolve.hpp"
#include "generated/shader_twin_blobs.hpp"
#include "global_state.hpp"
#include "rtti_types.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
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

        constexpr int k_max_sources = Constants::SHADER_TWIN_MAX_SOURCES;
        // The stock shaders with a twin (blobs::k_twins), indexed by kind.
        constexpr int k_kinds = static_cast<int>(std::size(blobs::k_twins));
        static_assert(k_kinds <= 255, "A swap set keeps each source's kind in a byte.");
        // The character's render nodes a swap set names at most.
        constexpr int k_max_character_nodes = Constants::CHARACTER_TWIN_MAX_NODES;
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

        /** @brief One published frame of the swap: the character's render nodes, and each source with its twin. */
        struct SwapSet
        {
            std::uint64_t stamp_ms = 0;
            // The player character's render node and his carried items': the items of a temporary render object that
            // names one of them swap to a twin.
            int character_count = 0;
            std::array<std::uintptr_t, k_max_character_nodes> character_nodes{};
            // The source table is full: a shader without an entry gets none, so it is not requested again.
            bool sources_full = false;
            int source_count = 0;
            std::array<std::uintptr_t, k_max_sources> sources{};
            // The twin to draw with, or 0 while it is not ready or was given up: the item then stays on its source.
            std::array<std::uintptr_t, k_max_sources> twins{};
            // Each source's kind (blobs::k_twins), so a transparent-only twin swaps only on a transparent item.
            std::array<std::uint8_t, k_max_sources> kinds{};
            // Whether the twin's sets for the fade are warm. A dissolving item takes the twin only then; one that does
            // not dissolve takes it as soon as it is ready.
            std::array<bool, k_max_sources> fade_ready{};
        };

        /** @brief A source shader seen on the character and the kind of twin it asks for, for the main thread. */
        struct TwinRequest
        {
            std::uintptr_t shader = 0;
            std::uintptr_t resources = 0;
            int kind = -1;
        };

        /** @brief Why a twin's set went to the main thread. */
        enum class WarmOrigin : std::uint8_t
        {
            None,    // no build made the record yet
            Ahead,   // built aside by a build that does not dissolve, as the close-up fade will ask for it
            Warming, // built aside by a dissolving build of the stock shader, with that build's description
            Cold,    // a dissolving build of the twin that kept the stock PSOs (its set did not draw yet)
        };

        /**
         * @brief A twin's set for the fade, handed from the render thread to the main thread with a reference held on
         *        every PSO in it.
         */
        struct PsoRecord
        {
            // The character's own render node the published swap set named when the set was built, so a level change
            // keeps the sets built for the new one (forget_warm_sets()).
            std::uintptr_t node = 0;
            std::uintptr_t twin_shader = 0;
            std::uint64_t rt_mask = 0;
            std::uint32_t slots = 0; // bit i: slot i was built by this compile
            bool twin_built = false; // the twin build reported success
            PsoSet stock{};
            PsoSet twin{};
            // Why it came, its description's key (desc_key()), its flags, and for one built ahead the RT mask and flags
            // of the build that did not dissolve, for the log.
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
         *          drops the value: a twin request comes again with the item's next draw, and a set handed over frees
         *          its key, so a later build makes it again.
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

        // Hooks and their originals.
        std::atomic<AddRenderObjectFn> s_add_render_object_original{nullptr};
        std::atomic<ShaderGenLoadFn> s_shader_gen_original{nullptr};
        std::atomic<CreatePsosFn> s_create_psos_original{nullptr};
        std::atomic<bool> s_available{false};
        // Set for the session when the feature had to stop (no twin loaded, a failed preparation).
        std::atomic<bool> s_disabled{false};
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

        RequestQueue<TwinRequest, 64> s_requests;

        // The ready twins and their sources, for the CreatePipelineStates detour, with whether the twin's kind is
        // transparent-only (IllumFade). Append-only on the main thread: an entry is written before the count that
        // covers it is published, and a twin never stops being ready. An entry's done flag is set by the main thread
        // while the twin is warm for the fade or once it was given up, and a dissolving build of its source then builds
        // nothing aside. A render thread whose check of a fading build found a permutation not compiled sets the
        // entry's cold flag, and the main thread clears done.
        std::array<std::atomic<std::uintptr_t>, k_max_sources> s_warm_twins{};
        std::array<std::atomic<std::uintptr_t>, k_max_sources> s_warm_sources{};
        std::array<std::atomic<bool>, k_max_sources> s_warm_transparent_only{};
        std::array<std::atomic<bool>, k_max_sources> s_warm_done{};
        std::array<std::atomic<bool>, k_max_sources> s_warm_cold{};
        std::atomic<int> s_warm_count{0};
        // Twin sets for the fade handed from the render thread to the main thread, with a reference held on every PSO
        // in them.
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
        std::atomic<std::uint32_t> s_guard_faults{0};
        std::atomic<std::uint32_t> s_dropped_requests{0};
        // The twins' builds: those of an item that does not dissolve (it keeps the stock PSOs), the sets
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

        // Resolved at init, read-only afterward.
        std::uintptr_t s_genv = 0;
        std::uintptr_t s_for_name = 0;
        std::uintptr_t s_adjust_file_name = 0;
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
        bool s_sources_full_logged = false;
        // Twin requests whose shader or resources were gone by the time they were handled, since the last log line.
        std::uint32_t s_stale_requests = 0;
        // A kind whose stock shader a game patch changed, or whose twin binary the engine refused (EF_NOTFOUND): no
        // twins of that kind for the session, and the other kinds go on. The feature turns off once every kind failed.
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
        // The twins' sets for the fade the main thread keeps (see WarmEntry), those of them still waiting,
        // and the sets dropped since the last log line because the table was full of waiting ones.
        std::array<WarmEntry, Constants::CHARACTER_WARM_MAX_ENTRIES> s_warm_entries{};
        int s_warm_entry_count = 0;
        int s_warm_pending_count = 0;
        std::uint32_t s_warm_entries_dropped = 0;
        // The waiting count last logged, so the once-a-second line only comes when something changed.
        int s_last_logged_pending = 0;
        std::uint64_t s_last_log_ms = 0;

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

        /** @brief True when every source name has one kind at most, as a kind lookup needs. */
        [[nodiscard]] constexpr bool kinds_unique() noexcept
        {
            for (int a = 0; a < k_kinds; ++a)
            {
                for (int b = a + 1; b < k_kinds; ++b)
                {
                    if (equals_nocase(blobs::k_twins[a].source_name, blobs::k_twins[b].source_name))
                    {
                        return false;
                    }
                }
            }
            return true;
        }
        static_assert(kinds_unique(), "Two k_twins rows give one source name two twins.");

        // The Eye kind: whether its twins are warm decides eye_fade_ready(), where a given-up Hair or IllumFade twin
        // only holds character_fade_ready() back.
        constexpr int k_eye_kind = kind_labelled("Eye");
        static_assert(k_eye_kind >= 0, "The generated header has no Eye twin.");

        /**
         * @brief The kind (index in blobs::k_twins) whose stock shader @p shader is an instance of, by the shader's
         *        name (IShader::GetName, the string at +0x88), or -1 for any other shader.
         * @note Render jobs, for a shader on the character that has no twin entry yet, and the main thread.
         *       SEH-guarded, POD body.
         */
        [[nodiscard]] int source_kind_guarded(std::uintptr_t shader) noexcept
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
                    if (equals_nocase(name, blobs::k_twins[kind].source_name))
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

        /**
         * @brief Queues a request for a twin of @p kind for @p item's shader, counting it when the queue is full.
         *        Nothing is queued once the source table is full, since the shader would get no entry.
         */
        void request_twin(const SwapSet &set, const ShaderItem &item, int kind) noexcept
        {
            if (set.sources_full)
            {
                return;
            }
            if (!s_requests.push(TwinRequest{.shader = item.shader, .resources = item.resources, .kind = kind}))
            {
                s_dropped_requests.fetch_add(1, std::memory_order_relaxed);
            }
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
         * @brief Puts @p item's twin in @p out when @p object is a temporary render object that names one of @p set's
         *        character nodes and the item's shader is a source instance whose twin is ready (and, while the object
         *        dissolves, warm for the fade).
         * @details Every other object (a persistent one, another character's) keeps the stock shaders. An item takes
         *          its twin as soon as the twin is ready while the object does not dissolve (the twin draws like the
         *          stock shader then, and its builds prepare the fade's sets, see build_resting_psos()), and while it
         *          dissolves only once the twin's sets for the fade are warm. A render job stamps each source it sees,
         *          so the readiness knows the character's current items. A source shader with no entry yet is queued
         *          for the main thread to create its twin; the item stays on its source this time (a temporary object
         *          is added again next frame). A transparent-only kind (IllumFade) takes only an item whose opacity is
         *          below 1: an opaque Illum item of the character (his clothes) never asks for, warms or swaps to that
         *          twin. An item that is not set up yet (preprocess flags -1) is left to the engine, which invalidates
         *          the object itself.
         * @note Render jobs. Reads engine data that is valid for the call.
         */
        [[nodiscard]] bool twin_item(const SwapSet &set, const ShaderItem &item, const std::uint8_t *object,
                                     ShaderItem &out) noexcept
        {
            if (item.shader == 0 || item.preprocess_flags == 0xFFFFFFFFu ||
                object[Constants::RENDER_OBJECT_PERSISTENT_OFFSET] != 0)
            {
                return false;
            }
            const bool dissolving = dissolving_object(object);
            if (!character_object(set, object))
            {
                if (dissolving)
                {
                    s_node_rejects.fetch_add(1, std::memory_order_relaxed);
                }
                return false;
            }
            // The camera stopped updating the set (a menu, a reload): the items keep the stock shader.
            const std::uint64_t now = GetTickCount64();
            if (now - set.stamp_ms > Constants::SHADER_TWIN_STALE_MS)
            {
                return false;
            }
            const int source_count = std::min(set.source_count, k_max_sources);
            for (int i = 0; i < source_count; ++i)
            {
                const auto slot = static_cast<std::size_t>(i);
                if (set.sources[slot] != item.shader)
                {
                    continue;
                }
                const int kind = set.kinds[slot];
                // An opaque item of a transparent-only kind keeps its source and does not count as on the character.
                if (!item_fits_kind(kind, item))
                {
                    note_opacity_skip(dissolving);
                    return false;
                }
                s_character_seen_ms[slot].store(now, std::memory_order_relaxed);
                // Not ready, given up, or (a dissolving item) not warm for the fade yet: the item keeps its source.
                if (set.twins[slot] == 0 || (dissolving && !set.fade_ready[slot]))
                {
                    return false;
                }
                out = item;
                out.shader = set.twins[slot];
                s_item_swaps.fetch_add(1, std::memory_order_relaxed);
                if (dissolving)
                {
                    s_kind_swaps[static_cast<std::size_t>(kind)].fetch_add(1, std::memory_order_relaxed);
                }
                else
                {
                    s_rest_swaps.fetch_add(1, std::memory_order_relaxed);
                }
                return true;
            }
            // The name test first, which rejects nearly every item of the character.
            const int kind = source_kind_guarded(item.shader);
            if (kind < 0)
            {
                return false;
            }
            if (!item_fits_kind(kind, item))
            {
                note_opacity_skip(dissolving);
                return false;
            }
            if (!set.sources_full)
            {
                s_character_unknown.store(true, std::memory_order_relaxed);
            }
            request_twin(set, item, kind);
            return false;
        }

        /**
         * @brief CRenderView::AddRenderObject detour: a main-view Eye, Hair or transparent Illum item of the player
         *        character in third person draws with its twin shader.
         * @details The twin goes in a stack copy of the item (same resources, technique and flags). The engine keeps
         *          only its shader and resources pointers in a pooled compiled object (sub_1804A6104), and the twin is
         *          never released, so the copy does not outlive anything it names.
         * @note Raw arbitrary-signature detour (DMK [B-84]): it must not throw, and every call in it is noexcept.
         */
        void __fastcall add_render_object_detour(void *view, void *element, const ShaderItem *item,
                                                 std::uint8_t *object, const std::uint8_t *pass, int list,
                                                 int after_water) noexcept
        {
            const DetourScope in_flight;
            const AddRenderObjectFn original = s_add_render_object_original.load(std::memory_order_acquire);
            const SwapSet *set = s_set_published.load(std::memory_order_acquire);
            if (set != nullptr && element != nullptr && item != nullptr && object != nullptr && pass != nullptr &&
                main_view_pass(pass))
            {
                ShaderItem swapped{};
                if (twin_item(*set, *item, object, swapped))
                {
                    original(view, element, &swapped, object, pass, list, after_water);
                    return;
                }
            }
            original(view, element, item, object, pass, list, after_water);
        }

        /**
         * @brief .ext loader detour: a twin's name loads its stock shader's gen flags.
         * @details The loader formats "%s%s.ext" from the name and caches the result by name, so passing the stock
         *          name ("Eye", "Hair", "Illum") returns the SShaderGen every instance of it already uses, and the
         *          twin's gen-mask bits mean the same thing as its source's. mfForName passes the name it was given,
         *          the twin's full name; a longer name that starts with it is taken too.
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
         *          a PSO whose D3D12 creation is only queued (Constants::PSO_STATE_QUEUED): that pass draws within a
         *          frame or two, and a twin taken now could lack it for the whole compile of its own. A twin PSO whose
         *          creation keeps failing leaves the item on its stock PSOs, and the warm-up gives that twin up
         *          (give_up_fade()).
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

        /** @brief Both PSO sets of one build: the twin's, built aside, and the stock's, built into the object. */
        struct PsoPair
        {
            PsoSet twin{};
            std::uint32_t slots = 0;      // bit i: slot i was written by this build
            bool twin_built = false;      // the twin build reported success
            std::uint8_t stock_built = 0; // the stock build's result
        };

        /**
         * @brief Builds a compiled object's PSOs twice: @p twin_shader's set into a local array, then @p stock_shader's
         *        into the object.
         * @details Both descriptions are copies of the one the engine built for the object with only the shader
         *          replaced, so the resources' local PSO cache keys the two apart. The twin set goes first, which asks
         *          the engine for every permutation it needs with the object's real RT mask, flags and pipeline state.
         *          A slot this build wrote is one the twin's got a PSO in or the stock's changed: the stages rewrite
         *          their own slots only, so the others keep what the object had.
         * @note Render thread and compile jobs, inside the engine's own call; re-entrant. Never throws.
         */
        [[nodiscard]] PsoPair build_pair(CreatePsosFn original, void *pipeline, std::uintptr_t *psos,
                                         const std::uint8_t *desc, void *cache, std::uintptr_t twin_shader,
                                         std::uintptr_t stock_shader) noexcept
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
            pair.twin_built = original(pipeline, pair.twin.data(), twin_desc.data(), cache, 0) != 0;
            pair.stock_built = original(pipeline, psos, stock_desc.data(), cache, 0);
            for (int i = 0; i < k_pso_slots; ++i)
            {
                const auto slot = static_cast<std::size_t>(i);
                pair.slots |= (pair.twin[slot] != 0 || psos[slot] != before[slot]) ? (1u << i) : 0u;
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

        /** @brief The warm-up entry whose twin is @p shader (a ready twin, fade-warm or not), or -1. */
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

        /** @brief The 64-bit field of a pipeline-state description at @p offset. */
        [[nodiscard]] std::uint64_t desc_field(const std::uint8_t *desc, std::ptrdiff_t offset) noexcept
        {
            std::uint64_t value = 0;
            std::memcpy(&value, desc + offset, sizeof(value));
            return value;
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
        [[nodiscard]] std::uint8_t build_warming_psos(CreatePsosFn original, void *pipeline, std::uintptr_t *psos,
                                                      std::uint8_t *desc, void *cache, int slot,
                                                      std::uintptr_t root) noexcept
        {
            const std::uintptr_t twin_shader =
                s_warm_twins[static_cast<std::size_t>(slot)].load(std::memory_order_relaxed);
            const std::uintptr_t source = *reinterpret_cast<const std::uintptr_t *>(desc);
            const std::uint64_t key = desc_key(desc, twin_shader);
            if (claim_key(key) != Claim::New)
            {
                return original(pipeline, psos, desc, cache, 0);
            }
            const PsoPair pair = build_pair(original, pipeline, psos, desc, cache, twin_shader, source);
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
        [[nodiscard]] std::uint8_t build_resting_psos(CreatePsosFn original, void *pipeline, std::uintptr_t *psos,
                                                      const std::uint8_t *desc, void *cache, int slot,
                                                      std::uintptr_t root) noexcept
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
                record.twin_built = original(pipeline, record.twin.data(), twin_desc.data(), cache, 0) != 0;
                (void)original(pipeline, record.stock.data(), stock_desc.data(), cache, 0);
                for (int i = 0; i < k_pso_slots; ++i)
                {
                    const auto at = static_cast<std::size_t>(i);
                    record.slots |= (record.twin[at] != 0 || record.stock[at] != 0) ? (1u << i) : 0u;
                }
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
         * @brief Builds a dissolving item that names a twin twice (build_pair()): the twin's set aside and its source's
         *        into the object, and the twin's taken only when it can draw every pass the source's draws.
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
        [[nodiscard]] std::uint8_t build_fading_psos(CreatePsosFn original, void *pipeline, std::uintptr_t *psos,
                                                     const std::uint8_t *desc, void *cache, int slot,
                                                     std::uintptr_t root) noexcept
        {
            const auto entry = static_cast<std::size_t>(slot);
            const std::uintptr_t twin_shader = *reinterpret_cast<const std::uintptr_t *>(desc);
            const std::uintptr_t source = s_warm_sources[entry].load(std::memory_order_relaxed);
            const std::uint64_t key = desc_key(desc, twin_shader);
            const bool ahead = key_known(key);
            if (!ahead)
            {
                note_fade_miss(desc);
            }
            PsoPair pair = build_pair(original, pipeline, psos, desc, cache, twin_shader, source);
            if (take_ready_twin(psos, pair))
            {
                s_fade_warm_builds.fetch_add(1, std::memory_order_relaxed);
                s_fade_warm_ahead.fetch_add(ahead ? 1 : 0, std::memory_order_relaxed);
                return 1;
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
         * @brief CreatePipelineStates detour: a compiled object that names a ready twin and does not dissolve keeps the
         *        stock set and builds the fade's sets ahead (see build_resting_psos()); one that names a ready twin and
         *        dissolves takes the twin's only when they can draw (see build_fading_psos()); and a dissolving stock
         *        item of the character whose twin is not warm for the fade, and that the twin's kind may take
         *        (warming_item_fits()), builds the twin's set aside (see build_warming_psos()).
         * @details Shadow builds are never swapped and pass straight through, as does every object with another shader
         *          (one atomic load while no twin is ready).
         * @note Raw arbitrary-signature detour (DMK [B-84]): it must not throw, and every call in it is noexcept.
         */
        std::uint8_t __fastcall create_psos_detour(void *pipeline, std::uintptr_t *psos, std::uint8_t *desc,
                                                   void *cache, int is_shadow) noexcept
        {
            const DetourScope in_flight;
            const CreatePsosFn original = s_create_psos_original.load(std::memory_order_acquire);
            if (is_shadow == 0 && psos != nullptr && desc != nullptr)
            {
                const std::uintptr_t shader = *reinterpret_cast<const std::uintptr_t *>(desc);
                if (s_warm_count.load(std::memory_order_acquire) > 0)
                {
                    if (const int slot = character_twin_slot(shader); slot >= 0)
                    {
                        const bool fading = (desc_field(desc, Constants::PSO_DESC_OBJECT_FLAGS_OFFSET) &
                                             Constants::RENDER_OBJECT_FLAG_DISSOLVE) != 0;
                        const std::uintptr_t root = published_character_root();
                        return fading ? build_fading_psos(original, pipeline, psos, desc, cache, slot, root)
                                      : build_resting_psos(original, pipeline, psos, desc, cache, slot, root);
                    }
                    const int slot = warming_slot(shader);
                    const SwapSet *set = slot >= 0 ? s_set_published.load(std::memory_order_acquire) : nullptr;
                    if (set != nullptr && set->character_count > 0 && dissolving_psos_guarded(psos, *set) &&
                        warming_item_fits(slot, desc))
                    {
                        return build_warming_psos(original, pipeline, psos, desc, cache, slot, set->character_nodes[0]);
                    }
                }
            }
            return original(pipeline, psos, desc, cache, is_shadow);
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

        /** @brief True when @p object's vtable carries the RTTI name @p mangled. */
        [[nodiscard]] bool object_has_type(std::uintptr_t object, std::string_view mangled) noexcept
        {
            const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{object});
            return vtable && *vtable != 0 && DMK::rtti::vtable_is_type(DMK::Address{*vtable}, mangled);
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
         * @brief Writes @p blob to @p path unless the file there already holds exactly it.
         * @param written Set when the file was (re)written.
         * @return An empty string on success, else what went wrong.
         */
        [[nodiscard]] std::string write_blob(const std::filesystem::path &path, const blobs::Blob &blob, bool &written)
        {
            written = false;
            std::error_code error;
            if (std::filesystem::file_size(path, error) == blob.size && !error)
            {
                std::ifstream existing(path, std::ios::binary);
                const std::vector<char> bytes{std::istreambuf_iterator<char>(existing), {}};
                if (bytes.size() == blob.size &&
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
                DMK::LogLevel::Info,
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

            // Every twin copies its own stock shader and keeps the stock includes, which the stock header CRC covers.
            // A game patch that changed one of them would leave its twin out of step with the gen flags it borrows, so
            // only that twin turns off, and its items keep the stock shader.
            std::string twin_names;
            for (int kind = 0; kind < k_kinds; ++kind)
            {
                const blobs::Twin &twin = blobs::k_twins[kind];
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
                "match the game's",
                std::size(blobs::k_blobs), utf8_text(directory), written_count, removed, twin_names);
            read_dissolve_rt_bit();
            return {};
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

        /** @brief Drops a held record's references. */
        void drop_record(const PsoRecord &record) noexcept
        {
            drop_pso_refs(record.stock);
            drop_pso_refs(record.twin);
        }

        /** @brief Turns the feature off for the session, unpublishes the swap set and says why, once. */
        void turn_off(const char *why) noexcept
        {
            if (s_disabled.exchange(true, std::memory_order_acq_rel))
            {
                return;
            }
            s_eye_ready.store(false, std::memory_order_release);
            s_character_ready.store(false, std::memory_order_release);
            s_set_published.store(nullptr, std::memory_order_release);
            (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                     "ShaderTwins: {}; the character keeps the stock shaders (its eyes, hair and eye "
                                     "film do not fade) for the session",
                                     why);
        }

        /**
         * @brief Prepares the twins on the first frame that names a character node, once the engine is up.
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
                turn_off(error.c_str());
                return false;
            }
            s_prepare = Prepare::Ready;
            return true;
        }

        /**
         * @brief Creates the twin of one source instance a render job asked for, from its kind's twin shader.
         * @details The twin is created the way the material path creates its source (EF_LoadShaderItem -> mfForName):
         *          flags 0, the material's CShaderResources and the source's gen mask. A request whose shader or
         *          resources are no longer what they were takes no slot, so a live shader at that address later still
         *          gets its twin. A source that takes a slot keeps one more reference for the session, whatever becomes
         *          of its twin, so its address can never be reused by another shader while the swap set names it, and
         *          the twin's reference is never released, because pooled compiled objects keep pointers to it. A
         *          source of a failed kind (s_kind_failed) keeps its stock shader.
         */
        void create_twin(const TwinRequest &request, std::uint64_t now) noexcept
        {
            if (request.kind < 0 || request.kind >= k_kinds || find_source(request.shader, request.kind) >= 0)
            {
                return;
            }
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

            // The request came from a render job a frame ago: the shader and its resources must still be what they say
            // (an instance of the kind's stock shader), and the shader must take the reference that holds its address.
            const int kind = object_has_type(request.shader, Constants::CSHADER_RTTI_NAME)
                                 ? source_kind_guarded(request.shader)
                                 : -1;
            if (kind != request.kind ||
                (request.resources != 0 &&
                 !object_has_type(request.resources, Constants::CSHADER_RESOURCES_RTTI_NAME)) ||
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
            const auto mask =
                DMK::memory::read<std::uint64_t>(DMK::Address{request.shader + Constants::SHADER_GEN_MASK_OFFSET});
            const auto techniques = DMK::memory::read<std::uint32_t>(
                DMK::Address{request.shader + Constants::SHADER_TECHNIQUE_COUNT_OFFSET});
            if (!mask || !techniques || *techniques == 0)
            {
                (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                         "ShaderTwins: {} shader {:#x} ({}) could not be read; no twin", twin.label,
                                         request.shader, source.name.data());
                return;
            }
            source.mask = *mask;
            source.techniques = *techniques;
            source.twin =
                for_name_guarded(s_for_name, s_shader_manager, twin.twin_name, request.resources, source.mask);
            if (source.twin == 0)
            {
                s_guard_faults.fetch_add(1, std::memory_order_relaxed);
                (void)DMK::log().try_log(DMK::LogLevel::Warning, "ShaderTwins: mfForName({}) failed for {}; no twin",
                                         twin.twin_name, source.name.data());
                return;
            }
            source.state = TwinState::Pending;
            (void)DMK::log().try_log(DMK::LogLevel::Info,
                                     "ShaderTwins: {} twin {:#x} requested for {} (CShader {:#x}, gen mask {:#x}, "
                                     "{} techniques, resources {:#x})",
                                     twin.label, source.twin, source.name.data(), source.shader, source.mask,
                                     source.techniques, request.resources);
        }

        /** @brief Creates the twins the render jobs asked for since the last frame. */
        void drain_requests(std::uint64_t now) noexcept
        {
            TwinRequest request{};
            while (s_requests.pop(request))
            {
                create_twin(request, now);
            }
        }

        /**
         * @brief Moves pending twins to ready (or gives them up).
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
                        turn_off("no twin shader loaded");
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
                        // The twin goes in the warm-up table before the swap set that carries it is published: its
                        // items, added again every frame, take it at rest, and their builds then prepare its sets for
                        // the fade.
                        const int at = s_warm_count.load(std::memory_order_relaxed);
                        if (at < k_max_sources)
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
                        (void)DMK::log().try_log(DMK::LogLevel::Info,
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
                    DMK::LogLevel::Info,
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
            (void)DMK::log().try_log(DMK::LogLevel::Info,
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
                (void)DMK::log().try_log(DMK::LogLevel::Info,
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
                DMK::LogLevel::Info,
                "ShaderTwins: a fading build of a character twin asked for a description no set was "
                "built ahead for (RT mask {:#x}, flags {:#x}); it takes the twin only if the "
                "engine has its permutations, else it turns the twin cold",
                s_fade_miss_rt_mask.load(std::memory_order_relaxed), s_fade_miss_flags.load(std::memory_order_relaxed));
        }

        /**
         * @brief Publishes the character's render nodes and the twins, or nothing when no character node was passed.
         * @param character_nodes The character's render nodes (none zero, at most k_max_character_nodes), or empty.
         */
        void publish_set(std::span<const std::uintptr_t> character_nodes, std::uint64_t now) noexcept
        {
            if (character_nodes.empty())
            {
                s_set_published.store(nullptr, std::memory_order_release);
                return;
            }
            SwapSet &set = s_sets[static_cast<std::size_t>(s_next_set)];
            s_next_set = (s_next_set + 1) % static_cast<int>(s_sets.size());
            set.character_count = static_cast<int>(std::min(character_nodes.size(), set.character_nodes.size()));
            std::copy_n(character_nodes.begin(), set.character_count, set.character_nodes.begin());
            set.sources_full = s_source_count >= k_max_sources;
            set.source_count = s_source_count;
            for (int i = 0; i < s_source_count; ++i)
            {
                const Source &source = s_sources[static_cast<std::size_t>(i)];
                const auto at = static_cast<std::size_t>(i);
                const bool usable = source.state == TwinState::Ready;
                set.sources[at] = source.shader;
                set.twins[at] = usable ? source.twin : 0;
                set.kinds[at] = static_cast<std::uint8_t>(source.kind);
                set.fade_ready[at] = usable && source.warm;
            }
            set.stamp_ms = now;
            s_set_published.store(&set, std::memory_order_release);
        }

        /**
         * @brief Whether the eyes, and everything of the character, fade with him now (eye_fade_ready(),
         *        character_fade_ready()), logged once when either changes, with the counts.
         * @details Only the twin sources a render job saw on him in the last CHARACTER_TWIN_RECENT_MS count
         *          (his eyes, eyelashes, beard, hair and eye film, whether he dissolves or not), and a shader seen on
         *          him that the table did not know yet counts as not warm. The eyes fade once the twins run, an Eye
         *          source on him is warm for the fade, and every such source is warm, except a Hair or IllumFade one
         *          that was given up: its items stay solid, which the head hide at the top of the fade covers, while an
         *          Eye one would leave solid eyes in the face. Everything fades once, besides, every such source is
         *          warm, none given up. A warm source turns not warm only when a fading build reports it cold
         *          (take_cold_report()), a level change finds a set of it for the new character node still waiting
         *          (forget_warm_sets()) or it is given up (give_up_fade()), and a new item's source starts not warm,
         *          so the readiness does not flicker; while none of his sources was seen (he is not drawn) it keeps
         *          its last value, and so it does while no character node was passed (first person), so a view toggle
         *          does not flip it. Only the character's items are stamped and warm up (twin_item(),
         *          build_resting_psos()), so other characters fading by distance never count. A stamp a render job made
         *          after this frame read its clock counts as recent.
         * @param running The twins run this frame: the character's render nodes were passed.
         */
        void refresh_character_ready(bool running, std::uint64_t now) noexcept
        {
            const bool unknown = s_character_unknown.exchange(false, std::memory_order_relaxed);
            if (!running)
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
                const std::uint64_t seen =
                    s_character_seen_ms[static_cast<std::size_t>(i)].load(std::memory_order_relaxed);
                if (seen == 0 || (seen < now && now - seen > Constants::CHARACTER_TWIN_RECENT_MS))
                {
                    continue;
                }
                const bool usable = source.state == TwinState::Ready && source.warm;
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
                const bool given_up = source.state == TwinState::Failed;
                count.given_up += given_up ? 1 : 0;
                eyes_held = eyes_held || eye || !given_up;
                all_held = true;
                first_cold = first_cold != nullptr ? first_cold : &source;
            }
            // None of his sources seen (he is not drawn) and no unknown shader: the readiness keeps its last value.
            const bool keep = recent == 0 && !unknown;
            const bool eyes_ready =
                keep ? s_eye_ready.load(std::memory_order_relaxed) : eye_warm > 0 && !eyes_held && !unknown;
            const bool all_ready = keep ? s_character_ready.load(std::memory_order_relaxed) : eyes_ready && !all_held;
            s_eye_ready.store(eyes_ready, std::memory_order_release);
            s_character_ready.store(all_ready, std::memory_order_release);
            const int logged = (eyes_ready ? 1 : 0) | (all_ready ? 2 : 0);
            if (logged == s_ready_logged)
            {
                return;
            }
            s_ready_logged = logged;
            // Per kind "Hair 4/5 warm" (of the sources on him now), then the first one not warm.
            std::array<char, 320> seen_text{};
            std::size_t used = 0;
            for (int kind = 0; kind < k_kinds; ++kind)
            {
                const KindCount &count = counts[static_cast<std::size_t>(kind)];
                if (used + 1 >= seen_text.size())
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
            (void)DMK::log().try_log(DMK::LogLevel::Info,
                                     "ShaderTwins: the character's twins: the eyes {}, everything {} ({} Eye twin(s) "
                                     "warm for the fade; on him now: {}; {} of {} set(s) for the fade waiting{})",
                                     eyes_ready ? "fade with him" : "do not fade yet",
                                     all_ready ? "fades" : "does not fade yet", eye_warm, seen_text.data(),
                                     s_warm_pending_count, s_warm_entry_count,
                                     unknown ? "; a shader without a twin entry seen on him" : "");
        }

        /** @brief Once a second, logs what the hooks did when anything happened. */
        void log_second(std::uint64_t now) noexcept
        {
            if (now - s_last_log_ms < 1000)
            {
                return;
            }
            s_last_log_ms = now;
            const std::uint32_t swaps = s_item_swaps.exchange(0, std::memory_order_relaxed);
            const std::uint32_t rest_swaps = s_rest_swaps.exchange(0, std::memory_order_relaxed);
            std::array<std::uint32_t, k_kinds> kind_swaps{};
            std::uint32_t dissolve_swaps = 0;
            for (int kind = 0; kind < k_kinds; ++kind)
            {
                const auto at = static_cast<std::size_t>(kind);
                kind_swaps[at] = s_kind_swaps[at].exchange(0, std::memory_order_relaxed);
                dissolve_swaps += kind_swaps[at];
            }
            const std::uint32_t node_rejects = s_node_rejects.exchange(0, std::memory_order_relaxed);
            const std::uint32_t opacity_skips = s_opacity_skips.exchange(0, std::memory_order_relaxed);
            const std::uint32_t aliases = s_ext_aliases.exchange(0, std::memory_order_relaxed);
            const std::uint32_t faults = s_guard_faults.exchange(0, std::memory_order_relaxed);
            const std::uint32_t dropped = s_dropped_requests.exchange(0, std::memory_order_relaxed);
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
            const bool build_news = rest_builds != 0 || ahead != 0 || ahead_full != 0 || warmups != 0 ||
                                    warm_dropped != 0 || fade_warm != 0 || fade_cold != 0 || kept_dropped != 0 ||
                                    s_warm_pending_count != s_last_logged_pending;
            if (swaps == rest_swaps && aliases == 0 && faults == 0 && dropped == 0 && stale == 0 && !build_news)
            {
                return;
            }
            if (build_news)
            {
                s_last_logged_pending = s_warm_pending_count;
                (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                         "ShaderTwins: in the last second: {} build(s) at rest, {} set(s) for the fade "
                                         "built ahead ({} built before, {} with the key table full), {} built aside by "
                                         "a fading build of the stock shader; {} fading build(s) of a twin took its "
                                         "PSOs at once ({} of them from a set built ahead), {} kept the stock PSOs; {} "
                                         "set(s) not handed over, {} not kept; {} of {} kept set(s) waiting",
                                         rest_builds, ahead, ahead_known, ahead_full, warmups, fade_warm,
                                         fade_warm_ahead, fade_cold, warm_dropped, kept_dropped, s_warm_pending_count,
                                         s_warm_entry_count);
            }
            if (dissolve_swaps != 0)
            {
                std::array<char, 160> per_kind{};
                std::size_t used = 0;
                for (int kind = 0; kind < k_kinds; ++kind)
                {
                    if (used + 1 >= per_kind.size())
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
            int twins = 0;
            int ready = 0;
            for (int i = 0; i < s_source_count; ++i)
            {
                const Source &source = s_sources[static_cast<std::size_t>(i)];
                twins += source.twin != 0 ? 1 : 0;
                ready += source.state == TwinState::Ready ? 1 : 0;
            }
            (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                     "ShaderTwins: {} twin item(s) added ({} of them while he fades, {} at rest), {} "
                                     ".ext alias(es) in the last second; twins {}/{} ready; {} PSO reference(s) left "
                                     "held",
                                     swaps, dissolve_swaps, rest_swaps, aliases, ready, twins,
                                     s_pso_leaked.load(std::memory_order_relaxed));
            if (faults != 0 || dropped != 0 || stale != 0)
            {
                (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                         "ShaderTwins: {} guarded read/write fault(s), {} dropped twin request(s) and "
                                         "{} twin request(s) for a shader gone before it was handled in the last "
                                         "second",
                                         faults, dropped, stale);
            }
        }

        /**
         * @brief Installs one inline hook, stores it in @p hooks, and publishes its trampoline before it is armed.
         * @throws std::bad_alloc If the hook name or the hook set cannot allocate.
         */
        template <typename Func, typename Detour>
        [[nodiscard]] DMK::Result<void> install(HookSet &hooks, std::string_view name, std::uintptr_t target,
                                                std::atomic<Func> &original, Detour detour)
        {
            auto result = DMK::hook::inline_at(
                DMK::hook::InlineRequest{
                    .name = std::string{name},
                    .target = DMK::Address{target},
                },
                detour);
            if (!result.has_value())
            {
                return std::unexpected(result.error());
            }
            original.store(result->template original<Func>(), std::memory_order_release);
            if (auto armed = hooks.push(std::move(*result)).enable(); !armed.has_value())
            {
                return std::unexpected(armed.error());
            }
            return {};
        }

    } // namespace

    DMK::Result<void> initialize_shader_twins(HookSet &hooks)
    {
        DMK::Logger &logger = DMK::log();
        const std::uintptr_t genv = gated_anchor_address(Feature::ShaderTwins, AnchorId::Genv);
        const std::uintptr_t add_render_object = gated_anchor_address(Feature::ShaderTwins, AnchorId::AddRenderObject);
        const std::uintptr_t shader_gen = gated_anchor_address(Feature::ShaderTwins, AnchorId::ShaderGenLoad);
        const std::uintptr_t for_name = gated_anchor_address(Feature::ShaderTwins, AnchorId::ShaderForName);
        const std::uintptr_t adjust = gated_anchor_address(Feature::ShaderTwins, AnchorId::AdjustFileName);
        const std::uintptr_t create_psos = gated_anchor_address(Feature::ShaderTwins, AnchorId::CreatePsos);
        const std::optional<std::int64_t> open_slot = gated_anchor_value(Feature::ShaderTwins, AnchorId::PakOpenSlot);
        const std::optional<std::int64_t> read_slot = gated_anchor_value(Feature::ShaderTwins, AnchorId::PakReadSlot);
        const std::optional<std::int64_t> close_slot = gated_anchor_value(Feature::ShaderTwins, AnchorId::PakCloseSlot);
        if (genv == 0 || add_render_object == 0 || shader_gen == 0 || for_name == 0 || adjust == 0 ||
            create_psos == 0 || !open_slot || !read_slot || !close_slot)
        {
            return std::unexpected(DMK::Error{DMK::ErrorCode::NoMatch, "shader_twins/anchors"});
        }
        s_genv = genv;
        s_for_name = for_name;
        s_adjust_file_name = adjust;
        s_pak_open_slot = static_cast<std::ptrdiff_t>(*open_slot);
        s_pak_read_slot = static_cast<std::ptrdiff_t>(*read_slot);
        s_pak_close_slot = static_cast<std::ptrdiff_t>(*close_slot);
        // The dissolve's RT-mask bit the character twins' sets for the fade are built ahead with; read when the twins
        // are prepared (read_dissolve_rt_bit()).
        s_dissolve_rt_global = gated_anchor_address(Feature::FadeWarmupRtBit, AnchorId::DissolveRtBit);

        // The .ext alias first, so it is in place before any twin is created, and the PSO detour before the swap, so
        // no compiled object ever takes a twin PSO set that cannot draw yet. The swap stays inert until the first
        // frame publishes a swap set.
        DMK_TRY_VOID(install(hooks, "ShaderGenLoad", shader_gen, s_shader_gen_original, shader_gen_load_detour));
        DMK_TRY_VOID(install(hooks, "CreatePipelineStates", create_psos, s_create_psos_original, create_psos_detour));
        DMK_TRY_VOID(install(hooks, "CRenderViewAddRenderObject", add_render_object, s_add_render_object_original,
                             add_render_object_detour));

        s_available.store(true, std::memory_order_release);
        logger.info("ShaderTwins: hooks on AddRenderObject {}, the .ext loader {}, CreatePipelineStates {}; mfForName "
                    "{}, CryPak slots FOpen {:#x} FReadRaw {:#x} FClose {:#x}; the twins are prepared when the camera "
                    "first names the character",
                    DMK::format::format_address(add_render_object), DMK::format::format_address(shader_gen),
                    DMK::format::format_address(create_psos), DMK::format::format_address(for_name), s_pak_open_slot,
                    s_pak_read_slot, s_pak_close_slot);
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

    void update_shader_twins(std::span<const std::uintptr_t> character_nodes) noexcept
    {
        if (!s_available.load(std::memory_order_acquire))
        {
            return;
        }
        const std::unique_lock lock(s_main_mutex, std::try_to_lock);
        if (!lock.owns_lock() || !s_available.load(std::memory_order_acquire) ||
            s_disabled.load(std::memory_order_acquire))
        {
            return;
        }
        const std::uint64_t now = GetTickCount64();
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
        // Nothing to prepare for until the camera first names the character.
        if (s_prepare == Prepare::Pending && character_count == 0)
        {
            return;
        }
        if (!prepare(now))
        {
            return;
        }
        drain_requests(now);
        refresh_twins(now);
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
        // so does a node that changes back.
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
        const std::span<const std::uintptr_t> listed(characters.data(), character_count);
        publish_set(listed, now);
        refresh_character_ready(!listed.empty(), now);
        log_second(now);
    }

    void shutdown_shader_twins() noexcept
    {
        try
        {
            const std::lock_guard lock(s_main_mutex);
            if (!s_available.exchange(false, std::memory_order_acq_rel))
            {
                return;
            }
            s_eye_ready.store(false, std::memory_order_release);
            s_character_ready.store(false, std::memory_order_release);
            s_set_published.store(nullptr, std::memory_order_release);
            // A render thread that sees the module off drops its record's references itself, but one that checked just
            // before the exchange above can still hand a record over until the hooks retire. These are dropped now,
            // and release_shader_twin_records() drops any that arrive later, once no detour can run.
            PsoRecord record{};
            while (s_warm_records.pop(record))
            {
                drop_record(record);
            }
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
                                     "ShaderTwins: shutdown; {} twin shader(s) stay loaded for the session; {} PSO "
                                     "reference(s) left held rather than drop a last one",
                                     twins, s_pso_leaked.load(std::memory_order_relaxed));
        }
        catch (...)
        {
            // A lock that cannot be taken leaves the held sets to the session; the hooks still retire.
            s_available.store(false, std::memory_order_release);
            s_eye_ready.store(false, std::memory_order_release);
            s_character_ready.store(false, std::memory_order_release);
            s_set_published.store(nullptr, std::memory_order_release);
        }
    }

    void release_shader_twin_records() noexcept
    {
        try
        {
            const std::lock_guard lock(s_main_mutex);
            int released = 0;
            PsoRecord record{};
            while (s_warm_records.pop(record))
            {
                drop_record(record);
                ++released;
            }
            if (released != 0)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "ShaderTwins: dropped {} set(s) for the fade handed over while the shutdown ran", released);
            }
        }
        catch (...)
        {
            // A lock that cannot be taken leaves those references held: a few PSOs stay alive, nothing breaks.
        }
    }

} // namespace TPVCamera
