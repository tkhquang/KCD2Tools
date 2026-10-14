/**
 * @file dev/twin_clone/twin_clone.hpp
 * @brief The DEV twin clone's read-only dry-run: plans a plant or frame twin from its parsed stock shader instead of a
 *        parse of the twin's binary, and compares the plan with the twin the engine parsed.
 *
 * A clone copies its stock shader's techniques, keeps every pass of the stock but the Z pre-pass and Z pass pixel
 * shaders, and asks the engine's HW shader factory for those under the twin's name, with the token maps derived from
 * the stock's (shader_token_splice.hpp). The dry-run only plans that (prepare_clone_plan()): what the clone's CShader
 * and techniques hold, the two derived Z-pass maps, and the keys the factory files them under, reading the stock shader
 * and never writing anything. It then compares each plan with the twin the engine parsed for the same stock shader
 * (dry_run_compare()), the ground truth a clone must reproduce. A kind is clone-eligible for the session once at least
 * one of its twins compared without a mismatch; run_dry_run() logs that verdict when its queue empties.
 *
 * The dry-run runs only inside a loading boundary, after its twins were created, and in a frame the game stands still
 * in whose pause has nothing left to create (hooks/shader_twins.hpp), never in unpaused gameplay, and each step is
 * bounded in pairs and time. It needs the TwinCloneDryRun gate: the layout proofs of SShaderTechnique, CShader,
 * CBaseResource and CHWShader_D3D (aob_resolver.hpp), which pin every offset it reads, the HW shader factory and its
 * lock, and the resource lookup. The proofs pin layouts, not a build: they match Steam 1.5.6, GOG 1.5 and Game Pass
 * 1.4 alike, so the dry-run runs on each of them. They justify reads only.
 */
#ifndef TPVCAMERA_HOOKS_TWIN_CLONE_HPP
#define TPVCAMERA_HOOKS_TWIN_CLONE_HPP

