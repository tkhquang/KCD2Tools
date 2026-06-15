/**
 * @file aob_resolver.hpp
 * @brief Cascading AOB candidate tables and the declarative anchor registry for the mod.
 *
 * Every memory location the mod hooks or reads is located by a cascade of
 * ordered AOB candidates rather than a single signature, so a game patch that
 * shifts code only has to leave ONE of three anchors intact for the feature to
 * keep working. Resolution is delegated to DetourModKit's module-scoped cascade
 * scanner (DMK::Scanner::resolve_cascade_in_module, DMK 3.5.0+): the search is
 * confined to the WHGame.dll image the caller passes as module_base/module_size,
 * NOT the whole process. This is deliberate. WHGame.dll is a normal unpacked PE
 * with every target inside it, so a whole-process scan (resolve_cascade /
 * scan_executable_regions) buys nothing and is actively unsafe -- a generic
 * prologue/epilogue candidate can false-match inside another injected module (a
 * graphics overlay, a sibling mod) and, because the cascade is first-match-wins,
 * shadow the correct in-module match, disabling the feature or hooking an
 * unrelated site. The cascade tables are wrapped one-to-one as RipGlobal entries
 * in a declarative DMK::Anchors registry (AnchorId below); resolve_all_anchors()
 * resolves the whole table in one parallel pass at startup and anchor_address()
 * returns each resolved address (or 0 on a cascade miss) to the call sites.
 *
 * Candidate ordering is most-specific first (P1), so a tight anchor wins before
 * a looser fallback. Each cascade carries at least one candidate anchored PAST
 * the 5-byte function prologue (a negative disp_offset that walks back to the
 * entry); that mid-body anchor still matches when a sibling mod has inline-hooked
 * the entry, because the overwritten prologue makes the earlier candidates miss
 * and the scan falls through to it. Every candidate keeps the DMK default
 * require_unique=true: a pattern matching more than once inside the image is
 * skipped as ambiguous (every candidate below is verified to match exactly once
 * over the full WHGame.dll image), so a freak collision falls through to the next
 * candidate rather than resolving blindly. A full cascade miss is a clean failure
 * (0), never a guess at an unrelated near-JMP site.
 *
 * Resolution shapes (DMK::Scanner::ResolveMode):
 *   - Direct      address = match + disp_offset. Entry-hook targets resolve to the
 *                 function entry (disp_offset 0); mid-body anchors use a negative
 *                 disp_offset equal to the entry->anchor byte distance.
 *   - RipRelative address = match + instr_end_offset + int32(match + disp_offset).
 *                 Resolves a lea/mov [rip+disp32] to the data slot it references
 *                 (the global-context storage slot and the g_env base).
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

namespace TPVCamera
{
    using AddrCandidate = DMK::Scanner::AddrCandidate;
    using ResolveMode = DMK::Scanner::ResolveMode;

    namespace Aob
    {
        // --- Global-context storage slot (qword_18549B4B0) -------------------
        // RipRelative: all three candidates resolve the SAME data slot the
        // camera-manager walk reads (context + OFFSET_MANAGER_PTR_STORAGE). P1
        // anchors in the TLS-guarded accessor (sub_18091C138) on the `jg` that
        // skips the fast-path epilogue; the mov it precedes loads the slot. The
        // accessor's body is a generic magic-static guard shared by dozens of
        // sibling accessors, so P2/P3 instead anchor on two OTHER functions that
        // load the slot, each isolated by a distinctive neighbouring field test
        // (cmp qword [rax+0E0h] / mov [rax+0F8h]). Three independent code sites
        // means a patch must move all three before the slot is lost.
        inline constexpr AddrCandidate k_contextCandidates[] = {
            {"Context_P1_GuardJgFastPathMov", "7F ?? 48 8B 05 ?? ?? ?? ?? 48 83 C4 ?? 5B C3", ResolveMode::RipRelative,
             5, 9},
            {"Context_P2_LoadCheckFieldE0", "48 8B 05 ?? ?? ?? ?? 48 83 B8 E0 00 00 00 00 74 ?? 42 8B 04 33 39 05",
             ResolveMode::RipRelative, 3, 7},
            {"Context_P3_LoadFieldF8Jnz", "48 8B 05 ?? ?? ?? ?? 48 8B A8 F8 00 00 00 75 ?? E8",
             ResolveMode::RipRelative, 3, 7},
        };

        // --- SSystemGlobalEnvironment base (g_env, 0x18492B800) --------------
        // RipRelative: all three resolve the g_env base. P1/P3 anchor on the same
        // `GetIGameFramework` call site (sub_181DCAF60): P1 leads with the
        // `mov r8, rdi` that precedes the lea, P3 drops it and anchors on the lea
        // plus the trailing virtual-call chain. P2 is a genuinely different site
        // (a struct-init sequence storing the g_env pointer into a member),
        // giving two independent reference sites. A static-RVA fallback in
        // camera_hook covers a total miss, so a third fully-distinct anchor is
        // not required here.
        inline constexpr AddrCandidate k_genvCandidates[] = {
            {"Genv_P1_LeaR8FrameworkChain",
             "4C 8B C7 48 8D 15 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8B D3 48 8B 01 FF 50 18",
             ResolveMode::RipRelative, 6, 10},
            {"Genv_P2_LeaStructInit",
             "48 8D 05 ?? ?? ?? ?? 48 89 0F 4C 8D 67 28 48 8D 0D ?? ?? ?? ?? 48 89 47 20 48 89 4F 08",
             ResolveMode::RipRelative, 3, 7},
            {"Genv_P3_LeaFrameworkChainTail",
             "48 8D 15 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8B D3 48 8B 01 FF 50 18",
             ResolveMode::RipRelative, 3, 7},
        };

        // --- Camera frustum builder (CCamera::UpdateFrustumPlanes) entry -----
        // Direct entry hook. P1 is the full prologue (mov rax,rsp + 9 pushes +
        // lea + sub) into the first matrix read. P2 drops the `mov rax,rsp` lead
        // and walks back 3 bytes. P3 anchors purely on the body's distinctive
        // matrix-read run (the movss [rcx+disp] chain that reads the 3x4 camera
        // matrix) and walks back 0x1A. The lea displacement and sub-rsp immediate
        // are wildcarded for frame-size resilience.
        inline constexpr AddrCandidate k_frustumCandidates[] = {
            {"Frustum_P1_PrologueMatrixRead",
             "48 8B C4 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 68 ?? 48 81 EC ?? ?? 00 00 F3 0F 10 09 48 8B D9",
             ResolveMode::Direct, 0, 0},
            {"Frustum_P2_PushChainMatrixRead",
             "55 53 56 57 41 54 41 55 41 56 41 57 48 8D 68 ?? 48 81 EC ?? ?? 00 00 F3 0F 10 09 48 8B D9 F3 0F 10 59 08",
             ResolveMode::Direct, -3, 0},
            {"Frustum_P3_MatrixReadBody",
             "F3 0F 10 09 48 8B D9 F3 0F 10 59 08 F3 0F 10 51 10 F3 0F 10 41 24 F3 0F 10 61 28", ResolveMode::Direct,
             -0x1A, 0},
        };

        // --- Head-visibility setter entry -----------------------------------
        // Direct entry hook (this, bool hide_head /*dl*/, char flags /*r8b*/).
        // P1 is the current prologue through `mov sil, r8b`. P2 extends through
        // the `mov dil,dl; mov rbx,rcx; call; test al,al` body for extra pinning.
        // P3 drops the two stack-save stores and walks back 0x0A from the push.
        inline constexpr AddrCandidate k_headVisibilityCandidates[] = {
            {"Head_P1_PrologueMovSil", "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC ?? 41 8A F0", ResolveMode::Direct, 0,
             0},
            {"Head_P2_PrologueMovSilCall",
             "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC ?? 41 8A F0 40 8A FA 48 8B D9 E8 ?? ?? ?? ?? 84 C0",
             ResolveMode::Direct, 0, 0},
            {"Head_P3_BodyMovSilDilRbx", "57 48 83 EC ?? 41 8A F0 40 8A FA 48 8B D9 E8 ?? ?? ?? ?? 84 C0 74",
             ResolveMode::Direct, -0x0A, 0},
        };

        // --- Generic input-event dispatcher entry ---------------------------
        // Direct entry hook. The `cmp [rcx+0D8h], r8b` guard is the unique
        // landmark; the jnz rel8 that skips it is wildcarded. P2 extends past the
        // first jz rel32 into the `cmp [rdx+10h], -1` pitch check. P3 drops the
        // two prologue stores and walks back 0x0A.
        inline constexpr AddrCandidate k_inputDispatchCandidates[] = {
            {"Input_P1_PrologueGuardCmp",
             "48 89 5C 24 08 57 48 83 EC ?? 48 8B DA 48 8B F9 45 84 C0 75 ?? 44 38 81 D8 00 00 00 0F 84",
             ResolveMode::Direct, 0, 0},
            {"Input_P2_GuardThroughPitchCmp",
             "48 89 5C 24 08 57 48 83 EC ?? 48 8B DA 48 8B F9 45 84 C0 75 ?? 44 38 81 D8 00 00 00 0F 84 "
             "?? ?? ?? ?? 83 7A 10 FF 0F 84",
             ResolveMode::Direct, 0, 0},
            {"Input_P3_BodyGuardCmpPitch",
             "48 8B DA 48 8B F9 45 84 C0 75 ?? 44 38 81 D8 00 00 00 0F 84 ?? ?? ?? ?? 83 7A 10 FF", ResolveMode::Direct,
             -0x0A, 0},
        };

        // --- Global action dispatcher entry (Lua Player:OnAction source) -----
        // Direct entry hook. The `movss [rax+20h], xmm3` that stores the float
        // value arg is the distinctive head. P2 extends past the sub-rsp into the
        // first movaps + `mov rdi,rcx`. P3 anchors on the movss-store body and
        // walks back 0x0B. Frame displacement and stack size are wildcarded.
        inline constexpr AddrCandidate k_actionDispatchCandidates[] = {
            {"Action_P1_PrologueMovssVal",
             "48 8B C4 48 89 58 10 48 89 70 18 F3 0F 11 58 20 55 57 41 56 48 8D 68 ?? 48 81 EC ?? ?? ?? ??",
             ResolveMode::Direct, 0, 0},
            {"Action_P2_PrologueThroughMovaps",
             "48 8B C4 48 89 58 10 48 89 70 18 F3 0F 11 58 20 55 57 41 56 48 8D 68 ?? 48 81 EC ?? ?? ?? ?? "
             "0F 29 70 ?? 48 8B F9",
             ResolveMode::Direct, 0, 0},
            {"Action_P3_MovssValBody",
             "F3 0F 11 58 20 55 57 41 56 48 8D 68 ?? 48 81 EC ?? ?? ?? ?? "
             "0F 29 70 ?? 48 8B F9 8B 41 18 41 BE 01 00 00 00",
             ResolveMode::Direct, -0x0B, 0},
        };

        // --- IPhysicalWorld::RayWorldIntersection helper entry ---------------
        // Direct: the match IS the callable function pointer (no detour). A
        // sibling helper (sub_1838D6E3C) shares the prologue and the first inner
        // call, then diverges: this helper stages the next call's count with
        // `mov r9d, imm32` (41 B9) where the sibling does `mov rcx, r9`. EVERY
        // candidate therefore extends to the 41 B9 discriminator so none can
        // resolve onto the sibling. P2 drops the `mov rax,rsp` lead (walk back 3);
        // P3 anchors on the arg-staging body (walk back 0x1F).
        inline constexpr AddrCandidate k_rayWorldIntersectionCandidates[] = {
            {"Ray_P1_PrologueThroughR9dImm",
             "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 4C 89 70 20 55 48 8D 68 ?? 48 81 EC ?? ?? 00 00 "
             "48 8B DA 49 8B F8 33 D2 4C 8B F1 48 8D 4D ?? 41 8B F1 44 8D 42 70 E8 ?? ?? ?? ?? 8B 43 08 "
             "48 8D 55 ?? F2 0F 10 03 41 B9 ?? ?? ?? ??",
             ResolveMode::Direct, 0, 0},
            {"Ray_P2_SavesThroughR9dImm",
             "48 89 58 08 48 89 70 10 48 89 78 18 4C 89 70 20 55 48 8D 68 ?? 48 81 EC ?? ?? 00 00 "
             "48 8B DA 49 8B F8 33 D2 4C 8B F1 48 8D 4D ?? 41 8B F1 44 8D 42 70 E8 ?? ?? ?? ?? 8B 43 08 "
             "48 8D 55 ?? F2 0F 10 03 41 B9",
             ResolveMode::Direct, -3, 0},
            {"Ray_P3_BodyThroughR9dImm",
             "48 8B DA 49 8B F8 33 D2 4C 8B F1 48 8D 4D ?? 41 8B F1 44 8D 42 70 E8 ?? ?? ?? ?? 8B 43 08 "
             "48 8D 55 ?? F2 0F 10 03 41 B9",
             ResolveMode::Direct, -0x1F, 0},
        };

        // --- Interaction ray-query builder entry ----------------------------
        // Direct entry hook. The function is a leaf-style Vec3 copier with no
        // standard prologue, so all anchors are body-shaped. P2 extends the
        // vec-copy run; P3 anchors on the distinctive `mov [rcx+228h], al` store
        // and walks back 0x1B.
        inline constexpr AddrCandidate k_interactionRayBuildCandidates[] = {
            {"RayBuild_P1_VecCopyHead",
             "F2 0F 10 02 4C 8B D1 4C 8B 5C 24 30 F2 0F 11 01 8B 42 08 89 41 08 F2 41 0F 10 00", ResolveMode::Direct, 0,
             0},
            {"RayBuild_P2_VecCopyExtended",
             "F2 0F 10 02 4C 8B D1 4C 8B 5C 24 30 F2 0F 11 01 8B 42 08 89 41 08 F2 41 0F 10 00 "
             "F2 0F 11 41 0C 41 8B 40 08 89 41 14",
             ResolveMode::Direct, 0, 0},
            {"RayBuild_P3_Store228Body",
             "F2 0F 11 41 0C 41 8B 40 08 89 41 14 8B 44 24 28 89 41 1C 8A 44 24 40 88 81 28 02 00 00",
             ResolveMode::Direct, -0x1B, 0},
        };

        // --- Interactor look-ray builder entry (used as a caller-range bound) -
        // Direct: resolves the function entry; the caller range [entry, entry +
        // INTERACTOR_LOOKRAY_SPAN) gates the ray-build detour. The xmm-save
        // prologue is shared by many functions, so P2/P3 extend past the saves
        // into the stack-canary load (`mov rax,[rip+cookie]; xor rax,rsp`) to
        // stay unique. P2 walks back 0x0B, P3 walks back 0x21.
        inline constexpr AddrCandidate k_interactorLookRayCandidates[] = {
            {"LookRay_P1_PrologueXmmSaves",
             "48 8B C4 48 89 58 10 48 89 70 18 55 57 41 54 41 56 41 57 48 8D A8 ?? ?? FF FF 48 81 EC ?? ?? 00 00 "
             "0F 29 70 C8 0F 29 78 B8 44 0F 29 40 A8 44 0F 29 50 98 44 0F 29 58 88",
             ResolveMode::Direct, 0, 0},
            {"LookRay_P2_PushXmmCanary",
             "55 57 41 54 41 56 41 57 48 8D A8 ?? ?? FF FF 48 81 EC ?? ?? 00 00 "
             "0F 29 70 C8 0F 29 78 B8 44 0F 29 40 A8 44 0F 29 50 98 44 0F 29 58 88 48 8B 05 ?? ?? ?? ?? 48 33 C4",
             ResolveMode::Direct, -0x0B, 0},
            {"LookRay_P3_XmmCanaryBody",
             "0F 29 70 C8 0F 29 78 B8 44 0F 29 40 A8 44 0F 29 50 98 44 0F 29 58 88 "
             "48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 ?? ?? ?? ?? 4C 8B F9",
             ResolveMode::Direct, -0x21, 0},
        };

        // --- On-screen reticle projection gate entry ------------------------
        // Direct entry hook. P1 is the prologue through `mov rcx, [rip+cam]`. P2
        // extends through the first two movss reads of the candidate world point.
        // P3 drops the leading shadow-save and walks back 0x0B.
        inline constexpr AddrCandidate k_interactionOnScreenCandidates[] = {
            {"OnScreen_P1_PrologueMovGlobal",
             "4C 8B DC 49 89 5B 10 49 89 73 18 49 89 4B 08 57 48 83 EC ?? 48 8B 0D ?? ?? ?? ?? 49 8D 70 04",
             ResolveMode::Direct, 0, 0},
            {"OnScreen_P2_PrologueThroughMovss",
             "4C 8B DC 49 89 5B 10 49 89 73 18 49 89 4B 08 57 48 83 EC ?? 48 8B 0D ?? ?? ?? ?? 49 8D 70 04 "
             "F3 0F 10 5A 08 49 8B F8 F3 0F 10 52 04",
             ResolveMode::Direct, 0, 0},
            {"OnScreen_P3_BodyMovGlobalMovss",
             "49 89 4B 08 57 48 83 EC ?? 48 8B 0D ?? ?? ?? ?? 49 8D 70 04 "
             "F3 0F 10 5A 08 49 8B F8 F3 0F 10 52 04 4D 8D 43 08 F3 0F 10 0A",
             ResolveMode::Direct, -0x0B, 0},
        };

        // --- HideOverlays entry ---------------------------------------------
        // Direct entry hook. HideOverlays and ShowOverlays share the prologue, so
        // every candidate keeps the `mov byte [rax+rcx+0B8h], 1` set-flag store
        // (C6 84 ?? ?? ?? ?? ?? 01) that distinguishes Hide from Show's cmp. P3
        // drops the prologue and walks back 0x0A.
        inline constexpr AddrCandidate k_overlayHideCandidates[] = {
            {"OverlayHide_P1_PrologueSetFlag",
             "44 88 44 24 18 53 48 83 EC ?? 0F B6 C2 48 8B D9 48 8D 15 ?? ?? ?? ?? C6 84 ?? ?? ?? ?? ?? 01",
             ResolveMode::Direct, 0, 0},
            {"OverlayHide_P2_SetFlagThroughCall",
             "44 88 44 24 18 53 48 83 EC ?? 0F B6 C2 48 8B D9 48 8D 15 ?? ?? ?? ?? C6 84 ?? ?? ?? ?? ?? 01 "
             "48 8D 4C 24 ?? E8",
             ResolveMode::Direct, 0, 0},
            {"OverlayHide_P3_BodySetFlag",
             "0F B6 C2 48 8B D9 48 8D 15 ?? ?? ?? ?? C6 84 ?? ?? ?? ?? ?? 01 48 8D 4C 24 ?? E8", ResolveMode::Direct,
             -0x0A, 0},
        };

        // --- ShowOverlays entry ---------------------------------------------
        // Direct entry hook. The `cmp byte [rax+rcx+0B8h], 0` test-flag read
        // (80 BC ?? ?? ?? ?? ?? 00) is the discriminator versus Hide's store. P1
        // deliberately stops before the following jz rel8 (the encoding can flip).
        // P2 wildcards that jz as ?? ?? and extends into the flag-clear store and
        // call. P3 drops the prologue and walks back 0x0A.
        inline constexpr AddrCandidate k_overlayShowCandidates[] = {
            {"OverlayShow_P1_PrologueTestFlag",
             "44 88 44 24 18 53 48 83 EC ?? 0F B6 C2 48 8B D9 80 BC ?? ?? ?? ?? ?? 00", ResolveMode::Direct, 0, 0},
            {"OverlayShow_P2_TestThroughClear",
             "44 88 44 24 18 53 48 83 EC ?? 0F B6 C2 48 8B D9 80 BC ?? ?? ?? ?? ?? 00 ?? ?? C6 84 ?? ?? ?? ?? ?? 00 E8",
             ResolveMode::Direct, 0, 0},
            {"OverlayShow_P3_BodyTestClear",
             "0F B6 C2 48 8B D9 80 BC ?? ?? ?? ?? ?? 00 ?? ?? C6 84 ?? ?? ?? ?? ?? 00 E8 ?? ?? ?? ?? 84 C0",
             ResolveMode::Direct, -0x0A, 0},
        };

        // --- UI menu-open entry (vftable[1]) --------------------------------
        // Direct entry hook. P1 anchors directly on the entry prologue through
        // the `cmp byte [rsi+670h], 0` field test. P2 is the mid-body anchor on the
        // vtable call that precedes lea "SetInputId" and walks back 0x36 to the
        // entry. P3 anchors on the field-test branch pair and walks back 0x1C. Jcc
        // rel32 displacements are wildcarded.
        inline constexpr AddrCandidate k_menuOpenCandidates[] = {
            {"MenuOpen_P1_EntryFieldTest",
             "48 89 5C 24 10 48 89 74 24 18 55 57 41 56 48 8B EC 48 83 EC 50 48 8D 71 A8 44 8A F2 80 BE 70 06 00 00 00",
             ResolveMode::Direct, 0, 0},
            {"MenuOpen_P2_VtableCallSetInput", "48 8B 41 B0 48 8B 48 30 48 8B 01 FF 10 48 8D 15 ?? ?? ?? ??",
             ResolveMode::Direct, -0x36, 0},
            {"MenuOpen_P3_FieldTestBranch", "80 BE 70 06 00 00 00 48 8B F9 0F 84 ?? ?? ?? ?? 80 79 48 00 0F 85",
             ResolveMode::Direct, -0x1C, 0},
        };

        // --- UI menu-close entry (vftable[2]) -------------------------------
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
        inline constexpr AddrCandidate k_menuCloseCandidates[] = {
            {"MenuClose_P1_EntryFieldTestRsi",
             "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 30 48 8D 71 A8 48 8B D9 80 BE A0 00 00 00 00",
             ResolveMode::Direct, 0, 0},
            {"MenuClose_P2_EntryFieldTestAltReg",
             "48 89 5C 24 18 57 48 83 EC 30 48 8D ?? A8 48 8B D9 80 ?? A0 00 00 00 00", ResolveMode::Direct, 0, 0},
            {"MenuClose_P3_DeactivateStore", "8A ?? 48 48 8D ?? 28 C6 ?? 49 00 E8 ?? ?? ?? ?? C6 ?? 48 00",
             ResolveMode::Direct, -0x18E, 0},
            {"MenuClose_P4_FieldTestBranch", "48 8D 71 A8 48 8B D9 80 BE A0 00 00 00 00 0F 84 ?? ?? ?? ?? E8",
             ResolveMode::Direct, -0x0F, 0},
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
        Count,
    };

    /**
     * @brief Resolves every game-image anchor in one parallel pass and records the results.
     * @details Builds the declarative DMK::Anchors table over the cascade candidate arrays above (each a
     *          RipGlobal anchor whose candidates already select Direct vs RIP-relative resolution) and
     *          resolves it with Anchors::resolve_all_parallel, confined to the WHGame.dll image
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
} // namespace TPVCamera

#endif // TPVCAMERA_AOB_RESOLVER_HPP
