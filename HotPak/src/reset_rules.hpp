/**
 * @file reset_rules.hpp
 * @brief Which clothing proxy the reset window resets, and why, from facts read off the proxy.
 *
 * A proxy needs a reset only while it still shows something a reload replaced. A character a save load creates during
 * the window loads its files from the overlay and builds everything new: its proxies show no replaced skin or
 * material, its manager is first seen after the window opened, and HotPak reset nothing on it. Only a proxy of it that
 * lands in Error is reset, once, to retry the load.
 */
#ifndef HOTPAK_RESET_RULES_HPP
#define HOTPAK_RESET_RULES_HPP

#include "constants.hpp"

#include <cstdint>
#include <optional>

namespace HotPak
{
    /** @brief Why a proxy was reset. */
    enum class ResetReason : std::uint8_t
    {
        /// It shows a CSkin that HotPak marked unshared: the skin file changed.
        ReplacedSkin,
        /// It renders with a material that HotPak renamed out of CMatMan: the .mtl changed.
        RetiredMaterial,
        /// It is in Error and asks for a file the pak ships: a broken build left it there.
        Error,
        /// A table it builds its material instance from was reloaded, and its character existed before.
        TableRows,
        /// It still waits for its swap well after HotPak reset another proxy of the same character.
        StuckSwap,
    };

    /** @brief What the manager pass read off one proxy and its character. */
    struct ProxyFacts
    {
        /// Its row asks for a skin or material the pak ships.
        bool shipped = false;
        /// Its row asks for a skin of a pak whose table reload restreams its wearers.
        bool restream = false;
        /// Its attachment shows a CSkin that HotPak marked unshared.
        bool replaced_skin = false;
        /// Its material instance is built on a material that HotPak renamed out of CMatMan.
        bool retired_material = false;
        /// E_StreamingState, or -1 when unreadable.
        int state = -1;
        /// Its character's manager ran a pass within the first moments of the window, so it existed before it.
        bool manager_early = false;
        /// Milliseconds since HotPak last reset a proxy of the same character in this window, if it did.
        std::optional<std::uint64_t> since_manager_reset_ms;
    };

    /// A reset proxy normally swaps in within a second. One that waits this long was dropped from the swap check.
    inline constexpr std::uint64_t STUCK_SWAP_MS = 4000;

    /** @brief The reason to reset a proxy with @p facts, or std::nullopt to leave it alone. */
    [[nodiscard]] constexpr std::optional<ResetReason> reset_reason(const ProxyFacts &facts) noexcept
    {
        if (facts.shipped && facts.replaced_skin)
        {
            return ResetReason::ReplacedSkin;
        }
        if (facts.shipped && facts.retired_material)
        {
            return ResetReason::RetiredMaterial;
        }
        if (facts.shipped && facts.state == constants::PROXY_STATE_ERROR)
        {
            return ResetReason::Error;
        }
        if (facts.restream && facts.manager_early)
        {
            return ResetReason::TableRows;
        }
        if (facts.shipped && facts.state == constants::PROXY_STATE_WAITING_FOR_SWAP &&
            facts.since_manager_reset_ms.has_value() && *facts.since_manager_reset_ms >= STUCK_SWAP_MS)
        {
            return ResetReason::StuckSwap;
        }
        return std::nullopt;
    }

    /** @brief A short name of @p reason for the log. */
    [[nodiscard]] constexpr const char *reset_reason_name(ResetReason reason) noexcept
    {
        switch (reason)
        {
        case ResetReason::ReplacedSkin:
            return "replaced skin";
        case ResetReason::RetiredMaterial:
            return "replaced material";
        case ResetReason::Error:
            return "recovery from Error";
        case ResetReason::TableRows:
            return "new table rows";
        case ResetReason::StuckSwap:
            return "stuck swap";
        }
        return "unknown";
    }

} // namespace HotPak

#endif // HOTPAK_RESET_RULES_HPP
