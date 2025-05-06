/**
 * @file constants.h
 * @brief Central definitions for constants used throughout the mod.
 *
 * Includes version info, filenames, default settings, AOB patterns, and memory offsets.
 * All hardcoded memory addresses have been replaced with AOB patterns for robustness.
 */
#define _USE_MATH_DEFINES
#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <string>
#include <math.h>
#include "version.h" // Mod versioning definitions

/**
 * @namespace Constants
 * @brief Encapsulates global constants and config defaults.
 */
namespace Constants
{
    // Version information derived from version.h
    constexpr const char *MOD_VERSION = Version::VERSION_STRING;
    constexpr const char *MOD_NAME = Version::MOD_NAME;
    constexpr const char *MOD_WEBSITE = Version::REPOSITORY;

    // File extensions
    constexpr const char *INI_FILE_EXTENSION = ".ini";
    constexpr const char *LOG_FILE_EXTENSION = ".log";

    /** @brief Gets the INI config filename (e.g., "KCD2_TPVToggle.ini"). */
    inline std::string getConfigFilename()
    {
        return std::string(MOD_NAME) + INI_FILE_EXTENSION;
    }
    /** @brief Gets the base log filename (e.g., "KCD2_TPVToggle.log"). */
    inline std::string getLogFilename()
    {
        return std::string(MOD_NAME) + LOG_FILE_EXTENSION;
    }

    // --- Default Configuration Values ---
    /** @brief Default logging level ("INFO"). */
    constexpr const char *DEFAULT_LOG_LEVEL = "INFO";

    // --- AOB (Array-of-Bytes) Patterns ---

    // WHGame.DLL+A27E07 - 7F 0D                 - jg WHGame.DLL+A27E16
    // WHGame.DLL+A27E09 - 48 8B 05 18EA9B04     - mov rax,[WHGame.DLL+53E6828] { (1D22B1887A0) }
    // WHGame.DLL+A27E10 - 48 83 C4 20           - add rsp,20 { 32 }
    // WHGame.DLL+A27E14 - 5B                    - pop rbx
    // WHGame.DLL+A27E15 - C3                    - ret
    /**
     * @brief AOB pattern to find code near the return path of the function
     *        that loads the Global Context Pointer. This provides access to the
     *        TPV flag data structure.
     */
    constexpr const char *CONTEXT_PTR_LOAD_AOB_PATTERN =
        "7F ?? 48 8B 05 ?? ?? ?? ?? 48 83 C4 20 5B C3";

    // WHGame.DLL+A50976 - 48 83 BB D8000000 00  - cmp qword ptr [rbx+000000D8],00 { 0 }
    // WHGame.DLL+A5097E - 77 27                 - ja WHGame.DLL+A509A7
    // WHGame.DLL+A50980 - 48 8B CB              - mov rcx,rbx
    /**
     * @brief AOB pattern for the overlay check (`cmp qword ptr [rbx+D8h],0`).
     *        Used to find the code that checks the overlay status.
     */
    constexpr const char *OVERLAY_CHECK_AOB_PATTERN =
        "48 83 BB D8 00 00 00 00 77 ?? 48 8B CB";

    // WHGame.DLL+3602F0 - 48 8B C4              - mov rax,rsp
    // WHGame.DLL+3602F3 - 48 89 58 08           - mov [rax+08],rbx
    // WHGame.DLL+3602F7 - 48 89 70 10           - mov [rax+10],rsi
    // WHGame.DLL+3602FB - 48 89 78 18           - mov [rax+18],rdi
    // WHGame.DLL+3602FF - 55                    - push rbp
    // WHGame.DLL+360300 - 41 54                 - push r12
    // WHGame.DLL+360302 - 41 57                 - push r15
    // WHGame.DLL+360304 - 48 8B EC              - mov rbp,rsp
    // WHGame.DLL+360307 - 48 83 EC 70           - sub rsp,70 { 112 }
    // WHGame.DLL+36030B - 33 F6                 - xor esi,esi
    /**
     * @brief AOB pattern for TPV FOV calculation function entry.
     *        Targets the function that updates the view's field of view.
     */
    constexpr const char *TPV_FOV_CALCULATE_AOB_PATTERN =
        "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 ?? ?? ?? ?? ?? 48 8B EC 48 83 EC ?? 33 F6";

