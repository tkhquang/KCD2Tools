/**
 * @file aob_resolver.hpp
 * @brief Cascading AOB candidate tables and the declarative anchor registry for the mod.
 *
 * A cascade of ordered AOB candidates locates every memory location the mod hooks
 * or reads, rather than a single signature, so a game patch that shifts code only
 * has to leave ONE of three anchors intact for the feature to keep working.
 * DetourModKit's declarative anchor registry drives the resolve over the
 * DMK::scan::resolve ladder, confined to the WHGame.dll image the caller passes as
 * module_base and module_size, NOT the whole process.
 *
 * That confinement is deliberate. WHGame.dll is a normal unpacked PE with every
 * target inside it, so a whole-process scan buys nothing and is actively unsafe.
 * A generic prologue or epilogue candidate can false-match inside another injected
 * module such as a graphics overlay or a sibling mod. The ladder is
 * first-match-wins, so that foreign match shadows the correct in-module one and
 * either disables the feature or hooks an unrelated site. The cascade tables enter
 * the registry as the anchors AnchorId names below. resolve_all_anchors() resolves
 * the whole table in one parallel pass at startup, and each feature reads its
 * anchors through its gate (gated_anchor_address()), which returns 0 when any
 * anchor the feature depends on missed.
 *
 * Candidate order is most-specific first (P1), so a tight anchor wins before a
 * looser fallback. Each cascade carries at least one candidate anchored PAST the
 * 5-byte function prologue, through a negative walk-back that returns to the entry.
 * That mid-body anchor still matches when a sibling mod has inline-hooked the
 * entry, because the overwritten prologue makes the earlier candidates miss and the
 * scan falls through to it.
 *
 * A rung that matches more than once inside its scope is skipped as ambiguous, so
 * a freak collision falls through to the next candidate rather than resolving
 * blindly. A full cascade miss is a clean failure (0), never a guess at an
 * unrelated near-JMP site.
 *
 * Resolution shapes (DMK::scan::Mode -> the DMK::scan::Candidate factory):
 *   - Direct      address = match + disp. Entry-hook targets resolve to the
 *                 function entry (disp 0); mid-body anchors use a negative
 *                 disp equal to the entry->anchor byte distance.
 *   - RipRelative address = (match + instr_len) + int32(match + disp).
 *                 Resolves a lea/mov [rip+disp32] to the data slot it references
 *                 (the global-context storage slot and the g_env base).
 *
 * Both offsets are measured from the pattern's `|` result marker, or from the
 * pattern start when the pattern carries none. instr_len is bounded at the
 * x86-64 maximum instruction length of 15 bytes, so every RipRelative candidate
 * marks its referencing instruction explicitly rather than counting from a
 * distant prefix.
 *
 * Constants the game encodes in its own instructions (vtable slot offsets, member
 * offsets, an event id and its flags) are read with CodeOperand anchors rather
 * than written down, and a Quorum anchor accepts such a value only when
 * independent instructions decode to the same one. The native-turn hook sites are
 * proven with anchors scoped to one function's .pdata range
 * (resolve_turn_decision_layout, resolve_movement_type_offset), so those
 * candidates only have to be unique inside that function.
 *
 * Every cascade below resolves to its one target on the Steam 1.5.6, GOG 1.5 and
 * Game Pass 1.4 builds of WHGame.dll.
 */
#ifndef TPVCAMERA_AOB_RESOLVER_HPP
#define TPVCAMERA_AOB_RESOLVER_HPP