#include "constants.hpp"
#include "shader_token_splice.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace TPVCamera::TwinClone
{
    /** @brief What the DEV twin clone runs in a build (k_stage). */
    enum class Stage : std::uint8_t
    {
        Off,    // a release build: nothing
        DryRun, // a DEV build: plans and compares with the parsed twins, never creates, arms or writes anything
    };

    // What this build runs. Only the DEV logic DLL compiles the clone's body (dev/twin_clone/twin_clone.cpp), so a
    // release build
    // defines none of the functions below: call them only from code under TPVCAMERA_DEV_BUILD or under
    // `if constexpr (k_stage == Stage::DryRun)`, whose discarded branch odr-uses nothing.
#if defined(TPVCAMERA_DEV_BUILD)
    inline constexpr Stage k_stage = Stage::DryRun;
#else
    inline constexpr Stage k_stage = Stage::Off;
#endif

    constexpr int k_max_techniques = Constants::TWIN_CLONE_MAX_TECHNIQUES;
    constexpr int k_slots = Constants::TECHNIQUE_SHADER_SLOTS;

    /** @brief Why prepare_clone_plan() made no plan. */
    enum class PlanStatus : std::uint8_t
    {
        Ready,
        NotClonable,     // the kind has no clone splice (a character twin), or is not a kind
        NotShader,       // the stock is not a CShader
        NotLoaded,       // its parse did not finish, or its binary did not load
        Reloaded,        // the engine reloaded it (EF_RELOADED)
        Techniques,      // no technique, or more than k_max_techniques
        TechniqueLayout, // a technique of another owner, or with render elements or render targets
        HardwareSlot,    // a technique slot that is not a CHWShader_D3D
        NoZPass,         // no Z pre-pass or Z pass pixel shader to derive
        TooManyZPasses,  // more than two distinct Z-pass pixel shaders
        Busy,            // the HW shader factory held its lock: tried again later
        TokenMap,        // a Z-pass pixel shader's token map is malformed or too large
        StockKey,        // a Z-pass pixel shader's key is not the factory's "<name>@<entry>(<platform>)(<mask>)"
        Splice,          // the splice refused the stock map (splice_status says why)
        Fault,           // a read faulted
        OutOfMemory,
    };

    /** @brief A status for logs. */
    [[nodiscard]] std::string_view describe(PlanStatus status) noexcept;

    /**
     * @brief A Z-pass pixel shader of the stock, and what a clone asks the HW shader factory for instead.
     * @details The factory call takes the rest of the permutation (class, RT masks, flags and type) from the stock
     * pass,
     *          which dry_run_compare() reads again.
     */
    struct ZPassPlan
    {
        std::uintptr_t stock_shader = 0; // the stock's CHWShader_D3D (a technique's pixel shader slot)
        ShaderSplice::Entry entry = ShaderSplice::Entry::ZPass;
        // The factory's resource name for the clone's pass, lower("<twin>@<entry>(<platform>)") plus "(<HW mask>)" when
        // the mask is not zero, and the name of its cache object, the same without the mask.
        std::array<char, 256> key{};
        std::array<char, 256> cache_key{};
        std::uint64_t hw_mask = 0; // the stock pass's HW mask, which key carries
        // The derived SHData and string table, which point into arena (the factory's inputs as they are), and the same
        // map as the engine stores one, for the compare.
        ShaderSplice::Plan splice{};
        ShaderSplice::Status splice_status = ShaderSplice::Status::Ok;
        std::vector<std::byte> arena;
        std::vector<std::uint8_t> derived;
    };

    /** @brief The CShader scalars a clone takes: its stock's, but for the binary CRCs its parsed twin carries. */
    struct ShaderScalars
    {
        std::uint32_t draw_type = 0;
        std::uint32_t flags = 0; // without EF_RELOADED and EF_NOTFOUND (a clone ORs its creation's flags in)
        std::uint32_t flags2 = 0;
        std::uint16_t features = 0;
        std::uint16_t features_high = 0;
        std::uint32_t techniques = 0;
        std::uint32_t fx_params_key = 0; // the stock's, shared (a parsed twin has its own)
        std::uint32_t shader_type = 0;
        std::uint64_t gen_mask = 0;
        std::uintptr_t gen_params = 0;
        std::uint32_t second_crc = 0; // 0, as every parsed twin carries
        std::uint32_t binary_crc = 0; // the family CRC in the twin's name
        std::uint32_t feature_count = 0;
        std::uint32_t decal_atlas_size = 0;
        std::uint32_t runtime_float = 0; // copied as it is; runtime state, not the parse's
    };

    /**
     * @brief A clone of one stock shader instance, planned and never armed: what a clone hands the engine, read from
     *        the stock alone.
     * @details Reused from pair to pair: its buffers keep their capacity.
     */
    struct ClonePlan
    {
        PlanStatus status = PlanStatus::NotClonable;
        std::uintptr_t stock = 0;
        int kind = -1;
        std::uint32_t family_crc = 0;
        std::uint32_t stock_flags = 0; // as read, for the check that nothing changed while the pair was compared
        ShaderScalars scalars{};
        std::uint32_t techniques = 0;
        // Each technique as the clone copies it (Constants::TECHNIQUE_SIZE bytes), and which Z-pass plan stands in for
        // its pixel shader (-1 for none).
        std::array<std::array<std::uint8_t, Constants::TECHNIQUE_SIZE>, k_max_techniques> technique_bytes{};
        std::array<std::int8_t, k_max_techniques> zpass_of{};
        int zpass_count = 0;
        std::array<ZPassPlan, 2> zpasses{};
        std::uint64_t micros = 0;
    };

    /**
     * @brief Plans a clone of the stock shader @p stock as a twin of @p kind (an index in shader_twin_blobs::k_twins).
     * @details Reads the stock CShader, its techniques and their HW shaders, checks that a clone could copy them (a
     *          loaded, unreloaded CShader of 1 to k_max_techniques techniques it owns, none with render elements or
     *          render targets, every slot empty or a CHWShader_D3D), and derives each Z-pass pixel shader's token map
     *          (shader_token_splice.hpp) and factory keys. The token maps are copied under the factory's lock taken
     *          shared without waiting: a busy lock gives PlanStatus::Busy. Writes nothing to the engine.
     * @return plan.status.
     * @note Main thread, at a loading boundary or in a paused frame, while the caller holds @p stock alive (the shader
     *       twins' sources are held for the session). No mod mutex may be held: the plan takes the factory's lock.
     */
    PlanStatus prepare_clone_plan(std::uintptr_t stock, int kind, ClonePlan &plan) noexcept;

    /** @brief How a plan compared with its parsed twin. */
    enum class Verdict : std::uint8_t
    {
        Exact,    // the parsed twin is what the plan would make
        Allowed,  // it differs only where a clone may (allowed holds why)
        Mismatch, // it differs where a clone may not (detail holds the first difference)
        Skipped,  // not compared (detail holds why); busy when the factory held its lock
    };

    /**
     * @brief The differences a clone may have from its parsed twin, as bits of CompareResult::allowed.
     * @details A string a derived Z-pass map's tokens name must have the same text in the parsed twin's table: a
     *          missing or other string would be a fault in the splice, so it is a mismatch, never allowed. Strings
     *          nothing names may differ (the map is then still equal).
     */
    namespace Allowed
    {
        // A Z-pass map differs only in the '#define' lines it opens with: the last parse to write a shared key.
        constexpr std::uint32_t HEADER_DEFINES = 1;
        // A pass a clone keeps from its stock differs from the parsed twin's (the last parse to write a shared stock
        // key, as on the Vegetation billboard masks): the clone then draws it exactly as the stock does. This rests
        // on scripts/build_shader_twins.py, which builds a twin that differs from its stock only in the Z-pass
        // includes; a generator change that touches another pass invalidates this rule.
        constexpr std::uint32_t STOCK_PASS = 2;
        // CShader's runtime float (Constants::SHADER_RUNTIME_FLOAT_OFFSET), which nothing in the parse writes.
        constexpr std::uint32_t RUNTIME_FLOAT = 4;
        // The number of bits above.
        constexpr int COUNT = 3;
    } // namespace Allowed

    /** @brief What dry_run_compare() found. */
    struct CompareResult
    {
        Verdict verdict = Verdict::Skipped;
        bool busy = false;         // skipped because the factory held its lock
        std::uint32_t allowed = 0; // Allowed bits
        int zpasses = 0;           // Z-pass maps compared
        int zpasses_identical = 0; // of them, byte for byte the derived map
        int stock_passes = 0;      // passes the clone keeps from the stock, compared with the twin's own objects
        std::array<char, 320> detail{};
        std::uint64_t micros = 0;
    };

    /**
     * @brief Compares a ready plan with @p twin, the twin the engine parsed for the plan's stock shader.
     * @details The CShader scalars (the vertex format byte, the FX parameters key and the reload stamp differ by design
     *          and are not compared; the runtime float is an allowed difference), the technique count and each
     *          technique's name, flags, empty render elements and targets, CRC and index table (+0x78 is never
     *          compared), and every HW slot. A Z-pass pixel shader must hold its stock pass's permutation (entry,
     *          class, RT and HW masks, flags without the runtime WASGENERATED bit, type) and match its plan: the key,
     *          family CRC, the name source (the twin's file name), the cache object the plan's cache key looks up (the
     *          parsed pass's must be that one), and the token map (shader_token_diff.hpp: a difference only in the
     *          leading #define lines is allowed; every string the tokens name must have the same text in both tables).
     *          Any other slot must hold the same permutation as the stock's, and a token difference there is allowed:
     *          a clone keeps the stock's object. A twin that shares its stock's FX parameters key is a clone, never
     *          ground truth, and is skipped.
     * @note Main thread, as prepare_clone_plan(), on a twin the caller holds alive. Reads only; takes the factory's
     *       lock shared without waiting and calls the engine's resource lookup.
     */
    [[nodiscard]] CompareResult dry_run_compare(const ClonePlan &plan, std::uintptr_t twin) noexcept;

    /** @brief A ready plant or frame twin and its stock source, as the shader twins hold them for the session. */
    struct DryRunPair
    {
        std::uintptr_t stock = 0;
        std::uintptr_t twin = 0;
        int kind = -1; // its kind in shader_twin_blobs::k_twins
        std::uint64_t mask = 0;
        std::array<char, 64> name{}; // the stock's resource name, for the log
        bool recovered = false;      // a twin a DEV hot reload recovered from the engine's cache
    };

    /**
     * @brief Resolves the dry-run's anchors (Feature::TwinCloneDryRun) and the factory's lock; logs whether it runs.
     * @note Setup only, on the init thread, after resolve_all_anchors().
     */
    void initialize_twin_clone() noexcept;

    /** @brief True once initialize_twin_clone() found everything the dry-run reads through. */
    [[nodiscard]] bool dry_run_available() noexcept;

    /** @brief True for a kind of twin the clone can derive (one with a clone splice). */
    [[nodiscard]] bool clone_capable(int kind) noexcept;

    /**
     * @brief Queues the pairs the dry-run has not compared yet this session, and starts a run when none is under way.
     * @param occasion Where the pairs came from, for the run's log lines (copied).
     * @return How many pairs were queued.
     * @note Main thread.
     */
    [[nodiscard]] int queue_dry_run(std::span<const DryRunPair> pairs, std::string_view occasion) noexcept;

    /** @brief True while queued pairs wait for a step. Main thread. */
    [[nodiscard]] bool dry_run_pending() noexcept;

    /**
     * @brief One step of the dry-run: plans and compares queued pairs until @p budget_us has passed or @p max_pairs
     *        were handled (0 for no pair limit), checked between pairs. Logs each pair, the step, and, once the queue
     *        is empty, the run's summary and each kind's clone-eligible verdict for the session.
     * @param where The loading boundary or pause this step runs in, for the log.
     * @note Main thread, only inside a loading boundary after its twins were created or in a frame the game stands
     *       still in with nothing left to create; never in unpaused gameplay. No mod mutex may be held.
     */
    void run_dry_run(std::uint64_t budget_us, int max_pairs, std::string_view where) noexcept;

} // namespace TPVCamera::TwinClone

#endif // TPVCAMERA_HOOKS_TWIN_CLONE_HPP
