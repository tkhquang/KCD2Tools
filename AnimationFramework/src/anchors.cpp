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
#include <initializer_list>
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

        // CScriptSystem::ExecuteBuffer(buffer, size, description, env) (slot 6) at its entry: the frame and
        // `mov rdi, rcx`, then the lua_load reader data built on the stack from the arguments (`mov [rax-18h], rdx`
        // the buffer, `mov [rax-10h], r8` the size), the Lua state (`mov rcx, [rcx+10h]`) and the description kept in
        // r14 (`mov r14, r9`), up to the lua_load call. Then, within its first 0x80 bytes, the load result test, the
        // LUA_GLOBALSINDEX argument (`mov edx, 0FFFFD8F0h`, -10000) and the null test of the fifth argument
        // (`mov rcx, [rsp+80h]`, the env), which a null env skips. Together they prove the argument order the bridge
        // calls it with.
        constexpr Pattern EXECUTE_BUFFER_BODY = Pattern::literal(
            "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 50 48 8B F9 48 89 50 E8 "
            "48 8B 49 10 48 8D 15 ?? ?? ?? ?? 4C 89 40 F0 4D 8B F1 4C 8D 40 E8 E8"
        );
        constexpr std::size_t EXECUTE_BUFFER_BODY_LENGTH = 55;
        constexpr Pattern EXECUTE_BUFFER_LAYOUT = Pattern::literal(
            "85 C0 75 ?? 48 8B 4F 10 BA F0 D8 FF FF 44 8B 47 58 48 8B 69 10 48 2B 69 18 48 C1 FD 04 E8 ?? ?? ?? ?? "
            "48 8B 4F 10 8B D5 E8 ?? ?? ?? ?? 48 8B 8C 24 80 00 00 00 48 85 C9 74"
        );
        constexpr std::size_t EXECUTE_BUFFER_LAYOUT_WINDOW = 0x80;

        // CMovieSystem::Load(file, mission) (slot 5) at its entry, up to its XML load: the ISystem field
        // (`mov rcx, [rcx+disp8]`) and ISystem::LoadXmlFromFile called with the out reference (`lea rdx, [rbp+30h]`),
        // the file (`mov r8, rdx`) and the flags 0, 1, 1 (`xor r9d, r9d`, `mov byte [rsp+20h], 1`, `mov byte
        // [rsp+28h], 1`) through `call [rax+disp32]`, a slot that is 0x418 on Steam and 0x410 on GOG and Game Pass.
        constexpr Pattern MOVIE_LOAD_BODY = Pattern::literal(
            "48 89 5C 24 10 55 56 57 41 56 41 57 48 8B EC 48 83 EC 50 4C 8B F1 C6 44 24 28 01 48 8B 49 ?? 4D 8B F8 "
            "4C 8B C2 C6 44 24 20 01 45 33 C9 48 8D 55 30 48 8B 01 FF 90 ?? ?? ?? ??"
        );
        constexpr std::size_t MOVIE_LOAD_BODY_LENGTH = 58;
        constexpr std::size_t MOVIE_LOAD_SYSTEM_FIELD_AT = 30;
        constexpr std::size_t MOVIE_LOAD_XML_SLOT_AT = 54;
        // ISystem::LoadXmlFromFile at its entry: loads the CXmlUtils field (`mov rcx, [rcx+disp32]`) and forwards to
        // its slot 1 (`mov r10, [rax+8]`), CXmlUtils::LoadXmlFromFile.
        constexpr Pattern SYSTEM_LOAD_XML_BODY = Pattern::literal(
            "40 53 48 83 EC 40 48 8B 89 ?? ?? ?? ?? 48 8B DA 44 88 4C 24 30 41 B1 01 48 8B 01 4C 8B 50 08"
        );
        constexpr std::size_t SYSTEM_LOAD_XML_BODY_LENGTH = 31;
        constexpr std::size_t SYSTEM_LOAD_XML_UTILS_FIELD_AT = 9;
        // CMovieSystem::LoadSequence(XmlNodeRef &, bool) (slot 8) at its entry: keeps the flag (`mov sil, r8b`), the
        // node reference (`mov rbp, rdx`) and the movie system (`mov rdi, rcx`), then allocates the 0x128-byte
        // CAnimSequence (`mov ebx, 128h`). Then, within its first 0xC0 bytes, the sequence's Serialize
        // (`call [rax+170h]`), its GetName (`call [rax+30h]`, slot 6) and the movie system's FindSequence by that name
        // (`mov r8, [rcx+58h]`, slot 11): the calls that prove the argument order the loader calls it with.
        constexpr Pattern MOVIE_LOAD_SEQUENCE_BODY = Pattern::literal(
            "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 40 83 3D ?? ?? ?? ?? 00 41 8A F0 48 8B EA 48 8B "
            "F9 75 05 E8 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 8D 54 24 50 BB 28 01 00 00"
        );
        constexpr std::size_t MOVIE_LOAD_SEQUENCE_BODY_LENGTH = 60;
        constexpr Pattern MOVIE_LOAD_SEQUENCE_LAYOUT = Pattern::literal(
            "FF 90 70 01 00 00 48 8B 03 48 8B CB FF 50 30 48 8B 0F 48 8B D0 4C 8B 41 58 48 8B CF 41 FF D0"
        );
        constexpr std::size_t MOVIE_LOAD_SEQUENCE_LAYOUT_WINDOW = 0xC0;

        // The handler node's two calls, at fixed offsets from its entry on all three builds. The configuration:
        // `lea rcx, [rsp+30h]` (the shared_ptr out) then the call, its result moved into the node. Then the enqueue:
        // the getter, `mov rdx, rdi` (the configuration), the field chain to the cutscene player, the call.
        constexpr Pattern CUTSCENE_CREATE_CALL = Pattern::literal("48 8D 4C 24 30 E8 ?? ?? ?? ?? 48 8D BB ?? ?? 00 00");
        constexpr std::size_t CUTSCENE_CREATE_CALL_AT = 0x77;
        constexpr std::size_t CUTSCENE_CREATE_CALL_LENGTH = 17;
        constexpr Pattern CUTSCENE_ENQUEUE_CALLS =
            Pattern::literal("E8 ?? ?? ?? ?? 48 8B D7 48 8B 88 ?? ?? 00 00 48 8B 49 ?? E8 ?? ?? ?? ??");
        constexpr std::size_t CUTSCENE_ENQUEUE_CALLS_AT = 0x137;
        constexpr std::size_t CUTSCENE_ENQUEUE_CALLS_LENGTH = 24;
        // Inside those windows: the create call at +5; the getter call at +0, the outer field's disp32
        // (`mov rcx, [rax+disp32]`) at +11, the inner field's disp8 (`mov rcx, [rcx+disp8]`) at +18 and the enqueue
        // call at +19.
        constexpr std::size_t CUTSCENE_CREATE_CALL_OPCODE_AT = 5;
        constexpr std::size_t CUTSCENE_PLAYER_OUTER_DISP_AT = 11;
        constexpr std::size_t CUTSCENE_PLAYER_INNER_DISP_AT = 18;
        constexpr std::size_t CUTSCENE_ENQUEUE_CALL_OPCODE_AT = 19;

        // The listener's manager load (`mov rsi, [rip+disp32]`, 7 bytes with its disp32 at +3), the filter call and the
        // make call, at fixed offsets from its entry on all three builds.
        constexpr std::size_t LINKABLE_MANAGER_LOAD_AT = 0x5E;
        constexpr std::size_t LINKABLE_MANAGER_DISP_AT = 3;
        constexpr std::size_t LINKABLE_MANAGER_LOAD_LENGTH = 7;
        constexpr std::size_t LINKABLE_FILTER_CALL_AT = 0x6B;
        constexpr std::size_t LINKABLE_MAKE_CALL_AT = 0x86;

        // CAnimSequence::TriggerTrackEvent(event, param, user) (slot 58), whole: it moves the arguments up one register,
        // puts the user pointer on the stack and calls the listener notifier with reason 3 (Triggered, `mov edx, 3`).
        constexpr Pattern TRIGGER_TRACK_EVENT_BODY = Pattern::literal(
            "48 83 EC 38 4C 89 4C 24 20 4D 8B C8 4C 8B C2 BA 03 00 00 00 E8 ?? ?? ?? ?? 48 83 C4 38 C3"
        );
        constexpr std::size_t TRIGGER_TRACK_EVENT_BODY_LENGTH = 30;
        constexpr std::size_t TRIGGER_TRACK_EVENT_NOTIFY_AT = 0x14;

        // CEntitySystem slot 42 is a thunk to CEntityLoadManager::ReserveEntityIds: `mov rcx, [rcx+disp32]` (the load
        // manager field, 7 bytes) then `jmp rel32`. Slot 17 is ReserveEntityId(id), which takes the id allocator's
        // spin lock (`lock cmpxchg [rcx+disp32], esi`, disp32 at +4), points rcx at the slot array
        // (`lea rcx, [rbx+disp32]`, disp32 at +3) and calls the insert. The insert calls the free-list unlink, whose
        // `cmp [r8+disp32], r9d` (disp32 at +3) reads the free list head relative to the slot array.
        constexpr std::size_t RESERVE_LAYER_IDS_SLOT = 42;
        constexpr std::size_t RESERVE_LAYER_IDS_JUMP_AT = 7;
        constexpr std::size_t RESERVE_ENTITY_ID_SLOT = 17;
        constexpr std::size_t ENTITY_ID_LOCK_AT = 0x23;
        constexpr std::size_t ENTITY_ID_LOCK_DISP_AT = 4;
        constexpr std::size_t ENTITY_ID_SLOTS_AT = 0x46;
        constexpr std::size_t ENTITY_ID_SLOTS_DISP_AT = 3;
        constexpr std::size_t ENTITY_ID_INSERT_CALL_AT = 0x8C;
        constexpr std::size_t ENTITY_ID_UNLINK_CALL_AT = 0x1E;
        constexpr std::size_t ENTITY_ID_FREE_HEAD_AT = 0x1F;
        constexpr std::size_t ENTITY_ID_FREE_HEAD_DISP_AT = 3;

        // Call sites and loads inside the three scene layer anchors, identical on all three builds. The profile step's
        // two loads (`mov rcx, [rax+disp32]`, `mov rdi, [rcx+disp32]`) carry their disp32 at +3.
        constexpr std::size_t PROFILE_RECORD_CREATE_CALL_AT = 0x65;
        constexpr std::size_t PROFILE_RECORDS_INSERT_CALL_AT = 0x9C;
        constexpr std::size_t PROFILE_LAYER_PUSH_CALL_AT = 0x75;
        constexpr std::size_t LAYER_STRING_RELEASE_CALL_AT = 0x7F;
        constexpr std::size_t STRING_FROM_TEXT_CALL_AT = 0xD0;
        constexpr std::size_t STRING_RELEASE_CALL_AT = 0xE9;
        constexpr std::size_t PROFILE_GAME_GETTER_CALL_AT = 0xEE;
        constexpr std::size_t PROFILE_MANAGER_OUTER_LOAD_AT = 0xF7;
        constexpr std::size_t PROFILE_MANAGER_INNER_LOAD_AT = 0xFE;
        constexpr std::size_t PROFILE_MANAGER_LOAD_DISP_AT = 3;

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

        // The quest CutsceneHandler node's enqueue (Steam 0x182B12644, GOG 0x182B03070, Game Pass 0x182BAB504): the
        // holder from its port, the configuration it makes, and the cutscene player it hands that to. P1 is the
        // prologue through the node's busy test (`cmp dword [rcx+2C8h], 0`), the holder read from its port
        // (`add rcx, 108h`, the call, `test rax, rax`) and `lea rcx, [rbx+1Ch]`. P2 is the create call's arguments
        // (`mov rdx, rdi` the holder, `lea rcx, [rsp+30h]` the shared_ptr out), the call and the move of the result
        // into the node (`lea rdi, [rbx+disp32]`, then the pointer pair read and cleared), walking back 0x60.
        const Candidate CUTSCENE_HANDLER_ENQUEUE[] = {
            Candidate::direct(
                "CutsceneHandlerEnqueue_P1_Prologue",
                Pattern::literal(
                    "48 89 5C 24 20 57 48 83 EC 40 83 B9 ?? ?? 00 00 00 48 8B D9 0F 85 ?? ?? 00 00 48 81 C1 ?? ?? 00 "
                    "00 E8 ?? ?? ?? ?? 48 8B F8 48 85 C0 0F 84 ?? ?? 00 00 48 8D 4B 1C"
                )
            ),
            Candidate::direct(
                "CutsceneHandlerEnqueue_P2_Body",
                Pattern::literal(
                    "4C 8D 4C 24 50 48 8D 44 24 60 48 8B D7 4C 8D 44 24 58 48 89 44 24 20 48 8D 4C 24 30 E8 ?? ?? ?? "
                    "?? 48 8D BB ?? ?? 00 00 48 8B 08 48 8B 50 08 48 C7 00 00 00 00 00"
                ),
                -0x60
            ),
        };

        // The entity spawn listener that makes a level's listed entities linkable (Steam 0x1805CB34C, GOG 0x1805EAAFC,
        // Game Pass 0x1805CE08C). P1 is the prologue, the lookup of the entity's pending record (`call [rax+10h]` on
        // the entity, then the lookup call and its null test) and the record's state test (`and al, 3`,
        // `cmp al, 2`). P2 is the manager static load (`mov rsi, [rip+disp32]`), the filter call and its test, and the
        // make call's ownership argument (bit 6 of the record's byte +20h), walking back 0x5E.
        const Candidate LINKABLE_SPAWN[] = {
            Candidate::direct(
                "LinkableSpawn_P1_Prologue",
                Pattern::literal(
                    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 02 48 8B CA 48 8B DA FF 50 10 48 8B D0 E8 ?? "
                    "?? ?? ?? 48 8B F8 48 85 C0 74 ?? 8A 48 20 8A C1 24 03 3C 02"
                )
            ),
            Candidate::direct(
                "LinkableSpawn_P2_Body",
                Pattern::literal(
                    "48 8B 35 ?? ?? ?? ?? 48 8B D3 48 8B CE E8 ?? ?? ?? ?? 84 C0 74 ?? 44 8A 47 20 48 8B D3 41 C0 E8 "
                    "06 48 8B CE 41 80 E0 01 E8"
                ),
                -0x5E
            ),
        };

        // The level loader's GameProfiles reader (Steam 0x181EC2F40, GOG 0x181EB5390, Game Pass 0x181F52410), which
        // calls the record constructor at +0x65 and the record insert at +0x9C. P1 is the prologue and its first two
        // element name tests (`lea rdx, <name>`, `call [rax+70h]`, `test al, al`). P2 is the constructor call
        // (`lea rcx, [rsp+50h]` the out pointer), the next call on the new record and the read of the next element
        // (`mov rdx, [rdi+18h]`), walking back 0x60.
        const Candidate GAME_PROFILES_PARSE[] = {
            Candidate::direct(
                "GameProfilesParse_P1_Prologue",
                Pattern::literal(
                    "4C 89 44 24 18 56 57 48 83 EC 28 48 8B 01 48 8D 15 ?? ?? ?? ?? 49 8B F9 48 8B F1 FF 50 70 84 C0 "
                    "0F 84 ?? ?? ?? ?? 48 8B 06 48 8D 15 ?? ?? ?? ?? 48 8B CE FF 50 70 84 C0 0F 84"
                )
            ),
            Candidate::direct(
                "GameProfilesParse_P2_Body",
                Pattern::literal(
                    "48 8D 4C 24 50 E8 ?? ?? ?? ?? 48 8B 5C 24 50 48 8B D5 48 8B CB E8 ?? ?? ?? ?? 48 8B 57 18 48 3B"
                ),
                -0x60
            ),
        };

        // The level loader's GameLayers reader (Steam 0x181EC2D70, GOG 0x181EB51C0, Game Pass 0x181F52180), which pushes
        // each layer name into the record's list at +0x75 and releases its CryString at +0x7F. Two sibling readers
        // share the prologue, so P1 runs on from it through the loop over the names (an empty CryString, `call` then
        // `add rax, 0Ch`, filled by `call [rax+18h]` on the element) up to the push's `lea rcx, [rdi+18h]`. P2 is that
        // push, the release and the next two element name tests (`call [rax+78h]`, `call [rax+70h]`), walking back
        // 0x71.
        const Candidate GAME_LAYERS_PARSE[] = {
            Candidate::direct(
                "GameLayersParse_P1_Prologue",
                Pattern::literal(
                    "48 89 5C 24 10 4C 89 44 24 18 57 48 83 EC 20 48 8B 01 48 8D 15 ?? ?? ?? ?? 49 8B F9 48 8B D9 FF "
                    "50 70 84 C0 0F 84 ?? ?? ?? ?? 48 8B 03 48 8D 15 ?? ?? ?? ?? 48 8B CB FF 50 70 84 C0 74 ?? 48 89 "
                    "74 24 30 48 8B 37 48 8B 7F 08 66 0F 1F 44 00 00 E8 ?? ?? ?? ?? 48 83 C0 0C 48 8D 54 24 40 48 89 "
                    "44 24 40 48 8B CE 48 8B 06 FF 50 18 48 8D 54 24 40 48 8D 4F 18"
                )
            ),
            Candidate::direct(
                "GameLayersParse_P2_Body",
                Pattern::literal(
                    "48 8D 4F 18 E8 ?? ?? ?? ?? 48 8D 4C 24 40 E8 ?? ?? ?? ?? 48 8B 03 48 8D 15 ?? ?? ?? ?? 48 8B CB "
                    "FF 50 78 48 8B 03 48 8D 15 ?? ?? ?? ?? 48 8B CB FF 50 70 84 C0 75"
                ),
                -0x71
            ),
        };

        // C_IngameCutscene's profile step (Steam 0x182B37808, GOG 0x182B28234, Game Pass 0x182BD06C8): CryString from
        // text at +0xD0, its release at +0xE9, the game state getter at +0xEE and the two loads down to the profile
        // manager at +0xF7 and +0xFE. P1 is the prologue, the first getter call and the loads up to `call [rax+170h]`.
        // P2 is the second getter call, the key pointer (`lea rdx, [rbp+20h]`), the two loads (`mov rcx, [rax+0E0h]`,
        // `mov rdi, [rcx+158h]`) and the manager's known-profile test (slot 4, `call [rax+20h]`), walking back 0xEE.
        const Candidate INGAME_PROFILE_PREPARE[] = {
            Candidate::direct(
                "IngameProfilePrepare_P1_Prologue",
                Pattern::literal(
                    "48 89 5C 24 18 55 56 57 48 8B EC 48 83 EC 30 48 8B 41 20 48 8B F1 48 8B 18 E8 ?? ?? ?? ?? 4C 8B "
                    "80 60 01 00 00 49 8B 88 A8 00 00 00 48 8B 01 FF 90 70 01 00 00 48 8B 53 38 E8"
                )
            ),
            Candidate::direct(
                "IngameProfilePrepare_P2_Body",
                Pattern::literal(
                    "E8 ?? ?? ?? ?? 48 8D 55 20 48 8B 88 E0 00 00 00 48 8B B9 58 01 00 00 48 8B CF 48 8B 07 FF 50 20 "
                    "84 C0"
                ),
                -0xEE
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
            ScriptSystemVtable,
            MovieSystemVtable,
            SystemVtable,
            CutsceneHandlerEnqueue,
            CutsceneHolderVtable,
            LinkableSpawn,
            EntitySystemVtable,
            GameProfileManagerVtable,
            GameProfilesParse,
            GameLayersParse,
            IngameProfilePrepare,
            ConsoleStringMethodVtable,
            AnimSequenceVtable,
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
            make_vtable("ScriptSystemVtable", constants::RTTI_SCRIPT_SYSTEM),
            make_vtable("MovieSystemVtable", constants::RTTI_MOVIE_SYSTEM),
            make_vtable("SystemVtable", constants::RTTI_SYSTEM),
            make_anchor("CutsceneHandlerEnqueue", CUTSCENE_HANDLER_ENQUEUE, Role::Code),
            make_vtable("CutsceneHolderVtable", constants::RTTI_CUTSCENE_HOLDER),
            make_anchor("LinkableSpawn", LINKABLE_SPAWN, Role::Code),
            make_vtable("EntitySystemVtable", constants::RTTI_ENTITY_SYSTEM),
            make_vtable("GameProfileManagerVtable", constants::RTTI_GAME_PROFILE_MANAGER),
            make_anchor("GameProfilesParse", GAME_PROFILES_PARSE, Role::Code),
            make_anchor("GameLayersParse", GAME_LAYERS_PARSE, Role::Code),
            make_anchor("IngameProfilePrepare", INGAME_PROFILE_PREPARE, Role::Code),
            make_vtable("ConsoleStringMethodVtable", constants::RTTI_CONSOLE_STRING_METHOD),
            make_vtable("AnimSequenceVtable", constants::RTTI_ANIM_SEQUENCE),
        };
        constexpr std::size_t ANCHOR_COUNT = std::size(ANCHORS);
        static_assert(static_cast<std::size_t>(AnchorId::AnimSequenceVtable) + 1 == ANCHOR_COUNT);

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
            LiveBridge,
            MovieSequences,
            CutscenePlay,
            Linkables,
            SceneLayers,
            QuestConsole,
            SceneEvents,
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
        constexpr AnchorId LIVE_BRIDGE_ANCHORS[] = {
            AnchorId::Genv,
            AnchorId::ConsoleVtable,
            AnchorId::ScriptSystemVtable,
        };
        constexpr AnchorId MOVIE_SEQUENCE_ANCHORS[] = {
            AnchorId::Genv,
            AnchorId::MovieSystemVtable,
            AnchorId::SystemVtable,
        };
        constexpr AnchorId CUTSCENE_PLAY_ANCHORS[] = {
            AnchorId::Genv,
            AnchorId::CutsceneHandlerEnqueue,
            AnchorId::CutsceneHolderVtable,
        };
        constexpr AnchorId LINKABLE_ANCHORS[] = {AnchorId::LinkableSpawn};
        constexpr AnchorId SCENE_LAYER_ANCHORS[] = {
            AnchorId::Genv,
            AnchorId::EntitySystemVtable,
            AnchorId::GameProfileManagerVtable,
            AnchorId::GameProfilesParse,
            AnchorId::GameLayersParse,
            AnchorId::IngameProfilePrepare,
        };
        constexpr AnchorId QUEST_CONSOLE_ANCHORS[] = {
            AnchorId::Genv,
            AnchorId::ConsoleVtable,
            AnchorId::ConsoleStringMethodVtable,
        };
        constexpr AnchorId SCENE_EVENT_ANCHORS[] = {
            AnchorId::Genv,
            AnchorId::ScriptSystemVtable,
            AnchorId::AnimSequenceVtable,
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
            {"LiveBridge", LIVE_BRIDGE_ANCHORS},
            {"MovieSequences", MOVIE_SEQUENCE_ANCHORS},
            {"CutscenePlay", CUTSCENE_PLAY_ANCHORS},
            {"Linkables", LINKABLE_ANCHORS},
            {"SceneLayers", SCENE_LAYER_ANCHORS},
            {"QuestConsole", QUEST_CONSOLE_ANCHORS},
            {"SceneEvents", SCENE_EVENT_ANCHORS},
        };
        constexpr std::size_t FEATURE_COUNT = std::size(FEATURES);
        static_assert(static_cast<std::size_t>(Feature::SceneEvents) + 1 == FEATURE_COUNT);
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

        /**
         * @brief CScriptSystem's vtable when its ExecuteBuffer slot checks out at its entry and for its argument
         *        layout, else 0.
         * @details The bridge calls the slot through the live object's vtable. Another virtual in slot 6 takes other
         *          arguments, so a vtable whose slot fails the check turns the bridge off.
         */
        [[nodiscard]] std::uintptr_t resolve_script_system(std::uintptr_t vtable, DMK::Region game)
        {
            if (vtable == 0)
            {
                return 0;
            }
            const std::uintptr_t target = slot_of(vtable, constants::SCRIPT_EXECUTE_BUFFER_SLOT, game);
            if (!is_function_entry(target, game) ||
                !code_window_matches(target, EXECUTE_BUFFER_BODY, EXECUTE_BUFFER_BODY_LENGTH, game) ||
                !code_window_matches(target, EXECUTE_BUFFER_LAYOUT, EXECUTE_BUFFER_LAYOUT_WINDOW, game))
            {
                DMK::log().warning(
                    "Anchors: CScriptSystem::ExecuteBuffer did not check out; the Blender add-on cannot reach the game"
                );
                return 0;
            }
            DMK::log().info(
                "Anchors: CScriptSystem::ExecuteBuffer at {} (RVA {:#x})",
                DMK::format::format_address(target),
                target - game.base.raw()
            );
            return vtable;
        }

        /**
         * @brief CMovieSystem::LoadSequence (slot 8), checked at its first bytes and for its Serialize, GetName and
         *        FindSequence calls, and the XML loader the engine itself loads moviedata.xml with.
         * @details CMovieSystem::Load (slot 5) reads its ISystem field and calls ISystem::LoadXmlFromFile through a
         *          slot that moves between the builds, which forwards to the CXmlUtils held in a CSystem field. Both
         *          fields come from that code, so the loader parses sequence files with the CXmlUtils the engine uses.
         */
        void resolve_movie_sequences(
            std::uintptr_t movie_vtable,
            std::uintptr_t system_vtable,
            DMK::Region game,
            Anchors &anchors
        )
        {
            if (movie_vtable == 0 || system_vtable == 0)
            {
                return;
            }
            const std::uintptr_t load = slot_of(movie_vtable, constants::MOVIE_SYSTEM_LOAD_SLOT, game);
            const std::uintptr_t load_sequence =
                slot_of(movie_vtable, constants::MOVIE_SYSTEM_LOAD_SEQUENCE_SLOT, game);
            const bool load_ok = is_function_entry(load, game) &&
                                 code_window_matches(load, MOVIE_LOAD_BODY, MOVIE_LOAD_BODY_LENGTH, game);
            const bool load_sequence_ok =
                is_function_entry(load_sequence, game) &&
                code_window_matches(load_sequence, MOVIE_LOAD_SEQUENCE_BODY, MOVIE_LOAD_SEQUENCE_BODY_LENGTH, game) &&
                code_window_matches(load_sequence, MOVIE_LOAD_SEQUENCE_LAYOUT, MOVIE_LOAD_SEQUENCE_LAYOUT_WINDOW, game);
            const std::optional<std::uint8_t> system_field =
                load_ok ? read_value<std::uint8_t>(load + MOVIE_LOAD_SYSTEM_FIELD_AT) : std::nullopt;
            const std::optional<std::int32_t> xml_slot_offset =
                load_ok ? read_value<std::int32_t>(load + MOVIE_LOAD_XML_SLOT_AT) : std::nullopt;
            std::uintptr_t load_xml = 0;
            if (xml_slot_offset.has_value() && *xml_slot_offset > 0 &&
                *xml_slot_offset % static_cast<std::int32_t>(sizeof(std::uintptr_t)) == 0)
            {
                load_xml =
                    slot_of(system_vtable, static_cast<std::size_t>(*xml_slot_offset) / sizeof(std::uintptr_t), game);
            }
            const bool load_xml_ok =
                is_function_entry(load_xml, game) &&
                code_window_matches(load_xml, SYSTEM_LOAD_XML_BODY, SYSTEM_LOAD_XML_BODY_LENGTH, game);
            const std::optional<std::int32_t> utils_field =
                load_xml_ok ? read_value<std::int32_t>(load_xml + SYSTEM_LOAD_XML_UTILS_FIELD_AT) : std::nullopt;
            if (!load_ok || !load_sequence_ok || !load_xml_ok || !system_field.has_value() ||
                !utils_field.has_value() || *utils_field <= 0)
            {
                DMK::log().warning(
                    "Anchors: CMovieSystem did not check out (Load {}, LoadSequence {}, ISystem::LoadXmlFromFile {}); "
                    "sequence files cannot load",
                    load_ok,
                    load_sequence_ok,
                    load_xml_ok
                );
                return;
            }
            anchors.movie_system_vtable = movie_vtable;
            anchors.system_vtable = system_vtable;
            anchors.movie_system_system_offset = *system_field;
            anchors.system_xml_utils_offset = *utils_field;
            DMK::log().info(
                "Anchors: CMovieSystem::LoadSequence at {} (RVA {:#x}), ISystem at +{:#x}, its XML loader in slot {} "
                "and CXmlUtils at +{:#x}",
                DMK::format::format_address(load_sequence),
                load_sequence - game.base.raw(),
                anchors.movie_system_system_offset,
                static_cast<std::size_t>(*xml_slot_offset) / sizeof(std::uintptr_t),
                anchors.system_xml_utils_offset
            );
        }

        /**
         * @brief The target of the `call rel32` (E8) or `jmp rel32` (E9) at @p branch, or 0 when the byte there is
         *        another opcode or the target lies outside the image.
         * @details A fixed call-site offset that a rebuild moved then lands on another opcode and fails closed instead
         *          of reading a displacement out of the middle of an instruction.
         */
        [[nodiscard]] std::uintptr_t branch_target(std::uintptr_t branch, DMK::Region game) noexcept
        {
            constexpr std::uint8_t CALL_REL32 = 0xE8;
            constexpr std::uint8_t JMP_REL32 = 0xE9;
            constexpr std::size_t REL32_AT = 1;
            constexpr std::size_t REL32_LENGTH = 5;
            const std::optional<std::uint8_t> opcode = read_value<std::uint8_t>(branch);
            if (opcode != CALL_REL32 && opcode != JMP_REL32)
            {
                return 0;
            }
            const DMK::Result<DMK::Address> target =
                DMK::scan::resolve_rip_relative(DMK::Address{branch}, REL32_AT, REL32_LENGTH);
            return target.has_value() && game.contains(*target) ? target->raw() : 0;
        }

        /** @brief True when the bytes at @p address are @p bytes (an opcode and its fixed operand bytes). */
        [[nodiscard]] bool bytes_at(std::uintptr_t address, std::initializer_list<std::uint8_t> bytes) noexcept
        {
            std::size_t index = 0;
            for (const std::uint8_t expected : bytes)
            {
                if (read_value<std::uint8_t>(address + index++) != expected)
                {
                    return false;
                }
            }
            return true;
        }

        /**
         * @brief The calls the quest CutsceneHandler node makes to enqueue a cutscene, decoded from its code.
         * @details These are the game's own functions: the command that uses them starts a cutscene as the node does.
         */
        void
        resolve_cutscene_play(std::uintptr_t handler, std::uintptr_t holder_vtable, DMK::Region game, Anchors &anchors)
        {
            if (handler == 0 || holder_vtable == 0)
            {
                return;
            }
            const std::uintptr_t create_at = handler + CUTSCENE_CREATE_CALL_AT;
            const std::uintptr_t enqueue_at = handler + CUTSCENE_ENQUEUE_CALLS_AT;
            if (!code_window_matches(create_at, CUTSCENE_CREATE_CALL, CUTSCENE_CREATE_CALL_LENGTH, game) ||
                !code_window_matches(enqueue_at, CUTSCENE_ENQUEUE_CALLS, CUTSCENE_ENQUEUE_CALLS_LENGTH, game))
            {
                DMK::log().warning("Anchors: the cutscene handler's calls did not check out; cutscenes cannot start");
                return;
            }
            const std::uintptr_t create = branch_target(create_at + CUTSCENE_CREATE_CALL_OPCODE_AT, game);
            const std::uintptr_t getter = branch_target(enqueue_at, game);
            const std::uintptr_t enqueue = branch_target(enqueue_at + CUTSCENE_ENQUEUE_CALL_OPCODE_AT, game);
            const std::optional<std::int32_t> outer =
                read_value<std::int32_t>(enqueue_at + CUTSCENE_PLAYER_OUTER_DISP_AT);
            const std::optional<std::uint8_t> inner =
                read_value<std::uint8_t>(enqueue_at + CUTSCENE_PLAYER_INNER_DISP_AT);
            if (!is_function_entry(create, game) || !is_function_entry(enqueue, game) || getter == 0 ||
                !outer.has_value() || !inner.has_value())
            {
                DMK::log().warning("Anchors: the cutscene handler's call targets did not check out");
                return;
            }
            anchors.cutscene_holder_vtable = holder_vtable;
            anchors.cutscene_create = create;
            anchors.cutscene_enqueue = enqueue;
            anchors.cutscene_game_getter = getter;
            anchors.cutscene_player_outer_offset = *outer;
            anchors.cutscene_player_inner_offset = *inner;
            DMK::log().info(
                "Anchors: cutscene configuration at RVA {:#x}, enqueue at RVA {:#x}, player at +{:#x} +{:#x}",
                create - game.base.raw(),
                enqueue - game.base.raw(),
                anchors.cutscene_player_outer_offset,
                anchors.cutscene_player_inner_offset
            );
        }

        /** @brief The two invoke slots of ExecuteConsoleString's method wrapper; both must be function entries. */
        void resolve_quest_console(std::uintptr_t method_vtable, DMK::Region game, Anchors &anchors)
        {
            if (method_vtable == 0)
            {
                return;
            }
            const std::uintptr_t invoke = slot_of(method_vtable, constants::CONSOLE_STRING_METHOD_INVOKE_SLOT, game);
            const std::uintptr_t invoke_list =
                slot_of(method_vtable, constants::CONSOLE_STRING_METHOD_INVOKE_LIST_SLOT, game);
            if (!is_function_entry(invoke, game) || !is_function_entry(invoke_list, game) || invoke == invoke_list)
            {
                DMK::log().warning(
                    "Anchors: ExecuteConsoleString's invoke slots did not check out; quest console is off"
                );
                return;
            }
            anchors.console_string_method_vtable = method_vtable;
            anchors.console_string_invoke = invoke;
            anchors.console_string_invoke_list = invoke_list;
            DMK::log().info(
                "Anchors: ExecuteConsoleString invoke at RVA {:#x}, invoke list at RVA {:#x}",
                invoke - game.base.raw(),
                invoke_list - game.base.raw()
            );
        }

        /** @brief CAnimSequence::TriggerTrackEvent, checked as a whole body that calls the listener notifier. */
        void resolve_scene_events(std::uintptr_t sequence_vtable, DMK::Region game, Anchors &anchors)
        {
            if (sequence_vtable == 0)
            {
                return;
            }
            const std::uintptr_t trigger =
                slot_of(sequence_vtable, constants::ANIM_SEQUENCE_TRIGGER_TRACK_EVENT_SLOT, game);
            if (!is_function_entry(trigger, game) ||
                !code_window_matches(trigger, TRIGGER_TRACK_EVENT_BODY, TRIGGER_TRACK_EVENT_BODY_LENGTH, game) ||
                !is_function_entry(branch_target(trigger + TRIGGER_TRACK_EVENT_NOTIFY_AT, game), game))
            {
                DMK::log().warning("Anchors: TriggerTrackEvent did not check out; scene events are off");
                return;
            }
            anchors.trigger_track_event = trigger;
            DMK::log().info("Anchors: TriggerTrackEvent at RVA {:#x}", trigger - game.base.raw());
        }

        /** @brief The game's way of making one entity linkable, decoded from its spawn listener. */
        void resolve_linkables(std::uintptr_t listener, DMK::Region game, Anchors &anchors)
        {
            if (listener == 0)
            {
                return;
            }
            // `mov rsi, [rip+disp32]`: the manager static.
            const std::uintptr_t load = listener + LINKABLE_MANAGER_LOAD_AT;
            const bool load_ok = bytes_at(load, {0x48, 0x8B, 0x35});
            const DMK::Result<DMK::Address> manager_static = DMK::scan::resolve_rip_relative(
                DMK::Address{load},
                LINKABLE_MANAGER_DISP_AT,
                LINKABLE_MANAGER_LOAD_LENGTH
            );
            const std::uintptr_t filter = branch_target(listener + LINKABLE_FILTER_CALL_AT, game);
            const std::uintptr_t make = branch_target(listener + LINKABLE_MAKE_CALL_AT, game);
            if (!load_ok || !manager_static.has_value() || !game.contains(*manager_static) ||
                !is_function_entry(filter, game) || !is_function_entry(make, game))
            {
                DMK::log().warning("Anchors: the linkable spawn listener's calls did not check out");
                return;
            }
            const std::uintptr_t manager = manager_static->raw();
            anchors.linkable_manager_static = manager;
            anchors.linkable_filter = filter;
            anchors.linkable_make = make;
            DMK::log().info(
                "Anchors: linkable manager static at RVA {:#x}, make at RVA {:#x}",
                manager - game.base.raw(),
                make - game.base.raw()
            );
        }

        /** @brief Decodes the entity id allocator, or leaves @p anchors.reserve_layer_ids zero when it fails. */
        void resolve_entity_id_allocator(std::uintptr_t entity_system_vtable, Anchors &anchors, DMK::Region game)
        {
            const std::uintptr_t thunk = slot_of(entity_system_vtable, RESERVE_LAYER_IDS_SLOT, game);
            const std::uintptr_t reserve = thunk != 0 && bytes_at(thunk, {0x48, 0x8B, 0x89})
                                               ? branch_target(thunk + RESERVE_LAYER_IDS_JUMP_AT, game)
                                               : 0;
            const std::uintptr_t reserve_one = slot_of(entity_system_vtable, RESERVE_ENTITY_ID_SLOT, game);
            const bool reserve_one_ok = reserve_one != 0 &&
                                        bytes_at(reserve_one + ENTITY_ID_LOCK_AT, {0xF0, 0x0F, 0xB1, 0xB1}) &&
                                        bytes_at(reserve_one + ENTITY_ID_SLOTS_AT, {0x48, 0x8D, 0x8B});
            const std::uintptr_t insert =
                reserve_one_ok ? branch_target(reserve_one + ENTITY_ID_INSERT_CALL_AT, game) : 0;
            const std::uintptr_t unlink = insert != 0 ? branch_target(insert + ENTITY_ID_UNLINK_CALL_AT, game) : 0;
            const std::optional<std::int32_t> lock =
                reserve_one_ok ? read_value<std::int32_t>(reserve_one + ENTITY_ID_LOCK_AT + ENTITY_ID_LOCK_DISP_AT)
                               : std::nullopt;
            const std::optional<std::int32_t> slots =
                reserve_one_ok ? read_value<std::int32_t>(reserve_one + ENTITY_ID_SLOTS_AT + ENTITY_ID_SLOTS_DISP_AT)
                               : std::nullopt;
            const std::optional<std::int32_t> free_head =
                unlink != 0 && bytes_at(unlink + ENTITY_ID_FREE_HEAD_AT, {0x45, 0x39, 0x88})
                    ? read_value<std::int32_t>(unlink + ENTITY_ID_FREE_HEAD_AT + ENTITY_ID_FREE_HEAD_DISP_AT)
                    : std::nullopt;
            // The unlink is a leaf without unwind data: its own shape check above stands in for the entry check.
            if (!is_function_entry(reserve, game) || !is_function_entry(insert, game) || unlink == 0 ||
                !lock.has_value() || !slots.has_value() || !free_head.has_value())
            {
                DMK::log().warning("Anchors: entity id allocator did not check out; Sandbox scene layers stay off");
                return;
            }
            anchors.reserve_layer_ids = reserve;
            anchors.entity_id_lock_offset = *lock;
            anchors.entity_id_slots_offset = *slots;
            anchors.entity_id_free_head_offset = *free_head;
            DMK::log().info(
                "Anchors: ReserveEntityIds at RVA {:#x}, id allocator lock +{:#x} slots +{:#x} free head +{:#x}",
                reserve - game.base.raw(),
                *lock,
                *slots,
                *free_head
            );
        }

        /** @brief What scene layers call: the entity system's layer slots and the profile manager's record list. */
        void resolve_scene_layers(
            std::uintptr_t entity_system_vtable,
            std::uintptr_t manager_vtable,
            std::uintptr_t profiles_parse,
            std::uintptr_t layers_parse,
            std::uintptr_t profile_prepare,
            DMK::Region game,
            Anchors &anchors
        )
        {
            if (entity_system_vtable == 0 || manager_vtable == 0 || profiles_parse == 0 || layers_parse == 0 ||
                profile_prepare == 0)
            {
                return;
            }
            const std::uintptr_t create = branch_target(profiles_parse + PROFILE_RECORD_CREATE_CALL_AT, game);
            const std::uintptr_t insert = branch_target(profiles_parse + PROFILE_RECORDS_INSERT_CALL_AT, game);
            const std::uintptr_t push = branch_target(layers_parse + PROFILE_LAYER_PUSH_CALL_AT, game);
            const std::uintptr_t layer_release = branch_target(layers_parse + LAYER_STRING_RELEASE_CALL_AT, game);
            const std::uintptr_t from_text = branch_target(profile_prepare + STRING_FROM_TEXT_CALL_AT, game);
            const std::uintptr_t release = branch_target(profile_prepare + STRING_RELEASE_CALL_AT, game);
            const std::uintptr_t getter = branch_target(profile_prepare + PROFILE_GAME_GETTER_CALL_AT, game);
            // `mov rcx, [rax+disp32]` and `mov rdi, [rcx+disp32]`.
            const bool loads_match = bytes_at(profile_prepare + PROFILE_MANAGER_OUTER_LOAD_AT, {0x48, 0x8B, 0x88}) &&
                                     bytes_at(profile_prepare + PROFILE_MANAGER_INNER_LOAD_AT, {0x48, 0x8B, 0xB9});
            const std::optional<std::int32_t> outer =
                read_value<std::int32_t>(profile_prepare + PROFILE_MANAGER_OUTER_LOAD_AT + PROFILE_MANAGER_LOAD_DISP_AT);
            const std::optional<std::int32_t> inner =
                read_value<std::int32_t>(profile_prepare + PROFILE_MANAGER_INNER_LOAD_AT + PROFILE_MANAGER_LOAD_DISP_AT);
            if (!is_function_entry(create, game) || !is_function_entry(insert, game) ||
                !is_function_entry(push, game) || !is_function_entry(from_text, game) ||
                !is_function_entry(release, game) || release != layer_release || getter == 0 || !loads_match ||
                !outer.has_value() || !inner.has_value())
            {
                DMK::log().warning("Anchors: the scene layer calls did not check out; scene layers are off");
                return;
            }
            anchors.profile_manager_vtable = manager_vtable;
            anchors.profile_game_getter = getter;
            anchors.profile_manager_outer_offset = *outer;
            anchors.profile_manager_inner_offset = *inner;
            anchors.string_from_text = from_text;
            anchors.string_release = release;
            anchors.profile_record_create = create;
            anchors.profile_records_insert = insert;
            anchors.profile_layer_push = push;
            resolve_entity_id_allocator(entity_system_vtable, anchors, game);
            DMK::log().info(
                "Anchors: profile records at RVA {:#x}, manager at +{:#x} +{:#x}",
                create - game.base.raw(),
                anchors.profile_manager_outer_offset,
                anchors.profile_manager_inner_offset
            );
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
        anchors.script_system_vtable =
            resolve_script_system(gated(Feature::LiveBridge, AnchorId::ScriptSystemVtable), game);
        resolve_movie_sequences(
            gated(Feature::MovieSequences, AnchorId::MovieSystemVtable),
            gated(Feature::MovieSequences, AnchorId::SystemVtable),
            game,
            anchors
        );
        resolve_cutscene_play(
            gated(Feature::CutscenePlay, AnchorId::CutsceneHandlerEnqueue),
            gated(Feature::CutscenePlay, AnchorId::CutsceneHolderVtable),
            game,
            anchors
        );
        resolve_linkables(gated(Feature::Linkables, AnchorId::LinkableSpawn), game, anchors);
        // Only an identity check of the live entity system, so it needs no feature gate of its own.
        anchors.entity_system_vtable = resolved(AnchorId::EntitySystemVtable);
        resolve_scene_layers(
            gated(Feature::SceneLayers, AnchorId::EntitySystemVtable),
            gated(Feature::SceneLayers, AnchorId::GameProfileManagerVtable),
            gated(Feature::SceneLayers, AnchorId::GameProfilesParse),
            gated(Feature::SceneLayers, AnchorId::GameLayersParse),
            gated(Feature::SceneLayers, AnchorId::IngameProfilePrepare),
            game,
            anchors
        );
        resolve_quest_console(gated(Feature::QuestConsole, AnchorId::ConsoleStringMethodVtable), game, anchors);
        resolve_scene_events(gated(Feature::SceneEvents, AnchorId::AnimSequenceVtable), game, anchors);
        return anchors;
    }
} // namespace AnimationFramework