#include <DetourModKit.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace TPVCamera
{

    namespace Aob
    {
        using DMK::scan::Candidate;
        using DMK::scan::Pattern;

        // Global-context storage slot
        // RipRelative: all three candidates resolve the SAME data slot the camera-manager walk reads (context +
        // OFFSET_MANAGER_PTR_STORAGE), across three different functions that load it. None anchors on a short Jcc
        // (rel8 opcodes 70-7F/EB/E3 flip to rel32 across builds and desync the pattern); each pins instead on a
        // distinctive NON-Jcc neighbour of the slot-load `mov rax,[rip+slot]` (48 8B 05, disp32 -> the slot):
        // P1 (sub_180682A08) leads with `mov eax,[rbx+r14]; cmp cs:dword,eax; jg-far` (the SIB 0x33 base/index
        // pins this site over a near-twin that shares the field test) and ends on the `cmp qword [rax+0E0h]`
        // field; P2 (sub_180B284B8) leads with `cmp cs:dword,ebx` and ends on `mov rbp,[rax+0F8h]`; P3
        // (sub_180F1B788) is pinned by the `mov rcx,[rdi+278h]` neighbour and `mov rsi,[rax+0F8h]`. Each rung is
        // one vote of a 2-of-3 quorum, so a coincidental match after a patch must fool two independent code sites
        // at once, and the slot still resolves while any two of the three sites survive.
        inline const Candidate k_contextCandidates[] = {
            Candidate::rip_relative(
                "Context_P1_ReadSlotCmpFieldE0",
                Pattern::literal("42 8B 04 33 39 05 ?? ?? ?? ?? 0F 8F ?? ?? ?? ?? | 48 8B 05 ?? ?? ?? ?? 48 83 B8 E0 "
                                 "00 00 00 00"),
                3, 7),
            Candidate::rip_relative(
                "Context_P2_ReadSlotFieldF8",
                Pattern::literal("39 1D ?? ?? ?? ?? | 48 8B 05 ?? ?? ?? ?? 48 8B A8 F8 00 00 00"),
                3, 7),
            Candidate::rip_relative(
                "Context_P3_ReadSlotField278",
                Pattern::literal("48 8B 05 ?? ?? ?? ?? 48 8B 8F 78 02 00 00 48 8B B0 F8 00 00 00"),
                3, 7),
        };

        // SSystemGlobalEnvironment base (g_env)
        // RipRelative: every rung resolves the g_env base, and the rungs vote in a 2-of-5 quorum. One vote is the
        // `GetIGameFramework` call site, a two-rung ladder: P1 leads with the `mov r8, rdi` that precedes the lea,
        // P2 drops it and anchors on the lea plus the trailing virtual-call chain. Both rungs match the same bytes,
        // so they are one vote, never two. That site only exists on Steam. GOG and Game Pass reshape it. Each other
        // vote is a single rung in its own function: V1 is a struct-init sequence that stores the g_env pointer into a
        // member, and V2-V4 read g_env's first member (`mov rcx, [rip+g_env]`, which addresses the base itself, then
        // `test rcx, rcx` and a virtual call through it): V2 where two such reads follow each other, V3 after a member
        // call (`mov rcx, [rdi+disp32]; call`), V4 after a getter call (`mov rdx, [rcx+8]; mov rcx, rax; call rdx`).
        // V1-V4 match exactly once on Steam 1.5.6, GOG 1.5 and Game Pass 1.4, so a coincidental match must fool two
        // independent sites at once, and g_env still resolves while any two sites survive. There is no fixed-address
        // fallback.
        inline const Candidate k_genvFrameworkSiteCandidates[] = {
            Candidate::rip_relative(
                "Genv_Framework_P1_LeaR8Chain",
                Pattern::literal("4C 8B C7 | 48 8D 15 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8B "
                                 "D3 48 8B 01 FF 50 18"),
                3, 7),
            Candidate::rip_relative(
                "Genv_Framework_P2_LeaChainTail",
                Pattern::literal("48 8D 15 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8B D3 48 8B "
                                 "01 FF 50 18"),
                3, 7),
        };
        inline const Candidate k_genvVoteCandidates[] = {
            Candidate::rip_relative(
                "Genv_V1_LeaStructInit",
                Pattern::literal("48 8D 05 ?? ?? ?? ?? 48 89 0F 4C 8D 67 28 48 8D 0D ?? ?? ?? ?? 48 89 47 20 48 89 "
                                 "4F 08"),
                3, 7),
            Candidate::rip_relative(
                "Genv_V2_FirstMemberReadPair",
                Pattern::literal("| 48 8B 0D ?? ?? ?? ?? 48 85 C9 [2-6] 48 8B 01 33 D2 FF 50 18 48 8B 0D ?? ?? ?? ?? "
                                 "48 8B 01 FF 50 08"),
                3, 7),
            Candidate::rip_relative(
                "Genv_V3_FirstMemberAfterMemberCall",
                Pattern::literal("48 8B 8F ?? ?? ?? ?? E8 ?? ?? ?? ?? | 48 8B 0D ?? ?? ?? ?? 48 85 C9 [2-6] 48 8B 01 "
                                 "B2 01 FF 50 18"),
                3, 7),
            Candidate::rip_relative(
                "Genv_V4_FirstMemberAfterGetter",
                Pattern::literal("48 8B 51 08 48 8B C8 FF D2 | 48 8B 0D ?? ?? ?? ?? 48 85 C9 [2-6] 48 8B 01 33 D2 FF "
                                 "50 18"),
                3, 7),
        };

        // Camera frustum builder (CCamera::UpdateFrustumPlanes) entry
        // Direct entry hook. P1 is the full prologue (mov rax,rsp + 9 pushes +
        // lea + sub) into the first matrix read. P2 drops the `mov rax,rsp` lead
        // and walks back 3 bytes. P3 anchors purely on the body's distinctive
        // matrix-read run (the movss [rcx+disp] chain that reads the 3x4 camera
        // matrix) and walks back 0x1A. The lea displacement and sub-rsp immediate
        // are wildcarded for frame-size resilience.
        inline const Candidate k_frustumCandidates[] = {
            Candidate::direct(
                "Frustum_P1_PrologueMatrixRead",
                Pattern::literal("48 8B C4 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 68 ?? 48 81 EC ?? ?? 00 00 F3 "
                                 "0F 10 09 48 8B D9")),
            Candidate::direct(
                "Frustum_P2_PushChainMatrixRead",
                Pattern::literal("55 53 56 57 41 54 41 55 41 56 41 57 48 8D 68 ?? 48 81 EC ?? ?? 00 00 F3 0F 10 09 "
                                 "48 8B D9 F3 0F 10 59 08"),
                -3),
            Candidate::direct(
                "Frustum_P3_MatrixReadBody",
                Pattern::literal("F3 0F 10 09 48 8B D9 F3 0F 10 59 08 F3 0F 10 51 10 F3 0F 10 41 24 F3 0F 10 61 28"),
                -0x1A),
        };

        // Head-visibility setter entry
        // Direct entry hook (this, bool hide_head /*dl*/, char flags /*r8b*/).
        // P1 is the current prologue through `mov sil, r8b`. P2 extends through
        // the `mov dil,dl; mov rbx,rcx; call; test al,al` body for extra pinning.
        // P3 drops the two stack-save stores and walks back 0x0A from the push.
        inline const Candidate k_headVisibilityCandidates[] = {
            Candidate::direct(
                "Head_P1_PrologueMovSil",
                Pattern::literal("48 89 5C 24 10 48 89 74 24 18 57 48 83 EC ?? 41 8A F0")),
            Candidate::direct(
                "Head_P2_PrologueMovSilCall",
                Pattern::literal("48 89 5C 24 10 48 89 74 24 18 57 48 83 EC ?? 41 8A F0 40 8A FA 48 8B D9 E8 ?? ?? "
                                 "?? ?? 84 C0")),
            Candidate::direct(
                "Head_P3_BodyMovSilDilRbx",
                Pattern::literal("57 48 83 EC ?? 41 8A F0 40 8A FA 48 8B D9 E8 ?? ?? ?? ?? 84 C0 74"),
                -0x0A),
        };

        // Generic input-event dispatcher entry
        // Direct entry hook, located by walking back from a mid-body landmark to the entry. The function is
        // reached only virtually (no call site) and its prologue is a generic shape that is not unique on its
        // own, so all three anchors are distinctive body runs PAST the prologue - which also means a sibling
        // 5-byte prologue hook does not break them. None anchors on a short Jcc: the volatile `jnz rel8` guard is
        // never inside the pattern, and the far `jz rel32` branches that ARE included have their rel32 wildcarded.
        // P1 = `cmp [rcx+0D8h],r8b; jz-far; cmp [rdx+10h],-1; jz-far` (entry+0x15); P2 = that pitch check into
        // `mov rax,[rip]; mov ecx,[rax]; test ecx,ecx` (entry+0x22); P3 = the eIS_Changed block
        // `movzx; movss; mov rcx,[rip]; mov r8,[rbx+8]; cvtps2pd` (entry+0x50). Each walks back to the entry.
        inline const Candidate k_inputDispatchCandidates[] = {
            Candidate::direct(
                "Input_P1_BodyFlagCmpFarJz",
                Pattern::literal("44 38 81 D8 00 00 00 0F 84 ?? ?? ?? ?? 83 7A 10 FF 0F 84 ?? ?? ?? ??"),
                -0x15),
            Candidate::direct(
                "Input_P2_PitchCmpRipMov",
                Pattern::literal("83 7A 10 FF 0F 84 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 8B 08 85 C9"),
                -0x22),
            Candidate::direct(
                "Input_P3_ChangedLogBlock",
                Pattern::literal("0F B6 52 28 F3 0F 10 5B 18 48 8B 0D ?? ?? ?? ?? 4C 8B 43 08 0F 5A DB"),
                -0x50),
        };

        // Global action dispatcher entry (Lua Player:OnAction source)
        // Direct entry hook. The `movss [rax+20h], xmm3` that stores the float
        // value arg is the distinctive head. P2 extends past the sub-rsp into the
        // first movaps + `mov rdi,rcx`. P3 anchors on the movss-store body and
        // walks back 0x0B. Frame displacement and stack size are wildcarded.
        inline const Candidate k_actionDispatchCandidates[] = {
            Candidate::direct(
                "Action_P1_PrologueMovssVal",
                Pattern::literal("48 8B C4 48 89 58 10 48 89 70 18 F3 0F 11 58 20 55 57 41 56 48 8D 68 ?? 48 81 EC "
                                 "?? ?? ?? ??")),
            Candidate::direct(
                "Action_P2_PrologueThroughMovaps",
                Pattern::literal("48 8B C4 48 89 58 10 48 89 70 18 F3 0F 11 58 20 55 57 41 56 48 8D 68 ?? 48 81 EC "
                                 "?? ?? ?? ?? 0F 29 70 ?? 48 8B F9")),
            Candidate::direct(
                "Action_P3_MovssValBody",
                Pattern::literal("F3 0F 11 58 20 55 57 41 56 48 8D 68 ?? 48 81 EC ?? ?? ?? ?? 0F 29 70 ?? 48 8B F9 "
                                 "8B 41 18 41 BE 01 00 00 00"),
                -0x0B),
        };

        // IPhysicalWorld::RayWorldIntersection helper entry
        // Direct: the match IS the callable function pointer (no detour). A
        // sibling helper (sub_1838D6E3C) shares the prologue and the first inner
        // call, then diverges: this helper stages the next call's count with
        // `mov r9d, imm32` (41 B9) where the sibling does `mov rcx, r9`. EVERY
        // candidate therefore extends to the 41 B9 discriminator so none can
        // resolve onto the sibling. P2 drops the `mov rax,rsp` lead (walk back 3);
        // P3 anchors on the arg-staging body (walk back 0x1F).
        inline const Candidate k_rayWorldIntersectionCandidates[] = {
            Candidate::direct(
                "Ray_P1_PrologueThroughR9dImm",
                Pattern::literal("48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 4C 89 70 20 55 48 8D 68 ?? 48 81 EC "
                                 "?? ?? 00 00 48 8B DA 49 8B F8 33 D2 4C 8B F1 48 8D 4D ?? 41 8B F1 44 8D 42 70 E8 "
                                 "?? ?? ?? ?? 8B 43 08 48 8D 55 ?? F2 0F 10 03 41 B9 ?? ?? ?? ??")),
            Candidate::direct(
                "Ray_P2_SavesThroughR9dImm",
                Pattern::literal("48 89 58 08 48 89 70 10 48 89 78 18 4C 89 70 20 55 48 8D 68 ?? 48 81 EC ?? ?? 00 "
                                 "00 48 8B DA 49 8B F8 33 D2 4C 8B F1 48 8D 4D ?? 41 8B F1 44 8D 42 70 E8 ?? ?? ?? "
                                 "?? 8B 43 08 48 8D 55 ?? F2 0F 10 03 41 B9"),
                -3),
            Candidate::direct(
                "Ray_P3_BodyThroughR9dImm",
                Pattern::literal("48 8B DA 49 8B F8 33 D2 4C 8B F1 48 8D 4D ?? 41 8B F1 44 8D 42 70 E8 ?? ?? ?? ?? "
                                 "8B 43 08 48 8D 55 ?? F2 0F 10 03 41 B9"),
                -0x1F),
        };

        // I3DEngine::GetObjectsInBox (render-node octree query) entry
        // Direct: the match IS the callable function (no detour). The function is identified by its
        // standard frame save followed IMMEDIATELY by `mov rcx, [rcx+698h]` (the C3DEngine octree root),
        // a load shared by no other function in the image, so the entry is unambiguous. P1 anchors the
        // entry through that octree load; P2 drops the leading `mov rax,rsp` (walk back 3); P3 anchors on
        // the octree-load body and walks back 0x10 to the entry. The `sub rsp` frame allocation and the
        // frame-relative stack-slot displacements (mov/lea [rax-disp8]) are wildcarded for frame-size
        // resilience; the +698h octree-root displacement is the semantic landmark and stays literal. All
        // three verified to match exactly once over the full WHGame.dll image.
        inline const Candidate k_getObjectsInBoxCandidates[] = {
            Candidate::direct(
                "GetObjInBox_P1_PrologueThroughOctree",
                Pattern::literal("48 8B C4 48 89 58 08 48 89 70 10 57 48 83 EC ?? 48 8B 89 98 06 00 00 49 8B F0 4C "
                                 "8B C2")),
            Candidate::direct(
                "GetObjInBox_P2_SavesThroughOctree",
                Pattern::literal("48 89 58 08 48 89 70 10 57 48 83 EC ?? 48 8B 89 98 06 00 00 49 8B F0 4C 8B C2 48 "
                                 "C7 40 ?? 00 00 00 00"),
                -3),
            Candidate::direct(
                "GetObjInBox_P3_OctreeLoadBody",
                Pattern::literal("48 8B 89 98 06 00 00 49 8B F0 4C 8B C2 48 C7 40 ?? 00 00 00 00 48 8B DA 0F 57 C0 "
                                 "48 8D 50 ??"),
                -0x10),
        };

        // Return address of the IsThirdPerson call in C_PlayerMovementAction's turn trigger (sub_1809490D8)
        // Direct, NOT a hook target: the IsThirdPerson detour compares its own return address with this one and
        // answers "third person" there, so the free-roam locomotion action starts its turn-in-place fragments once
        // the look leads the body by more than 35 degrees. The `|` marker sits on the instruction after the call.
        // P1 pins the |angle| computation (andps abs mask, comiss against the move threshold) into the virtual call.
        // P2 pins the call and the 35-degree comparison that consumes its result, and P3 drops P1's andps. P1 and P2
        // wildcard the IsThirdPerson slot displacement and P3 keeps it literal, so a re-numbered vtable and a changed
        // neighbourhood are separate failure domains. Every branch is crossed with a [2-6] gap, which spans both its
        // short and its near encoding. Each matches exactly once on Steam 1.5.6 (0x18094948C), GOG 1.5 and
        // Game Pass 1.4.
        inline const Candidate k_turnTriggerReturnCandidates[] = {
            Candidate::direct(
                "TurnTrigger_P1_AngleThroughCall",
                Pattern::literal("0F 54 3D ?? ?? ?? ?? 41 0F 2F F8 [2-6] 48 8B 03 48 8B CB FF 90 ?? ?? ?? ?? |")),
            Candidate::direct(
                "TurnTrigger_P2_CallThroughThreshold",
                Pattern::literal("FF 90 ?? ?? ?? ?? | 84 C0 [2-6] 0F 2F 3D ?? ?? ?? ?? 0F 97 C1 [2-6] 32 C9 F3 0F 10 "
                                 "05")),
            Candidate::direct("TurnTrigger_P3_CompareThroughCall",
                              Pattern::literal("41 0F 2F F8 [2-6] 48 8B 03 48 8B CB FF 90 40 02 00 00 |")),
        };

        // CAnimatedCharacter::UpdatePhysicalEntityMovement (sub_1808A3188) entry
        // Direct entry hook. It receives the frame's movement as a QuatT (rotation, then translation): it composes the
        // rotation into the body and requests the translation from physics as a velocity. Hooked so a native
        // turn-in-place step turns the body without moving it. P1 is the full prologue (mov rax,rsp, the register saves
        // and pushes, frame lea and sub) through `mov rsi,rdx`; P2 drops the leading `mov rax,rsp` and walks back 3; P3
        // anchors on the body after the prologue (`mov r15,[rcx+38h]; xor r13d,r13d`, the xmm saves) and walks back
        // 0x26. Frame sizes wildcarded. Verified to match exactly once on Steam 1.5.6 (0x1808A3188), GOG 1.5 and
        // Game Pass 1.4.
        inline const Candidate k_physEntMovementCandidates[] = {
            Candidate::direct(
                "PhysEntMove_P1_PrologueThroughArgs",
                Pattern::literal("48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 41 54 41 55 41 56 41 57 48 8D A8 ?? "
                                 "?? ?? ?? 48 81 EC ?? ?? ?? ?? 4C 8B 79 38 45 33 ED 0F 29 70 C8 48 8B F9 0F 29 78 "
                                 "B8 48 8B F2")),
            Candidate::direct(
                "PhysEntMove_P2_SavesThroughBody",
                Pattern::literal("48 89 58 08 48 89 70 10 48 89 78 18 55 41 54 41 55 41 56 41 57 48 8D A8 ?? ?? ?? ?? "
                                 "48 81 EC ?? ?? ?? ?? 4C 8B 79 38 45 33 ED 0F 29 70 C8 48 8B F9"),
                -3),
            Candidate::direct(
                "PhysEntMove_P3_BodyAfterPrologue",
                Pattern::literal("4C 8B 79 38 45 33 ED 0F 29 70 C8 48 8B F9 0F 29 78 B8 48 8B F2 44 0F 29 40 A8 44 0F "
                                 "29 48 98"),
                -0x26),
        };

        // Return address of the IsThirdPerson call in C_PlayerMovementAction's idle LockBodyTurn sync (sub_180B66D24)
        // Direct, NOT a hook target. On an idle fragment start and on the camera-changed SGameObjectEvent the action
        // re-reads IsThirdPerson here and takes (third person) or drops (first person) its LockBodyTurn reference. The
        // detour answers "third person" at this return address so the body stops following the look while idle.
        // P1 pins the preceding GetAnimatedCharacter call through the IsThirdPerson call and the latch-byte read
        // (mov cl,[rbp+disp32]) that follows. P2 and P3 are shorter windows around the call and the latch read. P1
        // and P2 wildcard both vtable displacements and the latch field, P3 keeps them literal. Each matches exactly
        // once on Steam 1.5.6 (0x180B66D8C), GOG 1.5 and Game Pass 1.4.
        inline const Candidate k_lockSyncReturnCandidates[] = {
            Candidate::direct(
                "LockSync_P1_AnimCharThroughLatch",
                Pattern::literal("48 8B 91 ?? ?? ?? ?? 48 8B C8 FF D2 48 8B 0F 48 8B F0 48 8B 91 ?? ?? ?? ?? 48 8B "
                                 "CF FF D2 | 8A 8D ?? ?? ?? ??")),
            Candidate::direct(
                "LockSync_P2_CallThroughTest",
                Pattern::literal("48 8B 0F 48 8B F0 48 8B 91 ?? ?? ?? ?? 48 8B CF FF D2 | 8A 8D ?? ?? ?? ?? 84 C0")),
            Candidate::direct(
                "LockSync_P3_CallThroughLatch",
                Pattern::literal("48 8B 91 40 02 00 00 48 8B CF FF D2 | 8A 8D F2 00 00 00 84 C0")),
        };

        // The values below are read out of the game's own instructions with DetourModKit CodeOperand anchors, so a
        // patch that renumbers a vtable, moves a member or renumbers an event is followed instead of trusted. Each
        // `|` sits on the instruction start the operand is decoded from. Where independent instructions encode the
        // same value, a Quorum anchor accepts it only when they agree, so one coincidental match cannot supply a
        // wrong value. Each rung matches exactly once on Steam 1.5.6, GOG 1.5 and Game Pass 1.4 and decodes to the
        // same value on all three.

        // IsThirdPerson's vtable slot (byte offset), from its two call sites above: the turn trigger's
        // `call [rax+disp32]` and the idle LockBodyTurn sync's `mov rdx, [rcx+disp32]`, which loads the slot for the
        // `call rdx` after it.
        inline const Candidate k_isThirdPersonSlotTriggerCandidates[] = {
            Candidate::direct("ItpSlotTrigger_P1_CallThroughThreshold",
                              Pattern::literal("48 8B 03 48 8B CB | FF 90 ?? ?? ?? ?? 84 C0 [2-6] 0F 2F 3D")),
            Candidate::direct("ItpSlotTrigger_P2_CompareThroughCall",
                              Pattern::literal("41 0F 2F F8 [2-6] 48 8B 03 48 8B CB | FF 90 ?? ?? ?? ?? 84 C0")),
        };
        inline const Candidate k_isThirdPersonSlotLockSyncCandidates[] = {
            Candidate::direct(
                "ItpSlotLockSync_P1_LoadThroughLatch",
                Pattern::literal("48 8B 0F 48 8B F0 | 48 8B 91 ?? ?? ?? ?? 48 8B CF FF D2 8A 8D ?? ?? ?? ?? 84 C0")),
            Candidate::direct("ItpSlotLockSync_P2_AnimCharThroughLoad",
                              Pattern::literal("48 8B F8 48 8B 08 48 8B 91 ?? ?? ?? ?? 48 8B C8 FF D2 48 8B 0F 48 8B "
                                               "F0 | 48 8B 91 ?? ?? ?? ??")),
        };

        // The camera-changed SGameObjectEvent. When the camera manager switches the active camera it builds the event
        // on the stack (`lea rax, SGameObjectEvent vftable`, `mov [rsp+28h], id`, `mov [rsp+2Ch], flags`, a zeroed
        // 16-byte parameter) and calls the client actor's HandleEvent through its vtable (`call [rax+disp32]`). Event
        // ids differ between builds of the engine, so the id is never assumed: the sender's immediate,
        // C_Player::HandleEvent's dispatch (`cmp dword [rsi+8], imm8`, which hands the event on to the player's
        // movement controller) and C_PlayerMovementAction::OnEvent's gate (`cmp dword [rdx+8], imm8`) vote, and two
        // of the three must agree. Another sender builds another event the same way and calls it through
        // `call [rax+disp8]`, so each sender rung reaches the `call [rax+disp32]` (P1) or the client-actor fetch
        // before the event (P2).
        inline const Candidate k_cameraEventSendIdCandidates[] = {
            Candidate::direct(
                "CameraEventSend_P1_IdThroughCall",
                Pattern::literal("48 8D 05 ?? ?? ?? ?? | C7 44 24 ?? ?? ?? ?? ?? 48 89 44 24 ?? 48 8D 54 24 ?? 48 8B "
                                 "01 0F 57 C0 C7 44 24 ?? ?? ?? ?? ?? F3 0F 7F 44 24 ?? FF 90")),
            Candidate::direct(
                "CameraEventSend_P2_ActorFetchThroughId",
                Pattern::literal("FF 90 ?? ?? ?? ?? 48 8B C8 48 85 C0 [2-6] 48 8D 05 ?? ?? ?? ?? | C7 44 24 ?? ?? ?? "
                                 "?? ?? 48 89 44 24")),
        };
        // The flags and slot reads each carry a second rung with its context on the other side of the instruction,
        // so a change on one side still resolves.
        inline const Candidate k_cameraEventSendFlagsCandidates[] = {
            Candidate::direct("CameraEventSend_P1_Flags",
                              Pattern::literal("48 8D 54 24 ?? 48 8B 01 0F 57 C0 | C7 44 24 ?? ?? ?? ?? ?? F3 0F 7F 44 "
                                               "24 ?? FF 90")),
            Candidate::direct("CameraEventSend_P2_FlagsThroughReturn",
                              Pattern::literal("| C7 44 24 ?? ?? ?? ?? ?? F3 0F 7F 44 24 ?? FF 90 ?? ?? ?? ?? 48 8B 5C "
                                               "24")),
        };
        inline const Candidate k_cameraEventSendSlotCandidates[] = {
            Candidate::direct(
                "CameraEventSend_P1_HandleEventSlot",
                Pattern::literal("48 8B 01 0F 57 C0 C7 44 24 ?? ?? ?? ?? ?? F3 0F 7F 44 24 ?? | FF 90 ?? ?? ?? ??")),
            Candidate::direct("CameraEventSend_P2_SlotThroughReturn",
                              Pattern::literal("F3 0F 7F 44 24 ?? | FF 90 ?? ?? ?? ?? 48 8B 5C 24 ?? 48 8B 6C 24")),
        };
        inline const Candidate k_cameraEventHandleEventIdCandidates[] = {
            Candidate::direct("CameraEventHandleEvent_P1_Dispatch",
                              Pattern::literal("| 83 7E 08 ?? [2-6] 48 8B CF E8 ?? ?? ?? ?? 48 85 C0 [2-6] 48 8B 08 48 "
                                               "8B D6 4C 8B 01 48 8B C8 41 FF D0")),
        };
        inline const Candidate k_cameraEventOnEventIdCandidates[] = {
            Candidate::direct("CameraEventOnEvent_P1_Gate",
                              Pattern::literal("40 53 48 83 EC ?? | 83 7A 08 ?? 48 8B D9 [2-6] 83 B9 ?? ?? ?? ?? 00 "
                                               "[2-6] 48 8B 15")),
        };

        // C_PlayerMovementAction's installed-turn state (int32, where 1 and 2 mean an installed turn fragment), from
        // the turn trigger's `mov eax, [rdi+disp32]; cmp eax, ebp; jz; cmp eax, 2`, which the spin-latch compare
        // branches to while the latch is clear, and from OnEvent's camera-changed gate `cmp dword [rcx+disp32], 0`.
        // Each member has a second rung with its context on the other side of the read.
        inline const Candidate k_turnStateTriggerCandidates[] = {
            Candidate::direct("TurnStateTrigger_P1_ThroughLastSignRead",
                              Pattern::literal("| 8B 87 ?? ?? ?? ?? 3B C5 [2-6] 83 F8 02 [2-6] F3 0F 10 87 ?? ?? ?? ?? "
                                               "0F 2E D0")),
            Candidate::direct(
                "TurnStateTrigger_P2_AfterLatchHold",
                Pattern::literal("0F 97 C0 0F 2F F9 [2-6] 84 C0 [2-6] 8B C5 [2-6] | 8B 87 ?? ?? ?? ?? 3B C5")),
        };
        inline const Candidate k_turnStateOnEventCandidates[] = {
            Candidate::direct("TurnStateOnEvent_P1_GateThroughFrame",
                              Pattern::literal("83 7A 08 ?? 48 8B D9 [2-6] | 83 B9 ?? ?? ?? ?? 00 [2-6] 48 8B 15 ?? ?? "
                                               "?? ?? 48 8D 4C 24")),
            Candidate::direct("TurnStateOnEvent_P2_PrologueThroughGate",
                              Pattern::literal("40 53 48 83 EC ?? 83 7A 08 ?? 48 8B D9 [2-6] | 83 B9 ?? ?? ?? ?? 00")),
        };

        // C_Player's LockBodyTurn reference count (int32), from LockBodyTurn itself: `cmp dword [rcx+disp32], 1`
        // before it adds or removes one. Read only, for the log.
        inline const Candidate k_lockBodyTurnCountCandidates[] = {
            Candidate::direct("LockBodyTurnCount_P1_CompareThroughArg",
                              Pattern::literal("| 83 B9 ?? ?? ?? ?? 01 48 8B D9 0F 29 74 24 ?? 0F B6 FA")),
            Candidate::direct("LockBodyTurnCount_P2_PrologueThroughCompare",
                              Pattern::literal("48 89 5C 24 08 57 48 83 EC ?? | 83 B9 ?? ?? ?? ?? 01 48 8B D9")),
        };

        // The ladders below resolve inside one function's .pdata range, not the whole image, so each only has to be
        // unique within that function and can stay short. ComputeMoveState is the function around the turn-trigger
        // return address. Its `this`, the C_PlayerMovementAction, stays in rdi from `mov rdi, rcx` in the prologue,
        // and every field rung addresses through rdi, which also proves the register the decision hook reads the
        // action from.

        // The spin latch (uint8): the compare `cmp byte [rdi+disp32], sil` and the store `mov [rdi+disp32], al` after
        // `xor eax, eax` (the path that sets the latch jumps straight to that store with al = 1).
        inline const Candidate k_turnLatchCompareCandidates[] = {
            Candidate::direct("TurnLatch_P1_Compare", Pattern::literal("| 40 38 B7 ?? ?? ?? ??")),
        };
        inline const Candidate k_turnLatchStoreCandidates[] = {
            Candidate::direct("TurnLatch_P1_Store", Pattern::literal("33 C0 | 88 87 ?? ?? ?? ?? 84 C0")),
        };

        // The previous evaluation's gap sign (float, 0 = none): the store before the turn test
        // (`movss [rdi+disp32], xmm2; test cl, cl`) and the load the latch condition compares the new sign with
        // (`movss xmm0, [rdi+disp32]; ucomiss xmm2, xmm0`).
        inline const Candidate k_turnSignStoreCandidates[] = {
            Candidate::direct("TurnSign_P1_Store", Pattern::literal("| F3 0F 11 97 ?? ?? ?? ?? 84 C9")),
        };
        inline const Candidate k_turnSignLoadCandidates[] = {
            Candidate::direct("TurnSign_P1_Load", Pattern::literal("| F3 0F 10 87 ?? ?? ?? ?? 0F 2E D0")),
        };

        // The decision hook's register contract, filling exactly the bytes before the turn-trigger return address:
        // the signed look-minus-body angle is copied to xmm13 (callee-saved, so it survives the IsThirdPerson call)
        // and its absolute value to xmm7, xmm13 is only read up to the call, and rcx = rbx = the actor for the call.
        // Every opcode is literal, because one more or one different instruction can write xmm13. Only the
        // abs-mask, branch and slot displacements are wildcarded.
        inline const Candidate k_turnContractCandidates[] = {
            Candidate::direct("TurnContract_P1_AngleThroughCall",
                              Pattern::literal("44 0F 28 E8 41 0F 28 FD 0F 54 3D ?? ?? ?? ?? 41 0F 2F F8 0F 86 ?? ?? "
                                               "?? ?? 48 8B 03 48 8B CB FF 90 ?? ?? ?? ??")),
        };

        // The decision, matched exactly at the return address:
        //   test al, al
        //   jz                         to `xor cl, cl` (idle)
        //   comiss xmm7, [rip+disp32]  |angle| against the game's 35 degrees
        //   seta cl                    the hook site, at the `|`
        //   jmp short                  over `xor cl, cl` to the join
        //   xor cl, cl
        // Both short branch distances are literal: they prove that both paths reach the join with cl set, and that
        // nothing between the IsThirdPerson call and the hook writes a register the hook reads (rbx, rdi, r12,
        // xmm13). The hook replaces exactly `seta cl` and the `jmp short`, so the `xor cl, cl` the `jz` lands on stays
        // in place.
        inline const Candidate k_turnDecisionSiteCandidates[] = {
            Candidate::direct("TurnDecisionSite_P1_TestCompareSeta",
                              Pattern::literal("84 C0 74 0C 0F 2F 3D ?? ?? ?? ?? | 0F 97 C1 EB 02 32 C9")),
        };

        // ComputeMoveState's turn-angle output pointer: `mov r12, rdx` in the prologue (searched within the prologue
        // length the unwind info declares) and `test r12, r12; jz; movss [r12], xmm` storing through it.
        inline const Candidate k_turnOutputSaveCandidates[] = {
            Candidate::direct("TurnOutput_P1_Save", Pattern::literal("4C 8B E2")),
        };
        inline const Candidate k_turnOutputStoreCandidates[] = {
            Candidate::direct("TurnOutput_P1_Store", Pattern::literal("4D 85 E4 [2-6] F3 41 0F 11 ?? 24")),
        };

        // CAnimatedCharacter's movement request type (int32: 1 absolute, 2 impulse), inside
        // UpdatePhysicalEntityMovement, where rdi holds the CAnimatedCharacter: `mov r9d, [rdi+disp32]; cmp r9d, 2`.
        inline const Candidate k_movementTypeCandidates[] = {
            Candidate::direct("MovementType_P1_CompareImpulse", Pattern::literal("| 44 8B 8F ?? ?? ?? ?? 41 83 F9 02")),
        };

        // CActionScope::InstallAnimation's lookup of a clip's animation:
        //   mov rdx, [r14]        the clip's 64-bit name hash (animRef.crc)
        //   mov rcx, [rax]        rax is the scope character's CAnimationSet, rcx its vtable
        //   mov r8, [rcx+disp8]   GetAnimIDByCRC, at the `|`
        //   mov rcx, rax
        //   call r8
        //   mov ebx, eax
        //   test eax, eax; js     a negative id, an animation the set does not hold
        // The displacement is GetAnimIDByCRC's vtable byte offset. P2 pins the GetIAnimationSet call before it.
        inline const Candidate k_animIdByCrcCallCandidates[] = {
            Candidate::direct("AnimIdByCrc_P1_InstallAnimation",
                              Pattern::literal("49 8B 16 48 8B 08 | 4C 8B 41 ?? 48 8B C8 41 FF D0 8B D8 85 C0 0F 88")),
            Candidate::direct(
                "AnimIdByCrc_P2_AnimationSetThroughSlot",
                Pattern::literal("48 8B 48 ?? 48 8B 01 FF 90 ?? ?? ?? ?? 49 8B 16 48 8B 08 | 4C 8B 41 ??")),
        };

        // CAnimationSet::GetAnimIDByName's call to the game's animation-name hash (name, length):
        //   cmp byte [rdx+rax], 0; jnz   the end of the strlen loop over the name
        //   mov edx, eax                 the length
        //   mov rcx, r8                  the name
        //   call rel32                   the name hash, at the `|`
        //   lea rcx, [rbx+disp8]         the name map, which GetAnimIDByCRC's thunk also selects
        //   mov rdx, rax                 the hash, for the map lookup the function tail-jumps to
        // A RipRelative candidate decodes a RIP-relative memory operand, never a branch, so the anchor resolves the
        // call itself, and its consumer decodes the callee with scan::resolve_rip_relative.
        inline const Candidate k_animNameHashCallCandidates[] = {
            Candidate::direct(
                "AnimNameHash_P1_CallThroughMapArgs",
                Pattern::literal("80 3C 02 00 [2-6] 8B D0 49 8B C8 | E8 ?? ?? ?? ?? 48 8D 4B ?? 48 8B D0")),
            Candidate::direct("AnimNameHash_P2_LengthLoopThroughCall",
                              Pattern::literal("48 83 C8 FF 48 FF C0 80 3C 02 00 [2-6] 8B D0 49 8B C8 | E8")),
        };

        // Identity checks on a vtable slot's target, searched within its first bytes (see read_checked_vtable_slot in
        // camera_hook.cpp). IsThirdPerson asks the active camera (`mov rdx, [rcx+disp8]; mov rcx, rax; call rdx`).
        // C_CameraObserver's update fetches the view camera through ISystem (`call [rax+disp32]`, whose ISystem slot
        // differs between builds) and copies its translation column (`[rax+0Ch]`, `[rax+1Ch]`, `[rax+2Ch]`) into
        // its position out-parameter.
        inline constexpr Pattern k_isThirdPersonBody = Pattern::literal("48 8B 51 ?? 48 8B C8 FF D2");
        inline constexpr std::size_t k_isThirdPersonBodyWindow = 0x50;
        inline constexpr Pattern k_cameraObserverUpdateBody =
            Pattern::literal("48 8B 01 FF 90 ?? ?? ?? ?? 8B 48 0C F3 0F 10 40 1C F3 0F 10 48 2C");
        inline constexpr std::size_t k_cameraObserverUpdateBodyWindow = 0x40;
        // CAnimationSet's GetAnimIDByCRC slot holds a thunk, matched at its first byte: `add rcx, imm8` (to the name
        // map), then `jmp rel32` (to the map lookup).
        inline constexpr Pattern k_animIdByCrcThunk = Pattern::literal("48 83 C1 ?? E9");
        // The animation-name hash, matched at its first byte before the mod calls it: the register saves and pushes,
        // the frame, `mov edi, edx` (the length), `mov rbp, rcx` (the name), then `cmp edx, 20h` (its first branch on
        // the length).
        inline constexpr Pattern k_animNameHashBody = Pattern::literal(
            "48 89 5C 24 08 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 83 EC ?? 8B FA 48 8B E9 83 FA 20");

        // Interaction ray-query builder entry
        // Direct entry hook. The function is a leaf-style Vec3 copier with no
        // standard prologue, so all anchors are body-shaped. P2 extends the
        // vec-copy run; P3 anchors on the distinctive `mov [rcx+228h], al` store
        // and walks back 0x1B.
        inline const Candidate k_interactionRayBuildCandidates[] = {
            Candidate::direct(
                "RayBuild_P1_VecCopyHead",
                Pattern::literal("F2 0F 10 02 4C 8B D1 4C 8B 5C 24 30 F2 0F 11 01 8B 42 08 89 41 08 F2 41 0F 10 00")),
            Candidate::direct(
                "RayBuild_P2_VecCopyExtended",
                Pattern::literal("F2 0F 10 02 4C 8B D1 4C 8B 5C 24 30 F2 0F 11 01 8B 42 08 89 41 08 F2 41 0F 10 00 "
                                 "F2 0F 11 41 0C 41 8B 40 08 89 41 14")),
            Candidate::direct(
                "RayBuild_P3_Store228Body",
                Pattern::literal("F2 0F 11 41 0C 41 8B 40 08 89 41 14 8B 44 24 28 89 41 1C 8A 44 24 40 88 81 28 02 "
                                 "00 00"),
                -0x1B),
        };

        // Interactor look-ray builder entry (used as a caller-range bound) -
        // Direct: resolves the function entry; the caller range [entry, entry +
        // INTERACTOR_LOOKRAY_SPAN) gates the ray-build detour. The xmm-save
        // prologue is shared by many functions, so P2/P3 extend past the saves
        // into the stack-canary load (`mov rax,[rip+cookie]; xor rax,rsp`) to
        // stay unique. P2 walks back 0x0B, P3 walks back 0x21.
        inline const Candidate k_interactorLookRayCandidates[] = {
            Candidate::direct(
                "LookRay_P1_PrologueXmmSaves",
                Pattern::literal("48 8B C4 48 89 58 10 48 89 70 18 55 57 41 54 41 56 41 57 48 8D A8 ?? ?? FF FF 48 "
                                 "81 EC ?? ?? 00 00 0F 29 70 C8 0F 29 78 B8 44 0F 29 40 A8 44 0F 29 50 98 44 0F 29 "
                                 "58 88")),
            Candidate::direct(
                "LookRay_P2_PushXmmCanary",
                Pattern::literal("55 57 41 54 41 56 41 57 48 8D A8 ?? ?? FF FF 48 81 EC ?? ?? 00 00 0F 29 70 C8 0F "
                                 "29 78 B8 44 0F 29 40 A8 44 0F 29 50 98 44 0F 29 58 88 48 8B 05 ?? ?? ?? ?? 48 33 "
                                 "C4"),
                -0x0B),
            Candidate::direct(
                "LookRay_P3_XmmCanaryBody",
                Pattern::literal("0F 29 70 C8 0F 29 78 B8 44 0F 29 40 A8 44 0F 29 50 98 44 0F 29 58 88 48 8B 05 ?? "
                                 "?? ?? ?? 48 33 C4 48 89 85 ?? ?? ?? ?? 4C 8B F9"),
                -0x21),
        };

        // On-screen reticle projection gate entry
        // Direct entry hook. P1 is the prologue through `mov rcx, [rip+cam]`. P2
        // extends through the first two movss reads of the candidate world point.
        // P3 drops the leading shadow-save and walks back 0x0B.
        inline const Candidate k_interactionOnScreenCandidates[] = {
            Candidate::direct(
                "OnScreen_P1_PrologueMovGlobal",
                Pattern::literal("4C 8B DC 49 89 5B 10 49 89 73 18 49 89 4B 08 57 48 83 EC ?? 48 8B 0D ?? ?? ?? ?? "
                                 "49 8D 70 04")),
            Candidate::direct(
                "OnScreen_P2_PrologueThroughMovss",
                Pattern::literal("4C 8B DC 49 89 5B 10 49 89 73 18 49 89 4B 08 57 48 83 EC ?? 48 8B 0D ?? ?? ?? ?? "
                                 "49 8D 70 04 F3 0F 10 5A 08 49 8B F8 F3 0F 10 52 04")),
            Candidate::direct(
                "OnScreen_P3_BodyMovGlobalMovss",
                Pattern::literal("49 89 4B 08 57 48 83 EC ?? 48 8B 0D ?? ?? ?? ?? 49 8D 70 04 F3 0F 10 5A 08 49 8B "
                                 "F8 F3 0F 10 52 04 4D 8D 43 08 F3 0F 10 0A"),
                -0x0B),
        };

        // HideOverlays entry
        // Direct entry hook. HideOverlays and ShowOverlays share the prologue, so
        // every candidate keeps the `mov byte [rax+rcx+0B8h], 1` set-flag store
        // (C6 84 ?? ?? ?? ?? ?? 01) that distinguishes Hide from Show's cmp. P3
        // drops the prologue and walks back 0x0A.
        inline const Candidate k_overlayHideCandidates[] = {
            Candidate::direct(
                "OverlayHide_P1_PrologueSetFlag",
                Pattern::literal("44 88 44 24 18 53 48 83 EC ?? 0F B6 C2 48 8B D9 48 8D 15 ?? ?? ?? ?? C6 84 ?? ?? "
                                 "?? ?? ?? 01")),
            Candidate::direct(
                "OverlayHide_P2_SetFlagThroughCall",
                Pattern::literal("44 88 44 24 18 53 48 83 EC ?? 0F B6 C2 48 8B D9 48 8D 15 ?? ?? ?? ?? C6 84 ?? ?? "
                                 "?? ?? ?? 01 48 8D 4C 24 ?? E8")),
            Candidate::direct(
                "OverlayHide_P3_BodySetFlag",
                Pattern::literal("0F B6 C2 48 8B D9 48 8D 15 ?? ?? ?? ?? C6 84 ?? ?? ?? ?? ?? 01 48 8D 4C 24 ?? E8"),
                -0x0A),
        };

        // ShowOverlays entry
        // Direct entry hook. The `cmp byte [rax+rcx+0B8h], 0` test-flag read
        // (80 BC ?? ?? ?? ?? ?? 00) is the discriminator versus Hide's store. P1
        // deliberately stops before the following jz rel8 (the encoding can flip).
        // P2 wildcards that jz as ?? ?? and extends into the flag-clear store and
        // call. P3 drops the prologue and walks back 0x0A.
        inline const Candidate k_overlayShowCandidates[] = {
            Candidate::direct(
                "OverlayShow_P1_PrologueTestFlag",
                Pattern::literal("44 88 44 24 18 53 48 83 EC ?? 0F B6 C2 48 8B D9 80 BC ?? ?? ?? ?? ?? 00")),
            Candidate::direct(
                "OverlayShow_P2_TestThroughClear",
                Pattern::literal("44 88 44 24 18 53 48 83 EC ?? 0F B6 C2 48 8B D9 80 BC ?? ?? ?? ?? ?? 00 ?? ?? C6 "
                                 "84 ?? ?? ?? ?? ?? 00 E8")),
            Candidate::direct(
                "OverlayShow_P3_BodyTestClear",
                Pattern::literal("0F B6 C2 48 8B D9 80 BC ?? ?? ?? ?? ?? 00 ?? ?? C6 84 ?? ?? ?? ?? ?? 00 E8 ?? ?? "
                                 "?? ?? 84 C0"),
                -0x0A),
        };

        // UI menu-open entry (vftable[1])
        // Direct entry hook. P1 anchors directly on the entry prologue through
        // the `cmp byte [rsi+670h], 0` field test. P2 is the mid-body anchor on the
        // vtable call that precedes lea "SetInputId" and walks back 0x36 to the
        // entry. P3 anchors on the field-test branch pair and walks back 0x1C. Jcc
        // rel32 displacements are wildcarded.
        inline const Candidate k_menuOpenCandidates[] = {
            Candidate::direct(
                "MenuOpen_P1_EntryFieldTest",
                Pattern::literal("48 89 5C 24 10 48 89 74 24 18 55 57 41 56 48 8B EC 48 83 EC 50 48 8D 71 A8 44 8A "
                                 "F2 80 BE 70 06 00 00 00")),
            Candidate::direct(
                "MenuOpen_P2_VtableCallSetInput",
                Pattern::literal("48 8B 41 B0 48 8B 48 30 48 8B 01 FF 10 48 8D 15 ?? ?? ?? ??"),
                -0x36),
            Candidate::direct(
                "MenuOpen_P3_FieldTestBranch",
                Pattern::literal("80 BE 70 06 00 00 00 48 8B F9 0F 84 ?? ?? ?? ?? 80 79 48 00 0F 85"),
                -0x1C),
        };

        // UI menu-close entry (vftable[2])
        // Direct entry hook. The object pointer (this - 0x58) lives in a
        // compiler-allocated register that differs across build configs, which
        // also shifts the prologue length: the Steam build uses rsi and saves it
        // with an extra `mov [rsp+20h], rsi` (5 bytes) before `push rdi`, while
        // the GOG build uses rdi and saves only `push rdi`. That changes both the
        // prologue LENGTH and the ModRM bytes of `lea r,[rcx-58h]` /
        // `cmp byte [r+0A0h], 0`, so a single entry pattern cannot span both, and
        // a single mid-body walk-back distance is build-specific too (the Steam
        // body is 0x18E entry->store while GOG is 0x15F, so a fixed -0x18E walk-back
        // would land 0x2F before the GOG entry, inside the preceding function). P1 is
        // the Steam rsi-form entry; P2 is the GOG/alt-register entry (save-one-reg
        // form) with the lea/cmp register ModRM wildcarded so it tolerates any
        // object register. Both are disp 0 (entry-anchored, build-robust), so a
        // GOG match wins before the mis-calibrated walk-backs are reached. P3/P4
        // are last-resort mid-body anchors (used only if both entry forms miss):
        // P3 the deactivate store (`mov byte [r+49h], 0; call;
        // mov byte [r+48h], 0`, register ModRM wildcarded) walking back 0x18E, P4
        // the field-test branch walking back 0x0F.
        inline const Candidate k_menuCloseCandidates[] = {
            Candidate::direct(
                "MenuClose_P1_EntryFieldTestRsi",
                Pattern::literal("48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 30 48 8D 71 A8 48 8B D9 80 BE A0 00 00 "
                                 "00 00")),
            Candidate::direct(
                "MenuClose_P2_EntryFieldTestAltReg",
                Pattern::literal("48 89 5C 24 18 57 48 83 EC 30 48 8D ?? A8 48 8B D9 80 ?? A0 00 00 00 00")),
            Candidate::direct(
                "MenuClose_P3_DeactivateStore",
                Pattern::literal("8A ?? 48 48 8D ?? 28 C6 ?? 49 00 E8 ?? ?? ?? ?? C6 ?? 48 00"),
                -0x18E),
            Candidate::direct(
                "MenuClose_P4_FieldTestBranch",
                Pattern::literal("48 8D 71 A8 48 8B D9 80 BE A0 00 00 00 00 0F 84 ?? ?? ?? ?? E8"),
                -0x0F),
        };

        // Shooting-utils FireProjectile entry (actor, dir, pos, speed -> pooled projectile -> Launch)
        // Direct entry hook. Every player and NPC shot goes through it with the shooter in rdx. P1 is the prologue
        // through the actor field read and `mov dl, 0C3h`. P2 drops `mov rax, rsp` and walks back 3. P3 is the
        // xmm-save run through the first virtual call and the movzx of the stack bool, walking back 0x29.
        inline const Candidate k_fireProjectileCandidates[] = {
            Candidate::direct(
                "FireProjectile_P1_Prologue",
                Pattern::literal("48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 57 41 56 41 57 48 81 EC B0 00 00 00 "
                                 "0F 29 70 D8 48 8B D9 48 8B 8A 68 06 00 00 4C 8B F2 0F 29 78 C8 B2 C3")),
            Candidate::direct(
                "FireProjectile_P2_SavesThroughActorRead",
                Pattern::literal("48 89 58 08 48 89 68 10 48 89 70 18 57 41 56 41 57 48 81 EC ?? ?? 00 00 0F 29 70 "
                                 "D8 48 8B D9 48 8B 8A ?? ?? 00 00 4C 8B F2 0F 29 78 C8 B2 C3 44 0F 29 40 B8 49 8B "
                                 "E9"),
                -0x3),
            Candidate::direct(
                "FireProjectile_P3_BodyXmmSavesCall",
                Pattern::literal("4C 8B F2 0F 29 78 C8 B2 C3 44 0F 29 40 B8 49 8B E9 44 0F 29 48 A8 45 8B F8 48 8B "
                                 "01 FF 90 ?? ?? 00 00 0F B6 94 24"),
                -0x29),
        };

        // CProjectile::Launch entry (CArrow's Launch slot is a jmp to it)
        // Direct entry hook. P1 is the prologue through `mov r8d, 100h`. P2 drops `mov rax, rsp`, wildcards the
        // frame size and walks back 3. P3 anchors on the launched-flag update (`and eax, 0FFFDFBF5h; or eax, 40h`)
        // and the IGameObject call after it, walking back 0x6C.
        inline const Candidate k_projectileLaunchCandidates[] = {
            Candidate::direct(
                "ProjectileLaunch_P1_Prologue",
                Pattern::literal("48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 4C 89 70 20 55 48 8D 68 D8 48 81 EC "
                                 "20 01 00 00 0F 29 70 E8 48 8B D9 0F 29 78 D8 49 8B F8 44 0F 29 40 C8 48 8B F2 44 "
                                 "0F 29 48 B8 41 B8 00 01 00 00")),
            Candidate::direct(
                "ProjectileLaunch_P2_SavesThroughEventMask",
                Pattern::literal("48 89 58 08 48 89 70 10 48 89 78 18 4C 89 70 20 55 48 8D 68 ?? 48 81 EC ?? ?? 00 "
                                 "00 0F 29 70 E8 48 8B D9 0F 29 78 D8 49 8B F8 44 0F 29 40 C8 48 8B F2 44 0F 29 48 "
                                 "B8 41 B8 00 01 00 00 44 0F 29 50 A8 B2 01"),
                -0x3),
            Candidate::direct("ProjectileLaunch_P3_BodyLaunchedFlag",
                              Pattern::literal("8B 81 A0 00 00 00 25 F5 FB FD FF 83 C8 40 89 81 A0 00 00 00 48 8B "
                                               "49 28 48 8B 01 FF 90"),
                              -0x6C),
        };

        // CArrow collision handler entry (vtable slot 47, fed one queued EventPhysCollision at a time)
        // Direct entry hook. P1 is the prologue through the cookie and the pEntity[1] load. P2 drops the rbx save and
        // walks back 5. P3 is the owner and pierceability tests (with [2-6] gaps over the short branches), walking
        // back 0x30.
        inline const Candidate k_arrowCollisionCandidates[] = {
            Candidate::direct(
                "ArrowCollision_P1_Prologue",
                Pattern::literal("48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 40 FF FF FF 48 81 EC "
                                 "C0 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 B0 00 00 00 4C 8B FA 33 F6 48 "
                                 "8B 52 18 4C 8B F1 48 85 D2")),
            Candidate::direct(
                "ArrowCollision_P2_PushesThroughEventRead",
                Pattern::literal("55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ?? FF FF FF 48 81 EC ?? ?? 00 00 48 "
                                 "8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 ?? ?? 00 00 4C 8B FA 33 F6 48 8B 52 18 4C 8B "
                                 "F1"),
                -0x5),
            Candidate::direct(
                "ArrowCollision_P3_BodyOwnerPierceTests",
                Pattern::literal("4C 8B FA 33 F6 48 8B 52 18 4C 8B F1 48 85 D2 [2-6] 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF "
                                 "90 ?? ?? 00 00 [2-6] 48 8B C6 48 8B D0 49 8B CE E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? "
                                 "?? ?? 41 0F BF 47 7E"),
                -0x30),
        };

        // IEntity::GetPhysics vtable slot, read from two independent call sites: Launch (`mov rcx, [rbx+38h]`, the
        // projectile entity, then `call [rax+disp32]` and the null test) and the fire routine's launch-point ray
        // (the shooter entity, then the skip-list store and the actor call after it). Both must decode the same slot.
        inline const Candidate k_getPhysicsSlotLaunchCandidates[] = {
            Candidate::direct("GetPhysicsSlotLaunch_P1_CallNullTest",
                              Pattern::literal("48 8B 4B 38 48 8B 01 | FF 90 ?? ?? 00 00 48 85 C0 [2-6] B9 00 00 00 "
                                               "80")),
        };
        inline const Candidate k_getPhysicsSlotFireRayCandidates[] = {
            Candidate::direct("GetPhysicsSlotFireRay_P1_CallSkipStore",
                              Pattern::literal("48 8B 4B 38 48 8B 01 | FF 90 ?? ?? 00 00 48 89 45 ?? 48 8B CB 48 8B "
                                               "03 FF 90 ?? ?? 00 00 48 8B 0D")),
        };

        // Aux geometry, called through the renderer and CAuxGeomCB vtables. Each slot read is validated against
        // the function these resolve before the call. GetIRenderAuxGeom and DrawLines are full functions;
        // SetRenderFlags is a leaf without unwind data, so it is matched as a code site.
        inline const Candidate k_getAuxGeomCandidates[] = {
            Candidate::direct("GetAuxGeom_P1_BodyThreadCompare",
                              Pattern::literal("48 8B D9 FF 15 ?? ?? ?? ?? 3B 87 ?? ?? ?? ?? [2-6] 3B 87 ?? ?? ?? ?? "
                                               "[2-6] 48 8B 8B ?? ?? ?? ??"),
                              -0x11),
            Candidate::direct("GetAuxGeom_P2_BodyLockedBuffer",
                              Pattern::literal("48 8B 8B 88 2F 00 00 E8 ?? ?? ?? ?? 48 8B 5C 24 30 48 83 C4 20 5F C3"),
                              -0x2A),
            Candidate::direct("GetAuxGeom_P3_BodyCompareCall",
                              Pattern::literal("3B 87 ?? ?? ?? ?? [2-6] 48 8B 8B ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 5C "
                                               "24 30"),
                              -0x22),
        };
        inline const Candidate k_auxSetFlagsCandidates[] = {
            Candidate::direct("AuxSetFlags_P1_BodyStoreReturn",
                              Pattern::literal("89 02 41 8B 00 41 89 81 ?? ?? ?? ?? 48 8B C2 C3"), -0xB),
            Candidate::direct("AuxSetFlags_P2_Entry",
                              Pattern::literal("4C 8B 49 ?? 41 8B 81 ?? ?? ?? ?? 89 02 41 8B 00")),
            Candidate::direct("AuxSetFlags_P3_StoreReturn",
                              Pattern::literal("41 89 81 ?? ?? ?? ?? 48 8B C2 C3"), -0x10),
        };
        inline const Candidate k_auxDrawLinesCandidates[] = {
            Candidate::direct("AuxDrawLines_P1_BodyThicknessTest",
                              Pattern::literal("48 8B FA 0F 2F C6 41 8B F0 4C 8B F1 [2-6] 41 8A 41 ??"), -0x34),
            Candidate::direct("AuxDrawLines_P2_BodyConstLoad",
                              Pattern::literal("F3 0F 10 05 ?? ?? ?? ?? 33 DB 0F 29 74 24 40 49 8B E9"), -0x19),
            Candidate::direct("AuxDrawLines_P3_AlphaFlags",
                              Pattern::literal("FE C8 49 89 5B D8 3C ?? 4D 8D 4B 28 49 8B 46 ??"), -0x4F),
        };
    } // namespace Aob

    /**
     * @brief Stable identity for every game-image anchor the mod resolves at startup.
     * @details Indexes the declarative anchor table that resolve_all_anchors() resolves once, and the stores the gated
     *          accessors read. The enumerator order IS the table order. Count is the element count and is not a valid
     *          anchor. The ids up to PhysEntMovement are addresses. The ids from IsThirdPersonSlot to AnimIdByCrcSlot
     *          are scalars decoded from game code, not addresses. AnimNameHashCall is a call site, and its consumer
     *          decodes the callee. The archery ids are addresses, except GetPhysicsSlot, a scalar.
     */
    enum class AnchorId : std::size_t
    {
        Context,              // global-context storage slot (camera-manager root; 2-of-3 quorum)
        Genv,                 // SSystemGlobalEnvironment base (2-of-5 quorum)
        Frustum,              // camera frustum builder (mandatory hook target)
        HeadVisibility,       // head-visibility setter
        InputDispatch,        // generic input-event dispatcher
        ActionDispatch,       // global action dispatcher (Lua Player:OnAction source)
        RayWorldIntersection, // IPhysicalWorld::RayWorldIntersection helper (called, not hooked)
        InteractionRayBuild,  // interaction ray-query builder
        InteractorLookRay,    // interactor look-ray builder (used as a caller-range bound, not hooked)
        InteractionOnScreen,  // on-screen reticle projection gate
        OverlayHide,          // HideOverlays
        OverlayShow,          // ShowOverlays
        MenuOpen,             // UI menu-open entry
        MenuClose,            // UI menu-close entry
        GetObjectsInBox,      // I3DEngine::GetObjectsInBox render-octree query (called, not hooked)
        TurnTriggerReturn,    // IsThirdPerson return address in the turn trigger (compared, not hooked)
        LockSyncReturn,       // IsThirdPerson return address in the idle LockBodyTurn sync (compared, not hooked)
        PhysEntMovement,      // CAnimatedCharacter::UpdatePhysicalEntityMovement (turn steps kept in place)
        IsThirdPersonSlot,    // C_Player IsThirdPerson vtable byte offset (quorum of its two call sites)
        HandleEventSlot,      // C_Player HandleEvent vtable byte offset (the camera-changed event's sender)
        CameraEventId,        // camera-changed SGameObjectEvent id (2-of-3 quorum: sender, HandleEvent, OnEvent)
        CameraEventFlags,     // camera-changed SGameObjectEvent target/flags word (the sender)
        TurnInstalledState,   // C_PlayerMovementAction installed-turn state offset (quorum: trigger, OnEvent)
        LockBodyTurnCount,    // C_Player LockBodyTurn reference-count offset (LockBodyTurn itself; log only)
        AnimIdByCrcSlot,      // CAnimationSet GetAnimIDByCRC vtable byte offset (CActionScope::InstallAnimation)
        AnimNameHashCall,     // the call to the animation-name hash in CAnimationSet::GetAnimIDByName
        FireProjectile,       // shooting-utils FireProjectile (the shooter filter for a player shot)
        ProjectileLaunch,     // CProjectile::Launch (the arrow's direction and velocity are rewritten here)
        ArrowCollision,       // CArrow collision handler (the impact point of a tracked shot)
        GetPhysicsSlot,       // IEntity::GetPhysics vtable byte offset (quorum of Launch and the fire ray)
        GetAuxGeom,           // renderer GetIRenderAuxGeom (vtable-slot validator)
        AuxSetFlags,          // CAuxGeomCB::SetRenderFlags (vtable-slot validator)
        AuxDrawLines,         // CAuxGeomCB::DrawLines (vtable-slot validator)
        Count,
    };

    /**
     * @brief A mod feature, enabled only through the gate over exactly the anchors it depends on.
     * @details resolve_all_anchors() evaluates each gate with DMK::anchor::evaluate_gate under the default, fail-closed
     *          policy (`[B-51]`): one failed anchor turns the whole feature off, so a feature never runs on a partial
     *          set. The enumerator order IS the gate table order. Count is the element count and is not a feature.
     */
    enum class Feature : std::size_t
    {
        Camera,              // Frustum: the third-person camera itself
        GameState,           // Context: the camera-manager reads behind menu, combat and mount detection
        Engine,              // Genv: the engine interfaces (aim convergence, player look, collision, occlusion)
        HeadVisibility,      // HeadVisibility: the player head from behind
        Orbit,               // InputDispatch: free-look orbit
        MoveIntent,          // ActionDispatch: device-agnostic movement intent for orbit move detection
        Collision,           // Genv, RayWorldIntersection: camera collision
        Interaction,         // InteractionRayBuild, InteractorLookRay: camera-space interaction
        InteractionOnScreen, // InteractionOnScreen: crosshair-picked shrines, beds and doors
        OverlayState,        // OverlayHide, OverlayShow: offset suppression under game UI
        MenuState,           // MenuOpen, MenuClose: offset suppression under the in-game menu
        Occlusion,           // Genv, GetObjectsInBox: render-only roof and canopy occlusion
        NativeTurn,          // the IsThirdPerson call sites and slot, and the camera-changed event: native turning
        TurnDecision,        // TurnInstalledState, with the function-scoped proofs: turns that finish on the look
        TurnSteps,           // PhysEntMovement: turn steps kept in place
        CrouchedAnimations,  // AnimIdByCrcSlot, AnimNameHashCall: the NPC crouched idle and turns for the player
        ArcheryAim,          // FireProjectile, ProjectileLaunch, GetPhysicsSlot: arrows aimed at the crosshair point
        ArcheryTrail,        // ArrowCollision and the aux-geometry calls: the trail and aim preview (needs ArcheryAim)
        Count,
    };

    /** @brief Where the turn hooks go and the C_PlayerMovementAction fields the decision hook reads. */
    struct TurnDecisionLayout
    {
        /// The `seta cl` of the turn decision. The mid hook sets the flags it reads.
        std::uintptr_t site = 0;
        /// The spin latch (uint8).
        std::ptrdiff_t spin_latch_offset = 0;
        /// The installed-turn state (int32, where 1 and 2 mean installed).
        std::ptrdiff_t installed_state_offset = 0;
        /// The previous evaluation's gap sign (float, 0 for none).
        std::ptrdiff_t last_sign_offset = 0;
    };

    /**
     * @brief Resolves every game-image anchor, applies the signature file's repairs, and evaluates the feature gates.
     * @details Builds the declarative DMK::anchor table over the cascade candidate arrays above and resolves it with
     *          anchor::resolve_all_parallel, confined to the WHGame.dll image [module_base, module_base +
     *          module_size). The explicit range is required: the DMK default host_module_range() is the host EXE,
     *          not WHGame.dll. Every row carries a role validator (require_validator): a function entry must lie on an
     *          executable page, open like a function and agree with its .pdata start, an instruction site must lie
     *          on an executable page, and a global must lie on a readable page that is not code. A
     *          KCD2_TPVCamera.signatures.ini beside the ASI can then replace a byte-signature or code-operand row by
     *          label (DMK::manifest::overlay). A repair counts only after it passes the manifest trust gate, and a
     *          repaired hook target needs the full mutation baselines. A per-anchor status line, the gate verdicts,
     *          and an assess_quality() summary are logged.
     * @note Setup/control-plane only: allocates and spawns a transient worker pool. Call once at init.
     */
    void resolve_all_anchors(std::uintptr_t module_base, std::size_t module_size);

    /**
     * @brief Writes every built-in signature, with the baselines captured from the live image, to
     *        KCD2_TPVCamera.signatures.captured.ini beside the ASI.
     * @details The file is the editable form of the built-in contract (`[B-54]`). After a game update, a section copied
     *          into KCD2_TPVCamera.signatures.ini and given a new pattern repairs that signature without a rebuild.
     *          Quorum anchors have no file form and are not exported.
     * @note Setup/control-plane only. Call after resolve_all_anchors().
     */
    void export_signatures();

    /**
     * @brief True when @p feature's gate passed, so every anchor it depends on resolved.
     * @note Valid only after resolve_all_anchors() has run. It returns false before then.
     */
    [[nodiscard]] bool feature_ready(Feature feature) noexcept;

    /**
     * @brief Returns @p id's resolved address when @p feature's gate passed, else 0.
     * @details @p id must be one of the anchors @p feature depends on.
     */
    [[nodiscard]] std::uintptr_t gated_anchor_address(Feature feature, AnchorId id) noexcept;

    /**
     * @brief Returns @p id's resolved value when @p feature's gate passed, else std::nullopt.
     * @details For a scalar anchor, the value decoded from game code and accepted by its plausibility check.
     */
    [[nodiscard]] std::optional<std::int64_t> gated_anchor_value(Feature feature, AnchorId id) noexcept;

    /**
     * @brief Returns @p id's resolved value with no feature gate, or std::nullopt.
     * @details Only for a value no feature depends on (a diagnostic read). A feature reads through its gate.
     */
    [[nodiscard]] std::optional<std::int64_t> anchor_value(AnchorId id) noexcept;

    /**
     * @brief Finds and proves the turn hook sites and the action fields the decision hook reads, around
     *        @p trigger_return.
     * @details Requires the Feature::TurnDecision gate. Everything resolves relative to the turn-trigger return
     *          address and inside the function that holds it (its .pdata range), never at a fixed distance:
     *          - the register contract window ends on the return address (k_turnContractCandidates).
     *          - the decision opens exactly at the return address and gives the hook site
     *            (k_turnDecisionSiteCandidates).
     *          - `mov r12, rdx` lies in the prologue and the output store in the same fragment as the trigger.
     *          - the spin latch and last sign are 2-of-2 quorums inside that fragment, and the installed state is
     *            the TurnInstalledState anchor.
     *          Each scoped anchor is appended to anchor_report(). Logs the first proof that fails.
     * @return The layout, or std::nullopt when any proof fails.
     * @note Setup/control-plane only. Call on the init thread after resolve_all_anchors().
     */
    [[nodiscard]] std::optional<TurnDecisionLayout> resolve_turn_decision_layout(std::uintptr_t trigger_return);

    /**
     * @brief Reads CAnimatedCharacter's movement request type offset from UpdatePhysicalEntityMovement.
     * @param update_physical_entity_movement The function's entry (AnchorId::PhysEntMovement).
     * @return The member offset, or std::nullopt when it does not resolve inside the function's .pdata range.
     * @note Setup/control-plane only. Call on the init thread after resolve_all_anchors().
     */
    [[nodiscard]] std::optional<std::ptrdiff_t>
    resolve_movement_type_offset(std::uintptr_t update_physical_entity_movement);

    /**
     * @brief True when the function at @p handle_event dispatches the camera-changed event @p event_id.
     * @details Searches the function's whole .pdata range, which must begin at @p handle_event, for the shape the
     *          k_cameraEventHandleEventIdCandidates vote reads the id from, with @p event_id filled in. Finding it in
     *          the HandleEvent slot's target ties that slot to the function the vote read the id from.
     * @note Setup/control-plane only: compiles the pattern at runtime.
     */
    [[nodiscard]] bool dispatches_camera_event(std::uintptr_t handle_event, std::uint8_t event_id);

    /**
     * @brief Returns the retained per-anchor resolution report (the startup table, then the function-scoped anchors
     *        the resolve_* helpers above add).
     * @details The same span resolve_all_anchors() logged its quality summary from, kept so
     *          diagnostics::collect() can roll it into the mod's health snapshot rather than the mod
     *          re-deriving the counts. Empty before resolve_all_anchors() has run.
     * @note The entries live in static storage for the process lifetime; the span never dangles.
     */
    [[nodiscard]] std::span<const DMK::anchor::ResolvedAnchor> anchor_report() noexcept;
} // namespace TPVCamera

#endif // TPVCAMERA_AOB_RESOLVER_HPP
