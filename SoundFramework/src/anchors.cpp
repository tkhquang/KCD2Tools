/**
 * @file anchors.cpp
 * @brief The anchor registry: candidate ladders, validators, vtable identities and the feature gates.
 *
 * Each function resolves from a ladder of byte signatures. DMK anchor::resolve takes the first rung that matches
 * exactly once, inside WHGame.dll and on executable pages only, and the result must pass a validator: a function entry
 * on an executable page that agrees with its unwind entry. gEnv and the game context slot are the roots of every other
 * read, so each resolves as a 2-of-3 quorum of independent load sites.
 *
 * Signature rules: every relative target, RIP displacement and virtual-call slot is a wildcard, every branch inside a
 * pattern is a bounded gap ([2-6] covers the rel8 and rel32 forms), and no pattern crosses the padding between
 * functions. Each function ladder holds a rung that starts past the prologue, so an inline hook by another mod on the
 * function entry cannot hide every rung.
 */

#include "anchors.hpp"
#include "constants.hpp"

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace SoundFramework
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

        // gEnv: three loads of the static SSystemGlobalEnvironment, each a RIP-relative disp32 to its first member.
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

        // The game context slot: three loads of the slot, each followed by a read of one of the context's members.
        const Candidate CONTEXT[] = {
            Candidate::rip_relative(
                "Context_P1_ReadSlotCmpFieldE0",
                Pattern::literal("48 8B 05 ?? ?? ?? ?? 48 83 B8 ?? ?? ?? ?? ?? [2-6] 42 8B 04 33"),
                3,
                7
            ),
            Candidate::rip_relative(
                "Context_P2_MovCmp",
                Pattern::literal("48 8B 05 ?? ?? ?? ?? 48 83 B8 ?? ?? ?? ?? ?? [2-6] 42 8B 04 3F"),
                3,
                7
            ),
            Candidate::rip_relative(
                "Context_P3_ReadSlotField278Test",
                Pattern::literal("48 8B 05 ?? ?? ?? ?? 48 8B 8F 78 02 00 00 48 8B B0 F8 00 00 00 48 85 C9"),
                3,
                7
            ),
        };

        // Speak(speak_man, request): asks the owning actor whether it may speak (a virtual call) or reads the
        // request's force flag (+0x1B), then copies the current request (+0x48) to the previous one (+0x10) and the
        // new request to the current one, and raises the lip-sync flag (+0x91).
        const Candidate SPEAK[] = {
            Candidate::direct(
                "Speak_P1_Prologue",
                Pattern::literal(
                    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 48 8B F9 0F 29 74 24 30 48 8B 49 08 48 8B F2 48 8B "
                    "01 FF 90 ?? ?? ?? ?? 84 C0 [2-6] 38 46 1B [2-6] 48 8D 5F 48 48 8B D3 48 8D 4F 10 E8"
                )
            ),
            Candidate::direct(
                "Speak_P2_CopyRequest",
                Pattern::literal(
                    "48 8D 5F 48 48 8B D3 48 8D 4F 10 E8 ?? ?? ?? ?? 48 8B D6 48 8B CB E8 ?? ?? ?? ?? 48 8B 13 83 7A "
                    "F8 00 [2-6] 48 8D 8F 98 00 00 00 C6 87 91 00 00 00 01"
                ),
                -0x34
            ),
        };

        // SpeakRequest's constructor: three empty game strings (+0x00, +0x08, +0x10), then the default flags.
        const Candidate REQUEST_CTOR[] = {
            Candidate::direct(
                "RequestCtor_P1_Body",
                Pattern::literal(
                    "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 83 C0 0C 48 89 03 E8 ?? ?? ?? ?? 48 83 C0 0C 48 89 "
                    "43 08 E8 ?? ?? ?? ?? 48 83 C0 0C 48 89 43 10 33 C0 88 43 1B"
                )
            ),
            Candidate::direct(
                "RequestCtor_P2_Defaults",
                Pattern::literal(
                    "33 C0 88 43 1B 48 89 43 20 88 43 28 48 8B C3 66 C7 43 19 01 00 C7 43 34 00 00 80 BF 48 83 C4 20 "
                    "5B C3"
                ),
                -0x2F
            ),
        };

        // CryString assignment from a C string: the source length, then the copy, returning the string.
        const Candidate STRING_ASSIGN[] = {
            Candidate::direct(
                "StringAssign_P1_Body",
                Pattern::literal(
                    "40 53 48 83 EC 20 48 8B D9 4C 8B CA 48 8B CA E8 ?? ?? ?? ?? 4C 8B C0 49 8B D1 48 8B CB E8 ?? ?? "
                    "?? ?? 48 8B C3 48 83 C4 20 5B C3"
                )
            ),
            Candidate::direct(
                "StringAssign_P2_LengthThenCopy",
                Pattern::literal("E8 ?? ?? ?? ?? 4C 8B C0 49 8B D1 48 8B CB E8 ?? ?? ?? ?? 48 8B C3 48 83 C4 20 5B C3"),
                -0x0F
            ),
        };

        // SpeakRequest's destructor: releases the strings at +0x10, +0x08 and +0x00 (each header 12 bytes before
        // its text). The function is too short and generic for a unique rung past 14 bytes, so the second rung starts
        // right after the 9-byte prologue, which a 5-byte jump on the entry leaves intact.
        const Candidate REQUEST_DTOR[] = {
            Candidate::direct(
                "RequestDtor_P1_Body",
                Pattern::literal(
                    "40 53 48 83 EC 20 48 8B D9 48 8B 49 10 48 83 E9 0C E8 ?? ?? ?? ?? 48 8B 4B 08 48 83 E9 0C E8 ?? "
                    "?? ?? ?? 48 8B 0B 48 83 E9 0C 48 83 C4 20 5B E9"
                )
            ),
            Candidate::direct(
                "RequestDtor_P2_ReleaseStrings",
                Pattern::literal(
                    "48 8B 49 10 48 83 E9 0C E8 ?? ?? ?? ?? 48 8B 4B 08 48 83 E9 0C E8 ?? ?? ?? ?? 48 8B 0B 48 83 E9 "
                    "0C 48 83 C4 20 5B E9"
                ),
                -0x09
            ),
        };

        // The speaker's stop: when the current line (+0x48) is set, stops its file through the audio proxy, empties
        // the path, and clears the playing sound id (+0x8C) and the start-pending flag (+0x90).
        const Candidate SPEAK_STOP[] = {
            Candidate::direct(
                "SpeakStop_P1_Body",
                Pattern::literal(
                    "48 89 5C 24 08 57 48 83 EC 20 48 8D 79 48 48 8B D9 48 8B 07 83 78 F8 00 [2-6] 48 8B CB E8 ?? ?? "
                    "?? ?? 48 8D 8B 98 00 00 00 E8 ?? ?? ?? ?? 48 8B 4B 68 48 85 C9"
                )
            ),
            Candidate::direct(
                "SpeakStop_P2_ClearLine",
                Pattern::literal(
                    "41 B8 01 00 00 00 48 8B C8 48 8B 10 4C 8B 8A ?? ?? ?? ?? 48 8B 17 41 FF D1 48 8D 15 ?? ?? ?? ?? "
                    "48 8B CF E8 ?? ?? ?? ?? C7 83 8C 00 00 00 00 00 00 00 C6 83 90 00 00 00 00"
                ),
                -0x4E
            ),
        };

        // The dialog line player's call into Speak, right after it shows the line's subtitle. The anchor is the
        // return address: one rung ends at it, the other starts at it, so the two spans are independent votes.
        const Candidate DIALOG_SPEAK_RETURN[] = {
            Candidate::direct(
                "DialogSpeakReturn_P1_AfterCall",
                Pattern::literal(
                    "48 8D 55 ?? 49 8B CD E8 ?? ?? ?? ?? | 48 8B 8B ?? ?? 00 00 E8 ?? ?? ?? ?? 48 85 C0 [2-6] 48 8B "
                    "C8 E8"
                )
            ),
            Candidate::direct(
                "DialogSpeakReturn_P2_BeforeCall",
                Pattern::literal("48 8B 55 ?? 49 8B CF E8 ?? ?? ?? ?? 48 8D 4D ?? E8 ?? ?? ?? ?? |"),
                0x0C
            ),
        };

        // SaveGameManager::LoadCryEngineData(manager, chunk, save path): the engine-data stage of every save load,
        // same-level reloads included. P1 is the prologue, the argument moves and the load of the function's own name
        // string ("SaveGameManager::LoadCryEngineData"). P2 is that string's release (`add rcx, -0Ch`), the load of
        // the manager's +0x178 member and the loading flag store (`mov byte [rsi+190h], 1`), walking back 0x2E.
        const Candidate SAVE_GAME_LOAD[] = {
            Candidate::direct(
                "SaveGameLoad_P1_Args",
                Pattern::literal(
                    "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B DA 48 8B F1 48 8D 15 ?? ?? ?? ?? 49 8B F8 48 "
                    "8D 4C 24 30"
                )
            ),
            Candidate::direct(
                "SaveGameLoad_P2_LoadingFlag",
                Pattern::literal(
                    "48 C7 44 24 ?? 00 00 00 00 48 8D 4C 24 ?? E8 ?? ?? ?? ?? 48 8B 4C 24 ?? 48 83 C1 F4 E8 ?? ?? ?? "
                    "?? 48 8B 8E 78 01 00 00 48 8B D3 C6 86 90 01 00 00 01 E8"
                ),
                -0x2E
            ),
        };

        /** @brief What an anchor names, which decides the validator it must pass. */
        enum class Role : std::uint8_t
        {
            /// A function entry the framework calls or hooks.
            Code,
            /// A global the framework reads through.
            Data,
            /// The return address of a call into Speak.
            SpeakReturn,
        };

        /** @brief What every validator checks against: the game image, and Speak once it resolved. */
        struct ValidatorContext
        {
            DMK::Region image{};
            std::uintptr_t speak = 0;
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
         *          recorded start then differs, and the anchor fails closed. A leaf function without unwind metadata
         *          keeps the prologue probe as its evidence.
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

        /**
         * @brief Validator of the dialog return address: it follows an E8 call (rel32) whose target is Speak.
         * @details Both rungs end or start at the address, so this check ties the vote to the call it must name.
         */
        [[nodiscard]] bool speak_return_valid(std::int64_t value, const void *context) noexcept
        {
            const auto *validation = static_cast<const ValidatorContext *>(context);
            constexpr std::size_t call_length = 5;
            if (!in_image(value, context) || validation->speak == 0)
            {
                return false;
            }
            const auto address = static_cast<std::uintptr_t>(value);
            if (!on_executable_page(address - call_length))
            {
                return false;
            }
            const auto *call = reinterpret_cast<const std::uint8_t *>(address - call_length);
            std::int32_t rel32 = 0;
            std::memcpy(&rel32, call + 1, sizeof(rel32));
            return call[0] == 0xE8 && address + static_cast<std::intptr_t>(rel32) == validation->speak;
        }

        [[nodiscard]] DMK::anchor::AnchorValidator validator_for(Role role) noexcept
        {
            switch (role)
            {
            case Role::Code:
                return code_target_valid;
            case Role::Data:
                return data_target_valid;
            case Role::SpeakReturn:
            default:
                return speak_return_valid;
            }
        }

        /** @brief One ladder-backed row; every rung is an instruction, so the scan stays on executable pages. */
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
        const auto CONTEXT_VOTES = single_rung_votes("Context", CONTEXT, Role::Data);
        const auto CONTEXT_VOTE_POINTERS = pointers_to(CONTEXT_VOTES);
        const auto DIALOG_RETURN_VOTES = single_rung_votes("DialogSpeakReturn", DIALOG_SPEAK_RETURN, Role::SpeakReturn);
        const auto DIALOG_RETURN_VOTE_POINTERS = pointers_to(DIALOG_RETURN_VOTES);

        /** @brief The registry rows, in ANCHORS order. DialogSpeakReturn stays last: its validator needs Speak. */
        enum class AnchorId : std::uint8_t
        {
            Genv,
            Context,
            ConsoleVtable,
            SystemVtable,
            EntitySystemVtable,
            EntityVtable,
            ActorSystemVtable,
            ActorVtable,
            UiSubtitlesVtable,
            PoseMorphVtable,
            CharacterVtable,
            SkeletonVtable,
            PoseDataVtable,
            TimerVtable,
            CvarStringVtable,
            EventInstanceVtable,
            Speak,
            RequestCtor,
            StringAssign,
            RequestDtor,
            SpeakStop,
            SaveGameLoad,
            DialogSpeakReturn,
        };

        const Anchor ANCHORS[] = {
            make_quorum("Genv", GENV_VOTE_POINTERS, Role::Data),
            make_quorum("Context", CONTEXT_VOTE_POINTERS, Role::Data),
            make_vtable("ConsoleVtable", constants::RTTI_CONSOLE),
            make_vtable("SystemVtable", constants::RTTI_SYSTEM),
            make_vtable("EntitySystemVtable", constants::RTTI_ENTITY_SYSTEM),
            make_vtable("EntityVtable", constants::RTTI_ENTITY),
            make_vtable("ActorSystemVtable", constants::RTTI_ACTOR_SYSTEM),
            make_vtable("ActorVtable", constants::RTTI_ACTOR),
            make_vtable("UiSubtitlesVtable", constants::RTTI_UI_SUBTITLES),
            make_vtable("PoseMorphVtable", constants::RTTI_POSE_MORPH),
            make_vtable("CharacterVtable", constants::RTTI_CHARACTER),
            make_vtable("SkeletonVtable", constants::RTTI_SKELETON),
            make_vtable("PoseDataVtable", constants::RTTI_POSE_DATA),
            make_vtable("TimerVtable", constants::RTTI_TIMER),
            make_vtable("CvarStringVtable", constants::RTTI_CVAR_STRING),
            make_vtable("EventInstanceVtable", constants::RTTI_EVENT_INSTANCE),
            make_anchor("Speak", SPEAK, Role::Code),
            make_anchor("RequestCtor", REQUEST_CTOR, Role::Code),
            make_anchor("StringAssign", STRING_ASSIGN, Role::Code),
            make_anchor("RequestDtor", REQUEST_DTOR, Role::Code),
            make_anchor("SpeakStop", SPEAK_STOP, Role::Code),
            make_anchor("SaveGameLoad", SAVE_GAME_LOAD, Role::Code),
            make_quorum("DialogSpeakReturn", DIALOG_RETURN_VOTE_POINTERS, Role::SpeakReturn),
        };
        constexpr std::size_t ANCHOR_COUNT = std::size(ANCHORS);
        static_assert(static_cast<std::size_t>(AnchorId::DialogSpeakReturn) + 1 == ANCHOR_COUNT);

        [[nodiscard]] constexpr std::size_t index_of(AnchorId id) noexcept
        {
            return static_cast<std::size_t>(id);
        }

        /** @brief A vtable whose layout the framework relies on: its last used slot must still point into the image. */
        struct SlotPin
        {
            AnchorId vtable;
            std::size_t slot_count;
        };

        constexpr SlotPin SLOT_PINS[] = {
            {AnchorId::ConsoleVtable, constants::CONSOLE_SLOT_COUNT},
            {AnchorId::SystemVtable, constants::SYSTEM_UPDATE_SLOT + 1},
            {AnchorId::EntitySystemVtable, constants::ENTITY_SYSTEM_SLOT_COUNT},
            {AnchorId::EntityVtable, constants::ENTITY_SLOT_COUNT},
            {AnchorId::ActorSystemVtable, constants::ACTOR_SYSTEM_SLOT_COUNT},
            {AnchorId::ActorVtable, constants::ACTOR_SLOT_COUNT},
            {AnchorId::PoseMorphVtable, constants::POSE_MORPH_SLOT_COUNT},
            {AnchorId::CharacterVtable, constants::CHARACTER_SLOT_COUNT},
            {AnchorId::SkeletonVtable, constants::SKELETON_SLOT_COUNT},
            {AnchorId::PoseDataVtable, constants::POSE_DATA_SLOT_COUNT},
            {AnchorId::TimerVtable, constants::TIMER_GET_REAL_FRAME_TIME_SLOT + 1},
            {AnchorId::CvarStringVtable, constants::CVAR_GET_STRING_SLOT + 1},
        };

        /** @brief A feature and the anchors it needs; it turns on when anchor::evaluate_gate passes over them. */
        struct FeatureSpec
        {
            std::string_view name;
            std::span<const AnchorId> anchors;
            bool Anchors::*flag;
        };

        constexpr AnchorId COMMAND_ANCHORS[] = {
            AnchorId::Genv,
            AnchorId::ConsoleVtable,
            AnchorId::SystemVtable,
        };
        constexpr AnchorId ENTITY_ANCHORS[] = {
            AnchorId::Genv,
            AnchorId::EntitySystemVtable,
            AnchorId::EntityVtable,
        };
        constexpr AnchorId SPEECH_ANCHORS[] = {
            AnchorId::Context,
            AnchorId::ActorSystemVtable,
            AnchorId::ActorVtable,
            AnchorId::UiSubtitlesVtable,
            AnchorId::Speak,
            AnchorId::RequestCtor,
            AnchorId::StringAssign,
            AnchorId::RequestDtor,
        };
        constexpr AnchorId DIALOG_HANDOVER_ANCHORS[] = {
            AnchorId::SpeakStop,
            AnchorId::DialogSpeakReturn,
        };
        constexpr AnchorId LOAD_STOP_ANCHORS[] = {
            AnchorId::SaveGameLoad,
            AnchorId::SpeakStop,
        };
        constexpr AnchorId LIP_DRIVE_ANCHORS[] = {
            AnchorId::PoseMorphVtable,
            AnchorId::CharacterVtable,
            AnchorId::SkeletonVtable,
            AnchorId::PoseDataVtable,
        };
        constexpr AnchorId VOICE_CLOCK_ANCHORS[] = {
            AnchorId::Genv,
            AnchorId::TimerVtable,
        };
        constexpr AnchorId CUTSCENE_VOICE_ANCHORS[] = {
            AnchorId::EventInstanceVtable,
        };
        constexpr AnchorId VOICE_LANGUAGE_ANCHORS[] = {
            AnchorId::Genv,
            AnchorId::ConsoleVtable,
            AnchorId::CvarStringVtable,
        };

        const FeatureSpec FEATURES[] = {
            {"Commands", COMMAND_ANCHORS, &Anchors::commands},
            {"Entities", ENTITY_ANCHORS, &Anchors::entities},
            {"Speech", SPEECH_ANCHORS, &Anchors::speech},
            {"DialogHandover", DIALOG_HANDOVER_ANCHORS, &Anchors::dialog_handover},
            {"LoadStop", LOAD_STOP_ANCHORS, &Anchors::load_stop},
            {"LipDrive", LIP_DRIVE_ANCHORS, &Anchors::lip_drive},
            {"VoiceClock", VOICE_CLOCK_ANCHORS, &Anchors::voice_clock},
            {"VoiceLanguage", VOICE_LANGUAGE_ANCHORS, &Anchors::voice_language},
            {"CutsceneVoices", CUTSCENE_VOICE_ANCHORS, &Anchors::cutscene_voices},
        };
        constexpr std::size_t MAX_FEATURE_ANCHORS = std::size(SPEECH_ANCHORS);

        /** @brief Reads slot @p index of @p vtable, or 0 when it does not point into the game image. */
        [[nodiscard]] std::uintptr_t slot_of(std::uintptr_t vtable, std::size_t index, DMK::Region game) noexcept
        {
            if (vtable == 0)
            {
                return 0;
            }
            // The vtable sits in the image's read-only data, which stays mapped for the process lifetime.
            const std::uintptr_t target = reinterpret_cast<const std::uintptr_t *>(vtable)[index];
            return game.contains(DMK::Address{target}) ? target : 0;
        }

        /** @brief Logs one report entry; an unresolved entry keeps a zero value. */
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
        s_validator_context = ValidatorContext{
            .image = game,
        };
        const std::span<const Anchor> table{ANCHORS};
        constexpr std::size_t first_phase = index_of(AnchorId::DialogSpeakReturn);
        (void)DMK::anchor::resolve_all(table.first(first_phase), std::span{report}.first(first_phase), game);
        if (report[index_of(AnchorId::Speak)].status == AnchorStatus::Resolved)
        {
            s_validator_context.speak = static_cast<std::uintptr_t>(report[index_of(AnchorId::Speak)].value);
        }
        report[first_phase] = DMK::anchor::resolve(table[first_phase], game);

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
        for (const ResolvedAnchor &entry : report)
        {
            log_entry(entry, game);
        }

        Anchors anchors{
            .genv = value_of(AnchorId::Genv),
            .context_slot = value_of(AnchorId::Context),
            .console_vtable = value_of(AnchorId::ConsoleVtable),
            .system_vtable = value_of(AnchorId::SystemVtable),
            .system_update = slot_of(value_of(AnchorId::SystemVtable), constants::SYSTEM_UPDATE_SLOT, game),
            .entity_system_vtable = value_of(AnchorId::EntitySystemVtable),
            .entity_vtable = value_of(AnchorId::EntityVtable),
            .actor_system_vtable = value_of(AnchorId::ActorSystemVtable),
            .get_speak_man = slot_of(value_of(AnchorId::ActorVtable), constants::ACTOR_GET_SPEAK_MAN_SLOT, game),
            .speak = value_of(AnchorId::Speak),
            .request_ctor = value_of(AnchorId::RequestCtor),
            .request_dtor = value_of(AnchorId::RequestDtor),
            .string_assign = value_of(AnchorId::StringAssign),
            .speak_stop = value_of(AnchorId::SpeakStop),
            .dialog_speak_return = value_of(AnchorId::DialogSpeakReturn),
            .save_game_load = value_of(AnchorId::SaveGameLoad),
            .timer_vtable = value_of(AnchorId::TimerVtable),
            .ui_subtitles_vtable = value_of(AnchorId::UiSubtitlesVtable),
            .pose_morph_execute =
                slot_of(value_of(AnchorId::PoseMorphVtable), constants::POSE_MORPH_EXECUTE_SLOT, game),
            .character_vtable = value_of(AnchorId::CharacterVtable),
            .skeleton_vtable = value_of(AnchorId::SkeletonVtable),
            .pose_data_vtable = value_of(AnchorId::PoseDataVtable),
            .cvar_string_vtable = value_of(AnchorId::CvarStringVtable),
            .event_instance_vtable = value_of(AnchorId::EventInstanceVtable),
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
                DMK::log().warning("Feature gate: {} Fail; the feature is off", feature.name);
            }
            else
            {
                DMK::log()
                    .info("Feature gate: {} {}", feature.name, verdict == GateVerdict::Pass ? "Pass" : "Degraded");
            }
        }
        // Speech and faces find their actors and characters through entities.
        anchors.speech = anchors.speech && anchors.entities;
        anchors.lip_drive = anchors.lip_drive && anchors.entities;
        // Cutscene lines drive faces like the game's own lines: they need both.
        anchors.cutscene_voices = anchors.cutscene_voices && anchors.lip_drive && anchors.speech;
        return anchors;
    }
} // namespace SoundFramework
