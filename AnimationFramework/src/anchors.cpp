/**
 * @file anchors.cpp
 * @brief The anchor registry: candidate ladders, validators, vtable identities and the feature gates.
 *
 * Each function resolves from a ladder of byte signatures. DMK anchor::resolve takes the first rung that matches
 * exactly once, inside WHGame.dll and on executable pages only, and the result must pass a validator: a function entry
 * on an executable page that agrees with its unwind entry, or an instruction site on one. gEnv is the root of every
 * other read, so it resolves as a 2-of-3 quorum of independent load sites.
 *
 * Signature rules: every call target and RIP displacement is a wildcard. A short branch keeps its displacement only
 * when fixed instructions inside the rung set it. A branch whose encoding can change inside a pattern is a bounded gap.
 * Every rung matches exactly once, at the same address, on the Steam 1.5.6, GOG 1.5 and Game Pass 1.4 builds. Each
 * function ladder holds a rung that starts past the first 14 bytes, so another mod's inline hook on the entry cannot
 * hide every rung.
 */

#include "anchors.hpp"
#include "constants.hpp"
#include "game_memory.hpp"

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace AnimationFramework
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

        // gEnv: three loads of the static SSystemGlobalEnvironment from unrelated functions, each a RIP-relative disp32
        // to its first member.
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

        // CXmlUtils::LoadXmlFromFile(out, name, patch, clean, read-only, reuse), CXmlUtils vtable slot 1 (Steam
        // 0x1809792B4, GOG 0x18099DD04, Game Pass 0x1808F2AF4). P1 is the prologue through the read-only test
        // (`cmp byte [rbp+48h], 0`), the out-reference clear (`mov qword [rdx], 0`) and the argument moves. P2 is that
        // test, the clear and the read-only parser's vtable load (`lea rax, XmlParserReadOnly::vftable`), walking back
        // 0x15.
        const Candidate XML_LOAD_FILE[] = {
            Candidate::direct(
                "XmlLoadFile_P1_PrologueReadOnlyTest",
                Pattern::literal(
                    "48 89 5C 24 08 48 89 74 24 18 55 57 41 56 48 8B EC 48 83 EC 50 80 7D 48 00 4C 8B F1 48 8D 4D E0 "
                    "48 C7 02 00 00 00 00 41 8A F1 49 8B F8 48 8B DA 0F 84"
                )
            ),
            Candidate::direct(
                "XmlLoadFile_P2_ReadOnlyParser",
                Pattern::literal(
                    "80 7D 48 00 4C 8B F1 48 8D 4D E0 48 C7 02 00 00 00 00 41 8A F1 49 8B F8 48 8B DA 0F 84 ?? ?? ?? "
                    "?? 48 8D 05 ?? ?? ?? ?? 48 C7 45 F0 00 00 00 00 48 89 45 E0"
                ),
                -0x15
            ),
        };

        // CharacterManager::LoadAnimationImageFileCAF(this, path, chunk file, count&) (Steam 0x1806A1F18, GOG
        // 0x1806C7448, Game Pass 0x1806706A8): appends an .img's headers from `count`. P1 is the prologue and the
        // argument moves. P2 is the chunk read (`call [rax+80h]`) and the GlobalAnimationHeaderCAF type and version
        // checks (`cmp dword [rax], 3007h`, `cmp dword [rax+4], 971h`), walking back 0xC3.
        const Candidate CLIP_IMAGE_LOAD[] = {
            Candidate::direct(
                "ClipImageLoad_P1_Prologue",
                Pattern::literal(
                    "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? "
                    "?? ?? ?? 48 33 C4 48 89 85 ?? ?? ?? ?? 4C 8B F1 48 89 4C 24 ?? 48 8D 4C 24 ?? 4D 8B E1 4D 8B F8 "
                    "E8"
                )
            ),
            Candidate::direct(
                "ClipImageLoad_P2_ChunkTypeVersion",
                Pattern::literal("FF 90 80 00 00 00 81 38 07 30 00 00 0F 85 ?? ?? ?? ?? 81 78 04 71 09 00 00"),
                -0xC3
            ),
        };

        // An animation set's CHRPARAMS reload (Steam 0x1820F4C30, GOG 0x1820E5DE0, Game Pass 0x182183760), which the
        // facial index load runs on every set so they pick up the new headers. P1 is the prologue through the
        // IsFileExist call (`call [rax+218h]`). P2 is that call's argument moves, the call and the set's +0x210
        // member, walking back 0x1E.
        const Candidate CHRPARAMS_RELOAD[] = {
            Candidate::direct(
                "ChrParamsReload_P1_Prologue",
                Pattern::literal(
                    "48 89 6C 24 10 56 57 41 56 48 83 EC 30 48 8B 2D ?? ?? ?? ?? 48 8B F9 48 8B 0D ?? ?? ?? ?? 45 0F "
                    "B6 F0 45 33 C0 48 8B F2 48 8B 01 FF 90 18 02 00 00 84 C0"
                )
            ),
            Candidate::direct(
                "ChrParamsReload_P2_FileExists",
                Pattern::literal(
                    "45 0F B6 F0 45 33 C0 48 8B F2 48 8B 01 FF 90 18 02 00 00 84 C0 74 ?? 48 89 5C 24 ?? 48 8D 9F 10 "
                    "02 00 00"
                ),
                -0x1E
            ),
        };

        // The facial clip index load (Steam 0x1820F3490, GOG 0x1820E4640, Game Pass 0x182181FC0), which the clip
        // index init runs right after animations.img and again on a language change. P1 is the prologue through the
        // ISystem call (`call [rax+568h]` on Steam, 560h on GOG and Game Pass, so the slot byte is a wildcard) and
        // the language-folder string load. P2 is the read of the CAF header count (`mov eax, [rbx+805A0h]`) and the
        // I3DEngine CreateChunkFile call (`call [rax+650h]`), walking back 0x53.
        const Candidate FACIAL_INDEX_LOAD[] = {
            Candidate::direct(
                "FacialIndexLoad_P1_Prologue",
                Pattern::literal(
                    "40 53 48 83 EC 20 48 8B D9 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 ?? 05 00 00 48 8B C8 48 8B 10 FF "
                    "52 58 48 8D 15"
                )
            ),
            Candidate::direct(
                "FacialIndexLoad_P2_CafCountChunkFile",
                Pattern::literal(
                    "8B 83 A0 05 08 00 B2 01 48 8B 0D ?? ?? ?? ?? 89 44 24 ?? 48 89 7C 24 ?? 48 8B 01 FF 90 50 06 00 "
                    "00"
                ),
                -0x53
            ),
        };

        // The .dba loader's "DBA/CAF mismatch" fatal check (Steam 0x180766850, GOG 0x18078BBF0, Game Pass
        // 0x1807F6C00): a clip the .dba lists whose index entry is a loose .caf (DBA CRC 0) ends the game while
        // ca_UseIMG_CAF is on, although the code right after it already skips such a clip (`cmp [entry+28h], 0` /
        // `je`). P1 is the DBA CRC test, the cvar test and the message load, landing on the je (+0xE, wildcarded, so a
        // generation after the patch still resolves it). P2 is the message load, the error call and the next block's
        // `lea rcx, [rbp+140h]` / call / `movups xmm0, [rsi]`, walking back 2.
        const Candidate DBA_MISMATCH_CHECK[] = {
            Candidate::direct(
                "DbaMismatchCheck_P1_CrcCvarMessage",
                Pattern::literal(
                    "44 39 64 ?? 28 75 ?? 44 39 25 ?? ?? ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? B9 C8 00 00 00 E8"
                ),
                0xE
            ),
            Candidate::direct(
                "DbaMismatchCheck_P2_MessageCallNextBlock",
                Pattern::literal(
                    "48 8D 15 ?? ?? ?? ?? B9 C8 00 00 00 E8 ?? ?? ?? ?? 48 8D 8D ?? ?? 00 00 E8 ?? ?? ?? ?? 0F 10 06"
                ),
                -0x2
            ),
        };

        // CAnimationDatabase's two option queries (slots 12 and 6), matched at their first bytes. Both open with their
        // frame and `lea rbx, [rcx+80h]` (the database's reader gate) ahead of the shared tag-set matcher. Both then
        // pass the matcher the query's tags and its required tags with r14 holding the SFragmentQuery
        // (`mov dword [rsp+38h], 14h`, the 20-byte tag state; `lea rax, [r14+2Ch]`, the required tags;
        // `lea rdx, [r14+4]`, the global then fragment tags), which proves the query layout the detours copy.
        // GetBestEntry also fills the selection with rdi holding it (the tag-set index at +28h, the option index at
        // +2Ch, the fragment GUID at +30h).
        constexpr Pattern GET_BEST_ENTRY_BODY =
            Pattern::literal("48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 40 48 8D 99 80 00 00 00");
        constexpr std::size_t GET_BEST_ENTRY_BODY_LENGTH = 28;
        constexpr Pattern FIND_BEST_MATCHING_TAG_BODY =
            Pattern::literal("40 53 55 56 57 41 56 48 83 EC 70 48 8D 99 80 00 00 00");
        constexpr std::size_t FIND_BEST_MATCHING_TAG_BODY_LENGTH = 18;
        constexpr Pattern QUERY_LAYOUT =
            Pattern::literal("C7 44 24 38 14 00 00 00 49 8D 46 2C 48 89 44 24 30 49 8D 56 04");
        constexpr std::size_t QUERY_LAYOUT_WINDOW = 0xC0;
        constexpr Pattern SELECTION_LAYOUT = Pattern::literal("89 47 28 89 4F 2C 0F 10 46 18 F3 0F 7F 47 30");
        constexpr std::size_t SELECTION_LAYOUT_WINDOW = 0x140;

        // CAnimationDatabase::FindBestBlends (slot 16), matched at its frame (the three spilled arguments, eight
        // pushes, `sub rsp, 68h`). Its variant pass hands the blend matcher the query's tag states with r15 holding the
        // SBlendQuery (`lea r13, [r15+58h]`, the additional tags; `lea r8, [r15+8]`, the source tag state;
        // `lea r9, [r15+30h]`, the target tag state), which proves the offsets the detour marks.
        constexpr Pattern FIND_BEST_BLENDS_BODY = Pattern::literal(
            "4C 89 4C 24 20 4C 89 44 24 18 48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 68"
        );
        constexpr std::size_t FIND_BEST_BLENDS_BODY_LENGTH = 34;
        constexpr Pattern BLEND_QUERY_LAYOUT = Pattern::literal(
            "4D 8D 6F 58 48 8D 14 FA 4D 8D 47 08 49 8D 41 04 48 89 44 24 30 4C 89 4C 24 28 4D 8D 4F 30"
        );
        constexpr std::size_t BLEND_QUERY_LAYOUT_WINDOW = 0x140;

        // CReadOnlyChunkFile::Read (slot 4) and ReadFromMemory (slot 5), matched at their entries: the frame and the
        // argument moves, both calls that reset the file first, then Read's flag test and ReadFromMemory's buffer
        // store. Identical on the Steam, GOG and Game Pass builds apart from the call targets.
        constexpr Pattern CHUNK_FILE_READ_BODY = Pattern::literal(
            "48 89 5C 24 10 48 89 74 24 20 57 48 83 EC 20 48 8B FA 48 8B D9 E8 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? "
            "8A 43 3D"
        );
        constexpr std::size_t CHUNK_FILE_READ_BODY_LENGTH = 37;
        constexpr Pattern CHUNK_FILE_READ_FROM_MEMORY_BODY = Pattern::literal(
            "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 41 8B F8 48 8B DA 48 8B F1 E8 ?? ?? ?? ?? 48 8B CE E8 "
            "?? ?? ?? ?? 48 8B CE 48 89 5E 30 C6 46 3C 00"
        );
        constexpr std::size_t CHUNK_FILE_READ_FROM_MEMORY_BODY_LENGTH = 48;

        // CActionScope::IsDifferent (slot 21) at its entry: the frame up to the security cookie, the argument moves and
        // `cmp [rcx+0ACh], edx` (the last fragment); then, within its first 0xA0 bytes, the cache compare of the
        // context's tag mask: `mov rcx, [rdi+18h]` (context), `add rcx, 10h` (its mask) against `[... + 90h]` (the
        // cached mask), 20 bytes.
        constexpr Pattern IS_DIFFERENT_BODY = Pattern::literal(
            "48 8B C4 48 89 58 10 48 89 70 18 48 89 78 20 55 41 56 41 57 48 8D 68 A1 48 81 EC F0 00 00 00 "
            "48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 37 49 8B F0 4D 63 D1 44 8B F2 48 8B F9 39 91 AC 00 00 00"
        );
        constexpr std::size_t IS_DIFFERENT_BODY_LENGTH = 63;
        constexpr Pattern IS_DIFFERENT_LAYOUT =
            Pattern::literal("48 8B 4F 18 33 D2 48 83 C1 10 4C 8B C7 4C 2B C1 42 8A 84 01 90 00 00 00 38 01");
        constexpr std::size_t IS_DIFFERENT_LAYOUT_WINDOW = 0xA0;

        /// What an anchor names, which decides the validator it must pass.
        enum class Role : std::uint8_t
        {
            /// A function entry the framework hooks.
            Code,
            /// An instruction the framework patches.
            Site,
            /// A global the framework reads through.
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
         * @brief True when @p address is a function entry in the image: on an executable page, shaped like a prologue,
         *        and the start of the unwind entry covering it.
         * @details A body rung's fixed walk-back can turn stale while its bytes still match. The unwind entry's
         *          recorded start then differs, and the anchor fails closed. Every function the framework names saves
         *          registers in its prologue, so it has an unwind entry, and an address without one fails too. A
         *          prologue another mod already patched into a jump still passes the prologue probe, and its unwind
         *          entry is the image's own.
         */
        [[nodiscard]] bool is_function_entry(std::uintptr_t address, DMK::Region image) noexcept
        {
            if (address == 0 || !image.contains(DMK::Address{address}) || !on_executable_page(address) ||
                !DMK::scan::is_likely_function_prologue(DMK::Address{address}))
            {
                return false;
            }
            DWORD64 image_base = 0;
            const RUNTIME_FUNCTION *entry = RtlLookupFunctionEntry(static_cast<DWORD64>(address), &image_base, nullptr);
            return entry != nullptr && image_base + entry->BeginAddress == static_cast<DWORD64>(address);
        }

        /** @brief Validator of a code anchor: a function entry (is_function_entry). */
        [[nodiscard]] bool code_target_valid(std::int64_t value, const void *context) noexcept
        {
            return in_image(value, context) && is_function_entry(
                                                   static_cast<std::uintptr_t>(value),
                                                   static_cast<const ValidatorContext *>(context)->image
                                               );
        }

        /** @brief Validator of a site anchor: an instruction in the image, on an executable page. */
        [[nodiscard]] bool site_target_valid(std::int64_t value, const void *context) noexcept
        {
            return in_image(value, context) && on_executable_page(static_cast<std::uintptr_t>(value));
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
            switch (role)
            {
            case Role::Code:
                return code_target_valid;
            case Role::Site:
                return site_target_valid;
            case Role::Data:
                return data_target_valid;
            }
            return nullptr;
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

        // C_ScriptBindHuman PlayAnim(this, args, fragment, tags) (Steam 0x182AC0834, GOG 0x182AB11F0, Game Pass
        // 0x182B596C4): the script actor (call +0x1D), IActor::IsPlayer (call +0x2D), then the play (call +0x3F) for
        // the player only. The two `je` keep their displacements: each lands on the shared tail right after the play
        // call, so the fixed instructions in between set it. P1 is the prologue through the third call. P2 starts at
        // the argument moves, walking back 0x14.
        const Candidate PLAY_ANIM_BIND[] = {
            Candidate::direct(
                "PlayAnimBind_P1_Entry",
                Pattern::literal(
                    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 49 8B F1 49 8B E8 48 8B DA "
                    "E8 ?? ?? ?? ?? 48 8B F8 48 85 C0 74 1A 48 8B C8 E8 ?? ?? ?? ?? 84 C0 74 0E 4C 8B C6 48 8B D5 "
                    "48 8B CF E8"
                )
            ),
            Candidate::direct(
                "PlayAnimBind_P2_Body",
                Pattern::literal(
                    "49 8B F1 49 8B E8 48 8B DA E8 ?? ?? ?? ?? 48 8B F8 48 85 C0 74 1A 48 8B C8 E8 ?? ?? ?? ?? "
                    "84 C0 74 0E 4C 8B C6 48 8B D5 48 8B CF E8 ?? ?? ?? ?? 48 8B 03 48 8B CB 48 8B 5C 24 30"
                ),
                -0x14
            ),
        };
        // C_ScriptBindHuman StopAnim(this, args) (Steam 0x182AC3944, GOG 0x182AB4300, Game Pass 0x182B5C7D4): the
        // script actor (call +0xD), IsPlayer (call +0x1D), then the stop (call +0x29), and the return through the
        // argument handler's vtable (`jmp [rax+58h]`). The two `je` land on that tail, as in PlayAnim. P1 is the
        // prologue through the third call. P2 starts after the first call, at `mov rdi, rax` (the script actor),
        // walking back 0x12.
        const Candidate STOP_ANIM_BIND[] = {
            Candidate::direct(
                "StopAnimBind_P1_Entry",
                Pattern::literal(
                    "48 89 5C 24 08 57 48 83 EC 20 48 8B DA E8 ?? ?? ?? ?? 48 8B F8 48 85 C0 74 14 48 8B C8 "
                    "E8 ?? ?? ?? ?? 84 C0 74 08 48 8B CF E8"
                )
            ),
            Candidate::direct(
                "StopAnimBind_P2_Body",
                Pattern::literal(
                    "48 8B F8 48 85 C0 74 14 48 8B C8 E8 ?? ?? ?? ?? 84 C0 74 08 48 8B CF E8 ?? ?? ?? ?? "
                    "48 8B 03 48 8B CB 48 8B 5C 24 30 48 83 C4 20 5F 48 FF 60 58"
                ),
                -0x12
            ),
        };

        /** @brief The registry rows, in ANCHORS order. */
        enum class AnchorId : std::uint8_t
        {
            Genv,
            CharacterManagerUpdate,
            XmlLoadFile,
            ClipImageLoad,
            ChrParamsReload,
            FacialIndexLoad,
            DbaMismatchCheck,
            ConsoleVtable,
            CharacterManagerVtable,
            AnimationDatabaseVtable,
            ReadOnlyChunkFileVtable,
            ActionScopeVtable,
            PlayAnimBind,
            StopAnimBind,
        };

        const Anchor ANCHORS[] = {
            make_quorum("Genv", GENV_VOTE_POINTERS, Role::Data),
            make_anchor("CharacterManagerUpdate", CHARACTER_MANAGER_UPDATE, Role::Code),
            make_anchor("XmlLoadFile", XML_LOAD_FILE, Role::Code),
            make_anchor("ClipImageLoad", CLIP_IMAGE_LOAD, Role::Code),
            make_anchor("ChrParamsReload", CHRPARAMS_RELOAD, Role::Code),
            make_anchor("FacialIndexLoad", FACIAL_INDEX_LOAD, Role::Code),
            make_anchor("DbaMismatchCheck", DBA_MISMATCH_CHECK, Role::Site),
            make_vtable("ConsoleVtable", constants::RTTI_CONSOLE),
            make_vtable("CharacterManagerVtable", constants::RTTI_CHARACTER_MANAGER),
            make_vtable("AnimationDatabaseVtable", constants::RTTI_ANIMATION_DATABASE),
            make_vtable("ReadOnlyChunkFileVtable", constants::RTTI_READ_ONLY_CHUNK_FILE),
            make_vtable("ActionScopeVtable", constants::RTTI_ACTION_SCOPE),
            make_anchor("PlayAnimBind", PLAY_ANIM_BIND, Role::Code),
            make_anchor("StopAnimBind", STOP_ANIM_BIND, Role::Code),
        };
        constexpr std::size_t ANCHOR_COUNT = std::size(ANCHORS);
        static_assert(static_cast<std::size_t>(AnchorId::StopAnimBind) + 1 == ANCHOR_COUNT);

        [[nodiscard]] constexpr std::size_t index_of(AnchorId id) noexcept
        {
            return static_cast<std::size_t>(id);
        }

        /** @brief The features, in FEATURES order. */
        enum class Feature : std::uint8_t
        {
            FrameTick,
            CameraView,
            AnimPatch,
            ModClips,
            ClipIndexStartup,
            PackedClipReplacement,
            AnimationDatabase,
            SkeletonMerge,
            ActionScope,
            NpcActions,
        };

        /** @brief A feature and the anchors it needs. It turns on when anchor::evaluate_gate passes over them. */
        struct FeatureSpec
        {
            std::string_view name;
            std::span<const AnchorId> anchors;
        };

        constexpr AnchorId FRAME_TICK_ANCHORS[] = {AnchorId::CharacterManagerUpdate};
        constexpr AnchorId CAMERA_VIEW_ANCHORS[] = {
            AnchorId::Genv,
            AnchorId::ConsoleVtable,
        };
        constexpr AnchorId ANIM_PATCH_ANCHORS[] = {
            AnchorId::Genv,
            AnchorId::XmlLoadFile,
        };
        constexpr AnchorId MOD_CLIP_ANCHORS[] = {
            AnchorId::Genv,
            AnchorId::ClipImageLoad,
            AnchorId::ChrParamsReload,
        };
        constexpr AnchorId CLIP_INDEX_STARTUP_ANCHORS[] = {AnchorId::FacialIndexLoad};
        constexpr AnchorId PACKED_CLIP_REPLACEMENT_ANCHORS[] = {AnchorId::DbaMismatchCheck};
        constexpr AnchorId ANIMATION_DATABASE_ANCHORS[] = {AnchorId::AnimationDatabaseVtable};
        constexpr AnchorId SKELETON_MERGE_ANCHORS[] = {AnchorId::Genv, AnchorId::ReadOnlyChunkFileVtable};
        constexpr AnchorId ACTION_SCOPE_ANCHORS[] = {AnchorId::ActionScopeVtable};
        constexpr AnchorId NPC_ACTION_ANCHORS[] = {
            AnchorId::PlayAnimBind,
            AnchorId::StopAnimBind,
        };

        constexpr FeatureSpec FEATURES[] = {
            {"FrameTick", FRAME_TICK_ANCHORS},
            {"CameraView", CAMERA_VIEW_ANCHORS},
            {"AnimPatch", ANIM_PATCH_ANCHORS},
            {"ModClips", MOD_CLIP_ANCHORS},
            {"ClipIndexStartup", CLIP_INDEX_STARTUP_ANCHORS},
            {"PackedClipReplacement", PACKED_CLIP_REPLACEMENT_ANCHORS},
            {"AnimationDatabase", ANIMATION_DATABASE_ANCHORS},
            {"SkeletonMerge", SKELETON_MERGE_ANCHORS},
            {"ActionScope", ACTION_SCOPE_ANCHORS},
            {"NpcActions", NPC_ACTION_ANCHORS},
        };
        constexpr std::size_t FEATURE_COUNT = std::size(FEATURES);
        static_assert(static_cast<std::size_t>(Feature::NpcActions) + 1 == FEATURE_COUNT);
        constexpr std::size_t MAX_FEATURE_ANCHORS = []
        {
            std::size_t most = 0;
            for (const FeatureSpec &feature : FEATURES)
            {
                most = feature.anchors.size() > most ? feature.anchors.size() : most;
            }
            return most;
        }();

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

        /** @brief Slot @p index of @p vtable when it points into the game image, else 0. */
        [[nodiscard]] std::uintptr_t slot_of(std::uintptr_t vtable, std::size_t index, DMK::Region game) noexcept
        {
            if (vtable == 0)
            {
                return 0;
            }
            const std::uintptr_t value = read_ptr(vtable + index * sizeof(std::uintptr_t));
            return game.contains(DMK::Address{value}) ? value : 0;
        }

        /** @brief True when @p signature matches inside the first @p window bytes of the function at @p target. */
        [[nodiscard]] bool
        code_window_matches(std::uintptr_t target, const Pattern &signature, std::size_t window, DMK::Region game)
        {
            if (target == 0 || window == 0)
            {
                return false;
            }
            const DMK::Region head{DMK::Address{target}, window};
            return game.contains(head.base) &&
                   game.contains(head.base.offset(static_cast<std::ptrdiff_t>(window) - 1)) &&
                   DMK::scan::scan(signature, head, 1, DMK::scan::Pages::Executable).has_value();
        }

        /**
         * @brief Cross-checks the resolved CharacterManager::Update against the CharacterManager vtable's slot 18.
         * @details A slot that points outside the image is another mod's vtable hook, which proves nothing either way,
         *          so only a slot that names another function of the image refuses the anchor.
         * @return @p update, or 0 when the vtable resolved and its slot names another function of the image.
         */
        [[nodiscard]] std::uintptr_t
        corroborate_character_manager_update(std::uintptr_t update, std::uintptr_t vtable, DMK::Region game)
        {
            if (update == 0 || vtable == 0)
            {
                return update;
            }
            const std::uintptr_t slot = slot_of(vtable, constants::CHARACTER_MANAGER_UPDATE_SLOT, game);
            if (slot != 0 && slot != update)
            {
                DMK::log().error(
                    "Anchors: CharacterManager vtable slot {} is {}, not the resolved Update at {}; the frame tick is "
                    "off",
                    constants::CHARACTER_MANAGER_UPDATE_SLOT,
                    DMK::format::format_address(slot),
                    DMK::format::format_address(update)
                );
                return 0;
            }
            return update;
        }

        /**
         * @brief The two option queries (slots 12 and 6), each checked at its first bytes and for the query layout, and
         *        the tag-definition field read from the GetTagDefs getter (slot 7, `mov rax, [rcx+disp8]; ret`).
         */
        void resolve_option_queries(std::uintptr_t database_vtable, DMK::Region game, Anchors &anchors)
        {
            const std::uintptr_t entry =
                slot_of(database_vtable, constants::ANIMATION_DATABASE_GET_BEST_ENTRY_SLOT, game);
            const std::uintptr_t matching =
                slot_of(database_vtable, constants::ANIMATION_DATABASE_FIND_BEST_MATCHING_TAG_SLOT, game);
            const std::uintptr_t getter =
                slot_of(database_vtable, constants::ANIMATION_DATABASE_GET_TAG_DEFS_SLOT, game);
            const bool entry_ok = is_function_entry(entry, game) &&
                                  code_window_matches(entry, GET_BEST_ENTRY_BODY, GET_BEST_ENTRY_BODY_LENGTH, game) &&
                                  code_window_matches(entry, QUERY_LAYOUT, QUERY_LAYOUT_WINDOW, game) &&
                                  code_window_matches(entry, SELECTION_LAYOUT, SELECTION_LAYOUT_WINDOW, game);
            const bool matching_ok =
                is_function_entry(matching, game) &&
                code_window_matches(matching, FIND_BEST_MATCHING_TAG_BODY, FIND_BEST_MATCHING_TAG_BODY_LENGTH, game) &&
                code_window_matches(matching, QUERY_LAYOUT, QUERY_LAYOUT_WINDOW, game);
            const std::optional<std::array<std::uint8_t, 5>> head =
                getter != 0 ? read_value<std::array<std::uint8_t, 5>>(getter) : std::nullopt;
            const bool getter_ok = head.has_value() && (*head)[0] == 0x48 && (*head)[1] == 0x8B && (*head)[2] == 0x41 &&
                                   (*head)[4] == 0xC3;
            if (!entry_ok || !matching_ok || !getter_ok)
            {
                DMK::log().warning(
                    "Anchors: CAnimationDatabase's option queries did not check out (GetBestEntry {}, "
                    "FindBestMatchingTag {}, GetTagDefs {})",
                    entry_ok,
                    matching_ok,
                    getter_ok
                );
                return;
            }
            anchors.get_best_entry = entry;
            anchors.find_best_matching_tag = matching;
            anchors.database_tag_defs_offset = (*head)[3];
            DMK::log().info(
                "Anchors: CAnimationDatabase::GetBestEntry at {} (RVA {:#x}), FindBestMatchingTag at {} (RVA {:#x}), "
                "tag definitions at +{:#x}",
                DMK::format::format_address(entry),
                entry - game.base.raw(),
                DMK::format::format_address(matching),
                matching - game.base.raw(),
                anchors.database_tag_defs_offset
            );

            const std::uintptr_t blends =
                slot_of(database_vtable, constants::ANIMATION_DATABASE_FIND_BEST_BLENDS_SLOT, game);
            if (!is_function_entry(blends, game) ||
                !code_window_matches(blends, FIND_BEST_BLENDS_BODY, FIND_BEST_BLENDS_BODY_LENGTH, game) ||
                !code_window_matches(blends, BLEND_QUERY_LAYOUT, BLEND_QUERY_LAYOUT_WINDOW, game))
            {
                DMK::log().warning(
                    "Anchors: CAnimationDatabase::FindBestBlends did not check out; transitions ignore "
                    "the view"
                );
                return;
            }
            anchors.find_best_blends = blends;
            DMK::log().info(
                "Anchors: CAnimationDatabase::FindBestBlends at {} (RVA {:#x})",
                DMK::format::format_address(blends),
                blends - game.base.raw()
            );
        }

        /** @brief CReadOnlyChunkFile's Read and ReadFromMemory, each checked at its entry, or neither. */
        void resolve_chunk_file_reads(std::uintptr_t vtable, DMK::Region game, Anchors &anchors)
        {
            if (vtable == 0)
            {
                return;
            }
            const std::uintptr_t read = slot_of(vtable, constants::CHUNK_FILE_READ_SLOT, game);
            const std::uintptr_t from_memory = slot_of(vtable, constants::CHUNK_FILE_READ_FROM_MEMORY_SLOT, game);
            const bool read_ok = is_function_entry(read, game) &&
                                 code_window_matches(read, CHUNK_FILE_READ_BODY, CHUNK_FILE_READ_BODY_LENGTH, game);
            const bool memory_ok = is_function_entry(from_memory, game) && code_window_matches(
                                                                               from_memory,
                                                                               CHUNK_FILE_READ_FROM_MEMORY_BODY,
                                                                               CHUNK_FILE_READ_FROM_MEMORY_BODY_LENGTH,
                                                                               game
                                                                           );
            if (!read_ok || !memory_ok)
            {
                DMK::log().warning(
                    "Anchors: CReadOnlyChunkFile's reads did not check out (Read {}, ReadFromMemory {}); skeletons "
                    "load as the last mod ships them",
                    read_ok,
                    memory_ok
                );
                return;
            }
            anchors.chunk_file_read = read;
            anchors.chunk_file_read_from_memory = from_memory;
            DMK::log().info(
                "Anchors: CReadOnlyChunkFile::Read at {} (RVA {:#x}), ReadFromMemory at {} (RVA {:#x})",
                DMK::format::format_address(read),
                read - game.base.raw(),
                DMK::format::format_address(from_memory),
                from_memory - game.base.raw()
            );
        }

        /** @brief CActionScope::IsDifferent, checked at its entry and for its cache layout, or 0. */
        [[nodiscard]] std::uintptr_t resolve_is_different(std::uintptr_t vtable, DMK::Region game)
        {
            if (vtable == 0)
            {
                return 0;
            }
            const std::uintptr_t target = slot_of(vtable, constants::ACTION_SCOPE_IS_DIFFERENT_SLOT, game);
            if (!is_function_entry(target, game) ||
                !code_window_matches(target, IS_DIFFERENT_BODY, IS_DIFFERENT_BODY_LENGTH, game) ||
                !code_window_matches(target, IS_DIFFERENT_LAYOUT, IS_DIFFERENT_LAYOUT_WINDOW, game))
            {
                DMK::log().warning(
                    "Anchors: CActionScope::IsDifferent did not check out; a tag change while Henry stands still waits "
                    "for his next move"
                );
                return 0;
            }
            DMK::log().info(
                "Anchors: CActionScope::IsDifferent at {} (RVA {:#x})",
                DMK::format::format_address(target),
                target - game.base.raw()
            );
            return target;
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
        const auto resolved = [&report](AnchorId id) -> std::uintptr_t
        {
            const ResolvedAnchor &entry = report[index_of(id)];
            return entry.status == AnchorStatus::Resolved ? static_cast<std::uintptr_t>(entry.value) : 0;
        };

        Anchors anchors{
            .genv = resolved(AnchorId::Genv),
            .console_vtable = gated(Feature::CameraView, AnchorId::ConsoleVtable),
            .character_manager_update = corroborate_character_manager_update(
                gated(Feature::FrameTick, AnchorId::CharacterManagerUpdate),
                resolved(AnchorId::CharacterManagerVtable),
                game
            ),
            .xml_load_file = gated(Feature::AnimPatch, AnchorId::XmlLoadFile),
            .clip_image_load = gated(Feature::ModClips, AnchorId::ClipImageLoad),
            .chrparams_reload = gated(Feature::ModClips, AnchorId::ChrParamsReload),
            .facial_index_load = gated(Feature::ClipIndexStartup, AnchorId::FacialIndexLoad),
            .dba_mismatch_check = gated(Feature::PackedClipReplacement, AnchorId::DbaMismatchCheck),
        };
        const std::uintptr_t database_vtable = gated(Feature::AnimationDatabase, AnchorId::AnimationDatabaseVtable);
        resolve_option_queries(database_vtable, game, anchors);
        resolve_chunk_file_reads(gated(Feature::SkeletonMerge, AnchorId::ReadOnlyChunkFileVtable), game, anchors);
        anchors.action_scope_is_different =
            resolve_is_different(gated(Feature::ActionScope, AnchorId::ActionScopeVtable), game);
        anchors.play_anim_bind = gated(Feature::NpcActions, AnchorId::PlayAnimBind);
        anchors.stop_anim_bind = gated(Feature::NpcActions, AnchorId::StopAnimBind);
        return anchors;
    }
} // namespace AnimationFramework