    // WHGame.DLL+6192FA - 48 8B 15 4F98DC04     - mov rdx,[WHGame.DLL+53E2B50] { (1D22DCA7C40) }
    // WHGame.DLL+619301 - 48 8B CB              - mov rcx,rbx
    // WHGame.DLL+619304 - C7 42 14 08000000     - mov [rdx+14],00000008 { 8 }
    // WHGame.DLL+61930B - 66 0F6E 83 D0000000   - movd xmm0,[rbx+000000D0]
    // WHGame.DLL+619313 - 0F5B C0               - cvtdq2ps xmm0,xmm0
    // WHGame.DLL+619316 - F3 0F11 42 1C         - movss [rdx+1C],xmm0
    /**
     * @brief AOB pattern for finding the scroll state base address.
     *        Locates the instruction that references the global scroll accumulator structure.
     */
    constexpr const char *SCROLL_STATE_BASE_AOB_PATTERN =
        "48 8B 15 ?? ?? ?? ?? 48 8B CB C7 42 14 ?? ?? ?? ?? 66 0F 6E 83 ?? ?? ?? ?? 0F 5B C0 F3 0F 11 42 1C";

    // WHGame.DLL+619316 - F3 0F11 42 1C         - movss [rdx+1C],xmm0
    // WHGame.DLL+61931B - E8 30F0FFFF           - call WHGame.DLL+618350
    /**
     * @brief AOB pattern for the accumulator write instruction to be NOPed.
     *        Used for scroll wheel input filtering when overlays are active.
     */
    constexpr const char *ACCUMULATOR_WRITE_AOB_PATTERN =
        "F3 0F 11 42 1C E8 ?? ?? ?? ??";
    constexpr int ACCUMULATOR_WRITE_HOOK_OFFSET = 0;     // Hook starts at movss
    constexpr size_t ACCUMULATOR_WRITE_INSTR_LENGTH = 5; // Size of 'movss [rdx+1c], xmm0'

    /**
     * @brief AOB pattern for the event handler function that processes input events.
     *        Used to intercept and filter scroll wheel events during overlay display.
     */
    constexpr const char *EVENT_HANDLER_AOB_PATTERN =
        "48 89 5C 24 10 48 89 74 24 18 55 57 41 54 41 56 41 57 48 8B EC 48 83 EC 60 48 8D 99";

    // AOB for FUN_183924908 (TPV Camera Input Processing)
    constexpr const char *TPV_INPUT_PROCESS_AOB_PATTERN = "48 8B C4 48 89 58 08 48 89 78 10 55 48 8B EC 48 83 EC 70 80 3A 01";
    // AOB for FUN_18036059c (Player State Copy Function)
    constexpr const char *PLAYER_STATE_COPY_AOB_PATTERN = "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 20 49 8B 01";

