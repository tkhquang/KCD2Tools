/**
 * @file code_proofs.hpp
 * @brief Proofs that code the mod reaches through live data, a vtable slot or a call's target, is the function it
 *        expects, checked before the mod calls or hooks it.
 * @details An anchor (aob_resolver.hpp) is found by scanning the game image, so each of its candidates must match once
 *          there. A proof is matched at one known address instead and needs no uniqueness. Each proof is a list of
 *          shapes tried in order (code_proven() and read_proven_vtable_slot() in hooks/vtable_slots.hpp). A shape
 *          matched at the target's first byte proves the whole entry. A shape searched within a window matches where
 *          the code sits in the function, and one that starts past the first 14 bytes, the most an inline hook
 *          overwrites, still proves a target whose entry another mod hooked. A function too short for a second shape,
 *          such as an 8-byte getter, has one. Every shape matches on Steam 1.5.6, GOG 1.5 and Game Pass 1.4.
 */
#ifndef TPVCAMERA_CODE_PROOFS_HPP
#define TPVCAMERA_CODE_PROOFS_HPP

#include <DetourModKit/scan.hpp>

#include <cstddef>

namespace TPVCamera::Proof
{
    using DMK::scan::Pattern;

    /** @brief One shape of a proof: a pattern and how far from the target's first byte its match may start. */
    struct Shape
    {
        Pattern pattern;
        /// The bytes from the target's first byte the whole match must lie within.
        std::size_t window;

        /** @brief The shape matched at the target's first byte. */
        [[nodiscard]] static constexpr Shape at_entry(const Pattern &pattern) noexcept
        {
            return {pattern, pattern.max_match_length()};
        }

        /** @brief The shape searched within the first @p window bytes of the target. */
        [[nodiscard]] static constexpr Shape within(const Pattern &pattern, std::size_t window) noexcept
        {
            return {pattern, window};
        }
    };

    // C_Player's IsThirdPerson (native_turn_hooks.cpp) asks the active camera: `mov rdx, [rcx+disp8]; mov rcx, rax;
    // call rdx`, within the first 0x50 bytes.
    inline constexpr Shape k_isThirdPerson[] = {
        Shape::within(Pattern::literal("48 8B 51 ?? 48 8B C8 FF D2"), 0x50),
    };

    // C_CameraObserver's update (ai_observer.cpp) fetches the view camera through ISystem (`call [rax+disp32]`, whose
    // ISystem slot differs between builds) and copies its translation column (`[rax+0Ch]`, `[rax+1Ch]`, `[rax+2Ch]`)
    // into its position out-parameter, within the first 0x40 bytes.
    inline constexpr Shape k_cameraObserverUpdate[] = {
        Shape::within(Pattern::literal("48 8B 01 FF 90 ?? ?? ?? ?? 8B 48 0C F3 0F 10 40 1C F3 0F 10 48 2C"), 0x40),
    };

    // CAnimationSet's GetAnimIDByCRC slot (native_turn_hooks.cpp) holds a thunk: `add rcx, imm8` (to the name map),
    // then `jmp rel32` (to the map lookup).
    inline constexpr Shape k_animIdByCrcThunk[] = {
        Shape::at_entry(Pattern::literal("48 83 C1 ?? E9")),
    };

    // The animation-name hash (native_turn_hooks.cpp), the target of the AnimNameHashCall anchor's call: the register
    // saves and pushes, the frame, `mov edi, edx` (the length), `mov rbp, rcx` (the name), then `cmp edx, 20h` (its
    // first branch on the length).
    inline constexpr Shape k_animNameHash[] = {
        Shape::at_entry(Pattern::literal(
            "48 89 5C 24 08 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 83 EC ?? 8B FA 48 8B E9 83 FA 20")),
    };

