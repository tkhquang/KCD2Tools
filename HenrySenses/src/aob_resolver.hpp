/**
 * @file aob_resolver.hpp
 * @brief The AOB candidate ladders, the anchor registry and the feature gates.
 *
 * Every code or data location the mod hooks, calls or reads is located at startup by a ladder of ordered AOB
 * candidates rather than a single signature, so a game patch that shifts code only has to leave one rung intact.
 * DetourModKit resolves each ladder in declared order (most specific first), confined to the WHGame.dll image (the
 * default scope, DMK::Region::host(), is the game executable), and only on executable pages: an instruction signature
 * can then never alias an identical byte run in data, nor a generic rung shadow a match in another module. A rung that
 * matches twice is skipped as ambiguous (require_unique).
 *
 * Signature rules (DetourModKit docs/misc/aob-signatures.md): every relative branch or call target, RIP displacement,
 * address immediate and virtual-call slot is wildcarded; a branch inside a pattern is a bounded gap ([2-6] for a
 * conditional jump, [2-5] for a jump) because compilers switch between the rel8 and rel32 encodings; no pattern
 * crosses the padding between functions; and every code ladder holds at least one rung that starts past the first 14
 * bytes of its function, so a sibling mod's inline hook on the prologue cannot hide every rung.
 *
 * Resolution shapes (DMK::scan::Candidate factories):
 *   - direct       address = match + walk_back (a mid-body rung walks back to the function entry).
 *   - rip_relative address = (match + instr_len) + disp32, for a load whose [rip+disp32] names a data slot. A call
 *                  site is never a rip_relative rung: the decoder accepts only a RIP-relative memory operand.
 *   - string_xref  the function enclosing the one reference to a unique string literal (.pdata bounds). A string
 *                  survives patches far better than the code bytes around it, so where a target references one it
 *                  leads its ladder.
 *
 * Every anchor passes a validator: a code anchor must lie on an executable page and agree with any covering unwind
 * function entry; a data anchor must be readable and non-executable inside the image. gEnv and the global context
 * resolve as 2-of-N quorums over their rungs,
 * because every other read goes through them. KCD2_HenrySenses.signatures.ini beside the ASI can replace any ladder by
 * label (manifest::overlay, gated by constants::SIGNATURE_REVISION), and [Settings] ExportSignatures writes the
 * built-in set in that format.
 *
 * A feature is enabled only through anchor::evaluate_gate over its own anchors (feature_ready): a missed anchor fails
 * its features closed.
 */
#ifndef HENRYSENSES_AOB_RESOLVER_HPP
#define HENRYSENSES_AOB_RESOLVER_HPP

