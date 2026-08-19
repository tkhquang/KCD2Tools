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
 * the whole table in one parallel pass at startup, and anchor_address() returns
 * each resolved address, or 0 on a cascade miss, to the call sites.
 *
 * Candidate order is most-specific first (P1), so a tight anchor wins before a
 * looser fallback. Each cascade carries at least one candidate anchored PAST the
 * 5-byte function prologue, through a negative walk-back that returns to the entry.
 * That mid-body anchor still matches when a sibling mod has inline-hooked the
 * entry, because the overwritten prologue makes the earlier candidates miss and the
 * scan falls through to it.
 *
 * Every ladder resolves under CandidateOrder::UniqueFirst, so a pattern that
 * matches more than once inside the image is skipped as ambiguous and a freak
 * collision falls through to the next candidate rather than resolving blindly. A
 * full cascade miss is a clean failure (0), never a guess at an unrelated near-JMP
 * site.
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
 * Every candidate below was verified to return exactly one match against the
 * live retail WHGame.dll; the per-cascade comments and the RVAs are recorded in
 * docs/analysis/aob_cascade_resolution.md.
 */
#ifndef TPVCAMERA_AOB_RESOLVER_HPP
#define TPVCAMERA_AOB_RESOLVER_HPP

#include <DetourModKit.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace TPVCamera
{

    namespace Aob
    {
        using DMK::scan::Candidate;
        using DMK::scan::Pattern;

        // Global-context storage slot (qword_18549B4B0)
        // RipRelative: all three candidates resolve the SAME data slot the camera-manager walk reads (context +
        // OFFSET_MANAGER_PTR_STORAGE), across three different functions that load it. None anchors on a short Jcc
        // (rel8 opcodes 70-7F/EB/E3 flip to rel32 across builds and desync the pattern); each pins instead on a
        // distinctive NON-Jcc neighbour of the slot-load `mov rax,[rip+slot]` (48 8B 05, disp32 -> the slot):
        // P1 (sub_180682A08) leads with `mov eax,[rbx+r14]; cmp cs:dword,eax; jg-far` (the SIB 0x33 base/index
        // pins this site over a near-twin that shares the field test) and ends on the `cmp qword [rax+0E0h]`
        // field; P2 (sub_180B284B8) leads with `cmp cs:dword,ebx` and ends on `mov rbp,[rax+0F8h]`; P3
        // (sub_180F1B788) is pinned by the `mov rcx,[rdi+278h]` neighbour and `mov rsi,[rax+0F8h]`. Three
        // independent code sites mean a patch must move all three before the slot is lost. All resolve to
        // 0x18549B4B0.
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

        // SSystemGlobalEnvironment base (g_env, 0x18492B800)
        // RipRelative: all three resolve the g_env base. P1/P3 anchor on the same
        // `GetIGameFramework` call site (sub_181DCAF60): P1 leads with the
        // `mov r8, rdi` that precedes the lea, P3 drops it and anchors on the lea
        // plus the trailing virtual-call chain. P2 is a genuinely different site
        // (a struct-init sequence storing the g_env pointer into a member),
        // giving two independent reference sites. A static-RVA fallback in
        // camera_hook covers a total miss, so a third fully-distinct anchor is
        // not required here.
        inline const Candidate k_genvCandidates[] = {
            Candidate::rip_relative(
                "Genv_P1_LeaR8FrameworkChain",
                Pattern::literal("4C 8B C7 | 48 8D 15 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8B "
                                 "D3 48 8B 01 FF 50 18"),
                3, 7),
            Candidate::rip_relative(
                "Genv_P2_LeaStructInit",
                Pattern::literal("48 8D 05 ?? ?? ?? ?? 48 89 0F 4C 8D 67 28 48 8D 0D ?? ?? ?? ?? 48 89 47 20 48 89 "
                                 "4F 08"),
                3, 7),
            Candidate::rip_relative(
                "Genv_P3_LeaFrameworkChainTail",
                Pattern::literal("48 8D 15 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8B D3 48 8B "
                                 "01 FF 50 18"),
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

        // Generic input-event dispatcher entry (0x180862BD8)
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
    } // namespace Aob

    /**
     * @brief Stable identity for every game-image anchor the mod resolves at startup.
     * @details Indexes both the declarative anchor table resolved once by resolve_all_anchors() and the
     *          address store read by anchor_address(). The enumerator order IS the table order; Count is
     *          the element count and is not a valid anchor.
     */
    enum class AnchorId : std::size_t
    {
        Context,              // global-context storage slot (camera-manager root)
        Genv,                 // SSystemGlobalEnvironment base
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
        Count,
    };

    /**
     * @brief Resolves every game-image anchor in one parallel pass and records the results.
     * @details Builds the declarative DMK::anchor table over the cascade candidate arrays above (each a
     *          RipGlobal anchor whose candidates already select Direct vs RIP-relative resolution) and
     *          resolves it with anchor::resolve_all_parallel, confined to the WHGame.dll image
     *          [module_base, module_base + module_size). The explicit range is required: the DMK default
     *          host_module_range() is the host EXE, not WHGame.dll. Each resolved address is stored for
     *          anchor_address(); a per-anchor status line plus an assess_quality() summary are logged. A
     *          best-effort anchor that misses records 0 and its consumer degrades; the mandatory anchors
     *          (Context, Frustum) are reported so the caller can fail init when anchor_address() is 0.
     * @note Setup/control-plane only: allocates and spawns a transient worker pool. Call once at init.
     */
    void resolve_all_anchors(std::uintptr_t module_base, std::size_t module_size);

    /**
     * @brief Returns the resolved absolute address for an anchor, or 0 if it did not resolve.
     * @note Valid only after resolve_all_anchors() has run; returns 0 before then or on a cascade miss.
     */
    [[nodiscard]] std::uintptr_t anchor_address(AnchorId id) noexcept;

    /**
     * @brief Returns the retained per-anchor resolution report.
     * @details The same span resolve_all_anchors() logged its quality summary from, kept so
     *          diagnostics::collect() can roll it into the mod's health snapshot rather than the mod
     *          re-deriving the counts. Empty before resolve_all_anchors() has run.
     * @note The entries live in static storage for the process lifetime; the span never dangles.
     */
    [[nodiscard]] std::span<const DMK::anchor::ResolvedAnchor> anchor_report() noexcept;
} // namespace TPVCamera

#endif // TPVCAMERA_AOB_RESOLVER_HPP