    // The player's skeleton getters (character_pose.cpp). CCharInstance's GetISkeletonPose is `lea rax, [rcx+disp32];
    // ret` and its GetIDefaultSkeleton `mov rax, [rcx+disp32]; ret`. The whole getter is those eight bytes, so each has
    // one shape.
    inline constexpr Shape k_charGetSkeletonPose[] = {
        Shape::at_entry(Pattern::literal("48 8D 81 ?? ?? ?? ?? C3")),
    };
    inline constexpr Shape k_charGetDefaultSkeleton[] = {
        Shape::at_entry(Pattern::literal("48 8B 81 ?? ?? ?? ?? C3")),
    };

    // CSkeletonPose's GetAbsJointByID (character_pose.cpp). The first shape, after the 10-byte frame, takes the pose
    // data (`lea rbx, [rcx+disp32]`), checks the index against the joint count (`call [rax+8]`) and returns the
    // absolute joint (`call [rax+50h]`, where the relative one calls +10h). The second is that call, the static
    // identity an out-of-range index gets (`lea rax, [rip+disp32]`) and the epilogue.
    inline constexpr Shape k_skeletonPoseAbsJoint[] = {
        Shape::within(Pattern::literal("48 8D 99 ?? ?? ?? ?? 8B FA 85 D2 78 ?? 48 8B 03 48 8B CB FF 50 08 3B F8 73 ?? "
                                       "48 8B 03 8B D7 48 8B CB FF 50 50"),
                      0x30),
        Shape::within(Pattern::literal("FF 50 50 EB ?? 48 8D 05 ?? ?? ?? ?? 48 8B 5C 24 ?? 48 83 C4 ?? 5F C3"), 0x48),
    };

    // CDefaultSkeleton's GetJointIDByCRC32 (character_pose.cpp). The first shape is its start: the name table
    // (`mov rcx, [rcx+10h]`) and the hash (`imul eax, edx, 0Dh`). The second is the bucket probe: the hash masked to
    // 0x1FF, the bucket's first index (`movzx r8d, word [rcx+rax*2]`) and the CRC compare (`cmp [rcx+rax*4+4], edx`).
    inline constexpr Shape k_jointIdByCrc[] = {
        Shape::at_entry(Pattern::literal("48 8B 49 10 48 85 C9 74 ?? 6B C2 0D")),
        Shape::within(Pattern::literal("6B C2 0D [8-14] 25 FF 01 00 00 44 0F B7 04 41 [16-28] 39 54 81 04"), 0x40),
    };

    // CXConsole's RegisterInt (camera_view_signal.cpp): its frame, the variable map (`add rcx, 0C0h`), and the flags
    // and 32-bit value kept in r14d and r15d (`mov r14d, r9d; mov r15d, r8d`). The second shape is the new variable's
    // value store (`mov [rbx+48h], r15d`) beside its CXConsoleVariableInt vtable store.
    inline constexpr Shape k_consoleRegisterInt[] = {
        Shape::at_entry(Pattern::literal("48 89 5C 24 18 48 89 54 24 10 55 56 57 41 56 41 57 48 83 EC 30 48 8B EA 48 "
                                         "8B F9 48 81 C1 C0 00 00 00 48 8D 54 24 ?? 45 8B F1 45 8B F8 E8")),
        Shape::within(Pattern::literal("48 8D 05 ?? ?? ?? ?? 44 89 7B 48 48 89 03"), 0xD0),
    };

    // CXConsole's GetCVar (camera_view_signal.cpp): its frame and the report of a null name (`call [rax+38h]`). The
    // second shape is the map lookup (`lea rcx, [rsi+0C0h]; call rel32`) and the name compare (`mov rdx, [rbx+20h];
    // mov rcx, rdi; call [rip+disp32]`).
    inline constexpr Shape k_consoleGetCVar[] = {
        Shape::at_entry(Pattern::literal("48 89 5C 24 08 48 89 74 24 18 48 89 54 24 10 57 48 83 EC 40 48 8B FA 48 8B "
                                         "F1 48 85 D2 75 ?? 48 8B 0D ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8B 01 FF 50 "
                                         "38")),
        Shape::within(Pattern::literal("48 8D 8E C0 00 00 00 E8 ?? ?? ?? ?? 48 8B 5C 24 ?? 80 7B 19 00 75 ?? 48 8B 53 "
                                       "20 48 8B CF FF 15"),
                      0xD0),
    };