    // AOB for TPV Camera Update Function
    // WHGame.DLL+392509C - 48 8B C4              - mov rax,rsp
    // WHGame.DLL+392509F - 48 89 58 08           - mov [rax+08],rbx
    // WHGame.DLL+39250A3 - 48 89 70 10           - mov [rax+10],rsi
    // WHGame.DLL+39250A7 - 48 89 78 18           - mov [rax+18],rdi
    // WHGame.DLL+39250AB - 55                    - push rbp
    // WHGame.DLL+39250AC - 41 56                 - push r14
    // WHGame.DLL+39250AE - 41 57                 - push r15
    // WHGame.DLL+39250B0 - 48 8D 68 A1           - lea rbp,[rax-5F]
    // WHGame.DLL+39250B4 - 48 81 EC D0000000     - sub rsp,000000D0 { 208 }
    // WHGame.DLL+39250BB - 0F29 70 D8            - movaps [rax-28],xmm6
    // WHGame.DLL+39250BF - 4C 8B F9              - mov r15,rcx
    // WHGame.DLL+39250C2 - 48 8B 0D E716AC01     - mov rcx,[WHGame.DLL+53E67B0] { (7FFE73DB90A0) }
    // WHGame.DLL+39250C9 - 48 8B F2              - mov rsi,rdx
    // WHGame.DLL+39250CC - 0F29 78 C8            - movaps [rax-38],xmm7
    // WHGame.DLL+39250D0 - 44 0F29 40 B8         - movaps [rax-48],xmm8
    // WHGame.DLL+39250D5 - 44 0F29 50 A8         - movaps [rax-58],xmm10
    // WHGame.DLL+39250DA - 44 0F29 58 98         - movaps [rax-68],xmm11
    // WHGame.DLL+39250DF - 44 0F29 60 88         - movaps [rax-78],xmm12
    // WHGame.DLL+39250E4 - 48 8B 01              - mov rax,[rcx]
    // WHGame.DLL+39250E7 - 44 0F29 6C 24 60      - movaps [rsp+60],xmm13
    // WHGame.DLL+39250ED - FF 90 80000000        - call qword ptr [rax+00000080]
    // WHGame.DLL+39250F3 - 48 8B C8              - mov rcx,rax
    // WHGame.DLL+39250F6 - 4C 8B 00              - mov r8,[rax]
    // WHGame.DLL+39250F9 - 41 FF 90 00020000     - call qword ptr [r8+00000200]
    // WHGame.DLL+3925100 - 33 DB                 - xor ebx,ebx
    // WHGame.DLL+3925102 - 48 8B F8              - mov rdi,rax
    // WHGame.DLL+3925105 - 48 85 C0              - test rax,rax
    // WHGame.DLL+3925108 - 0F84 70020000         - je WHGame.DLL+392537E
    // WHGame.DLL+392510E - 4C 8B 70 38           - mov r14,[rax+38]
    // WHGame.DLL+3925112 - 4D 85 F6              - test r14,r14
    // WHGame.DLL+3925115 - 0F84 63020000         - je WHGame.DLL+392537E
    // WHGame.DLL+392511B - 48 8B 08              - mov rcx,[rax]
    // WHGame.DLL+392511E - 48 8D 55 BB           - lea rdx,[rbp-45]
    // WHGame.DLL+3925122 - 4C 8B 81 C8010000     - mov r8,[rcx+000001C8]
    // WHGame.DLL+3925129 - 48 8B C8              - mov rcx,rax
    // WHGame.DLL+392512C - 41 FF D0              - call r8
    // WHGame.DLL+392512F - F3 41 0F10 5F 14      - movss xmm3,[r15+14]
    // WHGame.DLL+3925135 - 48 8D 55 BB           - lea rdx,[rbp-45]
    // WHGame.DLL+3925139 - F3 41 0F10 67 10      - movss xmm4,[r15+10]
    // WHGame.DLL+392513F - 0F28 C3               - movaps xmm0,xmm3
    // WHGame.DLL+3925142 - F3 41 0F10 77 18      - movss xmm6,[r15+18]
    // WHGame.DLL+3925148 - 0F28 D4               - movaps xmm2,xmm4
    // WHGame.DLL+392514B - F3 0F10 68 04         - movss xmm5,[rax+04]
    // WHGame.DLL+3925150 - 0F28 CE               - movaps xmm1,xmm6
    // WHGame.DLL+3925153 - F3 44 0F10 18         - movss xmm11,[rax]
    // WHGame.DLL+3925158 - 44 0F28 E6            - movaps xmm12,xmm6
    // WHGame.DLL+392515C - F3 44 0F10 40 08      - movss xmm8,[rax+08]
    // WHGame.DLL+3925162 - 49 8B CE              - mov rcx,r14
    // WHGame.DLL+3925165 - F3 0F10 78 0C         - movss xmm7,[rax+0C]
    // WHGame.DLL+392516A - 45 0F28 E8            - movaps xmm13,xmm8
    // WHGame.DLL+392516E - F3 45 0F10 57 1C      - movss xmm10,[r15+1C]
    // WHGame.DLL+3925174 - F3 0F59 C5            - mulss xmm0,xmm5
    // WHGame.DLL+3925178 - F3 41 0F59 D3         - mulss xmm2,xmm11
    // WHGame.DLL+392517D - F3 41 0F59 C8         - mulss xmm1,xmm8
    // WHGame.DLL+3925182 - F3 0F58 D0            - addss xmm2,xmm0
    // WHGame.DLL+3925186 - F3 44 0F59 EC         - mulss xmm13,xmm4
    // WHGame.DLL+392518B - 41 0F28 C0            - movaps xmm0,xmm8
    // WHGame.DLL+392518F - F3 44 0F59 E5         - mulss xmm12,xmm5
    // WHGame.DLL+3925194 - F3 45 0F59 47 1C      - mulss xmm8,[r15+1C]
    // WHGame.DLL+392519A - F3 0F59 C3            - mulss xmm0,xmm3
    // WHGame.DLL+392519E - F3 0F58 D1            - addss xmm2,xmm1
    // WHGame.DLL+39251A2 - 0F28 CC               - movaps xmm1,xmm4
    // WHGame.DLL+39251A5 - F3 44 0F59 D7         - mulss xmm10,xmm7
    // WHGame.DLL+39251AA - F3 44 0F5C E0         - subss xmm12,xmm0
    // WHGame.DLL+39251AF - F3 0F59 CF            - mulss xmm1,xmm7
    // WHGame.DLL+39251B3 - 41 0F28 C3            - movaps xmm0,xmm11
    // WHGame.DLL+39251B7 - F3 0F59 E5            - mulss xmm4,xmm5
    // WHGame.DLL+39251BB - F3 41 0F59 47 1C      - mulss xmm0,[r15+1C]
    // WHGame.DLL+39251C1 - F3 44 0F5C D2         - subss xmm10,xmm2
    // WHGame.DLL+39251C6 - F3 44 0F58 E1         - addss xmm12,xmm1
    // WHGame.DLL+39251CB - 0F28 CB               - movaps xmm1,xmm3
    // WHGame.DLL+39251CE - F3 0F59 CF            - mulss xmm1,xmm7
    // WHGame.DLL+39251D2 - F3 44 0F58 E0         - addss xmm12,xmm0
    // WHGame.DLL+39251D7 - 0F28 C6               - movaps xmm0,xmm6
    // WHGame.DLL+39251DA - F3 41 0F59 C3         - mulss xmm0,xmm11
    // WHGame.DLL+39251DF - F3 44 0F59 DB         - mulss xmm11,xmm3
    // WHGame.DLL+39251E4 - F3 44 0F5C E8         - subss xmm13,xmm0
    // WHGame.DLL+39251E9 - F3 0F59 F7            - mulss xmm6,xmm7
    // WHGame.DLL+39251ED - F3 44 0F5C DC         - subss xmm11,xmm4
    // WHGame.DLL+39251F2 - 0F28 C5               - movaps xmm0,xmm5
    // WHGame.DLL+39251F5 - F3 41 0F59 47 1C      - mulss xmm0,[r15+1C]
    // WHGame.DLL+39251FB - F3 44 0F11 66 0C      - movss [rsi+0C],xmm12
    // WHGame.DLL+3925201 - F3 44 0F58 E9         - addss xmm13,xmm1
    // WHGame.DLL+3925206 - F3 44 0F58 DE         - addss xmm11,xmm6
    // WHGame.DLL+392520B - F3 44 0F58 E8         - addss xmm13,xmm0
    // WHGame.DLL+3925210 - F3 45 0F58 D8         - addss xmm11,xmm8
    // WHGame.DLL+3925215 - F3 44 0F11 6E 10      - movss [rsi+10],xmm13
    // WHGame.DLL+392521B - F3 44 0F11 5E 14      - movss [rsi+14],xmm11
    // WHGame.DLL+3925221 - F3 44 0F11 56 18      - movss [rsi+18],xmm10
    // WHGame.DLL+3925227 - 49 8B 06              - mov rax,[r14]
    // WHGame.DLL+392522A - FF 90 70010000        - call qword ptr [rax+00000170]
    // WHGame.DLL+3925230 - 41 0F28 FA            - movaps xmm7,xmm10
    // WHGame.DLL+3925234 - 41 0F28 D5            - movaps xmm2,xmm13
    // WHGame.DLL+3925238 - F3 41 0F59 D5         - mulss xmm2,xmm13
    // WHGame.DLL+392523D - 41 0F28 CC            - movaps xmm1,xmm12
    // WHGame.DLL+3925241 - 4C 8B F0              - mov r14,rax
    // WHGame.DLL+3925244 - F3 41 0F59 CD         - mulss xmm1,xmm13
    // WHGame.DLL+3925249 - 41 0F28 C2            - movaps xmm0,xmm10
    // WHGame.DLL+392524D - 45 0F28 C2            - movaps xmm8,xmm10
    // WHGame.DLL+3925251 - F3 41 0F59 C3         - mulss xmm0,xmm11
    // WHGame.DLL+3925256 - F3 45 0F59 C2         - mulss xmm8,xmm10
    // WHGame.DLL+392525B - 41 0F28 F5            - movaps xmm6,xmm13
    // WHGame.DLL+392525F - F3 0F5C C8            - subss xmm1,xmm0
    // WHGame.DLL+3925263 - F3 41 0F59 FC         - mulss xmm7,xmm12
    // WHGame.DLL+3925268 - F3 41 0F58 D0         - addss xmm2,xmm8
    // WHGame.DLL+392526D - F3 41 0F59 F3         - mulss xmm6,xmm11
    // WHGame.DLL+3925272 - 0F28 DF               - movaps xmm3,xmm7
    // WHGame.DLL+3925275 - F3 0F58 C9            - addss xmm1,xmm1
    // WHGame.DLL+3925279 - F3 0F58 DE            - addss xmm3,xmm6
    // WHGame.DLL+392527D - F3 0F58 D2            - addss xmm2,xmm2
    // WHGame.DLL+3925281 - F3 0F11 4D 97         - movss [rbp-69],xmm1
    // WHGame.DLL+3925286 - F3 0F5C 15 86046B00   - subss xmm2,[WHGame.DLL+3FD5714] { (1.00) }
    // WHGame.DLL+392528E - F3 0F58 DB            - addss xmm3,xmm3
    // WHGame.DLL+3925292 - F3 0F11 55 9B         - movss [rbp-65],xmm2
    // WHGame.DLL+3925297 - F3 0F11 5D 9F         - movss [rbp-61],xmm3
    // WHGame.DLL+392529C - E8 3F2B10FD           - call WHGame.DLL+A27DE0
    // WHGame.DLL+39252A1 - 48 8B 48 20           - mov rcx,[rax+20]
    // WHGame.DLL+39252A5 - 8B C3                 - mov eax,ebx
    // WHGame.DLL+39252A7 - F3 0F10 89 10010000   - movss xmm1,[rcx+00000110]
    // WHGame.DLL+39252AF - F3 41 0F58 4F 20      - addss xmm1,[r15+20]
    // WHGame.DLL+39252B5 - 0F57 0D 44046B00      - xorps xmm1,[WHGame.DLL+3FD5700] { (-2147483648) }
    // WHGame.DLL+39252BC - 0F28 C1               - movaps xmm0,xmm1
    // WHGame.DLL+39252BF - F3 0F59 44 05 97      - mulss xmm0,[rbp+rax-69]
    // WHGame.DLL+39252C5 - F3 0F11 44 05 A3      - movss [rbp+rax-5D],xmm0
    // WHGame.DLL+39252CB - 48 83 C0 04           - add rax,04 { 4 }
    // WHGame.DLL+39252CF - 48 83 F8 0C           - cmp rax,0C { 12 }
    // WHGame.DLL+39252D3 - 7C E7                 - jl WHGame.DLL+39252BC
    // WHGame.DLL+39252D5 - 48 8D 45 A3           - lea rax,[rbp-5D]
    // WHGame.DLL+39252D9 - 48 8B CB              - mov rcx,rbx
    // WHGame.DLL+39252DC - 4C 2B F0              - sub r14,rax
    // WHGame.DLL+39252DF - 48 8D 45 A3           - lea rax,[rbp-5D]
    // WHGame.DLL+39252E3 - 48 03 C1              - add rax,rcx
    // WHGame.DLL+39252E6 - F3 41 0F10 04 06      - movss xmm0,[r14+rax]
    // WHGame.DLL+39252EC - F3 0F58 00            - addss xmm0,[rax]
    // WHGame.DLL+39252F0 - F3 0F11 44 0D AF      - movss [rbp+rcx-51],xmm0
    // WHGame.DLL+39252F6 - 48 83 C1 04           - add rcx,04 { 4 }
    // WHGame.DLL+39252FA - 48 83 F9 0C           - cmp rcx,0C { 12 }
    // WHGame.DLL+39252FE - 7C DF                 - jl WHGame.DLL+39252DF
    // WHGame.DLL+3925300 - F3 45 0F59 D5         - mulss xmm10,xmm13
    // WHGame.DLL+3925305 - F3 0F5C F7            - subss xmm6,xmm7
    // WHGame.DLL+3925309 - 48 8B CF              - mov rcx,rdi
    // WHGame.DLL+392530C - 41 0F28 C3            - movaps xmm0,xmm11
    // WHGame.DLL+3925310 - F3 45 0F59 E3         - mulss xmm12,xmm11
    // WHGame.DLL+3925315 - F3 41 0F59 C3         - mulss xmm0,xmm11
    // WHGame.DLL+392531A - F3 45 0F58 D4         - addss xmm10,xmm12
    // WHGame.DLL+392531F - F3 0F58 F6            - addss xmm6,xmm6
    // WHGame.DLL+3925323 - F3 41 0F58 C0         - addss xmm0,xmm8
    // WHGame.DLL+3925328 - F3 45 0F58 D2         - addss xmm10,xmm10
    // WHGame.DLL+392532D - F3 0F11 75 9B         - movss [rbp-65],xmm6
    // WHGame.DLL+3925332 - F3 0F58 C0            - addss xmm0,xmm0
    // WHGame.DLL+3925336 - F3 44 0F11 55 97      - movss [rbp-69],xmm10
    // WHGame.DLL+392533C - F3 0F5C 05 D0036B00   - subss xmm0,[WHGame.DLL+3FD5714] { (1.00) }
    // WHGame.DLL+3925344 - F3 0F11 45 9F         - movss [rbp-61],xmm0
    // WHGame.DLL+3925349 - E8 3E2CC7FC           - call WHGame.DLL+597F8C
    // WHGame.DLL+392534E - F3 0F10 40 08         - movss xmm0,[rax+08]
    // WHGame.DLL+3925353 - F3 0F59 44 1D 97      - mulss xmm0,[rbp+rbx-69]
    // WHGame.DLL+3925359 - F3 0F58 44 1D AF      - addss xmm0,[rbp+rbx-51]
    // WHGame.DLL+392535F - F3 0F11 44 1D A3      - movss [rbp+rbx-5D],xmm0
    // WHGame.DLL+3925365 - 48 83 C3 04           - add rbx,04 { 4 }
    // WHGame.DLL+3925369 - 48 83 FB 0C           - cmp rbx,0C { 12 }
    // WHGame.DLL+392536D - 7C DF                 - jl WHGame.DLL+392534E
    // WHGame.DLL+392536F - F2 0F10 45 A3         - movsd xmm0,[rbp-5D]
    // WHGame.DLL+3925374 - 8B 45 AB              - mov eax,[rbp-55]
    // WHGame.DLL+3925377 - F2 0F11 06            - movsd [rsi],xmm0
    // WHGame.DLL+392537B - 89 46 08              - mov [rsi+08],eax
    // WHGame.DLL+392537E - 4C 8D 9C 24 D0000000  - lea r11,[rsp+000000D0]
    // WHGame.DLL+3925386 - 49 8B 5B 20           - mov rbx,[r11+20]
    // WHGame.DLL+392538A - 49 8B 73 28           - mov rsi,[r11+28]
    // WHGame.DLL+392538E - 49 8B 7B 30           - mov rdi,[r11+30]
    // WHGame.DLL+3925392 - 41 0F28 73 F0         - movaps xmm6,[r11-10]
    // WHGame.DLL+3925397 - 41 0F28 7B E0         - movaps xmm7,[r11-20]
    // WHGame.DLL+392539C - 45 0F28 43 D0         - movaps xmm8,[r11-30]
    // WHGame.DLL+39253A1 - 45 0F28 53 C0         - movaps xmm10,[r11-40]
    // WHGame.DLL+39253A6 - 45 0F28 5B B0         - movaps xmm11,[r11-50]
    // WHGame.DLL+39253AB - 45 0F28 63 A0         - movaps xmm12,[r11-60]
    // WHGame.DLL+39253B0 - 45 0F28 6B 90         - movaps xmm13,[r11-70]
    // WHGame.DLL+39253B5 - 49 8B E3              - mov rsp,r11
    // WHGame.DLL+39253B8 - 41 5F                 - pop r15
    // WHGame.DLL+39253BA - 41 5E                 - pop r14
    // WHGame.DLL+39253BC - 5D                    - pop rbp
    // WHGame.DLL+39253BD - C3                    - ret
    constexpr const char *TPV_CAMERA_UPDATE_AOB_PATTERN = "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 41 56 41 57 48 8D 68 A1 48 81 EC D0 00 00 00 0F 29 70 D8 4C 8B F9 48 8B 0D E7 16 AC";

