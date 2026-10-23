/**
 * @file anchors.cpp
 * @brief The anchor registry: candidate ladders, validators, vtable identities and the feature gates.
 *
 * A function the mod calls or hooks is a 2-of-2 quorum of two body rungs that do not overlap: its prologue, and a
 * later stretch of its body that walks back to the entry. Both must match and agree, so a rung that drifted onto
 * another function fails its feature instead of a call into the wrong code. The game context getter is too short for
 * two such rungs and stays a ladder. The hooked Update must also equal its slot of the RTTI-resolved C_LockPicking
 * vtable. The hooked HUD handler is a 2-of-2 quorum of the lea that loads its address in the HUD's connect code and
 * its own body. gEnv, the root of every engine read, is a 2-of-3 quorum of three load sites. DMK anchor::resolve takes
 * a rung only when it matches exactly once, inside WHGame.dll and on executable pages, and the value passes the
 * validator: a function entry that agrees with its unwind entry, or a readable non-code global.
 *
 * Signature rules: every relative target, RIP displacement and virtual-call slot is a wildcard, every branch inside a
 * pattern is a bounded gap ([2-6] covers the rel8 and rel32 forms), and no pattern crosses the padding between
 * functions. Every rung matches exactly once on the Steam, GOG and Game Pass builds.
 */

