/**
 * @file aob_resolver.cpp
 * @brief Declarative anchor table, one-pass resolution, and the resolved-address store.
 *
 * The cascade candidate tables in aob_resolver.hpp are wrapped one-to-one as RipGlobal entries in a
 * DetourModKit anchor registry. resolve_all_anchors() resolves the whole table in a single parallel
 * pass at startup and records each address; anchor_address() hands the resolved address (or 0 on a
 * cascade miss) to the call sites.
 */

#include "aob_resolver.hpp"

#include <DetourModKit.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace TPVCamera
{
    namespace
    {

        using DMK::Anchors::Anchor;
        using DMK::Anchors::AnchorKind;

        constexpr std::size_t k_anchor_count = static_cast<std::size_t>(AnchorId::Count);

        /// Builds a RipGlobal anchor over a cascade candidate array (the candidates select Direct vs RIP).
        constexpr Anchor rip_global(std::string_view label, std::span<const AddrCandidate> site) noexcept
        {
            return Anchor{.label = label, .kind = AnchorKind::RipGlobal, .site = site};
        }

        // The declarative anchor table, indexed by AnchorId. Each entry wraps one cascade candidate array from
        // aob_resolver.hpp as a RipGlobal anchor. Anchors::resolve runs Scanner::resolve_cascade_in_module, and the
        // candidates' own ResolveMode selects Direct (function entry) vs RipRelative (global data slot), so a
        // resolved address comes straight from its matching cascade. The registry resolves the whole table in one
        // parallel pass and yields a unified quality report plus a foundation for validators / fingerprints. Each
        // anchor's label is also its log label.
        constexpr std::array<Anchor, k_anchor_count> k_anchor_table = {{
            rip_global("GlobalContextPtr", Aob::k_contextCandidates),
            rip_global("Genv", Aob::k_genvCandidates),
            rip_global("CameraFrustumBuild", Aob::k_frustumCandidates),
            rip_global("SetHeadVisibility", Aob::k_headVisibilityCandidates),
            rip_global("CameraInputDispatch", Aob::k_inputDispatchCandidates),
            rip_global("PlayerOnActionDispatch", Aob::k_actionDispatchCandidates),
            rip_global("RayWorldIntersection", Aob::k_rayWorldIntersectionCandidates),
            rip_global("InteractionRayBuild", Aob::k_interactionRayBuildCandidates),
            rip_global("InteractorLookRay", Aob::k_interactorLookRayCandidates),
            rip_global("InteractionOnScreenCheck", Aob::k_interactionOnScreenCandidates),
            rip_global("HideOverlays", Aob::k_overlayHideCandidates),
            rip_global("ShowOverlays", Aob::k_overlayShowCandidates),
            rip_global("MenuOpen", Aob::k_menuOpenCandidates),
            rip_global("MenuClose", Aob::k_menuCloseCandidates),
            rip_global("GetObjectsInBox", Aob::k_getObjectsInBoxCandidates),
        }};

        // Resolved absolute addresses, indexed by AnchorId; 0 means unresolved. Zero-initialized (constant
        // init, no static-init-order hazard). Written once by resolve_all_anchors() on the init thread before
        // any consumer reads, then read-only, so no synchronization is required.
        std::array<std::uintptr_t, k_anchor_count> s_resolved_addresses{};

    } // namespace

    void resolve_all_anchors(std::uintptr_t module_base, std::size_t module_size)
    {
        DMK::Logger &logger = DMK::Logger::get_instance();

        // Confine resolution to the WHGame.dll image. The DMK default host_module_range() is the host EXE,
        // not WHGame.dll, so the range is passed explicitly.
        const DMK::Memory::ModuleRange range{module_base, module_base + module_size};

        std::array<DMK::Anchors::ResolvedAnchor, k_anchor_count> report{};
        const std::size_t resolved_count = DMK::Anchors::resolve_all_parallel(k_anchor_table, report, range);

        // resolve_all_parallel writes report[i] for k_anchor_table[i], so the report index is the AnchorId.
        for (std::size_t i = 0; i < resolved_count; ++i)
        {
            const DMK::Anchors::ResolvedAnchor &entry = report[i];
            if (entry.status == DMK::Anchors::AnchorStatus::Resolved)
            {
                s_resolved_addresses[i] = static_cast<std::uintptr_t>(entry.value);
                // Per-anchor address is for RE / external tooling, not routine status, so keep it at Debug; the
                // one-line quality summary below is the default-level health check, and a failure still warns.
                logger.debug("Anchor {} -> {}", entry.label, DMK::Format::format_address(s_resolved_addresses[i]));
            }
            else
            {
                s_resolved_addresses[i] = 0;
                logger.warning("Anchor {} unresolved ({})", entry.label,
                               DMK::Anchors::anchor_status_to_string(entry.status));
            }
        }

        const DMK::Anchors::AnchorQuality quality =
            DMK::Anchors::assess_quality(std::span<const DMK::Anchors::ResolvedAnchor>(report.data(), resolved_count));
        logger.info("Anchor resolution: {}/{} resolved, {} failed, {} unsupported", quality.resolved, quality.total,
                    quality.failed, quality.unsupported);
    }

    std::uintptr_t anchor_address(AnchorId id) noexcept
    {
        const std::size_t index = static_cast<std::size_t>(id);
        if (index >= k_anchor_count)
        {
            return 0;
        }
        return s_resolved_addresses[index];
    }

} // namespace TPVCamera