    // --- AOB Hook Offsets ---
    constexpr int OVERLAY_HOOK_OFFSET = 0;
    constexpr int EVENT_HANDLER_HOOK_OFFSET = 0;

    // --- Memory Offsets ---
    constexpr ptrdiff_t OFFSET_ManagerPtrStorage = 0x38;      // Global context to camera manager
    constexpr ptrdiff_t OFFSET_TpvObjPtrStorage = 0x28;       // Camera manager to TPV object
    constexpr ptrdiff_t OFFSET_TpvFlag = 0x38;                // TPV object to flag byte
    constexpr ptrdiff_t OVERLAY_FLAG_OFFSET = 0xD8;           // UI module to overlay flag
    constexpr ptrdiff_t OFFSET_ScrollAccumulatorFloat = 0x1C; // Scroll state to accumulator
    constexpr ptrdiff_t OFFSET_TpvFovWrite = 0x30;            // FOV calculation offset

    constexpr ptrdiff_t TPV_CAMERA_QUATERNION_OFFSET = 0x10; // Confirmed XYZW quaternion start in C_CameraThirdPerson object

    constexpr ptrdiff_t PLAYER_STATE_POSITION_OFFSET = 0x0;  // Verified (part of first MOVUPS)
    constexpr ptrdiff_t PLAYER_STATE_ROTATION_OFFSET = 0x10; // Verified
    constexpr size_t PLAYER_STATE_SIZE = 0xD4;               // Verified from assembly (212 bytes)

