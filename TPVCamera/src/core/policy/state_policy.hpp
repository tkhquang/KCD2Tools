/**
 * @file core/policy/state_policy.hpp
 * @brief What each game situation asks of the camera, built from the five [StateBehavior] lists: one row per situation.
 *
 * @details The INI keeps its five lists (ForcedFPVState, ForcedTPVState, OrbitExcludeState, NativeTurnExcludeState,
 *          SuppressTPVState). The camera reads them once per frame into this table, and every policy asks the table, so
 *          the rule that ties the lists together lives in one place: EnableStateBehavior gates the three edge-triggered
 *          lists (the forced views and the free-look exclusion), never the SuppressTPVState gate or the native turn
 *          exclusion, and first person wins where both views are forced. Engine-free (no Windows or DetourModKit
 *          header), so the local tests compile it as is.
 */
#ifndef TPVCAMERA_CORE_POLICY_STATE_POLICY_HPP
#define TPVCAMERA_CORE_POLICY_STATE_POLICY_HPP

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace TPVCamera
{
    /** @brief The view a situation switches to as it starts. */
    enum class ViewForce : std::uint8_t
    {
        None,
        FirstPerson,
        ThirdPerson,
    };

    /** @brief What one situation asks of the camera. */
    struct StatePolicyRow
    {
        ViewForce view = ViewForce::None; // switched to once on entry (ForcedFPVState / ForcedTPVState)
        bool orbit_exclude = false;       // free-look turned off on entry, back on at exit (OrbitExcludeState)
        bool native_turn_exclude = false; // the native turn animation off (NativeTurnExcludeState)
        bool suppress = false;            // third person locked off while it lasts (SuppressTPVState)
    };

    /** @brief The five lists as the INI holds them (GameState bit masks), and EnableStateBehavior. */
    struct StatePolicyLists
    {
        std::uint32_t forced_fpv = 0;
        std::uint32_t forced_tpv = 0;
        std::uint32_t orbit_exclude = 0;
        std::uint32_t native_turn_exclude = 0;
        std::uint32_t suppress = 0;
        bool edge_rules = true; // EnableStateBehavior
    };

    /** @brief One row per GameState bit, and each policy's mask over all of them. */
    class StatePolicyTable
    {
    public:
        static constexpr std::size_t k_rows = 32;

        StatePolicyTable() noexcept = default;

        explicit StatePolicyTable(const StatePolicyLists &lists) noexcept : m_edge_rules(lists.edge_rules)
        {
            for (std::size_t bit = 0; bit < k_rows; ++bit)
            {
                const std::uint32_t flag = 1u << bit;
                StatePolicyRow &row = m_rows[bit];
                if (lists.edge_rules)
                {
                    row.view = (lists.forced_fpv & flag) != 0   ? ViewForce::FirstPerson
                               : (lists.forced_tpv & flag) != 0 ? ViewForce::ThirdPerson
                                                                : ViewForce::None;
                    row.orbit_exclude = (lists.orbit_exclude & flag) != 0;
                }
                row.native_turn_exclude = (lists.native_turn_exclude & flag) != 0;
                row.suppress = (lists.suppress & flag) != 0;
                m_first_person |= row.view == ViewForce::FirstPerson ? flag : 0u;
                m_third_person |= row.view == ViewForce::ThirdPerson ? flag : 0u;
                m_orbit_exclude |= row.orbit_exclude ? flag : 0u;
                m_native_turn_exclude |= row.native_turn_exclude ? flag : 0u;
                m_suppress |= row.suppress ? flag : 0u;
            }
        }

        /** @brief The row of the situation with GameState bit @p flag (a single bit), or a plain row for none. */
        [[nodiscard]] StatePolicyRow row(std::uint32_t flag) const noexcept
        {
            return std::has_single_bit(flag) ? m_rows[static_cast<std::size_t>(std::countr_zero(flag))]
                                             : StatePolicyRow{};
        }

        /**
         * @brief The view the situations in @p state force: first person when any forces it, else third person when
         *        any forces that, else none.
         */
        [[nodiscard]] ViewForce view(std::uint32_t state) const noexcept
        {
            return (state & m_first_person) != 0   ? ViewForce::FirstPerson
                   : (state & m_third_person) != 0 ? ViewForce::ThirdPerson
                                                   : ViewForce::None;
        }

        /** @brief The situations that force @p view (none for ViewForce::None). */
        [[nodiscard]] std::uint32_t forcing(ViewForce view) const noexcept
        {
            return view == ViewForce::FirstPerson   ? m_first_person
                   : view == ViewForce::ThirdPerson ? m_third_person
                                                    : 0u;
        }

        /** @brief Whether any situation forces a view, so the policy must watch every frame for an entry. */
        [[nodiscard]] bool forces_any_view() const noexcept { return (m_first_person | m_third_person) != 0; }

        /** @brief EnableStateBehavior: the edge-triggered rules (the forced views, the free-look exclusion) run. */
        [[nodiscard]] bool edge_rules() const noexcept { return m_edge_rules; }

        [[nodiscard]] std::uint32_t orbit_exclude() const noexcept { return m_orbit_exclude; }
        [[nodiscard]] std::uint32_t native_turn_exclude() const noexcept { return m_native_turn_exclude; }
        [[nodiscard]] std::uint32_t suppress() const noexcept { return m_suppress; }

    private:
        std::array<StatePolicyRow, k_rows> m_rows{};
        std::uint32_t m_first_person = 0;
        std::uint32_t m_third_person = 0;
        std::uint32_t m_orbit_exclude = 0;
        std::uint32_t m_native_turn_exclude = 0;
        std::uint32_t m_suppress = 0;
        bool m_edge_rules = true;
    };
} // namespace TPVCamera

#endif // TPVCAMERA_CORE_POLICY_STATE_POLICY_HPP
