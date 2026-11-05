/**
 * @file anchors.cpp
 * @brief The anchor registry: candidate ladders, validators, vtable identities and the feature gate.
 *
 * gEnv, the root of the Flash UI reads, is a 2-of-3 quorum of three load sites. The HUD mask static is a 2-of-2 quorum
 * of its loads in two unrelated functions. The game context static and the hooked CharacterManager::Update are each a
 * ladder of two overlapping rungs, and the hooked function must also equal slot 18 of the RTTI-resolved
 * CharacterManager vtable. DMK anchor::resolve takes a rung only when it matches exactly once, inside WHGame.dll and on
 * executable pages, and the value passes the validator: a function entry that agrees with its unwind entry, or a
 * readable non-code global. Every object read through these statics is checked against its RTTI vtable at each use.
 * The minigame and photo mode reads are features with their own gates, so a build without one of their classes keeps
 * the clock.
 *
 * Signature rules: every relative target, RIP displacement and virtual-call slot is a wildcard, every branch inside a
 * pattern is a bounded gap ([2-6] covers the rel8 and rel32 forms), and no pattern crosses the padding between
 * functions. Every rung matches exactly once on the Steam 1.5.6, GOG 1.5 and Game Pass 1.4 builds.
 */

#include "anchors.hpp"
#include "constants.hpp"

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace HUDClock
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

        // The C_UIHudMask singleton static. P1 is the wh_ui_ShowHud callback, which loads it, then the hud element
        // (+0x48) and shows or hides that element by the CVar's value (`setnle dl`). P2 is the mask's refresh of one
        // HUD part, which loads it, takes the part's name (+0x0A of a virtual call's result) and applies the part.
        const Candidate HUD_MASK[] = {
            Candidate::rip_relative(
                "HudMask_P1_ShowHudCallback",
                Pattern::literal(
                    "40 53 48 83 EC 20 | 48 8B 1D ?? ?? ?? ?? 48 85 DB [2-6] 48 8B 01 FF 50 ?? 48 8B 4B 48 48 85 C9 "
                    "[2-6] 85 C0 48 8B 01 0F 9F C2 FF 90 ?? ?? ?? ??"
                ),
                RIP_DISP32_OFFSET,
                RIP_INSTRUCTION_LENGTH
            ),
            Candidate::rip_relative(
                "HudMask_P2_PartRefresh",
                Pattern::literal(
                    "40 53 48 83 EC 20 | 48 8B 1D ?? ?? ?? ?? 48 85 DB [2-6] 48 8B 01 FF 50 ?? 48 8D 54 24 38 48 8D 48 "
                    "0A E8"
                ),
                RIP_DISP32_OFFSET,
                RIP_INSTRUCTION_LENGTH
            ),
        };

        // The game context getter, a guarded static: its fast path compares the TLS epoch, then loads the cached
        // pointer, the static these rungs name. P1 starts at the entry. P2 starts after the 6-byte frame setup, which a
        // 5-byte jump on the entry leaves intact, and runs into the first-call path. The static is read, never the
        // getter called, because a call before the game creates the context would create it out of the game's order.
        const Candidate GAME_CONTEXT[] = {
            Candidate::rip_relative(
                "GameContext_P1_GuardedStatic",
                Pattern::literal(
                    "40 53 48 83 EC 20 8B 0D ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 BA 18 00 00 00 48 8B 04 C8 "
                    "8B 04 02 39 05 ?? ?? ?? ?? [2-6] | 48 8B 05 ?? ?? ?? ?? 48 83 C4 20 5B C3"
                ),
                RIP_DISP32_OFFSET,
                RIP_INSTRUCTION_LENGTH
            ),
            Candidate::rip_relative(
                "GameContext_P2_EpochToInit",
                Pattern::literal(
                    "8B 0D ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 BA 18 00 00 00 48 8B 04 C8 8B 04 02 "
                    "39 05 ?? ?? ?? ?? [2-6] | 48 8B 05 ?? ?? ?? ?? 48 83 C4 20 5B C3 48 8D 0D"
                ),
                RIP_DISP32_OFFSET,
                RIP_INSTRUCTION_LENGTH
            ),
        };

        // CharacterManager::Update(bool paused), vtable slot 18 (Steam 0x18052FC38, GOG 0x18054F588, Game Pass
        // 0x1803CC188). P1 is the frame (`mov rax, rsp`, the two saves, three pushes, two xmm saves) through the
        // argument moves (`mov bl, dl`, `mov rdi, rcx`) and the paused-flag store. P2 starts at those moves (+0x1A) and
        // runs on through two calls, the update counter (`inc dword [rdi+70h]`) and the 16.0f store.
        const Candidate CHARACTER_MANAGER_UPDATE[] = {
            Candidate::direct(
                "CharacterManagerUpdate_P1_FrameArgMoves",
                Pattern::literal(
                    "48 8B C4 48 89 58 08 48 89 68 18 56 57 41 56 48 81 EC ?? ?? ?? ?? 0F 29 70 D8 8A DA 0F 29 78 C8 "
                    "48 8B F9 88 15 ?? ?? ?? ?? E8"
                )
            ),
            Candidate::direct(
                "CharacterManagerUpdate_P2_CounterBody",
                Pattern::literal(
                    "8A DA 0F 29 78 C8 48 8B F9 88 15 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 48 8B 0D ?? "
                    "?? ?? ?? FF 47 70 C7 05 ?? ?? ?? ?? 00 00 80 41"
                ),
                -0x1A
            ),
        };

        /** @brief What an anchor names, which decides the validator it must pass. */
        enum class Role : std::uint8_t
        {
            /// A function entry the mod hooks.
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
         * @details A body rung whose walk-back drifts into the middle of a function fails here, because the unwind
         *          entry's recorded start then differs.
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
        const auto HUD_MASK_VOTES = single_rung_votes("HudMaskGlobal", HUD_MASK, Role::Data);
        const auto HUD_MASK_VOTE_POINTERS = pointers_to(HUD_MASK_VOTES);

        /** @brief The registry rows, in ANCHORS order. */
        enum class AnchorId : std::uint8_t
        {
            Genv,
            FlashUiVtable,
            FlashElementVtable,
            FlashPlayerVtable,
            HudMaskVtable,
            RpgModuleVtable,
            CalendarVtable,
            CharacterManagerVtable,
            PlayerModuleVtable,
            PlayerVtable,
            AlchemyVtable,
            BlacksmithingVtable,
            DiceVtable,
            ReadingVtable,
            LockPickingVtable,
            PickpocketingVtable,
            HerbGatheringVtable,
            HoleDiggingVtable,
            PhotomodeManagerVtable,
            HudMaskGlobal,
            GameContextGlobal,
            CharacterManagerUpdate,
        };

        const Anchor ANCHORS[] = {
            make_quorum("Genv", GENV_VOTE_POINTERS, Role::Data),
            make_vtable("FlashUiVtable", constants::RTTI_FLASH_UI),
            make_vtable("FlashElementVtable", constants::RTTI_FLASH_UI_ELEMENT),
            make_vtable("FlashPlayerVtable", constants::RTTI_FLASH_PLAYER),
            make_vtable("HudMaskVtable", constants::RTTI_HUD_MASK),
            make_vtable("RpgModuleVtable", constants::RTTI_RPG_MODULE),
            make_vtable("CalendarVtable", constants::RTTI_CALENDAR),
            make_vtable("CharacterManagerVtable", constants::RTTI_CHARACTER_MANAGER),
            make_vtable("PlayerModuleVtable", constants::RTTI_PLAYER_MODULE),
            make_vtable("PlayerVtable", constants::RTTI_PLAYER),
            make_vtable("AlchemyVtable", constants::RTTI_ALCHEMY),
            make_vtable("BlacksmithingVtable", constants::RTTI_BLACKSMITHING),
            make_vtable("DiceVtable", constants::RTTI_DICE),
            make_vtable("ReadingVtable", constants::RTTI_READING),
            make_vtable("LockPickingVtable", constants::RTTI_LOCK_PICKING),
            make_vtable("PickpocketingVtable", constants::RTTI_PICKPOCKETING),
            make_vtable("HerbGatheringVtable", constants::RTTI_HERB_GATHERING),
            make_vtable("HoleDiggingVtable", constants::RTTI_HOLE_DIGGING),
            make_vtable("PhotomodeManagerVtable", constants::RTTI_PHOTOMODE_MANAGER),
            make_quorum("HudMaskGlobal", HUD_MASK_VOTE_POINTERS, Role::Data),
            make_anchor("GameContextGlobal", GAME_CONTEXT, Role::Data),
            make_anchor("CharacterManagerUpdate", CHARACTER_MANAGER_UPDATE, Role::Code),
        };
        constexpr std::size_t ANCHOR_COUNT = std::size(ANCHORS);
        static_assert(static_cast<std::size_t>(AnchorId::CharacterManagerUpdate) + 1 == ANCHOR_COUNT);

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
            {.vtable = AnchorId::FlashElementVtable, .slot_count = constants::ELEMENT_GET_PLAYER_SLOT + 1},
            {.vtable = AnchorId::FlashPlayerVtable, .slot_count = constants::PLAYER_INVOKE_SLOT + 1},
            {.vtable = AnchorId::CharacterManagerVtable, .slot_count = constants::CHARACTER_MANAGER_UPDATE_SLOT + 1},
        };

        /// Every anchor the clock needs. The mod has no other feature.
        constexpr AnchorId CLOCK_ANCHORS[] = {
            AnchorId::Genv,
            AnchorId::FlashUiVtable,
            AnchorId::FlashElementVtable,
            AnchorId::FlashPlayerVtable,
            AnchorId::HudMaskVtable,
            AnchorId::RpgModuleVtable,
            AnchorId::CalendarVtable,
            AnchorId::CharacterManagerVtable,
            AnchorId::HudMaskGlobal,
            AnchorId::GameContextGlobal,
            AnchorId::CharacterManagerUpdate,
        };

        /// The class vtable of each core::MINIGAMES entry, in that order.
        constexpr AnchorId MINIGAME_VTABLES[] = {
            AnchorId::AlchemyVtable,
            AnchorId::BlacksmithingVtable,
            AnchorId::DiceVtable,
            AnchorId::ReadingVtable,
            AnchorId::LockPickingVtable,
            AnchorId::PickpocketingVtable,
            AnchorId::HerbGatheringVtable,
            AnchorId::HoleDiggingVtable,
        };
        static_assert(std::size(MINIGAME_VTABLES) == core::MINIGAMES.size());

        /// Every anchor the minigame read needs: the player module, the player and each minigame class.
        constexpr AnchorId MINIGAME_ANCHORS[] = {
            AnchorId::GameContextGlobal,
            AnchorId::PlayerModuleVtable,
            AnchorId::PlayerVtable,
            AnchorId::AlchemyVtable,
            AnchorId::BlacksmithingVtable,
            AnchorId::DiceVtable,
            AnchorId::ReadingVtable,
            AnchorId::LockPickingVtable,
            AnchorId::PickpocketingVtable,
            AnchorId::HerbGatheringVtable,
            AnchorId::HoleDiggingVtable,
        };

        /// Every anchor the photo mode read needs.
        constexpr AnchorId PHOTO_MODE_ANCHORS[] = {
            AnchorId::GameContextGlobal,
            AnchorId::PhotomodeManagerVtable,
        };

        /** @brief Evaluates the feature gate over @p ids, logs the verdict, and returns whether the feature is on. */
        template <std::size_t N>
        [[nodiscard]] bool
        gate_feature(std::string_view feature, const AnchorId (&ids)[N], std::span<const ResolvedAnchor> report)
        {
            std::array<ResolvedAnchor, N> entries{};
            for (std::size_t i = 0; i < N; ++i)
            {
                entries[i] = report[index_of(ids[i])];
            }
            const GateVerdict verdict = DMK::anchor::evaluate_gate(entries);
            if (verdict == GateVerdict::Fail)
            {
                DMK::log().warning("Feature gate: {} Fail", feature);
                return false;
            }
            DMK::log().info("Feature gate: {} {}", feature, verdict == GateVerdict::Pass ? "Pass" : "Degraded");
            return true;
        }

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
        ResolvedAnchor &update = report[index_of(AnchorId::CharacterManagerUpdate)];
        if (update.status == AnchorStatus::Resolved &&
            slot_of(value_of(AnchorId::CharacterManagerVtable), constants::CHARACTER_MANAGER_UPDATE_SLOT, game) !=
                static_cast<std::uintptr_t>(update.value))
        {
            DMK::log().error(
                "Anchor {} is not slot {} of the CharacterManager vtable",
                update.label,
                constants::CHARACTER_MANAGER_UPDATE_SLOT
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
            .flash_ui_vtable = value_of(AnchorId::FlashUiVtable),
            .flash_element_vtable = value_of(AnchorId::FlashElementVtable),
            .flash_player_vtable = value_of(AnchorId::FlashPlayerVtable),
            .hud_mask_vtable = value_of(AnchorId::HudMaskVtable),
            .hud_mask_global = value_of(AnchorId::HudMaskGlobal),
            .game_context_global = value_of(AnchorId::GameContextGlobal),
            .rpg_module_vtable = value_of(AnchorId::RpgModuleVtable),
            .calendar_vtable = value_of(AnchorId::CalendarVtable),
            .character_manager_update = value_of(AnchorId::CharacterManagerUpdate),
            .player_module_vtable = value_of(AnchorId::PlayerModuleVtable),
            .player_vtable = value_of(AnchorId::PlayerVtable),
            .photomode_manager_vtable = value_of(AnchorId::PhotomodeManagerVtable),
        };
        for (std::size_t i = 0; i < anchors.minigame_vtables.size(); ++i)
        {
            anchors.minigame_vtables[i] = value_of(MINIGAME_VTABLES[i]);
        }
        anchors.clock = gate_feature("Clock", CLOCK_ANCHORS, report);
        anchors.minigames = gate_feature("Minigames", MINIGAME_ANCHORS, report);
        anchors.photo_mode = gate_feature("PhotoMode", PHOTO_MODE_ANCHORS, report);
        return anchors;
    }
} // namespace HUDClock