    // CXConsoleVariableInt's GetIVal (`mov eax, [rcx+48h]; ret`) and GetI64Val (`movsxd rax, dword [rcx+48h]; ret`)
    // (camera_view_signal.cpp), each the whole function, each proving the value field at CVAR_INT_VALUE_OFFSET.
    inline constexpr Shape k_cvarIntGetIVal[] = {
        Shape::at_entry(Pattern::literal("8B 41 48 C3")),
    };
    inline constexpr Shape k_cvarIntGetI64Val[] = {
        Shape::at_entry(Pattern::literal("48 63 41 48 C3")),
    };

    // C_UIOverlay's SetAlpha(layer, alpha) (screen_effects.cpp) spills the int alpha (`mov [rsp+18h], r8d`), where
    // HideOverlays and ShowOverlays spill a byte. Its body passes the layer (`[rsp+38h]`) and the alpha (`[rsp+40h]`)
    // to the movie call: `lea r9, [rsp+40h]; mov rcx, rbx; lea r8, [rsp+38h]; lea rdx, [rsp+48h]; call rel32`. Both
    // shapes match once in the image and at no neighbouring slot.
    inline constexpr Shape k_uiOverlaySetAlpha[] = {
        Shape::at_entry(Pattern::literal(
            "44 89 44 24 18 53 48 83 EC 20 0F B6 C2 48 8B D9 48 8D 15 ?? ?? ?? ?? 89 44 24 38 48 8D 4C 24 48 E8")),
        Shape::within(Pattern::literal("89 44 24 38 48 8D 4C 24 48 E8 ?? ?? ?? ?? 4C 8D 4C 24 40 48 8B CB 4C 8D 44 24 "
                                       "38 48 8D 54 24 48 E8"),
                      0x40),
    };

    // The particle manager's FindEffect(name, source, bLoadResources) (screen_effects.cpp) opens with its 0x270-byte
    // frame and the security cookie. Its body keeps bLoadResources and the name (`mov r12b, r9b; mov rsi, rdx`),
    // refuses a disabled manager (`cmp [rcx+1C8h], bl`) and an empty name, then steps back to the manager
    // (`add rcx, -8`) and calls the loaded-effect lookup. Both shapes match once in the image and at no neighbouring
    // slot.
    inline constexpr Shape k_particleFindEffect[] = {
        Shape::at_entry(Pattern::literal("40 53 55 56 57 41 54 41 56 41 57 48 81 EC 70 02 00 00 48 8B 05 ?? ?? ?? ?? "
                                         "48 33 C4 48 89 84 24 60 02 00 00 33 DB 45 8A")),
        Shape::within(Pattern::literal("33 DB 45 8A E1 49 8B E8 48 8B F2 4C 8B F9 38 99 C8 01 00 00 0F 84 ?? ?? ?? ?? "
                                       "48 85 D2 0F 84 ?? ?? ?? ?? 38 1A 0F 84 ?? ?? ?? ?? 48 83 C1 F8 E8"),
                      0x60),
    };

    // The particle manager's enable-by-name slot (screen_effects.cpp) steps back to the manager, looks up the loaded
    // effect, and calls the effect's SetEnabled with the flag (`mov dl, bl; mov r9, [rcx+50h]; call r9`, effect
    // vtable slot 10). That call and the epilogue are its second shape. Both match once in the image and at no
    // neighbouring slot.
    inline constexpr Shape k_particleSetEffectEnabled[] = {
        Shape::at_entry(Pattern::literal("40 53 48 83 EC 20 48 83 C1 F8 41 8A D8 E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8B "
                                         "08 8A D3 4C 8B 49 50 48 8B C8 41 FF D1")),
        Shape::within(Pattern::literal("48 85 C0 74 ?? 48 8B 08 8A D3 4C 8B 49 50 48 8B C8 41 FF D1 48 83 C4 20 5B C3"),
                      0x30),
    };

