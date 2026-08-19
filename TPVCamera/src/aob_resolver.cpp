/**
 * @file aob_resolver.cpp
 * @brief Declarative anchor table, one-pass resolution, and the resolved-address store.
 *
 * The candidate ladders in aob_resolver.hpp enter a DetourModKit anchor registry as RipGlobal
 * entries. resolve_all_anchors() resolves the whole table in a single parallel pass at startup and
 * records each address; anchor_address() hands the resolved address, or 0 on a cascade miss, to the
 * call sites.
 */

#include "aob_resolver.hpp"

#include "dmk_aliases.hpp"

#include <DetourModKit.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace TPVCamera
{
    namespace
    {

        using DMK::anchor::Anchor;
        using DMK::anchor::AnchorKind;
        using DMK::scan::Pages;

        constexpr std::size_t k_anchor_count = static_cast<std::size_t>(AnchorId::Count);

        // The WHGame.dll image the table resolves against, filled by resolve_all_anchors() before the sweep and
        // handed to every anchor's validator as its opaque context. Its address is a constant expression, so the
        // const table below can name it in a designated initializer.
        Region s_image_range{};

        /**
         * @brief Post-resolve validator: the resolved value must land inside the game image.
         * @details The scan SCOPE constrains where a candidate's bytes are FOUND, not where the value it decodes
         *          POINTS. A RipGlobal candidate resolves an absolute address from a disp32, and a Direct
         *          candidate applies a signed walk-back, so either can compute a target outside WHGame.dll from a
         *          freak match. Every anchor in this table names something inside the game image (a function
         *          entry, a callable helper, or a data slot the image owns), so a target outside it is proof the
         *          match was wrong. Returning false resets the value to 0 and reports Failed, exactly like a
         *          backend miss, so the consumer degrades instead of hooking or reading a bogus address.
         * @param value The resolved address.
         * @param context The image Region, forwarded verbatim from Anchor::validator_context.
         */
        [[nodiscard]] bool anchor_target_in_image(std::int64_t value, const void *context) noexcept
        {
            const auto *image = static_cast<const Region *>(context);
            if (image == nullptr || image->size == 0 || value <= 0)
            {
                return false;
            }
            return image->contains(Address{static_cast<std::uintptr_t>(value)});
        }

        // The registry, indexed by AnchorId. The enumerator order IS this order. Every target is code: a
        // function entry, a callable helper, or the instruction whose disp32 names a data slot.
        // Pages::Executable narrows each sweep to code pages so a byte signature that must land on an
        // instruction cannot alias an identical run in .rdata or .data.
        const Anchor k_anchors[] = {
            {
                .label = "GlobalContextPtr",
                .kind = AnchorKind::RipGlobal,
                .site = Aob::k_contextCandidates,
                .validator = anchor_target_in_image,
                .validator_context = &s_image_range,
                .pages = Pages::Executable,
            },
            {
                .label = "Genv",
                .kind = AnchorKind::RipGlobal,
                .site = Aob::k_genvCandidates,
                .validator = anchor_target_in_image,
                .validator_context = &s_image_range,
                .pages = Pages::Executable,
            },
            {
                .label = "CameraFrustumBuild",
                .kind = AnchorKind::RipGlobal,
                .site = Aob::k_frustumCandidates,
                .validator = anchor_target_in_image,
                .validator_context = &s_image_range,
                .pages = Pages::Executable,
            },
            {
                .label = "SetHeadVisibility",
                .kind = AnchorKind::RipGlobal,
                .site = Aob::k_headVisibilityCandidates,
                .validator = anchor_target_in_image,
                .validator_context = &s_image_range,
                .pages = Pages::Executable,
            },
            {
                .label = "CameraInputDispatch",
                .kind = AnchorKind::RipGlobal,
                .site = Aob::k_inputDispatchCandidates,
                .validator = anchor_target_in_image,
                .validator_context = &s_image_range,
                .pages = Pages::Executable,
            },
            {
                .label = "PlayerOnActionDispatch",
                .kind = AnchorKind::RipGlobal,
                .site = Aob::k_actionDispatchCandidates,
                .validator = anchor_target_in_image,
                .validator_context = &s_image_range,
                .pages = Pages::Executable,
            },
            {
                .label = "RayWorldIntersection",
                .kind = AnchorKind::RipGlobal,
                .site = Aob::k_rayWorldIntersectionCandidates,
                .validator = anchor_target_in_image,
                .validator_context = &s_image_range,
                .pages = Pages::Executable,
            },
            {
                .label = "InteractionRayBuild",
                .kind = AnchorKind::RipGlobal,
                .site = Aob::k_interactionRayBuildCandidates,
                .validator = anchor_target_in_image,
                .validator_context = &s_image_range,
                .pages = Pages::Executable,
            },
            {
                .label = "InteractorLookRay",
                .kind = AnchorKind::RipGlobal,
                .site = Aob::k_interactorLookRayCandidates,
                .validator = anchor_target_in_image,
                .validator_context = &s_image_range,
                .pages = Pages::Executable,
            },
            {
                .label = "InteractionOnScreenCheck",
                .kind = AnchorKind::RipGlobal,
                .site = Aob::k_interactionOnScreenCandidates,
                .validator = anchor_target_in_image,
                .validator_context = &s_image_range,
                .pages = Pages::Executable,
            },
            {
                .label = "HideOverlays",
                .kind = AnchorKind::RipGlobal,
                .site = Aob::k_overlayHideCandidates,
                .validator = anchor_target_in_image,
                .validator_context = &s_image_range,
                .pages = Pages::Executable,
            },
            {
                .label = "ShowOverlays",
                .kind = AnchorKind::RipGlobal,
                .site = Aob::k_overlayShowCandidates,
                .validator = anchor_target_in_image,
                .validator_context = &s_image_range,
                .pages = Pages::Executable,
            },
            {
                .label = "MenuOpen",
                .kind = AnchorKind::RipGlobal,
                .site = Aob::k_menuOpenCandidates,
                .validator = anchor_target_in_image,
                .validator_context = &s_image_range,
                .pages = Pages::Executable,
            },
            {
                .label = "MenuClose",
                .kind = AnchorKind::RipGlobal,
                .site = Aob::k_menuCloseCandidates,
                .validator = anchor_target_in_image,
                .validator_context = &s_image_range,
                .pages = Pages::Executable,
            },
            {
                .label = "GetObjectsInBox",
                .kind = AnchorKind::RipGlobal,
                .site = Aob::k_getObjectsInBoxCandidates,
                .validator = anchor_target_in_image,
                .validator_context = &s_image_range,
                .pages = Pages::Executable,
            },
        };
        static_assert(std::size(k_anchors) == k_anchor_count, "k_anchors must hold one entry per AnchorId.");

        // Resolved absolute addresses, indexed by AnchorId; 0 means unresolved. Zero-initialized (constant
        // init, no static-init-order hazard). Written once by resolve_all_anchors() on the init thread before
        // any consumer reads, then read-only, so no synchronization is required.
        std::array<std::uintptr_t, k_anchor_count> s_resolved_addresses{};

        // The per-anchor report, retained with the same write-once-then-read-only discipline so the shutdown
        // diagnostics snapshot can roll it up through diagnostics::collect() instead of the mod recomputing it.
        std::array<DMK::anchor::ResolvedAnchor, k_anchor_count> s_report{};
        std::size_t s_report_count = 0;

    } // namespace

    namespace
    {
        /**
         * @brief Grades every candidate pattern in the table and reports the weak ones.
         * @details sighealth is offline and side-effect-free: it reads the COMPILED pattern bytes and mask and
         *          scores atom rarity, byte entropy, and expected ambiguity in a nominal module. It touches no
         *          process memory and never gates resolution, so this runs before the sweep and only reports.
         *          Its value is on patch day: a cascade that stops resolving against a new WHGame.dll is usually
         *          a signature that was already weakly selective, and this line says which rung was, without a
         *          disassembler. RTTI and string-xref candidates carry no byte pattern and are skipped.
         * @param anchors The declarative table.
         */
        void report_signature_health(std::span<const Anchor> anchors)
        {
            DMK::Logger &logger = DMK::log();
            std::size_t fragile = 0;
            std::size_t unusable = 0;

            for (const Anchor &entry : anchors)
            {
                for (const DMK::scan::Candidate &candidate : entry.site)
                {
                    const DMK::scan::Pattern *pattern = nullptr;
                    if (const auto *direct = candidate.as_direct())
                    {
                        pattern = &direct->pattern;
                    }
                    else if (const auto *rip = candidate.as_rip_relative())
                    {
                        pattern = &rip->pattern;
                    }
                    if (pattern == nullptr)
                    {
                        continue;
                    }

                    const DMK::sighealth::PatternHealth health = DMK::sighealth::analyze_pattern(*pattern);
                    if (health.grade == DMK::sighealth::Grade::Robust)
                    {
                        logger.trace("Signature health: {}/{} Robust", entry.label, candidate.name());
                        continue;
                    }
                    (health.grade == DMK::sighealth::Grade::Unusable ? ++unusable : ++fragile);
                    logger.debug("Signature health: {}/{} {} -- {}", entry.label, candidate.name(),
                                 DMK::sighealth::to_string(health.grade),
                                 DMK::sighealth::format_report(health, candidate.name()));
                }
            }

            if (unusable > 0)
            {
                logger.warning("Signature health: {} candidate(s) grade Unusable and {} Fragile; re-author them "
                               "before the next game patch (details at Debug level)",
                               unusable, fragile);
            }
            else
            {
                logger.info("Signature health: {} fragile candidate(s), 0 unusable", fragile);
            }
        }
    } // namespace

    void resolve_all_anchors(std::uintptr_t module_base, std::size_t module_size)
    {
        DMK::Logger &logger = DMK::log();

        // Confine resolution to the WHGame.dll image. The DMK default Region::host() is the host EXE, not
        // WHGame.dll, so the region is built explicitly from the scanned base/size. The same region is published
        // to the per-anchor validators, which reject a resolved target outside the image.
        const DMK::Region range{DMK::Address{module_base}, module_size};
        s_image_range = range;

        // Offline signature grading first: it needs no game memory and says which rungs are structurally weak
        // BEFORE the sweep reports which ones missed, so the two lines read together on a patch-day log.
        report_signature_health(k_anchors);

        s_report_count = DMK::anchor::resolve_all_parallel(k_anchors, s_report, range);
        const std::size_t resolved_count = s_report_count;

        // resolve_all_parallel writes s_report[i] for k_anchors[i], so the report index is the AnchorId.
        for (std::size_t i = 0; i < resolved_count; ++i)
        {
            const DMK::anchor::ResolvedAnchor &entry = s_report[i];
            if (entry.status == DMK::anchor::AnchorStatus::Resolved)
            {
                s_resolved_addresses[i] = static_cast<std::uintptr_t>(entry.value);
                // Per-anchor address is for RE / external tooling, not routine status, so keep it at Debug; the
                // one-line quality summary below is the default-level health check, and a failure still warns.
                logger.debug("Anchor {} -> {}", entry.label, DMK::format::format_address(s_resolved_addresses[i]));
            }
            else
            {
                s_resolved_addresses[i] = 0;
                logger.warning("Anchor {} unresolved ({})", entry.label,
                               DMK::anchor::anchor_status_to_string(entry.status));
            }
        }

        const DMK::anchor::AnchorQuality quality = DMK::anchor::assess_quality(anchor_report());
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

    std::span<const DMK::anchor::ResolvedAnchor> anchor_report() noexcept
    {
        return std::span<const DMK::anchor::ResolvedAnchor>(s_report.data(), s_report_count);
    }

} // namespace TPVCamera
