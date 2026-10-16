/**
 * @file core/occlusion/fence_coverage.hpp
 * @brief Bounded upper-body coverage policy for camera-blocking fences.
 * @details A fence model is probed in rows of horizontal lines across it, one row per height from k_first_height up in
 *          k_height_step steps. A row whose probes mostly meet the drawn mesh is solid. A fence is a wall when enough
 *          rows are solid, enough of them at the upper body, so broad panels with seams stay walls while low panels
 *          with tall posts and open rails do not.
 */
#ifndef TPVCAMERA_CORE_OCCLUSION_FENCE_COVERAGE_HPP
#define TPVCAMERA_CORE_OCCLUSION_FENCE_COVERAGE_HPP

#include <array>
#include <cstdint>

namespace TPVCamera::FenceCoverage
{
    /// The number of probe rows, one per height.
    constexpr int k_rows = 8;
    /// The number of probes in a row, spread across the fence's width.
    constexpr int k_columns = 9;
    /// The height of the lowest row above the model's origin, in meters.
    constexpr float k_first_height = 1.25f;
    /// The height between two rows, in meters.
    constexpr float k_height_step = 0.1f;
    /// The probes of a row that must meet the mesh for the row to count as solid.
    constexpr int k_solid_columns = 6;
    /// The solid rows a wall needs.
    constexpr int k_solid_rows = 5;
    /// The first row at the upper body.
    constexpr int k_upper_start = 4;
    /// The solid upper-body rows a wall needs.
    constexpr int k_upper_rows = 2;

    /**
     * @brief Where probe @p column of row @p row crosses the fence, as a fraction of its width (0..1), staggered within
     *        its cell so regularly spaced posts do not line up with every row.
     */
    [[nodiscard]] constexpr float width_fraction(int row, int column) noexcept
    {
        return (static_cast<float>(column) + 0.2f + 0.1f * static_cast<float>((column * 5 + row * 3) % 7)) /
               static_cast<float>(k_columns);
    }

    /** @brief What the rows measured so far say about a fence. */
    enum class Verdict : std::uint8_t
    {
        // The rows so far allow both answers. The camera treats a pending fence as a wall.
        Pending,
        // No sequence of the remaining rows can make it a wall.
        Open,
        Wall,
    };

    /**
     * @brief The coverage rows measured so far for one fence model, and the verdict they give.
     * @details Only complete rows count: missing geometry or an exhausted budget leaves the verdict pending.
     */
    struct Measurement
    {
        std::array<std::uint8_t, k_rows> hits{};
        int rows = 0;
        // The probes of the row in progress done so far, and how many of them met the mesh.
        int columns = 0;
        int row_hits = 0;

        /** @brief The verdict of the complete rows: decided as soon as the remaining rows cannot change it. */
        [[nodiscard]] constexpr Verdict verdict() const noexcept
        {
            int solid = 0;
            int upper = 0;
            for (int row = 0; row < rows; ++row)
            {
                if (hits[row] >= k_solid_columns)
                {
                    ++solid;
                    upper += row >= k_upper_start ? 1 : 0;
                }
            }
            if (solid >= k_solid_rows && upper >= k_upper_rows)
            {
                return Verdict::Wall;
            }
            const int remaining = k_rows - rows;
            const int remaining_upper = k_rows - (rows > k_upper_start ? rows : k_upper_start);
            return solid + remaining < k_solid_rows || upper + remaining_upper < k_upper_rows ? Verdict::Open
                                                                                              : Verdict::Pending;
        }

        /**
         * @brief Records a row in which @p count probes met the mesh. Ignored when the count is out of range or once
         *        the verdict is decided.
         */
        constexpr void observe(int count) noexcept
        {
            if (rows < k_rows && count >= 0 && count <= k_columns && verdict() == Verdict::Pending)
            {
                hits[rows++] = static_cast<std::uint8_t>(count);
            }
        }

        /**
         * @brief Records one finished probe of the row in progress, and the row once its last probe is in, so
         *        finished probes carry across frame budgets.
         * @note The caller reports only a probe that ran to the end: an interrupted one (a budget or deadline cut
         *       it short, or the mesh could not be read) contributes no evidence.
         */
        constexpr void observe_probe(bool hit) noexcept
        {
            if (verdict() != Verdict::Pending)
            {
                return;
            }
            row_hits += hit ? 1 : 0;
            if (++columns == k_columns)
            {
                observe(row_hits);
                columns = 0;
                row_hits = 0;
            }
        }
    };
} // namespace TPVCamera::FenceCoverage

#endif // TPVCAMERA_CORE_OCCLUSION_FENCE_COVERAGE_HPP
