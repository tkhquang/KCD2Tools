/**
 * @file aob_resolver.cpp
 * @brief Declarative anchor table, signature-file overlay, one-pass resolution, feature gates, and the resolved-value
 *        store.
 *
 * The candidate ladders in aob_resolver.hpp enter a DetourModKit anchor registry as RipGlobal, CodeOperand and Quorum
 * entries, each with the validator of what it names. resolve_all_anchors() resolves the whole table in a single
 * parallel pass at startup, replaces any row the signature file repairs, records each result, and evaluates one gate
 * per feature. The resolve_* helpers then prove the native-turn hook sites inside the functions those anchors found,
 * with anchors scoped to each function's .pdata range.
 */

#include "aob_resolver.hpp"
#include "constants.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace TPVCamera
{
    namespace
    {
        using DMK::anchor::Anchor;
        using DMK::anchor::AnchorKind;
        using DMK::anchor::AnchorStatus;
        using DMK::anchor::GateVerdict;
        using DMK::anchor::ResolvedAnchor;
        using DMK::scan::Candidate;
        using DMK::scan::OperandKind;
        using DMK::scan::Pages;

        constexpr std::size_t k_anchor_count = static_cast<std::size_t>(AnchorId::Count);
        constexpr std::size_t k_feature_count = static_cast<std::size_t>(Feature::Count);

        /// What an address anchor names, which decides the validator it must pass.
        enum class Role : std::uint8_t
        {
            /// A function entry the mod hooks or calls.
            Entry,
            /// An instruction inside a function, such as a return address the mod compares against.
            Site,
            /// A `call rel32` whose target the mod calls.
            CallSite,
            /// A global the mod reads through.
            Data,
        };

        // The WHGame.dll image the table resolves against, filled by resolve_all_anchors() before the sweep and
        // handed to every address validator as its opaque context.
        DMK::Region s_image_range{};

        [[nodiscard]] bool in_image(std::int64_t value, const void *context) noexcept
        {
            const auto *image = static_cast<const DMK::Region *>(context);
            return image != nullptr && image->size != 0 && value > 0 &&
                   image->contains(DMK::Address{static_cast<std::uintptr_t>(value)});
        }

        /**
         * @brief True when @p address lies on a committed, executable, non-guard page.
         * @details DetourModKit exposes no page-class query, so this is one VirtualQuery, at setup time only.
         */
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
         * @brief Validator of a function entry: inside the image, on an executable page, opening like a function, and
         *        at the start of its .pdata entry.
         * @details The scan scope bounds where a candidate's bytes are FOUND, not where a walk-back or disp32 POINTS,
         *          so a freak match is caught here and fails the anchor closed. A body rung's fixed walk-back can turn
         *          stale while its bytes still match. Where unwind data covers the target, its recorded function start
         *          must agree, because a plausible opcode inside an instruction is not an entry. A leaf function
         *          without unwind data keeps the byte probe as its evidence. scan::is_likely_function_prologue accepts
         *          a jump, so an entry that another mod hooked first still passes.
         */
        [[nodiscard]] bool entry_valid(std::int64_t value, const void *context) noexcept
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

        /// Validator of an instruction site: inside the image and on an executable page.
        [[nodiscard]] bool site_valid(std::int64_t value, const void *context) noexcept
        {
            return in_image(value, context) && on_executable_page(static_cast<std::uintptr_t>(value));
        }

        /**
         * @brief Validator of a call site: an instruction site holding a `call rel32` whose target is a valid function
         *        entry.
         * @details scan::Candidate::rip_relative decodes a RIP-relative memory operand, never a branch, so the anchor
         *          resolves the call itself, and its consumer decodes the target with scan::resolve_rip_relative. This
         *          proves that decode first, so the consumer never calls into the middle of a function.
         */
        [[nodiscard]] bool call_site_valid(std::int64_t value, const void *context) noexcept
        {
            if (!site_valid(value, context))
            {
                return false;
            }
            const DMK::Address site{static_cast<std::uintptr_t>(value)};
            const auto opcode = DMK::memory::read<std::uint8_t>(site);
            const auto callee = DMK::scan::resolve_rip_relative(site, 1, 5);
            return opcode && *opcode == 0xE8 && callee &&
                   entry_valid(static_cast<std::int64_t>(callee->raw()), context);
        }

        /// Validator of a global: inside the image, readable, and not on an executable page.
        [[nodiscard]] bool data_valid(std::int64_t value, const void *context) noexcept
        {
            const auto address = static_cast<std::uintptr_t>(value);
            return in_image(value, context) &&
                   DMK::memory::is_readable(DMK::Region{DMK::Address{address}, sizeof(std::uintptr_t)}) &&
                   !on_executable_page(address);
        }

        [[nodiscard]] DMK::anchor::AnchorValidator validator_for(Role role) noexcept
        {
            switch (role)
            {
            case Role::Entry:
                return entry_valid;
            case Role::Site:
                return site_valid;
            case Role::CallSite:
                return call_site_valid;
            case Role::Data:
            default:
                return data_valid;
            }
        }

        /**
         * @brief A RipGlobal anchor over one ladder, validated by @p role.
         * @details Every rung anchors on an in-image instruction (a function entry, a callable helper, or the
         *          instruction whose disp32 names a data slot), so the byte tiers sweep executable pages only: an
         *          identical byte run in .rdata or .data cannot alias or make a unique code match ambiguous.
         *          require_validator fails the row closed if it ever lacks its validator.
         */
        [[nodiscard]] Anchor code_ladder(std::string_view label, std::span<const Candidate> site, Role role) noexcept
        {
            return Anchor{
                .label = label,
                .kind = AnchorKind::RipGlobal,
                .site = site,
                .validator = validator_for(role),
                .validator_context = &s_image_range,
                .require_validator = true,
                .pages = Pages::Executable,
            };
        }

        /// The values a decoded scalar may take: a closed range and an alignment.
        struct ScalarRange
        {
            std::int64_t min;
            std::int64_t max;
            std::int64_t align;
        };

        // A vtable slot's byte offset: pointer-aligned, inside a vtable of at most 1024 slots.
        constexpr ScalarRange k_vtable_offset_range{8, 0x2000, 8};
        // A member offset inside a game object, for a uint8 field and for an int32 or float field.
        constexpr ScalarRange k_byte_field_range{1, 0x4000, 1};
        constexpr ScalarRange k_dword_field_range{4, 0x4000, 4};
        // An SGameObjectEvent id. C_Player::HandleEvent compares it as `cmp dword [rsi+8], imm8`, whose sign-extended
        // immediate only encodes 0..0x7F as a positive id.
        constexpr ScalarRange k_event_id_range{1, 0x7F, 1};
        // The event's target/flags word, a non-zero uint32.
        constexpr ScalarRange k_event_flags_range{1, 0xFFFFFFFF, 1};

        /**
         * @brief Post-resolve validator for a scalar anchor: the decoded value must lie in the ScalarRange passed as
         *        @p context.
         * @details A CodeOperand decodes whatever instruction its rung lands on, so a coincidental match yields a
         *          wrong but well-formed number. The range rejects the implausible ones (a vtable offset that is not
         *          pointer-aligned, a member offset beyond any game object), which then report Failed.
         */
        [[nodiscard]] bool scalar_in_range(std::int64_t value, const void *context) noexcept
        {
            const auto *range = static_cast<const ScalarRange *>(context);
            return range != nullptr && value >= range->min && value <= range->max && value % range->align == 0;
        }

        /**
         * @brief A CodeOperand anchor: decodes operand @p operand_index of the instruction its ladder lands on.
         * @param byte_width 0 keeps the decoded value, and 4 reads an imm32 as a sign-extended int32.
         */
        [[nodiscard]] Anchor code_operand(std::string_view label, std::span<const Candidate> site, OperandKind kind,
                                          std::uint8_t operand_index, const ScalarRange &range,
                                          std::uint8_t byte_width = 0) noexcept
        {
            return Anchor{
                .label = label,
                .kind = AnchorKind::CodeOperand,
                .site = site,
                .operand_kind = kind,
                .operand_index = operand_index,
                .byte_width = byte_width,
                .validator = scalar_in_range,
                .validator_context = &range,
                .require_validator = true,
            };
        }

        /**
         * @brief A Quorum anchor over independent CodeOperand members.
         * @param threshold How many members must resolve to the same value. 0 means all of them.
         */
        [[nodiscard]] Anchor quorum(std::string_view label, std::span<const Anchor *const> members,
                                    std::size_t threshold, const ScalarRange &range) noexcept
        {
            return Anchor{
                .label = label,
                .kind = AnchorKind::Quorum,
                .validator = scalar_in_range,
                .validator_context = &range,
                .quorum_members = members,
                .quorum_threshold = threshold,
            };
        }

        /// One Data vote per rung of @p ladder, so every rung is a separate vote of a root quorum.
        template <std::size_t N>
        [[nodiscard]] std::array<Anchor, N> single_rung_votes(std::string_view label,
                                                              const Candidate (&ladder)[N]) noexcept
        {
            std::array<Anchor, N> votes{};
            for (std::size_t i = 0; i < N; ++i)
            {
                votes[i] = code_ladder(label, std::span<const Candidate>{&ladder[i], 1}, Role::Data);
            }
            return votes;
        }

        /**
         * @brief A 2-of-N Quorum over the Data votes of a root global.
         * @details g_env and the global context are the roots every other read goes through, so a coincidental match
         *          after a patch must fool two independent code sites at once (`[B-52]`). DMK rejects the quorum when
         *          two winning spans overlap, so every vote is a separate code site.
         */
        [[nodiscard]] Anchor root_quorum(std::string_view label, std::span<const Anchor *const> members) noexcept
        {
            return Anchor{
                .label = label,
                .kind = AnchorKind::Quorum,
                .validator = data_valid,
                .validator_context = &s_image_range,
                .quorum_members = members,
                .quorum_threshold = 2,
            };
        }

        // Quorum members. Each is resolved only through its quorum, never on its own, so none is a table entry.
        const auto k_context_votes = single_rung_votes("GlobalContextPtr", Aob::k_contextCandidates);
        const Anchor *const k_context_vote_ptrs[] = {&k_context_votes[0], &k_context_votes[1], &k_context_votes[2]};

        // The GetIGameFramework site's two rungs match the same bytes, so they form one vote, never two.
        const Anchor k_genv_framework_vote = code_ladder("Genv", Aob::k_genvFrameworkSiteCandidates, Role::Data);
        const auto k_genv_site_votes = single_rung_votes("Genv", Aob::k_genvVoteCandidates);
        const Anchor *const k_genv_vote_ptrs[] = {&k_genv_framework_vote, &k_genv_site_votes[0], &k_genv_site_votes[1],
                                                  &k_genv_site_votes[2], &k_genv_site_votes[3]};

        const Anchor k_is_third_person_slot_trigger =
            code_operand("IsThirdPersonSlot.Trigger", Aob::k_isThirdPersonSlotTriggerCandidates,
                         OperandKind::MemoryDisplacement, 0, k_vtable_offset_range);
        const Anchor k_is_third_person_slot_lock_sync =
            code_operand("IsThirdPersonSlot.LockSync", Aob::k_isThirdPersonSlotLockSyncCandidates,
                         OperandKind::MemoryDisplacement, 1, k_vtable_offset_range);
        const Anchor *const k_is_third_person_slot_votes[] = {&k_is_third_person_slot_trigger,
                                                              &k_is_third_person_slot_lock_sync};

        const Anchor k_camera_event_id_send = code_operand("CameraEventId.Send", Aob::k_cameraEventSendIdCandidates,
                                                           OperandKind::Immediate, 1, k_event_id_range, 4);
        const Anchor k_camera_event_id_handle_event =
            code_operand("CameraEventId.HandleEvent", Aob::k_cameraEventHandleEventIdCandidates, OperandKind::Immediate,
                         1, k_event_id_range);
        const Anchor k_camera_event_id_on_event =
            code_operand("CameraEventId.OnEvent", Aob::k_cameraEventOnEventIdCandidates, OperandKind::Immediate, 1,
                         k_event_id_range);
        const Anchor *const k_camera_event_id_votes[] = {&k_camera_event_id_send, &k_camera_event_id_handle_event,
                                                         &k_camera_event_id_on_event};

        const Anchor k_turn_state_trigger =
            code_operand("TurnInstalledState.Trigger", Aob::k_turnStateTriggerCandidates,
                         OperandKind::MemoryDisplacement, 1, k_dword_field_range);
        const Anchor k_turn_state_on_event =
            code_operand("TurnInstalledState.OnEvent", Aob::k_turnStateOnEventCandidates,
                         OperandKind::MemoryDisplacement, 0, k_dword_field_range);
        const Anchor *const k_turn_state_votes[] = {&k_turn_state_trigger, &k_turn_state_on_event};

        // The registry, indexed by AnchorId. The enumerator order IS this order.
        const Anchor k_anchors[] = {
            root_quorum("GlobalContextPtr", k_context_vote_ptrs),
            root_quorum("Genv", k_genv_vote_ptrs),
            code_ladder("CameraFrustumBuild", Aob::k_frustumCandidates, Role::Entry),
            code_ladder("SetHeadVisibility", Aob::k_headVisibilityCandidates, Role::Entry),
            code_ladder("CameraInputDispatch", Aob::k_inputDispatchCandidates, Role::Entry),
            code_ladder("PlayerOnActionDispatch", Aob::k_actionDispatchCandidates, Role::Entry),
            code_ladder("RayWorldIntersection", Aob::k_rayWorldIntersectionCandidates, Role::Entry),
            code_ladder("InteractionRayBuild", Aob::k_interactionRayBuildCandidates, Role::Entry),
            code_ladder("InteractorLookRay", Aob::k_interactorLookRayCandidates, Role::Entry),
            code_ladder("InteractionOnScreenCheck", Aob::k_interactionOnScreenCandidates, Role::Entry),
            code_ladder("HideOverlays", Aob::k_overlayHideCandidates, Role::Entry),
            code_ladder("ShowOverlays", Aob::k_overlayShowCandidates, Role::Entry),
            code_ladder("MenuOpen", Aob::k_menuOpenCandidates, Role::Entry),
            code_ladder("MenuClose", Aob::k_menuCloseCandidates, Role::Entry),
            code_ladder("GetObjectsInBox", Aob::k_getObjectsInBoxCandidates, Role::Entry),
            code_ladder("TurnTriggerIsThirdPersonReturn", Aob::k_turnTriggerReturnCandidates, Role::Site),
            code_ladder("LockSyncIsThirdPersonReturn", Aob::k_lockSyncReturnCandidates, Role::Site),
            code_ladder("UpdatePhysicalEntityMovement", Aob::k_physEntMovementCandidates, Role::Entry),
            quorum("IsThirdPersonSlot", k_is_third_person_slot_votes, 0, k_vtable_offset_range),
            code_operand("HandleEventSlot", Aob::k_cameraEventSendSlotCandidates, OperandKind::MemoryDisplacement, 0,
                         k_vtable_offset_range),
            quorum("CameraEventId", k_camera_event_id_votes, 2, k_event_id_range),
            code_operand("CameraEventFlags", Aob::k_cameraEventSendFlagsCandidates, OperandKind::Immediate, 1,
                         k_event_flags_range, 4),
            quorum("TurnInstalledState", k_turn_state_votes, 0, k_dword_field_range),
            code_operand("LockBodyTurnCount", Aob::k_lockBodyTurnCountCandidates, OperandKind::MemoryDisplacement, 0,
                         k_dword_field_range),
            code_operand("AnimIdByCrcSlot", Aob::k_animIdByCrcCallCandidates, OperandKind::MemoryDisplacement, 1,
                         k_vtable_offset_range),
            code_ladder("AnimNameHashCall", Aob::k_animNameHashCallCandidates, Role::CallSite),
        };
        static_assert(std::size(k_anchors) == k_anchor_count, "k_anchors must hold one entry per AnchorId.");

        // The anchors each feature depends on. A feature is enabled only through its gate over exactly these.
        constexpr AnchorId k_camera_anchors[] = {AnchorId::Frustum};
        constexpr AnchorId k_game_state_anchors[] = {AnchorId::Context};
        constexpr AnchorId k_engine_anchors[] = {AnchorId::Genv};
        constexpr AnchorId k_head_visibility_anchors[] = {AnchorId::HeadVisibility};
        constexpr AnchorId k_orbit_anchors[] = {AnchorId::InputDispatch};
        constexpr AnchorId k_move_intent_anchors[] = {AnchorId::ActionDispatch};
        constexpr AnchorId k_collision_anchors[] = {AnchorId::Genv, AnchorId::RayWorldIntersection};
        constexpr AnchorId k_interaction_anchors[] = {AnchorId::InteractionRayBuild, AnchorId::InteractorLookRay};
        constexpr AnchorId k_interaction_on_screen_anchors[] = {AnchorId::InteractionOnScreen};
        constexpr AnchorId k_overlay_state_anchors[] = {AnchorId::OverlayHide, AnchorId::OverlayShow};
        constexpr AnchorId k_menu_state_anchors[] = {AnchorId::MenuOpen, AnchorId::MenuClose};
        constexpr AnchorId k_occlusion_anchors[] = {AnchorId::Genv, AnchorId::GetObjectsInBox};
        constexpr AnchorId k_native_turn_anchors[] = {
            AnchorId::TurnTriggerReturn, AnchorId::LockSyncReturn, AnchorId::IsThirdPersonSlot,
            AnchorId::HandleEventSlot,   AnchorId::CameraEventId,  AnchorId::CameraEventFlags,
        };
        constexpr AnchorId k_turn_decision_anchors[] = {AnchorId::TurnInstalledState};
        constexpr AnchorId k_turn_steps_anchors[] = {AnchorId::PhysEntMovement};
        constexpr AnchorId k_crouched_animation_anchors[] = {AnchorId::AnimIdByCrcSlot, AnchorId::AnimNameHashCall};

        /// One feature gate: its log name and the anchors it depends on.
        struct FeatureSpec
        {
            std::string_view name;
            std::span<const AnchorId> anchors;
        };

        // Indexed by Feature. The enumerator order IS this order.
        constexpr std::array<FeatureSpec, k_feature_count> k_features = {{
            {"Camera", k_camera_anchors},
            {"GameState", k_game_state_anchors},
            {"Engine", k_engine_anchors},
            {"HeadVisibility", k_head_visibility_anchors},
            {"Orbit", k_orbit_anchors},
            {"MoveIntent", k_move_intent_anchors},
            {"Collision", k_collision_anchors},
            {"Interaction", k_interaction_anchors},
            {"InteractionOnScreen", k_interaction_on_screen_anchors},
            {"OverlayState", k_overlay_state_anchors},
            {"MenuState", k_menu_state_anchors},
            {"Occlusion", k_occlusion_anchors},
            {"NativeTurn", k_native_turn_anchors},
            {"TurnDecision", k_turn_decision_anchors},
            {"TurnSteps", k_turn_steps_anchors},
            {"CrouchedAnimations", k_crouched_animation_anchors},
        }};

        constexpr std::size_t k_max_feature_anchors = 6;
        static_assert(std::ranges::all_of(k_features,
                                          [](const FeatureSpec &spec)
                                          {
                                              return !spec.name.empty() && !spec.anchors.empty() &&
                                                     spec.anchors.size() <= k_max_feature_anchors;
                                          }),
                      "k_features must hold one named, non-empty entry per Feature within k_max_feature_anchors.");

        // The hooked anchors: a repaired signature for one of them authorizes a code write.
        constexpr AnchorId k_hooked_anchors[] = {
            AnchorId::Frustum,        AnchorId::HeadVisibility,      AnchorId::InputDispatch,
            AnchorId::ActionDispatch, AnchorId::InteractionRayBuild, AnchorId::InteractionOnScreen,
            AnchorId::OverlayHide,    AnchorId::OverlayShow,         AnchorId::MenuOpen,
            AnchorId::MenuClose,      AnchorId::PhysEntMovement,
        };

        // Room after the startup pass for the function-scoped anchors resolve_turn_decision_layout() (six) and
        // resolve_movement_type_offset() (one) append.
        constexpr std::size_t k_max_scoped_report = 7;
        constexpr std::size_t k_max_report = k_anchor_count + k_max_scoped_report;

        // Resolved absolute addresses, indexed by AnchorId. 0 means unresolved or a scalar anchor. Zero-initialized
        // (constant init, no static-init-order hazard). Written once by resolve_all_anchors() on the init thread
        // before any consumer reads, then read-only, so no synchronization is required. The same holds for every
        // store below.
        std::array<std::uintptr_t, k_anchor_count> s_resolved_addresses{};
        // Every resolved value, address and scalar anchors alike.
        std::array<std::optional<std::int64_t>, k_anchor_count> s_resolved_values{};
        // Each feature's gate result. False until resolve_all_anchors() evaluates it, so a feature fails closed.
        std::array<bool, k_feature_count> s_feature_ready{};

        // The per-anchor report (the startup table first, then the function-scoped anchors), retained so the
        // gates and the shutdown diagnostics snapshot read it. The startup slice is indexed by AnchorId.
        std::array<ResolvedAnchor, k_max_report> s_report{};
        std::size_t s_report_count = 0;

        // The merged signatures: the in-code rows with the signature file's repairs on top (manifest::overlay).
        // Quorum rows have no file form and stay out. Kept for export_signatures().
        std::vector<DMK::manifest::Signature> s_signatures;
        // The signature file's header, which the trust gate checks a repair's contract revision against.
        DMK::manifest::ManifestHeader s_file_header{};

        [[nodiscard]] constexpr std::size_t index_of(AnchorId id) noexcept
        {
            return static_cast<std::size_t>(id);
        }

        [[nodiscard]] bool is_hooked(std::size_t index) noexcept
        {
            return std::ranges::any_of(k_hooked_anchors, [index](AnchorId id) { return index_of(id) == index; });
        }

        /// The table index of the row labelled @p label, or k_anchor_count when none is.
        [[nodiscard]] std::size_t index_of_label(std::string_view label) noexcept
        {
            const auto *row = std::ranges::find_if(k_anchors, [label](const Anchor &a) { return a.label == label; });
            return static_cast<std::size_t>(row - std::begin(k_anchors));
        }

        /// A file beside the ASI, named after the mod.
        [[nodiscard]] std::filesystem::path runtime_file(std::string_view suffix)
        {
            return std::filesystem::path(DMK::filesystem::get_runtime_directory()) /
                   (std::string(Constants::MOD_NAME) + std::string(suffix));
        }

        /**
         * @brief Grades every candidate pattern in the table and reports the weak ones.
         * @details sighealth is offline and side-effect-free: it reads the COMPILED pattern bytes and mask and
         *          scores atom rarity, byte entropy, and expected ambiguity in a nominal module. It touches no
         *          process memory and never gates resolution (`[B-57]`), so this runs before the sweep and only
         *          reports. Its value is on patch day: a cascade that stops resolving against a new WHGame.dll is
         *          usually a signature that was already weakly selective, and this line says which rung was, without
         *          a disassembler. Quorum members are graded with their quorum. RTTI and string-xref candidates carry
         *          no byte pattern and are skipped.
         * @param anchors The declarative table.
         */
        void report_signature_health(std::span<const Anchor> anchors)
        {
            DMK::Logger &logger = DMK::log();
            std::size_t fragile = 0;
            std::size_t unusable = 0;

            const auto grade_ladder = [&](std::string_view label, std::span<const Candidate> ladder)
            {
                for (const Candidate &candidate : ladder)
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
                        logger.trace("Signature health: {}/{} Robust", label, candidate.name());
                        continue;
                    }
                    (health.grade == DMK::sighealth::Grade::Unusable ? ++unusable : ++fragile);
                    logger.debug("Signature health: {}/{} {} - {}", label, candidate.name(),
                                 DMK::sighealth::to_string(health.grade),
                                 DMK::sighealth::format_report(health, candidate.name()));
                }
            };
            for (const Anchor &entry : anchors)
            {
                grade_ladder(entry.label, entry.site);
                for (const Anchor *member : entry.quorum_members)
                {
                    grade_ladder(entry.label, member->site);
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

        /**
         * @brief Loads the signature file's repairs.
         * @details A missing file changes nothing. A file for another signature-contract revision is ignored with a
         *          warning, so a stale repair can never override a ladder this build corrected. A malformed entry is
         *          named here and falls back to its in-code row in the overlay, which is fail-soft.
         * @return The file's records, or none.
         */
        [[nodiscard]] std::vector<DMK::manifest::SignatureRecord> load_signature_repairs()
        {
            DMK::Logger &logger = DMK::log();
            const std::filesystem::path path = runtime_file(Constants::SIGNATURE_FILE_SUFFIX);
            std::error_code exists_error;
            if (!std::filesystem::exists(path, exists_error))
            {
                return {};
            }
            auto loaded = DMK::manifest::load(path);
            if (!loaded)
            {
                logger.error("Signatures: {} could not be read ({}); the built-in signatures stand",
                             path.filename().string(), loaded.error().message());
                return {};
            }
            if (!DMK::manifest::revision_compatible(loaded->header, Constants::SIGNATURE_REVISION))
            {
                logger.warning("Signatures: {} targets signature revision {}, this build is revision {}; the file is "
                               "ignored. Delete it, or set revision = {} only after re-verifying every entry",
                               path.filename().string(), loaded->header.revision, Constants::SIGNATURE_REVISION,
                               Constants::SIGNATURE_REVISION);
                return {};
            }
            s_file_header = loaded->header;
            for (const DMK::manifest::SignatureRecord &record : loaded->records)
            {
                logger.info("Signatures: [sig.{}] from {} {}", record.label, path.filename().string(),
                            DMK::manifest::Signature::compile(record) ? "is a repair candidate"
                                                                      : "is malformed; the built-in signature stands");
            }
            return std::move(loaded->records);
        }

        /**
         * @brief Merges the in-code rows with the signature file's repairs into s_signatures.
         * @details A Quorum row has no file form (it composes its members by pointer), so it stays out of the overlay
         *          and resolves from its in-code row.
         */
        void merge_signatures(std::span<const DMK::manifest::SignatureRecord> repairs)
        {
            std::vector<Anchor> serializable;
            serializable.reserve(k_anchor_count);
            for (const Anchor &row : k_anchors)
            {
                if (row.kind != AnchorKind::Quorum)
                {
                    serializable.push_back(row);
                }
            }
            auto merged = DMK::manifest::overlay(serializable, repairs);
            if (!merged)
            {
                DMK::log().error("Signatures: merging failed ({}); the built-in signatures stand",
                                 merged.error().message());
                s_signatures.clear();
                return;
            }
            s_signatures = std::move(*merged);
        }

        /**
         * @brief Replaces the report entry of every row the signature file repaired.
         * @details A merged signature whose fingerprint differs from its built-in row came from the file. It counts
         *          only after it resolves and passes the manifest trust gate. A repaired hook target authorizes a code
         *          write, so it needs the complete baseline set (GatePolicy::mutation_strict, `[B-54]`). A repair that
         *          does not pass reports Failed, so its features stay off rather than run on an untrusted address.
         */
        void apply_signature_repairs(const DMK::Region &range)
        {
            DMK::Logger &logger = DMK::log();
            for (const DMK::manifest::Signature &signature : s_signatures)
            {
                const std::size_t index = index_of_label(signature.label());
                if (index >= k_anchor_count)
                {
                    continue;
                }
                const auto builtin = DMK::manifest::Signature::adopt(k_anchors[index]);
                if (!builtin || builtin->current_fingerprint() == signature.current_fingerprint())
                {
                    continue;
                }
                logger.warning("Signatures: {} uses the repair from the signature file", signature.label());

                ResolvedAnchor resolved = signature.resolve(range);
                if (resolved.status == AnchorStatus::Resolved)
                {
                    const DMK::manifest::GatePolicy policy =
                        is_hooked(index) ? DMK::manifest::GatePolicy::mutation_strict() : DMK::manifest::GatePolicy{};
                    const DMK::manifest::GateResult gate =
                        DMK::manifest::resolve_and_gate(std::span<const DMK::manifest::Signature>{&signature, 1},
                                                        s_file_header, Constants::SIGNATURE_REVISION, policy, range);
                    if (gate.find(signature.label()) == nullptr)
                    {
                        logger.warning(
                            "Signatures: the repair of {} resolved but is not trusted ({}); it counts as unresolved",
                            signature.label(),
                            gate.rejected.empty() ? std::string_view{"rejected"}
                                                  : DMK::manifest::gate_reason_to_string(gate.rejected.front().reason));
                        resolved.status = AnchorStatus::Failed;
                        resolved.value = 0;
                    }
                }
                // The report keeps the table's label view, which outlives the merged signature.
                resolved.label = k_anchors[index].label;
                s_report[index] = resolved;
            }
        }

        /// Evaluates every feature's gate over its anchors' report entries and logs the verdict.
        void evaluate_feature_gates()
        {
            DMK::Logger &logger = DMK::log();
            for (std::size_t f = 0; f < k_feature_count; ++f)
            {
                const FeatureSpec &spec = k_features[f];
                std::array<ResolvedAnchor, k_max_feature_anchors> entries{};
                std::size_t count = 0;
                for (const AnchorId id : spec.anchors)
                {
                    entries[count++] = s_report[index_of(id)];
                }
                const GateVerdict verdict = DMK::anchor::evaluate_gate(std::span{entries.data(), count});
                s_feature_ready[f] = verdict != GateVerdict::Fail;
                if (verdict == GateVerdict::Pass)
                {
                    logger.debug("Feature gate: {} Pass", spec.name);
                }
                else
                {
                    logger.warning("Feature gate: {} {}{}", spec.name, DMK::anchor::gate_verdict_to_string(verdict),
                                   s_feature_ready[f] ? "; enabled" : "; the feature is off");
                }
            }
        }

        // Function-scoped quorum members (see resolve_turn_decision_layout()), resolved inside ComputeMoveState's
        // fragment through their quorum only. report_signature_health() does not grade the function-scoped ladders:
        // its estimate assumes a whole-module scan, and these are kept short on purpose because they scan one
        // function.
        const Anchor k_turn_latch_compare = code_operand("TurnSpinLatch.Compare", Aob::k_turnLatchCompareCandidates,
                                                         OperandKind::MemoryDisplacement, 0, k_byte_field_range);
        const Anchor k_turn_latch_store = code_operand("TurnSpinLatch.Store", Aob::k_turnLatchStoreCandidates,
                                                       OperandKind::MemoryDisplacement, 0, k_byte_field_range);
        const Anchor *const k_turn_latch_votes[] = {&k_turn_latch_compare, &k_turn_latch_store};

        const Anchor k_turn_sign_store = code_operand("TurnLastSign.Store", Aob::k_turnSignStoreCandidates,
                                                      OperandKind::MemoryDisplacement, 0, k_dword_field_range);
        const Anchor k_turn_sign_load = code_operand("TurnLastSign.Load", Aob::k_turnSignLoadCandidates,
                                                     OperandKind::MemoryDisplacement, 1, k_dword_field_range);
        const Anchor *const k_turn_sign_votes[] = {&k_turn_sign_store, &k_turn_sign_load};

        /// The .pdata bounds of the code around an address (see function_bounds()).
        struct FunctionBounds
        {
            /// The RUNTIME_FUNCTION range that holds the address: one fragment of a possibly split function.
            DMK::Region fragment;
            /// The function's entry fragment, reached through chained unwind info. It starts with the prologue.
            DMK::Region entry;
            /// The prologue length the entry's unwind info declares.
            std::size_t prologue_size = 0;
        };

        /**
         * @brief Looks up the .pdata entry that holds @p address and follows chained unwind info to the function's
         *        entry fragment.
         * @details A compiler can split a function into an entry fragment and cold fragments, each with its own
         *          RUNTIME_FUNCTION. A cold fragment's UNWIND_INFO carries UNW_FLAG_CHAININFO and ends, after its
         *          unwind codes (padded to an even count), with its parent's RUNTIME_FUNCTION. DetourModKit exposes no
         *          function-bounds API, so this walks the same table the OS unwinder does. Every UNWIND_INFO read is
         *          guarded.
         * @return The bounds, or std::nullopt when @p address has no entry or its unwind chain cannot be read.
         */
        [[nodiscard]] std::optional<FunctionBounds> function_bounds(std::uintptr_t address) noexcept
        {
            DWORD64 image_base = 0;
            const PRUNTIME_FUNCTION found = RtlLookupFunctionEntry(address, &image_base, nullptr);
            if (found == nullptr || image_base == 0)
            {
                return std::nullopt;
            }
            const auto region_of = [image_base](std::uint32_t begin, std::uint32_t end)
            {
                return DMK::Region{DMK::Address{static_cast<std::uintptr_t>(image_base) + begin},
                                   end > begin ? std::size_t{end - begin} : std::size_t{0}};
            };

            FunctionBounds bounds{.fragment = region_of(found->BeginAddress, found->EndAddress)};
            std::array<std::uint32_t, 3> entry{found->BeginAddress, found->EndAddress, found->UnwindData};
            constexpr int k_max_chain = 8;
            for (int hop = 0; hop < k_max_chain; ++hop)
            {
                // UNWIND_INFO header: version (low 3 bits) and flags (high 5 bits), prologue size, unwind code count,
                // frame register.
                const std::uintptr_t unwind_info = static_cast<std::uintptr_t>(image_base) + entry[2];
                const auto header = DMK::memory::read<std::array<std::uint8_t, 4>>(DMK::Address{unwind_info});
                if (!header)
                {
                    return std::nullopt;
                }
                if ((((*header)[0] >> 3) & UNW_FLAG_CHAININFO) == 0)
                {
                    bounds.entry = region_of(entry[0], entry[1]);
                    bounds.prologue_size = (*header)[1];
                    return bounds;
                }
                const std::size_t code_slots = (static_cast<std::size_t>((*header)[2]) + 1) & ~std::size_t{1};
                const auto parent = DMK::memory::read<std::array<std::uint32_t, 3>>(
                    DMK::Address{unwind_info + 4 + code_slots * sizeof(std::uint16_t)});
                if (!parent)
                {
                    return std::nullopt;
                }
                entry = *parent;
            }
            return std::nullopt;
        }

        /// The longest span any Direct rung of @p ladder can match.
        [[nodiscard]] std::size_t ladder_reach(std::span<const Candidate> ladder) noexcept
        {
            std::size_t reach = 0;
            for (const Candidate &candidate : ladder)
            {
                if (const auto *direct = candidate.as_direct())
                {
                    reach = std::max(reach, direct->pattern.max_match_length());
                }
            }
            return reach;
        }

        /**
         * @brief Resolves @p anchor inside @p scope and appends the result to the report.
         * @return The result, or std::nullopt when the report has no room left, which leaves the anchor unresolved.
         */
        [[nodiscard]] std::optional<ResolvedAnchor> resolve_scoped(const Anchor &anchor, DMK::Region scope)
        {
            if (s_report_count >= s_report.size())
            {
                return std::nullopt;
            }
            ResolvedAnchor &entry = s_report[s_report_count++];
            entry = DMK::anchor::resolve(anchor, scope);
            if (entry.status == AnchorStatus::Resolved)
            {
                DMK::log().debug("Anchor {} = {:#x}", entry.label, entry.value);
            }
            else
            {
                DMK::log().debug("Anchor {} unresolved ({})", entry.label,
                                 DMK::anchor::anchor_status_to_string(entry.status));
            }
            return entry;
        }

        /// The address a scoped code anchor resolves to, or 0.
        [[nodiscard]] std::uintptr_t scoped_address(const Anchor &anchor, DMK::Region scope)
        {
            const std::optional<ResolvedAnchor> entry = resolve_scoped(anchor, scope);
            return entry && entry->status == AnchorStatus::Resolved ? static_cast<std::uintptr_t>(entry->value) : 0;
        }

        /// The value a scoped scalar anchor resolves to, or std::nullopt.
        [[nodiscard]] std::optional<std::int64_t> scoped_value(const Anchor &anchor, DMK::Region scope)
        {
            const std::optional<ResolvedAnchor> entry = resolve_scoped(anchor, scope);
            if (!entry || entry->status != AnchorStatus::Resolved)
            {
                return std::nullopt;
            }
            return entry->value;
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
        s_resolved_addresses.fill(0);
        s_resolved_values.fill(std::nullopt);
        s_feature_ready.fill(false);

        // Offline signature grading first: it needs no game memory and says which rungs are structurally weak
        // BEFORE the sweep reports which ones missed, so the two lines read together on a patch-day log.
        report_signature_health(k_anchors);

        s_report_count = DMK::anchor::resolve_all_parallel(k_anchors, std::span{s_report}.first(k_anchor_count), range);

        merge_signatures(load_signature_repairs());
        apply_signature_repairs(range);

        // resolve_all_parallel writes s_report[i] for k_anchors[i], so the report index is the AnchorId.
        for (std::size_t i = 0; i < s_report_count; ++i)
        {
            const ResolvedAnchor &entry = s_report[i];
            if (entry.status == AnchorStatus::Resolved)
            {
                s_resolved_values[i] = entry.value;
                // Per-anchor values are for RE / external tooling, not routine status, so they stay at Debug. The
                // one-line quality summary below is the default-level health check, and a failure still warns.
                if (entry.domain == DMK::anchor::ResultDomain::Scalar)
                {
                    logger.debug("Anchor {} = {:#x}", entry.label, entry.value);
                }
                else
                {
                    s_resolved_addresses[i] = static_cast<std::uintptr_t>(entry.value);
                    logger.debug("Anchor {} -> {}", entry.label, DMK::format::format_address(s_resolved_addresses[i]));
                }
            }
            else
            {
                logger.warning("Anchor {} unresolved ({})", entry.label,
                               DMK::anchor::anchor_status_to_string(entry.status));
            }
        }

        evaluate_feature_gates();

        const DMK::anchor::AnchorQuality quality = DMK::anchor::assess_quality(anchor_report());
        logger.info("Anchor resolution: {}/{} resolved ({} corroborated), {} failed, {} unsupported", quality.resolved,
                    quality.total, quality.corroborated, quality.failed, quality.unsupported);
    }

    void export_signatures()
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
            if (auto captured = copy->recapture(s_image_range); !captured)
            {
                logger.debug("Signatures: [sig.{}] exported without a content baseline ({})", copy->label(),
                             captured.error().message());
            }
            records.push_back(copy->record());
        }
        const std::filesystem::path path = runtime_file(Constants::SIGNATURE_EXPORT_SUFFIX);
        const DMK::manifest::Manifest manifest{
            .header =
                {
                    .schema = DMK::manifest::SCHEMA_VERSION,
                    .revision = Constants::SIGNATURE_REVISION,
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

    bool feature_ready(Feature feature) noexcept
    {
        const std::size_t index = static_cast<std::size_t>(feature);
        return index < k_feature_count && s_feature_ready[index];
    }

    std::uintptr_t gated_anchor_address(Feature feature, AnchorId id) noexcept
    {
        const std::size_t index = index_of(id);
        return feature_ready(feature) && index < k_anchor_count ? s_resolved_addresses[index] : 0;
    }

    std::optional<std::int64_t> gated_anchor_value(Feature feature, AnchorId id) noexcept
    {
        return feature_ready(feature) ? anchor_value(id) : std::nullopt;
    }

    std::optional<std::int64_t> anchor_value(AnchorId id) noexcept
    {
        const std::size_t index = index_of(id);
        return index < k_anchor_count ? s_resolved_values[index] : std::nullopt;
    }

    std::optional<TurnDecisionLayout> resolve_turn_decision_layout(std::uintptr_t trigger_return)
    {
        DMK::Logger &logger = DMK::log();
        const auto refuse = [&logger](std::string_view proof) -> std::optional<TurnDecisionLayout>
        {
            logger.warning("Turn decision: {} did not resolve", proof);
            return std::nullopt;
        };

        const std::optional<std::int64_t> installed_state =
            gated_anchor_value(Feature::TurnDecision, AnchorId::TurnInstalledState);
        if (!installed_state)
        {
            return refuse("the movement action's installed-turn state");
        }

        const std::optional<FunctionBounds> bounds = function_bounds(trigger_return);
        if (!bounds || !s_image_range.contains(bounds->entry.base) ||
            !bounds->fragment.contains(DMK::Address{trigger_return}))
        {
            return refuse("ComputeMoveState's unwind entry");
        }
        const DMK::Region &body = bounds->fragment;

        // The contract window must end exactly on the return address, so its scope is its own length right before
        // it and the match must start at the scope's base.
        const std::size_t contract_length = ladder_reach(Aob::k_turnContractCandidates);
        const DMK::Region contract_scope{DMK::Address{trigger_return - contract_length}, contract_length};
        if (scoped_address(code_ladder("TurnContract", Aob::k_turnContractCandidates, Role::Site), contract_scope) !=
            contract_scope.base.raw())
        {
            return refuse("the register contract before the IsThirdPerson call");
        }

        // The decision opens exactly at the return address, so its scope is its own length from there. The `|`
        // resolves to the hook site.
        const std::uintptr_t site =
            scoped_address(code_ladder("TurnDecisionSite", Aob::k_turnDecisionSiteCandidates, Role::Site),
                           DMK::Region{DMK::Address{trigger_return}, ladder_reach(Aob::k_turnDecisionSiteCandidates)});
        if (site == 0)
        {
            return refuse("the IsThirdPerson test, the 35-degree compare and `seta cl`");
        }

        // r12 holds the turn-angle output pointer from the prologue to the store through it.
        if (bounds->prologue_size == 0 ||
            scoped_address(code_ladder("TurnOutputSave", Aob::k_turnOutputSaveCandidates, Role::Site),
                           DMK::Region{bounds->entry.base, bounds->prologue_size}) == 0 ||
            scoped_address(code_ladder("TurnOutputStore", Aob::k_turnOutputStoreCandidates, Role::Site), body) == 0)
        {
            return refuse("the turn-angle output pointer in r12");
        }

        const std::optional<std::int64_t> spin_latch =
            scoped_value(quorum("TurnSpinLatch", k_turn_latch_votes, 0, k_byte_field_range), body);
        const std::optional<std::int64_t> last_sign =
            scoped_value(quorum("TurnLastSign", k_turn_sign_votes, 0, k_dword_field_range), body);
        if (!spin_latch || !last_sign)
        {
            return refuse("the movement action's spin-latch fields");
        }

        logger.debug("Turn decision: site {}, spin latch +{:#x}, installed state +{:#x}, last sign +{:#x}",
                     DMK::format::format_address(site), *spin_latch, *installed_state, *last_sign);
        return TurnDecisionLayout{
            .site = site,
            .spin_latch_offset = static_cast<std::ptrdiff_t>(*spin_latch),
            .installed_state_offset = static_cast<std::ptrdiff_t>(*installed_state),
            .last_sign_offset = static_cast<std::ptrdiff_t>(*last_sign),
        };
    }

    std::optional<std::ptrdiff_t> resolve_movement_type_offset(std::uintptr_t update_physical_entity_movement)
    {
        // The anchor resolves the function entry, which must also be where its .pdata entry begins.
        const std::optional<FunctionBounds> bounds = function_bounds(update_physical_entity_movement);
        if (!bounds || bounds->fragment.base.raw() != update_physical_entity_movement)
        {
            DMK::log().warning("Turn steps: UpdatePhysicalEntityMovement's unwind entry did not resolve");
            return std::nullopt;
        }
        const std::optional<std::int64_t> offset =
            scoped_value(code_operand("MovementType", Aob::k_movementTypeCandidates, OperandKind::MemoryDisplacement, 1,
                                      k_dword_field_range),
                         bounds->fragment);
        if (!offset)
        {
            DMK::log().warning("Turn steps: the movement request type read did not resolve");
            return std::nullopt;
        }
        return static_cast<std::ptrdiff_t>(*offset);
    }

    bool dispatches_camera_event(std::uintptr_t handle_event, std::uint8_t event_id)
    {
        const std::optional<FunctionBounds> bounds = function_bounds(handle_event);
        if (!bounds || !s_image_range.contains(DMK::Address{handle_event}) ||
            bounds->fragment.base.raw() != handle_event)
        {
            return false;
        }
        // The k_cameraEventHandleEventIdCandidates shape with the id the quorum accepted in place of its wildcard.
        const DMK::Result<DMK::scan::Pattern> dispatch = DMK::scan::Pattern::compile(
            std::format("83 7E 08 {:02X} [2-6] 48 8B CF E8 ?? ?? ?? ?? 48 85 C0 [2-6] 48 8B 08 48 8B D6 4C 8B 01 48 8B "
                        "C8 41 FF D0",
                        event_id));
        return dispatch.has_value() && DMK::scan::scan(*dispatch, bounds->fragment, 1, Pages::Executable).has_value();
    }

    std::span<const ResolvedAnchor> anchor_report() noexcept
    {
        return std::span<const ResolvedAnchor>(s_report.data(), s_report_count);
    }

} // namespace TPVCamera
