/**
 * @file query_ledger.hpp
 * @brief Counts and times every engine query the camera and the ranged aim make, by the caller that asked for it.
 *
 * @details A query is one physics call (RayWorldIntersection or PrimitiveWorldIntersection), one render-octree call
 *          (GetObjectsInBox) or one mesh test of a render node. Each is charged to a Tag naming why it ran and a Kind
 *          naming what it cost. The camera closes a frame once per engine frame, so the export also holds the
 *          per-frame distribution of physics queries for moving, still and idle frames. Records use relaxed atomics
 *          because the ranged launch trace queries from a job worker. Outside a build with both TPVCAMERA_DEV_BUILD
 *          and DMK_ENABLE_PROFILING the same API compiles to empty inline stubs, so a release build carries no
 *          ledger code.
 */
#ifndef TPVCAMERA_QUERY_LEDGER_HPP
#define TPVCAMERA_QUERY_LEDGER_HPP

#include <array>
#include <cstdint>

namespace TPVCamera::QueryLedger
{
    /** @brief Why a query ran. The export names each one (see tag_name()). */
    enum class Tag : std::uint8_t
    {
        Fan,
        SweepPwi,
        Lens,
        Inside,
        StructureOnArm,
        DrawnOn,
        Sightline,
        OccluderScan,
        Prefetch,
        FenceWall,
        Crosshair,
        AimPreview,
        LaunchTrace,
        Diagnostic,
        Untagged,
        Count
    };

    /** @brief What a query cost: a physics call, a render-octree call or a mesh test. */
    enum class Kind : std::uint8_t
    {
        Physics,
        Octree,
        Mesh,
        Count
    };

    /** @brief How the camera moved over a closed frame. Idle covers frames with no third-person camera. */
    enum class Motion : std::uint8_t
    {
        Moving,
        Still,
        Idle,
        Count
    };

    constexpr std::size_t TAG_COUNT = static_cast<std::size_t>(Tag::Count);
    constexpr std::size_t KIND_COUNT = static_cast<std::size_t>(Kind::Count);
    constexpr std::size_t MOTION_COUNT = static_cast<std::size_t>(Motion::Count);
    /// Bins of the per-frame physics query count; the last bin holds every frame at or above it.
    constexpr std::size_t FRAME_QUERY_BINS = 128;

    /** @brief The camera pose a frame rendered, compared with the previous frame's to tell Moving from Still. */
    struct ViewPose
    {
        std::array<float, 3> position{};
        std::array<float, 3> forward{};
    };

    /** @brief Totals of one Tag and Kind. */
    struct TagStats
    {
        std::uint64_t calls = 0;
        std::int64_t ticks = 0;
        std::int64_t max_ticks = 0;
    };

    /** @brief Everything the ledger holds, copied out for the export. */
    struct Snapshot
    {
        std::array<std::array<TagStats, KIND_COUNT>, TAG_COUNT> totals{};
        std::array<std::uint64_t, MOTION_COUNT> frames{};
        std::array<std::array<std::uint64_t, FRAME_QUERY_BINS>, MOTION_COUNT> physics_per_frame{};
        std::array<std::int64_t, MOTION_COUNT> max_physics_ticks_per_frame{};
    };

    /** @brief The export name of @p tag. */
    [[nodiscard]] constexpr const char *tag_name(Tag tag) noexcept
    {
        constexpr std::array<const char *, TAG_COUNT> k_names = {
            "fan",       "sweep_pwi",   "lens",          "inside",     "structure_on_arm",
            "drawn_on",  "sightline",   "occluder_scan", "prefetch",   "fence_wall",
            "crosshair", "aim_preview", "launch_trace",  "diagnostic", "untagged",
        };
        const auto index = static_cast<std::size_t>(tag);
        return index < k_names.size() ? k_names[index] : "?";
    }

    /** @brief The export name of @p kind. */
    [[nodiscard]] constexpr const char *kind_name(Kind kind) noexcept
    {
        constexpr std::array<const char *, KIND_COUNT> k_names = {
            "physics",
            "octree",
            "mesh",
        };
        const auto index = static_cast<std::size_t>(kind);
        return index < k_names.size() ? k_names[index] : "?";
    }

    /** @brief The export name of @p motion. */
    [[nodiscard]] constexpr const char *motion_name(Motion motion) noexcept
    {
        constexpr std::array<const char *, MOTION_COUNT> k_names = {
            "moving",
            "still",
            "idle",
        };
        const auto index = static_cast<std::size_t>(motion);
        return index < k_names.size() ? k_names[index] : "?";
    }

#if defined(TPVCAMERA_DEV_BUILD) && defined(DMK_ENABLE_PROFILING)
    /**
     * @brief Starts timing a query.
     * @return QPC ticks, handed to end().
     */
    [[nodiscard]] std::int64_t begin() noexcept;

    /**
     * @brief Records @p calls queries of @p tag and @p kind that took the time since @p started.
     * @details Begin and end are plain calls, not a scope object, so a guarded caller can bracket a query without an
     *          object to unwind inside its __try frame. Callable from any thread; no lock, allocation or log.
     */
    void end(Tag tag, Kind kind, std::int64_t started, std::uint32_t calls = 1) noexcept;

    /**
     * @brief Notes the pose the camera rendered on this frame, so close_frame() files the frame as Moving or Still.
     * @note Main thread, once per engine frame, from the frame's first build.
     */
    void note_view(const ViewPose &pose) noexcept;

    /**
     * @brief Files the queries since the previous call as one frame of the motion note_view() recorded, or Idle.
     * @note Main thread, at the first build of each engine frame.
     */
    void close_frame() noexcept;

    /**
     * @brief Copies the totals and the frame distributions.
     * @note Only after hook rundown, like HookProfile::export_after_rundown(): a concurrent record can tear a total.
     */
    [[nodiscard]] Snapshot snapshot() noexcept;
#else
    // The same API, compiled to nothing outside DEV profiling builds.
    [[nodiscard]] inline std::int64_t begin() noexcept
    {
        return 0;
    }
    inline void end(Tag, Kind, std::int64_t, std::uint32_t = 1) noexcept {}
    inline void note_view(const ViewPose &) noexcept {}
    inline void close_frame() noexcept {}
#endif
} // namespace TPVCamera::QueryLedger

#endif // TPVCAMERA_QUERY_LEDGER_HPP