    // C3DEngine's SetPostEffectParam (screen_effects.cpp) refuses a null name, then forwards to the post-effect group
    // manager's slot 0x588 and returns: `test rcx, rcx; jz; mov rax, [rcx]; call [rax+588h]; add rsp, 28h; ret`. Both
    // shapes match once in the image and at no neighbouring slot.
    inline constexpr Shape k_engineSetPostEffectParam[] = {
        Shape::at_entry(
            Pattern::literal("48 83 EC 28 48 85 D2 74 ?? 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 ?? 48 8B 01 FF 90 "
                             "88 05 00 00 48 83 C4 28 C3")),
        Shape::within(Pattern::literal("48 85 C9 74 ?? 48 8B 01 FF 90 88 05 00 00 48 83 C4 28 C3"), 0x28),
    };

    // The cutscene gate's two getters (game_state.cpp), each the whole function. CCryAction's GetIViewSystem
    // (`mov rax, [rcx+disp32]; ret`): the disp32 at k_frameworkGetViewSystemMember is the CCryAction member holding
    // the CViewSystem. CViewSystem's IsPlayingCutScene (`cmp dword [rcx+disp8], 0; seta al; ret`): the disp8 at
    // k_viewSystemIsPlayingCutSceneCount is the running cutscene count. Both offsets hold only because each proof has
    // its one shape at the entry.
    inline constexpr Shape k_frameworkGetViewSystem[] = {
        Shape::at_entry(Pattern::literal("48 8B 81 ?? ?? ?? ?? C3")),
    };
    inline constexpr std::ptrdiff_t k_frameworkGetViewSystemMember = 3;
    inline constexpr Shape k_viewSystemIsPlayingCutScene[] = {
        Shape::at_entry(Pattern::literal("83 79 ?? 00 0F 97 C0 C3")),
    };
    inline constexpr std::ptrdiff_t k_viewSystemIsPlayingCutSceneCount = 2;

    // The setter the set-opponent wrapper calls (combat_retarget.cpp): the prologue through the target slot load, or
    // the two loads alone (`mov r8, [rax+210h]`, `mov rdi, [r8+0CF0h]`, the state block and the target slot the mod
    // reads) within its first 0x30 bytes.
    inline constexpr Shape k_setCombatOpponentSetter[] = {
        Shape::at_entry(Pattern::literal("48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC 30 48 8B 41 08 48 8B F2 48 8B D9 "
                                         "4C 8B 80 10 02 00 00 49 8B B8 F0 0C 00 00")),
        Shape::within(Pattern::literal("4C 8B 80 10 02 00 00 49 8B B8 F0 0C 00 00"), 0x30),
    };

    // C_CombatPlayer's per-frame update (combat_retarget.cpp; sub_181002DD4, GOG 0x180FFFE24, Game Pass 0x18075B5B8),
    // checked before it is hooked: the whole body (the frame, `mov rbx, rcx`, the lock-on controller load
    // `mov rcx, [rcx+450h]`, the call of its update, and the tail jump), or the controller load and the call alone,
    // a bounded gap apart, within its first 0x30 bytes.
    inline constexpr Shape k_combatPlayerUpdate[] = {
        Shape::at_entry(Pattern::literal("40 53 48 83 EC 30 48 8B D9 0F 29 74 24 20 48 8B 89 50 04 00 00 0F 28 F1 E8 "
                                         "?? ?? ?? ?? 0F 28 CE 48 8B CB 0F 28 74 24 20 48 83 C4 30 5B E9")),
        Shape::within(Pattern::literal("48 8B 89 50 04 00 00 [0-8] E8"), 0x30),
    };
} // namespace TPVCamera::Proof

#endif // TPVCAMERA_CODE_PROOFS_HPP
