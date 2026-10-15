/**
 * @file dev/hook_profile.hpp
 * @brief Per-thread hook timing aggregates for DEV profiling builds, exported after hook rundown.
 *
 * @details Each measuring thread claims one fixed slot on its first record and owns it alone, so a record takes no
 *          lock, allocates nothing and logs nothing. The export reads every slot, so it must run only after every
 *          measured hook has drained. Outside a build with both TPVCAMERA_DEV_BUILD and DMK_ENABLE_PROFILING the
 *          same API compiles to empty inline stubs.
 */
#ifndef TPVCAMERA_DEV_HOOK_PROFILE_HPP
#define TPVCAMERA_DEV_HOOK_PROFILE_HPP

#include <cstdint>

namespace TPVCamera::HookProfile
{
    /** @brief A measured hook path, or a synchronous engine call made from one. The export names each one. */
    enum class Path : std::uint8_t
    {
        AddRenderObject,
        PerViewUpload,
        CreatePsos,
        EngineAddStock,
        EngineAddTwin,
        EnginePsoStock,
        EnginePsoTwin,
        EnginePsoPassthrough,
        OtherFrustum,
        EngineOtherFrustum,
        EnginePsoTwinForward,
        GameFrustum,
        EngineGameFrustum,
        CloneDryRun,
        GameFrustumRepeat,
        EngineGameFrustumRepeat,
        Count
    };

    /** @brief An event counted without timing. The export names each one. */
    enum class Counter : std::uint8_t
    {
        /// An occluder scan that kept fewer render nodes than its box held.
        ScanTruncated,
        /// An occluder scan that queried the render octree instead of reusing the cached scan.
        ScanFull,
        /// An occluder scan that queried only the entity proxies and movable brushes again, keeping its static part.
        ScanDynamic,
        /// A draw of a listed item whose shader has no twin yet, which asked the creation queue again.
        UntwinnedRetry,
        /// A character twin PSO build (fade-ahead or warming) that took longer than the 2 ms per-frame budget.
        CharBuildOverBudget,
        Count
    };

    /**
     * @brief How a measured call ended. The meaning is path-specific, so the aliases share values.
     * @details AddRenderObject: stock or twin. PerViewUpload: skipped, written, in use or fault. CreatePsos:
     *          passthrough, guarded, resting, fading, warming or shadow. CloneDryRun: exact, allowed, mismatch or not
     *          compared. An engine call: success or failure.
     */
    enum class Outcome : std::uint8_t
    {
        Default,
        Twin,
        Resting,
        Fading,
        Warming,
        Shadow,
        Written = Twin,
        InUse = Resting,
        Fault = Fading,
        Guarded = Twin,
        Allowed = Twin,
        Mismatch = Resting,
        NotCompared = Fading
    };

#if defined(TPVCAMERA_DEV_BUILD) && defined(DMK_ENABLE_PROFILING)
    /**
     * @brief Reads the counter frequency and commits the fixed per-thread storage.
     * @note Setup only, before any measured hook is published. Idempotent.
     */
    void initialize() noexcept;

    /**
     * @brief Records one frustum build of a camera other than the game view.
     * @param start QPC ticks when the detour started.
     * @param original QPC ticks when the original engine call started.
     * @param end QPC ticks when the original returned.
     */
    void record_other_frustum(std::int64_t start, std::int64_t original, std::int64_t end) noexcept;

    /**
     * @brief Records one frustum build of the game view.
     * @param start QPC ticks when the detour started.
     * @param original QPC ticks when the original engine call started.
     * @param end QPC ticks when the original returned.
     * @param repeat True for the frame's second build of the game view, which records under its own paths.
     */
    void record_game_frustum(std::int64_t start, std::int64_t original, std::int64_t end, bool repeat) noexcept;

    /// Counts one event of @p counter. Callable from any thread; no lock, allocation or log.
    void count(Counter counter) noexcept;

    /// QPC ticks now, for count_slower_than().
    [[nodiscard]] std::int64_t stamp() noexcept;

    /// Counts one event of @p counter when more than @p limit_us microseconds passed since @p started (a stamp()).
    void count_slower_than(Counter counter, std::int64_t started, std::int64_t limit_us) noexcept;

    /**
     * @brief Writes every slot's totals, per-second windows and slow calls to @p path as JSON.
     * @return False when the file cannot be written or the export fails to allocate.
     * @note Only after successful hook rundown: the export reads the slots without synchronization.
     */
    [[nodiscard]] bool export_after_rundown(const char *path) noexcept;

    /**
     * @brief Times one hook call from construction to destruction and records it under its path.
     * @details Synchronous engine calls made inside it (EngineScope) are subtracted from its own time. No allocation
     *          and no logging on the hot path.
     */
    class Scope
    {
    public:
        /// Starts timing a call of @p path.
        explicit Scope(Path path) noexcept;
        /// Records the call with its outcome.
        ~Scope() noexcept;
        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;
        Scope(Scope &&) = delete;
        Scope &operator=(Scope &&) = delete;
        /// Sets the outcome recorded with the call.
        void outcome(Outcome value) noexcept { m_outcome = value; }

    private:
        friend class EngineScope;
        std::int64_t m_start;
        std::int64_t m_engine_ticks = 0;
        Path m_path;
        Outcome m_outcome = Outcome::Default;
    };

    /** @brief Times one original engine call and subtracts its elapsed time from the enclosing Scope's own time. */
    class EngineScope
    {
    public:
        /// Starts timing an engine call of @p path made inside @p owner.
        EngineScope(Path path, Scope &owner) noexcept;
        /// Records the call and charges its time to the owner.
        ~EngineScope() noexcept;
        EngineScope(const EngineScope &) = delete;
        EngineScope &operator=(const EngineScope &) = delete;
        EngineScope(EngineScope &&) = delete;
        EngineScope &operator=(EngineScope &&) = delete;
        /// Sets whether the engine call succeeded.
        void result(bool success) noexcept { m_success = success; }

    private:
        Scope &m_owner;
        std::int64_t m_start;
        Path m_path;
        bool m_success = true;
    };
#else
    // The same API, compiled to nothing outside DEV profiling builds.
    inline void initialize() noexcept {}
    inline void record_other_frustum(std::int64_t, std::int64_t, std::int64_t) noexcept {}
    inline void record_game_frustum(std::int64_t, std::int64_t, std::int64_t, bool) noexcept {}
    inline void count(Counter) noexcept {}
    [[nodiscard]] inline std::int64_t stamp() noexcept
    {
        return 0;
    }
    inline void count_slower_than(Counter, std::int64_t, std::int64_t) noexcept {}
    [[nodiscard]] inline bool export_after_rundown(const char *) noexcept
    {
        return true;
    }
    struct Scope
    {
        explicit Scope(Path) noexcept {}
        void outcome(Outcome) noexcept {}
    };
    struct EngineScope
    {
        EngineScope(Path, Scope &) noexcept {}
        void result(bool) noexcept {}
    };
#endif
} // namespace TPVCamera::HookProfile

#endif // TPVCAMERA_DEV_HOOK_PROFILE_HPP