    // Offsets relative to the outputPosePtr (RDX) in FUN_18392509c
    // ASSUMING standard Pos(XYZ) followed by Quat(XYZW) layout. VERIFY WITH DEBUGGER.
    constexpr ptrdiff_t TPV_OUTPUT_POSE_POSITION_OFFSET = 0x0;  // Base Offset (X, Y, Z = 12 bytes)
    constexpr ptrdiff_t TPV_OUTPUT_POSE_ROTATION_OFFSET = 0x0C; // Base Offset (Assuming starts right after Pos. Z = 0x8+0x4) (X, Y, Z, W = 16 bytes)
    constexpr size_t TPV_OUTPUT_POSE_REQUIRED_SIZE = 0x1C;      // Minimum size needed: Pos(12) + Quat(16) = 28 bytes (0x1C). Let's use 0x20 for alignment.

    // --- Input Event Offsets ---
    constexpr ptrdiff_t INPUT_EVENT_TYPE_OFFSET = 0x04;
    constexpr ptrdiff_t INPUT_EVENT_BYTE0_OFFSET = 0x00;
    constexpr ptrdiff_t INPUT_EVENT_ID_OFFSET = 0x10;
    constexpr ptrdiff_t INPUT_EVENT_VALUE_OFFSET = 0x18;
    constexpr int INPUT_EVENT_BYTE0_EXPECTED = 0x01;
    constexpr int MOUSE_INPUT_TYPE_ID = 8;
    constexpr int MOUSE_WHEEL_EVENT_ID = 0x10C;

    // --- Timing ---
    constexpr unsigned long OVERLAY_MONITOR_INTERVAL_MS = 66;
    constexpr unsigned long MAIN_MONITOR_SLEEP_MS = 33;

    /** @brief Name of the target game module. */
    constexpr const char *MODULE_NAME = "WHGame.dll";

} // namespace Constants
#endif // CONSTANTS_H