#include "anchors.hpp"
#include "constants.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace OblivionLockpicking
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

        // A 7-byte REX.W `mov` or `lea` with a RIP-relative operand keeps its disp32 three bytes past the REX prefix.
        constexpr std::ptrdiff_t RIP_DISP32_OFFSET = 3;
        constexpr std::size_t RIP_INSTRUCTION_LENGTH = 7;

        // gEnv: three loads of the static SSystemGlobalEnvironment, each a RIP-relative disp32 to its first member.
        const Candidate GENV[] = {
            Candidate::rip_relative(
                "Genv_P1_MovCallLea",
                Pattern::literal("48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 50 ?? 48 8D 94 24 F0 05 00 00"),
                RIP_DISP32_OFFSET,
                RIP_INSTRUCTION_LENGTH
            ),
            Candidate::rip_relative(
                "Genv_P2_MovCallTest",
                Pattern::literal("48 8B C8 FF D2 | 48 8B 0D ?? ?? ?? ?? 48 85 C9 [2-6] 48 8B 01 33 D2"),
                RIP_DISP32_OFFSET,
                RIP_INSTRUCTION_LENGTH
            ),
            Candidate::rip_relative(
                "Genv_P3_LeaStructInit",
                Pattern::literal("48 8D 05 ?? ?? ?? ?? 48 89 0F 4C 8D 67 ??"),
                RIP_DISP32_OFFSET,
                RIP_INSTRUCTION_LENGTH
            ),
        };

        // The hearing emitter step. P1 is the prologue, which loads the level at +8 and compares it against zero. P2
        // saves xmm6 and xmm7, keeps the frame time in xmm7, branches on that compare, then loads a global and its
        // vtable.
        const Candidate EMITTER_STEP[] = {
            Candidate::direct(
                "EmitterStep_P1_Prologue",
                Pattern::literal("48 89 5C 24 08 57 48 83 EC 50 F3 0F 10 41 08 48 8B F9 0F 2E 05")
            ),
            Candidate::direct(
                "EmitterStep_P2_SaveXmm",
                Pattern::literal("0F 29 74 24 40 0F 29 7C 24 30 0F 28 F9 [4-12] 48 8B 0D ?? ?? ?? ?? 48 8B 01"),
                -0x19
            ),
        };

        // The game context getter, a guarded static. P1 is its fast path from the entry: the TLS epoch compare, then
        // the load of the cached pointer. The getter is too short for a second rung past that, so P2 starts after the
        // 6-byte frame setup, which a 5-byte jump on the entry leaves intact, and runs into the first-call path. The
        // rungs overlap, so they form a ladder: another guarded getter with the same code matches both, and an
        // ambiguous rung fails closed.
        const Candidate GAME_CONTEXT[] = {
            Candidate::direct(
                "GameContext_P1_GuardedStatic",
                Pattern::literal(
                    "40 53 48 83 EC 20 8B 0D ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 BA 18 00 00 00 48 8B 04 C8 "
                    "8B 04 02 39 05 ?? ?? ?? ?? [2-6] 48 8B 05 ?? ?? ?? ?? 48 83 C4 20 5B C3"
                )
            ),
            Candidate::direct(
                "GameContext_P2_EpochToInit",
                Pattern::literal(
                    "8B 0D ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 BA 18 00 00 00 48 8B 04 C8 8B 04 02 "
                    "39 05 ?? ?? ?? ?? [2-6] 48 8B 05 ?? ?? ?? ?? 48 83 C4 20 5B C3 48 8D 0D"
                ),
                -0x06
            ),
        };

        // C_LockPicking::Update(dt). P1 is the prologue: it points rdi at the state (+0x88), keeps dt in xmm6 and
        // branches on states 3 to 5. P2 is the play-state body: it adds dt to +0xB8, then makes the three update calls
        // and loads the hearing emitter (+0x190) for its step.
        const Candidate LOCKPICKING_UPDATE[] = {
            Candidate::direct(
                "LockPickingUpdate_P1_Prologue",
                Pattern::literal(
                    "48 89 5C 24 08 57 48 83 EC 30 48 8D B9 88 00 00 00 0F 29 74 24 20 8A 07 0F 28 F1 48 8B D9 3C 03 "
                    "[2-6] 2C 04 3C 01"
                )
            ),
            Candidate::direct(
                "LockPickingUpdate_P2_PlayBody",
                Pattern::literal(
                    "0F 28 C6 F3 0F 58 81 B8 00 00 00 F3 0F 11 81 B8 00 00 00 E8 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? "
                    "48 8B CB E8 ?? ?? ?? ?? 48 8B 8B 90 01 00 00 0F 28 CE E8"
                ),
                -0x28
            ),
        };

        // The end of the failure state (OnFailAnimDone). P1 is the prologue, which compares the state (+0x88) against
        // 8. P2 is the code past that branch: +0x94 goes to xmm1 for a call, a second call follows, then +0x128 goes
        // to xmm2.
        const Candidate FAIL_ANIM_DONE[] = {
            Candidate::direct(
                "FailAnimDone_P1_Prologue",
                Pattern::literal("48 89 5C 24 08 57 48 83 EC 20 48 8D B9 88 00 00 00 48 8B D9 80 3F 08")
            ),
            Candidate::direct(
                "FailAnimDone_P2_CallPair",
                Pattern::literal("F3 0F 10 89 94 00 00 00 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? F3 0F 10 93 28 01 00 00"),
                -0x1D
            ),
        };

        // IAction::ForceFinish. P1 is the prologue, which clears the Interruptable flag (+0x2C bit 2). P2 reads +0x88,
        // keeps this in rdi and writes the finished status (4) to +0x28.
        const Candidate FORCE_FINISH[] = {
            Candidate::direct(
                "ForceFinish_P1_Prologue",
                Pattern::literal("48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 83 61 2C FB")
            ),
            Candidate::direct(
                "ForceFinish_P2_FinishedStatus",
                Pattern::literal("33 DB 48 8B 81 88 00 00 00 48 8B F9 C7 41 28 04 00 00 00"),
                -0x13
            ),
        };

        // C_UIHudMessages::OnLockpickingMessage. P1 is ConnectMinigame's lea of the handler, right before it connects
        // the handler to the minigame's HUD-message signal (+0x70). P2 is the handler's body: it skips code 0, then
        // indexes the message keys at +0x1F0 by the code.
        const Candidate HUD_LOCKPICKING_MESSAGE[] = {
            Candidate::rip_relative(
                "HudLockpickingMessage_P1_ConnectLea",
                Pattern::literal(
                    "| 48 8D 05 ?? ?? ?? ?? 48 8B CF 48 89 45 F8 E8 ?? ?? ?? ?? 48 8D 4B 70 48 89 45 F0 48 8D 55 F0 E8"
                ),
                RIP_DISP32_OFFSET,
                RIP_INSTRUCTION_LENGTH
            ),
            Candidate::direct(
                "HudLockpickingMessage_P2_Body",
                Pattern::literal("84 D2 [2-6] 53 48 83 EC 20 48 8B D9 0F B6 C2 48 81 C1 F0 01 00 00 48 8D 14 C1")
            ),
        };

        /** @brief What an anchor names, which decides the validator it must pass. */
        enum class Role : std::uint8_t
        {
            /// A function entry the mod calls or hooks.
            Code,
            /// A global the mod reads through.
            Data,
        };

        DMK::Region s_image{};

        /** @brief True when @p value is an address inside the game image. */
        [[nodiscard]] bool in_image(std::int64_t value, const void *context) noexcept
        {
            const auto *image = static_cast<const DMK::Region *>(context);
            return image != nullptr && image->size != 0 && value > 0 &&
                   image->contains(DMK::Address{static_cast<std::uintptr_t>(value)});
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
         *        prologue and agrees with the unwind entry that covers it.
         * @details A body rung whose walk-back drifts into the middle of a function, or a lea that loads another
         *          address, fails here, because the unwind entry's recorded start then differs. A leaf function
         *          without unwind metadata keeps the prologue probe as its evidence.
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
                .validator_context = &s_image,
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

        /** @brief A 2-of-N quorum row over @p members. The validator runs once more on the corroborated value. */
        [[nodiscard]] Anchor
        make_quorum(std::string_view label, std::span<const Anchor *const> members, Role role) noexcept
        {
            return Anchor{
                .label = label,
                .kind = AnchorKind::Quorum,
                .validator = validator_for(role),
                .validator_context = &s_image,
                .quorum_members = members,
                .quorum_threshold = 2,
            };
        }

        const auto GENV_VOTES = single_rung_votes("Genv", GENV, Role::Data);
        const auto GENV_VOTE_POINTERS = pointers_to(GENV_VOTES);
        const auto UPDATE_VOTES = single_rung_votes("LockPickingUpdate", LOCKPICKING_UPDATE, Role::Code);
        const auto UPDATE_VOTE_POINTERS = pointers_to(UPDATE_VOTES);
        const auto EMITTER_STEP_VOTES = single_rung_votes("EmitterStep", EMITTER_STEP, Role::Code);
        const auto EMITTER_STEP_VOTE_POINTERS = pointers_to(EMITTER_STEP_VOTES);
        const auto FAIL_ANIM_DONE_VOTES = single_rung_votes("FailAnimDone", FAIL_ANIM_DONE, Role::Code);
        const auto FAIL_ANIM_DONE_VOTE_POINTERS = pointers_to(FAIL_ANIM_DONE_VOTES);
        const auto FORCE_FINISH_VOTES = single_rung_votes("ForceFinish", FORCE_FINISH, Role::Code);
        const auto FORCE_FINISH_VOTE_POINTERS = pointers_to(FORCE_FINISH_VOTES);
        const auto HUD_MESSAGE_VOTES = single_rung_votes("HudLockpickingMessage", HUD_LOCKPICKING_MESSAGE, Role::Code);
        const auto HUD_MESSAGE_VOTE_POINTERS = pointers_to(HUD_MESSAGE_VOTES);

        /** @brief The registry rows, in ANCHORS order. */
        enum class AnchorId : std::uint8_t
        {
            Genv,
            LockPickingVtable,
            PlayerVtable,
            SoulVtable,
            RpgMinigamesVtable,
            EntitySystemVtable,
            ConsoleVtable,
            InputVtable,
            FlashUiVtable,
            FlashElementVtable,
            FlashPlayerVtable,
            TurnFailVtable,
            LockPickingUpdate,
            EmitterStep,
            GameContext,
            FailAnimDone,
            ForceFinish,
            HudLockpickingMessage,
        };

        const Anchor ANCHORS[] = {
            make_quorum("Genv", GENV_VOTE_POINTERS, Role::Data),
            make_vtable("LockPickingVtable", constants::RTTI_LOCKPICKING),
            make_vtable("PlayerVtable", constants::RTTI_PLAYER),
            make_vtable("SoulVtable", constants::RTTI_SOUL),
            make_vtable("RpgMinigamesVtable", constants::RTTI_RPG_MINIGAMES),
            make_vtable("EntitySystemVtable", constants::RTTI_ENTITY_SYSTEM),
            make_vtable("ConsoleVtable", constants::RTTI_CONSOLE),
            make_vtable("InputVtable", constants::RTTI_INPUT),
            make_vtable("FlashUiVtable", constants::RTTI_FLASH_UI),
            make_vtable("FlashElementVtable", constants::RTTI_FLASH_UI_ELEMENT),
            make_vtable("FlashPlayerVtable", constants::RTTI_FLASH_PLAYER),
            make_vtable("TurnFailVtable", constants::RTTI_TURN_FAIL_ACTION),
            make_quorum("LockPickingUpdate", UPDATE_VOTE_POINTERS, Role::Code),
            make_quorum("EmitterStep", EMITTER_STEP_VOTE_POINTERS, Role::Code),
            make_anchor("GameContext", GAME_CONTEXT, Role::Code),
            make_quorum("FailAnimDone", FAIL_ANIM_DONE_VOTE_POINTERS, Role::Code),
            make_quorum("ForceFinish", FORCE_FINISH_VOTE_POINTERS, Role::Code),
            make_quorum("HudLockpickingMessage", HUD_MESSAGE_VOTE_POINTERS, Role::Code),
        };
        constexpr std::size_t ANCHOR_COUNT = std::size(ANCHORS);
        static_assert(static_cast<std::size_t>(AnchorId::HudLockpickingMessage) + 1 == ANCHOR_COUNT);

        [[nodiscard]] constexpr std::size_t index_of(AnchorId id) noexcept
        {
            return static_cast<std::size_t>(id);
        }

        /** @brief A vtable whose layout the mod relies on: its last used slot must still point into the image. */
        struct SlotPin
        {
            AnchorId vtable;
            std::size_t slot_count;
        };

        constexpr SlotPin SLOT_PINS[] = {
            {.vtable = AnchorId::LockPickingVtable, .slot_count = constants::LOCKPICKING_SLOT_COUNT},
            {.vtable = AnchorId::SoulVtable, .slot_count = constants::SOUL_SKILL_LEVEL_SLOT + 1},
            {.vtable = AnchorId::RpgMinigamesVtable, .slot_count = constants::RPG_LOCKPICK_NOISE_SLOT + 1},
            {.vtable = AnchorId::EntitySystemVtable, .slot_count = constants::ENTITY_SYSTEM_GET_ENTITY_SLOT + 1},
            {.vtable = AnchorId::ConsoleVtable, .slot_count = constants::CONSOLE_SLOT_COUNT},
            {.vtable = AnchorId::InputVtable, .slot_count = constants::INPUT_SLOT_COUNT},
            {.vtable = AnchorId::FlashElementVtable, .slot_count = constants::ELEMENT_GET_PLAYER_SLOT + 1},
            {.vtable = AnchorId::FlashPlayerVtable, .slot_count = constants::PLAYER_INVOKE_SLOT + 1},
        };

        /** @brief A feature and the anchors it needs. It turns on when anchor::evaluate_gate passes over them. */
        struct FeatureSpec
        {
            std::string_view name;
            std::span<const AnchorId> anchors;
            bool Anchors::*flag;
        };

        constexpr AnchorId MINIGAME_ANCHORS[] = {
            AnchorId::Genv,
            AnchorId::LockPickingVtable,
            AnchorId::PlayerVtable,
            AnchorId::RpgMinigamesVtable,
            AnchorId::InputVtable,
            AnchorId::FlashUiVtable,
            AnchorId::FlashElementVtable,
            AnchorId::FlashPlayerVtable,
            AnchorId::LockPickingUpdate,
            AnchorId::EmitterStep,
            AnchorId::GameContext,
        };
        constexpr AnchorId VANILLA_SOUND_ANCHORS[] = {
            AnchorId::Genv,
            AnchorId::EntitySystemVtable,
        };
        constexpr AnchorId SHORT_FAILURE_ANCHORS[] = {
            AnchorId::TurnFailVtable,
            AnchorId::FailAnimDone,
            AnchorId::ForceFinish,
        };
        constexpr AnchorId QUIET_FAILURE_ANCHORS[] = {
            AnchorId::HudLockpickingMessage,
        };
        constexpr AnchorId THIEVERY_ANCHORS[] = {
            AnchorId::SoulVtable,
        };

        const FeatureSpec FEATURES[] = {
            {.name = "Minigame", .anchors = MINIGAME_ANCHORS, .flag = &Anchors::minigame},
            {.name = "VanillaSounds", .anchors = VANILLA_SOUND_ANCHORS, .flag = &Anchors::vanilla_sounds},
            {.name = "ShortFailure", .anchors = SHORT_FAILURE_ANCHORS, .flag = &Anchors::short_failure},
            {.name = "QuietFailure", .anchors = QUIET_FAILURE_ANCHORS, .flag = &Anchors::quiet_failure},
            {.name = "Thievery", .anchors = THIEVERY_ANCHORS, .flag = &Anchors::thievery},
        };
        /// The longest feature list, the size of the report each gate reads.
        constexpr std::size_t MAX_FEATURE_ANCHORS = std::max({
            std::size(MINIGAME_ANCHORS),
            std::size(VANILLA_SOUND_ANCHORS),
            std::size(SHORT_FAILURE_ANCHORS),
            std::size(QUIET_FAILURE_ANCHORS),
            std::size(THIEVERY_ANCHORS),
        });

        /**
         * @brief Reads slot @p index of @p vtable, or 0 when it is not code of the game image.
         * @details The word past the end of a shorter vtable is the next vtable's object locator, which also lies in
         *          the image, so only an executable target counts as a slot.
         */
        [[nodiscard]] std::uintptr_t slot_of(std::uintptr_t vtable, std::size_t index, DMK::Region game) noexcept
        {
            if (vtable == 0)
            {
                return 0;
            }
            // The vtable sits in the image's read-only data, which stays mapped for the process lifetime.
            const std::uintptr_t target = reinterpret_cast<const std::uintptr_t *>(vtable)[index];
            return game.contains(DMK::Address{target}) && on_executable_page(target) ? target : 0;
        }

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
            DMK::log().info(
                "Anchor {} at {} (RVA {:#x})",
                entry.label,
                DMK::format::format_address(address),
                address - game.base.raw()
            );
        }
    } // namespace

    Anchors resolve_anchors(DMK::Region game)
    {
        std::array<ResolvedAnchor, ANCHOR_COUNT> report{};
        s_image = game;
        (void)DMK::anchor::resolve_all(ANCHORS, report, game);

        for (const SlotPin &pin : SLOT_PINS)
        {
            ResolvedAnchor &entry = report[index_of(pin.vtable)];
            if (entry.status == AnchorStatus::Resolved &&
                slot_of(static_cast<std::uintptr_t>(entry.value), pin.slot_count - 1, game) == 0)
            {
                DMK::log().error("Anchor {} is shorter than {} slots", entry.label, pin.slot_count);
                entry.status = AnchorStatus::Failed;
                entry.value = 0;
            }
        }

        const auto value_of = [&report](AnchorId id) -> std::uintptr_t
        {
            const ResolvedAnchor &entry = report[index_of(id)];
            return entry.status == AnchorStatus::Resolved ? static_cast<std::uintptr_t>(entry.value) : 0;
        };

        // The hooked Update must be the vtable's own slot. A shifted vtable or a ladder that drifted onto another
        // function fails the hook, so no hook patches the wrong code.
        ResolvedAnchor &update = report[index_of(AnchorId::LockPickingUpdate)];
        if (update.status == AnchorStatus::Resolved &&
            slot_of(value_of(AnchorId::LockPickingVtable), constants::LOCKPICKING_UPDATE_SLOT, game) !=
                static_cast<std::uintptr_t>(update.value))
        {
            DMK::log().error(
                "Anchor {} is not slot {} of the C_LockPicking vtable",
                update.label,
                constants::LOCKPICKING_UPDATE_SLOT
            );
            update.status = AnchorStatus::Failed;
            update.value = 0;
        }

        for (const ResolvedAnchor &entry : report)
        {
            log_entry(entry, game);
        }

        Anchors anchors{
            .genv = value_of(AnchorId::Genv),
            .lockpicking_vtable = value_of(AnchorId::LockPickingVtable),
            .lockpicking_update = value_of(AnchorId::LockPickingUpdate),
            .player_vtable = value_of(AnchorId::PlayerVtable),
            .soul_vtable = value_of(AnchorId::SoulVtable),
            .rpg_minigames_vtable = value_of(AnchorId::RpgMinigamesVtable),
            .entity_system_vtable = value_of(AnchorId::EntitySystemVtable),
            .console_vtable = value_of(AnchorId::ConsoleVtable),
            .input_vtable = value_of(AnchorId::InputVtable),
            .flash_ui_vtable = value_of(AnchorId::FlashUiVtable),
            .flash_element_vtable = value_of(AnchorId::FlashElementVtable),
            .flash_player_vtable = value_of(AnchorId::FlashPlayerVtable),
            .turn_fail_vtable = value_of(AnchorId::TurnFailVtable),
            .emitter_step = value_of(AnchorId::EmitterStep),
            .game_context = value_of(AnchorId::GameContext),
            .on_fail_anim_done = value_of(AnchorId::FailAnimDone),
            .action_force_finish = value_of(AnchorId::ForceFinish),
            .hud_lockpicking_message = value_of(AnchorId::HudLockpickingMessage),
        };

        for (const FeatureSpec &feature : FEATURES)
        {
            std::array<ResolvedAnchor, MAX_FEATURE_ANCHORS> entries{};
            std::size_t count = 0;
            for (const AnchorId id : feature.anchors)
            {
                entries[count++] = report[index_of(id)];
            }
            const GateVerdict verdict = DMK::anchor::evaluate_gate(std::span{entries.data(), count});
            anchors.*feature.flag = verdict != GateVerdict::Fail;
            if (verdict == GateVerdict::Fail)
            {
                DMK::log().warning("Feature gate: {} Fail, so the feature is off", feature.name);
            }
            else
            {
                DMK::log()
                    .info("Feature gate: {} {}", feature.name, verdict == GateVerdict::Pass ? "Pass" : "Degraded");
            }
        }
        return anchors;
    }
} // namespace OblivionLockpicking
