/**
 * @file dev/see_through_trace.hpp
 * @brief CPU-side event trace of the tree see-through for DEV profiling builds, exported after hook rundown.
 *
 * @details Each recording thread claims one fixed slot on its first event and owns it alone: two bounded rings
 *          (activity and lifecycle events) and a PSO descriptor table, with no hot-path allocation, formatting, disk
 *          I/O or lock. The export reads every slot, so it must run only after every hook that records has drained.
 *          Outside a build with both TPVCAMERA_DEV_BUILD and DMK_ENABLE_PROFILING the same API compiles to empty
 *          inline stubs, and enabled is false so callers can drop the work that feeds it.
 */
#ifndef TPVCAMERA_DEV_SEE_THROUGH_TRACE_HPP
#define TPVCAMERA_DEV_SEE_THROUGH_TRACE_HPP

#include <array>
#include <cstdint>
#include <span>

namespace TPVCamera::SeeThroughTrace
{
    /** @brief A traced event. The recorded value is its index, which the export maps to its name. */
    enum class Event : std::uint8_t
    {
        CandidateBegin,
        CandidateEnd,
        CandidateCancelled,
        CameraWindow,
        UpdateSkipped,
        TrackAdded,
        TrackRejected,
        TrackReleased,
        SourceRequest,
        SourceResult,
        SourceState,
        ItemStock,
        ItemTwin,
        SubmitTwinReturned,
        BuildBegin,
        CompiledBinding,
        BuildEnd,
        BuildDeferred,
        PsoSelectedTwin,
        PsoSelectedStock,
        RecordQueued,
        RecordLost,
        RecordReceived,
        RecordReplaced,
        RecordDropped,
        RecordState,
        RetryScheduled,
        DeferredScheduled,
        RefillBegin,
        RefillEnd,
        SetPublished,
        RowsPublished,
        RowsDisabled,
        RowsWrite,
        PsoSlotState,
        Count
    };

    /**
     * @brief Why a shader item was drawn with its stock shader, recorded with Event::ItemStock.
     * @details The values are the recorded codes the analysis scripts read, so they are fixed: a new reason is
     *          appended. A twin draw records the twin instead of a reason, so code 0 is never recorded.
     */
    enum class ItemReason : std::uint8_t
    {
        InvalidItem = 1,
        StaleSet,
        UnsupportedShader,
        Opacity,
        TwinUnavailable,
        SourceTableFull,
        AwaitingLoading,
        BindingExpired,
        AwaitingPause
    };

    /** @brief Why a track's render objects were refilled, recorded with Event::RefillBegin. */
    enum class RefillReason : std::uint8_t
    {
        Activate,
        SourceReady,
        Deferred,
        LostRecord,
        PsosReady,
        Release
    };

    /**
     * @brief A copied pipeline-state description, the key of the PSO discovery table.
     * @details Scalar copies only: the recorder never owns or later dereferences these native identities.
     */
    struct PsoDescriptor
    {
        /// The native 56-byte description, normalized to the stock shader.
        std::array<std::uint64_t, 7> words{};
        std::uintptr_t pipeline = 0, cache = 0;
        std::uint64_t load_epoch = 0;
        std::uint32_t kind = 0;
        bool operator==(const PsoDescriptor &) const = default;
    };

#if defined(TPVCAMERA_DEV_BUILD) && defined(DMK_ENABLE_PROFILING)
    /// True in a build that records the trace.
    inline constexpr bool enabled = true;

    /**
     * @brief Reads the counter frequency and commits the fixed per-thread storage.
     * @note Setup only, before any recording hook is published. Idempotent.
     */
    void initialize() noexcept;

    /**
     * @brief Appends @p event to the calling thread's activity or lifecycle ring.
     * @param frame The shader twins' frame sequence number.
     * @param node,object,shader,resources The native identities the event concerns, or 0.
     * @param a,b Event-specific values.
     */
    void emit(Event event, std::uint32_t frame, std::uintptr_t node = 0, std::uintptr_t object = 0,
              std::uintptr_t shader = 0, std::uintptr_t resources = 0, std::uint64_t a = 0,
              std::uint64_t b = 0) noexcept;

    /**
     * @brief Records the window's candidate plants and frames for one camera frame, with begin and end events for each
     *        candidate episode.
     * @details The candidates are the broad phase, not visible pixels.
     * @note The shader twins' main-thread update only, under its main mutex: the candidate table is not synchronized.
     */
    void candidates(std::uint32_t frame, std::span<void *const> trees, std::span<void *const> frames, float strength,
                    float near_strength) noexcept;

    /**
     * @brief Ends every open candidate episode with Event::CandidateCancelled.
     * @note The shader twins' main-thread update only: the candidate table is not synchronized.
     */
    void cancel_candidates(std::uint32_t frame) noexcept;

    /**
     * @brief Keeps a time-stamped mesh name for @p node, for the export. An unchanged name is not stored again.
     * @note The shader twins' main-thread update only: the label table is not synchronized.
     */
    void label(std::uintptr_t node, const char *name) noexcept;

    /**
     * @brief Counts one stock build or twin demand of the descriptor @p descriptor in the calling thread's table.
     * @param node The persistent render node the PSOs belong to, or 0.
     * @param compiled The compiled object the build is for.
     * @param ready_twin The ready twin PSO for this descriptor, or 0.
     * @param twin_demand Whether the call asked for the twin rather than built the stock PSO.
     */
    void observe_pso_descriptor(const PsoDescriptor &descriptor, std::uintptr_t node, std::uintptr_t compiled,
                                std::uintptr_t ready_twin, bool twin_demand) noexcept;

    /// Counts one descriptor that faulted while it was copied.
    void pso_descriptor_fault() noexcept;

    /**
     * @brief Writes every slot's events, totals, PSO descriptors and the labels to @p path as JSON.
     * @return False when the file cannot be written or the export fails to allocate.
     * @note Only after successful hook rundown: the export reads the slots without synchronization.
     */
    [[nodiscard]] bool export_after_rundown(const char *path) noexcept;
#else
    // The same API, compiled to nothing outside DEV profiling builds.
    inline constexpr bool enabled = false;
    inline void initialize() noexcept {}
    inline void emit(Event, std::uint32_t, std::uintptr_t = 0, std::uintptr_t = 0, std::uintptr_t = 0,
                     std::uintptr_t = 0, std::uint64_t = 0, std::uint64_t = 0) noexcept
    {
    }
    inline void candidates(std::uint32_t, std::span<void *const>, std::span<void *const>, float, float) noexcept {}
    inline void cancel_candidates(std::uint32_t) noexcept {}
    inline void label(std::uintptr_t, const char *) noexcept {}
    inline void observe_pso_descriptor(const PsoDescriptor &, std::uintptr_t, std::uintptr_t, std::uintptr_t,
                                       bool) noexcept
    {
    }
    inline void pso_descriptor_fault() noexcept {}
    [[nodiscard]] inline bool export_after_rundown(const char *) noexcept
    {
        return true;
    }
#endif
} // namespace TPVCamera::SeeThroughTrace

#endif // TPVCAMERA_DEV_SEE_THROUGH_TRACE_HPP
