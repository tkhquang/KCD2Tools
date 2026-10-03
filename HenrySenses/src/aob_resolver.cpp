/**
 * @file aob_resolver.cpp
 * @brief The declarative anchor table, its signature-file overlay, two-phase resolution, the per-feature gates and
 *        the address store.
 */

#include "aob_resolver.hpp"
#include "config.hpp"
#include "constants.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace HenrySenses
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

        /// What an anchor names, which decides the validator it must pass.
        enum class Role : std::uint8_t
        {
            /// A function entry the mod hooks, calls, or compares a vtable slot against.
            Code,
            /// A global the mod reads through (a data slot or an inline struct).
            Data,
        };

        /**
         * @struct ValidatorContext
         * @brief The WHGame.dll range every validator checks against, filled before each phase.
         */
        struct ValidatorContext
        {
            DMK::Region image{};
        };

        ValidatorContext s_validator_context{};

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
         * @brief Validator of a code anchor: a function entry inside the image, on an executable page, whose first
         *        byte is not scan poison (zero fill, an INT3 pad or a bare RET).
         * @details The scan scope bounds where a candidate's bytes are FOUND, not where a walk-back or disp32 POINTS,
         *          so a freak match is caught here and fails the anchor closed. A body signature's fixed walk-back
         *          can become stale while its bytes still match. When unwind metadata covers the target, require its
         *          recorded function start to agree; a plausible opcode in the middle of an instruction is not an
         *          entry. Stackless leaf functions can lack unwind metadata and retain the byte probe as evidence.
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

        /** @brief Validator of a data anchor: an address inside the image that is not code. */
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

        /**
         * @brief Builds one ladder-backed table row.
         * @details Every rung is an instruction (a function body, or the load whose disp32 names a data slot), so
         *          Pages::Executable keeps a byte signature from aliasing an identical run in .rdata or .data. A row
         *          without its validator fails closed (require_validator).
         */
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

        /**
         * @brief Builds one quorum member per rung of a ladder, so every rung is an independent vote.
         */
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
         * @brief Builds a 2-of-N quorum row over @p members.
         * @details gEnv and the global context are the roots every other read goes through, so a coincidental match
         *          after a patch must fool two independent code sites at once ([B-52]).
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

        const auto GENV_VOTES = single_rung_votes("Genv", aob::GENV_CANDIDATES, Role::Data);
        const auto GENV_VOTE_POINTERS = pointers_to(GENV_VOTES);
        const auto CONTEXT_VOTES = single_rung_votes("Context", aob::CONTEXT_CANDIDATES, Role::Data);
        const auto CONTEXT_VOTE_POINTERS = pointers_to(CONTEXT_VOTES);

        // The registry, indexed by AnchorId. The enumerator order IS this order.
        const Anchor ANCHORS[] = {
            make_anchor("StdPipelineInit", aob::STD_PIPELINE_INIT_CANDIDATES, Role::Code),
            make_anchor("CustomCreatePso", aob::CUSTOM_CREATE_PSO_CANDIDATES, Role::Code),
            make_anchor("RegisterCustomStage", aob::REGISTER_CUSTOM_STAGE_CANDIDATES, Role::Code),
            make_quorum("Genv", GENV_VOTE_POINTERS, Role::Data),
            make_quorum("Context", CONTEXT_VOTE_POINTERS, Role::Data),
            make_anchor("AfterPostHdr", aob::AFTER_POST_HDR_CANDIDATES, Role::Code),
            make_anchor("ClearSurface", aob::CLEAR_SURFACE_CANDIDATES, Role::Code),
            make_anchor("PrepareRenderPass", aob::PREPARE_RENDER_PASS_CANDIDATES, Role::Code),
            make_anchor("DrawRenderItems", aob::DRAW_RENDER_ITEMS_CANDIDATES, Role::Code),
            make_anchor("JobifyDraws", aob::JOBIFY_DRAWS_CANDIDATES, Role::Code),
            make_anchor("WaitDraws", aob::WAIT_DRAWS_CANDIDATES, Role::Code),
            make_anchor("SetTechnique", aob::SET_TECHNIQUE_CANDIDATES, Role::Code),
            make_anchor("SetRenderTarget", aob::SET_RENDER_TARGET_CANDIDATES, Role::Code),
            make_anchor("SetTexture", aob::SET_TEXTURE_CANDIDATES, Role::Code),
            make_anchor("SetSampler", aob::SET_SAMPLER_CANDIDATES, Role::Code),
            make_anchor("BeginConstantUpdate", aob::BEGIN_CONSTANT_UPDATE_CANDIDATES, Role::Code),
            make_anchor("SetConstant", aob::SET_CONSTANT_CANDIDATES, Role::Code),
            make_anchor("FullscreenExecute", aob::FULLSCREEN_EXECUTE_CANDIDATES, Role::Code),
            make_anchor("CryNameR", aob::CRY_NAME_R_CANDIDATES, Role::Code),
            make_anchor("DisplayTargetDst", aob::DISPLAY_TARGET_DST_CANDIDATES, Role::Code),
            make_anchor("CoreCommandListSlot", aob::CORE_COMMAND_LIST_SLOT_CANDIDATES, Role::Data),
            make_anchor("RecursionCounter", aob::RECURSION_COUNTER_CANDIDATES, Role::Data),
            make_anchor("PostEffectsGameSlot", aob::POST_EFFECTS_GAME_SLOT_CANDIDATES, Role::Data),
            make_anchor("ProxyRender", aob::PROXY_RENDER_CANDIDATES, Role::Code),
            make_anchor("PostUpdate", aob::POST_UPDATE_CANDIDATES, Role::Code),
            make_anchor("RegisterEntity", aob::REGISTER_ENTITY_CANDIDATES, Role::Code),
            make_anchor("UnRegisterEntity", aob::UNREGISTER_ENTITY_CANDIDATES, Role::Code),
            make_anchor("GetEntity", aob::GET_ENTITY_CANDIDATES, Role::Code),
            make_anchor("GetEntityIterator", aob::GET_ENTITY_ITERATOR_CANDIDATES, Role::Code),
            make_anchor("GetProxy", aob::GET_PROXY_CANDIDATES, Role::Code),
            make_anchor("GetWorldBounds", aob::GET_WORLD_BOUNDS_CANDIDATES, Role::Code),
            make_anchor("GetAuxGeom", aob::GET_AUX_GEOM_CANDIDATES, Role::Code),
            make_anchor("AuxSetFlags", aob::AUX_SET_FLAGS_CANDIDATES, Role::Code),
            make_anchor("AuxGetFlags", aob::AUX_GET_FLAGS_CANDIDATES, Role::Code),
            make_anchor("AuxDrawLines", aob::AUX_DRAW_LINES_CANDIDATES, Role::Code),
            make_anchor("GetObjectsInBox", aob::GET_OBJECTS_IN_BOX_CANDIDATES, Role::Code),
            make_anchor("ObjManager", aob::OBJ_MANAGER_CANDIDATES, Role::Data),
            make_anchor("AfterPostLdr", aob::AFTER_POST_LDR_CANDIDATES, Role::Code),
            make_anchor("LdrTarget", aob::LDR_TARGET_CANDIDATES, Role::Code),
            make_anchor("StatObjRenderInternal", aob::RENDER_INTERNAL_CANDIDATES, Role::Code),
            make_anchor("StatObjRender", aob::STAT_OBJ_RENDER_CANDIDATES, Role::Code),
            make_anchor("BlurPassCtor", aob::BLUR_PASS_CTOR_CANDIDATES, Role::Code),
            make_anchor("BlurPassExecute", aob::BLUR_PASS_EXECUTE_CANDIDATES, Role::Code),
            make_anchor("SuperResolutionExecute", aob::SUPER_RESOLUTION_EXECUTE_CANDIDATES, Role::Code),
            make_anchor("StashFromEntity", aob::STASH_FROM_ENTITY_CANDIDATES, Role::Code),
            make_anchor("StashMasterInventory", aob::STASH_MASTER_INVENTORY_CANDIDATES, Role::Code),
            make_anchor("InventoryOwner", aob::INVENTORY_OWNER_CANDIDATES, Role::Code),
            make_anchor("PublicEnemyTag", aob::PUBLIC_ENEMY_TAG_CANDIDATES, Role::Data),
            make_anchor("ScriptContextMap", aob::SCRIPT_CONTEXT_MAP_CANDIDATES, Role::Data),
            make_anchor("ShaderForName", aob::SHADER_FOR_NAME_CANDIDATES, Role::Code),
            make_anchor("AdjustFileName", aob::ADJUST_FILE_NAME_CANDIDATES, Role::Code),
            make_anchor("FindEffect", aob::FIND_EFFECT_CANDIDATES, Role::Code),
            make_anchor("ProxyLoadParticleEmitter", aob::PROXY_LOAD_PARTICLE_EMITTER_CANDIDATES, Role::Code),
            make_anchor("ProxySetSlotLocalTM", aob::PROXY_SET_SLOT_LOCAL_TM_CANDIDATES, Role::Code),
            make_anchor("ProxyFreeSlot", aob::PROXY_FREE_SLOT_CANDIDATES, Role::Code),
            make_anchor("ParticleLoadLibrary", aob::PARTICLE_LOAD_LIBRARY_CANDIDATES, Role::Code),
            make_anchor("XmlLoadFromBuffer", aob::XML_LOAD_FROM_BUFFER_CANDIDATES, Role::Code),
            make_anchor("ManagerCreateEmitter", aob::MANAGER_CREATE_EMITTER_CANDIDATES, Role::Code),
            make_anchor("EmitterKill", aob::EMITTER_KILL_CANDIDATES, Role::Code),
            make_anchor("SceneSetRenderTargets", aob::SCENE_SET_RENDER_TARGETS_CANDIDATES, Role::Code),
            make_anchor("PostFxRenderTarget", aob::POST_FX_RENDER_TARGET_CANDIDATES, Role::Code),
            make_anchor("PostFxDepthStencil", aob::POST_FX_DEPTH_STENCIL_CANDIDATES, Role::Code),
            make_anchor("ClearDepth", aob::CLEAR_DEPTH_CANDIDATES, Role::Code),
        };
        static_assert(std::size(ANCHORS) == ANCHOR_COUNT, "ANCHORS must hold one entry per AnchorId.");
        static_assert(EARLY_ANCHOR_COUNT < ANCHOR_COUNT, "The early phase must leave a main phase.");

        // The anchors each feature depends on. A feature is enabled only through its gate over exactly these.
        constexpr AnchorId CORE_ANCHORS[] = {AnchorId::Genv};
        constexpr AnchorId GAME_STATE_ANCHORS[] = {AnchorId::Context};
        constexpr AnchorId ENTITY_LOOKUP_ANCHORS[] = {AnchorId::GetEntity, AnchorId::GetProxy};
        constexpr AnchorId ENTITY_BOUNDS_ANCHORS[] = {AnchorId::GetWorldBounds};
        constexpr AnchorId ENTITY_ITERATION_ANCHORS[] = {AnchorId::GetEntityIterator};
        constexpr AnchorId RENDER_REGISTRATION_ANCHORS[] = {AnchorId::RegisterEntity, AnchorId::UnRegisterEntity};
        constexpr AnchorId STASH_ANCHORS[] = {AnchorId::StashFromEntity, AnchorId::StashMasterInventory};
        constexpr AnchorId INVENTORY_OWNER_ANCHORS[] = {AnchorId::InventoryOwner};
        constexpr AnchorId PUBLIC_ENEMY_ANCHORS[] = {AnchorId::PublicEnemyTag};
        constexpr AnchorId SCRIPT_CONTEXT_ANCHORS[] = {AnchorId::ScriptContextMap};
        constexpr AnchorId OCTREE_ANCHORS[] = {AnchorId::GetObjectsInBox};
        constexpr AnchorId HERB_SCAN_ANCHORS[] = {AnchorId::GetObjectsInBox, AnchorId::ObjManager};
        constexpr AnchorId TICK_ANCHORS[] = {AnchorId::PostUpdate};
        constexpr AnchorId AUX_MARKER_ANCHORS[] = {
            AnchorId::GetAuxGeom,
            AnchorId::AuxSetFlags,
            AnchorId::AuxGetFlags,
            AnchorId::AuxDrawLines,
        };
        constexpr AnchorId STAGE_REGISTRATION_ANCHORS[] = {
            AnchorId::StdPipelineInit,
            AnchorId::CustomCreatePso,
            AnchorId::RegisterCustomStage,
        };
        constexpr AnchorId MASK_DRAW_ANCHORS[] = {
            AnchorId::ClearSurface,
            AnchorId::PrepareRenderPass,
            AnchorId::DrawRenderItems,
            AnchorId::JobifyDraws,
            AnchorId::WaitDraws,
            AnchorId::CoreCommandListSlot,
            AnchorId::RecursionCounter,
        };
        constexpr AnchorId COMPOSITE_ANCHORS[] = {
            AnchorId::SetTechnique,
            AnchorId::SetRenderTarget,
            AnchorId::SetTexture,
            AnchorId::SetSampler,
            AnchorId::BeginConstantUpdate,
            AnchorId::SetConstant,
            AnchorId::FullscreenExecute,
            AnchorId::CryNameR,
            AnchorId::DisplayTargetDst,
            AnchorId::PostEffectsGameSlot,
        };
        constexpr AnchorId PROXY_INJECTION_ANCHORS[] = {AnchorId::ProxyRender};
        constexpr AnchorId BRUSH_MARKING_ANCHORS[] = {AnchorId::StatObjRenderInternal};
        constexpr AnchorId AFTER_HDR_ANCHORS[] = {AnchorId::AfterPostHdr};
        constexpr AnchorId BEFORE_UPSCALE_ANCHORS[] = {AnchorId::SuperResolutionExecute};
        constexpr AnchorId AFTER_LDR_ANCHORS[] = {AnchorId::AfterPostLdr, AnchorId::LdrTarget};
        constexpr AnchorId MASK_BLUR_ANCHORS[] = {AnchorId::BlurPassCtor, AnchorId::BlurPassExecute};
        constexpr AnchorId HERB_OUTLINE_ANCHORS[] = {AnchorId::StatObjRender};
        constexpr AnchorId SILHOUETTE_SHADER_ANCHORS[] = {AnchorId::ShaderForName, AnchorId::AdjustFileName};
        constexpr AnchorId LOOT_EFFECTS_ANCHORS[] = {
            AnchorId::FindEffect,
            AnchorId::ProxyLoadParticleEmitter,
            AnchorId::ProxySetSlotLocalTM,
            AnchorId::ProxyFreeSlot,
        };
        constexpr AnchorId EFFECT_LIBRARY_ANCHORS[] = {AnchorId::ParticleLoadLibrary, AnchorId::XmlLoadFromBuffer};
        constexpr AnchorId WORLD_EFFECTS_ANCHORS[] = {
            AnchorId::FindEffect,
            AnchorId::ManagerCreateEmitter,
            AnchorId::EmitterKill,
        };
        constexpr AnchorId SUPERSAMPLED_MASK_ANCHORS[] = {
            AnchorId::SceneSetRenderTargets,
            AnchorId::PostFxRenderTarget,
            AnchorId::PostFxDepthStencil,
            AnchorId::ClearDepth,
        };

        /**
         * @struct FeatureSpec
         * @brief One feature gate: its log name and the anchors it depends on.
         */
        struct FeatureSpec
        {
            std::string_view name;
            std::span<const AnchorId> anchors;
        };

        // Indexed by Feature. The enumerator order IS this order.
        constexpr std::array<FeatureSpec, FEATURE_COUNT> FEATURES = {{
            {"Core", CORE_ANCHORS},
            {"GameState", GAME_STATE_ANCHORS},
            {"EntityLookup", ENTITY_LOOKUP_ANCHORS},
            {"EntityBounds", ENTITY_BOUNDS_ANCHORS},
            {"EntityIteration", ENTITY_ITERATION_ANCHORS},
            {"RenderRegistration", RENDER_REGISTRATION_ANCHORS},
            {"Stashes", STASH_ANCHORS},
            {"InventoryOwner", INVENTORY_OWNER_ANCHORS},
            {"PublicEnemy", PUBLIC_ENEMY_ANCHORS},
            {"ScriptContexts", SCRIPT_CONTEXT_ANCHORS},
            {"Octree", OCTREE_ANCHORS},
            {"HerbScan", HERB_SCAN_ANCHORS},
            {"Tick", TICK_ANCHORS},
            {"AuxMarkers", AUX_MARKER_ANCHORS},
            {"StageRegistration", STAGE_REGISTRATION_ANCHORS},
            {"MaskDraw", MASK_DRAW_ANCHORS},
            {"Composite", COMPOSITE_ANCHORS},
            {"ProxyInjection", PROXY_INJECTION_ANCHORS},
            {"BrushMarking", BRUSH_MARKING_ANCHORS},
            {"AfterHdr", AFTER_HDR_ANCHORS},
            {"BeforeUpscale", BEFORE_UPSCALE_ANCHORS},
            {"AfterLdr", AFTER_LDR_ANCHORS},
            {"MaskBlur", MASK_BLUR_ANCHORS},
            {"HerbOutline", HERB_OUTLINE_ANCHORS},
            {"SilhouetteShader", SILHOUETTE_SHADER_ANCHORS},
            {"LootEffects", LOOT_EFFECTS_ANCHORS},
            {"EffectLibrary", EFFECT_LIBRARY_ANCHORS},
            {"WorldEffects", WORLD_EFFECTS_ANCHORS},
            {"SupersampledMask", SUPERSAMPLED_MASK_ANCHORS},
        }};
        static_assert(!FEATURES[FEATURE_COUNT - 1].name.empty(), "FEATURES must hold one entry per Feature.");

        constexpr std::size_t MAX_FEATURE_ANCHORS = 16;

        // The hooked anchors: a repaired signature for one of them authorizes a code write.
        constexpr AnchorId HOOKED_ANCHORS[] = {
            AnchorId::StdPipelineInit,
            AnchorId::CustomCreatePso,
            AnchorId::AfterPostHdr,
            AnchorId::ProxyRender,
            AnchorId::PostUpdate,
            AnchorId::AfterPostLdr,
            AnchorId::StatObjRenderInternal,
            AnchorId::SuperResolutionExecute,
        };

        // The merged signatures: the in-code ladders with the signature file's repairs on top (manifest::overlay).
        std::vector<DMK::manifest::Signature> s_signatures;
        // The merged signature of each AnchorId. A composite (quorum) anchor has none: a signature file cannot
        // express one, so it resolves from its in-code row.
        std::array<std::optional<std::size_t>, ANCHOR_COUNT> s_signature_of{};
        // The anchors whose signature came from the file rather than the build.
        std::array<bool, ANCHOR_COUNT> s_repaired{};
        DMK::manifest::ManifestHeader s_file_header{};
        bool s_signatures_ready = false;

        // Resolved absolute addresses, indexed by AnchorId; 0 means unresolved. Each phase writes its own slice once
        // on the init thread before any consumer of that slice reads it, then the slice is read-only.
        std::array<std::uintptr_t, ANCHOR_COUNT> s_resolved_addresses{};

        // The per-anchor report (the drift report), retained with the same write-once discipline so the gates and
        // the shutdown diagnostics snapshot read it. Both phases write their slice at the anchor's own index.
        std::array<ResolvedAnchor, ANCHOR_COUNT> s_report{};
        std::size_t s_report_count = 0;

        // Each feature's verdict, published once its anchors' phase has run.
        std::array<std::atomic<GateVerdict>, FEATURE_COUNT> s_verdicts{};
        std::array<std::atomic<bool>, FEATURE_COUNT> s_verdict_ready{};

        [[nodiscard]] constexpr std::size_t index_of(AnchorId id) noexcept
        {
            return static_cast<std::size_t>(id);
        }

        [[nodiscard]] bool is_hooked(std::size_t index) noexcept
        {
            return std::any_of(
                std::begin(HOOKED_ANCHORS),
                std::end(HOOKED_ANCHORS),
                [index](AnchorId id) { return index_of(id) == index; }
            );
        }

        /** @brief The signature file beside the ASI. */
        [[nodiscard]] std::filesystem::path signature_file_path()
        {
            return std::filesystem::path(DMK::filesystem::get_runtime_directory()) /
                   (std::string(constants::MOD_NAME) + constants::SIGNATURE_FILE_SUFFIX);
        }

        /**
         * @brief Merges the in-code table with the signature file's repairs.
         * @details A missing file changes nothing. A file for another signature-contract revision is ignored with a
         *          warning, so a stale repair can never override a ladder this build corrected. A malformed entry falls
         *          back to its in-code default (manifest::overlay is fail-soft) and is named in the log.
         */
        void prepare_signatures()
        {
            if (s_signatures_ready)
            {
                return;
            }
            DMK::Logger &logger = DMK::log();

            std::vector<DMK::manifest::SignatureRecord> overrides;
            const std::filesystem::path path = signature_file_path();
            std::error_code exists_error;
            if (std::filesystem::exists(path, exists_error))
            {
                if (auto loaded = DMK::manifest::load(path); !loaded)
                {
                    logger.error(
                        "Signatures: {} could not be read ({}); the built-in signatures stand",
                        path.filename().string(),
                        loaded.error().message()
                    );
                }
                else if (!DMK::manifest::revision_compatible(loaded->header, constants::SIGNATURE_REVISION))
                {
                    logger.warning(
                        "Signatures: {} targets signature revision {}, this build is revision {}; the file is "
                        "ignored. Delete it, or set revision = {} only after re-verifying every entry",
                        path.filename().string(),
                        loaded->header.revision,
                        constants::SIGNATURE_REVISION,
                        constants::SIGNATURE_REVISION
                    );
                }
                else
                {
                    s_file_header = loaded->header;
                    overrides = std::move(loaded->records);
                    for (const DMK::manifest::SignatureRecord &record : overrides)
                    {
                        const auto compiled = DMK::manifest::Signature::compile(record);
                        logger.info(
                            "Signatures: [sig.{}] from {} {}",
                            record.label,
                            path.filename().string(),
                            compiled ? "is a repair candidate" : "is malformed; the built-in signature stands"
                        );
                    }
                }
            }

            // A composite (quorum) anchor has no file form, so it stays out of the overlay and resolves in code.
            std::vector<Anchor> serializable;
            serializable.reserve(ANCHOR_COUNT);
            for (const Anchor &row : ANCHORS)
            {
                if (row.kind != AnchorKind::Quorum)
                {
                    serializable.push_back(row);
                }
            }
            if (auto merged = DMK::manifest::overlay(serializable, overrides); merged)
            {
                s_signatures = std::move(*merged);
            }
            else
            {
                logger.error(
                    "Signatures: merging failed ({}); the built-in signatures stand",
                    merged.error().message()
                );
                s_signatures.clear();
            }

            for (std::size_t i = 0; i < ANCHOR_COUNT; ++i)
            {
                s_signature_of[i].reset();
                s_repaired[i] = false;
                for (std::size_t j = 0; j < s_signatures.size(); ++j)
                {
                    if (s_signatures[j].label() != ANCHORS[i].label)
                    {
                        continue;
                    }
                    s_signature_of[i] = j;
                    // The file won where its definition differs from the build's own.
                    if (auto builtin = DMK::manifest::Signature::adopt(ANCHORS[i]); builtin)
                    {
                        s_repaired[i] = builtin->current_fingerprint() != s_signatures[j].current_fingerprint();
                    }
                    if (s_repaired[i])
                    {
                        logger.warning(
                            "Signatures: {} uses the repair from {}",
                            ANCHORS[i].label,
                            path.filename().string()
                        );
                    }
                    break;
                }
            }
            s_signatures_ready = true;
        }

        /**
         * @brief Resolves one anchor: its merged signature, or the in-code row of a composite anchor.
         * @details A repair from the file passes the manifest trust gate before it is trusted. A repaired hook target
         *          authorizes a code write, so it needs the complete baseline set (GatePolicy::mutation_strict,
         *          [B-54]).
         */
        [[nodiscard]] ResolvedAnchor resolve_entry(std::size_t index, const DMK::Region &range)
        {
            const std::optional<std::size_t> signature_index = s_signature_of[index];
            if (!signature_index.has_value())
            {
                return DMK::anchor::resolve(ANCHORS[index], range);
            }
            const DMK::manifest::Signature &signature = s_signatures[*signature_index];
            ResolvedAnchor resolved = signature.resolve(range);
            if (!s_repaired[index] || resolved.status != AnchorStatus::Resolved)
            {
                return resolved;
            }
            const DMK::manifest::GatePolicy policy =
                is_hooked(index) ? DMK::manifest::GatePolicy::mutation_strict() : DMK::manifest::GatePolicy{};
            const DMK::manifest::GateResult gate = DMK::manifest::resolve_and_gate(
                std::span<const DMK::manifest::Signature>{&signature, 1},
                s_file_header,
                constants::SIGNATURE_REVISION,
                policy,
                range
            );
            if (gate.find(signature.label()) == nullptr)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Warning,
                    "Signatures: the repair of {} resolved but is not trusted ({}); it counts as unresolved",
                    ANCHORS[index].label,
                    gate.rejected.empty() ? std::string_view{"rejected"}
                                          : DMK::manifest::gate_reason_to_string(gate.rejected.front().reason)
                );
                resolved.status = AnchorStatus::Failed;
                resolved.value = 0;
            }
            return resolved;
        }

        /**
         * @brief Resolves a contiguous slice of the table into the report, on a transient worker pool.
         * @details Each entry is independent, and every validator is thread-safe, so the slice fans out the way
         *          anchor::resolve_all_parallel does.
         */
        void resolve_range(std::size_t first, std::size_t count, const DMK::Region &range)
        {
            std::atomic<std::size_t> next{0};
            auto work = [&]() noexcept
            {
                for (std::size_t i = next.fetch_add(1); i < count; i = next.fetch_add(1))
                {
                    const std::size_t index = first + i;
                    try
                    {
                        s_report[index] = resolve_entry(index, range);
                    }
                    catch (...)
                    {
                        s_report[index] = ResolvedAnchor{
                            .label = ANCHORS[index].label,
                            .kind = ANCHORS[index].kind,
                            .status = AnchorStatus::Failed,
                        };
                    }
                }
            };
            const std::size_t workers =
                std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1, std::min<std::size_t>(count, 8));
            std::vector<std::jthread> pool;
            try
            {
                for (std::size_t w = 1; w < workers; ++w)
                {
                    pool.emplace_back(work);
                }
            }
            catch (...)
            {
                // Fewer workers only slows the slice down; this thread resolves whatever is left.
            }
            work();
        }

        /**
         * @brief Grades every candidate pattern in a table slice and reports the weak ones.
         * @details sighealth scores the compiled pattern bytes without touching process memory and never gates
         *          resolution ([B-57]). On patch day, a cascade that stops resolving is usually one that was already
         *          weakly selective, and this line names that rung without a disassembler.
         */
        void report_signature_health(std::span<const Anchor> anchors)
        {
            DMK::Logger &logger = DMK::log();
            std::size_t fragile = 0;
            std::size_t unusable = 0;

            auto grade = [&](std::string_view label, const Candidate &candidate)
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
                    return;
                }
                const DMK::sighealth::PatternHealth health = DMK::sighealth::analyze_pattern(*pattern);
                if (health.grade == DMK::sighealth::Grade::Robust)
                {
                    logger.trace("Signature health: {}/{} Robust", label, candidate.name());
                    return;
                }
                (health.grade == DMK::sighealth::Grade::Unusable ? ++unusable : ++fragile);
                logger.debug(
                    "Signature health: {}/{} {} - {}",
                    label,
                    candidate.name(),
                    DMK::sighealth::to_string(health.grade),
                    DMK::sighealth::format_report(health, candidate.name())
                );
            };

            for (const Anchor &entry : anchors)
            {
                for (const Candidate &candidate : entry.site)
                {
                    grade(entry.label, candidate);
                }
                for (const Anchor *member : entry.quorum_members)
                {
                    if (member != nullptr)
                    {
                        for (const Candidate &candidate : member->site)
                        {
                            grade(entry.label, candidate);
                        }
                    }
                }
            }

            if (unusable > 0)
            {
                logger.warning(
                    "Signature health: {} candidate(s) grade Unusable and {} Fragile; re-author them "
                    "before the next game patch (details at Debug level)",
                    unusable,
                    fragile
                );
            }
            else
            {
                logger.info("Signature health: {} fragile candidate(s), 0 unusable", fragile);
            }
        }

        /** @brief Evaluates and publishes the gate of every feature whose anchors all lie below @p resolved_end. */
        void publish_gates(std::size_t resolved_end)
        {
            DMK::Logger &logger = DMK::log();
            for (std::size_t f = 0; f < FEATURE_COUNT; ++f)
            {
                const FeatureSpec &spec = FEATURES[f];
                if (s_verdict_ready[f].load(std::memory_order_acquire))
                {
                    continue;
                }
                const bool covered = std::all_of(
                    spec.anchors.begin(),
                    spec.anchors.end(),
                    [resolved_end](AnchorId id) { return index_of(id) < resolved_end; }
                );
                if (!covered)
                {
                    continue;
                }
                std::array<ResolvedAnchor, MAX_FEATURE_ANCHORS> entries{};
                std::size_t count = 0;
                for (const AnchorId id : spec.anchors)
                {
                    entries[count++] = s_report[index_of(id)];
                }
                const GateVerdict verdict = DMK::anchor::evaluate_gate(std::span{entries.data(), count});
                s_verdicts[f].store(verdict, std::memory_order_relaxed);
                s_verdict_ready[f].store(true, std::memory_order_release);
                switch (verdict)
                {
                case GateVerdict::Pass:
                    logger.info("Feature gate: {} Pass", spec.name);
                    break;
                case GateVerdict::Degraded:
                    logger.warning("Feature gate: {} Degraded; enabled", spec.name);
                    break;
                case GateVerdict::Fail:
                default:
                    logger.warning("Feature gate: {} Fail; the feature is off", spec.name);
                    break;
                }
            }
        }

        /**
         * @brief Resolves one contiguous slice of the table and records addresses and the report.
         */
        void resolve_slice(
            std::uintptr_t module_base,
            std::size_t module_size,
            std::size_t first,
            std::size_t count,
            std::string_view phase
        )
        {
            DMK::Logger &logger = DMK::log();

            // Confine resolution to the WHGame.dll image; the same region is published to the per-anchor validators.
            const DMK::Region range{DMK::Address{module_base}, module_size};
            s_validator_context.image = range;
            prepare_signatures();

            report_signature_health(std::span<const Anchor>{ANCHORS + first, count});
            resolve_range(first, count, range);

            std::size_t misses = 0;
            for (std::size_t i = 0; i < count; ++i)
            {
                const std::size_t index = first + i;
                if (s_report[index].status == AnchorStatus::Resolved)
                {
                    s_resolved_addresses[index] = static_cast<std::uintptr_t>(s_report[index].value);
                    logger.info(
                        "Anchor {} -> 0x{:016X} (RVA {:#x})",
                        s_report[index].label,
                        s_resolved_addresses[index],
                        s_resolved_addresses[index] - module_base
                    );
                    continue;
                }

                s_resolved_addresses[index] = 0;
                ++misses;
                logger.warning(
                    "Anchor {} unresolved ({})",
                    s_report[index].label,
                    DMK::anchor::anchor_status_to_string(s_report[index].status)
                );
            }

            if (first + count > s_report_count)
            {
                s_report_count = first + count;
            }
            logger.info(
                "Anchor resolution ({} phase): {}/{} by signature, {} missing",
                phase,
                count - misses,
                count,
                misses
            );
            publish_gates(first + count);
        }

        /**
         * @brief Writes every merged signature, with its captured baselines, to a manifest beside the ASI.
         * @details The file is the editable form of the built-in contract ([B-54]): copy a section into
         *          KCD2_HenrySenses.signatures.ini to repair it after a game patch.
         */
        void export_signatures(const DMK::Region &range)
        {
            DMK::Logger &logger = DMK::log();
            std::vector<DMK::manifest::SignatureRecord> records;
            records.reserve(s_signatures.size());
            for (const DMK::manifest::Signature &signature : s_signatures)
            {
                auto copy = DMK::manifest::Signature::compile(signature.record());
                if (!copy)
                {
                    continue;
                }
                if (auto captured = copy->recapture(range); !captured)
                {
                    logger.debug(
                        "Signatures: [sig.{}] exported without a content baseline ({})",
                        copy->label(),
                        captured.error().message()
                    );
                }
                records.push_back(copy->record());
            }
            const std::filesystem::path path = std::filesystem::path(DMK::filesystem::get_runtime_directory()) /
                                               (std::string(constants::MOD_NAME) + constants::SIGNATURE_EXPORT_SUFFIX);
            const DMK::manifest::Manifest manifest{
                .header =
                    {
                        .schema = DMK::manifest::SCHEMA_VERSION,
                        .revision = constants::SIGNATURE_REVISION,
                    },
                .records = std::move(records),
            };
            if (auto saved = DMK::manifest::save(path, manifest); saved)
            {
                logger.info("Signatures: exported {} signature(s) to {}", manifest.records.size(), path.string());
            }
            else
            {
                logger.warning("Signatures: export to {} failed ({})", path.string(), saved.error().message());
            }
        }
    } // namespace

    void resolve_early_anchors(std::uintptr_t module_base, std::size_t module_size)
    {
        resolve_slice(module_base, module_size, 0, EARLY_ANCHOR_COUNT, "early");
    }

    void resolve_all_anchors(std::uintptr_t module_base, std::size_t module_size)
    {
        resolve_slice(module_base, module_size, EARLY_ANCHOR_COUNT, ANCHOR_COUNT - EARLY_ANCHOR_COUNT, "main");

        const DMK::anchor::AnchorQuality quality = DMK::anchor::assess_quality(anchor_report());
        DMK::log().info(
            "Anchor resolution: {}/{} resolved ({} corroborated), {} failed, {} unsupported",
            quality.resolved,
            quality.total,
            quality.corroborated,
            quality.failed,
            quality.unsupported
        );

        if (settings().export_signatures.load(std::memory_order_relaxed))
        {
            export_signatures(DMK::Region{DMK::Address{module_base}, module_size});
        }
    }

    std::uintptr_t anchor_address(AnchorId id) noexcept
    {
        const std::size_t index = index_of(id);
        return index < ANCHOR_COUNT ? s_resolved_addresses[index] : 0;
    }

    const char *anchor_label(AnchorId id) noexcept
    {
        const std::size_t index = index_of(id);
        return index < ANCHOR_COUNT ? ANCHORS[index].label.data() : "?";
    }

    std::span<const ResolvedAnchor> anchor_report() noexcept
    {
        return std::span<const ResolvedAnchor>(s_report.data(), s_report_count);
    }

    GateVerdict feature_gate(Feature feature) noexcept
    {
        const auto index = static_cast<std::size_t>(feature);
        if (index >= FEATURE_COUNT || !s_verdict_ready[index].load(std::memory_order_acquire))
        {
            return GateVerdict::Fail;
        }
        return s_verdicts[index].load(std::memory_order_relaxed);
    }

    bool feature_ready(Feature feature) noexcept
    {
        return feature_gate(feature) != GateVerdict::Fail;
    }

    std::uintptr_t gated_anchor_address(Feature feature, AnchorId id) noexcept
    {
        return feature_ready(feature) ? anchor_address(id) : 0;
    }

    const char *feature_name(Feature feature) noexcept
    {
        const auto index = static_cast<std::size_t>(feature);
        return index < FEATURE_COUNT ? FEATURES[index].name.data() : "?";
    }

} // namespace HenrySenses
