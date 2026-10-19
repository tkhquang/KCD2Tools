/**
 * @file anchors.cpp
 * @brief The anchor registry: candidate ladders, validators, vtable identities, runtime exports and the feature gates.
 *
 * Each function resolves from a ladder of byte signatures. DMK anchor::resolve takes the first rung that matches
 * exactly once, inside WHGame.dll and on executable pages only, and the result must pass a validator: a function entry
 * on an executable page that agrees with its unwind entry. gEnv is the root of every other read, so it resolves as a
 * 2-of-3 quorum of independent load sites.
 *
 * Signature rules: every relative target, RIP displacement and virtual-call slot is a wildcard, every branch inside a
 * pattern is a bounded gap ([2-6] covers the rel8 and rel32 forms), and every rung matches exactly once, at the same
 * address, on the Steam 1.5.6, GOG 1.5 and Game Pass builds. Each function ladder holds
 * rungs that start past the first 14 bytes, so another mod's inline hook on the entry (Henry's Senses and TPVCamera
 * both hook CCryAction::PostUpdate) cannot hide every rung.
 */

#include "anchors.hpp"
#include "constants.hpp"

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace HotPak
{
    namespace
    {
        using DMK::anchor::Anchor;
        using DMK::anchor::AnchorKind;
        using DMK::anchor::AnchorStatus;
        using DMK::anchor::GateVerdict;
        using DMK::anchor::ResolvedAnchor;
        using DMK::scan::Candidate;
        using DMK::scan::Pages;
        using DMK::scan::Pattern;

        // gEnv: three loads of the inline SSystemGlobalEnvironment from unrelated functions.
        const Candidate GENV[] = {
            Candidate::rip_relative(
                "Genv_P1_MovCallLea",
                Pattern::literal("48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 50 ?? 48 8D 94 24 F0 05 00 00"),
                3,
                7
            ),
            Candidate::rip_relative(
                "Genv_P2_MovCallTest",
                Pattern::literal("48 8B C8 FF D2 | 48 8B 0D ?? ?? ?? ?? 48 85 C9 [2-6] 48 8B 01 33 D2"),
                3,
                7
            ),
            Candidate::rip_relative(
                "Genv_P3_LeaStructInit",
                Pattern::literal("48 8D 05 ?? ?? ?? ?? 48 89 0F 4C 8D 67 ??"),
                3,
                7
            ),
        };

        // CCryAction::PostUpdate(bool haveFocus, unsigned updateFlags). P1 is the entry, which opens with the
        // editor-only flag test. P2 (+0x1A) and P3 (+0x2B) start in the body.
        const Candidate POST_UPDATE[] = {
            Candidate::direct(
                "PostUpdate_P1_EditorOnlyTestPrologue",
                Pattern::literal("41 F6 C0 ?? [2-6] 48 8B C4 48 89 58 08 48 89 70 10")
            ),
            Candidate::direct(
                "PostUpdate_P2_BodyMovapsPushMov",
                Pattern::literal("57 41 57 48 8B EC 48 81 EC ?? ?? ?? ?? 0F 29 70 D8"),
                -0x1A
            ),
            Candidate::direct(
                "PostUpdate_P3_AiPhysicsTestGameCheck",
                Pattern::literal("48 8B F1 41 F6 C0 ?? [2-6] 48 8B 89 ?? ?? ?? ?? 48 8B 01"),
                -0x2B
            ),
        };

        // C_ComponentAttachmentManager's proxy pass: it stores the manager's activity word at +0x38, then calls the
        // per-proxy streaming update on each entry of the {+0x20 array, +0x28 count} pair. P1 is the entry, P2 (+0x17)
        // and P3 (+0x33) start in the body.
        const Candidate PROXY_MANAGER_UPDATE[] = {
            Candidate::direct(
                "ProxyManagerUpdate_P1_PrologueCallStore38",
                Pattern::literal(
                    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B F9 E8 ?? ?? ?? ?? 89 47 ?? 48 8B 47 ?? "
                    "48 8B 5F ??"
                )
            ),
            Candidate::direct(
                "ProxyManagerUpdate_P2_BodyArrayEndLea",
                Pattern::literal("89 47 ?? 48 8B 47 ?? 48 8B 5F ?? 48 8D 34 C3 48 85 C0 [2-6] 48 8B F3 48 3B DE"),
                -0x17
            ),
            Candidate::direct(
                "ProxyManagerUpdate_P3_BodyLoopCallVcall",
                Pattern::literal(
                    "48 8B 0B E8 ?? ?? ?? ?? 48 83 C3 08 [2-5] 83 7F ?? 00 [2-6] 48 8B 4F ?? 33 D2 48 8B 01 FF 90 "
                    "?? ?? ?? ??"
                ),
                -0x33
            ),
        };

        /// What an anchor names, which decides the validator it must pass.
        enum class Role : std::uint8_t
        {
            /// A function entry the tool hooks.
            Code,
            /// A global the tool reads through.
            Data,
        };

        /** @brief What every validator checks against: the game image. */
        struct ValidatorContext
        {
            DMK::Region image{};
        };

        ValidatorContext s_validator_context{};

        /** @brief True when @p value is an address inside the game image. */
        [[nodiscard]] bool in_image(std::int64_t value, const void *context) noexcept
        {
            const auto *validation = static_cast<const ValidatorContext *>(context);
            return validation != nullptr && validation->image.size != 0 && value > 0 &&
                   validation->image.contains(DMK::Address{static_cast<std::uintptr_t>(value)});
        }

        /** @brief True when @p address lies on a committed executable page (setup time only: one VirtualQuery). */
        [[nodiscard]] bool on_executable_page(std::uintptr_t address) noexcept
        {
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) == 0)
            {
                return false;
            }
            constexpr DWORD executable =
                PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
            return info.State == MEM_COMMIT && (info.Protect & PAGE_GUARD) == 0 && (info.Protect & executable) != 0;
        }

        /**
         * @brief Validator of a code anchor: a function entry in the image, on an executable page, that looks like a
         *        prologue and agrees with the unwind entry covering it.
         * @details A body rung's fixed walk-back can turn stale while its bytes still match. The unwind entry's
         *          recorded start then differs, and the anchor fails closed. A prologue another mod already patched
         *          into a jump still passes the prologue probe.
         */
        [[nodiscard]] bool code_target_valid(std::int64_t value, const void *context) noexcept
        {
            if (!in_image(value, context))
            {
                return false;
            }
            const auto address = static_cast<std::uintptr_t>(value);
            if (!on_executable_page(address) || !DMK::scan::is_likely_function_prologue(DMK::Address{address}))
            {
                return false;
            }
            DWORD64 image_base = 0;
            const RUNTIME_FUNCTION *entry = RtlLookupFunctionEntry(static_cast<DWORD64>(address), &image_base, nullptr);
            return entry == nullptr || image_base + entry->BeginAddress == static_cast<DWORD64>(address);
        }

        /** @brief Validator of a data anchor: a readable address in the image that is not code. */
        [[nodiscard]] bool data_target_valid(std::int64_t value, const void *context) noexcept
        {
            const auto address = static_cast<std::uintptr_t>(value);
            return in_image(value, context) &&
                   DMK::memory::is_readable(DMK::Region{DMK::Address{address}, sizeof(std::uintptr_t)}) &&
                   !on_executable_page(address);
        }

        [[nodiscard]] DMK::anchor::AnchorValidator validator_for(Role role) noexcept
        {
            return role == Role::Code ? code_target_valid : data_target_valid;
        }

        /** @brief One ladder-backed row. Every rung is an instruction, so the scan stays on executable pages. */
        [[nodiscard]] Anchor make_anchor(std::string_view label, std::span<const Candidate> site, Role role) noexcept
        {
            return Anchor{
                .label = label,
                .kind = AnchorKind::RipGlobal,
                .site = site,
                .validator = validator_for(role),
                .validator_context = &s_validator_context,
                .require_validator = true,
                .pages = Pages::Executable,
            };
        }

        /** @brief One RTTI-backed row: the primary vtable of @p mangled. */
        [[nodiscard]] constexpr Anchor make_vtable(std::string_view label, std::string_view mangled) noexcept
        {
            return Anchor{
                .label = label,
                .kind = AnchorKind::VtableIdentity,
                .mangled = mangled,
            };
        }

        /** @brief One export-table row: @p name exported by the C++ runtime the game links. */
        [[nodiscard]] constexpr Anchor make_runtime_export(std::string_view label, std::string_view name) noexcept
        {
            return Anchor{
                .label = label,
                .kind = AnchorKind::ExportName,
                .export_module = constants::CPP_RUNTIME_MODULE,
                .export_name = name,
            };
        }

        /** @brief One quorum member per rung of @p ladder, so every rung is an independent vote. */
        template <std::size_t N>
        [[nodiscard]] std::array<Anchor, N>
        single_rung_votes(std::string_view label, const Candidate (&ladder)[N], Role role) noexcept
        {
            std::array<Anchor, N> votes{};
            for (std::size_t i = 0; i < N; ++i)
            {
                votes[i] = make_anchor(label, std::span<const Candidate>{&ladder[i], 1}, role);
            }
            return votes;
        }

        /** @brief The addresses of @p anchors, the form Anchor::quorum_members takes. */
        template <std::size_t N>
        [[nodiscard]] std::array<const Anchor *, N> pointers_to(const std::array<Anchor, N> &anchors) noexcept
        {
            std::array<const Anchor *, N> pointers{};
            for (std::size_t i = 0; i < N; ++i)
            {
                pointers[i] = &anchors[i];
            }
            return pointers;
        }

        /**
         * @brief A 2-of-N quorum row over @p members.
         * @details A coincidental match after a game patch then has to fool two independent sites at once.
         */
        [[nodiscard]] Anchor
        make_quorum(std::string_view label, std::span<const Anchor *const> members, Role role) noexcept
        {
            return Anchor{
                .label = label,
                .kind = AnchorKind::Quorum,
                .validator = validator_for(role),
                .validator_context = &s_validator_context,
                .quorum_members = members,
                .quorum_threshold = 2,
            };
        }

        const auto GENV_VOTES = single_rung_votes("Genv", GENV, Role::Data);
        const auto GENV_VOTE_POINTERS = pointers_to(GENV_VOTES);

        /** @brief The registry rows, in ANCHORS order. */
        enum class AnchorId : std::uint8_t
        {
            Genv,
            PostUpdate,
            ProxyManagerUpdate,
            ScriptSystemVtable,
            CryPakVtable,
            RendererVtable,
            CharacterManagerVtable,
            SkinVtable,
            AttachmentSkinVtable,
            ClothingProxyVtable,
            SkinElementVtable,
            MaterialInfoVtable,
            MaterialManagerVtable,
            MaterialInstanceVtable,
            DatabaseManagerVtable,
            MutexLock,
            MutexUnlock,
        };

        const Anchor ANCHORS[] = {
            make_quorum("Genv", GENV_VOTE_POINTERS, Role::Data),
            make_anchor("PostUpdate", POST_UPDATE, Role::Code),
            make_anchor("ProxyManagerUpdate", PROXY_MANAGER_UPDATE, Role::Code),
            make_vtable("ScriptSystemVtable", constants::RTTI_SCRIPT_SYSTEM),
            make_vtable("CryPakVtable", constants::RTTI_CRYPAK),
            make_vtable("RendererVtable", constants::RTTI_RENDERER),
            make_vtable("CharacterManagerVtable", constants::RTTI_CHARACTER_MANAGER),
            make_vtable("SkinVtable", constants::RTTI_SKIN),
            make_vtable("AttachmentSkinVtable", constants::RTTI_ATTACHMENT_SKIN),
            make_vtable("ClothingProxyVtable", constants::RTTI_CLOTHING_PROXY),
            make_vtable("SkinElementVtable", constants::RTTI_SKIN_ELEMENT),
            make_vtable("MaterialInfoVtable", constants::RTTI_MATERIAL_INFO),
            make_vtable("MaterialManagerVtable", constants::RTTI_MATERIAL_MANAGER),
            make_vtable("MaterialInstanceVtable", constants::RTTI_MATERIAL_INSTANCE),
            make_vtable("DatabaseManagerVtable", constants::RTTI_DATABASE_MANAGER),
            make_runtime_export("MutexLock", constants::MUTEX_LOCK_EXPORT),
            make_runtime_export("MutexUnlock", constants::MUTEX_UNLOCK_EXPORT),
        };
        constexpr std::size_t ANCHOR_COUNT = std::size(ANCHORS);
        static_assert(static_cast<std::size_t>(AnchorId::MutexUnlock) + 1 == ANCHOR_COUNT);

        [[nodiscard]] constexpr std::size_t index_of(AnchorId id) noexcept
        {
            return static_cast<std::size_t>(id);
        }

        /** @brief The features, in FEATURES order. */
        enum class Feature : std::uint8_t
        {
            Core,
            Scripts,
            Renderer,
            Skins,
            Materials,
            Tables,
            ProxyResets,
        };

        /** @brief A feature and the anchors it needs. It turns on when anchor::evaluate_gate passes over them. */
        struct FeatureSpec
        {
            std::string_view name;
            std::span<const AnchorId> anchors;
        };

        constexpr AnchorId CORE_ANCHORS[] = {
            AnchorId::Genv,
            AnchorId::CryPakVtable,
            AnchorId::PostUpdate,
        };
        constexpr AnchorId SCRIPT_ANCHORS[] = {AnchorId::ScriptSystemVtable};
        constexpr AnchorId RENDERER_ANCHORS[] = {AnchorId::RendererVtable};
        constexpr AnchorId SKIN_ANCHORS[] = {
            AnchorId::CharacterManagerVtable,
            AnchorId::SkinVtable,
            AnchorId::MutexLock,
            AnchorId::MutexUnlock,
        };
        constexpr AnchorId MATERIAL_ANCHORS[] = {
            AnchorId::MaterialInfoVtable,
            AnchorId::MaterialManagerVtable,
        };
        constexpr AnchorId TABLE_ANCHORS[] = {AnchorId::DatabaseManagerVtable};
        constexpr AnchorId PROXY_RESET_ANCHORS[] = {
            AnchorId::ProxyManagerUpdate,
            AnchorId::ClothingProxyVtable,
            AnchorId::SkinElementVtable,
            AnchorId::AttachmentSkinVtable,
            AnchorId::MaterialInstanceVtable,
        };

        constexpr FeatureSpec FEATURES[] = {
            {"Core", CORE_ANCHORS},
            {"Scripts", SCRIPT_ANCHORS},
            {"Renderer", RENDERER_ANCHORS},
            {"Skins", SKIN_ANCHORS},
            {"Materials", MATERIAL_ANCHORS},
            {"Tables", TABLE_ANCHORS},
            {"ProxyResets", PROXY_RESET_ANCHORS},
        };
        constexpr std::size_t FEATURE_COUNT = std::size(FEATURES);
        static_assert(static_cast<std::size_t>(Feature::ProxyResets) + 1 == FEATURE_COUNT);
        constexpr std::size_t MAX_FEATURE_ANCHORS = std::size(PROXY_RESET_ANCHORS);

        /** @brief Logs one report entry. An unresolved entry keeps a zero value. */
        void log_entry(const ResolvedAnchor &entry, DMK::Region game)
        {
            if (entry.status != AnchorStatus::Resolved)
            {
                DMK::log().warning(
                    "Anchor {} unresolved ({})",
                    entry.label,
                    DMK::anchor::anchor_status_to_string(entry.status)
                );
                return;
            }
            const auto address = static_cast<std::uintptr_t>(entry.value);
            if (game.contains(DMK::Address{address}))
            {
                DMK::log().info(
                    "Anchor {} at {} (RVA {:#x})",
                    entry.label,
                    DMK::format::format_address(address),
                    address - game.base.raw()
                );
            }
            else
            {
                DMK::log().info("Anchor {} at {}", entry.label, DMK::format::format_address(address));
            }
        }
    } // namespace

    Anchors resolve_anchors(DMK::Region game)
    {
        std::array<ResolvedAnchor, ANCHOR_COUNT> report{};
        s_validator_context = ValidatorContext{
            .image = game,
        };
        (void)DMK::anchor::resolve_all(ANCHORS, report, game);
        for (const ResolvedAnchor &entry : report)
        {
            log_entry(entry, game);
        }

        std::array<bool, FEATURE_COUNT> passed{};
        for (std::size_t i = 0; i < FEATURE_COUNT; ++i)
        {
            const FeatureSpec &feature = FEATURES[i];
            std::array<ResolvedAnchor, MAX_FEATURE_ANCHORS> entries{};
            std::size_t count = 0;
            for (const AnchorId id : feature.anchors)
            {
                entries[count++] = report[index_of(id)];
            }
            const GateVerdict verdict = DMK::anchor::evaluate_gate(std::span{entries.data(), count});
            passed[i] = verdict != GateVerdict::Fail;
            if (verdict == GateVerdict::Fail)
            {
                DMK::log().warning("Feature gate: {} Fail; the feature is off", feature.name);
            }
            else
            {
                DMK::log()
                    .info("Feature gate: {} {}", feature.name, verdict == GateVerdict::Pass ? "Pass" : "Degraded");
            }
        }

        // A member of a feature whose gate failed stays 0, so every use of it fails closed.
        const auto gated = [&report, &passed](Feature feature, AnchorId id) -> std::uintptr_t
        {
            const ResolvedAnchor &entry = report[index_of(id)];
            return passed[static_cast<std::size_t>(feature)] && entry.status == AnchorStatus::Resolved
                       ? static_cast<std::uintptr_t>(entry.value)
                       : 0;
        };
        const EngineAnchors engine{
            .script_system = gated(Feature::Scripts, AnchorId::ScriptSystemVtable),
            .cry_pak = gated(Feature::Core, AnchorId::CryPakVtable),
            .renderer = gated(Feature::Renderer, AnchorId::RendererVtable),
            .character_manager = gated(Feature::Skins, AnchorId::CharacterManagerVtable),
            .skin = gated(Feature::Skins, AnchorId::SkinVtable),
            .attachment_skin = gated(Feature::ProxyResets, AnchorId::AttachmentSkinVtable),
            .clothing_proxy = gated(Feature::ProxyResets, AnchorId::ClothingProxyVtable),
            .skin_element = gated(Feature::ProxyResets, AnchorId::SkinElementVtable),
            .material_info = gated(Feature::Materials, AnchorId::MaterialInfoVtable),
            .material_manager = gated(Feature::Materials, AnchorId::MaterialManagerVtable),
            .material_instance = gated(Feature::ProxyResets, AnchorId::MaterialInstanceVtable),
            .database_manager = gated(Feature::Tables, AnchorId::DatabaseManagerVtable),
            .mutex_lock = gated(Feature::Skins, AnchorId::MutexLock),
            .mutex_unlock = gated(Feature::Skins, AnchorId::MutexUnlock),
        };
        return Anchors{
            .genv = gated(Feature::Core, AnchorId::Genv),
            .post_update = gated(Feature::Core, AnchorId::PostUpdate),
            .proxy_manager_update = gated(Feature::ProxyResets, AnchorId::ProxyManagerUpdate),
            .engine = engine,
            .core = passed[static_cast<std::size_t>(Feature::Core)],
            .proxy_resets = passed[static_cast<std::size_t>(Feature::ProxyResets)],
        };
    }

} // namespace HotPak