#include <DetourModKit.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace HenrySenses
{
    namespace aob
    {
        using DMK::scan::Candidate;
        using DMK::scan::Pattern;

        // StdPipelineInit: CStandardGraphicsPipeline::Init (hooked, early phase). 3 rungs, most specific first.
        inline const Candidate STD_PIPELINE_INIT_CANDIDATES[] = {
            Candidate::direct(
                "StdPipelineInit_P1_BodyPushSub",
                Pattern::literal("41 56 48 83 EC ?? 48 8B D9 E8 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 48 8B CB"),
                -0x13
            ),
            Candidate::direct(
                "StdPipelineInit_P2_BodyXorTest",
                Pattern::literal("33 FF 48 85 C0 [2-6] 4C 8D 05 ?? ?? ?? ?? 48 8B D3"),
                -0x136
            ),
            Candidate::direct(
                "StdPipelineInit_P3_Flag20BaseInit",
                Pattern::literal("48 8B CB E8 ?? ?? ?? ?? BD ?? ?? ?? ?? 8B CD"),
                -0x122
            ),
        };

        // CustomCreatePso: CSceneCustomStage::CreatePipelineState (hooked, early phase). 3 rungs, most specific first.
        inline const Candidate CUSTOM_CREATE_PSO_CANDIDATES[] = {
            Candidate::direct(
                "CustomCreatePso_P1_Prologue",
                Pattern::literal(
                    "48 89 5C 24 18 48 89 74 24 20 55 57 41 54 41 56 41 57 48 8B EC 48 83 EC ?? 48 8B F1 "
                    "4D 8B E1"
                )
            ),
            Candidate::direct(
                "CustomCreatePso_P2_BodyMovCall",
                Pattern::literal("49 8B C9 41 8A F8 4C 8B F2 E8 ?? ?? ?? ?? 49 8B 0E"),
                -0x1F
            ),
            Candidate::direct(
                "CustomCreatePso_P3_BodyMovTest",
                Pattern::literal("48 8B 96 ?? ?? ?? ?? 44 8B F8 48 85 D2 [2-6] 48 8B 52 ??"),
                -0x39
            ),
        };

        // RegisterCustomStage: CGraphicsPipeline::RegisterStage<CSceneCustomStage> (called, early phase). 3 rungs, most
        // specific first.
        inline const Candidate REGISTER_CUSTOM_STAGE_CANDIDATES[] = {
            Candidate::direct(
                "RegisterCustomStage_P1_BodyMovAdd",
                Pattern::literal("48 8B D8 48 89 5F 50 48 8B 5C 24 38 48 83 C4 20"),
                -0x5A
            ),
            Candidate::direct(
                "RegisterCustomStage_P2_AllocStore50",
                Pattern::literal("45 33 C0 B9 E0 14 00 00 FF D0 8B 4C 24 30"),
                -0x23
            ),
            Candidate::direct(
                "RegisterCustomStage_P3_AllocStore50Alt",
                Pattern::literal("F0 01 0D ?? ?? ?? ?? F0 81 05 ?? ?? ?? ?? E0 14 00 00"),
                -0x31
            ),
        };

        // Genv: SSystemGlobalEnvironment base (data). 3 rungs, most specific first.
        inline const Candidate GENV_CANDIDATES[] = {
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

        // Context: Global-context storage slot (data). 3 rungs, most specific first.
        inline const Candidate CONTEXT_CANDIDATES[] = {
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

        // AfterPostHdr: CSceneForwardStage::ExecuteAfterPostProcessHDR (hooked). 3 rungs, most specific first.
        inline const Candidate AFTER_POST_HDR_CANDIDATES[] = {
            Candidate::direct(
                "AfterPostHdr_P1_Prologue",
                Pattern::literal("48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 30 48 8B 79 ?? 48 8B F1")
            ),
            Candidate::direct(
                "AfterPostHdr_P2_BodyTestMov",
                Pattern::literal("84 C0 [2-6] 48 8B 05 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8D 4C 24 50"),
                -0x2F
            ),
            Candidate::direct(
                "AfterPostHdr_P3_BodyLeaCallMov",
                Pattern::literal("48 8D 4C 24 50 E8 ?? ?? ?? ?? 48 8B 4E ?? 41 8D 56 ??"),
                -0x63
            ),
        };

        // ClearSurface: CClearSurfacePass::Execute (called). 3 rungs, most specific first.
        inline const Candidate CLEAR_SURFACE_CANDIDATES[] = {
            Candidate::direct(
                "ClearSurface_P1_Body",
                Pattern::literal("40 53 48 83 EC 30 41 0F 10 00 48 8D 59 ?? 41 B0 02")
            ),
            Candidate::direct(
                "ClearSurface_P2_BodyMovupsMovLea",
                Pattern::literal("41 B0 ?? 4C 8D 4C 24 20 48 8B CB 0F 11 44 24 20"),
                -0xE
            ),
            Candidate::direct(
                "ClearSurface_P3_BodyMovupsLeaMov",
                Pattern::literal("4C 8D 4C 24 20 48 8B CB 0F 11 44 24 20"),
                -0x11
            ),
        };

        // PrepareRenderPass: CSceneRenderPass::PrepareRenderPassForUse (called). 3 rungs, most specific first.
        inline const Candidate PREPARE_RENDER_PASS_CANDIDATES[] = {
            Candidate::direct(
                "PrepareRenderPass_P1_BodyMovLea",
                Pattern::literal("48 8B 03 48 8D 55 ?? 48 8B CB FF 50 ?? 88 43 ??"),
                -0x3F
            ),
            Candidate::direct(
                "PrepareRenderPass_P2_BodyPushSub",
                Pattern::literal("41 57 48 83 EC ?? 4C 8B F2 48 8D B9 ?? ?? ?? ??"),
                -0x17
            ),
            Candidate::direct(
                "PrepareRenderPass_P3_BodyMovCall",
                Pattern::literal("48 8B CF E8 ?? ?? ?? ?? 48 8B 1F 80 7B ?? ??"),
                -0x2E
            ),
        };

        // DrawRenderItems: CSceneRenderPass::DrawRenderItems (called). 3 rungs, most specific first.
        inline const Candidate DRAW_RENDER_ITEMS_CANDIDATES[] = {
            Candidate::direct(
                "DrawRenderItems_P1_Prologue",
                Pattern::literal("44 89 4C 24 20 55 53 56 57 41 56 48 8D 6C 24 C0")
            ),
            Candidate::direct(
                "DrawRenderItems_P2_NoDrawAndNearestMask",
                Pattern::literal("48 8B D9 41 0F B6 F8 48 8B 8A ?? ?? ?? ?? 48 8B F2"),
                -0x25
            ),
            Candidate::direct(
                "DrawRenderItems_P3_BodyXorMovBts",
                Pattern::literal("45 32 F6 44 8B 83 ?? ?? ?? ?? 40 8A D7 41 0F BA E8 ?? 48 8B CE"),
                -0x6E
            ),
        };

        // JobifyDraws: CRenderItemDrawer::JobifyDrawSubmission (called). 3 rungs, most specific first.
        inline const Candidate JOBIFY_DRAWS_CANDIDATES[] = {
            Candidate::direct(
                "JobifyDraws_P1_BodyXorMov",
                Pattern::literal("48 33 C4 48 89 45 27 4C 8B 61 ?? 33 FF 4C 8B 79 ??"),
                -0x2A
            ),
            Candidate::direct(
                "JobifyDraws_P2_BodyMovCmp",
                Pattern::literal("4C 89 65 C7 4D 3B FC [2-6] 49 8D 4F ?? 8B 41 ?? 2B 01"),
                -0x3E
            ),
            Candidate::direct(
                "JobifyDraws_P3_BodyLeaAdd",
                Pattern::literal("48 8D 89 ?? ?? ?? ?? 03 F8 48 8D 41 ?? 49 3B C4"),
                -0x54
            ),
        };

        // WaitDraws: CRenderItemDrawer::WaitForDrawSubmission (called). 2 rungs, most specific first.
        inline const Candidate WAIT_DRAWS_CANDIDATES[] = {
            Candidate::direct(
                "WaitDraws_P1_BodyExact",
                Pattern::literal("48 83 EC 28 83 3D ?? ?? ?? ?? 00 [2-6] E8 ?? ?? ?? ?? 48 83 C4 28")
            ),
            Candidate::direct(
                "WaitDraws_P2_BodyAnyCmpImm",
                Pattern::literal("48 83 EC 28 83 3D ?? ?? ?? ?? ?? [2-6] E8 ?? ?? ?? ?? 48 83 C4 28 C3")
            ),
        };

        // SetTechnique: CRenderPrimitive::SetTechnique (called). 3 rungs, most specific first.
        inline const Candidate SET_TECHNIQUE_CANDIDATES[] = {
            Candidate::direct(
                "SetTechnique_P1_Prologue",
                Pattern::literal("48 89 5C 24 08 48 8B 81 ?? ?? ?? ?? 48 8B DA 48 2B C2")
            ),
            Candidate::direct(
                "SetTechnique_P2_BodyNegSbb",
                Pattern::literal("48 89 91 ?? ?? ?? ?? 48 F7 D8 4C 8B D9 45 1B D2"),
                -0x12
            ),
            Candidate::direct(
                "SetTechnique_P3_BodyNegSbbAlt",
                Pattern::literal("8B 81 ?? ?? ?? ?? 41 2B 00 F7 D8 1B C0 83 E0 ??"),
                -0x2E
            ),
        };

        // SetRenderTarget: CPrimitiveRenderPass::SetRenderTarget implementation, behind the thunk that adds 0x58 to the
        // pass (called). 3 rungs, most specific first.
        inline const Candidate SET_RENDER_TARGET_CANDIDATES[] = {
            Candidate::direct(
                "SetRenderTarget_P1_BodyOrMov",
                Pattern::literal("0B C6 4C 89 45 D0 89 45 D8 89 5D DC [2-5] 48 89 5D E0"),
                -0x40
            ),
            Candidate::direct(
                "SetRenderTarget_P2_BodyMovapsMovSub",
                Pattern::literal("48 8B EC 48 83 EC ?? 33 DB 0F 29 70 D8 4D 8B F0"),
                -0x18
            ),
            Candidate::direct(
                "SetRenderTarget_P3_BodyMovzxMovTest",
                Pattern::literal("BE ?? ?? ?? ?? 4D 85 C0 [2-6] 41 0F B6 C1 4C 8D 7D D0"),
                -0x2B
            ),
        };

        // SetTexture: CDeviceResourceSetDesc::SetTexture (called). 3 rungs, most specific first.
        inline const Candidate SET_TEXTURE_CANDIDATES[] = {
            Candidate::direct(
                "SetTexture_P1_BodyShlLeaMov",
                Pattern::literal("48 8D 4C 24 40 C1 E0 ?? 4C 89 44 24 30 83 C8 ??"),
                -0x16
            ),
            Candidate::direct("SetTexture_P2_Prologue", Pattern::literal("40 53 48 83 EC ?? 41 0F B6 C1 48 8B D9")),
            Candidate::direct(
                "SetTexture_P3_BodyShlMovOr",
                Pattern::literal("C1 E0 ?? 4C 89 44 24 30 83 C8 ?? 44 8A C2"),
                -0x1B
            ),
        };

        // SetSampler: CDeviceResourceSetDesc::SetSampler (called). 3 rungs, most specific first.
        inline const Candidate SET_SAMPLER_CANDIDATES[] = {
            Candidate::direct(
                "SetSampler_P1_BodyMovapsXorMov",
                Pattern::literal("33 FF 0F 29 78 C8 44 8A C2 48 89 7D B0 4C 8B F9"),
                -0x2A
            ),
            Candidate::direct(
                "SetSampler_P2_BodyMovzxPushMov",
                Pattern::literal("41 57 48 8B EC 48 81 EC ?? ?? ?? ?? 41 0F B7 D8"),
                -0x16
            ),
            Candidate::direct(
                "SetSampler_P3_BodyMovLea",
                Pattern::literal("66 89 5D B0 48 8D 55 B0 48 C7 45 B8 ?? ?? ?? ??"),
                -0x3A
            ),
        };

        // BeginConstantUpdate: CFullscreenPass::BeginConstantUpdate (called). 3 rungs, most specific first.
        inline const Candidate BEGIN_CONSTANT_UPDATE_CANDIDATES[] = {
            Candidate::direct(
                "BeginConstantUpdate_P1_Prologue",
                Pattern::literal(
                    "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC ?? F6 81 ?? "
                    "?? ?? ?? ??"
                )
            ),
            Candidate::direct(
                "BeginConstantUpdate_P2_BodyMovsxdMovTest",
                Pattern::literal("48 8B 83 ?? ?? ?? ?? 48 63 30 48 85 F6 [2-6] 41 8B FE"),
                -0x3F
            ),
            Candidate::direct(
                "BeginConstantUpdate_P3_BodyTestXorCmp",
                Pattern::literal("F6 83 ?? ?? ?? ?? ?? [2-6] 45 33 F6 44 38 B3 ?? ?? ?? ??"),
                -0x2A
            ),
        };

        // SetConstant: CFullscreenPass::SetConstant (called). 3 rungs, most specific first.
        inline const Candidate SET_CONSTANT_CANDIDATES[] = {
            Candidate::direct(
                "SetConstant_P1_Body",
                Pattern::literal("48 83 EC ?? 41 0F 10 00 44 88 4C 24 20 4C 8D 44 24 30")
            ),
            Candidate::direct(
                "SetConstant_P2_BodyMovdquMov",
                Pattern::literal("41 B9 ?? ?? ?? ?? F3 0F 7F 44 24 30"),
                -0x19
            ),
            Candidate::direct(
                "SetConstant_P3_BodyAddMovImm",
                Pattern::literal("48 81 C1 18 04 00 00 41 B9 01 00 00 00"),
                -0x12
            ),
        };

        // FullscreenExecute: CFullscreenPass::Execute (called). 3 rungs, most specific first.
        inline const Candidate FULLSCREEN_EXECUTE_CANDIDATES[] = {
            Candidate::direct(
                "FullscreenExecute_P1_BodyXorMov",
                Pattern::literal("48 33 C4 48 89 44 24 70 33 FF 48 89 4C 24 28"),
                -0x26
            ),
            Candidate::direct(
                "FullscreenExecute_P2_BodyCmpMov",
                Pattern::literal("48 39 83 ?? ?? ?? ?? [2-6] 48 8B CB E8 ?? ?? ?? ?? 40 88 BB ?? ?? ?? ??"),
                -0x51
            ),
            Candidate::direct(
                "FullscreenExecute_P3_BodyMovCmpCall",
                Pattern::literal("48 8B D9 40 38 B9 ?? ?? ?? ?? [2-6] E8 ?? ?? ?? ?? 48 8B 83 ?? ?? ?? ??"),
                -0x35
            ),
        };

        // CryNameR: CCryNameR constructor (called). 3 rungs, most specific first.
        inline const Candidate CRY_NAME_R_CANDIDATES[] = {
            Candidate::direct(
                "CryNameR_P1_Prologue",
                Pattern::literal("48 89 5C 24 08 48 89 74 24 10 57 48 83 EC ?? 33 DB 48 8B F2 48 89 19")
            ),
            Candidate::direct(
                "CryNameR_P2_BodyMovTest",
                Pattern::literal("48 8B F9 48 85 D2 [2-6] 38 1A [2-6] 48 8D 0D ?? ?? ?? ??"),
                -0x17
            ),
            Candidate::direct(
                "CryNameR_P3_BodyMovCall",
                Pattern::literal("48 8B C8 E8 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 48 8B D8"),
                -0x38
            ),
        };

        // DisplayTargetDst: GetDisplayTargetDst (called). 3 rungs, most specific first.
        inline const Candidate DISPLAY_TARGET_DST_CANDIDATES[] = {
            Candidate::direct(
                "DisplayTargetDst_P1_Body",
                Pattern::literal("40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 84 C0 [2-6] 48 8B 83 00 02 00 00")
            ),
            Candidate::direct(
                "DisplayTargetDst_P2_BodyTestMov",
                Pattern::literal("84 C0 [2-6] 48 8B 83 00 02 00 00 48 83 C4 20 5B C3"),
                -0xE
            ),
            Candidate::direct(
                "DisplayTargetDst_P3_BodyMovAdd",
                Pattern::literal("48 8B 83 00 02 00 00 48 83 C4 20 5B C3"),
                -0x16
            ),
        };

        // CoreCommandListSlot: Core graphics command-list slot (data). 3 rungs, most specific first.
        inline const Candidate CORE_COMMAND_LIST_SLOT_CANDIDATES[] = {
            Candidate::rip_relative(
                "CoreCommandListSlot_P1_MovCallTest",
                Pattern::literal("48 8B 15 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? [2-5] 84 C0"),
                3,
                7
            ),
            Candidate::rip_relative(
                "CoreCommandListSlot_P2_MovCall",
                Pattern::literal("48 8B 15 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? B0 ??"),
                3,
                7
            ),
            Candidate::rip_relative(
                "CoreCommandListSlot_P3_LoadR8ImulStride",
                Pattern::literal("4C 8B 05 ?? ?? ?? ?? 48 6B D0 ?? 8B 43 ?? 48 03 13 89 44 24 20"),
                3,
                7
            ),
        };

        // RecursionCounter: CSceneRenderPass::s_recursionCounter (data). 3 rungs, most specific first.
        inline const Candidate RECURSION_COUNTER_CANDIDATES[] = {
            Candidate::rip_relative(
                "RecursionCounter_P1_SubMov",
                Pattern::literal("29 05 ?? ?? ?? ?? 48 8B 6C 24 68 48 8B 74 24 70"),
                2,
                6
            ),
            Candidate::rip_relative(
                "RecursionCounter_P2_IncMov",
                Pattern::literal("FF 05 ?? ?? ?? ?? 48 8B D6 49 89 46 ?? 48 8B CD"),
                2,
                6
            ),
            Candidate::rip_relative(
                "RecursionCounter_P3_AddCounterArgSetup",
                Pattern::literal("01 05 ?? ?? ?? ?? 45 33 C9 48 8B 44 24 70 41 B0 ?? 89 7C 24 20"),
                2,
                6
            ),
        };

        // PostEffectsGameSlot: CShaderMan::s_shPostEffectsGame slot (data). 3 rungs, most specific first.
        inline const Candidate POST_EFFECTS_GAME_SLOT_CANDIDATES[] = {
            Candidate::rip_relative(
                "PostEffectsGameSlot_P1_MovAddXor",
                Pattern::literal("48 8B 15 ?? ?? ?? ?? 48 03 CE 45 33 C9 C6 44 24 20 ??"),
                3,
                7
            ),
            Candidate::rip_relative(
                "PostEffectsGameSlot_P2_MovLea",
                Pattern::literal("48 8B 15 ?? ?? ?? ?? 48 8D 9F ?? ?? ?? ?? 48 8B CB C6 44 24 20 ??"),
                3,
                7
            ),
            Candidate::rip_relative(
                "PostEffectsGameSlot_P3_MovCallXor",
                Pattern::literal("48 8B 15 ?? ?? ?? ?? E8 ?? ?? ?? ?? 4C 8B 05 ?? ?? ?? ?? 33 D2 48 8B CE"),
                3,
                7
            ),
        };

        // ProxyRender: CRenderProxy::Render (hooked). 3 rungs, most specific first.
        inline const Candidate PROXY_RENDER_CANDIDATES[] = {
            Candidate::direct(
                "ProxyRender_P1_BodyMov",
                Pattern::literal("48 8B DA 48 8B F9 48 8B 0D ?? ?? ?? ?? 4D 8B F8"),
                -0x25
            ),
            Candidate::direct(
                "ProxyRender_P2_HiddenTestCopyCtor",
                Pattern::literal("84 C0 [2-6] F6 87 ?? ?? ?? ?? ?? [2-6] 48 8B D3 48 8D 4C 24 50"),
                -0x4B
            ),
            Candidate::direct(
                "ProxyRender_P3_BodyMovShrAnd",
                Pattern::literal("8B 57 ?? 48 8B 01 48 C1 EA ?? 83 E2 ?? FF 90 ?? ?? ?? ?? 45 33 E4"),
                -0x35
            ),
        };

        // PostUpdate: CCryAction::PostUpdate (hooked, main-thread tick). 3 rungs, most specific first.
        inline const Candidate POST_UPDATE_CANDIDATES[] = {
            Candidate::direct(
                "PostUpdate_P1_EditorOnlyTestPrologue",
                Pattern::literal("41 F6 C0 ?? [2-6] 48 8B C4 48 89 58 08 48 89 70 10")
            ),
            Candidate::direct(
                "PostUpdate_P2_BodyMovapsPushMov",
                Pattern::literal("57 41 57 48 8B EC 48 81 EC ?? ?? ?? ?? 0F 29 70 D8"),
                -0x1A
            ),
            Candidate::direct(
                "PostUpdate_P3_AiPhysicsTestGameCheck",
                Pattern::literal("48 8B F1 41 F6 C0 ?? [2-6] 48 8B 89 ?? ?? ?? ?? 48 8B 01"),
                -0x2B
            ),
        };

        // RegisterEntity: C3DEngine::RegisterEntity (vtable validator). 3 rungs, most specific first.
        inline const Candidate REGISTER_ENTITY_CANDIDATES[] = {
            Candidate::direct(
                "RegisterEntity_P1_BodyTestMovCall",
                Pattern::literal("48 85 42 ?? [2-6] 44 8B 05 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 83 C4 28 C3"),
                -0xE
            ),
            Candidate::direct(
                "RegisterEntity_P2_PrologueSubMovabsTest",
                Pattern::literal("48 83 EC 28 48 B8 ?? ?? ?? ?? ?? ?? ?? ?? 48 85 42 ??")
            ),
            Candidate::direct(
                "RegisterEntity_P3_BodyMovCallAdd",
                Pattern::literal("44 8B 05 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 83 C4 28 C3"),
                -0x14
            ),
        };

        // UnRegisterEntity: C3DEngine::UnRegisterEntityDirect implementation (vtable validator). 3 rungs, most specific
        // first.
        inline const Candidate UNREGISTER_ENTITY_CANDIDATES[] = {
            Candidate::direct(
                "UnRegisterEntity_P1_Prologue",
                Pattern::literal("48 8B C4 48 89 58 18 48 89 68 20 48 89 50 10 56 57 41 56 48 83 EC ?? 80 7A ?? ??")
            ),
            Candidate::direct(
                "UnRegisterEntity_P2_BodyMov",
                Pattern::literal("48 8B FA 48 8B E9 48 89 50 08 [2-6] 48 8B 07 48 8B CF"),
                -0x1B
            ),
            Candidate::direct(
                "UnRegisterEntity_P3_BodyMovTest",
                Pattern::literal("8B F0 48 85 C9 [2-6] 48 8B D7 E8 ?? ?? ?? ?? 44 8A F0"),
                -0x3B
            ),
        };

        // GetEntity: CEntitySystem::GetEntity (vtable validator). 3 rungs, most specific first.
        inline const Candidate GET_ENTITY_CANDIDATES[] = {
            Candidate::direct(
                "GetEntity_P1_PrologueReadLock",
                Pattern::literal("48 89 5C 24 08 57 48 83 EC ?? 8B FA 48 8B D9 B8 ?? ?? ?? ??")
            ),
            Candidate::direct(
                "GetEntity_P2_SpinWaitWriterBit",
                Pattern::literal("48 8D 4C 24 38 E8 ?? ?? ?? ?? 8B 83 ?? ?? ?? ?? 0F BA E0 ?? [2-6] 8B D7"),
                -0x28
            ),
            Candidate::direct(
                "GetEntity_P3_BodyMovTest",
                Pattern::literal("48 8B 84 C3 ?? ?? ?? ?? 48 85 C0 [2-6] 39 78 ?? [2-6] 33 C0"),
                -0x67
            ),
        };

        // GetEntityIterator: CEntitySystem::GetEntityIterator (vtable validator). 3 rungs, most specific first.
        inline const Candidate GET_ENTITY_ITERATOR_CANDIDATES[] = {
            Candidate::direct(
                "GetEntityIterator_P1_BodyMovCmp",
                Pattern::literal("48 8B F9 39 35 ?? ?? ?? ?? [2-6] E8 ?? ?? ?? ?? 45 33 C0 48 8D 54 24 38 41 8D 48 20"),
                -0x11
            ),
            Candidate::direct(
                "GetEntityIterator_P2_BodyMov",
                Pattern::literal("48 8B DE 48 8B 74 24 40 48 8B C3 48 8B 5C 24 30"),
                -0x77
            ),
            Candidate::direct(
                "GetEntityIterator_P3_ItMapVtableStore",
                Pattern::literal("48 8B CB 48 89 03 66 89 73 ?? 89 73 ?? 89 73 ??"),
                -0x60
            ),
        };

        // GetProxy: CEntity::GetProxy (vtable validator). 3 rungs, most specific first.
        inline const Candidate GET_PROXY_CANDIDATES[] = {
            Candidate::direct(
                "GetProxy_P1_MapKeyWalk",
                Pattern::literal("41 39 51 ?? 49 8D 41 ?? 49 0F 43 C1 49 0F 43 C9"),
                -0x13
            ),
            Candidate::direct(
                "GetProxy_P2_MapHead",
                Pattern::literal("4C 8B 81 ?? ?? ?? ?? 45 33 D2 49 8B C8 4D 8B 48 ??")
            ),
            Candidate::direct(
                "GetProxy_P3_BodyMovCmpRet",
                Pattern::literal(
                    "4C 8B 08 45 38 51 ?? [2-6] 49 3B C8 [2-6] 44 38 51 ?? [2-6] 3B 51 ?? [2-6] 48 8B 41 "
                    "?? C3"
                ),
                -0x23
            ),
        };

        // GetWorldBounds: CEntity::GetWorldBounds (vtable validator). 3 rungs, most specific first.
        inline const Candidate GET_WORLD_BOUNDS_CANDIDATES[] = {
            Candidate::direct(
                "GetWorldBounds_P1_Prologue",
                Pattern::literal("48 89 5C 24 08 48 89 7C 24 10 55 48 8D 6C 24 A9 48 81 EC 90 00 00 00 48 8B FA")
            ),
            Candidate::direct(
                "GetWorldBounds_P2_LocalBoundsInvalidTest",
                Pattern::literal("48 8B D9 E8 ?? ?? ?? ?? F3 0F 10 07 0F 2F 47 ??"),
                -0x1A
            ),
            Candidate::direct(
                "GetWorldBounds_P3_WorldTmLea",
                Pattern::literal("48 8D 53 ?? 48 8D 4D 1F E8 ?? ?? ?? ?? 45 33 DB"),
                -0x30
            ),
        };

        // GetAuxGeom: CD3D9Renderer::GetIRenderAuxGeom (vtable validator). 3 rungs, most specific first.
        inline const Candidate GET_AUX_GEOM_CANDIDATES[] = {
            Candidate::direct(
                "GetAuxGeom_P1_BodyMovCall",
                Pattern::literal(
                    "48 8B D9 FF 15 ?? ?? ?? ?? 3B 87 ?? ?? ?? ?? [2-6] 3B 87 ?? ?? ?? ?? [2-6] 48 8B 8B "
                    "?? ?? ?? ??"
                ),
                -0x11
            ),
            Candidate::direct(
                "GetAuxGeom_P2_BodyMovCallAdd",
                Pattern::literal("48 8B 8B 88 2F 00 00 E8 ?? ?? ?? ?? 48 8B 5C 24 30 48 83 C4 20 5F C3"),
                -0x2A
            ),
            Candidate::direct(
                "GetAuxGeom_P3_BodyCmpMovCall",
                Pattern::literal("3B 87 ?? ?? ?? ?? [2-6] 48 8B 8B ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 5C 24 30"),
                -0x22
            ),
        };

        // AuxSetFlags: CAuxGeomCB::SetRenderFlags (vtable validator). 3 rungs, most specific first.
        inline const Candidate AUX_SET_FLAGS_CANDIDATES[] = {
            Candidate::direct(
                "AuxSetFlags_P1_BodyMovRet",
                Pattern::literal("89 02 41 8B 00 41 89 81 ?? ?? ?? ?? 48 8B C2 C3"),
                -0xB
            ),
            Candidate::direct(
                "AuxSetFlags_P2_Body",
                Pattern::literal("4C 8B 49 ?? 41 8B 81 ?? ?? ?? ?? 89 02 41 8B 00")
            ),
            Candidate::direct(
                "AuxSetFlags_P3_BodyMovRetAlt",
                Pattern::literal("41 89 81 ?? ?? ?? ?? 48 8B C2 C3"),
                -0x10
            ),
        };

        // AuxGetFlags: CAuxGeomCB::GetRenderFlags (vtable validator). 3 rungs, most specific first.
        inline const Candidate AUX_GET_FLAGS_CANDIDATES[] = {
            Candidate::direct(
                "AuxGetFlags_P1_BodyExact",
                Pattern::literal("48 8B 41 10 8B 88 E0 00 00 00 48 8B C2 89 0A C3")
            ),
            Candidate::direct(
                "AuxGetFlags_P2_Body",
                Pattern::literal("48 8B 41 ?? 8B 88 ?? ?? ?? ?? 48 8B C2 89 0A C3")
            ),
            Candidate::direct(
                "AuxGetFlags_P3_BodyMovRet",
                Pattern::literal("8B 88 ?? ?? ?? ?? 48 8B C2 89 0A C3"),
                -0x4
            ),
        };

        // AuxDrawLines: CAuxGeomCB::DrawLines (vtable validator). 3 rungs, most specific first.
        inline const Candidate AUX_DRAW_LINES_CANDIDATES[] = {
            Candidate::direct(
                "AuxDrawLines_P1_BodyComissMov",
                Pattern::literal("48 8B FA 0F 2F C6 41 8B F0 4C 8B F1 [2-6] 41 8A 41 ??"),
                -0x34
            ),
            Candidate::direct(
                "AuxDrawLines_P2_BodyMovssMovaps",
                Pattern::literal("F3 0F 10 05 ?? ?? ?? ?? 33 DB 0F 29 74 24 40 49 8B E9"),
                -0x19
            ),
            Candidate::direct(
                "AuxDrawLines_P3_AlphaFlags",
                Pattern::literal("FE C8 49 89 5B D8 3C ?? 4D 8D 4B 28 49 8B 46 ??"),
                -0x4F
            ),
        };

        // GetObjectsInBox: C3DEngine::GetObjectsInBox (called). 3 rungs, most specific first.
        inline const Candidate GET_OBJECTS_IN_BOX_CANDIDATES[] = {
            Candidate::direct(
                "GetObjectsInBox_P1_PrologueThroughOctree",
                Pattern::literal("48 8B C4 48 89 58 08 48 89 70 10 57 48 83 EC ?? 48 8B 89 ?? ?? ?? ?? 49 8B F0")
            ),
            Candidate::direct(
                "GetObjectsInBox_P2_OctreeLoadBody",
                Pattern::literal("4C 8B C2 48 C7 40 E8 ?? ?? ?? ?? 48 8B DA 0F 57 C0"),
                -0x1A
            ),
            Candidate::direct(
                "GetObjectsInBox_P3_BodyMovdquCallMov",
                Pattern::literal("F3 0F 7F 40 D8 E8 ?? ?? ?? ?? 48 8B 3D ?? ?? ?? ??"),
                -0x2F
            ),
        };

        // ObjManager: CObjManager instance slot (data). 3 rungs, most specific first.
        inline const Candidate OBJ_MANAGER_CANDIDATES[] = {
            Candidate::rip_relative(
                "ObjManager_P1_MovCmovne",
                Pattern::literal("48 8B 0D ?? ?? ?? ?? 48 0F 45 D3 44 88 44 24 28"),
                3,
                7
            ),
            Candidate::rip_relative(
                "ObjManager_P2_CmpMovapsMov",
                Pattern::literal("48 83 3D ?? ?? ?? ?? ?? 49 8B F0 0F 29 74 24 40"),
                3,
                8
            ),
            Candidate::rip_relative(
                "ObjManager_P3_MovMovapsMulss",
                Pattern::literal("48 8B 0D ?? ?? ?? ?? 0F 28 C1 F3 0F 59 44 05 ??"),
                3,
                7
            ),
        };

        // AfterPostLdr: CSceneForwardStage::ExecuteAfterPostProcessLDR (hooked, P5). 4 rungs, most specific first.
        inline const Candidate AFTER_POST_LDR_CANDIDATES[] = {
            Candidate::string_xref(
                "AfterPostLdr_X1_EffectLabelXref",
                DMK::scan::StringRefQuery{
                    .text = "POST_EFFECTS_LDR_AP",
                    .return_mode = DMK::scan::XrefReturn::EnclosingFunction,
                }
            ),
            Candidate::direct(
                "AfterPostLdr_P1_MarkerSetup",
                Pattern::literal("48 8D 15 ?? ?? ?? ?? 4C 8B F1 49 8D 4B 08"),
                -0x23
            ),
            Candidate::direct(
                "AfterPostLdr_P2_BodyLeaCall",
                Pattern::literal("48 8D 4C 24 60 E8 ?? ?? ?? ?? 4D 8B 7E ??"),
                -0x4A
            ),
            Candidate::direct(
                "AfterPostLdr_P3_BodyLeaCallXor",
                Pattern::literal("4D 8D AF ?? ?? ?? ?? E8 ?? ?? ?? ?? 45 33 E4 48 85 C0"),
                -0x5F
            ),
        };

        // LdrTarget: The LDR display target helper (called). 3 rungs, most specific first.
        inline const Candidate LDR_TARGET_CANDIDATES[] = {
            Candidate::direct(
                "LdrTarget_P1_Prologue",
                Pattern::literal(
                    "40 53 48 83 EC ?? 48 8B D9 E8 ?? ?? ?? ?? 48 8B 8B ?? ?? ?? ?? 48 8B 89 ?? ?? ?? ?? "
                    "84 C0"
                )
            ),
            Candidate::direct(
                "LdrTarget_P2_BodyMovTestCall",
                Pattern::literal("48 8B 89 ?? ?? ?? ?? 84 C0 [2-6] E8 ?? ?? ?? ?? 48 83 C4 20 5B C3"),
                -0x15
            ),
            Candidate::direct(
                "LdrTarget_P3_BodyMovTestCall",
                Pattern::literal("48 8B 8B ?? ?? ?? ?? 48 8B 89 ?? ?? ?? ?? 84 C0 [2-6] E8 ?? ?? ?? ?? 48 83 C4 20"),
                -0xE
            ),
        };

        // StatObjRenderInternal: CStatObj::RenderInternal (hooked, P3b). 3 rungs, most specific first.
        inline const Candidate RENDER_INTERNAL_CANDIDATES[] = {
            Candidate::direct(
                "StatObjRenderInternal_P1_ArgSetup",
                Pattern::literal("4D 8B F8 48 8B FA 48 8B F1 [2-6] 41 8B 45 ?? 45 33 DB"),
                -0x35
            ),
            Candidate::direct(
                "StatObjRenderInternal_P2_ArgSetupAlt",
                Pattern::literal("48 33 C4 48 89 45 07 F6 81 ?? ?? ?? ?? ?? 4D 8B F1"),
                -0x20
            ),
            Candidate::direct(
                "StatObjRenderInternal_P3_BodyMovAnd",
                Pattern::literal("41 8B 45 ?? 89 41 ?? 41 8B 40 ?? 41 B9 ?? ?? ?? ?? 41 8B D3 49 23 C1"),
                -0x5A
            ),
        };

        // StatObjRender: CStatObj::Render (called, herb outlines). 3 rungs, most specific first.
        inline const Candidate STAT_OBJ_RENDER_CANDIDATES[] = {
            Candidate::direct(
                "StatObjRender_P1_Prologue",
                Pattern::literal("48 89 5C 24 10 48 89 74 24 18 55 57 41 56 48 8B EC 48 83 EC ?? F6 81 ?? ?? ?? ?? ??")
            ),
            Candidate::direct(
                "StatObjRender_P2_BodyMovXor",
                Pattern::literal("48 8B FA 4C 8B F1 [2-6] 48 8B 42 ?? 33 DB 48 89 5D 20"),
                -0x1F
            ),
            Candidate::direct(
                "StatObjRender_P3_BodyMovLea",
                Pattern::literal("48 8B 57 ?? 4C 8D 8F ?? ?? ?? ?? 48 8B 07 4C 8D 45 20 48 89 7C 24 30"),
                -0x63
            ),
        };

        // BlurPassCtor: CGaussianBlurPass constructor (called, mask blur). 3 rungs, most specific first.
        inline const Candidate BLUR_PASS_CTOR_CANDIDATES[] = {
            Candidate::direct(
                "BlurPassCtor_P1_ScaleInit",
                Pattern::literal("4C 8B C3 48 8B D7 E8 ?? ?? ?? ?? B8 ?? ?? ?? ??"),
                -0x42
            ),
            Candidate::direct(
                "BlurPassCtor_P2_BodyMovLea",
                Pattern::literal("89 86 ?? ?? ?? ?? 44 8B C7 48 8D 8E ?? ?? ?? ??"),
                -0x67
            ),
            Candidate::direct(
                "BlurPassCtor_P3_BodyMovCallLea",
                Pattern::literal("4C 8B CB 44 8B C7 8B D7 E8 ?? ?? ?? ?? 48 8D 8E ?? ?? ?? ?? 4C 8B CB"),
                -0x85
            ),
        };

        // BlurPassExecute: CGaussianBlurPass::Execute (called, mask blur). 3 rungs, most specific first.
        inline const Candidate BLUR_PASS_EXECUTE_CANDIDATES[] = {
            Candidate::direct(
                "BlurPassExecute_P1_Prologue",
                Pattern::literal(
                    "48 85 D2 [2-6] 48 8B C4 48 89 58 08 48 89 70 18 48 89 78 20 55 41 54 41 55 41 56 41 "
                    "57 48 8B EC"
                )
            ),
            Candidate::direct(
                "BlurPassExecute_P2_ArgSaves",
                Pattern::literal("0F 28 F3 0F 29 78 B8 4D 8B E8 4C 8B FA 4C 8B F1"),
                -0x2C
            ),
            Candidate::direct(
                "BlurPassExecute_P3_ArgSavesAlt",
                Pattern::literal("48 8B 05 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8D 4D 38"),
                -0x45
            ),
        };

        // SuperResolutionExecute: CSuperResolutionStage::Execute (hooked, P6). 3 rungs, most specific first.
        inline const Candidate SUPER_RESOLUTION_EXECUTE_CANDIDATES[] = {
            Candidate::direct(
                "SuperResolutionExecute_P1_Prologue",
                Pattern::literal(
                    "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 54 41 55 41 56 41 57 48 83 EC ?? "
                    "83 79 ?? ??"
                )
            ),
            Candidate::direct(
                "SuperResolutionExecute_P2_BodyLeaMov",
                Pattern::literal("48 8D 59 ?? 48 8B 41 ?? 48 8B F9 44 8B 70 ??"),
                -0x20
            ),
            Candidate::direct(
                "SuperResolutionExecute_P3_BodyLeaTest",
                Pattern::literal("48 8D 5F ?? 84 C0 [2-6] 41 B5 ?? [2-5] 45 32 ED 83 7F ?? ??"),
                -0x39
            ),
        };

        // StashFromEntity: StashFromEntity: the C_Stash extension of an entity (called). 3 rungs, most specific first.
        inline const Candidate STASH_FROM_ENTITY_CANDIDATES[] = {
            Candidate::direct(
                "StashFromEntity_P1_ExtensionClassLoop",
                Pattern::literal("48 89 54 24 38 80 7A ?? ?? [2-6] 48 8B 0B 48 8B 52 ??"),
                -0x3C
            ),
            Candidate::direct(
                "StashFromEntity_P2_BodyMovCall",
                Pattern::literal("4C 8B 82 ?? ?? ?? ?? 8B D3 41 FF D0 48 8B D8"),
                -0x1E
            ),
            Candidate::direct(
                "StashFromEntity_P3_BodyMovCallTest",
                Pattern::literal("48 8B 41 ?? 48 8B CB FF D0 48 85 C0 [2-6] 48 8D 4C 24 38"),
                -0x4E
            ),
        };

        // StashMasterInventory: StashMasterInventory: the WUID of a stash's master inventory (called). 3 rungs, most
        // specific first.
        inline const Candidate STASH_MASTER_INVENTORY_CANDIDATES[] = {
            Candidate::direct(
                "StashMasterInventory_P1_MasterStashLookup",
                Pattern::literal("48 8B CE 48 8B 06 FF 90 ?? ?? ?? ?? 48 8B 53 ??"),
                -0x37
            ),
            Candidate::direct(
                "StashMasterInventory_P2_BodyMovCall",
                Pattern::literal("48 8B CE FF D2 48 8B 0D ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ??"),
                -0x62
            ),
            Candidate::direct(
                "StashMasterInventory_P3_BodyMovCallAlt",
                Pattern::literal("48 8B D9 E8 ?? ?? ?? ?? 48 8B 88 ?? ?? ?? ?? 48 8B B1 ?? ?? ?? ??"),
                -0x21
            ),
        };

        // InventoryOwner: InventoryOwner: the WUID of an inventory's owner (called). 3 rungs, most specific first.
        inline const Candidate INVENTORY_OWNER_CANDIDATES[] = {
            Candidate::direct(
                "InventoryOwner_P1_OwnerIdCombine",
                Pattern::literal("48 8B CF 48 8B D8 41 FF 50 ?? 48 8B D6 48 8B CB"),
                -0x31
            ),
            Candidate::direct(
                "InventoryOwner_P2_BodyMovCall",
                Pattern::literal("4C 8B C0 E8 ?? ?? ?? ?? 48 8B 5C 24 30 48 8B C6"),
                -0x41
            ),
            Candidate::direct(
                "InventoryOwner_P3_BodyMovCallAlt",
                Pattern::literal("49 8B 88 ?? ?? ?? ?? 48 8B 01 FF 50 ?? 4C 8B 07"),
                -0x21
            ),
        };

        // PublicEnemyTag: The public-enemy reputation tag (data). 3 rungs, most specific first.
        inline const Candidate PUBLIC_ENEMY_TAG_CANDIDATES[] = {
            Candidate::rip_relative(
                "PublicEnemyTag_P1_LeaMovCall",
                Pattern::literal("48 8D 15 ?? ?? ?? ?? 48 8B 08 4C 8B 41 ?? 48 8B C8 41 FF D0 32 DB"),
                3,
                7
            ),
            Candidate::rip_relative(
                "PublicEnemyTag_P2_LeaRelationFlags",
                Pattern::literal("48 8D 15 ?? ?? ?? ?? 48 8B 01 FF 50 ?? 48 8B CF 8A D0"),
                3,
                7
            ),
            Candidate::rip_relative(
                "PublicEnemyTag_P3_LeaRepOutParamCheck",
                Pattern::literal("48 8D 15 ?? ?? ?? ?? BB ?? ?? ?? ?? 48 8B 01 FF 50 ?? 84 C0"),
                3,
                7
            ),
        };

        // ScriptContextMap: The soul script-context name table (data). 3 rungs, most specific first.
        inline const Candidate SCRIPT_CONTEXT_MAP_CANDIDATES[] = {
            Candidate::rip_relative(
                "ScriptContextMap_P1_LeaXorMov",
                Pattern::literal("48 8D 05 ?? ?? ?? ?? 33 D2 48 8D 4D D0 48 89 45 10"),
                3,
                7
            ),
            Candidate::rip_relative(
                "ScriptContextMap_P2_LeaCallXor",
                Pattern::literal("48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 33 C9 48 85 C0"),
                3,
                7
            ),
            Candidate::rip_relative(
                "ScriptContextMap_P3_LeaCallMov",
                Pattern::literal("48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 7C 24 70"),
                3,
                7
            ),
        };

        // ShaderForName: CShaderMan::mfForName (called, the silhouette shader). 3 rungs, most specific first.
        inline const Candidate SHADER_FOR_NAME_CANDIDATES[] = {
            Candidate::direct(
                "ShaderForName_P1_ArgShuffle",
                Pattern::literal("48 8D 45 30 41 8B F0 49 89 43 B0 48 8B F9 4D 89 4B A8 45 8B C8 4C 8B C2 48 8D 55 C0"),
                -0x17
            ),
            Candidate::direct(
                "ShaderForName_P2_ResultRefs",
                Pattern::literal("80 7D D0 00 48 8B 5D C0 [2] 48 89 7D D8 48 89 5D E0 48 85 DB"),
                -0x38
            ),
            Candidate::direct(
                "ShaderForName_P3_ParseRequest",
                Pattern::literal("F0 FF 43 5C 48 8B 45 C8 48 8D 55 D8 48 89 45 E8 48 8B 45 30 48 89 45 F0 89 75 F8"),
                -0x4F
            ),
        };

        // AdjustFileName: CCryPak::AdjustFileName (vtable validator). 3 rungs, most specific first.
        inline const Candidate ADJUST_FILE_NAME_CANDIDATES[] = {
            Candidate::direct(
                "AdjustFileName_P1_AliasArgs",
                Pattern::literal("80 3A 25 [2] 48 3B 9F ?? ?? ?? ?? [2] 44 8B CD 4D 8B C7 49 8B D6 48 8B CF"),
                -0x53
            ),
            Candidate::direct(
                "AdjustFileName_P2_SlashJoin",
                Pattern::literal("41 B0 2F 48 8D 4C 24 40 BA 01 00 00 00 E8 ?? ?? ?? ?? 49 8B D6 48 8D 4C 24 40"),
                -0xA0
            ),
            Candidate::direct(
                "AdjustFileName_P3_RealPathRetry",
                Pattern::literal(
                    "44 8B CD 41 0F BA E9 10 4D 8B C7 48 8B CF E8 ?? ?? ?? ?? 48 8B 8F ?? ?? ?? ?? 48 8B F0"
                ),
                -0xC4
            ),
        };

        // FindEffect: CParticleManager::FindEffect (vtable validator, loot effects). 4 rungs, most specific first.
        inline const Candidate FIND_EFFECT_CANDIDATES[] = {
            Candidate::string_xref(
                "FindEffect_X1_NotFoundXref",
                DMK::scan::StringRefQuery{
                    .text = "Particle effect not found: '%s'%s%s",
                    .return_mode = DMK::scan::XrefReturn::EnclosingFunction,
                }
            ),
            Candidate::direct(
                "FindEffect_P2_EnabledNameTest",
                Pattern::literal("45 8A E1 49 8B E8 48 8B F2 4C 8B F9 38 99 ?? ?? ?? ?? [2-6] 48 85 D2"),
                -0x26
            ),
            Candidate::direct(
                "FindEffect_P3_LibraryDotSplit",
                Pattern::literal("8D 53 2E 48 8B CE FF 15 ?? ?? ?? ?? 48 85 C0 [2-6] 4C 8B C0"),
                -0x70
            ),
            Candidate::direct(
                "FindEffect_P4_LoadLibraryRefind",
                Pattern::literal("49 8B 07 45 33 C9 48 8B 54 24 ?? 45 33 C0 49 8B CF FF 50 ?? 48 8B D6 49 8D 4F F8"),
                -0x91
            ),
        };

        // ProxyLoadParticleEmitter: CRenderProxy::LoadParticleEmitter (called, loot effects). 3 rungs, most specific
        // first.
        inline const Candidate PROXY_LOAD_PARTICLE_EMITTER_CANDIDATES[] = {
            Candidate::direct(
                "ProxyLoadParticleEmitter_P1_Prologue",
                Pattern::literal(
                    "48 89 5C 24 08 89 54 24 10 55 56 57 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 49 8B F9 49 8B D8"
                )
            ),
            Candidate::direct(
                "ProxyLoadParticleEmitter_P2_AttachParams",
                Pattern::literal(
                    "48 8B CB FF 50 ?? 8B 88 84 00 00 00 48 8B 03 89 4D ?? 48 8B CB FF 50 ?? 8B 88 88 00 00 00"
                ),
                -0x44
            ),
            Candidate::direct(
                "ProxyLoadParticleEmitter_P3_PrimeSpawnParams",
                Pattern::literal("8A 45 ?? 88 44 24 30 89 4D ?? 48 85 FF [2-6] 48 8B D7 48 8D 4C 24 30"),
                -0x62
            ),
        };

        // ProxySetSlotLocalTM: CRenderProxy::SetSlotLocalTM (called, loot effects). 3 rungs, most specific first.
        inline const Candidate PROXY_SET_SLOT_LOCAL_TM_CANDIDATES[] = {
            Candidate::direct(
                "ProxySetSlotLocalTM_P1_Prologue",
                Pattern::literal("48 83 EC 78 4D 8B D0 E8 ?? ?? ?? ?? 84 C0 [2-6] 41 0F 10 02 48 89 4C 24 30")
            ),
            Candidate::direct(
                "ProxySetSlotLocalTM_P2_QueueRowCopy",
                Pattern::literal("41 0F 10 4A 10 48 8B 0D ?? ?? ?? ?? 45 33 C9 0F 11 44 24 3C 89 54 24 38"),
                -0x1D
            ),
            Candidate::direct(
                "ProxySetSlotLocalTM_P3_QueueRowStore",
                Pattern::literal("41 0F 10 42 20 C7 44 24 6C 00 00 00 00 0F 11 4C 24 4C C6 44 24 20 00 0F 11 44 24 5C"),
                -0x3A
            ),
        };

        // ProxyFreeSlot: CRenderProxy::FreeSlot (called, loot effects). 3 rungs, most specific first.
        inline const Candidate PROXY_FREE_SLOT_CANDIDATES[] = {
            Candidate::direct(
                "ProxyFreeSlot_P1_Prologue",
                Pattern::literal("85 D2 [2-6] 4C 8B DC 49 89 5B 08 49 89 73 10 57 48 83 EC 30 48 8B D9")
            ),
            Candidate::direct(
                "ProxyFreeSlot_P2_QueueSlotDelete",
                Pattern::literal(
                    "49 89 5B E8 49 8D 53 E8 49 89 73 F0 E8 ?? ?? ?? ?? 48 8B 83 ?? ?? ?? ?? 48 C7 04 F8 00 00 00 00"
                ),
                -0x44
            ),
            Candidate::direct(
                "ProxyFreeSlot_P3_TrimTrailingSlots",
                Pattern::literal("48 FF C8 48 3B F8 [2-6] 48 83 3C F9 00 [2-6] 48 83 83 ?? ?? ?? ?? F8 48 83 EF 01"),
                -0x79
            ),
        };

        // ParticleLoadLibrary: CParticleManager::LoadLibrary(name, XmlNodeRef &, bLoadResources) (vtable validator,
        // the mod's particle library). 4 rungs, most specific first. "System.Default" is referenced twice, so no string
        // rung.
        inline const Candidate PARTICLE_LOAD_LIBRARY_CANDIDATES[] = {
            Candidate::direct(
                "ParticleLoadLibrary_P1_Prologue",
                Pattern::literal(
                    "48 89 5C 24 10 48 89 74 24 18 55 57 41 54 41 56 41 57 48 8B EC 48 83 EC ?? 80 B9 ?? ?? ?? ?? 00"
                )
            ),
            Candidate::direct(
                "ParticleLoadLibrary_P2_EnabledArgs",
                Pattern::literal("80 B9 ?? ?? ?? ?? 00 45 8A E1 49 8B F0 4C 8B FA 48 8B F9"),
                -0x19
            ),
            Candidate::direct(
                "ParticleLoadLibrary_P3_SystemLibraryTest",
                Pattern::literal("48 83 BF ?? ?? ?? ?? 00 [2-6] 48 8D 15 ?? ?? ?? ?? 49 8B CF FF 15 ?? ?? ?? ?? 85 C0"),
                -0x6A
            ),
            Candidate::direct(
                "ParticleLoadLibrary_P4_ChildCount",
                Pattern::literal("48 8B 0E 48 8B 01 FF 90 ?? ?? ?? ?? 33 DB 44 8B F0 85 C0"),
                -0xC9
            ),
        };

        // XmlLoadFromBuffer: CXmlUtils::LoadXmlFromBuffer (vtable validator, the mod's particle library). 3 rungs, most
        // specific first.
        inline const Candidate XML_LOAD_FROM_BUFFER_CANDIDATES[] = {
            Candidate::direct(
                "XmlLoadFromBuffer_P1_Prologue",
                Pattern::literal(
                    "4C 8B DC 49 89 5B 08 49 89 73 10 57 48 83 EC 50 33 C0 49 8D 4B D8 49 8B F9 49 8B F0 48 8B DA"
                )
            ),
            Candidate::direct(
                "XmlLoadFromBuffer_P2_ReadOnlyParser",
                Pattern::literal(
                    "49 89 43 E8 48 8D 05 ?? ?? ?? ?? 49 89 43 D8 48 8D 05 ?? ?? ?? ?? 49 89 43 E0 C6 44 24 20 01"
                ),
                -0x28
            ),
            Candidate::direct(
                "XmlLoadFromBuffer_P3_ParseBufferArgs",
                Pattern::literal(
                    "8A 94 24 ?? ?? ?? ?? E8 ?? ?? ?? ?? 44 8B CF C6 44 24 20 01 4C 8B C6 48 8B D3 48 8B 08"
                ),
                -0x5F
            ),
        };

        // ManagerCreateEmitter: CParticleManager::CreateEmitter(loc, effect, SpawnParams *) (called, free-standing
        // effects). 4 rungs, most specific first.
        inline const Candidate MANAGER_CREATE_EMITTER_CANDIDATES[] = {
            Candidate::direct(
                "ManagerCreateEmitter_P1_Prologue",
                Pattern::literal(
                    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 20 57 41 56 41 57 48 83 EC 30 33 ED 49 8B F1 49 8B F8"
                )
            ),
            Candidate::direct(
                "ManagerCreateEmitter_P2_EmitterListAlloc",
                Pattern::literal("4C 8B FA 4C 8B F1 4D 85 C0 [2-6] 48 8D 99 ?? ?? ?? ?? 48 8B CB"),
                -0x20
            ),
            Candidate::direct(
                "ManagerCreateEmitter_P3_ConstructAddRef",
                Pattern::literal(
                    "4C 8B CE 4D 8B C7 48 8B D7 49 8B CA E8 ?? ?? ?? ?? 48 8B F0 48 89 44 24 ?? F0 FF 40 58"
                ),
                -0x57
            ),
            Candidate::direct(
                "ManagerCreateEmitter_P4_ForceFlagFront",
                Pattern::literal("8B 80 ?? ?? ?? ?? 23 86 ?? ?? ?? ?? 0F BA E0 0E [2-6] 41 83 BE ?? ?? ?? ?? 01"),
                -0x89
            ),
        };

        // EmitterKill: CParticleEmitter::Kill (vtable validator, free-standing effects). 3 rungs, most specific first.
        inline const Candidate EMITTER_KILL_CANDIDATES[] = {
            Candidate::direct(
                "EmitterKill_P1_Prologue",
                Pattern::literal("F3 0F 10 81 ?? ?? ?? ?? 48 8B D1 0F 2F 05 ?? ?? ?? ?? [2-6] B8 ?? ?? ?? ?? 89 41 ??")
            ),
            Candidate::direct(
                "EmitterKill_P2_DeadAgeStore",
                Pattern::literal("B8 ?? ?? ?? ?? 89 41 ?? 89 81 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 33 C9 48 85 C0"),
                -0x14
            ),
            Candidate::direct(
                "EmitterKill_P3_ManagerTail",
                Pattern::literal("89 41 ?? 89 81 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 33 C9 48 85 C0 [2-6] 48 8D 48 F8"),
                -0x19
            ),
        };

        // SceneSetRenderTargets: CSceneRenderPass::SetRenderTargets (called, supersampled mask). 3 rungs, most specific
        // first.
        inline const Candidate SCENE_SET_RENDER_TARGETS_CANDIDATES[] = {
            Candidate::direct(
                "SceneSetRenderTargets_P1_DescLeaMovs",
                Pattern::literal("48 8D 79 ?? 49 8B D9 48 8B F2 48 8B E9 48 8B CF 41 B1 02 33 D2"),
                -0x14
            ),
            Candidate::direct(
                "SceneSetRenderTargets_P2_Slot1Call",
                Pattern::literal("41 B1 02 4C 8B C3 BA 01 00 00 00 48 8B CF E8 ?? ?? ?? ?? 4C 8B 44 24 50 41 B1 02"),
                -0x2E
            ),
            Candidate::direct(
                "SceneSetRenderTargets_P3_DepthCallLea",
                Pattern::literal(
                    "41 B0 02 48 8B D6 48 8B CF E8 ?? ?? ?? ?? 48 8D 8D ?? ?? ?? ?? 48 8B D7 48 8B 5C 24 30"
                ),
                -0x6B
            ),
        };

        // PostFxRenderTarget: SPostEffectsUtils::GetOrCreateRenderTarget (called, supersampled mask). 3 rungs, most
        // specific first.
        inline const Candidate POST_FX_RENDER_TARGET_CANDIDATES[] = {
            Candidate::direct(
                "PostFxRenderTarget_P1_Prologue",
                Pattern::literal(
                    "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 40 41 8B E9 45 8B F0 "
                    "48 8B F2 4C 8B D1"
                )
            ),
            Candidate::direct(
                "PostFxRenderTarget_P2_MipFlagsMask",
                Pattern::literal(
                    "F6 9C 24 ?? ?? ?? ?? BB 01 00 00 00 8B 84 24 ?? ?? ?? ?? 48 8B 0A 1B FF 81 E7 FF 07 00 00 83 E0 F7"
                ),
                -0x37
            ),
            Candidate::direct(
                "PostFxRenderTarget_P3_FlagsOr41000",
                Pattern::literal("83 E0 F7 03 FB 0B F8 81 CF 00 10 04 00 48 85 C9"),
                -0x55
            ),
        };

        // PostFxDepthStencil: SPostEffectsUtils::GetOrCreateDepthStencil (called, supersampled mask). 3 rungs, most
        // specific first.
        inline const Candidate POST_FX_DEPTH_STENCIL_CANDIDATES[] = {
            Candidate::direct(
                "PostFxDepthStencil_P1_Prologue",
                Pattern::literal(
                    "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 40 41 8B F1 41 8B E8 "
                    "48 8B FA 4C 8B D1"
                )
            ),
            Candidate::direct(
                "PostFxDepthStencil_P2_BtrOr40009",
                Pattern::literal("44 8B B4 24 ?? ?? ?? ?? BB 01 00 00 00 48 8B 0A 41 0F BA F6 0C 41 81 CE 09 00 04 00"),
                -0x37
            ),
            Candidate::direct(
                "PostFxDepthStencil_P3_BtrOrTest",
                Pattern::literal("41 0F BA F6 0C 41 81 CE 09 00 04 00 48 85 C9"),
                -0x47
            ),
        };

        // ClearDepth: CClearSurfacePass::Execute, the depth-stencil overload (called, supersampled mask). 3 rungs, most
        // specific first.
        inline const Candidate CLEAR_DEPTH_CANDIDATES[] = {
            Candidate::direct(
                "ClearDepth_P1_FrameArgs",
                Pattern::literal(
                    "48 81 EC A0 00 00 00 0F 29 70 E8 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 ?? ?? ?? ?? 48 8B DA "
                    "41 8B F8"
                ),
                -0xC
            ),
            Candidate::direct(
                "ClearDepth_P2_MemsetDesc58",
                Pattern::literal("48 8B DA 41 8B F8 33 D2 48 8B F1 48 8D 4C 24 20 0F 28 F3 44 8D 42 58"),
                -0x29
            ),
            Candidate::direct(
                "ClearDepth_P3_DescStores",
                Pattern::literal("C6 44 24 20 02 48 89 5C 24 30 C6 44 24 38 02 89 7C 24 3C"),
                -0x5F
            ),
        };
    } // namespace aob

    /**
     * @brief Stable identity for every game-image anchor the mod resolves at startup.
     * @details Indexes the declarative anchor table and the resolved-address store. The
     *          enumerator order IS the table order; Count is the element count and is not a valid anchor. The first
     *          EARLY_ANCHOR_COUNT anchors form the early phase: they are resolved and hooked before anything else so
     *          the custom stage can be registered before the renderer builds its standard pipeline.
     */
    enum class AnchorId : std::size_t
    {
        /// CStandardGraphicsPipeline::Init (hooked, early phase).
        StdPipelineInit,
        /// CSceneCustomStage::CreatePipelineState (hooked, early phase).
        CustomCreatePso,
        /// CGraphicsPipeline::RegisterStage<CSceneCustomStage> (called, early phase).
        RegisterCustomStage,
        /// SSystemGlobalEnvironment base (data).
        Genv,
        /// Global-context storage slot (data).
        Context,
        /// CSceneForwardStage::ExecuteAfterPostProcessHDR (hooked).
        AfterPostHdr,
        /// CClearSurfacePass::Execute (called).
        ClearSurface,
        /// CSceneRenderPass::PrepareRenderPassForUse (called).
        PrepareRenderPass,
        /// CSceneRenderPass::DrawRenderItems (called).
        DrawRenderItems,
        /// CRenderItemDrawer::JobifyDrawSubmission (called).
        JobifyDraws,
        /// CRenderItemDrawer::WaitForDrawSubmission (called).
        WaitDraws,
        /// CRenderPrimitive::SetTechnique (called).
        SetTechnique,
        /// CPrimitiveRenderPass::SetRenderTarget implementation, behind the thunk that adds 0x58 to the pass (called).
        SetRenderTarget,
        /// CDeviceResourceSetDesc::SetTexture (called).
        SetTexture,
        /// CDeviceResourceSetDesc::SetSampler (called).
        SetSampler,
        /// CFullscreenPass::BeginConstantUpdate (called).
        BeginConstantUpdate,
        /// CFullscreenPass::SetConstant (called).
        SetConstant,
        /// CFullscreenPass::Execute (called).
        FullscreenExecute,
        /// CCryNameR constructor (called).
        CryNameR,
        /// GetDisplayTargetDst (called).
        DisplayTargetDst,
        /// Core graphics command-list slot (data).
        CoreCommandListSlot,
        /// CSceneRenderPass::s_recursionCounter (data).
        RecursionCounter,
        /// CShaderMan::s_shPostEffectsGame slot (data).
        PostEffectsGameSlot,
        /// CRenderProxy::Render (hooked).
        ProxyRender,
        /// CCryAction::PostUpdate (hooked, main-thread tick).
        PostUpdate,
        /// C3DEngine::RegisterEntity (vtable validator).
        RegisterEntity,
        /// C3DEngine::UnRegisterEntityDirect implementation (vtable validator).
        UnRegisterEntity,
        /// CEntitySystem::GetEntity (vtable validator).
        GetEntity,
        /// CEntitySystem::GetEntityIterator (vtable validator).
        GetEntityIterator,
        /// CEntity::GetProxy (vtable validator).
        GetProxy,
        /// CEntity::GetWorldBounds (vtable validator).
        GetWorldBounds,
        /// CD3D9Renderer::GetIRenderAuxGeom (vtable validator).
        GetAuxGeom,
        /// CAuxGeomCB::SetRenderFlags (vtable validator).
        AuxSetFlags,
        /// CAuxGeomCB::GetRenderFlags (vtable validator).
        AuxGetFlags,
        /// CAuxGeomCB::DrawLines (vtable validator).
        AuxDrawLines,
        /// C3DEngine::GetObjectsInBox (called).
        GetObjectsInBox,
        /// CObjManager instance slot (data).
        ObjManager,
        /// CSceneForwardStage::ExecuteAfterPostProcessLDR (hooked, P5).
        AfterPostLdr,
        /// The LDR display target helper (called).
        LdrTarget,
        /// CStatObj::RenderInternal (hooked, P3b).
        StatObjRenderInternal,
        /// CStatObj::Render (called, herb outlines).
        StatObjRender,
        /// CGaussianBlurPass constructor (called, mask blur).
        BlurPassCtor,
        /// CGaussianBlurPass::Execute (called, mask blur).
        BlurPassExecute,
        /// CSuperResolutionStage::Execute (hooked, P6).
        SuperResolutionExecute,
        /// StashFromEntity: the C_Stash extension of an entity (called).
        StashFromEntity,
        /// StashMasterInventory: the WUID of a stash's master inventory (called).
        StashMasterInventory,
        /// InventoryOwner: the WUID of an inventory's owner (called).
        InventoryOwner,
        /// The public-enemy reputation tag (data).
        PublicEnemyTag,
        /// The soul script-context name table (data).
        ScriptContextMap,
        /// CShaderMan::mfForName (called, the silhouette shader).
        ShaderForName,
        /// CCryPak::AdjustFileName (vtable validator).
        AdjustFileName,
        /// CParticleManager::FindEffect (vtable validator, loot effects).
        FindEffect,
        /// CRenderProxy::LoadParticleEmitter (called, loot effects).
        ProxyLoadParticleEmitter,
        /// CRenderProxy::SetSlotLocalTM (called, loot effects).
        ProxySetSlotLocalTM,
        /// CRenderProxy::FreeSlot (called, loot effects).
        ProxyFreeSlot,
        /// CParticleManager::LoadLibrary from an XML node (vtable validator, the mod's particle library).
        ParticleLoadLibrary,
        /// CXmlUtils::LoadXmlFromBuffer (vtable validator, the mod's particle library).
        XmlLoadFromBuffer,
        /// CParticleManager::CreateEmitter (called, free-standing effects).
        ManagerCreateEmitter,
        /// CParticleEmitter::Kill (vtable validator, free-standing effects).
        EmitterKill,
        /// CSceneRenderPass::SetRenderTargets (called, supersampled mask).
        SceneSetRenderTargets,
        /// SPostEffectsUtils::GetOrCreateRenderTarget (called, supersampled mask).
        PostFxRenderTarget,
        /// SPostEffectsUtils::GetOrCreateDepthStencil (called, supersampled mask).
        PostFxDepthStencil,
        /// CClearSurfacePass::Execute, the depth-stencil overload (called, supersampled mask).
        ClearDepth,
        /// Enumerator count. Not an anchor.
        Count,
    };

    /// Number of anchors in the registry.
    inline constexpr std::size_t ANCHOR_COUNT = static_cast<std::size_t>(AnchorId::Count);

    /// Number of leading anchors resolved by the early phase (resolve_early_anchors).
    inline constexpr std::size_t EARLY_ANCHOR_COUNT = static_cast<std::size_t>(AnchorId::Genv);

    /**
     * @brief A mod feature gated on the anchors it depends on.
     * @details The enumerator order IS the order of the feature table in aob_resolver.cpp.
     */
    enum class Feature : std::size_t
    {
        /// gEnv, which everything engine-facing reads through.
        Core,
        /// The global context the dialogue, combat and minigame gates read.
        GameState,
        /// Entity lookup and its render proxy: every highlight.
        EntityLookup,
        /// Entity world bounds (markers and interactable sizes).
        EntityBounds,
        /// The entity-system iterator (the container index walk).
        EntityIteration,
        /// Render-node re-registration (the see-through refresh).
        RenderRegistration,
        /// Stash lookup and its master inventory (container rules).
        Stashes,
        /// An inventory's owner (stolen-item rules).
        InventoryOwner,
        /// The public-enemy tag (the Hostile tag).
        PublicEnemy,
        /// The soul script-context table (shop and ownership rules).
        ScriptContexts,
        /// The 3D-engine octree query (interactables and static objects).
        Octree,
        /// The octree query and the CObjManager slot (herbs).
        HerbScan,
        /// The main-thread tick.
        Tick,
        /// The aux-geometry marker path.
        AuxMarkers,
        /// The early silhouette stage registration (P1, P2).
        StageRegistration,
        /// The silhouette mask draw.
        MaskDraw,
        /// The silhouette composite.
        Composite,
        /// The render-proxy word injection (P3).
        ProxyInjection,
        /// Static-mesh marking (P3b).
        BrushMarking,
        /// The after-HDR mask and composite point (P4).
        AfterHdr,
        /// The composite before the upscaler (P6).
        BeforeUpscale,
        /// The after-LDR composite point (P5).
        AfterLdr,
        /// The mask blur ([Render] Softness).
        MaskBlur,
        /// Herb outlines (CStatObj::Render).
        HerbOutline,
        /// The mod's own composite shader (CShaderMan::mfForName, CCryPak::AdjustFileName).
        SilhouetteShader,
        /// Particle effects attached to highlighted loot (a group's Effect).
        LootEffects,
        /// The mod's own particle library (KCD2_HenrySenses.particles.xml, effects HenrySenses.*).
        EffectLibrary,
        /// Free-standing effects on objects without an entity slot (interactables, herbs, static objects).
        WorldEffects,
        /// The supersampled silhouette mask (the mod's own render targets, drawn into by the mask pass).
        SupersampledMask,
        /// Enumerator count. Not a feature.
        Count,
    };

    /// Number of feature gates.
    inline constexpr std::size_t FEATURE_COUNT = static_cast<std::size_t>(Feature::Count);

    /**
     * @brief Resolves the early-phase anchors (the stage-registration hooks) and records the results.
     * @details Loads the signature file and resolves the first EARLY_ANCHOR_COUNT
     *          anchors, so the pipeline-Init and CreatePipelineState hooks can be armed before the full table (which
     *          takes much longer) has finished. Publishes the gates whose anchors all lie in this phase.
     * @param module_base WHGame.dll base address.
     * @param module_size WHGame.dll image size.
     * @note Setup and control plane only. Call once from init(), before resolve_all_anchors().
     */
    void resolve_early_anchors(std::uintptr_t module_base, std::size_t module_size);

    /**
     * @brief Resolves every remaining anchor in one parallel pass, records the results and publishes the gates.
     * @details Confined to the WHGame.dll image [module_base, module_base + module_size). A miss records 0 and its
     *          features fail their gates. With [Settings] ExportSignatures, the built-in signatures are then written
     *          to the export file.
     * @param module_base WHGame.dll base address.
     * @param module_size WHGame.dll image size.
     * @note Setup and control plane only: allocates and spawns a transient worker pool. Call once from init().
     */
    void resolve_all_anchors(std::uintptr_t module_base, std::size_t module_size);

    /**
     * @brief Returns the resolved absolute address for an anchor, or 0 if it did not resolve.
     * @note Callback-safe: a bounds check and an array read. Enable a feature through feature_ready(), not through
     *       this address.
     */
    [[nodiscard]] std::uintptr_t anchor_address(AnchorId id) noexcept;

    /**
     * @brief Returns the label of an anchor, for log lines.
     * @return The static label; "?" for an out-of-range id.
     */
    [[nodiscard]] const char *anchor_label(AnchorId id) noexcept;

    /**
     * @brief Returns the retained per-anchor resolution report (the drift report).
     * @return The report span; empty before resolve_early_anchors() has run.
     * @note The entries live in static storage for the process lifetime; the span never dangles.
     */
    [[nodiscard]] std::span<const DMK::anchor::ResolvedAnchor> anchor_report() noexcept;

    /**
     * @brief Returns a feature's gate verdict (anchor::evaluate_gate over its anchors, the fail-closed default).
     * @return Fail before the phase of its anchors has run.
     * @note Callback-safe: two atomic loads.
     */
    [[nodiscard]] DMK::anchor::GateVerdict feature_gate(Feature feature) noexcept;

    /**
     * @brief Returns whether a feature may run: its gate did not fail.
     * @note Callback-safe.
     */
    [[nodiscard]] bool feature_ready(Feature feature) noexcept;

    /**
     * @brief Returns an anchor's address when @p feature is ready, else 0.
     * @note Callback-safe.
     */
    [[nodiscard]] std::uintptr_t gated_anchor_address(Feature feature, AnchorId id) noexcept;

    /**
     * @brief Returns a feature's name, for log lines.
     * @return The static name; "?" for an out-of-range feature.
     */
    [[nodiscard]] const char *feature_name(Feature feature) noexcept;

} // namespace HenrySenses

#endif // HENRYSENSES_AOB_RESOLVER_HPP
